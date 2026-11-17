#include "suites/suites.h"
#include "wavplay/control/playback_controller.h"
#include "wavplay/core/win32.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/playback/playback_signal.h"
#include "diagnostics/audition.h"
#include "fixtures/call_gate.h"
#include "fixtures/playback_fixture.h"
#include "framework/supervisor.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
using wavplay::control::PlaybackCommand;
using wavplay::control::PlaybackController;
using wavplay::control::PlaybackSnapshot;
using wavplay::control::PlaybackState;
using wavplay::playback::PlaybackResult;

constexpr wchar_t selected_text[] = L"{76543210-89AB-CDEF-8123-456789ABCDEF}";

// The caller owns the fixture and passes activation() to the controller, whose
// new STA activates it once. Capture data is read only after an idle snapshot or after join().
class ControllerDriver final : public MockAsio {
public:
    static constexpr long block = 48;
    unsigned activations{}, inits{}, releases{}, references{}, overload_queries{};
    DWORD owner_thread{};
    HWND owner_window{};
    bool bad_lifetime{}, initialized{}, fail_init{}, refuse_restore{}, notify_rate_changes{};
    std::function<void()> on_init, on_release, on_future;
    std::vector<std::vector<Block>> captures;

    explicit ControllerDriver(bool packed24 = false, long frames = block)
    {
        buffer_size = {frames, frames, frames, 0};
        next_width = frames;
        latency = next_latency = 73;
        left_type = right_type = next_left_type = next_right_type = packed24 ? ASIOSTInt24LSB : ASIOSTInt16LSB;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        note_owner();
        bad_lifetime |= references != 1 || !IsWindow(owner_window);
        if (on_release) {
            on_release();
        }
        ++releases;
        initialized = false;
        return --references;
    }
    ASIOBool init(void* window) override
    {
        note_owner();
        ++inits;
        owner_window = static_cast<HWND>(window);
        bad_lifetime |= !IsWindow(owner_window) || GetWindowThreadProcessId(owner_window, nullptr) != owner_thread;
        if (on_init) {
            on_init();
        }
        initialized = !fail_init;
        return initialized ? ASIOTrue : ASIOFalse;
    }
    ASIOError future(long, void*) override
    {
        note_owner();
        ++overload_queries;
        if (on_future) { on_future(); }
        return ASE_NotPresent;
    }
    ASIOError start() override
    {
        const std::lock_guard lock(clock_gate_);
        started = std::promise<void>{};
        return MockAsio::start();
    }
    ASIOError stop() override
    {
        const std::lock_guard lock(clock_gate_);
        const auto result = MockAsio::stop();
        if (result == ASE_OK) {
            captures.push_back(submitted);
        }
        return result;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override
    {
        if (refuse_restore && rate == 44100) {
            note_owner();
            set_rates.push_back(rate);
            return ASE_HWMalfunction;
        }
        const auto result = MockAsio::setSampleRate(rate);
        if (result == ASE_OK && notify_rate_changes && rate_notification) {
            rate_notification(rate);
        }
        return result;
    }
    void pulse()
    {
        const std::lock_guard lock(clock_gate_);
        MockAsio::tick();
    }
    void check_closed() const
    {
        expect("controller releases driver references", references, 0);
        expect("controller releases every activation", releases, activations);
        expect("all driver calls and the owner window stay on the STA", static_cast<int>(bad_lifetime), 0);
        expect("controller worker is separate from submitter", static_cast<int>(owner_thread != GetCurrentThreadId()), 1);
        expect("controller destroys its owner window after release", IsWindow(owner_window), FALSE);
    }

protected:
    void check_entry(Call) override
    {
        note_owner();
        bad_lifetime |= !initialized || references != 1;
    }

    IASIO* activated(const CLSID& clsid) override
    {
        owner_thread = GetCurrentThreadId();
        bad_lifetime |= !in_sta() || references != 0 || !IsEqualCLSID(clsid, selected);
        ++activations;
        AddRef();
        return this;
    }

private:
    std::mutex clock_gate_;
    void note_owner() { bad_lifetime |= !in_sta() || GetCurrentThreadId() != owner_thread; }
};

class StaMessageProbe {
public:
    std::function<std::uint64_t()> inspect;
    HWND window{};

    void attach()
    {
        static std::once_flag registered;
        constexpr wchar_t class_name[] = L"WavplayControllerTestProbe";
        std::call_once(registered, [] {
            WNDCLASSW type{};
            type.lpfnWndProc = window_proc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"WavplayControllerTestProbe";
            if (!RegisterClassW(&type)) {
                throw std::runtime_error("Could not register controller STA test window");
            }
        });
        window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
            nullptr, GetModuleHandleW(nullptr), this);
        if (!window) {
            throw std::runtime_error("Could not create controller STA test window");
        }
    }
    void post() const { expect("STA probe message is queued", PostMessageW(window, probe_message, 0, 0), TRUE); }
    bool ready() const { return serviced_.wait_for(0ms) == std::future_status::ready; }
    std::uint64_t wait()
    {
        if (serviced_.wait_for(hang_budget(5s)) != std::future_status::ready) {
            throw std::runtime_error("Controller did not service a queued STA message");
        }
        return serviced_.get();
    }
    void detach()
    {
        DestroyWindow(window);
        window = nullptr;
    }

private:
    static constexpr UINT probe_message = WM_APP + 29;
    std::promise<std::uint64_t> promise_;
    std::future<std::uint64_t> serviced_{promise_.get_future()};

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<StaMessageProbe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<StaMessageProbe*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == probe_message) {
            try {
                self->promise_.set_value(self->inspect());
            } catch (...) {
                self->promise_.set_exception(std::current_exception());
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

PlaybackSnapshot await(PlaybackController& controller, auto ready, ControllerDriver* driver = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    auto state = controller.snapshot();
    while (!ready(state)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Controller state deadline expired at state " + std::to_string(static_cast<int>(state.state)) +
                ", accepted " + std::to_string(state.accepted) + ", settled " + std::to_string(state.settled));
        }
        if (driver && (state.state == PlaybackState::playing || state.state == PlaybackState::pausing)) {
            driver->pulse();
        }
        auto next = controller.wait_for_change(state.revision, 1ms);
        expect("controller revisions do not regress", static_cast<int>(next.revision >= state.revision), 1);
        expect("controller accepted receipts do not regress", static_cast<int>(next.accepted >= state.accepted), 1);
        expect("controller settled receipts do not regress", static_cast<int>(next.settled >= state.settled), 1);
        expect("controller cannot settle an unaccepted command", static_cast<int>(next.settled <= next.accepted), 1);
        state = std::move(next);
    }
    return state;
}

PlaybackSnapshot state_is(PlaybackController& controller, PlaybackState wanted, ControllerDriver* driver = nullptr)
{
    return await(controller, [wanted](const auto& state) { return state.state == wanted; }, driver);
}

std::uint64_t send(PlaybackController& controller, PlaybackCommand command)
{
    const auto receipt = controller.submit(command);
    expect("controller accepts command", static_cast<int>(receipt != 0), 1);
    return receipt;
}

PlaybackSnapshot settle(PlaybackController& controller, std::uint64_t receipt, ControllerDriver* driver = nullptr)
{
    return await(controller, [receipt](const auto& state) { return state.settled >= receipt; }, driver);
}

PlaybackSnapshot close(PlaybackController& controller)
{
    const auto receipt = send(controller, PlaybackCommand::close);
    expect("closed input rejects Play immediately", controller.submit(PlaybackCommand::play), 0);
    const auto state = state_is(controller, PlaybackState::closed);
    expect("close settles accepted requests", state.settled, receipt);
    controller.join();
    return state;
}

// The controller's hidden top-level owner windows that exist in this process.
std::size_t owner_windows()
{
    std::size_t count{};
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        DWORD process{};
        wchar_t name[32]{};
        if (GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() &&
            GetClassNameW(window, name, 32) && std::wstring_view(name) == L"WavplayPlaybackController") {
            ++*reinterpret_cast<std::size_t*>(context);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
    return count;
}

void check_capture(const std::vector<MockAsio::Block>& captured, const Bytes& pcm, std::size_t offset,
    std::size_t frames, bool packed24, std::size_t width = ControllerDriver::block)
{
    if (captured.size() * width < frames) {
        throw std::runtime_error("Controller replay did not submit every PCM frame");
    }
    expect_true("controller capture uses a valid source-frame range",
        pcm.size() % 4 == 0 && offset <= pcm.size() / 4 && frames <= pcm.size() / 4 - offset);
    check_pcm16_capture(captured, width, std::span<const std::uint8_t>(pcm).subspan(offset * 4, frames * 4),
        packed24 ? 3 : 2);
}

void test_settings_read_once()
{
    TempFile audio, configuration;
    const auto path = audio.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4099, 0x1234)}));
    const auto write_settings = [&](std::string_view values) {
        const auto text = std::string("[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\n") + std::string(values);
        return configuration.write(Bytes(text.begin(), text.end()));
    };
    const auto settings_path = write_settings("BufferSize=96\nStartSilence=7\n");
    {
        ControllerDriver driver;
        driver.buffer_size = {48, 192, 48, 48};
        PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms}, {}, settings_path);
        const auto initialized = state_is(controller, PlaybackState::stopped);
        expect_true("configuration is confirmed before stopped", initialized.settings_loaded);
        expect("configured frames are visible before allocation", initialized.requested_buffer_frames, 96);
        expect_true("requested width is not an invented allocation", !initialized.diagnostics.segments[0]);
        expect_true("startup does not invent buffer capabilities", !initialized.diagnostics.buffer_capabilities[0]);
        write_settings("BufferSize=48\nStartSilence=0\n");
        for (unsigned run = 0; run < 2; ++run) {
            const auto playing = settle(controller, send(controller, PlaybackCommand::play));
            expect("configuration is not reread on Play", playing.diagnostics.segments[0].value().buffer_frames, 96);
            expect("silence is retained and rounded to complete blocks",
                playing.diagnostics.segments[0].value().start_silence_blocks, 4);
            (void)settle(controller, send(controller, PlaybackCommand::stop));
            if (run == 0) {
                driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
                const auto recovered = await(controller, [](const auto& state) {
                    return state.state == PlaybackState::stopped && !state.diagnostics.segments[0];
                });
                expect("reinitialization retains startup buffer settings", recovered.requested_buffer_frames, 96);
                expect_true("reinitialization clears the previous buffer capabilities", !recovered.diagnostics.buffer_capabilities[0]);
                expect("settings check includes a fresh driver instance", driver.inits, 2);
            }
        }
        (void)close(controller);
        driver.check_closed();
    }
    {
        ControllerDriver driver;
        driver.buffer_size = {48, 192, 48, 48};
        PlaybackController controller(selected, path, driver.activation(), {}, {}, settings_path);
        const auto initialized = state_is(controller, PlaybackState::stopped);
        expect("new controller observes the edited file", initialized.requested_buffer_frames, 48);
        const auto playing = settle(controller, send(controller, PlaybackCommand::play));
        expect("new launch uses edited width", playing.diagnostics.segments[0].value().buffer_frames, 48);
        expect("explicit zero overrides default silence", playing.diagnostics.segments[0].value().start_silence_blocks, 0);
        (void)close(controller);
        driver.check_closed();
    }
    {
        write_settings("FutureKey=not-supported\n");
        ControllerDriver driver;
        driver.buffer_size = {48, 192, 48, 48};
        PlaybackController controller(selected, path, driver.activation(), {.start_silence = 11ms, .buffer_frames = 144}, {}, settings_path);
        (void)state_is(controller, PlaybackState::stopped);
        const auto playing = settle(controller, send(controller, PlaybackCommand::play));
        expect("unspecified width retains injected settings", playing.diagnostics.segments[0].value().buffer_frames, 144);
        expect("unspecified silence retains injected settings", playing.diagnostics.segments[0].value().start_silence_blocks, 4);
        (void)close(controller);
        driver.check_closed();
    }
}

const wchar_t* write_playlist(TempFile& list, std::initializer_list<std::wstring_view> paths,
    std::string_view prefix = {})
{
    Bytes bytes(prefix.begin(), prefix.end());
    for (const auto path : paths) {
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
            static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
        if (!length) { throw std::runtime_error("Cannot encode test playlist"); }
        const auto offset = bytes.size();
        bytes.resize(offset + static_cast<std::size_t>(length));
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
            reinterpret_cast<char*>(bytes.data() + offset), length, nullptr, nullptr)) {
            throw std::runtime_error("Cannot encode test playlist");
        }
        bytes.push_back('\n');
    }
    return list.write(bytes);
}

void test_playlist_snapshot_and_completion()
{
    TempFile first, second, third, list;
    const auto a = pcm16_ramp(2, 3, 11), b = pcm16_ramp(2, 5, 91), c = pcm16_ramp(2, 7, 211);
    const auto ap = first.write(wave({pcm16(2, 48000), a}));
    const auto bp = second.write(wave({pcm16(2, 48000), b}));
    const auto cp = third.write(wave({pcm16(2, 48000), c}));
    const auto list_path = write_playlist(list, {ap, bp, cp, ap}, "# playlist\nrelative.wav\n\n");
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {}, list_path);
    const auto initial = state_is(controller, PlaybackState::stopped);
    // No player exists yet: the controller's list, its published state and this
    // snapshot own the list. The idle worker copies no snapshot of its own.
    const auto owners = initial.playlist.use_count();
    expect("playlist preserves all duplicate entries", initial.playlist->size(), 4);
    expect("playlist reports excluded paths", initial.excluded_count, 1);
    expect_true("playlist starts without selection or playback", !initial.selected_index && driver.starts == 0);
    expect("an unselected playlist has no invented time base", initial.sample_rate, 0);
    expect("out-of-range selection rejected", controller.select_track(4), 0);
    list.write(Bytes{0xff}); // Neither Play nor repeated snapshots may reload it.
    const auto started = settle(controller, send(controller, PlaybackCommand::play));
    expect_true("Play selects a current item", started.selected_index.has_value());
    const auto done = await(controller, [](const auto& state) {
        return state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    expect_true("completion follows the last duplicate item", done.current_index == 3 && done.selected_index == 3);
    expect("position is relative to final track", done.position_frames, 3);
    expect_true("snapshots share one immutable list", done.playlist == initial.playlist);
    expect("all short tracks share one ASIO start", driver.starts, 1);
    auto expected = a.payload;
    expected.append_range(b.payload);
    expected.append_range(c.payload);
    expected.append_range(a.payload);
    check_capture(driver.captures.at(0), expected, 0, 18, false);
    const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
    expect_true("Stop preserves last selection", stopped.selected_index == 3);
    expect("Stop at EOF resets track position", stopped.position_frames, 0);
    // Three more snapshots (started, done, stopped) and the idle player, which
    // shares the controller's list instead of holding a copy.
    expect("the player co-owns the controller's list", stopped.playlist.use_count(), owners + 4);
    (void)close(controller);
    driver.check_closed();
}

void test_submitted_time_and_transport()
{
    TempFile file;
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint64_t frames = rate * 5 + 7;
    const auto path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, frames)}));
    ControllerDriver driver(false, 480);
    driver.preroll_switches = 0;
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 100ms});
    const auto initial = state_is(controller, PlaybackState::stopped);
    expect("idle duration uses WAV frames", initial.frame_count, frames);
    expect("idle duration has the WAV rate", initial.sample_rate, rate);
    const auto started = settle(controller, send(controller, PlaybackCommand::play));
    expect("prefilled start silence has no elapsed PCM", started.position_frames, 0);
    for (unsigned index = 0; index < 9; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    const auto silent = controller.wait_for_change(started.revision, 20ms);
    expect("start silence does not publish time progress", silent.revision, started.revision);
    expect("start silence stays at zero", silent.position_frames, 0);
    for (unsigned index = 0; index < 100; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    const auto progressed = await(controller, [](const auto& state) { return state.position_frames >= rate; });
    expect("elapsed time follows submitted PCM, not elapsed wall time", progressed.position_frames, rate);
    expect("progress keeps the selected track's total", progressed.frame_count, frames);
    expect("progress is not a transport command", progressed.settled, started.settled);
    wavplay::playback::wake_playback_control();
    const auto unchanged = controller.wait_for_change(progressed.revision, 20ms);
    expect("unchanged seconds do not create state notifications", unchanged.revision, progressed.revision);
    const auto paused = settle(controller, send(controller, PlaybackCommand::pause), &driver);
    expect_true("Pause retains the confirmed submitted position", paused.position_frames >= progressed.position_frames);
    const auto held = controller.wait_for_change(paused.revision, 20ms);
    expect("Pause does not advance time", held.position_frames, paused.position_frames);
    const auto resumed = settle(controller, send(controller, PlaybackCommand::play));
    expect("resume silence retains the paused time", resumed.position_frames, paused.position_frames);
    const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
    expect("Stop rewinds elapsed time", stopped.position_frames, 0);
    expect("Stop preserves the total and its rate", stopped.frame_count, frames);
    expect("Stop keeps the WAV time base", stopped.sample_rate, rate);
    (void)close(controller);
    driver.check_closed();
}

void test_time_refresh_preserves_long_block_watchdog()
{
    TempFile file;
    constexpr std::uint32_t rate = 48000;
    const auto path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, rate * 8)}));
    // Half-second buffers give the existing four-period watchdog a two-second
    // deadline. Time must refresh before it, without resetting that deadline.
    ControllerDriver driver(false, rate / 2);
    driver.preroll_switches = 0;
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    const auto started = settle(controller, send(controller, PlaybackCommand::play));
    expect("one half-second block is prefilled", started.position_frames, rate / 2);
    for (unsigned index = 0; index < 3; ++index) { driver.pulse(); }
    // No control wake or further callbacks: the control wait must publish PCM
    // progress itself, and must then detect the missing callbacks normally.
    const auto progressed = await(controller, [](const auto& state) { return state.position_frames >= rate * 2; });
    expect("long-block progress is published while playing", progressed.state, PlaybackState::playing);
    expect("long-block progress has no wall-clock interpolation", progressed.position_frames, rate * 2);
    const auto failed = state_is(controller, PlaybackState::failed);
    expect("progress wakes cannot keep a dead driver alive", failed.result, PlaybackResult::driver_error);
    (void)close(controller);
    driver.check_closed();
}

