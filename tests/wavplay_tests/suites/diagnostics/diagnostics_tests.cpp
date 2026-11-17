#include "suites/suites.h"
#include "diagnostics/audition.h"
#include "diagnostics/lifecycle_probe.h"
#include "doubles/registered_driver.h"
#include "fixtures/playback_fixture.h"
#include "wavplay/playback/segment.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace wavplay_test;

void test_pcm_capture_matcher()
{
    // Three interleaved frames and their independently written planar buffers.
    // The second block has a silent tail; packed24 also has a zero low byte.
    const Bytes pcm{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto rejects = [](const char* field, auto operation) {
        bool rejected{};
        try { operation(); }
        catch (const Failure&) { rejected = true; } // This case deliberately tests the assertion helper itself.
        expect_true(field, rejected);
    };
    const auto check = [&](const std::vector<MockAsio::Block>& captured, std::size_t sample_bytes) {
        check_pcm16_capture(captured, 2, pcm, sample_bytes);
        check_submitted_pcm(captured, 2, pcm, 0, sample_bytes);
        const auto rejects_change = [&](const char* field, auto change) {
            auto changed = captured;
            change(changed);
            rejects(field, [&] { check_pcm16_capture(changed, 2, pcm, sample_bytes); });
        };
        rejects_change("capture matcher rejects a missing final frame", [](auto& changed) { changed.pop_back(); });
        rejects_change("capture matcher rejects a truncated channel", [](auto& changed) { changed[0][0].pop_back(); });
        rejects_change("capture matcher rejects a changed sample", [](auto& changed) { changed[0][1].back() ^= 1; });
        rejects_change("capture matcher rejects nonzero tail padding", [](auto& changed) { changed[1][0].back() = 1; });
        if (sample_bytes == 3) {
            rejects_change("capture matcher rejects nonzero low padding", [](auto& changed) { changed[0][0][0] = 1; });
        }
        auto extra = captured;
        extra.push_back({Bytes(2 * sample_bytes), Bytes(2 * sample_bytes)});
        rejects("submitted PCM wrapper retains its exact block count", [&] {
            check_submitted_pcm(extra, 2, pcm, 0, sample_bytes);
        });
    };
    check({{Bytes{1, 2, 5, 6}, Bytes{3, 4, 7, 8}}, {Bytes{9, 10, 0, 0}, Bytes{11, 12, 0, 0}}}, 2);
    check({{Bytes{0, 1, 2, 0, 5, 6}, Bytes{0, 3, 4, 0, 7, 8}},
        {Bytes{0, 9, 10, 0, 0, 0}, Bytes{0, 11, 12, 0, 0, 0}}}, 3);
}

void test_audition_modes()
{
    constexpr auto clsid = L"{76543210-89ab-cdef-8123-456789abcdef}";
    TempFile left, right;
    const auto* first = left.write(wave({pcm16(2, 48000), {"data", samples(1, 10)}}));
    const auto* second = right.write(wave({pcm16(2, 44100), {"data", samples(1, 90)}}));
    for (const bool transition : {false, true}) {
        RegisteredDriver driver;
        expect_throw<std::runtime_error>("WAV pair does not match the audition mode", [&] {
            audition_asio(clsid, first, transition ? first : second, transition, driver.activation());
        });
        expect("invalid audition rejected before opening the device", driver.attempts, 0);

        driver.buffer_size = {544, 544, 544, 0};
        driver.next_width = 512;
        driver.ticks_after_start = {16, 16}; // Includes the default start silence in both segments.
        audition_asio(clsid, first, transition ? second : first, transition, driver.activation());
        expect("streams started for the audition pair", driver.starts, transition ? 2 : 1);
        expect("last WAV clock", static_cast<int>(driver.runs.back().rate == (transition ? 44100 : 48000)), 1);
        expect("original clock after audition", static_cast<int>(driver.sample_rate == 48000), 1);
        driver.check_closed();
    }
}

constexpr auto lifecycle_clsid = L"{76543210-89ab-cdef-8123-456789abcdef}";

LifecycleProbeOptions quick_lifecycle_options()
{
    LifecycleProbeOptions options{};
    options.switches = 24;
    options.boundary_wait_ms = 20;
    options.switch_timeout_ms = 2000;
    return options;
}

void test_rate_lifecycle_probe()
{
    using enum MockAsio::Call;
    using enum LifecycleScenario;
    const auto options = quick_lifecycle_options();
    struct PositionDriver : LifecycleDriver {
        unsigned observations{};
        ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp*) override
        {
            const auto step = observations++;
            std::uint64_t position{};
            switch (lifecycle_scenarios[inits - 1]) {
            case LifecycleScenario::fresh:
                // Cross the signed 64-bit boundary in the steady window, retaining exact 512-frame steps.
                position = (std::uint64_t{1} << 63) - 12 * 512 + std::uint64_t{step} * 512;
                break;
            case LifecycleScenario::created:
                break; // Model ASE_OK with position zero for every switch.
            case LifecycleScenario::started:
                // Overall forward progress can conceal a reversal at every other switch.
                position = std::uint64_t{step ^ 1u} * 512;
                break;
            case LifecycleScenario::started_then_wait:
                return ASE_NotPresent;
            }
            samples->hi = static_cast<unsigned long>(position >> 32);
            samples->lo = static_cast<unsigned long>(position);
            return ASE_OK;
        }
    } driver;
    const auto reports = probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), options);
    expect("scenario reports", reports.size(), lifecycle_scenarios.size());
    expect("positions sampled only for the requested switches", driver.observations, driver.starts * options.switches);
    expect("one device open per scenario", driver.activations, lifecycle_scenarios.size());
    expect("rate set and restored per scenario", driver.set_rates.size(), 2 * lifecycle_scenarios.size());
    expect("original clock after the probe", static_cast<int>(driver.sample_rate == 48000), 1);
    for (std::size_t i = 0; i < reports.size(); ++i) {
        const auto& report = reports[i];
        expect("complete probe run", static_cast<int>(report.timed_out()), 0);
        expect("scenario order", static_cast<int>(report.scenario), static_cast<int>(lifecycle_scenarios[i]));
        const bool allocated = report.scenario != fresh;
        const bool streamed = report.scenario == started || report.scenario == started_then_wait;
        expect("opened at the original clock", static_cast<int>(report.before.sample_rate == 48000), 1);
        expect("opened width", report.before.buffer_size.preferred, 544);
        expect("original buffers only after a lifecycle", static_cast<int>(report.original.has_value()), static_cast<int>(allocated));
        if (allocated) {
            const auto& original = *report.original;
            expect("original width", original.width, 544);
            expect("original latency", original.output_latency, 544);
            expect("original started", static_cast<int>(original.started), static_cast<int>(streamed));
            expect("original switch count", static_cast<int>(original.switches >= (streamed ? options.switches : 0)), 1);
            expect("original timeout", static_cast<int>(original.timed_out), 0);
        }
        expect("target clock", static_cast<int>(report.after_set.sample_rate == 44100), 1);
        expect("width reported after setSampleRate", report.after_set.buffer_size.preferred, allocated ? 544 : 512);
        const auto& target = report.target;
        expect("target buffers use the reported width", target.width, report.after_set.buffer_size.preferred);
        expect("target buffers at the target clock", static_cast<int>(target.sample_rate == 44100), 1);
        expect("target output type", target.output_types[1], static_cast<long>(ASIOSTInt24LSB));
        expect("target latency", target.output_latency, target.width);
        expect("target switch count", static_cast<int>(target.switches >= options.switches), 1);
        expect("target timeout", static_cast<int>(target.timed_out), 0);
        expect("synchronous pre-roll counted during start", static_cast<int>(target.during_start >= 1), 1);
        if (target.during_start == 1) { // The mock clock normally starts after start() returns.
            expect("timed target intervals", static_cast<int>(target.steady_intervals > 0 && target.mean_interval_ms > 0), 1);
        }
        expect("sample position status", target.position_error,
            static_cast<long>(report.scenario == started_then_wait ? ASE_NotPresent : ASE_OK));
        if (report.scenario == fresh && target.steady_intervals) {
            expect("full-width sample position advance", static_cast<int>(target.position_frames_per_switch == 512.0), 1);
        } else {
            expect("no average without enough strictly advancing positions",
                static_cast<int>(target.position_frames_per_switch.has_value()), 0);
        }
        expect("idle width after target buffers", report.after_target.buffer_size.preferred, target.width);
        // MockAsio::start asks whether the host accepts buffer-size changes; the host declines.
        const auto inquiries = std::ranges::count_if(report.events, [](const DriverEvent& event) {
            return !event.rate_change && event.selector == kAsioSelectorSupported &&
                event.value == kAsioBufferSizeChange && event.reply == 0;
        });
        expect("logged driver inquiries", inquiries, streamed ? 2 : 1);
    }
    expect_calls("fresh calls", driver.lifecycle_calls(0),
        {can_rate, set_rate, create, start, stop, dispose, set_rate});
    expect_calls("created calls", driver.lifecycle_calls(1),
        {create, dispose, can_rate, set_rate, create, start, stop, dispose, set_rate});
    for (const std::size_t open : {2, 3}) {
        expect_calls("started calls", driver.lifecycle_calls(open),
            {create, start, stop, dispose, can_rate, set_rate, create, start, stop, dispose, set_rate});
    }
    expect("only silence submitted", static_cast<int>(std::ranges::all_of(driver.submitted, MockAsio::silent)), 1);
    expect("callbacks wrote only the requested half", static_cast<int>(driver.peer_untouched), 1);
    driver.check_closed();
}

