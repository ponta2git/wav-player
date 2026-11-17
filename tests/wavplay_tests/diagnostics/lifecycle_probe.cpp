#include "diagnostics/lifecycle_probe.h"
#include "diagnostics/capability_probe.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace {

using wavplay::asio::throw_if_error;

constexpr unsigned max_switches = 4096;
constexpr unsigned max_probe_rate = 1'000'000;
constexpr unsigned max_events = 256;
// A driver may still deliver a burst of switches just after start() returns.
constexpr unsigned startup_skip = 4;
constexpr unsigned min_steady_intervals = 8;

LONGLONG qpc_now() noexcept
{
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

double qpc_ms(LONGLONG ticks) noexcept
{
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return 1000.0 * static_cast<double>(ticks) / frequency;
}

struct EventLog {
    LONGLONG origin = qpc_now();
    std::atomic<unsigned> count{};
    DriverEvent entries[max_events]{};

    void add(bool rate_change, long selector, long value, double rate, long reply) noexcept
    {
        const auto index = count.fetch_add(1);
        if (index < max_events) {
            entries[index] = {qpc_ms(qpc_now() - origin), rate_change, selector, value, rate, reply};
        }
    }
};

// Written by the driver's callback thread until detach_callbacks() returns.
struct SwitchLog {
    struct Observation {
        LONGLONG at;
        std::uint64_t position;
        ASIOError status;
    };
    IASIO* driver{};
    void* halves[2][2]{}; // [buffer index][output]
    std::size_t bytes{};
    HANDLE done{};
    std::atomic<bool> starting{};
    std::atomic<unsigned> count{}, in_start{};
    std::vector<Observation> samples; // Sized before createBuffers and never grown by a callback.
};

std::atomic<SwitchLog*> active_switches{};
std::atomic<EventLog*> active_events{};
std::atomic<int> callbacks_in_flight{};

// Once this returns, no callback holds either log.
void detach_callbacks(bool events) noexcept
{
    active_switches.store(nullptr);
    if (events) {
        active_events.store(nullptr);
    }
    while (callbacks_in_flight.load()) {
        Sleep(1);
    }
}

void switch_buffer(long index, ASIOBool) noexcept
{
    callbacks_in_flight.fetch_add(1);
    auto* log = active_switches.load();
    if (log && (index == 0 || index == 1)) {
        const auto n = log->count.fetch_add(1);
        if (log->starting.load()) {
            log->in_start.fetch_add(1);
        }
        if (n < log->samples.size()) {
            auto& sample = log->samples[n];
            sample.at = qpc_now();
            ASIOSamples samples{};
            ASIOTimeStamp stamp{};
            sample.status = log->driver->getSamplePosition(&samples, &stamp);
            sample.position = (static_cast<std::uint64_t>(samples.hi) << 32) | samples.lo;
        }
        for (auto* channel : log->halves[index]) {
            std::memset(channel, 0, log->bytes);
        }
        if (n + 1 == log->samples.size()) {
            SetEvent(log->done);
        }
    }
    callbacks_in_flight.fetch_sub(1);
}

void rate_changed(ASIOSampleRate rate) noexcept
{
    callbacks_in_flight.fetch_add(1);
    if (auto* log = active_events.load()) {
        log->add(true, 0, 0, rate, 0);
    }
    callbacks_in_flight.fetch_sub(1);
}

// Same replies as the asio_message callback in playback::segment_renderer, so
// the driver sees the playback host.
long asio_message(long selector, long value, void*, double*) noexcept
{
    callbacks_in_flight.fetch_add(1);
    long reply{};
    if (selector == kAsioEngineVersion) {
        reply = 2;
    } else {
        const long request = selector == kAsioSelectorSupported ? value : selector;
        reply = request == kAsioSelectorSupported || request == kAsioEngineVersion ||
            request == kAsioResetRequest || request == kAsioOverload;
    }
    if (auto* log = active_events.load()) {
        log->add(false, selector, value, 0, reply);
    }
    callbacks_in_flight.fetch_sub(1);
    return reply;
}

ASIOCallbacks callbacks{switch_buffer, rate_changed, asio_message, nullptr};

struct CloseEvent {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

// Returns true once event is set. Without an event, waits the full time.
bool pump_messages(DWORD milliseconds, HANDLE event)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    const DWORD handles = event ? 1 : 0;
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
        }
        const auto wake = MsgWaitForMultipleObjectsEx(handles, event ? &event : nullptr,
            static_cast<DWORD>(remaining.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (event && wake == WAIT_OBJECT_0) {
            return true;
        }
        if (wake == WAIT_TIMEOUT) {
            continue;
        }
        if (wake != WAIT_OBJECT_0 + handles) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "MsgWaitForMultipleObjectsEx");
        }
        MSG message{};
        // A handler can keep posting work; recheck the event and deadline between dispatches.
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                throw std::runtime_error("WM_QUIT received during the lifecycle probe");
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

// Zero bytes are silence for every integer and float PCM type. DSD is excluded.
std::size_t sample_bytes(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB: case ASIOSTInt16MSB:
        return 2;
    case ASIOSTInt24LSB: case ASIOSTInt24MSB:
        return 3;
    case ASIOSTInt32LSB: case ASIOSTInt32MSB: case ASIOSTFloat32LSB: case ASIOSTFloat32MSB:
    case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB16: case ASIOSTInt32MSB18: case ASIOSTInt32MSB20: case ASIOSTInt32MSB24:
        return 4;
    case ASIOSTFloat64LSB: case ASIOSTFloat64MSB:
        return 8;
    default:
        return 0;
    }
}