void test_idle_selection_publication()
{
    TempFile first, second, list;
    constexpr std::array<std::uint32_t, 2> rates{44100, 48000};
    constexpr std::array<std::size_t, 2> frames{701, 1001};
    const auto ap = first.write(wave({pcm16(2, rates[0]), pcm16_ramp(2, frames[0], 1)}));
    const auto bp = second.write(wave({pcm16(2, rates[1]), pcm16_ramp(2, frames[1], 501)}));
    ControllerDriver driver;
    CallGate starting;
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start) { starting.block(); }
    };
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {ap, bp}));
    ReleaseGate release_start{starting};
    (void)state_is(controller, PlaybackState::stopped);
    for (std::size_t index = 0; index < rates.size(); ++index) {
        const auto before = controller.snapshot();
        const auto receipt = controller.select_track(index);
        expect_true("idle selection is accepted", receipt != 0);
        const auto chosen = settle(controller, receipt);
        expect_true("idle selection publishes the chosen item and completed receipt together",
            chosen.state == PlaybackState::stopped && chosen.result == PlaybackResult::stopped &&
            chosen.current_index == index && chosen.selected_index == index &&
            chosen.settled == receipt && chosen.accepted == receipt && !chosen.track_error && !chosen.faulted());
        expect("completed selection publishes its own duration", chosen.frame_count, frames[index]);
        expect("completed selection publishes its own rate", chosen.sample_rate, rates[index]);
        expect("completed selection starts at frame zero", chosen.position_frames, 0);
        // In this idle interval no other operation or progress is published:
        // one acceptance and one complete selection must be the only changes.
        expect("selection has one completion publication", chosen.revision, before.revision + 2);
        expect("selection completion stamps all changed state", chosen.state_revision, chosen.revision);
    }
    expect("selection alone never starts audio", driver.starts, 0);

    const auto before_play = controller.snapshot();
    const auto play = controller.select_track(0, true);
    expect_true("Select and Play is accepted", play != 0);
    starting.wait();
    const auto pending = controller.snapshot();
    expect_true("Select and Play exposes its prepared metadata before start",
        pending.current_index == 0 && pending.selected_index == 0 &&
        pending.frame_count == frames[0] && pending.sample_rate == rates[0]);
    expect("preparing Select and Play does not settle its receipt", pending.settled, before_play.settled);
    starting.open();
    const auto playing = settle(controller, play);
    expect_true("Select and Play settles only after start succeeds",
        playing.state == PlaybackState::playing && playing.settled == play);
    (void)close(controller);
    driver.check_closed();
}

void test_playlist_selection_and_pause()
{
    TempFile first, second, third, list;
    const auto a = pcm16_ramp(2, 60, 1), b = pcm16_ramp(2, 4099, 0x2345), c = pcm16_ramp(2, 193, 0x5678);
    const auto ap = first.write(wave({pcm16(2, 48000), a}));
    const auto bp = second.write(wave({pcm16(2, 48000), b}));
    const auto cp = third.write(wave({pcm16(2, 48000), c}));
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {ap, bp, cp}));
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    for (unsigned index = 0; index < 5; ++index) { driver.pulse(); }
    const auto paused = settle(controller, send(controller, PlaybackCommand::pause), &driver);
    expect_true("pause across boundary selects second track", paused.current_index == 1 && paused.selected_index == 1);
    expect_true("pause retains a position inside second track", paused.position_frames > 0 && paused.position_frames < 4099);
    const auto resumed = settle(controller, send(controller, PlaybackCommand::play));
    expect_true("resume stays on second track", resumed.current_index == 1);
    const auto chosen = settle(controller, controller.select_track(2));
    expect_true("click changes selection while current track keeps playing",
        chosen.selected_index == 2 && chosen.current_index == 1 && chosen.state == PlaybackState::playing);
    expect("click does not restart ASIO", driver.starts, 2);
    const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
    expect_true("Stop keeps a manually selected item", stopped.selected_index == 2);
    expect_true("Stop makes selected metadata current", stopped.current_index == 2);
    expect("Stop displays the selected item's length", stopped.frame_count, 193);
    expect("Stop resets the selected item's displayed position", stopped.position_frames, 0);
    expect("Stop retains the selected WAV rate", stopped.sample_rate, 48000);
    const auto restarted = settle(controller, send(controller, PlaybackCommand::play));
    expect_true("Play after Stop starts the selected item", restarted.current_index == 2);
    const auto chosen_play = settle(controller, controller.select_track(1, true));
    expect_true("explicit selection playback restarts the requested item", chosen_play.current_index == 1);
    expect("explicit selection playback waits for teardown", driver.starts, 4);
    (void)settle(controller, send(controller, PlaybackCommand::stop));
    (void)close(controller);
    driver.check_closed();
}

// The refused item's reason, rethrown from track_error as `Error`.
template<class Error>
std::string refusal_reason(const PlaybackSnapshot& state)
{
    if (!state.track_error) {
        fail("A track refusal has no reason");
    }
    try {
        std::rethrow_exception(state.track_error);
    } catch (const Error& error) {
        return error.what();
    }
}

void test_playlist_refusal_and_reselection()
{
    for (const bool unavailable_rate : {false, true}) {
        TempFile first, rejected, last, list;
        const auto a = pcm16_ramp(2, 7, 1), c = pcm16_ramp(2, 9, 71);
        const auto ap = first.write(wave({pcm16(2, 48000), a}));
        const auto bp = rejected.write(unavailable_rate
            ? wave({pcm16(2, 96000), pcm16_ramp(2, 11)})
            : wave({pcm24(2, 48000), pcm24_ramp(2, 11)}));
        const auto cp = last.write(wave({pcm16(2, 48000), c}));
        ControllerDriver driver;
        if (unavailable_rate) {
            driver.on_call = [&](MockAsio::Call call) {
                if (call == MockAsio::Call::stop) { driver.can_rate_result = ASE_NoClock; }
            };
        }
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
            write_playlist(list, {ap, bp, cp}));
        (void)state_is(controller, PlaybackState::stopped);
        (void)send(controller, PlaybackCommand::play);
        const auto refused = await(controller, [](const auto& state) {
            return state.state == PlaybackState::stopped && state.track_error;
        }, &driver);
        expect_true("unplayable next item is selected and stopped", refused.selected_index == 1 && refused.current_index == 1);
        expect("refused track starts at zero", refused.position_frames, 0);
        expect("refusal retains the rejected WAV's known length", refused.frame_count, 11);
        expect("refusal retains the rejected WAV's own rate", refused.sample_rate, unavailable_rate ? 96000 : 48000);
        expect_true("track refusal does not poison controller", !refused.faulted());
        // Neither refusal has an exception of its own, so each keeps its fixed text.
        expect_true("a rate or format refusal keeps its fixed text",
            refusal_reason<std::runtime_error>(refused) == (unavailable_rate
                ? "The selected track's sample rate is not supported by this device."
                : "The selected track cannot be read or converted losslessly for this device."));
        expect("refused next item never starts", driver.starts, 1);
        check_capture(driver.captures.at(0), a.payload, 0, 7, false);
        driver.on_call = {};
        driver.can_rate_result = ASE_OK;
        const auto playing = settle(controller, controller.select_track(2, true));
        expect_true("another selection clears the refusal", playing.current_index == 2 && !playing.track_error);
        (void)await(controller, [](const auto& state) {
            return state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
        }, &driver);
        check_capture(driver.captures.at(1), c.payload, 0, 9, false);
        expect_true("recovered playlist closes cleanly", !close(controller).faulted());
        driver.check_closed();
    }
}

void test_playlist_refusal_reasons()
{
    // A refusal that has its own exception shows it instead of the generic
    // text: an unsupported BufferSize names the requested width, the driver's
    // range and the item's rate, and a following item that no longer opens
    // keeps the reader's error.
    enum class Case { width_at_96k, width_after_rate_change, open_failure };
    for (const auto refusal : {Case::width_at_96k, Case::width_after_rate_change, Case::open_failure}) {
        TempFile first, second, list;
        const auto rate = refusal == Case::width_at_96k ? 96000u : refusal == Case::width_after_rate_change ? 44100u : 48000u;
        const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 7, 1)}));
        const auto bp = second.write(wave({pcm16(2, rate), pcm16_ramp(2, 11, 31)}));
        // BufferSize=8: valid at 48 kHz, below the minimum of 16 at the refused rate.
        ControllerDriver driver(false, 8);
        driver.next_width = 16; // The range after setSampleRate(44100).
        if (refusal == Case::width_at_96k) {
            driver.sample_rate = 96000;
            driver.buffer_size = {16, 2048, 512, -1};
        }
        const bool only_refused = refusal == Case::width_at_96k;
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms, .buffer_frames = 8},
            {}, {}, only_refused ? write_playlist(list, {bp}) : write_playlist(list, {ap, bp}));
        (void)state_is(controller, PlaybackState::stopped);
        std::string expected;
        if (refusal == Case::open_failure) {
            second.write(Bytes{'b', 'a', 'd'}); // After loading validated the list.
            try {
                (void)wavplay::media::WavReader(bp);
            } catch (const std::runtime_error& error) {
                expected = error.what();
            }
            expect_true("the damaged item fails to open", !expected.empty());
        } else {
            expected = "Unsupported ASIO buffer size: requested=8, minimum=16, preferred=" +
                std::string(only_refused ? "512, maximum=2048, granularity=-1" : "16, maximum=16, granularity=0") +
                ", rate=" + std::to_string(rate);
        }
        (void)send(controller, PlaybackCommand::play);
        const auto refused = await(controller, [](const auto& state) {
            return state.state == PlaybackState::stopped && state.track_error;
        }, &driver);
        const std::size_t index = only_refused ? 0 : 1;
        expect_true("the refused item is selected and current", refused.selected_index == index && refused.current_index == index);
        expect("the refusal is a track refusal", static_cast<int>(refused.result), static_cast<int>(PlaybackResult::next_track_rejected));
        expect_true("a refusal with a reason is not a fault", !refused.faulted());
        const auto reason = refusal == Case::open_failure ? refusal_reason<std::runtime_error>(refused) :
            refusal_reason<std::invalid_argument>(refused);
        expect_true("the refusal shows its own reason: " + reason, reason == expected);
        if (refusal == Case::open_failure) {
            // Play again without reselecting: TrackPlayer reopens the refused item
            // itself, and its failure is the reason, not the generic text.
            const auto retried = settle(controller, send(controller, PlaybackCommand::play));
            expect("a failed reopen is a track refusal", static_cast<int>(retried.result),
                static_cast<int>(PlaybackResult::next_track_rejected));
            expect_true("a failed reopen keeps the item selected and stopped", retried.state == PlaybackState::stopped &&
                retried.selected_index == index && retried.current_index == index && !retried.faulted());
            const auto again = refusal_reason<std::runtime_error>(retried);
            expect_true("a failed reopen shows its own reason: " + again, again == expected);
        }
        expect("only the playable item starts", driver.starts, only_refused ? 0 : 1);
        expect("a refused width never allocates buffers", driver.creates, only_refused ? 0 : 1);
        expect_true("the refused playlist closes cleanly", !close(controller).faulted());
        driver.check_closed();
    }
}

void test_stop_refreshes_selected_time()
{
    for (const bool corrupt_selection : {false, true}) {
        TempFile first, next_rate, chosen, list;
        const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4099)}));
        const auto bp = next_rate.write(wave({pcm16(2, 44100), pcm16_ramp(2, 7)}));
        const auto cp = chosen.write(wave({pcm16(2, 96000), pcm16_ramp(2, 13)}));
        ControllerDriver driver;
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
            write_playlist(list, {ap, bp, cp}));
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        // The next-rate boundary ends the prefetch run before this third file.
        // Selection while playing must not open it or disturb the playing time.
        if (corrupt_selection) { chosen.write(Bytes{'b', 'a', 'd'}); }
        const auto selected_state = settle(controller, controller.select_track(2));
        expect("a playing selection keeps the active track's duration", selected_state.frame_count, 4099);
        expect("a playing selection keeps the active track's rate", selected_state.sample_rate, 48000);
        const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
        expect_true("Stop publishes the selected item", stopped.selected_index == 2 && stopped.current_index == 2);
        expect("the selected item's elapsed time starts at zero", stopped.position_frames, 0);
        expect("Stop does not start the selected rate", driver.starts, 1);
        if (corrupt_selection) {
            expect_true("a missing duration is a recoverable file refusal", stopped.track_error && !stopped.faulted());
            expect("a refused selection clears the prior duration", stopped.frame_count, 0);
            expect("a refused selection clears the prior time base", stopped.sample_rate, 0);
        } else {
            expect("Stop takes total frames from the selected WAV", stopped.frame_count, 13);
            expect("Stop takes the selected WAV rate, not the idle device rate", stopped.sample_rate, 96000);
        }
        (void)close(controller);
        driver.check_closed();
    }
}

void test_empty_and_invalid_playlists()
{
    TempFile list;
    {
        ControllerDriver driver;
        PlaybackController controller(selected, L"", driver.activation(), {}, {}, {},
            write_playlist(list, {}, "# empty\nrelative.wav\n"));
        const auto initial = state_is(controller, PlaybackState::stopped);
        expect_true("empty playlist remains a usable stopped window", initial.playlist->empty() && !initial.faulted());
        expect("empty playlist excludes its invalid path", initial.excluded_count, 1);
        expect("empty playlist rejects Play", controller.submit(PlaybackCommand::play), 0);
        expect("empty playlist rejects selection", controller.select_track(0), 0);
        (void)settle(controller, send(controller, PlaybackCommand::stop));
        (void)close(controller);
        expect("empty list never starts ASIO", driver.starts, 0);
        driver.check_closed();
    }
    {
        ControllerDriver driver;
        PlaybackController controller(selected, L"", driver.activation(), {}, {}, {}, list.write(Bytes{0xff}));
        const auto failed = state_is(controller, PlaybackState::failed);
        expect_true("invalid UTF-8 is a list error, not exclusions", failed.error && !failed.playlist);
        expect("invalid list does not initialize ASIO", driver.activations, 0);
        (void)close(controller);
    }
}

// When the Stop after a startup selection arrives: while start is held, while
// createBuffers is held, or once the run has started.
enum class StartupStop { during_start, before_start, after_start };

void test_playlist_selection_receipts_and_stop()
{
    for (const auto stop_at : {StartupStop::during_start, StartupStop::before_start, StartupStop::after_start}) {
        TempFile first, second, list;
        const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)}));
        const auto bp = second.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 501)}));
        ControllerDriver driver;
        CallGate creating, starting;
        bool created{}, started{};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::create && !std::exchange(created, true)) { creating.block(); }
            if (call == MockAsio::Call::start && !std::exchange(started, true)) { starting.block(); }
        };
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
            write_playlist(list, {ap, bp}));
        ReleaseGate release_create{creating}, release_start{starting};
        (void)state_is(controller, PlaybackState::stopped);
        const auto play = send(controller, PlaybackCommand::play);
        creating.wait();
        const auto selection = controller.select_track(1);
        expect_true("startup selection is accepted", selection > play);
        std::uint64_t stop{};
        if (stop_at == StartupStop::before_start) { stop = send(controller, PlaybackCommand::stop); }
        creating.open();
        if (stop_at != StartupStop::before_start) {
            starting.wait();
            const auto pending = controller.snapshot();
            expect_true("selection is visible before blocked start returns", pending.selected_index == 1);
            expect_true("selection cannot settle the preceding unstarted Play", pending.settled < play);
            if (stop_at == StartupStop::during_start) { stop = send(controller, PlaybackCommand::stop); }
        }
        starting.open();
        if (stop_at == StartupStop::after_start) {
            // The selection was delivered before the held start (checked above),
            // so the start settles its receipt, not only the Play's.
            const auto playing = state_is(controller, PlaybackState::playing);
            expect("the start settles the selection delivered during startup", playing.settled, selection);
            expect_true("the startup selection stays selected while the first item plays",
                playing.selected_index == 1 && playing.current_index == 0);
            stop = send(controller, PlaybackCommand::stop);
        }
        const auto stopped = settle(controller, stop);
        expect_true("startup Stop retains the manually selected track", stopped.selected_index == 1);
        expect_true("startup selection and Stop finish together", stopped.state == PlaybackState::stopped && stopped.settled == stop);
        expect("selection does not hide a Stop that cancels start", driver.starts,
            stop_at == StartupStop::before_start ? 0 : 1);
        (void)close(controller);
        driver.check_closed();
    }
    {
        TempFile first, second, list;
        const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)}));
        const auto bp = second.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 501)}));
        ControllerDriver driver;
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
            write_playlist(list, {ap, bp}));
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        const auto pause = send(controller, PlaybackCommand::pause);
        (void)state_is(controller, PlaybackState::pausing);
        (void)controller.select_track(1);
        // No callback arrives, so the drain cannot end: the selection shows at
        // once, but its receipt stays behind the unfinished Pause.
        const auto selected_state = await(controller, [](const auto& state) { return state.selected_index == 1; });
        expect_true("a selection during the pause drain is shown while pausing",
            selected_state.state == PlaybackState::pausing);
        expect_true("a selection cannot settle the unfinished Pause", selected_state.settled < pause);
        const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
        expect_true("selection cannot hide Stop behind an unfinished pause drain",
            stopped.state == PlaybackState::stopped && stopped.selected_index == 1);
        expect("pause-selection-Stop requires no further audio callbacks", driver.starts, 1);
        (void)close(controller);
        driver.check_closed();
    }
}