void test_rate_lifecycle_probe_failures()
{
    const auto options = quick_lifecycle_options();
    for (const auto* text : {L"", L"0", L"44.1k", L"-44100", L"-4294967295", L"1000001", L"99999999999"}) {
        expect_throw<std::invalid_argument>("invalid probe rate", [text] { parse_probe_rate(text); });
    }
    expect("probe rate", parse_probe_rate(L"44100"), 44100);
    expect("maximum probe rate", parse_probe_rate(L"1000000"), 1'000'000);
    {
        struct ChangedClock : LifecycleDriver {
            ASIOBool init(void* window) override
            {
                if (inits == 1) {
                    sample_rate = 88200; // An external change between the first and second scenario.
                }
                return LifecycleDriver::init(window);
            }
        } driver;
        expect_throw<std::runtime_error>("changed baseline on reopen", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), options);
        });
        expect("only the first scenario allocated buffers", driver.creates, 1);
        expect("only the first scenario set and restored the clock", driver.set_rates.size(), 2);
        expect("external clock preserved", static_cast<int>(driver.sample_rate == 88200), 1);
        driver.check_closed();
    }
    {
        LifecycleDriver driver;
        for (auto rate : {0u, 1'000'001u}) {
            expect_throw<std::invalid_argument>("invalid probe target", [&] {
                probe_asio_rate_lifecycle(lifecycle_clsid, rate, driver.activation(), options);
            });
        }
        expect("invalid target does not activate the device", driver.attempts, 0);
    }
    {
        LifecycleDriver driver;
        auto few = options;
        few.switches = 4;
        expect_throw<std::invalid_argument>("too few switches", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), few);
        });
        expect("no device opened for invalid options", driver.attempts, 0);
    }
    {
        LifecycleDriver driver;
        expect_throw<std::runtime_error>("device already at target", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 48000, driver.activation(), options);
        });
        expect("no clock change at the target", driver.set_rates.size(), 0);
        driver.check_closed();
    }
    {
        LifecycleDriver driver;
        driver.can_rate_result = ASE_NoClock;
        expect_throw<std::runtime_error>("unsupported target", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), options);
        });
        expect("no clock change when refused", driver.set_rates.size(), 0);
        driver.check_closed();
    }
    {
        LifecycleDriver driver;
        driver.fail_start = true;
        expect_throw<std::runtime_error>("target start failure", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), options);
        });
        expect("failed start stopped", driver.stops, 1);
        expect("failed start buffers disposed", driver.disposals, driver.creates);
        expect("clock restored after a failure", static_cast<int>(driver.sample_rate == 48000), 1);
        driver.check_closed();
    }
    {
        LifecycleDriver driver;
        driver.ticks_per_start = 0;
        auto quick = options;
        quick.switch_timeout_ms = 50;
        const auto reports = probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), quick);
        for (const auto& report : reports) {
            expect("missing switches reported", static_cast<int>(report.target.timed_out), 1);
            expect("probe run incomplete on timeout", static_cast<int>(report.timed_out()), 1);
            expect("no timing without switches", report.target.steady_intervals, 0);
        }
        expect("every silent run disposed", driver.disposals, driver.creates);
        driver.check_closed();
    }
}