const char* selector_name(long selector)
{
    switch (selector) {
    case kAsioSelectorSupported: return "kAsioSelectorSupported";
    case kAsioEngineVersion: return "kAsioEngineVersion";
    case kAsioResetRequest: return "kAsioResetRequest";
    case kAsioBufferSizeChange: return "kAsioBufferSizeChange";
    case kAsioResyncRequest: return "kAsioResyncRequest";
    case kAsioLatenciesChanged: return "kAsioLatenciesChanged";
    case kAsioSupportsTimeInfo: return "kAsioSupportsTimeInfo";
    case kAsioSupportsTimeCode: return "kAsioSupportsTimeCode";
    case kAsioOverload: return "kAsioOverload";
    default: return "other";
    }
}

void summarize(const SwitchLog& log, unsigned recorded, SilentRun& run)
{
    run.switches = log.count.load();
    run.during_start = log.in_start.load();
    const auto& samples = log.samples;
    const auto first = std::min(run.during_start, recorded) + startup_skip;
    const bool timed = recorded >= first + min_steady_intervals + 1;
    const auto from = timed ? first : 0;
    for (auto i = from; i < recorded; ++i) {
        if (samples[i].status != ASE_OK) {
            run.position_error = samples[i].status;
            break;
        }
    }
    if (!timed) {
        return;
    }
    const auto last = recorded - 1;
    run.steady_intervals = last - first;
    run.mean_interval_ms = qpc_ms(samples[last].at - samples[first].at) / run.steady_intervals;
    auto shortest = samples[first + 1].at - samples[first].at;
    auto longest = shortest;
    bool advancing = run.position_error == ASE_OK;
    for (auto i = first + 1; i <= last; ++i) {
        const auto ticks = samples[i].at - samples[i - 1].at;
        shortest = std::min(shortest, ticks);
        longest = std::max(longest, ticks);
        advancing &= samples[i].position > samples[i - 1].position;
    }
    run.min_interval_ms = qpc_ms(shortest);
    run.max_interval_ms = qpc_ms(longest);
    if (advancing) {
        run.position_frames_per_switch =
            static_cast<double>(samples[last].position - samples[first].position) / run.steady_intervals;
    }
}