void test_superseded_idle_selections()
{
    // The STA opens and validates the file of every idle selection it
    // prepares. A batch oplock holds that open for the first Select while
    // five more queue behind it: only the last of them may open its file,
    // and its settlement covers the skipped receipts. A Select followed by
    // anything other than a selection is still prepared.
    std::array<TempFile, 6> files;
    std::array<const wchar_t*, 6> paths{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        paths[index] = files[index].write(wave({pcm16(2, 48000), pcm16_ramp(2, 100 + index, 1)}));
    }
    TempFile list;
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {paths[0], paths[1], paths[2], paths[3], paths[4], paths[5]}));
    (void)state_is(controller, PlaybackState::stopped);
    // After loading, which validates every item and releases its handles.
    // Declared after the controller, the gates open before it closes.
    OpenGate first(paths[0]), second(paths[1]), third(paths[2]), fourth(paths[3]), fifth(paths[4]);
    (void)controller.select_track(0);
    first.wait();
    std::uint64_t last{};
    for (std::size_t index = 1; index < files.size(); ++index) {
        last = controller.select_track(index);
        expect_true("a queued selection is accepted", last != 0);
    }
    first.open();
    const auto skipped = await(controller, [&](const auto& state) {
        expect_true("a superseded selection never opens its file",
            !second.reached() && !third.reached() && !fourth.reached() && !fifth.reached());
        return state.settled >= last;
    });
    expect("the last selection settles every queued receipt", skipped.settled, last);
    expect_true("the last selection is current without an error",
        skipped.selected_index == 5 && skipped.current_index == 5 && !skipped.track_error);
    expect("the last selection opened its own file", skipped.frame_count, 105);
    expect_true("superseded selections leave their files unopened",
        !second.reached() && !third.reached() && !fourth.reached() && !fifth.reached());

    OpenGate again(paths[0]);
    (void)controller.select_track(0);
    again.wait();
    third.open();
    (void)controller.select_track(2);
    const auto pause = send(controller, PlaybackCommand::pause);
    again.open();
    const auto paused = settle(controller, pause);
    expect_true("a Select followed by Pause is prepared", paused.selected_index == 2 && paused.current_index == 2);
    expect("a Select followed by Pause opens its file", paused.frame_count, 102);
    expect("no selection starts audio", driver.starts, 0);

    // A Select directly followed by Select and Play is superseded as well: the
    // second item's gate stays closed, and the run starts at the chosen item.
    const auto pcm = [](std::size_t index) { return pcm16_ramp(2, 100 + index, 1).payload; };
    fifth.open();
    OpenGate held(paths[0]);
    (void)controller.select_track(0);
    held.wait();
    (void)controller.select_track(1);
    const auto chosen = controller.select_track(4, true);
    held.open();
    const auto played = await(controller, [&](const auto& state) {
        expect_true("a Select before Select and Play never opens its file", !second.reached());
        return state.settled >= chosen && state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    expect_true("Select and Play completes the list from its own item", played.current_index == 5 && !played.track_error);
    auto from_chosen = pcm(4);
    from_chosen.append_range(pcm(5));
    check_capture(driver.captures.at(0), from_chosen, 0, 104 + 105, false);

    // A Select followed by Play is prepared first, so Play starts the item it
    // selected, not the one prepared before it.
    fourth.open();
    OpenGate held_again(paths[0]);
    (void)controller.select_track(0);
    held_again.wait();
    (void)controller.select_track(3);
    const auto play = send(controller, PlaybackCommand::play);
    held_again.open();
    (void)await(controller, [&](const auto& state) {
        // Starting at the first item would open the second one's closed gate.
        expect_true("Play starts the item selected before it", !second.reached());
        return state.settled >= play && state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    auto from_selected = pcm(3);
    from_selected.append_range(pcm(4));
    from_selected.append_range(pcm(5));
    check_capture(driver.captures.at(1), from_selected, 0, 103 + 104 + 105, false);
    expect("each Play starts audio once", driver.starts, 2);
    (void)close(controller);
    driver.check_closed();
}

// Polls the process-wide playback signal until the preparing request has
// accepted a Stop into it. The signal's only event is auto-reset and the
// controller's own thread consumes it, so the test cannot wait on it; the
// result is the only observation, and the poll is bounded to fail, not hang.
void await_accepted_stop(const char* failure)
{
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    while (wavplay::playback::notifications().result() != PlaybackResult::stopped) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail(failure);
        }
        std::this_thread::sleep_for(1ms);
    }
}

void test_stop_and_close_while_run_is_prepared()
{
    // The reader validates every following same-rate item before priming. The
    // gate holds its open of the second item, so the run stays in preparation
    // until the test opens it. Stop and Close must still reach the request,
    // end validation before the third item and never allocate or start ASIO
    // buffers; a later Play starts from the first item.
    for (const bool closing : {false, true}) {
        TempFile first, second, third, list;
        const auto a = pcm16_ramp(2, 7, 1), b = pcm16_ramp(2, 11, 31), c = pcm16_ramp(2, 13, 71);
        const auto ap = first.write(wave({pcm16(2, 48000), a}));
        const auto bp = second.write(wave({pcm16(2, 48000), b}));
        const auto cp = third.write(wave({pcm16(2, 48000), c}));
        const auto list_path = write_playlist(list, {ap, bp, cp});
        ControllerDriver driver;
        PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {}, list_path);
        (void)state_is(controller, PlaybackState::stopped);
        // After loading, which validates every item and releases its handles.
        // Declared after the controller, the gates open before it closes.
        OpenGate second_open(bp), third_open(cp);
        const auto play = send(controller, PlaybackCommand::play);
        second_open.wait();
        const auto request = send(controller, closing ? PlaybackCommand::close : PlaybackCommand::stop);
        const auto delivered = state_is(controller, closing ? PlaybackState::closing : PlaybackState::stopping);
        expect_true("the held run has not settled its Play", delivered.settled < play);
        // The poll publishes the state first, then requests the Stop that the
        // request accepts into its signal; the reader checks both.
        await_accepted_stop("The preparing request did not accept the Stop");
        second_open.open();
        const auto ended = await(controller, [&](const auto& state) {
            expect_true("the reader did not open the third item", !third_open.reached());
            return closing ? state.state == PlaybackState::closed : state.settled >= request;
        });
        if (closing) {
            expect("Close settles every accepted request", ended.settled, request);
            expect_true("Close during preparation ends clean, without a fault",
                ended.result == PlaybackResult::stopped && !ended.faulted() && !ended.error);
            controller.join();
        } else {
            expect_true("Stop during preparation ends stopped at the first item",
                ended.state == PlaybackState::stopped && ended.result == PlaybackResult::stopped &&
                ended.current_index == 0 && ended.position_frames == 0);
        }
        expect_true("the reader did not open the third item", !third_open.reached());
        expect("the cancelled run never allocates buffers", driver.creates, 0);
        expect("the cancelled run never starts", driver.starts, 0);
        if (!closing) {
            third_open.open();
            (void)send(controller, PlaybackCommand::play);
            const auto done = await(controller, [](const auto& state) {
                return state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
            }, &driver);
            expect("a later Play ends at the third item", done.current_index.value_or(0), 2);
            auto expected = a.payload;
            expected.append_range(b.payload);
            expected.append_range(c.payload);
            check_capture(driver.captures.at(0), expected, 0, 31, false);
            (void)close(controller);
        }
        driver.check_closed();
    }
}

void test_select_and_play_while_run_is_prepared()
{
    // As above, the reader's open of the second item is held. Select and Play
    // of the fourth item must stop the prepared run the same way, before its
    // third item and without buffers, and then play the chosen item alone.
    TempFile first, second, third, fourth, list;
    const auto a = pcm16_ramp(2, 7, 1), b = pcm16_ramp(2, 11, 31), c = pcm16_ramp(2, 13, 71), d = pcm16_ramp(2, 17, 113);
    const auto ap = first.write(wave({pcm16(2, 48000), a}));
    const auto bp = second.write(wave({pcm16(2, 48000), b}));
    const auto cp = third.write(wave({pcm16(2, 48000), c}));
    const auto dp = fourth.write(wave({pcm16(2, 48000), d}));
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {ap, bp, cp, dp}));
    (void)state_is(controller, PlaybackState::stopped);
    // After loading; declared after the controller, so they open before it closes.
    OpenGate second_open(bp), third_open(cp);
    const auto play = send(controller, PlaybackCommand::play);
    second_open.wait();
    const auto chosen = controller.select_track(3, true);
    expect_true("Select and Play is accepted while the run is prepared", chosen > play);
    const auto delivered = state_is(controller, PlaybackState::stopping);
    expect_true("the held run has not settled its Play", delivered.settled < play);
    await_accepted_stop("The preparing request did not accept the Stop of Select and Play");
    second_open.open();
    const auto done = await(controller, [&](const auto& state) {
        expect_true("the reader did not open the third item", !third_open.reached());
        return state.settled >= chosen && state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    expect_true("the chosen item is current", done.current_index == 3 && done.selected_index == 3);
    expect("only the chosen item allocates buffers", driver.creates, 1);
    expect("only the chosen item starts", driver.starts, 1);
    check_capture(driver.captures.at(0), d.payload, 0, 17, false);
    (void)close(controller);
    driver.check_closed();
}

void test_playlist_revalidates_changed_files()
{
    TempFile first, last, list;
    const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 7, 1)}));
    const auto data = pcm16_ramp(2, 11, 81);
    const auto bp = last.write(wave({pcm16(2, 48000), data}));
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {ap, bp}));
    const auto initial = state_is(controller, PlaybackState::stopped);
    first.write(Bytes{'b', 'a', 'd'});
    const auto refused = settle(controller, send(controller, PlaybackCommand::play));
    expect_true("a changed first file is a recoverable track refusal",
        refused.state == PlaybackState::stopped && refused.track_error && !refused.faulted());
    expect("unknown rejected media has no stale time base", refused.sample_rate, 0);
    expect("unknown rejected media has no stale duration", refused.frame_count, 0);
    expect_true("changed media does not rewrite the startup list", refused.playlist == initial.playlist);
    expect("changed media is never started", driver.starts, 0);
    const auto playing = settle(controller, controller.select_track(1, true));
    expect_true("another row remains playable after an opening failure", playing.current_index == 1 && !playing.track_error);
    (void)await(controller, [](const auto& state) { return state.result == PlaybackResult::completed; }, &driver);
    check_capture(driver.captures.at(0), data.payload, 0, 11, false);
    expect_true("changed-file refusal closes cleanly after reselection", !close(controller).faulted());
    driver.check_closed();
}

void test_settings_failure_before_activation()
{
    TempFile audio, configuration;
    const auto path = audio.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100)}));
    for (const auto value : {"BufferSize=-1", "StartSilence=1001"}) {
        const auto text = std::string("[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\n") + value;
        ControllerDriver driver;
        PlaybackController controller(selected, path, driver.activation(), {}, {}, configuration.write(Bytes(text.begin(), text.end())));
        const auto failed = state_is(controller, PlaybackState::failed);
        expect_true("invalid configuration remains observable", failed.error && !failed.settings_loaded);
        expect("a settings error creates no owner window", owner_windows(), 0);
        expect_throw<std::invalid_argument>("invalid settings preserve their diagnostic", [&] { std::rethrow_exception(failed.error); });
        (void)close(controller);
        expect("invalid settings never activate ASIO", driver.activations, 0);
        expect("invalid settings never start playback", driver.starts, 0);
    }
}

void test_buffer_capability_diagnostics()
{
    enum class Failure { manual_width, create, invalid_range, inquiry };
    for (const auto failure : {Failure::manual_width, Failure::create, Failure::invalid_range, Failure::inquiry}) {
        TempFile file;
        ControllerDriver driver;
        driver.buffer_size = {48, 192, 48, 48};
        if (failure == Failure::invalid_range) { driver.buffer_size.preferred = 72; }
        if (failure == Failure::inquiry) { driver.buffer_size_result = ASE_HWMalfunction; }
        if (failure == Failure::create) {
            driver.on_call = [&](MockAsio::Call call) {
                // A driver can reject a width that its earlier inquiry allowed.
                if (call == MockAsio::Call::create) { driver.buffer_size = {48, 48, 48, 0}; }
            };
        }
        const long requested = failure == Failure::manual_width ? 72 : 96;
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(),
            {.start_silence = 0ms, .buffer_frames = requested});
        (void)state_is(controller, PlaybackState::stopped);
        const auto play = send(controller, PlaybackCommand::play);
        const auto failed = state_is(controller, PlaybackState::failed);
        expect("buffer refusal settles the Play receipt", failed.settled, play);
        expect_true("buffer refusal keeps its exception", static_cast<bool>(failed.error));
        expect("buffer refusal preserves the exact requested width", failed.requested_buffer_frames, requested);
        expect_true("buffer refusal never invents a verified allocation", !failed.diagnostics.segments[0]);
        const bool valid_inquiry = failure == Failure::manual_width || failure == Failure::create;
        const auto& capabilities = failed.diagnostics.buffer_capabilities[0];
        expect_true("only a successful valid inquiry publishes capabilities", capabilities.has_value() == valid_inquiry);
        if (capabilities) {
            expect_true("buffer capabilities identify the confirmed WAV clock", capabilities->sample_rate == 48000);
            expect("refusal retains the queried minimum", capabilities->minimum, 48);
            expect("refusal retains the queried maximum", capabilities->maximum, 192);
            expect("refusal retains the queried preferred width", capabilities->preferred, 48);
            expect("refusal retains the queried granularity", capabilities->granularity, 48);
        }
        expect("buffer refusal does not retry another width", std::ranges::count(driver.calls, MockAsio::Call::create),
            failure == Failure::create ? 1 : 0);
        expect("buffer refusal never starts ASIO", driver.starts, 0);
        (void)close(controller);
        driver.check_closed();
    }
}

void test_buffer_capabilities_refresh()
{
    TempFile file;
    ControllerDriver driver;
    CallGate inquiry;
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::size && driver.size_queries == 1) {
            driver.buffer_size = {96, 192, 96, 96};
            inquiry.block();
        }
    };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    ReleaseGate release_inquiry{inquiry};
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto stopped = settle(controller, send(controller, PlaybackCommand::stop));
    expect("Stop retains the last queried capability", stopped.diagnostics.buffer_capabilities[0].value().preferred, 48);
    const auto replay = send(controller, PlaybackCommand::play);
    inquiry.wait();
    const auto starting = controller.snapshot();
    expect_true("buffer inquiry keeps Play pending", starting.state == PlaybackState::starting && starting.settled < replay);
    expect_true("new Play clears both old capability and allocation before inquiry",
        !starting.diagnostics.buffer_capabilities[0] && !starting.diagnostics.segments[0]);
    inquiry.open();
    const auto playing = settle(controller, replay);
    const auto& capabilities = playing.diagnostics.buffer_capabilities[0].value();
    expect("new Play publishes the newly queried preferred width", capabilities.preferred, 96);
    expect("new Play publishes the newly queried maximum", capabilities.maximum, 192);
    expect("zero configuration follows the new preferred width", playing.diagnostics.segments[0].value().buffer_frames, 96);
    (void)close(controller);
    driver.check_closed();
}

void test_application_settings_and_window_save()
{
    TempFile audio, configuration;
    const auto data = pcm16_ramp(2, 4099, 0x1234);
    const auto path = audio.write(wave({pcm16(2, 48000), data}));
    const std::string prefix = "[General]\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\n"
        "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nName=Studio \xe6\x97\xa5\xe6\x9c\xac\n";
    const auto original = prefix + "OutputPair=3\nBufferSize=96\nStartSilence=0\n"
        "[Window]\nX=30\nY=40\nWidth=700\nHeight=500\nMaximized=0\n";
    const auto settings_path = configuration.write(Bytes(original.begin(), original.end()));
    ControllerDriver driver;
    driver.outputs = 4;
    driver.first_output = 2;
    driver.buffer_size = {48, 192, 48, 48};
    PlaybackController controller(CLSID{}, path, driver.activation(), {}, {}, settings_path);
    const auto initialized = state_is(controller, PlaybackState::stopped);
    expect_true("INI selection initializes the configured device", IsEqualCLSID(initialized.selected_device, selected));
    expect_true("configuration and device are separately confirmed", initialized.settings_loaded && initialized.device_available);
    expect_true("Unicode device label reaches the snapshot", initialized.device_name == L"Studio \u65e5\u672c");
    expect("configured pair is visible before playback", initialized.output_pair, 3);
    expect("window coordinates load on the worker", initialized.window.value().x, 30);
    expect("window width loads on the worker", initialized.window.value().width, 700);

    // A completed manual edit affects the next launch, while the current device
    // and every later stream start retain the startup values.
    const auto edited = prefix + "OutputPair=1\nBufferSize=48\nStartSilence=23\n";
    configuration.write(Bytes(edited.begin(), edited.end()));
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto paused = settle(controller, send(controller, PlaybackCommand::pause), &driver);
    expect_true("configured pair can pause", paused.state == PlaybackState::paused);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    (void)state_is(controller, PlaybackState::completed, &driver);
    (void)settle(controller, send(controller, PlaybackCommand::stop));
    driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
    const auto recovered = await(controller, [](const auto& state) {
        return state.state == PlaybackState::stopped && !state.diagnostics.segments[0];
    });
    expect("reset retains the selected output pair", recovered.output_pair, 3);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    (void)state_is(controller, PlaybackState::completed, &driver);
    const wavplay::settings::WindowPlacement bounds{-120, 50, 900, 650, true};
    const auto receipt = controller.close(bounds);
    expect_true("window Close is accepted", receipt != 0);
    expect("a second Close cannot replace the accepted bounds", controller.close({}), 0);
    const auto closed = state_is(controller, PlaybackState::closed);
    expect("window save settles before Close acknowledgment", closed.settled, receipt);
    expect_true("settings save completes without fault", !closed.faulted());
    controller.join();
    driver.check_closed();
    expect("pause, resume and reset replay each allocate once", driver.created_outputs.size(), 3);
    for (const auto& pair : driver.created_outputs) {
        expect("all starts retain physical left output 3", pair[0], 2);
        expect("all starts retain physical right output 4", pair[1], 3);
    }
    check_capture(driver.captures.at(0), data.payload, 0, paused.position_frames, false, 96);
    check_capture(driver.captures.at(1), data.payload, paused.position_frames, 4099 - paused.position_frames, false, 96);
    check_capture(driver.captures.at(2), data.payload, 0, 4099, false, 96);
    const auto saved = wavplay::settings::load_application_config(settings_path);
    expect("Close preserves manually edited OutputPair", saved.device.output_pair.value(), 1);
    expect("Close preserves manually edited BufferSize", saved.device.buffer_frames.value(), 48);
    expect("Close preserves manually edited StartSilence", saved.device.start_silence.value().count(), 23);
    expect("Close writes normal window X", saved.window.value().x, -120);
    expect("Close writes normal window width", saved.window.value().width, 900);
    expect_true("Close writes maximized state independently of normal bounds", saved.window.value().maximized);
}