void test_lifecycle_buffer_validation()
{
    using enum MockAsio::BufferFault;
    for (const auto fault : {overlap, changed_rate, changed_type, inactive_output, swapped_channels, input_buffer}) {
        LifecycleDriver driver;
        driver.buffer_fault = fault;
        expect_throw<std::runtime_error>("reject invalid silent output before filling it", [&] {
            probe_asio_rate_lifecycle(lifecycle_clsid, 44100, driver.activation(), quick_lifecycle_options());
        });
        expect("rejected target buffer disposed", driver.disposals, 1);
        expect("all four halves untouched", static_cast<int>(driver.untouched_on_dispose), 1);
        expect("no invalid stream started", driver.starts, 0);
        expect("original clock after rejecting buffers", static_cast<int>(driver.sample_rate == 48000), 1);
        driver.check_closed();
    }
}

// The reported preferred width remains eight while createBuffers accepts a
// different exact width. Position increments are independent of wall timing.
class BufferProbeDriver : public LifecycleDriver {
public:
    unsigned create_attempts{}, position_observations{};
    long selected_width{};
    long position_step{}; // Zero: accepted width. Nonzero: independently reported hardware advance.
    bool reject_create{};
    ASIOError position_result = ASE_OK;

    BufferProbeDriver() { buffer_size = {8, 512, 8, -1}; }
    ASIOBool init(void* window) override { return RegisteredDriver::init(window); }
    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
    {
        ++create_attempts;
        selected_width = size;
        if (reject_create) {
            return ASE_InvalidParameter;
        }
        latency = size;
        return LifecycleDriver::createBuffers(infos, count, size, callbacks);
    }
    ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp*) override
    {
        const auto position = std::uint64_t{position_observations++} *
            static_cast<std::uint64_t>(position_step ? position_step : selected_width);
        samples->hi = static_cast<unsigned long>(position >> 32);
        samples->lo = static_cast<unsigned long>(position);
        return position_result;
    }
};