// Allocate outputs 1/2 at the exact selected width, optionally stream silence,
// then dispose. Zero selects the reported preferred width for lifecycle probes.
SilentRun silent_run(IASIO& driver, const wavplay::asio::DriverCapabilities& requested, bool start,
    const LifecycleProbeOptions& options, long requested_frames = 0)
{
    const auto width = static_cast<long>(wavplay::asio::select_buffer_frames(requested.buffer_size, requested_frames));
    SwitchLog log;
    log.samples.resize(start ? options.switches : 0);
    // A failed start can still call back until stop and detach finish.
    std::unique_ptr<std::remove_pointer_t<HANDLE>, CloseEvent> done;
    ASIOBufferInfo buffers[2]{{ASIOFalse, 0, {}}, {ASIOFalse, 1, {}}};
    const auto created = driver.createBuffers(buffers, 2, width, &callbacks);
    if (created != ASE_OK) {
        if (buffers[0].buffers[0] || buffers[0].buffers[1] || buffers[1].buffers[0] || buffers[1].buffers[1]) {
            driver.disposeBuffers();
        }
        throw_if_error(created, "ASIO createBuffers failed");
    }
    SilentRun run{};
    run.width = width;
    run.started = start;
    bool stop_pending = false;
    unsigned recorded{};
    try {
        const auto rate = wavplay::asio::current_sample_rate(driver);
        if (!rate || *rate != requested.sample_rate) {
            throw std::runtime_error("Unexpected ASIO clock after createBuffers");
        }
        run.sample_rate = *rate;
        std::size_t bytes = 0;
        for (long channel = 0; channel < 2; ++channel) {
            ASIOChannelInfo info{};
            info.channel = channel;
            info.isInput = ASIOFalse;
            throw_if_error(driver.getChannelInfo(&info), "ASIO getChannelInfo failed");
            if (info.channel != channel || info.isInput != ASIOFalse || info.isActive != ASIOTrue ||
                info.type != requested.output_types[channel]) {
                throw std::runtime_error("Silent probe output changed or was not activated after createBuffers");
            }
            run.output_types[channel] = info.type;
            const auto size = sample_bytes(info.type);
            if (!size || (bytes && size != bytes)) {
                throw std::runtime_error("Silent run requires two PCM outputs with the same sample size");
            }
            bytes = size;
        }
        log.driver = &driver;
        log.bytes = bytes * static_cast<std::size_t>(width);
        wavplay::asio::validate_output_buffers(buffers, log.bytes);
        run.output_latency = wavplay::asio::output_latency(driver);
        for (long half = 0; half < 2; ++half) {
            for (long channel = 0; channel < 2; ++channel) {
                auto* buffer = buffers[channel].buffers[half];
                std::memset(buffer, 0, log.bytes);
                log.halves[half][channel] = buffer;
            }
        }
        if (start) {
            done.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!done) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateEventW");
            }
            log.done = done.get();
            log.starting.store(true);
            active_switches.store(&log);
            stop_pending = true; // Even a failed start can have launched callbacks.
            const auto started = driver.start();
            log.starting.store(false);
            throw_if_error(started, "ASIO start failed");
            run.timed_out = !pump_messages(options.switch_timeout_ms, done.get());
            // Stop can synchronously ask for silence; those requests have no running clock cadence.
            recorded = std::min(options.switches, log.count.load());
            stop_pending = false;
            throw_if_error(driver.stop(), "ASIO stop failed");
            detach_callbacks(false);
        }
    } catch (...) {
        if (stop_pending) {
            driver.stop();
        }
        detach_callbacks(false);
        driver.disposeBuffers();
        throw;
    }
    throw_if_error(driver.disposeBuffers(), "ASIO disposeBuffers failed");
    summarize(log, recorded, run);
    return run;
}

void print_capabilities(const char* label, const wavplay::asio::DriverCapabilities& caps)
{
    const auto& size = caps.buffer_size;
    std::printf("  %s: %.15g Hz, outputs %s (%ld) / %s (%ld), buffer min/max/preferred/granularity %ld/%ld/%ld/%ld\n",
        label, caps.sample_rate, wavplay::asio::sample_type_name(caps.output_types[0]), caps.output_types[0],
        wavplay::asio::sample_type_name(caps.output_types[1]), caps.output_types[1], size.minimum, size.maximum, size.preferred, size.granularity);
    std::fflush(stdout);
}