void test_no_device_and_save_failures()
{
    TempFile audio, configuration;
    const auto path = audio.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100)}));
    const wavplay::settings::WindowPlacement bounds{20, 30, 700, 500, false};
    for (const auto text : {"", "[General]\nSelectedDevice=\n", "[General]\nSelectedDevice=invalid\n"}) {
        const std::string contents(text);
        const auto settings_path = configuration.write(Bytes(contents.begin(), contents.end()));
        if (contents.empty()) { expect_true("test removes the initial INI", DeleteFileW(settings_path) != FALSE); }
        ControllerDriver driver;
        PlaybackController controller(CLSID{}, path, driver.activation(), {}, {}, settings_path);
        const auto failed = state_is(controller, PlaybackState::failed);
        expect_true("missing or invalid device selection is visible", failed.faulted() && !failed.device_available);
        expect("a missing selection never activates any device", driver.activations, 0);
        expect("a missing selection creates no owner window", owner_windows(), 0);
        expect("a failed selection never accepts Play", controller.submit(PlaybackCommand::play), 0);
        const auto receipt = controller.close(bounds);
        expect_true("failed initialization still closes", receipt != 0);
        (void)state_is(controller, PlaybackState::closed);
        controller.join();
        if (failed.settings_loaded) {
            expect_true("no-device startup can still save the window",
                wavplay::settings::load_application_config(settings_path).window.has_value());
        } else {
            expect_throw<std::invalid_argument>("invalid configuration is never overwritten by Close", [&] {
                (void)wavplay::settings::load_application_config(settings_path);
            });
        }
    }
    const std::string source = "[General]\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\n";
    const auto settings_path = configuration.write(Bytes(source.begin(), source.end()));
    {
        PlaybackController controller(CLSID{}, path, [](const CLSID& id) -> IASIO* {
            expect_true("activation receives only the selected CLSID", IsEqualCLSID(id, selected));
            throw std::runtime_error("Configured ASIO device is missing");
        }, {}, {}, settings_path);
        const auto failed = state_is(controller, PlaybackState::failed);
        expect_true("missing installed device keeps its selected CLSID", IsEqualCLSID(failed.selected_device, selected));
        expect_true("failed activation is unavailable without a fallback", !failed.device_available);
        (void)controller.close(bounds);
        (void)state_is(controller, PlaybackState::closed);
        controller.join();
        expect("window can be saved after activation failure",
            wavplay::settings::load_application_config(settings_path).window.value().height, 500);
    }
    {
        ControllerDriver driver;
        PlaybackController controller(CLSID{}, path, driver.activation(), {}, {}, settings_path);
        (void)state_is(controller, PlaybackState::stopped);
        expect("an initialized device has one owner window", owner_windows(), 1);
        const wavplay::core::UniqueHandle locked(CreateFileW(settings_path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        expect_true("test locks the settings file for the shutdown write", locked.get() != INVALID_HANDLE_VALUE);
        (void)controller.close(wavplay::settings::WindowPlacement{100, 100, 800, 600, false});
        const auto closed = state_is(controller, PlaybackState::closed);
        expect_true("save failure remains observable after driver release", closed.faulted() && closed.error);
        controller.join();
        driver.check_closed();
    }
    expect("failed write preserves previous window bounds",
        wavplay::settings::load_application_config(settings_path).window.value().x, 20);
}

void test_close_during_list_load()
{
    // No device is selected. A batch oplock holds the playlist load's open of
    // its item while Close is accepted: the load completes and publishes the
    // list, then startup releases without entering the device phase, so the
    // missing device is neither checked nor reported.
    TempFile audio, list;
    const auto path = audio.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100)}));
    const auto list_path = write_playlist(list, {path});
    OpenGate load(path);
    ControllerDriver driver;
    PlaybackController controller(CLSID{}, L"", driver.activation(), {}, {}, {}, list_path);
    ReleaseOpen release{load};
    load.wait();
    const auto receipt = send(controller, PlaybackCommand::close);
    load.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    controller.join();
    expect_true("Close during the list load reports no NO DEVICE fault", !closed.faulted() && !closed.error);
    expect("Close during the list load settles", closed.settled, receipt);
    expect_true("the list loaded before Close is published", closed.playlist && closed.playlist->size() == 1);
    expect("Close during the list load activates no device", driver.activations, 0);
}

void test_no_device_after_playlist()
{
    // Startup publishes the settings, then the playlist with its exclusions,
    // and only then reports NO DEVICE, which keeps the list visible.
    TempFile first, second, list;
    const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 7, 1)}));
    const auto bp = second.write(wave({pcm16(2, 48000), pcm16_ramp(2, 11, 31)}));
    ControllerDriver driver;
    PlaybackController controller(CLSID{}, L"", driver.activation(), {}, {}, {},
        write_playlist(list, {ap, bp}, "relative.wav\n"));
    const auto failed = state_is(controller, PlaybackState::failed);
    expect_true("the settings are published before NO DEVICE", failed.settings_loaded);
    expect_true("the playlist is published before NO DEVICE", failed.playlist && failed.playlist->size() == 2);
    expect("the excluded row is published before NO DEVICE", failed.excluded_count, 1);
    expect_true("NO DEVICE is a fault without a device", failed.faulted() && !failed.device_available);
    std::string reason;
    try {
        std::rethrow_exception(failed.error);
    } catch (const std::invalid_argument& error) {
        reason = error.what();
    }
    expect_true("the failure is NO DEVICE: " + reason, reason.starts_with("NO DEVICE"));
    expect("NO DEVICE activates no device", driver.activations, 0);
    expect("NO DEVICE creates no owner window", owner_windows(), 0);
    expect("NO DEVICE accepts no Play", controller.submit(PlaybackCommand::play), 0);
    (void)close(controller);
}

void test_close_during_wav_validation()
{
    // A device is selected. A batch oplock holds the --wav item's validation,
    // the first open of the file, while Close is accepted: startup then
    // releases without creating the owner window or activating the driver.
    TempFile audio;
    const auto path = audio.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100)}));
    OpenGate validation(path);
    ControllerDriver driver;
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
    ReleaseOpen release{validation};
    validation.wait();
    const auto receipt = send(controller, PlaybackCommand::close);
    validation.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    controller.join();
    expect_true("Close during the --wav validation is not a fault", !closed.faulted() && !closed.error);
    expect("Close during the --wav validation settles", closed.settled, receipt);
    expect_true("the --wav item is published before its validation", closed.playlist && closed.playlist->size() == 1);
    expect("Close during the --wav validation activates no device", driver.activations, 0);
    expect_true("Close during the --wav validation initializes no driver", !driver.owner_window && driver.inits == 0);
}

void test_close_cancels_list_validation()
{
    // A device is selected. Close is accepted while the first of several
    // entries is validated: that entry completes, the load returns no list at
    // the next line, and startup releases without publishing the valid
    // prefix. Without the per-entry check the load would hold at the second
    // entry's gate until the await deadline expired.
    TempFile first, second, list;
    const auto first_path = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 100)}));
    const auto second_path = second.write(wave({pcm16(2, 44100), pcm16_ramp(2, 100)}));
    const auto list_path = write_playlist(list, {first_path, second_path, first_path, second_path});
    OpenGate held(first_path), next(second_path);
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {}, {}, {}, list_path);
    ReleaseOpen release_held{held}, release_next{next};
    held.wait();
    const auto receipt = send(controller, PlaybackCommand::close);
    const auto released = std::chrono::steady_clock::now();
    held.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    const auto elapsed = std::chrono::steady_clock::now() - released;
    controller.join();
    expect_true("Close stops the list before its next entry", !next.reached());
    expect_true("Close during validation publishes no partial list", !closed.playlist && closed.excluded_count == 0);
    expect_true("Close during validation reaches closed promptly", elapsed < hang_budget(2s));
    expect_true("Close during validation is not a fault", !closed.faulted() && !closed.error);
    expect("Close during validation settles", closed.settled, receipt);
    expect("Close during validation activates no device", driver.activations, 0);
}

void test_pause_resume_and_eof_replay()
{
    constexpr std::size_t frames = 7 * ControllerDriver::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x1234);
    for (const bool packed24 : {false, true}) {
        TempFile file;
        ControllerDriver driver(packed24);
        PlaybackController controller(selected, file.write(wave({pcm16(2, 48000), data})), driver.activation(), {.start_silence = 0ms});
        const auto initialized = state_is(controller, PlaybackState::stopped);
        expect("controller reports track length after initialization", initialized.frame_count, frames);
        const auto play = send(controller, PlaybackCommand::play);
        const auto playing = settle(controller, play);
        expect("Play settles after ASIO start", static_cast<int>(playing.state), static_cast<int>(PlaybackState::playing));
        const auto duplicate = send(controller, PlaybackCommand::play);
        (void)settle(controller, duplicate);
        const auto pause = send(controller, PlaybackCommand::pause);
        (void)state_is(controller, PlaybackState::pausing);
        const auto paused = settle(controller, pause, &driver);
        expect("Pause settles after teardown", static_cast<int>(paused.state), static_cast<int>(PlaybackState::paused));
        expect("pause confirms submitted valid frames", paused.position_frames, 2 * ControllerDriver::block);
        const auto resume = send(controller, PlaybackCommand::play);
        (void)settle(controller, resume);
        // A Play while playing settles at once without touching the run. No
        // other request follows it, so only EOF ends this run: it must keep
        // that receipt settled rather than fall back to the run's own Play.
        const auto repeated = send(controller, PlaybackCommand::play);
        (void)settle(controller, repeated);
        const auto completed = state_is(controller, PlaybackState::completed, &driver);
        expect("EOF keeps a duplicate Play settled", completed.settled, repeated);
        expect("resumed EOF position", completed.position_frames, frames);
        const auto replay = send(controller, PlaybackCommand::play);
        (void)settle(controller, replay);
        (void)state_is(controller, PlaybackState::completed, &driver);
        (void)close(controller);
        expect("duplicate Play does not create another run", driver.starts, 3);
        expect("selected driver remains initialized across pause and EOF", driver.inits, 1);
        expect("every started allocation is disposed", driver.disposals, 3);
        expect("three controller captures", driver.captures.size(), 3);
        check_capture(driver.captures[0], data.payload, 0, paused.position_frames, packed24);
        check_capture(driver.captures[1], data.payload, paused.position_frames, frames - paused.position_frames, packed24);
        check_capture(driver.captures[2], data.payload, 0, frames, packed24);
        driver.check_closed();
    }
}

void test_same_track_select_and_play()
{
    constexpr std::size_t frames = 13 * ControllerDriver::block + 11;
    constexpr unsigned pulses = 3;
    constexpr std::size_t first_frames = (2 + pulses) * static_cast<std::size_t>(ControllerDriver::block);
    const auto data = pcm16_ramp(2, frames, 0x3456);
    TempFile file, list;
    const auto* path = file.write(wave({pcm16(2, 48000), data}));
    ControllerDriver driver;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {path}));
    (void)state_is(controller, PlaybackState::stopped);
    const auto playing = settle(controller, send(controller, PlaybackCommand::play));
    expect_true("the current playlist item is playing", playing.state == PlaybackState::playing &&
        playing.current_index.has_value());
    // Keep both runs within initial priming. Only these manual callbacks advance
    // the first run; settling the selection must retire it without more pulses.
    for (unsigned index = 0; index < pulses; ++index) { driver.pulse(); }
    const auto restart = controller.select_track(*playing.current_index, true);
    expect_true("same-item Select and Play is accepted", restart != 0);
    const auto restarted = settle(controller, restart);
    expect("same-item selection starts a new run", restarted.state, PlaybackState::playing);
    expect_true("same-item restart preserves current and selected metadata",
        restarted.current_index == playing.current_index && restarted.selected_index == playing.current_index);
    expect("same-item restart preserves the WAV rate", restarted.sample_rate, 48000);
    expect("same-item restart preserves the WAV length", restarted.frame_count, frames);
    // The new run already submitted prefilled B and start's A. Its PCM below
    // must begin at frame zero, not at the retired run's submitted position.
    expect("restart counts only the new run's initial submissions", restarted.position_frames,
        2 * ControllerDriver::block);
    const auto completed = await(controller, [](const auto& state) {
        return state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    expect("same-item replay reaches the full track end", completed.position_frames, frames);
    expect("same-item replay keeps its selection receipt settled", completed.settled, restart);
    const auto closed = close(controller);
    expect_true("same-item restart and Close finish without a fault", !closed.faulted());
    expect("same-item restart keeps the activated driver", driver.activations, 1);
    expect("same-item restart keeps driver initialization", driver.inits, 1);
    expect("same-item selection starts exactly two runs", driver.starts, 2);
    expect("same-item restart retires both allocations", driver.disposals, 2);
    expect("same-item restart captures both runs", driver.captures.size(), 2);
    check_capture(driver.captures.at(0), data.payload, 0, first_frames, false);
    check_capture(driver.captures.at(1), data.payload, 0, frames, false);
    driver.check_closed();
}

void test_stop_then_play()
{
    constexpr std::size_t frames = 7 * ControllerDriver::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x2345);
    TempFile file;
    ControllerDriver driver(true);
    PlaybackController controller(selected, file.write(wave({pcm16(2, 48000), data})), driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto stopped = send(controller, PlaybackCommand::stop);
    const auto replay = send(controller, PlaybackCommand::play);
    expect("receipts retain Stop then Play order", replay, stopped + 1);
    const auto restarted = settle(controller, replay);
    expect("Stop then Play restarts", static_cast<int>(restarted.state), static_cast<int>(PlaybackState::playing));
    expect("restart reports only the new run's initial submitted blocks", restarted.position_frames,
        2 * ControllerDriver::block);
    (void)state_is(controller, PlaybackState::completed, &driver);
    (void)close(controller);
    expect("Stop then Play starts exactly twice", driver.starts, 2);
    expect("Stop then Play retires both allocations", driver.disposals, 2);
    check_capture(driver.captures.at(1), data.payload, 0, frames, true);
    driver.check_closed();
}

void test_pending_pause_stop_and_play()
{
    TempFile file;
    const auto data = pcm16_ramp(2, 7 * ControllerDriver::block + 11, 0x3456);
    ControllerDriver driver;
    PlaybackController controller(selected, file.write(wave({pcm16(2, 48000), data})), driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto pause = send(controller, PlaybackCommand::pause);
    (void)state_is(controller, PlaybackState::pausing);
    const auto stop = send(controller, PlaybackCommand::stop);
    const auto stopped = settle(controller, stop); // No callback: Stop must cancel the pause drain.
    expect("Stop settles earlier Pause as well", static_cast<int>(stopped.settled >= pause), 1);
    expect("Stop overrides a pending Pause", static_cast<int>(stopped.state), static_cast<int>(PlaybackState::stopped));
    expect("pending Pause then Stop rewinds", stopped.position_frames, 0);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    (void)state_is(controller, PlaybackState::completed, &driver);
    (void)close(controller);
    check_capture(driver.captures.at(1), data.payload, 0, data.payload.size() / 4, false);
    driver.check_closed();
}

void test_nonblocking_and_full_queue_close()
{
    TempFile file;
    ControllerDriver driver;
    CallGate initialization;
    driver.on_init = [&] { initialization.block(); };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    // Destroy this guard before controller if an assertion fails below.
    ReleaseGate release{initialization};
    initialization.wait();
    std::uint64_t last{};
    for (std::size_t i = 0; i < PlaybackController::queue_capacity; ++i) {
        last = send(controller, PlaybackCommand::play);
    }
    expect("full command queue rejects an extra request", controller.submit(PlaybackCommand::pause), 0);
    const auto accepted = controller.snapshot();
    expect("accepted commands are visible while init is blocked", accepted.accepted, last);
    expect("accepted is not completed while init is blocked", accepted.settled, 0);
    const auto shutdown = send(controller, PlaybackCommand::close);
    expect("close bypasses the full queue", shutdown, last + 1);
    expect("close immediately rejects new commands", controller.submit(PlaybackCommand::play), 0);
    initialization.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    controller.join();
    expect("close cancels queued commands", closed.settled, shutdown);
    expect("queued Play never starts after close", driver.starts, 0);
    expect("initialization close leaves no ASIO allocation", driver.creates, 0);
    driver.check_closed();
}

void test_stop_completion_follows_cleanup()
{
    TempFile file;
    ControllerDriver driver;
    CallGate teardown;
    bool blocked{};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::stop && !std::exchange(blocked, true)) {
            teardown.block();
        }
    };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    ReleaseGate release{teardown};
    (void)state_is(controller, PlaybackState::stopped);
    const auto started = send(controller, PlaybackCommand::play);
    (void)settle(controller, started);
    const auto stop = send(controller, PlaybackCommand::stop);
    teardown.wait();
    const auto pending = controller.snapshot();
    expect("Stop acceptance does not claim teardown completed", pending.settled, started);
    expect("Stop is accepted during blocked teardown", pending.accepted, stop);
    const auto replay = send(controller, PlaybackCommand::play);
    expect("later Play remains accepted during teardown", controller.snapshot().accepted, replay);
    teardown.open();
    (void)settle(controller, replay);
    (void)close(controller);
    expect("queued Play begins only after first teardown", driver.starts, 2);
    driver.check_closed();
}

void test_close_states_and_restore()
{
    for (const auto before_close : {PlaybackState::stopped, PlaybackState::playing, PlaybackState::paused}) {
        TempFile file;
        ControllerDriver driver;
        driver.sample_rate = 44100;
        driver.notify_rate_changes = true;
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        if (before_close != PlaybackState::stopped) {
            (void)settle(controller, send(controller, PlaybackCommand::play));
        }
        if (before_close == PlaybackState::paused) {
            const auto pause = send(controller, PlaybackCommand::pause);
            (void)state_is(controller, PlaybackState::pausing);
            (void)settle(controller, pause, &driver);
        }
        const auto closed = close(controller);
        expect("normal close has no exception", static_cast<int>(static_cast<bool>(closed.error)), 0);
        expect("close restores the original 44.1 kHz clock", static_cast<int>(driver.sample_rate == 44100), 1);
        expect("close releases active buffers", driver.disposals, before_close == PlaybackState::stopped ? 0 : 1);
        driver.check_closed();
    }
}