void test_buffer_probe()
{
    const auto options = quick_lifecycle_options();
    for (const long width : {8L, 64L, 256L}) {
        BufferProbeDriver driver;
        const auto report = probe_asio_buffer(lifecycle_clsid, width, driver.activation(), options);
        expect("buffer probe keeps explicit request", report.requested_frames, width);
        expect("buffer probe passes the exact width once", driver.selected_width, width);
        expect("buffer probe allocation attempts", driver.create_attempts, 1);
        expect("buffer probe accepted create width", report.run.width, width);
        expect("buffer probe retains reported preferred width", report.before.buffer_size.preferred, 8);
        expect("buffer probe reports preferred width after disposal", report.after.buffer_size.preferred, 8);
        expect("buffer probe preserves original clock", static_cast<int>(report.before.sample_rate == 48000 &&
            report.run.sample_rate == 48000 && report.after.sample_rate == 48000 && driver.sample_rate == 48000), 1);
        expect("buffer probe does not set or restore a clock", driver.set_rates.size(), 0);
        expect("buffer probe does not inquire another clock", std::ranges::count(driver.calls, MockAsio::Call::can_rate), 0);
        expect("buffer probe starts once", driver.starts, 1);
        expect("buffer probe stops once", driver.stops, 1);
        expect("buffer probe disposes once", driver.disposals, 1);
        expect("buffer probe completes requested callbacks", static_cast<int>(report.run.timed_out), 0);
        expect("buffer probe samples only requested positions", driver.position_observations, options.switches);
        expect("buffer probe has steady observations", static_cast<int>(report.run.steady_intervals > 0), 1);
        expect("buffer probe measured independent frame advance",
            static_cast<int>(report.run.position_frames_per_switch == static_cast<double>(width)), 1);
        expect("buffer probe prefilled A with silence", static_cast<int>(driver.first_a_silent), 1);
        expect("buffer probe prefilled B and submitted only silence",
            static_cast<int>(!driver.submitted.empty() && std::ranges::all_of(driver.submitted, MockAsio::silent)), 1);
        expect("buffer probe writes only the requested half", static_cast<int>(driver.peer_untouched), 1);
        driver.check_closed();
    }
}