void print_run(const char* label, const SilentRun& run)
{
    std::printf("  %s: createBuffers %ld frames, actual %.15g Hz, outputs %s / %s, output latency %ld frames", label,
        run.width, run.sample_rate, wavplay::asio::sample_type_name(run.output_types[0]),
        wavplay::asio::sample_type_name(run.output_types[1]), run.output_latency);
    if (!run.started) {
        std::puts(", disposed without start");
        std::fflush(stdout);
        return;
    }
    std::printf(", %u silent switches (%u during start)%s\n", run.switches, run.during_start,
        run.timed_out ? ", TIMED OUT before the requested count" : "");
    if (!run.steady_intervals) {
        std::puts("    Too few switches after start to time");
    } else {
        const double expected = 1000.0 * run.width / run.sample_rate;
        std::printf("    Mean switch interval %.3f ms over %u intervals; %ld frames at %.15g Hz expect %.3f ms"
            " (ratio %.4f, %.1f frames per switch at that rate)\n",
            run.mean_interval_ms, run.steady_intervals, run.width, run.sample_rate, expected,
            run.mean_interval_ms / expected, run.mean_interval_ms * run.sample_rate / 1000.0);
        std::printf("    Interval min / max %.3f / %.3f ms\n", run.min_interval_ms, run.max_interval_ms);
    }
    if (run.position_frames_per_switch) {
        std::printf("    getSamplePosition advance %.2f frames per switch\n", *run.position_frames_per_switch);
    } else if (run.position_error != ASE_OK) {
        std::printf("    getSamplePosition failed (ASIOError %ld)\n", run.position_error);
    } else if (run.steady_intervals) {
        std::puts("    getSamplePosition repeated or moved backwards; no position average");
    }
    std::fflush(stdout);
}

void print_events(const std::vector<DriverEvent>& events)
{
    if (events.empty()) {
        std::puts("  Driver notifications: none");
    }
    for (const auto& event : events) {
        if (event.rate_change) {
            std::printf("  +%.1f ms sampleRateDidChange(%.15g)\n", event.ms, event.rate);
        } else if (event.selector == kAsioSelectorSupported) {
            std::printf("  +%.1f ms asioMessage(kAsioSelectorSupported, %s (%ld)) -> %ld\n", event.ms,
                selector_name(event.value), event.value, event.reply);
        } else {
            std::printf("  +%.1f ms asioMessage(%s (%ld), %ld) -> %ld\n", event.ms, selector_name(event.selector),
                event.selector, event.value, event.reply);
        }
    }
    std::fflush(stdout);
}

const char* scenario_steps(LifecycleScenario scenario)
{
    switch (scenario) {
    case LifecycleScenario::fresh: return "init -> setSampleRate";
    case LifecycleScenario::created: return "init -> createBuffers/disposeBuffers -> setSampleRate";
    case LifecycleScenario::started: return "init -> createBuffers/start/stop/disposeBuffers -> setSampleRate";
    case LifecycleScenario::started_then_wait:
        return "init -> createBuffers/start/stop/disposeBuffers -> wait -> setSampleRate";
    }
    return "";
}