void test_failures()
{
    for (unsigned failure = 0; failure < 4; ++failure) {
        TempFile file;
        ControllerDriver driver;
        driver.fail_init = failure == 0;
        driver.fail_start = failure == 1;
        driver.fail_dispose = failure == 2;
        driver.refuse_restore = failure == 3;
        if (failure == 3) {
            driver.sample_rate = 44100;
        }
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        if (failure == 0) {
            (void)state_is(controller, PlaybackState::failed);
        } else {
            (void)state_is(controller, PlaybackState::stopped);
            const auto play = send(controller, PlaybackCommand::play);
            if (failure == 1) {
                const auto failed = state_is(controller, PlaybackState::failed);
                expect("failed Play settles its receipt", failed.settled, play);
                expect("failed start keeps its verified buffer setup",
                    static_cast<int>(failed.diagnostics.segments[0].has_value()), 1);
                expect("failed start keeps the actual buffer width",
                    failed.diagnostics.segments[0]->buffer_frames, ControllerDriver::block);
                expect("failed start keeps the reported latency",
                    failed.diagnostics.segments[0]->output_latency_frames, 73);
            } else {
                (void)settle(controller, play);
                if (failure == 2) {
                    const auto stop = send(controller, PlaybackCommand::stop);
                    const auto failed = state_is(controller, PlaybackState::failed);
                    expect("failed Stop settles its receipt", failed.settled, stop);
                }
            }
        }
        if (failure != 3) {
            expect("faulted controller rejects new Play", controller.submit(PlaybackCommand::play), 0);
        }
        const auto closed = close(controller);
        expect("close retains initialization/playback/restore error", static_cast<int>(static_cast<bool>(closed.error)), 1);
        expect("Close keeps verified diagnostics, including failed start",
            static_cast<int>(closed.diagnostics.segments[0].has_value()), failure == 0 ? 0 : 1);
        driver.check_closed();
    }
}

void test_read_error_cause()
{
    // The first refill after priming hits a locked frame. The request ends with
    // read_error, and the failed state carries the producer's own ReadFile error.
    TempFile file;
    constexpr std::uint32_t rate = 48000;
    const auto path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, 3 * rate + 11, 1)}));
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        wavplay::core::throw_last_error("open read-error lock");
    }
    const wavplay::core::UniqueHandle lock(handle);
    // 100 ms blocks: the two-second ring holds exactly the frames before the
    // lock, and the first refill comes after half of it has been consumed.
    const auto offset = wavplay::media::WavReader(path).info().data_offset + std::uint64_t{2} * rate * 4;
    if (!LockFile(lock.get(), static_cast<DWORD>(offset), 0, 4, 0)) {
        wavplay::core::throw_last_error("lock the first refill frame");
    }
    ControllerDriver driver(false, rate / 10);
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto failed = state_is(controller, PlaybackState::failed, &driver);
    expect("a failed refill is a read error", static_cast<int>(failed.result), static_cast<int>(PlaybackResult::read_error));
    expect_true("the read error keeps its cause", static_cast<bool>(failed.error));
    try {
        std::rethrow_exception(failed.error);
    } catch (const std::system_error& error) {
        expect_true("the cause is ReadFile's lock violation",
            error.code() == std::error_code(ERROR_LOCK_VIOLATION, std::system_category()) &&
            std::string_view(error.what()).starts_with("ReadFile"));
    }
    expect("a read error never restarts", driver.starts, 1);
    const auto closed = close(controller);
    expect("Close keeps the read error", static_cast<int>(closed.result), static_cast<int>(PlaybackResult::read_error));
    expect_true("Close keeps the read error's cause", closed.error == failed.error);
    driver.check_closed();
}

void test_cleanup_failure_keeps_original()
{
    // start fails with an error of its own type, then disposeBuffers also fails
    // during cleanup. The disposal error is reported with the original nested.
    TempFile file;
    ControllerDriver driver;
    driver.fail_dispose = true;
    driver.on_call = [](MockAsio::Call call) {
        if (call == MockAsio::Call::start) {
            throw std::system_error(ERROR_GEN_FAILURE, std::system_category(), "Injected start failure");
        }
    };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)send(controller, PlaybackCommand::play);
    const auto failed = state_is(controller, PlaybackState::failed);
    expect("failed cleanup is a driver error", static_cast<int>(failed.result), static_cast<int>(PlaybackResult::driver_error));
    expect_true("failed cleanup keeps an exception", static_cast<bool>(failed.error));
    bool nested{};
    try {
        std::rethrow_exception(failed.error);
    } catch (const std::runtime_error& error) {
        expect_true("the outer error names the cleanup disposal and its code",
            std::string_view(error.what()) == "ASIO disposeBuffers failed during cleanup: ASE_HWMalfunction (-999)");
        try {
            std::rethrow_if_nested(error);
        } catch (const std::system_error& original) {
            nested = original.code() == std::error_code(ERROR_GEN_FAILURE, std::system_category()) &&
                std::string_view(original.what()).starts_with("Injected start failure");
        }
    }
    expect_true("the original start failure survives with its type", nested);
    expect("cleanup attempts one disposal", driver.disposals, 1);
    const auto closed = close(controller);
    expect_true("Close keeps the nested report", closed.error == failed.error);
    driver.check_closed();
}

void test_notification_during_stop()
{
    for (const long selector : {kAsioResetRequest, kAsioBufferSizeChange, kAsioOverload}) {
        for (const auto boundary : {MockAsio::Call::stop, MockAsio::Call::dispose}) {
            TempFile file;
            ControllerDriver driver;
            driver.on_call = [&](MockAsio::Call call) {
                if (call == boundary) {
                    driver.message_notification(selector, 0, nullptr, nullptr);
                }
            };
            PlaybackController controller(selected,
                file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
            (void)state_is(controller, PlaybackState::stopped);
            (void)settle(controller, send(controller, PlaybackCommand::play));
            const auto stop = send(controller, PlaybackCommand::stop);
            const bool recoverable = selector != kAsioOverload;
            const auto settled = settle(controller, stop);
            const auto ended = state_is(controller, recoverable ? PlaybackState::stopped : PlaybackState::failed);
            expect("notification settles the Stop receipt", settled.settled, stop);
            expect("notification distinguishes recovery from a fault", static_cast<int>(ended.result),
                static_cast<int>(recoverable ? PlaybackResult::stopped : PlaybackResult::underrun));
            expect("only recoverable notifications reinitialize", driver.inits, recoverable ? 2 : 1);
            if (!recoverable) {
                expect("notification failure rejects another Play", controller.submit(PlaybackCommand::play), 0);
            }
            (void)close(controller);
            expect("notification never auto-restarts audio", driver.starts, 1);
            driver.check_closed();
        }
    }
}

void test_controls_during_startup()
{
    for (const auto blocked_call : {MockAsio::Call::create, MockAsio::Call::start}) {
        for (const auto command : {PlaybackCommand::pause, PlaybackCommand::stop, PlaybackCommand::close}) {
            TempFile file;
            ControllerDriver driver;
            CallGate startup;
            bool blocked{};
            driver.on_call = [&](MockAsio::Call call) {
                if (call == blocked_call && !std::exchange(blocked, true)) {
                    startup.block();
                }
            };
            PlaybackController controller(selected,
                file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
            ReleaseGate release{startup};
            (void)state_is(controller, PlaybackState::stopped);
            const auto play = send(controller, PlaybackCommand::play);
            startup.wait();
            const auto pending = controller.snapshot();
            expect("blocked startup has not settled Play", pending.settled, play - 1);
            const auto interrupt = send(controller, command);
            startup.open();
            if (command == PlaybackCommand::close) {
                const auto closed = state_is(controller, PlaybackState::closed);
                expect("startup Close settles pending Play", closed.settled, interrupt);
                controller.join();
            } else {
                const auto stopped = settle(controller, interrupt, command == PlaybackCommand::pause ? &driver : nullptr);
                expect("startup control settles in its requested idle state", static_cast<int>(stopped.state),
                    static_cast<int>(command == PlaybackCommand::pause ? PlaybackState::paused : PlaybackState::stopped));
                if (command == PlaybackCommand::stop) {
                    expect("startup Stop rewinds", stopped.position_frames, 0);
                }
                (void)close(controller);
            }
            const auto expected_starts = blocked_call == MockAsio::Call::create && command != PlaybackCommand::pause ? 0 : 1;
            expect("startup Stop or Close avoids an unentered start", driver.starts, expected_starts);
            expect("startup interruption disposes its buffers", driver.disposals, driver.creates);
            driver.check_closed();
        }
    }
}

void test_close_during_teardown()
{
    for (const auto blocked_call : {MockAsio::Call::stop, MockAsio::Call::dispose}) {
        TempFile file;
        ControllerDriver driver;
        CallGate teardown;
        bool blocked{};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == blocked_call && !std::exchange(blocked, true)) {
                teardown.block();
            }
        };
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        ReleaseGate release{teardown};
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        (void)send(controller, PlaybackCommand::stop);
        teardown.wait();
        (void)send(controller, PlaybackCommand::play);
        const auto close_receipt = send(controller, PlaybackCommand::close);
        teardown.open();
        const auto closed = state_is(controller, PlaybackState::closed);
        controller.join();
        expect("Close cancels replay queued during teardown", closed.settled, close_receipt);
        expect("Close prevents another start after teardown", driver.starts, 1);
        expect("Close disposes the interrupted run once", driver.disposals, 1);
        driver.check_closed();
    }
}

void enter_idle_state(PlaybackController& controller, ControllerDriver& driver, PlaybackState state)
{
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    if (state == PlaybackState::completed) {
        (void)state_is(controller, state, &driver);
    } else {
        const auto receipt = send(controller, state == PlaybackState::paused ? PlaybackCommand::pause : PlaybackCommand::stop);
        (void)settle(controller, receipt, &driver);
        (void)state_is(controller, state);
    }
}

void test_idle_driver_notifications()
{
    for (const auto idle : {PlaybackState::stopped, PlaybackState::paused, PlaybackState::completed}) {
        for (const long selector : {kAsioResetRequest, kAsioBufferSizeChange, kAsioOverload}) {
            TempFile file;
            ControllerDriver driver;
            PlaybackController controller(selected,
                file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
            enter_idle_state(controller, driver, idle);
            driver.message_notification(selector, 0, nullptr, nullptr);
            const bool recoverable = selector != kAsioOverload;
            const auto ended = await(controller, [recoverable](const auto& state) {
                return recoverable ? state.state == PlaybackState::stopped && !state.diagnostics.segments[0] :
                    state.state == PlaybackState::failed;
            });
            expect("idle notification distinguishes recovery from a fault", static_cast<int>(ended.result),
                static_cast<int>(recoverable ? PlaybackResult::stopped : PlaybackResult::underrun));
            expect("idle notification opens only for recovery", driver.inits, recoverable ? 2 : 1);
            if (recoverable) {
                expect("idle recovery discards the retained position", ended.position_frames, 0);
                driver.message_notification(selector, 0, nullptr, nullptr);
                (void)await(controller, [revision = ended.revision](const auto& state) {
                    return state.state == PlaybackState::stopped && state.revision > revision;
                });
                expect("recovered idle session continues watching notifications", driver.inits, 3);
            } else {
                expect("idle fault rejects replay", controller.submit(PlaybackCommand::play), 0);
            }
            const auto closed = close(controller);
            expect("Close preserves idle notification outcome", static_cast<int>(closed.result), static_cast<int>(ended.result));
            expect("idle fault does not restart ASIO", driver.starts, 1);
            driver.check_closed();
        }
    }
}

void test_idle_fault_reaching_play()
{
    // The device reports the fault after the idle loop last polled for one and
    // before the popped Play reaches the player: the STA's publication of the
    // starting state lies exactly in that window.
    struct FaultWhenStarting {
        ControllerDriver& driver;
        long selector;
        std::atomic<bool> armed{};
        static void changed(void* context, std::uint64_t) noexcept
        {
            auto& self = *static_cast<FaultWhenStarting*>(context);
            if (GetCurrentThreadId() == self.driver.owner_thread && self.armed.exchange(false)) {
                (void)self.driver.message_notification(self.selector, 0, nullptr, nullptr);
            }
        }
    };
    for (const auto idle : {PlaybackState::stopped, PlaybackState::paused, PlaybackState::completed}) {
        for (const long selector : {kAsioResetRequest, kAsioBufferSizeChange, kAsioOverload}) {
            TempFile file;
            ControllerDriver driver;
            FaultWhenStarting hook{driver, selector};
            PlaybackController controller(selected, file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})),
                driver.activation(), {.start_silence = 0ms}, {FaultWhenStarting::changed, &hook});
            enter_idle_state(controller, driver, idle);
            const auto paused_at = controller.snapshot().position_frames;
            hook.armed = true;
            const auto play = send(controller, PlaybackCommand::play);
            const bool recoverable = selector != kAsioOverload;
            (void)settle(controller, play);
            const auto failed = state_is(controller, recoverable ? PlaybackState::stopped : PlaybackState::failed);
            expect("fault was injected before the Play reached the player", static_cast<int>(hook.armed.load()), 0);
            expect("Play that met an idle fault settles its receipt", failed.settled, play);
            expect("Play publishes the idle fault's own result", static_cast<int>(failed.result),
                static_cast<int>(recoverable ? PlaybackResult::stopped : PlaybackResult::underrun));
            expect("Play publishes the idle fault without a generic refusal", static_cast<int>(static_cast<bool>(failed.error)), 0);
            // Play after EOF rewinds before it reaches the player; otherwise the idle position stays.
            expect("idle fault keeps the idle position", failed.position_frames,
                !recoverable && idle == PlaybackState::paused ? paused_at : 0);
            expect("pre-Play notification reinitializes only when recoverable", driver.inits, recoverable ? 2 : 1);
            if (!recoverable) {
                expect("idle fault rejects another Play", controller.submit(PlaybackCommand::play), 0);
            }
            const auto closed = close(controller);
            expect("Close preserves the fault Play reported", static_cast<int>(closed.result), static_cast<int>(failed.result));
            expect("Close adds no exception to the reported fault", static_cast<int>(static_cast<bool>(closed.error)), 0);
            expect("idle fault before Play does not restart ASIO", driver.starts, 1);
            driver.check_closed();
        }
    }
}

void test_active_reinitialization(long selector)
{
    constexpr std::size_t frames = 1003;
    const auto data = pcm16_ramp(2, frames, 0x2345);
    TempFile file;
    ControllerDriver driver;
    driver.sample_rate = 44100;
    driver.notify_rate_changes = true;
    CallGate teardown, initialization;
    bool blocked{};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::stop && !std::exchange(blocked, true)) { teardown.block(); }
    };
    driver.on_release = [&] {
        if (driver.releases == 0) {
            // Notifications from the retired instance coalesce with its request.
            driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
            driver.message_notification(kAsioBufferSizeChange, 96, nullptr, nullptr);
        }
    };
    driver.on_init = [&] {
        if (driver.inits == 2) {
            expect("recovery releases old buffers before init", driver.disposals, 1);
            expect("recovery releases the old instance before init", driver.releases, 1);
            expect_true("recovery restores the original clock before init", driver.sample_rate == 44100);
            driver.buffer_size = {96, 96, 96, 0};
            driver.next_width = 96;
            driver.left_type = driver.right_type = driver.next_left_type = driver.next_right_type = ASIOSTInt24LSB;
            initialization.block();
        }
    };
    const auto path = file.write(wave({pcm16(2, 48000), data}));
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
    ReleaseGate release_teardown{teardown}, release_initialization{initialization};
    (void)state_is(controller, PlaybackState::stopped);
    const auto owner = driver.owner_window;
    const auto thread = driver.owner_thread;
    (void)settle(controller, send(controller, PlaybackCommand::play));
    driver.message_notification(selector, 96, nullptr, nullptr);
    teardown.wait();
    const auto pending = send(controller, PlaybackCommand::play);
    teardown.open();
    initialization.wait();
    expect("blocked recovery exposes its state", static_cast<int>(controller.snapshot().state),
        static_cast<int>(PlaybackState::reinitializing));
    for (const auto command : {PlaybackCommand::play, PlaybackCommand::pause, PlaybackCommand::stop}) {
        expect("transport is rejected until reinitialization completes", controller.submit(command), 0);
    }
    initialization.open();
    const auto recovered = state_is(controller, PlaybackState::stopped);
    expect("recovery settles and cancels pending Play", recovered.settled, pending);
    expect("recovery rewinds the track", recovered.position_frames, 0);
    expect("recovery keeps track metadata", recovered.frame_count, frames);
    expect_true("recovery clears stale allocation diagnostics", !recovered.diagnostics.segments[0]);
    expect_true("recovery clears stale buffer capabilities", !recovered.diagnostics.buffer_capabilities[0]);
    expect_true("successful recovery has no retained fault", !recovered.faulted());
    expect("recovery does not allocate buffers", driver.creates, 1);
    expect("recovery does not restart playback", driver.starts, 1);
    expect("recovery inquires overload support on the fresh driver", driver.overload_queries, 2);
    expect_true("recovery retains the owner window", driver.owner_window == owner);
    expect("recovery retains the owning STA", driver.owner_thread, thread);
    const auto deleted = DeleteFileW(path);
    const auto deletion_error = GetLastError();
    expect("selected WAV remains protected through recovery", deleted, FALSE);
    expect("WAV deletion fails through sharing protection", deletion_error, ERROR_SHARING_VIOLATION);
    const auto playing = settle(controller, send(controller, PlaybackCommand::play));
    expect("explicit Play queries the fresh buffer width", playing.diagnostics.segments[0]->buffer_frames, 96);
    expect("explicit Play publishes the fresh capabilities", playing.diagnostics.buffer_capabilities[0].value().preferred, 96);
    expect("explicit Play queries the fresh output type", playing.diagnostics.segments[0]->output_types[0], ASIOSTInt24LSB);
    (void)state_is(controller, PlaybackState::completed, &driver);
    (void)close(controller);
    expect("only a new user Play starts the second run", driver.starts, 2);
    expect("recovery opens exactly one fresh instance", driver.inits, 2);
    check_capture(driver.captures.at(1), data.payload, 0, frames, true, 96);
    driver.check_closed();
}