void test_buffer_probe_failures()
{
    const auto options = quick_lifecycle_options();
    for (const auto* text : {L"", L"0", L"-8", L"+8", L" 8", L"8 ", L"8.0", L"2147483648", L"999999999999"}) {
        expect_throw<std::invalid_argument>("invalid explicit probe width", [text] { parse_probe_buffer_frames(text); });
    }
    expect_throw<std::invalid_argument>("null probe width", [] { parse_probe_buffer_frames(nullptr); });
    expect("small probe width", parse_probe_buffer_frames(L"8"), 8);
    expect("largest parsed probe width", parse_probe_buffer_frames(L"2147483647"), 2147483647);
    {
        BufferProbeDriver driver;
        for (const long width : {0L, -1L}) {
            expect_throw<std::invalid_argument>("nonpositive probe request", [&] {
                probe_asio_buffer(lifecycle_clsid, width, driver.activation(), options);
            });
        }
        auto invalid = options;
        invalid.switches = 1;
        expect_throw<std::invalid_argument>("too few buffer probe switches", [&] {
            probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), invalid);
        });
        expect("invalid buffer arguments precede activation", driver.attempts, 0);
    }
    for (const long width : {7L, 12L, 513L}) {
        BufferProbeDriver driver;
        expect_throw<std::invalid_argument>("width outside driver contract", [&] {
            probe_asio_buffer(lifecycle_clsid, width, driver.activation(), options);
        });
        expect("contract rejection precedes createBuffers", driver.create_attempts, 0);
        expect("contract rejection never starts", driver.starts, 0);
        expect("contract rejection keeps the clock", driver.set_rates.size(), 0);
        driver.check_closed();
    }
    {
        BufferProbeDriver driver;
        driver.reject_create = true;
        expect_throw<std::runtime_error>("driver refuses an advertised width", [&] {
            probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), options);
        });
        expect("create refusal has no fallback width", driver.create_attempts, 1);
        expect("create refusal keeps the exact request", driver.selected_width, 64);
        expect("create refusal never starts", driver.starts, 0);
        driver.check_closed();
    }
    {
        BufferProbeDriver driver;
        driver.buffer_fault = MockAsio::BufferFault::overlap;
        expect_throw<std::runtime_error>("invalid manual-width buffers", [&] {
            probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), options);
        });
        expect("invalid manual-width buffers remain untouched", static_cast<int>(driver.untouched_on_dispose), 1);
        expect("invalid manual-width buffers never start", driver.starts, 0);
        expect("invalid manual-width buffers disposed", driver.disposals, 1);
        driver.check_closed();
    }
    {
        BufferProbeDriver driver;
        driver.position_step = 8;
        const auto report = probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), options);
        expect("buffer probe retains accepted width when position differs", report.run.width, 64);
        expect("buffer probe reports measured rather than assumed advance",
            static_cast<int>(report.run.position_frames_per_switch == 8.0), 1);
        driver.check_closed();
    }
    {
        BufferProbeDriver driver;
        driver.position_result = ASE_NotPresent;
        const auto report = probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), options);
        expect("buffer probe records unavailable position API", report.run.position_error, static_cast<long>(ASE_NotPresent));
        expect("buffer probe does not invent unavailable sample advance", static_cast<int>(report.run.position_frames_per_switch.has_value()), 0);
        driver.check_closed();
    }
    {
        BufferProbeDriver driver;
        driver.ticks_per_start = 0;
        auto quick = options;
        quick.switch_timeout_ms = 50;
        const auto report = probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), quick);
        expect("missing manual-width callbacks report timeout", static_cast<int>(report.run.timed_out), 1);
        expect("timeout has no invented cadence", report.run.steady_intervals, 0);
        expect("timeout has no invented sample progression", static_cast<int>(report.run.position_frames_per_switch.has_value()), 0);
        expect("timed-out buffer stopped", driver.stops, 1);
        expect("timed-out buffer disposed", driver.disposals, 1);
        driver.check_closed();
    }
    for (const unsigned failure : {0u, 1u, 2u}) {
        BufferProbeDriver driver;
        driver.fail_start = failure == 0;
        driver.fail_stop = failure == 1;
        driver.fail_dispose = failure == 2;
        expect_throw<std::runtime_error>("buffer probe lifecycle failure", [&] {
            probe_asio_buffer(lifecycle_clsid, 64, driver.activation(), options);
        });
        expect("failed buffer probe tries disposal", driver.disposals, 1);
        expect("failed buffer probe does not alter the rate", driver.set_rates.size(), 0);
        if (failure == 1) {
            expect("stop failure detaches callbacks before disposal", static_cast<int>(driver.late_switch_untouched), 1);
        }
        driver.check_closed();
    }
}

enum class ControlAudition { pause, stop, paused_stop };

void run_control_audition(MockAsio& driver, ControlAudition mode, const wchar_t* path, std::chrono::milliseconds delay)
{
    if (mode == ControlAudition::pause) {
        audition_asio_pause(lifecycle_clsid, path, delay, driver.activation());
    } else {
        audition_asio_stop(lifecycle_clsid, path, delay, mode == ControlAudition::paused_stop, driver.activation());
    }
}

void test_control_audition_inputs(ControlAudition mode)
{
    using namespace std::chrono_literals;
    TempFile at48, at44;
    const auto* first = at48.write(wave({pcm16(2, 48000), pcm16_ramp(2, 10, 1)}));
    const auto* other = at44.write(wave({pcm16(2, 44100), pcm16_ramp(2, 10, 1)}));
    RegisteredDriver driver;
    for (const auto invalid : {-1ms, 60001ms}) {
        expect_throw<std::invalid_argument>("audition delay outside its range", [&] {
            run_control_audition(driver, mode, first, invalid);
        });
    }
    expect_throw<std::invalid_argument>("control audition WAV must be 48 kHz", [&] {
        run_control_audition(driver, mode, other, 0ms);
    });
    expect("invalid control audition never activates a driver", driver.attempts, 0);
}