LifecycleReport run_scenario(LifecycleScenario scenario, const CLSID& clsid, HWND window,
    wavplay::asio::ActivateDriver activate, unsigned target_rate, const LifecycleProbeOptions& options,
    const wavplay::asio::DriverCapabilities* baseline)
{
    LifecycleReport report{};
    report.scenario = scenario;
    const auto events = std::make_unique<EventLog>();
    struct Detach {
        ~Detach() { detach_callbacks(true); }
    } detach;
    active_events.store(events.get());
    wavplay::asio::DriverSession session(clsid, window, activate);
    auto& driver = session.driver();
    report.before = wavplay::asio::query_capabilities(driver);
    print_capabilities("Opened", report.before);
    if (baseline && (report.before.sample_rate != baseline->sample_rate ||
        report.before.buffer_size.preferred != baseline->buffer_size.preferred ||
        report.before.output_types != baseline->output_types)) {
        throw std::runtime_error("Device did not reopen in the state of the first scenario");
    }
    if (report.before.sample_rate == target_rate) {
        throw std::runtime_error("Device already runs at the target rate; the probe needs another starting rate");
    }
    (void)driver.future(kAsioCanReportOverload, nullptr); // Each playback request asks this first.
    if (scenario != LifecycleScenario::fresh) {
        report.original = silent_run(driver, report.before,
            scenario != LifecycleScenario::created, options);
        print_run("Original-rate buffers", *report.original);
        if (scenario == LifecycleScenario::started_then_wait) {
            pump_messages(options.boundary_wait_ms, nullptr);
            std::printf("  Waited %lu ms with messages dispatched\n", options.boundary_wait_ms);
        }
    }
    {
        wavplay::asio::DriverLease guard(session); // Records the original rate for close().
        const auto capability = driver.canSampleRate(target_rate);
        if (capability == ASE_NoClock) {
            throw std::runtime_error("Driver does not offer the target rate");
        }
        throw_if_error(capability, "ASIO canSampleRate failed");
        throw_if_error(guard.set_sample_rate(target_rate, report.before.sample_rate), "ASIO setSampleRate failed");
    }
    report.after_set = wavplay::asio::query_capabilities(driver);
    print_capabilities("After setSampleRate", report.after_set);
    if (report.after_set.sample_rate != target_rate) {
        throw std::runtime_error("Driver did not report the target rate after setSampleRate");
    }
    report.target = silent_run(driver, report.after_set, true, options);
    print_run("Target-rate buffers", report.target);
    report.after_target = wavplay::asio::query_capabilities(driver);
    print_capabilities("After target buffers were disposed", report.after_target);
    session.close();
    detach_callbacks(true);
    report.events.assign(events->entries, events->entries + std::min(events->count.load(), max_events));
    print_events(report.events);
    return report;
}

void print_summary(unsigned target_rate, const std::vector<LifecycleReport>& reports)
{
    std::printf("Summary at %u Hz (preferred after setSampleRate, then the silent run at that width):\n", target_rate);
    for (const auto& report : reports) {
        const auto& run = report.target;
        std::printf("  %-18s preferred %ld", lifecycle_scenario_name(report.scenario), report.after_set.buffer_size.preferred);
        if (run.steady_intervals) {
            std::printf(", interval %.3f ms (ratio %.4f)", run.mean_interval_ms,
                run.mean_interval_ms * run.sample_rate / (1000.0 * run.width));
        }
        if (run.position_frames_per_switch) {
            std::printf(", position %.2f frames/switch", *run.position_frames_per_switch);
        }
        std::puts("");
    }
    std::fflush(stdout);
}

} // namespace

const char* lifecycle_scenario_name(LifecycleScenario scenario)
{
    switch (scenario) {
    case LifecycleScenario::fresh: return "fresh";
    case LifecycleScenario::created: return "created";
    case LifecycleScenario::started: return "started";
    case LifecycleScenario::started_then_wait: return "started+wait";
    }
    return "unknown";
}

unsigned parse_probe_rate(const wchar_t* text)
{
    wchar_t* end{};
    errno = 0;
    const long value = text ? std::wcstol(text, &end, 10) : 0;
    if (!text || end == text || *end != L'\0' || errno == ERANGE ||
        value <= 0 || value > static_cast<long>(max_probe_rate)) {
        throw std::invalid_argument("Expected an integer sample rate from 1 to 1000000 Hz");
    }
    return static_cast<unsigned>(value);
}