void test_reinitialization_fault_priority()
{
    for (unsigned fault = 0; fault < 4; ++fault) {
        TempFile file;
        ControllerDriver driver;
        driver.fail_stop = fault == 2;
        driver.fail_dispose = fault == 3;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::stop) {
                if (fault == 0) { driver.message_notification(kAsioOverload, 0, nullptr, nullptr); }
                if (fault == 1) { driver.rate_notification(0); }
            }
        };
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        const auto failed = state_is(controller, PlaybackState::failed);
        expect("teardown fault wins over recovery", static_cast<int>(failed.result),
            static_cast<int>(fault == 0 ? PlaybackResult::underrun : PlaybackResult::driver_error));
        expect("fault prevents reinitialization", driver.inits, 1);
        expect("fault prevents replay", controller.submit(PlaybackCommand::play), 0);
        (void)close(controller);
        expect("fault never restarts ASIO", driver.starts, 1);
        driver.check_closed();
    }
}

void test_reinitialization_revalidates_manual_width()
{
    TempFile file;
    ControllerDriver driver;
    driver.buffer_size = {48, 192, 48, 48};
    driver.on_init = [&] {
        if (driver.inits == 2) { driver.buffer_size = {48, 48, 48, 0}; }
    };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(),
        {.start_silence = 0ms, .buffer_frames = 96});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    driver.message_notification(kAsioBufferSizeChange, 48, nullptr, nullptr);
    const auto recovered = state_is(controller, PlaybackState::stopped);
    expect("recovery retains the requested width", recovered.requested_buffer_frames, 96);
    expect_true("recovery does not retain capabilities from the old driver", !recovered.diagnostics.buffer_capabilities[0]);
    (void)send(controller, PlaybackCommand::play);
    const auto failed = state_is(controller, PlaybackState::failed);
    expect_true("fresh range refusal is visible to the user", static_cast<bool>(failed.error));
    expect("fresh range refusal publishes the new maximum", failed.diagnostics.buffer_capabilities[0].value().maximum, 48);
    expect_true("fresh range refusal does not retain the old allocation", !failed.diagnostics.segments[0]);
    expect("invalid manual width never falls back to preferred", driver.creates, 1);
    expect("invalid manual width never starts again", driver.starts, 1);
    (void)close(controller);
    driver.check_closed();
}

void test_reinitialization_failure()
{
    enum class Failure { restore, init, reset, overload, clock_notification, release_overload, release_clock,
        null_activation, activation_exception, buffer_change, invalid_clock, no_clock, clock_error, inquiry_exception };
    for (const auto failure : {Failure::restore, Failure::init, Failure::reset, Failure::overload,
        Failure::clock_notification, Failure::release_overload, Failure::release_clock, Failure::null_activation,
        Failure::activation_exception, Failure::buffer_change, Failure::invalid_clock, Failure::no_clock,
        Failure::clock_error, Failure::inquiry_exception}) {
        TempFile file;
        ControllerDriver driver;
        unsigned activation_attempts{}, fresh_rate_queries{};
        const auto activate = driver.activation();
        const auto factory = [&](const CLSID& clsid) -> IASIO* {
            if (++activation_attempts == 2) {
                expect("recovery releases the old reference before activation", driver.references, 0);
                expect_true("recovery restores the old clock before activation", driver.sample_rate == 44100);
                if (failure == Failure::null_activation) { return nullptr; }
                if (failure == Failure::activation_exception) { throw std::runtime_error("mock reactivation failed"); }
            }
            return activate(clsid);
        };
        driver.sample_rate = 44100;
        driver.notify_rate_changes = true;
        driver.refuse_restore = failure == Failure::restore;
        driver.on_release = [&] {
            if (driver.releases == 0) {
                if (failure == Failure::release_overload) { driver.message_notification(kAsioOverload, 0, nullptr, nullptr); }
                if (failure == Failure::release_clock) { driver.rate_notification(0); }
            }
        };
        driver.on_init = [&] {
            if (driver.inits == 2) {
                driver.fail_init = failure == Failure::init;
                if (failure == Failure::reset) { driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr); }
                if (failure == Failure::buffer_change) { driver.message_notification(kAsioBufferSizeChange, 96, nullptr, nullptr); }
                if (failure == Failure::overload) { driver.message_notification(kAsioOverload, 0, nullptr, nullptr); }
                if (failure == Failure::clock_notification) { driver.rate_notification(0); }
                if (failure == Failure::invalid_clock) { driver.sample_rate = 0; }
                if (failure == Failure::no_clock) { driver.sample_rate_result = ASE_NoClock; }
                if (failure == Failure::clock_error) { driver.sample_rate_result = ASE_HWMalfunction; }
            }
        };
        driver.on_future = [&] {
            if (driver.inits == 2 && failure == Failure::inquiry_exception) {
                throw std::runtime_error("mock overload inquiry failed");
            }
        };
        driver.on_call = [&](MockAsio::Call call) {
            if (driver.inits == 2 && call == MockAsio::Call::rate) { ++fresh_rate_queries; }
        };
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), factory, {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        driver.message_notification(kAsioBufferSizeChange, 96, nullptr, nullptr);
        const auto failed = state_is(controller, PlaybackState::failed);
        expect_true("reinitialization failure remains observable", failed.faulted());
        expect_true("failed reinitialization cannot advertise an available device", !failed.device_available);
        expect("failed recovery refuses replay", controller.submit(PlaybackCommand::play), 0);
        const bool retirement_failed = failure == Failure::restore || failure == Failure::release_overload ||
            failure == Failure::release_clock;
        const bool activation_failed = failure == Failure::null_activation || failure == Failure::activation_exception;
        expect("recovery attempts activation only after clean retirement", activation_attempts, retirement_failed ? 1 : 2);
        expect("reinitialization never loops or erases a release fault", driver.inits,
            retirement_failed || activation_failed ? 1 : 2);
        const bool exception = failure == Failure::restore || failure == Failure::init || activation_failed ||
            failure == Failure::clock_error || failure == Failure::inquiry_exception;
        expect("recovery preserves exceptions separately from notifications", static_cast<int>(static_cast<bool>(failed.error)),
            static_cast<int>(exception));
        const bool overload = failure == Failure::overload || failure == Failure::release_overload;
        expect("recovery preserves the specific failure result", static_cast<int>(failed.result),
            static_cast<int>(overload ? PlaybackResult::underrun : PlaybackResult::driver_error));
        const bool clock_query_failed = failure == Failure::invalid_clock || failure == Failure::no_clock ||
            failure == Failure::clock_error;
        expect("only clock inquiry failures reach the fresh rate query", fresh_rate_queries, clock_query_failed ? 1 : 0);
        if (failure == Failure::inquiry_exception) {
            expect("overload inquiry exception is injected only after reinitialization", driver.overload_queries, 2);
        }
        (void)close(controller);
        expect("failed recovery never starts audio", driver.starts, 1);
        expect("failed recovery leaves no ASIO buffers", driver.disposals, driver.creates);
        driver.check_closed();
    }
}

void test_close_during_reinitialization()
{
    enum class Boundary { release, init, overload_query };
    for (const auto phase : {Boundary::release, Boundary::init, Boundary::overload_query}) {
        TempFile file;
        ControllerDriver driver;
        CallGate boundary;
        driver.on_release = [&] {
            if (phase == Boundary::release && driver.releases == 0) { boundary.block(); }
        };
        driver.on_init = [&] {
            if (phase == Boundary::init && driver.inits == 2) { boundary.block(); }
        };
        driver.on_future = [&] {
            if (phase == Boundary::overload_query && driver.overload_queries == 2) { boundary.block(); }
        };
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        ReleaseGate release{boundary};
        enter_idle_state(controller, driver, PlaybackState::paused);
        driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        boundary.wait();
        const auto rate_queries = std::count(driver.calls.begin(), driver.calls.end(), MockAsio::Call::rate);
        const auto receipt = send(controller, PlaybackCommand::close);
        boundary.open();
        const auto closed = state_is(controller, PlaybackState::closed);
        controller.join();
        expect("Close during recovery settles all commands", closed.settled, receipt);
        expect("Close prevents any not-yet-entered init", driver.inits, phase == Boundary::release ? 1 : 2);
        if (phase == Boundary::overload_query) {
            expect("Close during overload inquiry avoids the following clock query",
                std::count(driver.calls.begin(), driver.calls.end(), MockAsio::Call::rate), rate_queries);
        }
        expect("Close during recovery never restarts playback", driver.starts, 1);
        driver.check_closed();
    }
}

void test_fault_during_driver_release()
{
    for (const auto idle : {PlaybackState::stopped, PlaybackState::paused, PlaybackState::completed}) {
        for (unsigned fault = 0; fault < 4; ++fault) {
            TempFile file;
            ControllerDriver driver;
            driver.sample_rate = 44100;
            driver.notify_rate_changes = true;
            driver.on_release = [&] {
                if (fault == 0) {
                    driver.rate_notification(driver.sample_rate); // A late confirmation of the restored clock.
                } else if (fault == 3) {
                    driver.rate_notification(0); // Clock loss remains a fault during restoration.
                } else {
                    driver.message_notification(fault == 1 ? kAsioResetRequest : kAsioOverload, 0, nullptr, nullptr);
                }
            };
            PlaybackController controller(selected,
                file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
            enter_idle_state(controller, driver, idle);
            const auto closed = close(controller);
            const auto healthy = idle == PlaybackState::paused ? wavplay::playback::PlaybackResult::paused :
                idle == PlaybackState::stopped ? wavplay::playback::PlaybackResult::stopped : wavplay::playback::PlaybackResult::completed;
            const auto expected = fault == 0 ? healthy : fault == 1 ? PlaybackResult::reinitialize_required :
                fault == 2 ? PlaybackResult::underrun : PlaybackResult::driver_error;
            expect("Close distinguishes restored clock from a Release fault", static_cast<int>(closed.result), static_cast<int>(expected));
            expect("Release notifications do not prevent clock restoration", static_cast<int>(driver.sample_rate == 44100), 1);
            driver.check_closed();
        }
    }
}

void test_close_keeps_fault_after_cancelled_reinitialization()
{
    for (const bool clock_loss : {false, true}) {
        TempFile file;
        ControllerDriver driver;
        driver.sample_rate = 44100;
        driver.notify_rate_changes = true;
        CallGate teardown;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::stop) { teardown.block(); }
        };
        driver.on_release = [&] {
            if (clock_loss) {
                driver.rate_notification(0);
            } else {
                driver.message_notification(kAsioOverload, 0, nullptr, nullptr);
            }
        };
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        ReleaseGate release{teardown};
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        teardown.wait();
        // play() will cache and report reinitialize_required. Close is already
        // accepted when it returns, so only final Release may update that cache.
        const auto receipt = send(controller, PlaybackCommand::close);
        teardown.open();
        const auto closed = state_is(controller, PlaybackState::closed);
        controller.join();
        expect("final Release fault replaces a reported recovery request", static_cast<int>(closed.result),
            static_cast<int>(clock_loss ? PlaybackResult::driver_error : PlaybackResult::underrun));
        expect_true("final notification preserves its own result without an exception", !closed.error);
        expect("Close settles the cancelled recovery", closed.settled, receipt);
        expect("Close never reopens the cancelled recovery", driver.inits, 1);
        expect("Close never starts the cancelled recovery", driver.starts, 1);
        expect_true("final notification does not skip clock restoration", driver.sample_rate == 44100);
        driver.check_closed();
    }
}

void test_window_shutdown()
{
    for (const bool quit : {false, true}) {
        for (const auto state : {PlaybackState::stopped, PlaybackState::playing, PlaybackState::paused}) {
            TempFile file;
            ControllerDriver driver;
            PlaybackController controller(selected,
                file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
            if (state == PlaybackState::paused) {
                enter_idle_state(controller, driver, state);
            } else {
                (void)state_is(controller, PlaybackState::stopped);
                if (state == PlaybackState::playing) {
                    (void)settle(controller, send(controller, PlaybackCommand::play));
                }
            }
            const auto before = controller.snapshot();
            const auto posted = quit ? PostThreadMessageW(driver.owner_thread, WM_QUIT, 0, 0) :
                PostMessageW(driver.owner_window, WM_CLOSE, 0, 0);
            expect("owner STA accepts the shutdown message", posted, TRUE);
            const auto closed = state_is(controller, PlaybackState::closed); // No more callbacks.
            controller.join();
            expect("Win32 shutdown submits a Close receipt", closed.accepted, before.accepted + 1);
            expect("Win32 shutdown settles every receipt", closed.settled, closed.accepted);
            // WM_QUIT during playback ends the request like Stop, not as a driver fault.
            expect(quit ? "WM_QUIT does not report a driver fault" : "WM_CLOSE does not report a callback watchdog fault",
                static_cast<int>(closed.result),
                static_cast<int>(state == PlaybackState::paused ? wavplay::playback::PlaybackResult::paused : wavplay::playback::PlaybackResult::stopped));
            driver.check_closed();
        }
    }
}

void test_rate_refusal_result()
{
    // A device that cannot run the WAV's rate reports the refusal, not a driver
    // fault, and Close keeps that result.
    TempFile file;
    ControllerDriver driver;
    driver.can_rate_result = ASE_NoClock;
    PlaybackController controller(selected, file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 3 * ControllerDriver::block)})),
        driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)send(controller, PlaybackCommand::play);
    const auto failed = state_is(controller, PlaybackState::failed);
    expect("refused rate result", static_cast<int>(failed.result),
        static_cast<int>(wavplay::playback::PlaybackResult::first_rate_rejected));
    const auto closed = close(controller);
    expect("refused rate result after Close", static_cast<int>(closed.result),
        static_cast<int>(wavplay::playback::PlaybackResult::first_rate_rejected));
    expect("no rate set after refusal", driver.set_rates.size(), 0);
    expect("no buffers after refusal", driver.creates, 0);
}
void expect_same_snapshot(const PlaybackSnapshot& copied, const PlaybackSnapshot& value)
{
    expect("refreshed state", static_cast<int>(copied.state), static_cast<int>(value.state));
    expect("refreshed revision", copied.revision, value.revision);
    expect("refreshed non-progress revision", copied.state_revision, value.state_revision);
    expect("refreshed accepted receipts", copied.accepted, value.accepted);
    expect("refreshed settled receipts", copied.settled, value.settled);
    expect("refreshed position", copied.position_frames, value.position_frames);
    expect("refreshed frame count", copied.frame_count, value.frame_count);
    expect("refreshed sample rate", copied.sample_rate, value.sample_rate);
    expect_true("refreshed list is the shared list", copied.playlist == value.playlist);
    expect("refreshed excluded count", copied.excluded_count, value.excluded_count);
    expect_true("refreshed selection and current item",
        copied.selected_index == value.selected_index && copied.current_index == value.current_index);
    expect_true("refreshed track error", copied.track_error == value.track_error);
    expect_true("refreshed settings and device", copied.settings_loaded == value.settings_loaded &&
        IsEqualCLSID(copied.selected_device, value.selected_device) && copied.device_name == value.device_name &&
        copied.output_pair == value.output_pair && copied.device_available == value.device_available);
    expect_true("refreshed window", copied.window.has_value() == value.window.has_value() && (!value.window ||
        (copied.window->x == value.window->x && copied.window->y == value.window->y &&
            copied.window->width == value.window->width && copied.window->height == value.window->height &&
            copied.window->maximized == value.window->maximized)));
    expect("refreshed requested width", copied.requested_buffer_frames, value.requested_buffer_frames);
    expect("refreshed result", static_cast<int>(copied.result), static_cast<int>(value.result));
    const auto& copied_diagnostics = copied.diagnostics;
    const auto& diagnostics = value.diagnostics;
    expect("refreshed current segment", copied_diagnostics.current_segment, diagnostics.current_segment);
    expect("refreshed capability records", copied_diagnostics.buffer_capabilities.size(), diagnostics.buffer_capabilities.size());
    for (std::size_t index = 0; index < diagnostics.buffer_capabilities.size(); ++index) {
        const auto& left = copied_diagnostics.buffer_capabilities[index];
        const auto& right = diagnostics.buffer_capabilities[index];
        expect_true("refreshed capability record", left.has_value() == right.has_value() && (!right ||
            (left->sample_rate == right->sample_rate && left->minimum == right->minimum && left->maximum == right->maximum &&
                left->preferred == right->preferred && left->granularity == right->granularity)));
    }
    expect("refreshed allocation records", copied_diagnostics.segments.size(), diagnostics.segments.size());
    for (std::size_t index = 0; index < diagnostics.segments.size(); ++index) {
        const auto& left = copied_diagnostics.segments[index];
        const auto& right = diagnostics.segments[index];
        expect_true("refreshed allocation record", left.has_value() == right.has_value() && (!right ||
            (left->sample_rate == right->sample_rate && left->output_types == right->output_types &&
                left->buffer_frames == right->buffer_frames && left->output_latency_frames == right->output_latency_frames &&
                left->start_silence_blocks == right->start_silence_blocks && left->ring_pages_locked == right->ring_pages_locked &&
                left->ring_metadata_pages_locked == right->ring_metadata_pages_locked &&
                left->renderer_pages_locked == right->renderer_pages_locked &&
                left->request_pages_locked == right->request_pages_locked &&
                left->image_pages_locked == right->image_pages_locked)));
    }
    expect_true("refreshed error", copied.error == value.error);
}

void expect_command_state(const PlaybackController& controller, const PlaybackSnapshot& expected)
{
    const auto actual = controller.command_state();
    expect("command query reports the current state", actual.state, expected.state);
    expect("command query reports accepted receipts", actual.accepted, expected.accepted);
    expect("command query reports settled receipts", actual.settled, expected.settled);
    expect("command query reports the published position", actual.position_frames, expected.position_frames);
    expect_true("command query distinguishes empty and nonempty playlists",
        actual.has_tracks == (expected.playlist && !expected.playlist->empty()));
    expect_true("command query preserves recoverable refusal versus fault", actual.faulted == expected.faulted());
}