void test_pause_audition()
{
    using namespace std::chrono_literals;
    constexpr std::size_t frames = 100 * 544 + 17;
    const auto pcm = pcm16_ramp(2, frames, 0x1234);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm}));
    LifecycleDriver driver;
    driver.ticks_per_start = 150;
    // Submit two audio blocks synchronously after the nine silent blocks. The
    // zero-delay timer therefore requests a real mid-track pause, even if the
    // test machine schedules it before the first asynchronous clock tick.
    driver.preroll_switches = 10;
    const auto owner = GetCurrentThreadId();
    bool wrong_thread{};
    std::vector<std::vector<MockAsio::Block>> streams;
    std::vector<wavplay::playback::PlaybackResult> stops;
    driver.on_call = [&](MockAsio::Call call) {
        wrong_thread |= GetCurrentThreadId() != owner || !in_sta();
        if (call == MockAsio::Call::stop) {
            streams.push_back(driver.submitted);
            stops.push_back(wavplay::playback::notifications().result());
        }
    };
    audition_asio_pause(lifecycle_clsid, path, 0ms, driver.activation());
    expect("pause audition keeps ASIO calls on its owner STA", static_cast<int>(wrong_thread), 0);
    expect("pause audition uses one initialized device", driver.activations, 1);
    expect("pause audition starts twice", driver.starts, 2);
    expect("pause audition stops twice", driver.stops, 2);
    expect("pause audition disposes both allocations", driver.disposals, 2);
    expect("pause audition captured both streams", streams.size(), 2);
    expect("first stop is paused", static_cast<int>(stops[0]), static_cast<int>(wavplay::playback::PlaybackResult::paused));
    expect("second stop is completed", static_cast<int>(stops[1]), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    std::size_t heard{};
    for (std::size_t stream = 0; stream < streams.size(); ++stream) {
        const auto before = heard;
        for (std::size_t block = 0; block < streams[stream].size(); ++block) {
            const auto& output = streams[stream][block];
            if (block < 9) {
                expect("pause audition primes start silence again", static_cast<int>(MockAsio::silent(output)), 1);
            }
            for (std::size_t slot = 0; slot < 544; ++slot) {
                const auto offset = slot * 3;
                const bool silent = output[0][offset] == 0 && output[0][offset + 1] == 0 && output[0][offset + 2] == 0 &&
                    output[1][offset] == 0 && output[1][offset + 1] == 0 && output[1][offset + 2] == 0;
                if (silent) {
                    continue;
                }
                if (heard >= frames) {
                    throw std::runtime_error("Pause audition repeated PCM after the final frame");
                }
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    expect("pause audition packed-24 low byte", output[channel][offset], 0);
                    expect("pause audition PCM low byte", output[channel][offset + 1], pcm.payload[4 * heard + 2 * channel]);
                    expect("pause audition PCM high byte", output[channel][offset + 2], pcm.payload[4 * heard + 2 * channel + 1]);
                }
                ++heard;
            }
        }
        expect("each audition stream includes audio", static_cast<int>(heard > before), 1);
        if (stream == 0) {
            expect("pause occurred before EOF", static_cast<int>(heard < frames), 1);
        }
    }
    expect("pause audition sends every frame exactly once", heard, frames);
    driver.check_closed();
    expect("completed pause audition releases its WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
}