long parse_probe_buffer_frames(const wchar_t* text)
{
    const std::wstring_view digits = text ? std::wstring_view(text) : std::wstring_view{};
    if (digits.empty() || digits.find_first_not_of(L"0123456789") != std::wstring_view::npos) {
        throw std::invalid_argument("Expected a positive integer ASIO buffer width in frames");
    }
    wchar_t* end{};
    errno = 0;
    const long value = std::wcstol(text, &end, 10);
    if (*end != L'\0' || errno == ERANGE || value <= 0) {
        throw std::invalid_argument("ASIO buffer width must fit a positive 32-bit integer");
    }
    return value;
}

BufferProbeReport probe_asio_buffer(const wchar_t* clsid_text, long buffer_frames,
    wavplay::asio::ActivateDriver activate, const LifecycleProbeOptions& options)
{
    if (buffer_frames <= 0) {
        throw std::invalid_argument("The buffer probe requires a positive explicit width");
    }
    if (options.switches < startup_skip + min_steady_intervals + 2 || options.switches > max_switches) {
        throw std::invalid_argument("Buffer probe switch count is out of range");
    }
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    (void)qpc_ms(0);
    const auto window = make_asio_probe_window();
    BufferProbeReport report{};
    report.requested_frames = buffer_frames;
    const auto events = std::make_unique<EventLog>();
    struct Detach {
        ~Detach() { detach_callbacks(true); }
    } detach;
    active_events.store(events.get());
    wavplay::asio::DriverSession session(clsid, window.get(), activate);
    auto& driver = session.driver();
    report.before = wavplay::asio::query_capabilities(driver);
    std::printf("Buffer probe: requested %ld frames, keeping the current sample rate\n", buffer_frames);
    std::puts("  QPC/getSamplePosition instrumented silence; not a product CPU-load comparison.");
    print_capabilities("Opened", report.before);
    (void)driver.future(kAsioCanReportOverload, nullptr);
    report.run = silent_run(driver, report.before, true, options, buffer_frames);
    print_run("Requested-width buffers", report.run);
    if (report.run.position_frames_per_switch &&
        *report.run.position_frames_per_switch != static_cast<double>(report.run.width)) {
        std::puts("  Position advance differs from createBuffers width; requested width alone does not establish device cadence.");
    }
    report.after = wavplay::asio::query_capabilities(driver);
    print_capabilities("After buffers were disposed", report.after);
    if (report.after.sample_rate != report.before.sample_rate) {
        throw std::runtime_error("Driver clock changed during the fixed-rate buffer probe");
    }
    session.close();
    detach_callbacks(true);
    report.events.assign(events->entries, events->entries + std::min(events->count.load(), max_events));
    print_events(report.events);
    return report;
}

std::vector<LifecycleReport> probe_asio_rate_lifecycle(const wchar_t* clsid_text, unsigned target_rate,
    wavplay::asio::ActivateDriver activate, const LifecycleProbeOptions& options)
{
    if (target_rate == 0 || target_rate > max_probe_rate) {
        throw std::invalid_argument("Target sample rate is out of range");
    }
    if (options.switches < startup_skip + min_steady_intervals + 2 || options.switches > max_switches) {
        throw std::invalid_argument("Lifecycle probe switch count is out of range");
    }
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    (void)qpc_ms(0); // Initialize the timer frequency before any callback can use it.
    const auto window = make_asio_probe_window();
    std::vector<LifecycleReport> reports;
    reports.reserve(lifecycle_scenarios.size());
    for (std::size_t i = 0; i < lifecycle_scenarios.size(); ++i) {
        const auto scenario = lifecycle_scenarios[i];
        std::printf("Scenario %zu/%zu %s: %s(%u)\n", i + 1, lifecycle_scenarios.size(), lifecycle_scenario_name(scenario),
            scenario_steps(scenario), target_rate);
        std::fflush(stdout);
        const auto* baseline = reports.empty() ? nullptr : &reports.front().before;
        reports.push_back(run_scenario(scenario, clsid, window.get(), activate, target_rate, options, baseline));
    }
    print_summary(target_rate, reports);
    return reports;
}