void test_snapshot_refresh_into_existing_object()
{
    // Refreshing a reused snapshot gives exactly the by-value snapshot, also
    // when the reused object held larger stale strings, records and errors.
    TempFile first, second, list, configuration;
    const auto ap = first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3 * ControllerDriver::block, 1)}));
    const auto bp = second.write(wave({pcm16(2, 48000), pcm16_ramp(2, 2 * ControllerDriver::block, 301)}));
    const std::string text = "[General]\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\n"
        "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nName=A device label longer than a short string\n"
        "BufferSize=48\nStartSilence=0\n[Window]\nX=10\nY=20\nWidth=640\nHeight=480\n";
    ControllerDriver driver;
    CallGate releasing;
    driver.on_release = [&] { releasing.block(); };
    PlaybackController controller(CLSID{}, L"", driver.activation(), {}, {},
        configuration.write(Bytes(text.begin(), text.end())), write_playlist(list, {ap, bp}));
    ReleaseGate release{releasing};
    (void)state_is(controller, PlaybackState::stopped);
    expect_true("completion query does not confuse stopped with closed", !controller.closed());
    (void)send(controller, PlaybackCommand::play);
    (void)await(controller, [](const auto& state) {
        return state.state == PlaybackState::stopped && state.result == PlaybackResult::completed;
    }, &driver);
    first.write(Bytes{'b', 'a', 'd'}); // The finished run no longer holds the first item.
    const auto refused = settle(controller, controller.select_track(0));
    expect_true("the compared snapshot carries a refusal, records, a long name and a window",
        refused.track_error && refused.diagnostics.segments[0] && refused.device_name.size() > 15 && refused.window);
    expect_command_state(controller, refused);

    PlaybackSnapshot reused;
    reused.revision = refused.revision + 100;
    reused.state_revision = refused.state_revision + 100;
    reused.device_name.assign(100, L'x');
    reused.diagnostics.segments.resize(4, wavplay::playback::BufferSetup{96000.0, {18, 18}, 512, 375, 9, true, true, true, true, true});
    reused.diagnostics.buffer_capabilities.resize(4);
    reused.error = std::make_exception_ptr(std::runtime_error("stale"));
    controller.snapshot(reused);
    expect_same_snapshot(reused, controller.snapshot());

    // The steady-state use: refresh the same object after the next revision.
    const auto selected_second = settle(controller, controller.select_track(1));
    expect_true("a later selection changes the snapshot", !selected_second.track_error && selected_second.current_index == 1);
    controller.snapshot(reused);
    expect_same_snapshot(reused, controller.snapshot());
    const auto receipt = send(controller, PlaybackCommand::close);
    releasing.wait();
    expect_true("completion query remains false until driver release returns", !controller.closed());
    releasing.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    expect("closed settles its receipt after release", closed.settled, receipt);
    expect_true("completion query observes the same final closed publication", controller.closed());
    controller.join();
    driver.check_closed();
}

void test_command_state_during_pending_operations()
{
    TempFile file;
    constexpr std::uint32_t rate = 48000;
    ControllerDriver driver(false, 480);
    driver.preroll_switches = 0;
    CallGate starting, stopping, releasing;
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start) { starting.block(); }
        if (call == MockAsio::Call::stop) { stopping.block(); }
    };
    driver.on_release = [&] { releasing.block(); };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, rate), pcm16_ramp(2, 3 * rate)})), driver.activation(), {.start_silence = 0ms});
    ReleaseGate release_start{starting}, release_stop{stopping}, release_driver{releasing};
    const auto idle = state_is(controller, PlaybackState::stopped);
    expect_command_state(controller, idle);

    const auto play = send(controller, PlaybackCommand::play);
    starting.wait();
    const auto pending_play = controller.snapshot();
    expect_true("start remains pending while the driver is blocked",
        pending_play.state == PlaybackState::starting && pending_play.accepted == play && pending_play.settled < play);
    expect_command_state(controller, pending_play);
    starting.open();
    expect_command_state(controller, settle(controller, play));

    // Advance within the initial two-second fill; no producer scheduling race
    // or wall-clock playback is needed to publish a nonzero position.
    for (unsigned index = 0; index < 100; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    const auto progressed = await(controller, [](const auto& state) { return state.position_frames >= rate; });
    expect_command_state(controller, progressed);

    const auto stop = send(controller, PlaybackCommand::stop);
    stopping.wait();
    const auto pending_stop = controller.snapshot();
    expect_true("Stop cannot settle before driver cleanup",
        pending_stop.state == PlaybackState::stopping && pending_stop.accepted == stop && pending_stop.settled < stop);
    expect_command_state(controller, pending_stop);
    stopping.open();
    const auto stopped = settle(controller, stop);
    expect("Stop rewinds before its completion is offered to the UI", stopped.position_frames, 0);
    expect_command_state(controller, stopped);

    const auto closing = send(controller, PlaybackCommand::close);
    releasing.wait();
    const auto pending_close = controller.snapshot();
    expect_true("Close remains pending until Release returns",
        pending_close.state == PlaybackState::closing && pending_close.settled < closing);
    expect_command_state(controller, pending_close);
    releasing.open();
    expect_command_state(controller, state_is(controller, PlaybackState::closed));
    controller.join();
    driver.check_closed();
}

void test_display_snapshot_refresh()
{
    TempFile first, second, list, configuration;
    constexpr std::uint32_t rate = 48000;
    const auto first_path = first.write(wave({pcm16(2, rate), pcm16_ramp(2, rate + rate / 4 + 13)}));
    const auto second_path = second.write(wave({pcm16(2, rate), pcm16_ramp(2, rate * 3 + 29)}));
    const std::string text = "[General]\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\n"
        "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nName=A device label retained across progress\n"
        "BufferSize=480\nStartSilence=0\n[Window]\nX=10\nY=20\nWidth=640\nHeight=480\n";
    ControllerDriver driver(false, 480);
    driver.preroll_switches = 0;
    PlaybackController controller(CLSID{}, L"", driver.activation(), {}, {},
        configuration.write(Bytes(text.begin(), text.end())), write_playlist(list, {first_path, second_path}));
    (void)state_is(controller, PlaybackState::stopped);
    PlaybackSnapshot view;
    expect_true("the first display refresh copies the full state", !controller.refresh_snapshot(view, 0));
    expect_same_snapshot(view, controller.snapshot());
    const auto idle_revision = view.revision;
    (void)settle(controller, send(controller, PlaybackCommand::play));
    expect_true("starting playback copies the full state", !controller.refresh_snapshot(view, idle_revision));
    expect_same_snapshot(view, controller.snapshot());
    const auto rendered = view.revision;
    auto unfinished_render = view;

    // Stay within the initial two-second fill, including the later same-rate
    // boundary. This test must not depend on the producer winning a refill race.
    for (unsigned index = 0; index < 100; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    (void)await(controller, [](const auto& state) { return state.position_frames >= rate; });
    expect_true("same-track progress copies only its scalar fields", controller.refresh_snapshot(view, rendered));
    expect_same_snapshot(view, controller.snapshot());

    // A caller that copied progress but did not finish rendering must not use
    // that copy as a rendered baseline on its next attempt. No API fault hook
    // is needed to exercise this failure-to-commit contract.
    expect_true("a second view receives the same progress", controller.refresh_snapshot(unfinished_render, rendered));
    expect_true("an unfinished render requires a full refresh",
        !controller.refresh_snapshot(unfinished_render, rendered));
    expect_same_snapshot(unfinished_render, controller.snapshot());
    expect_true("forcing the already current revision still copies the full state",
        !controller.refresh_snapshot(view, 0));
    expect_same_snapshot(view, controller.snapshot());

    auto previous = view.revision;
    (void)settle(controller, controller.select_track(1));
    expect_true("a separate selection requires a full refresh", !controller.refresh_snapshot(view, previous));
    expect_same_snapshot(view, controller.snapshot());
    expect_true("selection leaves the current track alone", view.current_index == 0 && view.selected_index == 1);
    previous = view.revision;
    const auto state_revision = view.state_revision;
    for (unsigned index = 0; index < 30; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    (void)await(controller, [](const auto& state) { return state.current_index == 1; });
    expect_true("a same-rate track boundary requires a full refresh", !controller.refresh_snapshot(view, previous));
    expect_same_snapshot(view, controller.snapshot());
    expect("the track boundary used a progress publication", view.state_revision, state_revision);
    expect("the new track supplies its own duration", view.frame_count, rate * 3 + 29);

    previous = view.revision;
    (void)settle(controller, send(controller, PlaybackCommand::pause), &driver);
    expect_true("Pause requires a full refresh", !controller.refresh_snapshot(view, previous));
    expect_same_snapshot(view, controller.snapshot());
    previous = view.revision;
    (void)settle(controller, send(controller, PlaybackCommand::play));
    expect_true("Resume requires a full refresh", !controller.refresh_snapshot(view, previous));
    expect_same_snapshot(view, controller.snapshot());
    previous = view.revision;
    (void)send(controller, PlaybackCommand::close);
    expect_true("accepted Close cannot use the progress path", !controller.refresh_snapshot(view, previous));
    (void)state_is(controller, PlaybackState::closed);
    expect_true("Closed always refreshes the full state", !controller.refresh_snapshot(view, view.revision));
    expect_same_snapshot(view, controller.snapshot());
    controller.join();
    driver.check_closed();
}

void test_display_snapshot_with_diagnostic_history()
{
    std::array<TempFile, 4> files;
    TempFile list;
    // MockAsio supports 44.1/48 kHz. Alternating them creates four distinct
    // rate segments without turning this history test into a rate refusal.
    constexpr std::array<std::uint32_t, 4> rates{48000, 44100, 48000, 44100};
    std::array<std::wstring, 4> paths;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto frames = index + 1 == files.size() ? rates[index] * 3 : 3 * ControllerDriver::block;
        paths[index] = files[index].write(wave({pcm16(2, rates[index]), pcm16_ramp(2, frames)}));
    }
    ControllerDriver driver(false, 480);
    driver.preroll_switches = 0;
    PlaybackController controller(selected, L"", driver.activation(), {.start_silence = 0ms}, {}, {},
        write_playlist(list, {paths[0], paths[1], paths[2], paths[3]}));
    (void)state_is(controller, PlaybackState::stopped);
    auto view = settle(controller, send(controller, PlaybackCommand::play));
    for (std::size_t index = 1; index < paths.size(); ++index) {
        const auto rendered = view.revision;
        (void)await(controller, [index](const auto& state) {
            return state.state == PlaybackState::playing && state.current_index == index &&
                state.diagnostics.current_segment == index;
        }, &driver);
        expect_true("each new rate copies its diagnostic history", !controller.refresh_snapshot(view, rendered));
        expect_same_snapshot(view, controller.snapshot());
    }
    expect("capability history grows beyond the two initial slots", view.diagnostics.buffer_capabilities.size(), 4);
    expect("allocation history grows beyond the two initial slots", view.diagnostics.segments.size(), 4);
    expect_command_state(controller, view);
    const auto rendered = view.revision;
    for (unsigned index = 0; index < 100; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    (void)await(controller, [rate = rates.back()](const auto& state) { return state.position_frames >= rate; });
    expect_true("steady progress reuses the complete diagnostic history", controller.refresh_snapshot(view, rendered));
    expect_same_snapshot(view, controller.snapshot());

    const auto before_fault = view.revision;
    driver.message_notification(kAsioOverload, 0, nullptr, nullptr);
    (void)state_is(controller, PlaybackState::failed);
    expect_true("a device fault copies the full state", !controller.refresh_snapshot(view, before_fault));
    expect_same_snapshot(view, controller.snapshot());
    expect_true("the refreshed snapshot retains the device fault", view.faulted());
    expect_command_state(controller, view);
    (void)close(controller);
    expect_true("Close retains the full diagnostic and fault state", !controller.refresh_snapshot(view, view.revision));
    expect_same_snapshot(view, controller.snapshot());
    expect_command_state(controller, view);
    driver.check_closed();
}

void test_change_listener()
{
    struct Counter {
        std::atomic<std::uint64_t> calls{}, last{};
        static void changed(void* context, std::uint64_t revision) noexcept
        {
            auto& self = *static_cast<Counter*>(context);
            self.calls.fetch_add(1, std::memory_order_relaxed);
            self.last.store(revision, std::memory_order_relaxed);
        }
    } counter;
    const auto data = pcm16_ramp(2, 3 * ControllerDriver::block, 0x1357);
    TempFile file;
    ControllerDriver driver;
    PlaybackController controller(selected, file.write(wave({pcm16(2, 48000), data})), driver.activation(),
        {.start_silence = 0ms}, {Counter::changed, &counter});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    const auto closed = close(controller);
    // Each revision after the initial one is announced once, the final closed revision included.
    expect("listener calls", counter.calls.load(), closed.revision - 1);
    expect("listener saw the final revision", counter.last.load(), closed.revision);
}

// Starts, accepts a same-track selection, plays past the first whole second
// and closes. A listener without a progress member receives that second
// through `changed`; both forms retain the intervening state publication.
void check_progress_listener(bool progress_member)
{
    expect("an unpublished snapshot has no state revision", PlaybackSnapshot{}.state_revision, 0);
    struct Counter {
        std::atomic<std::uint64_t> changes{}, progress{}, last_change{}, last_progress{};
        static void changed(void* context, std::uint64_t revision) noexcept
        {
            auto& self = *static_cast<Counter*>(context);
            self.changes.fetch_add(1, std::memory_order_relaxed);
            self.last_change.store(revision, std::memory_order_relaxed);
        }
        static void progressed(void* context, std::uint64_t revision) noexcept
        {
            auto& self = *static_cast<Counter*>(context);
            self.progress.fetch_add(1, std::memory_order_relaxed);
            self.last_progress.store(revision, std::memory_order_relaxed);
        }
    } counter;
    TempFile file;
    constexpr std::uint32_t rate = 48000;
    const auto path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, rate * 3)}));
    ControllerDriver driver(false, 480);
    driver.preroll_switches = 0;
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms},
        {Counter::changed, &counter, progress_member ? &Counter::progressed : nullptr});
    (void)state_is(controller, PlaybackState::stopped);
    const auto started = settle(controller, send(controller, PlaybackCommand::play));
    expect("initialization and start are not progress", counter.progress.load(), 0);
    expect("the start went to the change listener", counter.last_change.load(), started.revision);
    expect("start stamps its non-progress revision", started.state_revision, started.revision);
    // A view may miss the separate selection notification and next observe a
    // progress publication. The state stamp must still force its full refresh,
    // even for a selection that changes receipts but not the selected item.
    const auto selection_receipt = controller.select_track(0);
    expect_true("same-track selection is accepted during playback", selection_receipt != 0);
    const auto selection = settle(controller, selection_receipt);
    expect("selection stamps its non-progress revision", selection.state_revision, selection.revision);
    for (unsigned index = 0; index < 100; ++index) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    const auto progressed = await(controller, [](const auto& state) { return state.position_frames >= rate; });
    // Nothing but the new whole second changed this revision.
    expect("progress advances the revision", static_cast<int>(progressed.revision > selection.revision), 1);
    expect("progress retains a coalesced state publication", progressed.state_revision, selection.state_revision);
    expect_true("a skipped state notification cannot look like progress alone",
        progressed.state_revision != started.state_revision);
    auto skipped_state = started;
    expect_true("a coalesced state change cannot use a scalar-only refresh",
        !controller.refresh_snapshot(skipped_state, started.revision));
    expect_same_snapshot(skipped_state, controller.snapshot());
    if (progress_member) {
        expect("the new second went to the progress listener", counter.last_progress.load(), progressed.revision);
        expect("progress is not a state change", counter.last_change.load(), selection.revision);
    } else {
        expect("without a progress member the new second is a change", counter.last_change.load(),
            progressed.revision);
    }
    const auto closed = close(controller);
    expect("Close stamps its final state revision", closed.state_revision, closed.revision);
    expect_true("Close went to the change listener", counter.last_change.load() == closed.revision &&
        counter.last_progress.load() < closed.revision);
    // Every revision after the initial one reaches exactly one of the two.
    expect("each revision is announced once", counter.changes.load() + counter.progress.load(), closed.revision - 1);
    driver.check_closed();
}

void test_progress_listener()
{
    check_progress_listener(true);
    check_progress_listener(false);
}

void test_controller_claim_and_implicit_close()
{
    TempFile file;
    const auto path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)}));
    ControllerDriver driver;
    expect_throw<std::invalid_argument>("invalid construction releases the controller claim", [&] {
        PlaybackController rejected(selected, path, nullptr, {.start_silence = 0ms});
    });
    {
        PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        expect_throw<std::logic_error>("a second controller cannot borrow the global ASIO loop", [&] {
            PlaybackController rejected(selected, path, driver.activation(), {.start_silence = 0ms});
        });
        (void)settle(controller, send(controller, PlaybackCommand::play));
    } // Destruction must stop and join without an explicit Close.
    driver.check_closed();
    {
        PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        (void)close(controller);
    }
    expect("destruction releases the singleton claim for a new selection", driver.activations, 2);
    driver.check_closed();
}

void test_callback_watchdog_under_control_flood()
{
    TempFile file;
    ControllerDriver driver;
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    (void)state_is(controller, PlaybackState::stopped);
    (void)settle(controller, send(controller, PlaybackCommand::play));
    {
        std::jthread commands([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                (void)controller.submit(PlaybackCommand::play);
                std::this_thread::yield();
            }
        });
        const auto failed = state_is(controller, PlaybackState::failed); // Deliberately no pulses.
        expect("command wakes do not conceal a dead callback clock", static_cast<int>(failed.result),
            static_cast<int>(wavplay::playback::PlaybackResult::driver_error));
    }
    (void)close(controller);
    expect("control flood never restarts stalled playback", driver.starts, 1);
    driver.check_closed();
}

void test_command_burst_wake_coalescing()
{
    TempFile file;
    ControllerDriver driver;
    CallGate startup;
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start) { startup.block(); }
    };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    ReleaseGate release{startup};
    (void)state_is(controller, PlaybackState::stopped);
    (void)send(controller, PlaybackCommand::play);
    startup.wait();
    // The owning STA is held inside start; it cannot consume this event and
    // the mock cannot publish callbacks yet. Observe each submit in isolation.
    const auto event = wavplay::playback::notifications().event_handle();
    if (!event) {
        throw std::runtime_error("start did not create the playback notification event");
    }
    expect("clear pre-existing setup wakes", ResetEvent(event), TRUE);
    for (std::size_t index = 0; index < PlaybackController::queue_capacity; ++index) {
        (void)send(controller, PlaybackCommand::play);
        expect("one wake covers a pending command burst", WaitForSingleObject(event, 0),
            index == 0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
    }
    const auto closing = send(controller, PlaybackCommand::close);
    expect("Close wakes even with a full pending batch", WaitForSingleObject(event, 0), WAIT_OBJECT_0);
    startup.open();
    const auto closed = state_is(controller, PlaybackState::closed);
    expect("burst Close settles every accepted command", closed.settled, closing);
    controller.join();
    expect("pending duplicate Plays never restart audio", driver.starts, 1);
    driver.check_closed();
}