void test_stop_audition(bool paused_first)
{
    using namespace std::chrono_literals;
    constexpr std::size_t width = 544, silent_blocks = 9, frames = 100 * width + 17;
    const auto pcm = pcm16_ramp(2, frames, 0x1234);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm}));
    LifecycleDriver driver;
    driver.ticks_per_start = 150;
    driver.preroll_switches = 10;
    const auto owner = GetCurrentThreadId();
    bool wrong_thread{};
    std::vector<std::vector<MockAsio::Block>> streams;
    std::vector<wavplay::playback::PlaybackResult> stops;
    driver.on_call = [&](MockAsio::Call call) {
        wrong_thread |= GetCurrentThreadId() != owner || !in_sta();
        if (call == MockAsio::Call::stop) {
            streams.push_back(driver.submitted);
            stops.push_back(wavplay::playback::notifications().result());
        }
        if (call == MockAsio::Call::create && driver.creates == 1) {
            expect("old stream stopped before restart allocation", driver.stops, 1);
            expect("old buffers disposed before restart allocation", driver.disposals, 1);
            expect("same driver remains initialized until restart", driver.inits, 1);
            expect("selected driver retained between streams", driver.releases, 0);
            expect("WAV retained between streams", probe_file_access(path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
        }
    };
    audition_asio_stop(lifecycle_clsid, path, 0ms, paused_first, driver.activation());
    expect("Stop audition keeps ASIO calls on its owner STA", static_cast<int>(wrong_thread), 0);
    expect("Stop audition activates once", driver.activations, 1);
    expect("Stop audition initializes once", driver.inits, 1);
    expect("Stop audition starts twice", driver.starts, 2);
    expect("Stop audition stops twice", driver.stops, 2);
    expect("Stop audition allocates fresh buffers for restart", driver.creates, 2);
    expect("Stop audition disposes both allocations", driver.disposals, 2);
    expect("Stop audition captured both streams", streams.size(), 2);
    expect("first stream has requested terminal result", static_cast<int>(stops[0]),
        static_cast<int>(paused_first ? wavplay::playback::PlaybackResult::paused : wavplay::playback::PlaybackResult::stopped));
    expect("restarted stream completed", static_cast<int>(stops[1]), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("first stream stopped before submitting the full WAV",
        static_cast<int>(streams[0].size() < silent_blocks + (frames + width - 1) / width), 1);
    expect("restart submitted every WAV frame",
        static_cast<int>(streams[1].size() >= silent_blocks + (frames + width - 1) / width), 1);
    // Check every submitted byte, including silence, PCM from frame 0, and EOF
    // padding. DAC consumption at Stop is verified separately by the track player tests.
    check_silence_then_pcm(streams[1], silent_blocks, pcm.payload, static_cast<long>(width));
    driver.check_closed();
    expect("completed Stop audition releases its WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
}

void test_control_audition_cancels_timer(ControlAudition mode)
{
    using namespace std::chrono_literals;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1, 1)}));
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        LifecycleDriver driver;
        driver.ticks_per_start = 20;
        driver.fail_start = scenario == 1;
        if (scenario == 2) {
            driver.buffer_fault = MockAsio::BufferFault::changed_type;
        }
        const auto began = std::chrono::steady_clock::now();
        expect_throw<std::runtime_error>("early EOF or driver failure cancels control audition", [&] {
            run_control_audition(driver, mode, path, 60000ms);
        });
        expect("cancel joins timer without waiting for its one-minute delay",
            static_cast<int>(std::chrono::steady_clock::now() - began < 5s), 1);
        expect("failed audition does not restart", driver.starts, scenario == 2 ? 0 : 1);
        expect("failed audition disposes its buffers", driver.disposals, 1);
        driver.check_closed();
        expect("failed audition releases its WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
    }
}

void test_control_audition_restart_failures(ControlAudition mode)
{
    using namespace std::chrono_literals;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100 * 544, 1)}));
    for (const bool fail_start : {false, true}) {
        LifecycleDriver driver;
        driver.ticks_per_start = 150;
        driver.preroll_switches = 10;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::create && driver.creates == 1) {
                driver.fail_start = fail_start;
                driver.buffer_fault = fail_start ? MockAsio::BufferFault::none : MockAsio::BufferFault::changed_type;
            }
        };
        expect_throw<std::runtime_error>("control audition rejects a failed restart", [&] {
            run_control_audition(driver, mode, path, 0ms);
        });
        expect("restart failure disposes both allocations", driver.disposals, 2);
        expect("invalid restart buffers are not started", driver.starts, fail_start ? 2 : 1);
        driver.check_closed();
        expect("restart failure releases its retained WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
    }
}

thread_local bool audition_reset_delivered{};
thread_local MockAsio* audition_reset_driver{}; // The driver the audition on this thread opened.

void CALLBACK reset_idle_audition(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    audition_reset_delivered = true;
    audition_reset_driver->message_notification(kAsioResetRequest, 0, nullptr, nullptr);
}

void test_control_audition_idle_reset(ControlAudition mode)
{
    using namespace std::chrono_literals;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100 * 544, 1)}));
    LifecycleDriver driver;
    driver.ticks_per_start = 150;
    driver.preroll_switches = 10;
    audition_reset_delivered = false;
    audition_reset_driver = &driver;
    UINT_PTR timer{};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::dispose && driver.disposals == 0) {
            timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, reset_idle_audition);
            if (!timer) {
                throw std::runtime_error("Could not schedule the stopped-driver reset");
            }
        }
    };
    expect_throw<std::runtime_error>("idle reset prevents automatic audition restart", [&] {
        run_control_audition(driver, mode, path, 0ms);
    });
    if (timer) {
        KillTimer(nullptr, timer);
    }
    expect("idle STA dispatched the driver reset", static_cast<int>(audition_reset_delivered), 1);
    expect("idle reset did not start another stream", driver.starts, 1);
    driver.check_closed();
}