void test_active_command_batches_without_callbacks()
{
    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        TempFile file;
        ControllerDriver driver;
        PlaybackController controller(selected,
            file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
        (void)state_is(controller, PlaybackState::stopped);
        (void)settle(controller, send(controller, PlaybackCommand::play));
        const auto deadline = std::chrono::steady_clock::now() + hang_budget(500ms);
        std::uint64_t last{};
        // More requests than fit in one queue/batch. No driver pulse may help
        // deliver them; every drained/refilled batch must remain discoverable.
        for (std::size_t index = 0; index < 8 * PlaybackController::queue_capacity;) {
            if (const auto receipt = controller.submit(PlaybackCommand::play)) {
                last = receipt;
                ++index;
            } else {
                if (std::chrono::steady_clock::now() >= deadline) { fail("Active command batch did not make progress"); }
                std::this_thread::yield();
            }
        }
        while (controller.snapshot().settled < last) {
            if (std::chrono::steady_clock::now() >= deadline) { fail("Final active command batch lost its wake"); }
            const auto state = controller.snapshot();
            (void)controller.wait_for_change(state.revision, 1ms);
        }
        const auto closed = close(controller);
        expect("control batches do not wait for the callback watchdog", static_cast<int>(closed.faulted()), 0);
        expect("control batches do not restart audio", driver.starts, 1);
        driver.check_closed();
    }
}

void test_idle_message_fairness()
{
    TempFile file;
    ControllerDriver driver;
    CallGate initialization;
    StaMessageProbe probe;
    driver.on_init = [&] {
        probe.attach();
        initialization.block();
    };
    driver.on_release = [&] { probe.detach(); };
    PlaybackController controller(selected,
        file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})), driver.activation(), {.start_silence = 0ms});
    ReleaseGate release{initialization};
    initialization.wait();
    probe.inspect = [&] { return controller.snapshot().settled; };
    std::uint64_t last{};
    for (std::size_t i = 0; i < PlaybackController::queue_capacity; ++i) {
        last = send(controller, PlaybackCommand::pause);
    }
    probe.post();
    initialization.open();
    expect("pending STA message is serviced before the first command batch", probe.wait(), 0);
    (void)settle(controller, last);
    (void)close(controller);
    expect("idle control batch does not start audio", driver.starts, 0);
    driver.check_closed();
}

void test_audition_preflight()
{
    ControllerDriver driver;
    expect_throw<std::invalid_argument>("unknown controller scenario is refused before driver activation", [&] {
        audition_asio_controller(selected_text, L"unused.wav", L"unknown", driver.activation());
    });
    struct Input { std::uint8_t channels; std::uint32_t rate; std::size_t frames; };
    for (const auto input : {Input{2, 48000, 48000}, Input{2, 44100, 6 * 44100}, Input{1, 48000, 6 * 48000}}) {
        TempFile file;
        const auto path = file.write(wave({pcm16(input.channels, input.rate), pcm16_ramp(input.channels, input.frames, 1)}));
        expect_throw<std::invalid_argument>("unsuitable controller audition source is refused before activation", [&] {
            audition_asio_controller(selected_text, path, L"pause-resume", driver.activation());
        });
    }
    expect("audition preflight never activates the driver", driver.activations, 0);
}

void test_audition_scenario(const wchar_t* scenario, bool fault_on_close = false)
{
    TempFile file;
    const auto path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 6 * 48000, 0x1234)}));
    ControllerDriver driver(true, 480);
    driver.sample_rate = 44100;
    bool injected{};
    if (fault_on_close) {
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::stop) {
                injected = true;
                driver.message_notification(kAsioOverload, 0, nullptr, nullptr);
            }
        };
    }
    {
        // The CLI deliberately waits real time between commands. A 10 ms
        // callback period matches this fixture's 480-frame buffers at 48 kHz.
        // pulse/start/stop share clock_gate_, so ticks cannot race a restart.
        // Waiting from the previous callback avoids catch-up bursts after a
        // scheduling delay; the six-second source leaves room for that jitter.
        std::jthread clock([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(10ms);
                if (!stop.stop_requested()) {
                    driver.pulse();
                }
            }
        });
        const auto audition = [&] { audition_asio_controller(selected_text, path, scenario, driver.activation()); };
        if (fault_on_close) {
            expect_throw<std::runtime_error>("audition reports a driver fault during Close", audition);
        } else {
            audition();
        }
    }
    const auto repeated = std::wstring_view(scenario) == L"pause-resume" ||
        std::wstring_view(scenario) == L"stop-play" || std::wstring_view(scenario) == L"select-play";
    expect("audition starts only the requested runs", driver.starts, repeated ? 2 : 1);
    expect("audition stops every started run", driver.stops, driver.starts);
    expect("audition disposes every created allocation", driver.disposals, driver.creates);
    expect("audition restores the original clock", static_cast<int>(driver.sample_rate == 44100), 1);
    expect("audition close fault was injected", static_cast<int>(injected), static_cast<int>(fault_on_close));
    driver.check_closed();
}

// run_default_timer_child's exit code when the checks passed although its
// waits were finer than the default tick.
constexpr unsigned long default_timer_finer = 5;

// The median wall time of Sleep(1): about 15.6 ms at the default timer
// resolution, about 1 ms in a suite process.
std::chrono::duration<double, std::milli> sleep_granularity()
{
    std::array<std::chrono::steady_clock::duration, 15> samples{};
    Sleep(1); // Start the samples at a tick.
    for (auto& sample : samples) {
        const auto before = std::chrono::steady_clock::now();
        Sleep(1);
        sample = std::chrono::steady_clock::now() - before;
    }
    std::ranges::nth_element(samples, samples.begin() + samples.size() / 2);
    return samples[samples.size() / 2];
}

// The controller's progress where the product's waits end on the timer tick
// of a process that raises no resolution. A clock thread delivers 4,096-frame
// callbacks at 48 kHz (85.3 ms) in real time; a wake that comes a tick late
// delivers every block due since, as a driver's clock keeps time. Until the
// seventh second each whole second is published exactly once: as many progress
// publications as seconds. No watchdog fault occurs, and the track completes
// with its drain.
void check_progress_at_default_timer()
{
    struct Counter {
        std::atomic<std::uint64_t> progress{};
        static void changed(void*, std::uint64_t) noexcept {}
        static void progressed(void* context, std::uint64_t) noexcept
        {
            static_cast<Counter*>(context)->progress.fetch_add(1, std::memory_order_relaxed);
        }
    } counter;
    constexpr std::uint32_t rate = 48000;
    constexpr long width = 4096;
    constexpr std::uint64_t seconds = 9;
    TempFile file;
    const auto path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, rate * seconds)}));
    ControllerDriver driver(false, width);
    driver.preroll_switches = 0;
    PlaybackController controller(selected, path, driver.activation(), {.start_silence = 0ms},
        {Counter::changed, &counter, Counter::progressed});
    (void)state_is(controller, PlaybackState::stopped);
    PlaybackSnapshot completed;
    {
        std::jthread clock([&](std::stop_token stop) {
            constexpr auto period = std::chrono::nanoseconds(std::chrono::seconds(1)) * width / rate;
            auto next = std::chrono::steady_clock::now() + period;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_until(next);
                for (const auto now = std::chrono::steady_clock::now(); next <= now; next += period) {
                    driver.pulse(); // Not streaming yet, or any more: nothing happens.
                }
            }
        });
        (void)settle(controller, send(controller, PlaybackCommand::play));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2 * seconds + 5);
        // Real time, not scaled: the clock paces the run in every build.
        const auto follow = [&](auto done, const char* phase) {
            auto state = controller.snapshot();
            while (!done(state)) {
                if (state.faulted() || (state.state != PlaybackState::playing && state.state != PlaybackState::completed)) {
                    fail(std::string(phase) + ": state " + std::to_string(static_cast<int>(state.state)) + ", result " +
                        std::to_string(static_cast<int>(state.result)) + " at frame " + std::to_string(state.position_frames));
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    fail(std::string(phase) + ": deadline expired at frame " + std::to_string(state.position_frames));
                }
                state = controller.wait_for_change(state.revision, 100ms);
            }
            return state;
        };
        (void)follow([&](const auto& state) { return state.position_frames >= (seconds - 2) * rate; }, "playing");
        // No progress publication falls between the two counts, so the
        // snapshot shows the second of the last one counted.
        std::uint64_t published{}, second{};
        do {
            published = counter.progress.load();
            second = controller.snapshot().position_frames / rate;
        } while (counter.progress.load() != published);
        expect("every whole second is published once (publications and the second shown)", published, second);
        completed = follow([](const auto& state) { return state.state == PlaybackState::completed; }, "completion");
    }
    expect("the track completes", static_cast<int>(completed.result), static_cast<int>(PlaybackResult::completed));
    expect("the completed track shows its last frame", completed.position_frames, completed.frame_count);
    const auto closed = close(controller);
    expect("closing keeps a clean result", static_cast<int>(closed.faulted()), 0);
    expect("one run", driver.starts, 1);
    driver.check_closed();
}

// check_progress_at_default_timer runs in a child that keeps the default timer
// resolution (run_default_timer_child). It runs alongside the suite's other
// cases, which leave its process alone, and reports here. The check is judged
// in every case; where its waits were finer anyway, a pass adds a note that
// the default tick was not exercised.
ChildRun run_default_timer_child_process()
{
    return run_own_child(L"--default-timer-child", hang_budget(30s));
}

void test_progress_at_default_timer_resolution(const ChildRun& child)
{
    if (child.timed_out) {
        fail("the default timer resolution child did not exit: " + child.output);
    }
    if (child.exit_code == default_timer_finer) {
        std::printf("[NOTE] controller progress at the default timer resolution: the child's waits were finer, "
            "so the default tick was not exercised (%s).\n", child.output.c_str());
        return;
    }
    if (child.exit_code != 0) {
        fail("the default timer resolution child exited with " + std::to_string(child.exit_code) + ": " + child.output);
    }
}

} // namespace

// Exit codes: 0 passed, 1 a check failed (its reason on stderr), 2 a malformed
// request, default_timer_finer passed with waits finer than the default tick.
int wavplay_test::run_default_timer_child(std::span<wchar_t* const> operands)
{
    if (!operands.empty()) {
        return 2;
    }
    try {
        const auto granularity = sleep_granularity();
        check_progress_at_default_timer();
        if (granularity < 8ms) {
            std::printf("Sleep(1) median %.1f ms", granularity.count());
            return default_timer_finer;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

int wavplay_test::run_playback_controller_tests()
{
    TestSuite suite;
    // About 10 s of real time in a process of its own, so it runs meanwhile;
    // its case comes last.
    auto default_timer = std::async(std::launch::async, run_default_timer_child_process);
    suite.run_timed("controller loads one immutable playlist and completes arbitrary duplicate entries", test_playlist_snapshot_and_completion);
    suite.run_timed("controller reports submitted PCM time through silence pause resume and Stop", test_submitted_time_and_transport);
    suite.run_timed("controller refreshes time without extending a long-block callback watchdog", test_time_refresh_preserves_long_block_watchdog);
    suite.run_timed("controller publishes idle selection metadata and completion together", test_idle_selection_publication);
    suite.run_timed("controller separates selection from playback and resumes across track boundaries", test_playlist_selection_and_pause);
    suite.run_timed("controller selects refused next tracks and accepts another selection", test_playlist_refusal_and_reselection);
    suite.run_timed("controller shows a refused width's range and rate and a failed open's own error", test_playlist_refusal_reasons);
    suite.run_timed("controller Stop refreshes selected time and clears unavailable metadata", test_stop_refreshes_selected_time);
    suite.run_timed("controller handles empty and invalid playlists without starting audio", test_empty_and_invalid_playlists);
    suite.run_timed("controller preserves selection receipts and delivers Stop during setup and pause", test_playlist_selection_receipts_and_stop);
    suite.run_timed("controller prepares only the last of consecutive idle selections", test_superseded_idle_selections);
    suite.run_timed("controller Stop and Close reach a playlist run while it is prepared", test_stop_and_close_while_run_is_prepared);
    suite.run_timed("controller Select and Play reaches a playlist run while it is prepared", test_select_and_play_while_run_is_prepared);
    suite.run_timed("controller revalidates changed files while retaining its startup list", test_playlist_revalidates_changed_files);
    suite.run_timed("controller applies startup settings once and observes edits on the next launch", test_settings_read_once);
    suite.run_timed("controller refuses invalid startup settings before opening ASIO", test_settings_failure_before_activation);
    suite.run_timed("controller reports queried capabilities independently of rejected allocations", test_buffer_capability_diagnostics);
    suite.run_timed("controller clears old buffer facts before querying a new Play", test_buffer_capabilities_refresh);
    suite.run_timed("controller uses INI selection and output pair and saves only window state after release", test_application_settings_and_window_save);
    suite.run_timed("controller reports no device and preserves settings across startup and save failures", test_no_device_and_save_failures);
    suite.run_timed("controller Close during the list load skips the device phase and NO DEVICE", test_close_during_list_load);
    suite.run_timed("controller publishes its playlist before reporting NO DEVICE", test_no_device_after_playlist);
    suite.run_timed("controller Close during the --wav validation skips the window and the driver", test_close_during_wav_validation);
    suite.run_timed("controller Close stops a long list's validation at its next entry and publishes no list", test_close_cancels_list_validation);
    suite.run_timed("controller pause/resume and EOF replay preserve PCM and STA ownership", test_pause_resume_and_eof_replay);
    suite.run_timed("controller same-item Select and Play restarts the complete PCM without reinitialization", test_same_track_select_and_play);
    suite.run_timed("controller serializes Stop immediately followed by Play", test_stop_then_play);
    suite.run_timed("controller Stop overrides a pending pause drain", test_pending_pause_stop_and_play);
    suite.run_timed("controller accepts without waiting and closes through a full queue", test_nonblocking_and_full_queue_close);
    suite.run_timed("controller distinguishes Stop acceptance from cleanup completion", test_stop_completion_follows_cleanup);
    suite.run_timed("controller closes idle, playing and paused states and restores the clock", test_close_states_and_restore);
    suite.run_timed("controller closes after initialization, playback and cleanup failures", test_failures);
    suite.run_timed("controller reports a read error with the producer's ReadFile cause", test_read_error_cause);
    suite.run_timed("controller keeps the original failure nested under a cleanup disposal failure", test_cleanup_failure_keeps_original);
    suite.run_timed("controller reports driver faults that race a Stop command", test_notification_during_stop);
    suite.run_timed("controller accepts Pause Stop and Close during startup", test_controls_during_startup);
    suite.run_timed("controller Close cancels replay during stop and disposal", test_close_during_teardown);
    suite.run_timed("controller observes driver faults after Stop Pause and EOF", test_idle_driver_notifications);
    suite.run_timed("controller reports an idle fault that reaches the next Play with its own result", test_idle_fault_reaching_play);
    suite.run_timed("controller reinitializes reset requests and waits for a new Play", [] { test_active_reinitialization(kAsioResetRequest); });
    suite.run_timed("controller reinitializes buffer changes and uses the fresh configuration", [] { test_active_reinitialization(kAsioBufferSizeChange); });
    suite.run_timed("controller keeps fatal teardown faults ahead of reinitialization", test_reinitialization_fault_priority);
    suite.run_timed("controller revalidates a retained manual width against the fresh driver", test_reinitialization_revalidates_manual_width);
    suite.run_timed("controller stops on recovery activation init inquiry or notification failure", test_reinitialization_failure);
    suite.run_timed("controller accepts Close during release and reinitialization", test_close_during_reinitialization);
    suite.run_timed("controller preserves notifications delivered during driver Release", test_fault_during_driver_release);
    suite.run_timed("controller retains Release faults after Close cancels reinitialization", test_close_keeps_fault_after_cancelled_reinitialization);
    suite.run_timed("controller closes on WM_CLOSE and WM_QUIT without callbacks", test_window_shutdown);
    suite.run_timed("controller releases singleton ownership and implicitly closes", test_controller_claim_and_implicit_close);
    suite.run_timed("controller announces every snapshot revision to its change listener", test_change_listener);
    suite.run_timed("controller labels progress publications for an optional progress listener", test_progress_listener);
    suite.run_timed("controller refreshes a reused snapshot exactly like the by-value snapshot", test_snapshot_refresh_into_existing_object);
    suite.run_timed("controller command queries stay current during pending start Stop and Close", test_command_state_during_pending_operations);
    suite.run_timed("controller refreshes display progress with a valid rendered baseline", test_display_snapshot_refresh);
    suite.run_timed("controller refreshes display progress after diagnostic history grows", test_display_snapshot_with_diagnostic_history);
    suite.run_timed("controller reports an unsupported rate as a refusal, not a driver fault", test_rate_refusal_result);
    suite.run_timed("controller watchdog detects dead callbacks despite command flood", test_callback_watchdog_under_control_flood);
    suite.run_timed("controller coalesces command bursts and still wakes Close", test_command_burst_wake_coalescing);
    suite.run_timed("controller drains active command batches without callback wakes", test_active_command_batches_without_callbacks);
    suite.run_timed("controller services queued STA messages before an idle command batch", test_idle_message_fairness);
    suite.run_timed("controller audition validates input before activating ASIO", test_audition_preflight);
    suite.run_timed("controller audition CLI completes pause and resume with mock ASIO", [] { test_audition_scenario(L"pause-resume"); });
    suite.run_timed("controller audition CLI completes immediate Stop and Play with mock ASIO", [] { test_audition_scenario(L"stop-play"); });
    suite.run_timed("controller audition CLI restarts the same playing item with mock ASIO", [] { test_audition_scenario(L"select-play"); });
    suite.run_timed("controller audition CLI closes while playing with mock ASIO", [] { test_audition_scenario(L"close-playing"); });
    suite.run_timed("controller audition CLI closes while paused with mock ASIO", [] { test_audition_scenario(L"close-paused"); });
    suite.run_timed("controller audition CLI reports a notification fault during Close", [] { test_audition_scenario(L"close-playing", true); });
    // The child's own timeout bounds the wait.
    suite.run("controller publishes every whole second at the default timer resolution",
        [&] { test_progress_at_default_timer_resolution(default_timer.get()); });
    return suite.result("Playback controller");
}