void test_control_audition_idle_quit(ControlAudition mode)
{
    using namespace std::chrono_literals;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100 * 544, 1)}));
    LifecycleDriver driver;
    driver.ticks_per_start = 150;
    driver.preroll_switches = 10;
    UINT_PTR timer{};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::dispose && driver.disposals == 0) {
            timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, request_quit);
            if (!timer) {
                throw std::runtime_error("Could not schedule quit while idle");
            }
        }
    };
    expect_throw<std::runtime_error>("idle quit prevents automatic audition restart", [&] {
        run_control_audition(driver, mode, path, 0ms);
    });
    if (timer) {
        KillTimer(nullptr, timer);
    }
    MSG quit{};
    expect("audition preserves WM_QUIT", PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
    expect("audition preserves quit exit code", quit.wParam, 37);
    expect("idle quit does not start another stream", driver.starts, 1);
    driver.check_closed();
    expect("idle quit releases its retained WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
}

void test_stop_audition_teardown_faults()
{
    using namespace std::chrono_literals;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100 * 544, 1)}));
    for (const bool paused_first : {false, true}) {
        for (const auto boundary : {MockAsio::Call::stop, MockAsio::Call::dispose}) {
            LifecycleDriver driver;
            driver.ticks_per_start = 150;
            driver.preroll_switches = 10;
            bool reset{};
            driver.on_call = [&](MockAsio::Call call) {
                if (call == boundary && !reset) {
                    reset = true;
                    driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
                }
            };
            expect_throw<std::runtime_error>("driver fault overrides Stop audition", [&] {
                audition_asio_stop(lifecycle_clsid, path, 0ms, paused_first, driver.activation());
            });
            expect("fault boundary was exercised", static_cast<int>(reset), 1);
            expect("Stop audition does not restart after a fault", driver.starts, 1);
            expect("Stop audition fault still disposes its allocation", driver.disposals, 1);
            driver.check_closed();
            expect("Stop audition fault releases its WAV", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
        }
    }
}

} // namespace

int wavplay_test::run_diagnostics_tests()
{
    TestSuite suite;
    suite.run("PCM capture matcher preserves samples and rejects damaged buffers", test_pcm_capture_matcher);
    suite.run_timed("same-rate and transition audition on the mock driver", test_audition_modes);
    suite.run_timed("pause audition resumes on its owning STA", test_pause_audition);
    suite.run_timed("Stop audition restarts from frame 0 on its owning STA", [] { test_stop_audition(false); });
    suite.run_timed("paused Stop audition restarts from frame 0 on its owning STA", [] { test_stop_audition(true); });
    for (const auto mode : {ControlAudition::pause, ControlAudition::stop, ControlAudition::paused_stop}) {
        const std::string name = mode == ControlAudition::pause ? "pause audition" :
            mode == ControlAudition::stop ? "Stop audition" : "paused Stop audition";
        suite.run_timed((name + " validates its WAV and delay before activation").c_str(),
            [mode] { test_control_audition_inputs(mode); });
        suite.run_timed((name + " cancels its timer on EOF and exceptions").c_str(),
            [mode] { test_control_audition_cancels_timer(mode); });
        suite.run_timed((name + " releases resources after restart failures").c_str(),
            [mode] { test_control_audition_restart_failures(mode); });
        suite.run_timed((name + " refuses reset while waiting to restart").c_str(),
            [mode] { test_control_audition_idle_reset(mode); });
        suite.run_timed((name + " cancels restart on WM_QUIT").c_str(),
            [mode] { test_control_audition_idle_quit(mode); });
    }
    suite.run_timed("driver faults override Stop audition at teardown", test_stop_audition_teardown_faults);
    suite.run_timed("silent rate lifecycle probe across fresh opens", test_rate_lifecycle_probe);
    suite.run_timed("silent rate lifecycle probe failures", test_rate_lifecycle_probe_failures);
    suite.run_timed("silent probe validates all buffers before filling either half", test_lifecycle_buffer_validation);
    suite.run_timed("explicit-width silence probe preserves the clock and checks sample advance", test_buffer_probe);
    suite.run_timed("explicit-width silence probe refusal, timeout and teardown", test_buffer_probe_failures);
    return suite.result("ASIO diagnostics");
}
