#include "suites/suites.h"
#include "wavplay/core/win32.h"
#include "wavplay/ui/error_message.h"
#include "wavplay/ui/player_controls.h"
#include "wavplay/ui/player_text.h"
#include "wavplay/ui/player_window.h"
#include "wavplay/settings/device_config.h"
#include "fixtures/call_gate.h"
#include "fixtures/thread_state.h"
#include "fixtures/ui_desktop_guard.h"
#include "fixtures/playback_fixture.h"

#include <imm.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "imm32.lib")

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
namespace ui = wavplay::ui;
constexpr wchar_t selected_text[] = L"{76543210-89AB-CDEF-8123-456789ABCDEF}";

class GuiDriver final : public MockAsio {
public:
    static constexpr long frames = 48;
    // Callbacks have no real-time clock in these tests. A whole 4,099-frame run
    // after 100 ms of start silence, with its drain, takes fewer than this.
    static constexpr unsigned run_pulses = 256;
    std::function<void()> on_init, on_release;
    bool fail_init{}, refuse_restore{};
    unsigned references{}, releases{}, activations{}, inits{};
    std::vector<std::vector<Block>> captures;

    GuiDriver()
    {
        buffer_size = {frames, frames, frames, 0};
        next_width = frames;
        latency = next_latency = 73;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (on_release) { on_release(); }
        ++releases;
        return --references;
    }
    ASIOBool init(void*) override
    {
        ++inits;
        if (on_init) { on_init(); }
        return fail_init ? ASIOFalse : ASIOTrue;
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
        if (result == ASE_OK) { captures.push_back(submitted); }
        return result;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override
    {
        if (refuse_restore && rate == 44100) { return ASE_HWMalfunction; }
        return MockAsio::setSampleRate(rate);
    }
    void pulse()
    {
        const std::lock_guard lock(clock_gate_);
        tick();
    }
    void check_closed() const
    {
        expect("GUI releases every driver instance", releases, activations);
        expect("GUI retains no driver reference", references, 0);
        expect("GUI disposes every allocation", disposals, creates);
    }
protected:
    IASIO* activated(const CLSID& clsid) override
    {
        expect("GUI opens the requested CLSID", IsEqualCLSID(clsid, selected), TRUE);
        ++activations;
        AddRef();
        return this;
    }
private:
    std::mutex clock_gate_;
};

// Run the actual product message loop. The test never replaces its WndProc or
// controller, and probes responsiveness from a separate thread while a driver
// call is held indefinitely by a test-controlled gate.
class Gui {
public:
    Gui(MockAsio& driver, std::wstring path, int show_command = SW_HIDE, std::chrono::milliseconds start_silence = 0ms,
        std::wstring settings_path = {}, bool read_selected_device = false, std::wstring playlist_path = {},
        std::source_location location = std::source_location::current())
    {
        try {
            std::promise<DWORD> started;
            auto thread = started.get_future();
            done_ = TestThread<int>([this, activate = driver.activation(), path = std::move(path),
                started = std::move(started), show_command, start_silence, settings_path = std::move(settings_path),
                read_selected_device, playlist_path = std::move(playlist_path)]() mutable {
                // Hooks must be removed while their target thread still exists,
                // including when the product loop throws or exits early.
                const struct HookLifetime {
                    Gui& gui;
                    ~HookLifetime() { gui.release_hooks(); }
                } hooks{*this};
                entry_priorities_ = thread_priorities(GetCurrentThread());
                started.set_value(GetCurrentThreadId());
                const auto code = ui::run_player_window({read_selected_device ? CLSID{} : selected, std::move(path),
                    std::move(settings_path), std::move(playlist_path)}, show_command,
                    std::move(activate), {.start_silence = start_silence});
                // Read after finish(), once this future is ready. A failed
                // query is recorded rather than replacing the exit code.
                try {
                    exit_speed_ = thread_speed_state(GetCurrentThread());
                    exit_priorities_ = thread_priorities(GetCurrentThread());
                } catch (...) {
                    exit_query_failed_ = true;
                }
                return code;
            });
            thread_ = thread.get();
            wait_until([&] {
                EnumThreadWindows(thread_, [](HWND candidate, LPARAM context) -> BOOL {
                    wchar_t name[80]{};
                    GetClassNameW(candidate, name, static_cast<int>(std::size(name)));
                    if (std::wstring_view(name) == ui::player_window_class) {
                        *reinterpret_cast<HWND*>(context) = candidate;
                        return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&window));
                return window && GetDlgItem(window, ui::control_id::pending);
            }, "GUI window creation", location);
            // A thread ID alone does not imply a message queue exists. Window
            // creation establishes it before the test installs its input hook
            // or sends any input, without altering the product's startup path.
            const std::lock_guard lock(hook_gate_);
            if (hook_thread_finished_) {
                throw std::runtime_error("GUI exited before installing test hooks" + detail::location_suffix(location));
            }
            hook_ = SetWindowsHookExW(WH_GETMESSAGE, input_barrier, nullptr, thread_);
            if (!hook_) {
                const auto error = GetLastError();
                throw std::runtime_error("Could not install GUI input barrier hook: Win32 error " + std::to_string(error) +
                    detail::location_suffix(location));
            }
            // The product changes control text and enabled state only on its
            // own thread. Observing those calls lets waits wake on each update
            // instead of sleeping between reads of the controls.
            updated_gui_.store(this);
            update_hook_ = SetWindowsHookExW(WH_CALLWNDPROCRET, control_updated, nullptr, thread_);
            if (!update_hook_) {
                const auto error = GetLastError();
                throw std::runtime_error("Could not install GUI update hook: Win32 error " + std::to_string(error) +
                    detail::location_suffix(location));
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Gui() { cleanup(); }
    void cleanup() noexcept
    {
        SetEvent(modal_release_.get());
        if (done_.valid()) {
            // Without a window to close, quit the loop that still runs.
            const auto close = [this] {
                if ((!window || !PostMessageW(window, WM_CLOSE, 0, 0)) && thread_) {
                    PostThreadMessageW(thread_, WM_QUIT, 1, 0);
                }
            };
            close();
            if (done_.wait_for(5s) != std::future_status::ready) {
                // A late shutdown error deliberately leaves the product window
                // open for acknowledgment. Send Close again before giving up.
                close();
                if (done_.wait_for(hang_budget(5s)) != std::future_status::ready) {
                    abandon_hung("GUI cleanup timed out");
                }
            }
        }
    }
    HWND window{};
    // The thread that runs run_player_window.
    [[nodiscard]] DWORD thread() const noexcept { return thread_; }
    // That thread's power throttling right after run_player_window returned
    // (nullopt if unreported or if the query failed). Valid after finish().
    [[nodiscard]] const std::optional<ThreadSpeedState>& exit_speed() const noexcept { return exit_speed_; }
    [[nodiscard]] bool exit_query_failed() const noexcept { return exit_query_failed_; }
    // entry is published by the constructor's started promise; exit by finish().
    [[nodiscard]] const ThreadPriorities& entry_priorities() const noexcept { return entry_priorities_; }
    [[nodiscard]] const ThreadPriorities& exit_priorities() const noexcept { return exit_priorities_; }
    struct ControlWork { unsigned fonts, placements; };
    [[nodiscard]] ControlWork control_work() const noexcept
    {
        return {font_changes_.load(), placements_.load()};
    }
    // Posted state notifications the product's thread has retrieved so far.
    [[nodiscard]] unsigned posted_notifications() const noexcept { return notifications_.load(); }
    // WM_SETTEXT per child control (by control_id::slot), WM_ENABLE to any
    // child, and selection queries received by the list. The last also counts
    // selected_track() calls from the test, so sample it before those queries.
    struct ControlUpdates {
        std::array<unsigned, ui::control_id::count> texts;
        unsigned enables;
        unsigned selection_queries;
    };
    [[nodiscard]] ControlUpdates control_updates() const noexcept
    {
        ControlUpdates updates{};
        for (std::size_t slot = 0; slot < updates.texts.size(); ++slot) { updates.texts[slot] = texts_set_[slot].load(); }
        updates.enables = enables_.load();
        updates.selection_queries = selection_queries_.load();
        return updates;
    }
    // Holds the UI thread inside its message retrieval until the product
    // queues a state notification, then destroys the window from that thread,
    // which discards the queued notification. Returns once the window is gone.
    template<class Publish>
    void destroy_with_queued_notification(Publish publish)
    {
        ResetEvent(barrier_.get());
        expect("window destruction probe is queued", PostThreadMessageW(thread_, focus_message,
            reinterpret_cast<WPARAM>(this), 7), TRUE);
        publish();
        wait_for(barrier_.get(), "external window destruction");
        expect("the product queued a notification before the destruction", static_cast<int>(destroy_timed_out_.load()), 0);
    }
    void check_no_input_method()
    {
        probe_focus(5);
        expect("IME probe executes on the window's owning thread", ime_probe_thread_.load(), thread_);
        expect("read-only GUI has no default IME window", ime_windows_.load(), 0);
    }
    void begin_modal_loop()
    {
        ResetEvent(barrier_.get());
        expect("nested UI loop request is queued", PostThreadMessageW(thread_, focus_message,
            reinterpret_cast<WPARAM>(this), 6), TRUE);
        wait_for(modal_entered_.get(), "nested UI loop entry");
    }
    void end_modal_loop()
    {
        SetEvent(modal_release_.get());
        wait_for(barrier_.get(), "nested UI loop exit");
        expect("nested UI loop has no wait failure", static_cast<int>(modal_failed_.load()), 0);
    }
    void responsive(std::source_location location = std::source_location::current()) const
    {
        (void)send(window, WM_NULL, 0, 0, location);
    }
    std::wstring text(int id, std::source_location location = std::source_location::current()) const
    {
        wchar_t value[2048]{};
        const HWND child = GetDlgItem(window, id);
        if (!child) {
            throw std::runtime_error("GUI control does not exist: " + std::to_string(id) + detail::location_suffix(location));
        }
        (void)send(child, WM_GETTEXT, std::size(value), reinterpret_cast<LPARAM>(value), location);
        return value;
    }
    bool enabled(int id) const { return IsWindowEnabled(GetDlgItem(window, id)) != FALSE; }
    void click(int id, std::source_location location = std::source_location::current()) const
    {
        (void)send(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, id)), location);
    }
    LRESULT selected_track(std::source_location location = std::source_location::current()) const
    {
        return send(GetDlgItem(window, ui::control_id::playlist), LB_GETCURSEL, 0, 0, location);
    }
    void select_track(std::size_t index, bool play = false,
        std::source_location location = std::source_location::current()) const
    {
        const auto list = GetDlgItem(window, ui::control_id::playlist);
        expect("GUI list selection is valid", send(list, LB_SETCURSEL, index, 0, location), static_cast<LRESULT>(index));
        (void)send(window, WM_COMMAND, MAKEWPARAM(ui::control_id::playlist, play ? LBN_DBLCLK : LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(list), location);
    }
    void key(UINT key, int target = 0, bool repeat = false) const
    {
        const HWND destination = target ? GetDlgItem(window, target) : window;
        expect("GUI key is queued", PostMessageW(destination, WM_KEYDOWN, key, repeat ? (LPARAM{1} << 30) | 1 : 1), TRUE);
        if (!repeat) { expect("GUI key release is queued", PostMessageW(destination, WM_KEYUP, key, (LPARAM{3} << 30) | 1), TRUE); }
    }
    HWND focused_window()
    {
        probe_focus(0);
        return observed_focus_.load();
    }
    void focus(int id)
    {
        requested_focus_.store(id ? GetDlgItem(window, id) : window);
        probe_focus(1);
        expect("GUI focus setup reaches its control",
            static_cast<int>(observed_focus_.load() == requested_focus_.load()), 1);
    }
    void reactivate()
    {
        requested_focus_.store(window);
        activation_changed_.store(false);
        probe_focus(2);
        expect("GUI focus test activates a different top-level window", static_cast<int>(activation_changed_.load()), 1);
    }
    HWND require_keyboard_focus(const char* phase = "GUI",
        std::source_location location = std::source_location::current())
    {
        const auto target = focused_window();
        if (!target) {
            const auto active = observed_active_.load();
            const auto foreground = observed_foreground_.load();
            DWORD foreground_process{};
            (void)GetWindowThreadProcessId(foreground, &foreground_process);
            char foreground_class[128]{};
            (void)GetClassNameA(foreground, foreground_class, static_cast<int>(std::size(foreground_class)));
            throw std::runtime_error(std::string(phase) + " has no keyboard focus target (active=" +
                (active == window ? "player" : active ? "other" : "none") + ", foreground=" +
                (foreground == window ? "player" : foreground ? "other" : "none") +
                ", foreground PID=" + std::to_string(foreground_process) + ", test PID=" +
                std::to_string(GetCurrentProcessId()) + ", class=" + foreground_class + ")" + detail::location_suffix(location));
        }
        expect("GUI keyboard target belongs to the player",
            static_cast<int>(target == window || IsChild(window, target)), 1);
        expect("GUI keyboard target stays enabled", IsWindowEnabled(target), TRUE);
        return target;
    }
    void focused_key(UINT key)
    {
        // Unlike key(), use the actual focus selected by the product, so losing
        // focus during EnableWindow cannot be hidden by an explicit HWND.
        const auto phase = std::string("focused key ") + std::to_string(key);
        const HWND target = require_keyboard_focus(phase.c_str());
        expect("focused GUI key is queued", PostMessageW(target, WM_KEYDOWN, key, 1), TRUE);
        expect("focused GUI key release is queued", PostMessageW(target, WM_KEYUP, key, (LPARAM{3} << 30) | 1), TRUE);
    }
    enum Modifier : unsigned { control = 1, shift = 2, alt = 4 };
    void modified_key(UINT key, unsigned modifiers,
        std::source_location location = std::source_location::current())
    {
        activate_for_keyboard();
        modifiers_.store(modifiers);
        probe_focus(3);
        if (!keyboard_state_ok_.load()) {
            throw std::runtime_error("Cannot prepare the GUI keyboard modifier state" + detail::location_suffix(location));
        }
        try {
            focused_key(key);
            drain_input();
        } catch (const std::exception& error) {
            probe_focus(4);
            throw std::runtime_error("modifier " + std::to_string(modifiers) + ": " + error.what());
        } catch (...) {
            probe_focus(4);
            throw;
        }
        probe_focus(4);
        if (!keyboard_state_ok_.load()) {
            throw std::runtime_error("Cannot restore the GUI keyboard modifier state" + detail::location_suffix(location));
        }
    }
    void mnemonic(wchar_t key)
    {
        activate_for_keyboard();
        modifiers_.store(Modifier::alt);
        probe_focus(3);
        const auto phase = std::string("mnemonic ") + static_cast<char>(key);
        expect("GUI mnemonic is queued", PostMessageW(require_keyboard_focus(phase.c_str()), WM_SYSCHAR, key, (LPARAM{1} << 29) | 1), TRUE);
        drain_input();
        probe_focus(4);
    }
    void quit(int code) const
    {
        expect("GUI thread quit is queued", PostThreadMessageW(thread_, WM_QUIT,
            static_cast<WPARAM>(code), 0), TRUE);
    }
    // Delivers `pulses` driver callbacks after each read of the state that
    // does not match yet.
    void state(std::wstring_view expected, GuiDriver* driver = nullptr, unsigned pulses = 1,
        std::source_location location = std::source_location::current())
    {
        wait_until([&] {
            if (text(ui::control_id::status, location) == expected) { return true; }
            for (unsigned pulse = 0; driver && pulse < pulses; ++pulse) { driver->pulse(); }
            return false;
        }, "GUI state transition", location);
    }
    void settled(std::wstring_view expected, GuiDriver* driver = nullptr, unsigned pulses = 1,
        std::source_location location = std::source_location::current())
    {
        state(expected, driver, pulses, location);
        wait_until([&] { return enabled(ui::control_id::play_pause); }, "GUI settled transport", location);
    }
    // A Pause must reach playback before the track ends, so waits for Paused
    // pulse once per read. Playing to EOF waits for no request, so each read
    // may deliver a whole run of callbacks.
    void play_to_end(GuiDriver& driver) { settled(L"Completed", &driver, GuiDriver::run_pulses); }
    void request_close() const { expect("GUI close is queued", PostMessageW(window, WM_CLOSE, 0, 0), TRUE); }
    void drain_input()
    {
        // A thread message follows earlier posted input. A test-only retrieval
        // hook acknowledges it without replacing the product WndProc.
        ResetEvent(barrier_.get());
        expect("GUI input barrier is queued", PostThreadMessageW(thread_, barrier_message,
            reinterpret_cast<WPARAM>(barrier_.get()), 0), TRUE);
        wait_for(barrier_.get(), "GUI input barrier");
    }
    int finish(std::source_location location = std::source_location::current())
    {
        if (done_.wait_for(hang_budget(5s)) != std::future_status::ready) {
            fail("GUI did not finish closing" + detail::location_suffix(location));
        }
        const int code = done_.get();
        expect("GUI test hooks release before their target thread exits", static_cast<int>(hook_cleanup_failed_), 0);
        expect("GUI window is destroyed after cleanup", IsWindow(window), FALSE);
        return code;
    }
    static LRESULT send(HWND hwnd, UINT message, WPARAM wparam = 0, LPARAM lparam = 0,
        std::source_location location = std::source_location::current())
    {
        DWORD_PTR result{};
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(hwnd, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result)) {
            const auto error = GetLastError();
            throw std::runtime_error("gui_send_failed message=" + std::to_string(message) + " win32=" +
                std::to_string(error) + detail::location_suffix(location));
        }
        return static_cast<LRESULT>(result);
    }
    // True while the window holds a shutdown block reason, which the product
    // sets only for its session-end wait. The query runs on the window's
    // thread, inside a sent message that the wait itself dispatches.
    bool blocking_shutdown()
    {
        (void)send(window, WM_NULL, shutdown_probe);
        return blocking_shutdown_.load();
    }
    // A logoff's WM_QUERYENDSESSION or WM_ENDSESSION. The product may wait up
    // to its own bound for the release, longer than send() allows.
    LRESULT session_end(UINT message, bool ending,
        std::source_location location = std::source_location::current()) const
    {
        DWORD_PTR result{};
        if (!SendMessageTimeoutW(window, message, ending, ENDSESSION_LOGOFF, SMTO_BLOCK,
            static_cast<UINT>(hang_budget(10s).count()), &result)) {
            throw std::runtime_error("GUI did not return from a session-end message" + detail::location_suffix(location));
        }
        return static_cast<LRESULT>(result);
    }
    // Reads the GUI again after each control update on its thread. The short
    // timeout covers conditions that no control update announces.
    template<class Predicate>
    void wait_until(Predicate ready, const char* operation,
        std::source_location location = std::source_location::current())
    {
        const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
        while (!ready()) {
            check_running(operation, deadline, location);
            (void)WaitForSingleObject(updated_.get(), 1);
        }
    }
private:
    DWORD thread_{};
    HHOOK hook_{}, update_hook_{};
    std::mutex hook_gate_;
    bool hook_thread_finished_{}, hook_cleanup_failed_{};
    wavplay::core::UniqueHandle barrier_{wavplay::core::create_event()};
    wavplay::core::UniqueHandle updated_{wavplay::core::create_event()};
    wavplay::core::UniqueHandle modal_entered_{wavplay::core::create_event()}, modal_release_{wavplay::core::create_event()};
    std::atomic<bool> modal_failed_{}, blocking_shutdown_{}, destroy_timed_out_{};
    std::array<std::atomic<unsigned>, ui::control_id::count> texts_set_{};
    std::atomic<unsigned> enables_{};
    std::atomic<unsigned> selection_queries_{};
    // The update hook has no context argument. Only one Gui exists at a time.
    static inline std::atomic<Gui*> updated_gui_{};
    std::atomic<unsigned> font_changes_{}, placements_{}, notifications_{};
    std::atomic<DWORD> ime_probe_thread_{};
    std::atomic<unsigned> ime_windows_{};
    // Written by the UI thread before done_ is ready.
    std::optional<ThreadSpeedState> exit_speed_;
    ThreadPriorities entry_priorities_, exit_priorities_;
    bool exit_query_failed_{};
    TestThread<int> done_;
    std::atomic<HWND> requested_focus_{}, observed_focus_{};
    std::atomic<HWND> observed_active_{}, observed_foreground_{};
    std::atomic<bool> activation_changed_{};
    std::atomic<unsigned> modifiers_{};
    std::atomic<bool> keyboard_state_ok_{};
    std::array<BYTE, 256> saved_keyboard_{};
    static constexpr UINT barrier_message = WM_APP + 127;
    static constexpr UINT focus_message = WM_APP + 128;
    static constexpr WPARAM shutdown_probe = 0x5E55; // WM_NULL's wParam for blocking_shutdown().
    void release_hooks() noexcept
    {
        // Serialize with constructor installation; the loop can finish before
        // the test sees the window, or while it installs the second hook.
        const std::lock_guard lock(hook_gate_);
        hook_thread_finished_ = true;
        for (auto* hook : {&hook_, &update_hook_}) {
            if (*hook && !UnhookWindowsHookEx(std::exchange(*hook, nullptr))) {
                hook_cleanup_failed_ = true;
            }
        }
        updated_gui_.store(nullptr);
    }
    void activate_for_keyboard()
    {
        // Another process can deactivate a visible test window. Establish the
        // input precondition before installing simulated modifier state, but
        // never repair missing child focus while the player is still active.
        probe_focus(0);
        if (observed_active_.load() != window) { reactivate(); }
    }
    void probe_focus(LPARAM operation)
    {
        ResetEvent(barrier_.get());
        expect("GUI focus probe is queued", PostThreadMessageW(thread_, focus_message,
            reinterpret_cast<WPARAM>(this), operation), TRUE);
        wait_for(barrier_.get(), "GUI focus probe");
    }
    void check_running(const char* operation, std::chrono::steady_clock::time_point deadline,
        std::source_location location = std::source_location::current())
    {
        if (done_.wait_for(0ms) == std::future_status::ready) {
            (void)done_.get();
            throw std::runtime_error(std::string(operation) + ": product message loop exited" + detail::location_suffix(location));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(operation) + ": deadline expired" + detail::location_suffix(location));
        }
    }
    // Waits for the input hook to acknowledge a probe it retrieved.
    void wait_for(HANDLE acknowledged, const char* operation)
    {
        const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
        while (WaitForSingleObject(acknowledged, 1) != WAIT_OBJECT_0) {
            check_running(operation, deadline);
        }
    }
    static LRESULT CALLBACK control_updated(int code, WPARAM current_thread, LPARAM value) noexcept
    {
        if (code == HC_ACTION) {
            const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(value);
            if (const auto gui = updated_gui_.load()) {
                if (message.message == WM_SETTEXT || message.message == WM_ENABLE) {
                    SetEvent(gui->updated_.get());
                }
                if (message.hwnd == gui->window && message.message == WM_NULL && message.wParam == shutdown_probe) {
                    DWORD length{};
                    gui->blocking_shutdown_.store(
                        ShutdownBlockReasonQuery(message.hwnd, nullptr, &length) != FALSE && length != 0);
                }
                if (GetParent(message.hwnd) == gui->window) {
                    if (message.message == WM_SETTEXT) {
                        const auto id = GetDlgCtrlID(message.hwnd);
                        if (id >= ui::control_id::first && id <= ui::control_id::last) {
                            ++gui->texts_set_[ui::control_id::slot(id)];
                        }
                    }
                    if (message.message == WM_ENABLE) { ++gui->enables_; }
                    if (message.message == LB_GETCURSEL && GetDlgCtrlID(message.hwnd) == ui::control_id::playlist) {
                        ++gui->selection_queries_;
                    }
                    if (message.message == WM_SETFONT) { ++gui->font_changes_; }
                    if (message.message == WM_WINDOWPOSCHANGED) {
                        const auto& placement = *reinterpret_cast<const WINDOWPOS*>(message.lParam);
                        // Native list scrollbars can recalculate their frame
                        // after a font change without moving/resizing the list.
                        if ((placement.flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
                            ++gui->placements_;
                        }
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, current_thread, value);
    }
    static LRESULT CALLBACK input_barrier(int code, WPARAM removed, LPARAM value) noexcept
    {
        if (code >= 0 && removed == PM_REMOVE) {
            const auto& message = *reinterpret_cast<const MSG*>(value);
            // The product posts its window only WM_APP-range state notifications.
            if (const auto gui = updated_gui_.load(); gui && message.hwnd && message.hwnd == gui->window &&
                message.message >= WM_APP && message.message <= 0xBFFF) {
                ++gui->notifications_;
            }
            if (!message.hwnd && message.message == barrier_message) {
                SetEvent(reinterpret_cast<HANDLE>(message.wParam));
            } else if (!message.hwnd && message.message == focus_message) {
                auto& gui = *reinterpret_cast<Gui*>(message.wParam);
                if (message.lParam == 1) {
                    SetFocus(gui.requested_focus_.load());
                } else if (message.lParam == 2) {
                    // Transition through a real top-level window: NULL is not
                    // a documented deactivation operand for SetActiveWindow.
                    const auto other = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                    if (other) {
                        ShowWindow(other, SW_SHOWNOACTIVATE);
                        SetActiveWindow(other);
                        gui.activation_changed_.store(GetActiveWindow() == other);
                        SetActiveWindow(gui.requested_focus_.load());
                        DestroyWindow(other);
                    }
                } else if (message.lParam == 3) {
                    gui.keyboard_state_ok_.store(false);
                    if (GetKeyboardState(gui.saved_keyboard_.data())) {
                        auto keys = gui.saved_keyboard_;
                        const auto modifiers = gui.modifiers_.load();
                        for (const auto key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT,
                            VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU}) {
                            keys[static_cast<std::size_t>(key)] &= 0x7f;
                        }
                        if (modifiers & Modifier::control) { keys[VK_CONTROL] |= 0x80; keys[VK_LCONTROL] |= 0x80; }
                        if (modifiers & Modifier::shift) { keys[VK_SHIFT] |= 0x80; keys[VK_LSHIFT] |= 0x80; }
                        if (modifiers & Modifier::alt) { keys[VK_MENU] |= 0x80; keys[VK_LMENU] |= 0x80; }
                        gui.keyboard_state_ok_.store(SetKeyboardState(keys.data()) != FALSE);
                    }
                } else if (message.lParam == 4) {
                    gui.keyboard_state_ok_.store(SetKeyboardState(gui.saved_keyboard_.data()) != FALSE);
                } else if (message.lParam == 5) {
                    // IMM checks thread ownership. Querying these HWNDs from
                    // the test thread could return null even with IME enabled.
                    gui.ime_probe_thread_.store(GetCurrentThreadId());
                    gui.ime_windows_.store(0);
                    const auto inspect = [&](HWND window) {
                        // HIMC can still exist when IME is disabled; the
                        // thread's default IME window is the observed resource.
                        if (ImmGetDefaultIMEWnd(window)) { ++gui.ime_windows_; }
                    };
                    inspect(gui.window);
                    for (auto child = GetWindow(gui.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                        inspect(child);
                    }
                } else if (message.lParam == 6) {
                    // Like move/size/menu loops, this dispatches window messages
                    // without waiting on the product's state-notification event.
                    SetEvent(gui.modal_entered_.get());
                    const auto release = gui.modal_release_.get();
                    for (;;) {
                        const auto wake = MsgWaitForMultipleObjectsEx(1, &release, 5000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                        if (wake == WAIT_OBJECT_0) { break; }
                        if (wake != WAIT_OBJECT_0 + 1) { gui.modal_failed_.store(true); break; }
                        MSG nested{};
                        for (unsigned count = 0; count < 32 && PeekMessageW(&nested, nullptr, 0, 0, PM_REMOVE); ++count) {
                            if (nested.message == WM_QUIT) {
                                PostQuitMessage(static_cast<int>(nested.wParam));
                                gui.modal_failed_.store(true);
                                break;
                            }
                            TranslateMessage(&nested);
                            DispatchMessageW(&nested);
                        }
                        if (gui.modal_failed_.load()) { break; }
                    }
                } else if (message.lParam == 7) {
                    // Wake only for newly posted messages until the product's
                    // notification is queued behind this one, bounded.
                    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
                    gui.destroy_timed_out_.store(true);
                    for (;;) {
                        MSG queued{};
                        if (PeekMessageW(&queued, gui.window, WM_APP, 0xBFFF, PM_NOREMOVE)) {
                            gui.destroy_timed_out_.store(false);
                            DestroyWindow(gui.window);
                            break;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= deadline || MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(
                            std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count()), QS_POSTMESSAGE, 0) ==
                            WAIT_FAILED) {
                            break;
                        }
                    }
                }
                gui.observed_focus_.store(GetFocus());
                gui.observed_active_.store(GetActiveWindow());
                gui.observed_foreground_.store(GetForegroundWindow());
                SetEvent(gui.barrier_.get());
            }
        }
        return CallNextHookEx(nullptr, code, removed, value);
    }
};

std::wstring source(TempFile& file, std::size_t frames = 1000)
{
    return file.write(wave({pcm16(2, 48000), pcm16_ramp(2, frames, 0x1234)}));
}

std::wstring playlist_file(TempFile& file, std::initializer_list<std::wstring_view> paths, bool excluded = false)
{
    std::wstring text;
    for (const auto path : paths) { text += path; text += L"\r\n"; }
    if (excluded) { text += L"relative.wav\r\n"; }
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes == 0 && !text.empty()) { throw std::runtime_error("Cannot encode the GUI playlist fixture"); }
    Bytes encoded(static_cast<std::size_t>(bytes));
    if (bytes && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(encoded.data()), bytes, nullptr, nullptr)) {
        throw std::runtime_error("Cannot encode the GUI playlist fixture");
    }
    return file.write(encoded);
}

void check_playlist_pcm(const std::vector<MockAsio::Block>& blocks, const Bytes& expected)
{
    expect_true("playlist PCM capture covers every source frame", blocks.size() * GuiDriver::frames * 4 >= expected.size());
    check_pcm16_capture(blocks, GuiDriver::frames, expected);
}

void test_message_failure_diagnostic()
{
    bool rejected{};
    try {
        Gui::send(nullptr, WM_NULL);
    } catch (const std::runtime_error& error) {
        rejected = true;
        const std::string_view message(error.what());
        constexpr std::string_view prefix = "gui_send_failed message=0 win32=";
        expect_true("GUI send failure identifies the message and Win32 error", message.starts_with(prefix));
        const auto location = message.find(" (player_window_tests.cpp:");
        expect_true("GUI send failure identifies its source location", location != std::string_view::npos);
        const auto code = message.substr(prefix.size(), location - prefix.size());
        expect_true("GUI send failure exposes only a bounded numeric Win32 code", !code.empty() && code.size() <= 10 &&
            std::ranges::all_of(code, [](char value) { return value >= '0' && value <= '9'; }));
        expect_true("GUI send failure does not include a full source path", message.find_first_of("\\/") == std::string_view::npos);
    }
    expect_true("an invalid window message is rejected", rejected);
}

void test_transport_availability()
{
    using wavplay::control::PlaybackSnapshot;
    using wavplay::control::PlaybackState;
    using wavplay::playback::PlaybackResult;
    PlaybackSnapshot snapshot;
    snapshot.playlist = std::make_shared<const std::vector<std::wstring>>(std::vector<std::wstring>{L"track.wav"});
    snapshot.accepted = snapshot.settled = 4;
    const auto check = [&](const char* name, std::uint64_t stop_receipt, ui::TransportAvailability expected) {
        const auto actual = ui::available(snapshot.command_state(), stop_receipt);
        expect_true(name, actual.toggle == expected.toggle && actual.stop == expected.stop && actual.select == expected.select);
    };

    // Independent UI expectations: selection may change during transport, but
    // initialization and final Close must not offer a new user operation.
    struct Case {
        PlaybackState state;
        ui::TransportAvailability expected;
    };
    constexpr Case cases[]{
        {PlaybackState::initializing, {false, false, false}},
        {PlaybackState::reinitializing, {false, false, false}},
        {PlaybackState::stopped, {true, false, true}},
        {PlaybackState::starting, {false, true, true}},
        {PlaybackState::playing, {true, true, true}},
        {PlaybackState::pausing, {false, true, true}},
        {PlaybackState::stopping, {false, false, true}},
        {PlaybackState::paused, {true, true, true}},
        {PlaybackState::completed, {true, true, true}},
        {PlaybackState::closing, {false, false, false}},
        {PlaybackState::closed, {false, false, false}}
    };
    for (const auto& item : cases) {
        snapshot.state = item.state;
        check("settled transport availability follows the state contract", 4, item.expected);
    }

    snapshot.state = PlaybackState::playing;
    snapshot.accepted = 5;
    check("an uncompleted Stop receipt suppresses another Stop", 5, {false, false, true});
    check("Stop may cancel a different pending transport command", 4, {false, true, true});
    snapshot.state = PlaybackState::stopped;
    check("Stop may cancel a Play that has not started", 4, {false, true, true});
    snapshot.settled = 5;
    check("a completed Stop at zero offers Play and selection", 5, {true, false, true});
    snapshot.position_frames = 1;
    check("Stop can rewind a nonzero stopped position", 5, {true, true, true});

    snapshot.position_frames = 0;
    snapshot.result = PlaybackResult::next_track_rejected;
    snapshot.track_error = std::make_exception_ptr(std::runtime_error("recoverable track refusal"));
    check("a track refusal leaves another selection and playback available", 5, {true, false, true});
    snapshot.state = PlaybackState::failed;
    snapshot.result = PlaybackResult::driver_error;
    check("an exception-free device fault blocks selection and transport", 5, {false, false, false});
    snapshot.result = PlaybackResult::stopped;
    snapshot.error = std::make_exception_ptr(std::runtime_error("controller failure"));
    check("a controller exception also blocks selection and transport", 5, {false, false, false});

    snapshot.state = PlaybackState::stopped;
    snapshot.error = snapshot.track_error = {};
    snapshot.playlist = std::make_shared<const std::vector<std::wstring>>();
    check("an empty loaded playlist offers no transport or selection", 5, {false, false, false});
    snapshot.playlist.reset();
    check("a missing playlist offers no transport or selection", 5, {false, false, false});
}

void test_playback_time_format()
{
    expect_true("unknown timing does not divide by zero or retain a previous duration",
        ui::playback_time_text(99, 100, 0) == L"Time: --:-- / --:--");
    for (const std::uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u}) {
        expect_true("all supported rates use complete integer seconds",
            ui::playback_time_text(static_cast<std::uint64_t>(rate) * 65 + rate - 1,
                static_cast<std::uint64_t>(rate) * 3661, rate) == L"Time: 1:05 / 1:01:01");
        expect_true("subsecond tracks remain valid zero-second durations",
            ui::playback_time_text(rate - 1, rate - 1, rate) == L"Time: 0:00 / 0:00");
    }
    expect_true("display positions cannot exceed the total",
        ui::playback_time_text(480000, 144000, 48000) == L"Time: 0:03 / 0:03");
    expect_true("64-bit lengths neither narrow nor overflow during conversion",
        ui::playback_time_text(std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(), 44100) ==
            L"Time: 116192643447:24:07 / 116192643447:24:07");
}

// The GUI text is built with integer arithmetic only. These compare it with the
// std::format output it replaced (tests may still use std::format) and pin the
// boundary cases independently.
void test_integer_text()
{
    const auto millis = [](std::uint32_t frames, std::uint32_t rate) {
        std::wstring text;
        ui::append_millis(text, frames, rate);
        return text;
    };
    for (const std::uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u}) {
        for (std::uint32_t frames = 1; frames <= 8192; ++frames) {
            if (millis(frames, rate) != std::format(L"{:.3f}", 1000.0 * frames / rate)) {
                fail("integer milliseconds differ from {:.3f} of the double at " + std::to_string(frames) +
                    " frames and " + std::to_string(rate) + " Hz");
            }
        }
    }
    // Exact ties at 48 kHz round to even; 44.1 kHz quotients are never ties.
    expect_true("3 frames at 48 kHz is the tie 0.0625", millis(3, 48000) == L"0.062");
    expect_true("9 frames at 48 kHz is the tie 0.1875", millis(9, 48000) == L"0.188");
    expect_true("15 frames at 48 kHz is the tie 0.3125", millis(15, 48000) == L"0.312");
    expect_true("33 frames at 48 kHz is the tie 0.6875", millis(33, 48000) == L"0.688");
    expect_true("whole milliseconds keep three zeros", millis(96, 48000) == L"2.000");
    expect_true("odd 44.1 kHz widths round to nearest", millis(1, 44100) == L"0.023" &&
        millis(7, 44100) == L"0.159" && millis(8, 44100) == L"0.181" && millis(512, 44100) == L"11.610");
    expect_true("96 kHz rounds up past the half", millis(16, 96000) == L"0.167");
    expect_true("the largest width and rate stay exact", millis(0xffffffffu, 1) == L"4294967295000.000" &&
        millis(0xffffffffu, 0xffffffffu) == L"1000.000");
    expect_true("a zero rate appends nothing instead of dividing by zero", millis(96, 0).empty());

    const auto signed_text = [](long value) {
        std::wstring text;
        ui::append_signed(text, value);
        return text;
    };
    expect_true("negative granularity keeps its sign", signed_text(-1) == L"-1" && signed_text(0) == L"0");
    expect_true("LONG extremes print in full", signed_text(std::numeric_limits<long>::min()) == L"-2147483648" &&
        signed_text(std::numeric_limits<long>::max()) == L"2147483647");
    std::wstring padded;
    ui::append_uint(padded, std::numeric_limits<std::uint64_t>::max());
    ui::append_uint(padded, 5, 25);
    expect_true("64-bit values print in full and padding stops at 20 digits",
        padded == L"18446744073709551615" L"00000000000000000005");

    // m:ss below an hour and h:mm:ss from there on, as std::format wrote them.
    const auto reference = [](std::uint64_t seconds) {
        return seconds >= 3600 ? std::format(L"{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60) :
            std::format(L"{}:{:02}", seconds / 60, seconds % 60);
    };
    for (std::uint64_t seconds = 0; seconds <= 3 * 3600 + 61; ++seconds) {
        if (ui::playback_time_text(seconds, seconds, 1) != L"Time: " + reference(seconds) + L" / " + reference(seconds)) {
            fail("integer time text differs from std::format at " + std::to_string(seconds) + " s");
        }
    }
    expect_true("time boundaries around one hour", ui::playback_time_text(3599, 3600, 1) == L"Time: 59:59 / 1:00:00");
    expect_true("time boundaries around one minute", ui::playback_time_text(59, 60, 1) == L"Time: 0:59 / 1:00");
    expect_true("hours are not limited to two digits",
        ui::playback_time_text(36000, 100 * 3600 + 59 * 60 + 59, 1) == L"Time: 10:00:00 / 100:59:59");

    // Byte-identical diagnostics at the LONG extremes, and the rate omitted
    // instead of rounded when a rate is not a verified positive integer.
    wavplay::control::PlaybackSnapshot snapshot;
    snapshot.settings_loaded = true;
    snapshot.requested_buffer_frames = std::numeric_limits<long>::max();
    snapshot.diagnostics.segments[0] = wavplay::playback::BufferSetup{.sample_rate = 48000.0, .buffer_frames = 3};
    snapshot.diagnostics.buffer_capabilities[0] = wavplay::playback::BufferCapabilities{.sample_rate = 48000.0,
        .minimum = std::numeric_limits<long>::min(), .maximum = std::numeric_limits<long>::max(), .preferred = -1,
        .granularity = -1};
    expect_true("buffer text keeps its wording with extreme values", ui::buffer_text(snapshot) ==
        L"Requested: 2147483647 frames\r\nLast allocation: 3 frames / 0.062 ms at 48000 Hz\r\n"
        L"Capabilities at 48000 Hz: min -2147483648; preferred -1\r\nMax 2147483647; granularity -1 (powers of two)");
    snapshot.state = wavplay::control::PlaybackState::playing;
    snapshot.diagnostics.segments[0]->sample_rate = 48000.5;
    snapshot.diagnostics.buffer_capabilities[0]->sample_rate = std::numeric_limits<double>::quiet_NaN();
    snapshot.diagnostics.buffer_capabilities[0]->granularity = 0;
    expect_true("an unverified rate is omitted rather than rounded", ui::buffer_text(snapshot) ==
        L"Requested: 2147483647 frames\r\nAllocated: 3 frames\r\n"
        L"Capabilities: min -2147483648; preferred -1\r\nMax 2147483647; granularity 0 (no extra step)");
}

// The production refresh reuses one scratch string across all these fields.
// Pin both the text and its backing storage through long/short/unknown values;
// this catches a value-returning temporary moved over the reserved buffer.
void test_reused_text_storage()
{
    std::wstring text(700, L'x');
    text.reserve(1024);
    const auto* const storage = text.data();
    const auto capacity = text.capacity();
    const auto check = [&](std::wstring_view expected) {
        expect_true("reused formatter replaces the whole previous text", text == expected);
        expect_true("reused formatter keeps the reserved backing storage", text.data() == storage);
        expect("reused formatter keeps its capacity", text.capacity(), capacity);
    };
    wavplay::control::PlaybackSnapshot snapshot;
    ui::buffer_text(text, snapshot);
    check(L"Awaiting configuration");
    ui::output_text(text, snapshot);
    check(L"Output: awaiting configuration");
    ui::device_text(text, snapshot);
    check(L"Device: awaiting configuration");
    snapshot.settings_loaded = true;
    ui::output_text(text, snapshot);
    check(L"Output: not configured");
    ui::device_text(text, snapshot);
    check(L"Device: NO DEVICE");
    if (FAILED(CLSIDFromString(selected_text, &snapshot.selected_device))) {
        throw std::runtime_error("Cannot parse the text fixture CLSID");
    }
    snapshot.device_available = true;
    snapshot.device_name.assign(600, L'\u97f3');
    ui::device_text(text, snapshot);
    check(L"Device: " + snapshot.device_name + L" " + selected_text);
    snapshot.device_name = L"Studio";
    snapshot.output_pair = 17;
    snapshot.state = wavplay::control::PlaybackState::playing;
    snapshot.diagnostics.segments[0] = wavplay::playback::BufferSetup{.sample_rate = 48000.0, .buffer_frames = 96};
    snapshot.excluded_count = 123;
    for (std::uint64_t second = 0; second < 60; ++second) {
        ui::playback_time_text(text, second, 3601, 1);
        check(std::format(L"Time: 0:{:02} / 1:00:01", second));
        ui::device_text(text, snapshot);
        check(L"Device: Studio " + std::wstring(selected_text));
        ui::output_text(text, snapshot);
        check(L"Output: 17 / 18 (configured)");
        ui::buffer_text(text, snapshot);
        check(L"Requested: driver preferred (BufferSize=0)\r\nAllocated: 96 frames / 2.000 ms at 48000 Hz\r\n"
            L"Capabilities: unavailable");
        ui::playlist_label_text(text, snapshot);
        check(L"&List: 0 tracks, 123 excluded");
    }
    ui::playback_time_text(text, 99, 100, 0);
    check(L"Time: --:-- / --:--");
    snapshot.settings_loaded = false;
    snapshot.result = wavplay::playback::PlaybackResult::driver_error;
    ui::buffer_text(text, snapshot);
    check(L"Configuration unavailable");
    ui::output_text(text, snapshot);
    check(L"Output: configuration unavailable");
}

void test_control_text_storage_reuse()
{
    ui::PlayerControls controls;
    struct ParentWindow {
        HWND handle{CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
        ~ParentWindow() { if (handle) { DestroyWindow(handle); } }
    } parent; // Destroy the child windows before controls releases their font.
    if (!parent.handle) { throw std::runtime_error("Cannot create the text storage fixture window"); }
    controls.create(parent.handle, {});
    const auto label = controls.child(ui::control_id::time);
    struct Probe {
        unsigned sets{};
        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
            UINT_PTR id, DWORD_PTR context) noexcept
        {
            auto& probe = *reinterpret_cast<Probe*>(context);
            if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, procedure, id); }
            if (message == WM_SETTEXT) {
                ++probe.sets;
            }
            return DefSubclassProc(window, message, wparam, lparam);
        }
    } probe;
    if (!SetWindowSubclass(label, Probe::procedure, 2, reinterpret_cast<DWORD_PTR>(&probe))) {
        throw std::runtime_error("Cannot observe text storage fixture updates");
    }
    struct RemoveProbe {
        HWND window;
        ~RemoveProbe() { RemoveWindowSubclass(window, Probe::procedure, 2); }
    } remove_probe{label}; // The subclass never outlives its stack context.
    const auto shown = [&] {
        std::array<wchar_t, 1024> value{};
        const auto size = GetWindowTextW(label, value.data(), static_cast<int>(value.size()));
        return std::wstring(value.data(), static_cast<std::size_t>(size));
    };
    std::wstring text(180, L'A');
    text.reserve(512);
    const auto* const first_storage = text.data();
    controls.text_reuse(ui::control_id::time, text);
    expect_true("successful update returns the previous cached text", text == L"Time: --:-- / --:--");
    expect_true("successful update displays the prepared text", shown() == std::wstring(180, L'A'));
    text.assign(180, L'A');
    text.reserve(512);
    const auto* const second_storage = text.data();
    controls.text_reuse(ui::control_id::time, text);
    expect("unchanged text sends no window update", probe.sets, 1);
    expect_true("unchanged text keeps its caller-owned storage", text.data() == second_storage);
    text.assign(190, L'B');
    controls.text_reuse(ui::control_id::time, text);
    expect("changed text updates once", probe.sets, 2);
    expect_true("successful commit returns the previous heap allocation", text.data() == first_storage);
    expect_true("successful commit returns the previous value", text == std::wstring(180, L'A'));
    text.assign(190, L'B');
    controls.show_text(ui::control_id::time, L"Temporary failure text");
    controls.text_reuse(ui::control_id::time, text);
    expect("failure-path text invalidates the cache even when the prepared text matches it", probe.sets, 4);
    expect_true("normal update restores the cached text after invalidation", shown() == std::wstring(190, L'B'));
    // Every changed time swaps the same two heap buffers. Combined with the
    // formatter's storage check above, steady refreshes need no string allocation.
    for (std::uint64_t second = 0; second < 60; ++second) {
        ui::playback_time_text(text, second, 60, 1);
        expect_true("time formatting reuses one of the two existing allocations",
            text.data() == first_storage || text.data() == second_storage);
        controls.text_reuse(ui::control_id::time, text);
        expect_true("time update returns the other existing allocation",
            text.data() == first_storage || text.data() == second_storage);
        expect_true("reused control text follows every elapsed second",
            shown() == std::format(L"Time: 0:{:02} / 1:00", second));
    }
    expect("each changed second sends exactly one update", probe.sets, 64);
}

// A 32-bit bitmap in a memory device context, to compare drawn pixels.
class Canvas {
public:
    static constexpr COLORREF untouched = RGB(255, 0, 255); // Neither label color.
    static constexpr std::uint32_t untouched_pixel = 0xff00ff; // The same color as pixels() returns it.

    Canvas(int width, int height) : width_(width), height_(height), dc_(CreateCompatibleDC(nullptr))
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // Top-down rows.
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap_ = dc_ ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
        if (!bitmap_) {
            if (dc_) { DeleteDC(dc_); }
            throw std::runtime_error("Cannot create the drawing fixture");
        }
        previous_ = SelectObject(dc_, bitmap_);
        fill();
    }
    ~Canvas()
    {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    [[nodiscard]] HDC dc() const noexcept { return dc_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    void fill()
    {
        const RECT all{0, 0, width_, height_};
        const auto brush = CreateSolidBrush(untouched);
        FillRect(dc_, &all, brush);
        DeleteObject(brush);
    }
    // 0x00RRGGBB per pixel, row by row from the top.
    [[nodiscard]] std::vector<std::uint32_t> pixels() const
    {
        GdiFlush();
        const auto* const first = static_cast<const std::uint32_t*>(bits_);
        std::vector<std::uint32_t> result(first, first + static_cast<std::size_t>(width_) * height_);
        for (auto& pixel : result) { pixel &= 0xffffff; }
        return result;
    }
    // The columns and rows that hold a pixel drawn since fill(): {left, top, right, bottom}, empty if none.
    [[nodiscard]] RECT touched() const
    {
        const auto all = pixels();
        RECT area{width_, height_, 0, 0};
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                if (all[static_cast<std::size_t>(y) * width_ + x] != untouched_pixel) {
                    area.left = std::min<LONG>(area.left, x);
                    area.top = std::min<LONG>(area.top, y);
                    area.right = std::max<LONG>(area.right, x + 1);
                    area.bottom = std::max<LONG>(area.bottom, y + 1);
                }
            }
        }
        return area.left < area.right ? area : RECT{};
    }

private:
    int width_, height_;
    HDC dc_;
    HBITMAP bitmap_{};
    void* bits_{};
    HGDIOBJ previous_{};
};

struct TestFont {
    HFONT handle;
    explicit TestFont(const LOGFONTW& description) : handle(CreateFontIndirectW(&description))
    {
        if (!handle) { throw std::runtime_error("Cannot create the label test font"); }
    }
    TestFont(const wchar_t* face, LONG height, bool italic = false, LONG weight = FW_NORMAL) : TestFont([&] {
        LOGFONTW description{};
        description.lfHeight = height;
        description.lfItalic = italic;
        description.lfWeight = weight;
        description.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(description.lfFaceName, face, _TRUNCATE);
        return description;
    }()) {}
    ~TestFont() { DeleteObject(handle); }
    TestFont(const TestFont&) = delete;
    TestFont& operator=(const TestFont&) = delete;
};

LOGFONTW message_font()
{
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, 96)) {
        throw std::runtime_error("Cannot read the system message font");
    }
    return metrics.lfMessageFont;
}

// Every text the time field shows in order, with the changes of length around
// them, an unrelated text, an unchanged text and the empty text.
constexpr std::array<std::wstring_view, 22> label_texts{L"Time: --:-- / --:--", L"Time: 0:00 / 0:04",
    L"Time: 0:01 / 0:04", L"Time: 0:02 / 0:04", L"Time: 0:09 / 0:04", L"Time: 0:10 / 0:04", L"Time: 0:11 / 0:04",
    L"Time: 0:59 / 59:00", L"Time: 1:00 / 59:00", L"Time: 1:11 / 59:00", L"Time: 9:59 / 59:00", L"Time: 10:00 / 59:00",
    L"Time: 59:59 / 1:00:01", L"Time: 1:00:00 / 1:00:01", L"Time: 1:00:00 / 1:00:01", L"Time: 0:00 / 0:03", L"",
    L"Time: 7:11 / 1:14", L"Time: 1:17 / 7:14", L"Closing", L"Time: 0:00 / 0:04", L"Time: 0:00 / 0:05"};

// redraw_changed_label() must leave the pixels a complete draw of the new text
// leaves, and must draw only the columns its contract names. The expected
// columns are measured here with the plain extent of a prefix.
void test_changed_label_redraw()
{
    const RECT bounds{7, 5, 407, 45}; // Inside the bitmap, so drawing outside the label shows.
    const TestFont system(message_font());
    const TestFont proportional(L"Georgia", -16);
    const TestFont fixed(L"Courier New", -13);
    const TestFont slanted(L"Segoe UI", -20, true, FW_BOLD);
    for (const auto* font : {&system, &proportional, &fixed, &slanted}) {
        Canvas complete(414, 50), redrawn(414, 50), changed(414, 50);
        const auto extent = [&](std::wstring_view text) {
            const auto original = SelectObject(changed.dc(), font->handle);
            SIZE size{};
            const bool measured = text.empty() ||
                GetTextExtentPoint32W(changed.dc(), text.data(), static_cast<int>(text.size()), &size);
            SelectObject(changed.dc(), original);
            expect_true("the label test measures its text", measured);
            return size.cx;
        };
        for (std::size_t index = 1; index < label_texts.size(); ++index) {
            const auto previous = label_texts[index - 1], next = label_texts[index];
            const auto step = "label step " + std::to_string(index);
            complete.fill();
            expect_true(step + " draws the new text completely",
                ui::draw_label(complete.dc(), font->handle, bounds, bounds, next));
            redrawn.fill();
            expect_true(step + " draws the previous text completely",
                ui::draw_label(redrawn.dc(), font->handle, bounds, bounds, previous));
            expect_true(step + " redraws the change",
                ui::redraw_changed_label(redrawn.dc(), font->handle, bounds, previous, next));
            expect_true(step + " leaves the pixels of a complete draw", redrawn.pixels() == complete.pixels());

            std::size_t first = 0;
            while (first < previous.size() && first < next.size() && previous[first] == next[first]) { ++first; }
            std::size_t tail = next.size();
            for (auto old = previous.size(); old > first && tail > first && previous[old - 1] == next[tail - 1]; --old) {
                --tail;
            }
            const LONG left = bounds.left + extent(next.substr(0, first ? first - 1 : 0));
            const LONG right = extent(previous) == extent(next) && tail < next.size() ?
                std::min(bounds.right, bounds.left + extent(next.substr(0, tail + 1))) : bounds.right;
            changed.fill();
            expect_true(step + " redraws on a surface of its own",
                ui::redraw_changed_label(changed.dc(), font->handle, bounds, previous, next));
            const auto touched = changed.touched();
            expect(step + " starts one character before the change", touched.left, left);
            expect(step + " ends one character after the change, or at the end of the label", touched.right, right);
            expect(step + " stays below the top of the label", touched.top, bounds.top);
            expect(step + " stays above the bottom of the label", touched.bottom, bounds.bottom);
        }
    }
    // The update of a new second in the system font: three characters of
    // nineteen, wherever the digits have one width.
    Canvas second(414, 50);
    const auto original = SelectObject(second.dc(), system.handle);
    SIZE one{}, two{}, whole{}, part{};
    const bool measured = GetTextExtentPoint32W(second.dc(), L"1", 1, &one) && GetTextExtentPoint32W(second.dc(), L"2", 1, &two) &&
        GetTextExtentPoint32W(second.dc(), L"Time: 0:02 / 0:04", 17, &whole) && GetTextExtentPoint32W(second.dc(), L"02 ", 3, &part);
    SelectObject(second.dc(), original);
    expect_true("the system font measures a time text", measured);
    if (one.cx == two.cx) {
        expect_true("a new second redraws its characters",
            ui::redraw_changed_label(second.dc(), system.handle, bounds, L"Time: 0:01 / 0:04", L"Time: 0:02 / 0:04"));
        const auto touched = second.touched();
        expect("a new second redraws the changed digit and its two neighbors", touched.right - touched.left, part.cx);
        expect_true("a new second redraws less than the text", touched.right - touched.left < whole.cx);
    }
    // Longer texts than the redraw measures are left to a complete repaint.
    const std::wstring longest(ui::label_capacity, L'8'), longer(ui::label_capacity + 1, L'8');
    second.fill();
    expect_true("the longest redrawn text is accepted",
        ui::redraw_changed_label(second.dc(), system.handle, bounds, L"8", longest));
    second.fill();
    expect_true("a longer new text is refused",
        !ui::redraw_changed_label(second.dc(), system.handle, bounds, L"8", longer));
    expect_true("a longer previous text is refused",
        !ui::redraw_changed_label(second.dc(), system.handle, bounds, longer, L"8"));
    expect_true("a refused redraw draws nothing", second.touched().right == 0);
    expect_true("a complete draw has no limit", ui::draw_label(second.dc(), system.handle, bounds, bounds, longer));
}

// The controls of PlayerControls in a top-level window of their own, laid out
// at a size that shows every control.
struct ControlsWindow {
    ui::PlayerControls controls;
    HWND parent{};

    explicit ControlsWindow(bool visible)
    {
        parent = CreateWindowExW(visible ? WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, L"STATIC", L"",
            WS_POPUP | WS_CLIPCHILDREN, 40, 40, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!parent) { throw std::runtime_error("Cannot create the controls fixture window"); }
        try {
            controls.create(parent, {});
            // The initial size of the product window at this monitor's scaling.
            const auto size = controls.window_size(true);
            if (!SetWindowPos(parent, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                throw std::runtime_error("Cannot size the controls fixture window");
            }
            controls.layout();
            if (visible) {
                ShowWindow(parent, SW_SHOWNOACTIVATE);
                update(parent);
            }
        } catch (...) {
            DestroyWindow(parent);
            throw;
        }
    }
    ~ControlsWindow() { DestroyWindow(parent); } // Before controls releases the font of its children.
    ControlsWindow(const ControlsWindow&) = delete;
    ControlsWindow& operator=(const ControlsWindow&) = delete;
    [[nodiscard]] HWND time() const noexcept { return controls.child(ui::control_id::time); }
    // Paints every invalid part of the window and its children now.
    static void update(HWND window) { RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN); }
    static void repaint(HWND window) { RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); }
    [[nodiscard]] static SIZE client(HWND window)
    {
        RECT area{};
        if (!GetClientRect(window, &area) || area.right <= 0 || area.bottom <= 0) {
            throw std::runtime_error("The label fixture has no client area");
        }
        return {area.right, area.bottom};
    }
    // What the control paints into a device context it is given.
    [[nodiscard]] static std::vector<std::uint32_t> printed(HWND window)
    {
        const auto size = client(window);
        Canvas canvas(size.cx, size.cy);
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.dc()), PRF_CLIENT | PRF_ERASEBKGND);
        return canvas.pixels();
    }
    // What the window shows.
    [[nodiscard]] static std::vector<std::uint32_t> shown(HWND window)
    {
        const auto size = client(window);
        Canvas canvas(size.cx, size.cy);
        const auto dc = GetDC(window);
        if (!dc) { throw std::runtime_error("Cannot read the label fixture window"); }
        GdiFlush();
        const bool copied = BitBlt(canvas.dc(), 0, 0, size.cx, size.cy, dc, 0, 0, SRCCOPY) != FALSE;
        ReleaseDC(window, dc);
        if (!copied) { throw std::runtime_error("Cannot copy the label fixture window"); }
        return canvas.pixels();
    }
};

// The time field paints itself. It must look like the standard label it
// replaces: compare it with one of the same size, font and text.
void test_time_field_matches_standard_label()
{
    ControlsWindow window(false);
    const auto time = window.time();
    const auto size = ControlsWindow::client(time);
    const auto reference = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, size.cx, size.cy, window.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!reference) { throw std::runtime_error("Cannot create the reference label"); }
    SendMessageW(reference, WM_SETFONT, static_cast<WPARAM>(SendMessageW(time, WM_GETFONT, 0, 0)), FALSE);
    expect_true("the time field starts as a label would", SetWindowTextW(reference, L"Time: --:-- / --:--") &&
        ControlsWindow::printed(time) == ControlsWindow::printed(reference));
    const auto blank = [&] {
        expect_true("the reference label clears", SetWindowTextW(reference, L"") != FALSE);
        return ControlsWindow::printed(reference);
    }();
    std::wstring text;
    for (const auto expected : label_texts) {
        if (expected.empty()) { continue; }
        text = expected;
        window.controls.text_reuse(ui::control_id::time, text);
        expect_true("the reference label takes the text", SetWindowTextW(reference, std::wstring(expected).c_str()) != FALSE);
        const auto painted = ControlsWindow::printed(time);
        expect_true("the time field paints its text as a standard label does", painted == ControlsWindow::printed(reference));
        expect_true("the painted time field shows a text", painted != blank);
    }
    // A failure-path text bypasses the cache. The field paints what the window holds.
    window.controls.show_text(ui::control_id::time, L"Closing");
    expect_true("the reference label takes the failure text", SetWindowTextW(reference, L"Closing") != FALSE);
    expect_true("the time field paints a failure-path text", ControlsWindow::printed(time) == ControlsWindow::printed(reference));
}

// On a visible window, a new time text draws its change at once and leaves no
// update region: the row is not invalidated, and the standard label's own
// repaint, which asks the parent for its colors, does not run. A text that
// follows a failure-path text has no known pixels to change, so the field
// repaints.
void test_time_field_redraws_on_screen()
{
    // Counts the color requests of standard repaints. Declared first: the
    // window removes this subclass while it is destroyed.
    struct Repaints {
        HWND time{}, reference{};
        unsigned by_time{}, by_reference{};
        static LRESULT CALLBACK procedure(HWND parent, UINT message, WPARAM wparam, LPARAM lparam,
            UINT_PTR id, DWORD_PTR context) noexcept
        {
            auto& self = *reinterpret_cast<Repaints*>(context);
            if (message == WM_NCDESTROY) { RemoveWindowSubclass(parent, procedure, id); }
            if (message == WM_CTLCOLORSTATIC) {
                const auto source = reinterpret_cast<HWND>(lparam);
                if (source == self.time) { ++self.by_time; }
                if (source == self.reference) { ++self.by_reference; }
            }
            return DefSubclassProc(parent, message, wparam, lparam);
        }
    } repaints;
    ControlsWindow window(true);
    const auto time = window.time();
    const auto size = ControlsWindow::client(time);
    repaints.time = time;
    repaints.reference = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, size.cx, size.cy, window.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!repaints.reference || !SetWindowSubclass(window.parent, Repaints::procedure, 3, reinterpret_cast<DWORD_PTR>(&repaints))) {
        throw std::runtime_error("Cannot observe the label repaints");
    }
    ControlsWindow::update(window.parent);
    repaints.by_time = repaints.by_reference = 0;
    expect_true("the reference label takes a text", SetWindowTextW(repaints.reference, L"Time: 0:01 / 0:04") != FALSE);
    expect_true("a standard label repaints itself for a new text", repaints.by_reference > 0);
    const auto pending = [&] { return GetUpdateRect(time, nullptr, FALSE) != FALSE; };
    expect_true("the shown time field is painted", !pending());
    std::wstring text;
    for (const auto expected : label_texts) {
        if (expected == L"Time: --:-- / --:--" || expected.empty()) { continue; } // The initial text; never shown later.
        text = expected;
        const bool same = [&] {
            std::array<wchar_t, 64> current{};
            return std::wstring_view(current.data(), static_cast<std::size_t>(
                GetWindowTextW(time, current.data(), static_cast<int>(current.size())))) == expected;
        }();
        const auto before = ControlsWindow::shown(time);
        window.controls.text_reuse(ui::control_id::time, text);
        expect_true("a new time text invalidates nothing", !pending());
        const auto drawn = ControlsWindow::shown(time);
        expect_true("a new time text changes the shown pixels", same || drawn != before);
        ControlsWindow::repaint(time);
        expect_true("the redrawn change equals a complete repaint", drawn == ControlsWindow::shown(time));
    }
    expect("the time field never runs the standard repaint for a new text", repaints.by_time, 0);
    window.controls.show_text(ui::control_id::time, L"Closing");
    expect_true("a failure-path text invalidates the field", pending());
    ControlsWindow::update(time);
    const auto failure = ControlsWindow::shown(time);
    ControlsWindow::repaint(time);
    expect_true("the painted failure-path text equals a complete repaint", failure == ControlsWindow::shown(time));
    text = L"Time: 0:07 / 0:09";
    window.controls.text_reuse(ui::control_id::time, text);
    expect_true("the text after a failure-path text invalidates the field", pending());
    ControlsWindow::update(time);
    const auto drawn = ControlsWindow::shown(time);
    ControlsWindow::repaint(time);
    expect_true("the field repainted after a failure-path text equals a complete repaint",
        drawn == ControlsWindow::shown(time));
    text = L"Time: 0:08 / 0:09";
    window.controls.text_reuse(ui::control_id::time, text);
    expect_true("later texts draw their change again", !pending());
}

void test_single_track_timing()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file, 48000 * 4), SW_HIDE, 1000ms);
    gui.settled(L"Stopped");
    expect_true("loaded single track shows its duration before playback",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    for (unsigned i = 0; i < 100; ++i) { driver.pulse(); }
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Paused", &driver);
    expect_true("pausing in source-independent start silence retains zero elapsed time",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    for (unsigned i = 0; i < 2100; ++i) { driver.pulse(); }
    gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04"; },
        "live single-track elapsed time excluding resumed start silence");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Paused", &driver);
    expect_true("Pause preserves the confirmed elapsed second",
        gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04");
    gui.click(ui::control_id::stop);
    gui.settled(L"Stopped");
    expect_true("Stop resets elapsed time and retains the duration",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.play_to_end(driver);
    expect_true("natural completion displays the complete duration",
        gui.text(ui::control_id::time) == L"Time: 0:04 / 0:04");
    gui.request_close();
    expect("timing display closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

void test_playlist_timing_selection_and_refusal()
{
    TempFile first, second, incompatible, playlist;
    const auto first_path = source(first, 48000 * 4);
    const std::wstring second_path = second.write(wave({pcm16(2, 44100), pcm16_ramp(2, 44100 * 2)}));
    const std::wstring rejected_path = incompatible.write(wave({pcm24(2, 44100), pcm24_ramp(2, 44100)}));
    GuiDriver driver;
    Gui gui(driver, {}, SW_HIDE, 0ms, {}, false, playlist_file(playlist, {first_path, second_path, rejected_path}));
    gui.settled(L"Stopped");
    expect_true("unselected playlist has no invented duration",
        gui.text(ui::control_id::time) == L"Time: --:-- / --:--");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    for (unsigned i = 0; i < 200; ++i) { driver.pulse(); }
    gui.select_track(1);
    gui.wait_until([&] { return gui.text(ui::control_id::file) == second_path; }, "selection during timed playback");
    expect_true("selection leaves the playing track's timing unchanged",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    for (unsigned i = 0; i < 1000; ++i) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04"; },
        "progress after a separate selection");
    expect_true("same-track progress preserves the user's separately selected path",
        gui.text(ui::control_id::file) == second_path);
    expect("same-track progress preserves the user's selected row", gui.selected_track(), 1);
    gui.click(ui::control_id::stop);
    gui.settled(L"Stopped");
    expect_true("Stop adopts the selected track's own rate and duration",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:02");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    gui.wait_until([&] { return gui.selected_track() == 2; }, "timed refused-track selection");
    expect_true("refused track begins at zero with its own duration",
        gui.text(ui::control_id::time) == L"Time: 0:00 / 0:01");
    gui.request_close();
    expect("timed track refusal stays recoverable", gui.finish(), 0);
    driver.check_closed();
}

void test_playlist_display_and_continuous_playback()
{
    TempFile first, second, last, playlist;
    // A real long Unicode path exercises the native list, horizontal extent,
    // copyable selection details and playback from the same displayed value.
    const auto directory = std::filesystem::path(L"\\\\?\\" + std::wstring(playlist.write({})) + L"-gui");
    struct RemoveDirectory {
        std::filesystem::path path;
        ~RemoveDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto nested = directory / std::wstring(120, L'x') / std::wstring(120, L'y');
    std::error_code directory_error;
    std::filesystem::create_directories(nested, directory_error);
    expect("long Unicode GUI fixture directory can be created", directory_error.value(), 0);
    const auto a = pcm16_ramp(2, 3, 100);
    const auto b = pcm16_ramp(2, 97, 1000);
    const auto c = pcm16_ramp(2, 11, 3000);
    const std::wstring first_path = first.write(wave({pcm16(2, 48000), a}));
    const std::wstring second_path = second.write(wave({pcm16(2, 48000), b}));
    const auto last_path = (nested / L"最後の曲 日本語.wav").wstring();
    expect("long Unicode GUI audio can be created", CopyFileW(last.write(wave({pcm16(2, 48000), c})),
        last_path.c_str(), TRUE), TRUE);
    expect_true("GUI fixture exceeds the legacy path limit", last_path.size() > MAX_PATH);
    GuiDriver driver;
    Gui gui(driver, {}, SW_HIDE, 0ms, {}, false,
        playlist_file(playlist, {first_path, second_path, first_path, last_path}, true));
    // Play starts disabled and is enabled after the first list and its
    // horizontal extent have been rendered. Wait for that transition before
    // the first synchronous status read on the UI thread.
    gui.wait_until([&] { return gui.enabled(ui::control_id::play_pause); }, "initial playlist display");
    gui.state(L"Stopped");
    const auto list = GetDlgItem(gui.window, ui::control_id::playlist);
    expect("GUI shows every valid playlist item", Gui::send(list, LB_GETCOUNT), 4);
    expect("playlist starts without selecting a track", gui.selected_track(), LB_ERR);
    RECT list_client{};
    expect("playlist client width can be read", GetClientRect(list, &list_client), TRUE);
    expect_true("long full paths have a horizontal scroll range",
        Gui::send(list, LB_GETHORIZONTALEXTENT) > list_client.right);
    expect_true("GUI reports excluded paths independently of accepted tracks",
        gui.text(ui::control_id::playlist_label) == L"&List: 4 tracks, 1 excluded");
    std::size_t index{};
    for (const auto& path : {first_path, second_path, first_path, last_path}) {
        const auto length = Gui::send(list, LB_GETTEXTLEN, index);
        std::wstring displayed(static_cast<std::size_t>(length) + 1, L'\0');
        expect("list returns the full path", Gui::send(list, LB_GETTEXT, index,
            reinterpret_cast<LPARAM>(displayed.data())), length);
        displayed.resize(static_cast<std::size_t>(length));
        expect_true("playlist order and duplicate full paths are preserved", displayed == path);
        ++index;
    }
    RECT before{}, after{}, window{};
    expect("playlist bounds can be read", GetWindowRect(list, &before), TRUE);
    expect("playlist window bounds can be read", GetWindowRect(gui.window, &window), TRUE);
    expect("playlist window can grow", SetWindowPos(gui.window, nullptr, 0, 0,
        window.right - window.left + 100, window.bottom - window.top + 100,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), TRUE);
    expect("grown playlist bounds can be read", GetWindowRect(list, &after), TRUE);
    expect_true("list follows both dimensions of window resizing", after.right - after.left > before.right - before.left &&
        after.bottom - after.top > before.bottom - before.top);
    (void)playlist_file(playlist, {last_path}); // Startup snapshot is immutable.
    gui.key(VK_SPACE, ui::control_id::playlist);
    gui.settled(L"Playing");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    gui.wait_until([&] { return gui.selected_track() == 3; }, "playlist final selection");
    expect("whole same-rate playlist uses one ASIO start", driver.starts, 1);
    expect("whole same-rate playlist uses one capture", driver.captures.size(), 1);
    Bytes expected = a.payload;
    expected.append_range(b.payload);
    expected.append_range(a.payload);
    expected.append_range(c.payload);
    check_playlist_pcm(driver.captures.front(), expected);
    expect("editing the text file does not change the current list", Gui::send(list, LB_GETCOUNT), 4);
    expect_true("final selected full path is available to copy", gui.text(ui::control_id::file) == last_path);
    expect_true("Stop is available at a playlist EOF", gui.enabled(ui::control_id::stop));
    gui.key(VK_ESCAPE, ui::control_id::playlist);
    // EOF is already Stopped. Observe Escape being processed before waiting
    // for its receipt to settle; otherwise the old Stopped/Play-enabled UI can
    // satisfy the wait and the next Space can arrive while Stop is pending.
    gui.drain_input();
    gui.settled(L"Stopped");
    expect("EOF Stop retains the selected final track", gui.selected_track(), 3);
    expect_true("EOF Stop clears the retained nonzero position", !gui.enabled(ui::control_id::stop));
    gui.key(VK_SPACE, ui::control_id::playlist);
    gui.settled(L"Playing");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    expect("Play after EOF Stop starts one new run", driver.starts, 2);
    expect("Play after EOF Stop records the new completed run", driver.captures.size(), 2);
    check_playlist_pcm(driver.captures.at(1), c.payload);
    gui.request_close();
    expect("playlist completion closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

void test_playlist_selection_and_transport()
{
    TempFile first, second, last, playlist;
    const auto first_path = source(first, 4099);
    const auto b = pcm16_ramp(2, 4099, 0x4000);
    const auto c = pcm16_ramp(2, 4099, 0x6000);
    const std::wstring second_path = second.write(wave({pcm16(2, 48000), b}));
    const std::wstring last_path = last.write(wave({pcm16(2, 48000), c}));
    GuiDriver driver;
    Gui gui(driver, {}, SW_SHOWNORMAL, 0ms, {}, false, playlist_file(playlist, {first_path, second_path, last_path}));
    gui.settled(L"Stopped");
    gui.select_track(1);
    gui.wait_until([&] { return gui.text(ui::control_id::file) == second_path; }, "clicked playlist selection");
    expect("a click selects without starting audio", driver.starts, 0);
    gui.mnemonic(L'l');
    expect("Alt+L focuses the playlist", GetDlgCtrlID(gui.focused_window()), ui::control_id::playlist);
    gui.focused_key(VK_RETURN);
    gui.settled(L"Playing");
    gui.key(VK_SPACE, ui::control_id::playlist);
    gui.settled(L"Paused", &driver);
    expect("Pause retains the selected playlist row", gui.selected_track(), 1);
    gui.key(VK_ESCAPE, ui::control_id::playlist);
    gui.settled(L"Stopped");
    expect("Stop retains the selected playlist row", gui.selected_track(), 1);
    gui.key(VK_SPACE, ui::control_id::playlist);
    gui.settled(L"Playing");
    gui.select_track(2);
    gui.wait_until([&] { return gui.text(ui::control_id::file) == last_path; }, "selection during playback");
    expect("single click during playback does not restart ASIO", driver.starts, 2);
    gui.select_track(2, true);
    gui.settled(L"Playing");
    expect("double-click starts the selected track", driver.starts, 3);
    gui.select_track(0);
    gui.wait_until([&] { return gui.text(ui::control_id::file) == first_path; }, "manual selection during final track");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    expect("EOF selects the completed track instead of retaining a different highlighted row", gui.selected_track(), 2);
    expect("selection produces only explicitly requested starts", driver.captures.size(), 3);
    expect_true("Stop followed by Play restarts at the selected source's first frame",
        driver.captures.at(0).front() == driver.captures.at(1).front());
    check_playlist_pcm(driver.captures.at(2), c.payload);
    gui.request_close();
    expect("playlist selection transport closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

void test_playlist_next_refusal()
{
    TempFile first, incompatible, last, playlist;
    const auto a = pcm16_ramp(2, 997, 0x1200);
    const auto c = pcm16_ramp(2, 53, 0x5000);
    const std::wstring first_path = first.write(wave({pcm16(2, 48000), a}));
    const std::wstring rejected_path = incompatible.write(wave({pcm24(2, 48000), pcm24_ramp(2, 127)}));
    const std::wstring last_path = last.write(wave({pcm16(2, 48000), c}));
    GuiDriver driver;
    Gui gui(driver, {}, SW_HIDE, 0ms, {}, false, playlist_file(playlist, {first_path, rejected_path, last_path}));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    gui.wait_until([&] { return gui.selected_track() == 1; }, "refused next track selection");
    expect_true("next-track refusal shows its path and a recoverable reason",
        gui.text(ui::control_id::file) == rejected_path && !gui.text(ui::control_id::error).empty());
    expect("refusal does not skip ahead to another playable row", driver.starts, 1);
    check_playlist_pcm(driver.captures.at(0), a.payload);
    gui.select_track(2, true);
    gui.settled(L"Playing");
    gui.settled(L"Stopped", &driver, GuiDriver::run_pulses);
    expect_true("explicitly selecting a playable track clears the refusal", gui.text(ui::control_id::error).empty());
    check_playlist_pcm(driver.captures.at(1), c.payload);
    gui.request_close();
    expect("a handled next-track refusal is not a fatal GUI error", gui.finish(), 0);
    driver.check_closed();
}

void test_playlist_empty_and_invalid_utf8()
{
    for (const bool invalid : {false, true}) {
        TempFile playlist;
        GuiDriver driver;
        const auto path = playlist.write(invalid ? Bytes{'#', 'x', '\n', 0xc3, 0x28} : Bytes{});
        Gui gui(driver, {}, SW_HIDE, 0ms, {}, false, path);
        gui.state(invalid ? L"Error" : L"Stopped");
        expect("an empty or invalid playlist publishes no rows",
            Gui::send(GetDlgItem(gui.window, ui::control_id::playlist), LB_GETCOUNT), 0);
        expect_true("an empty or invalid playlist disables Play", !gui.enabled(ui::control_id::play_pause));
        if (invalid) { expect_true("invalid UTF-8 reports the load failure", !gui.text(ui::control_id::error).empty()); }
        gui.request_close();
        expect("playlist load result has the expected exit status", gui.finish(), invalid ? 1 : 0);
        driver.check_closed();
    }
}

void test_arguments()
{
    const std::wstring_view valid[]{L"--wav", L"C:\\music with spaces\\日本語.wav"};
    const auto options = ui::parse_player_options(valid);
    expect("GUI argument parser retains Unicode and spaces", static_cast<int>(options.path == valid[1]), 1);
    expect("GUI command line leaves device selection to settings.ini", IsEqualCLSID(options.driver, CLSID{}), TRUE);
    const auto reject = [](std::initializer_list<std::wstring_view> args) {
        expect_throw<std::invalid_argument>("invalid GUI command line is rejected", [&] {
            (void)ui::parse_player_options(std::span(args.begin(), args.size()));
        });
    };
    expect_true("no arguments select the default playlist", ui::parse_player_options({}).path.empty());
    expect_true("playlist and settings resolve beside the same executable",
        std::filesystem::path(ui::executable_playlist_path()).parent_path() ==
        std::filesystem::path(ui::executable_settings_path()).parent_path() &&
        std::filesystem::path(ui::executable_playlist_path()).filename() == L"playlist.txt");
    reject({L"--driver", selected_text});
    reject({L"--wav"});
    reject({L"--wav", L""});
    reject({L"--wav", L"file.wav", L"--wav", L"second.wav"});
    reject({L"--unknown", L"file.wav"});
    reject({L"--driver", selected_text, L"--wav", L"file.wav"});
    reject({L"--driver", selected_text, L"--wav"});
    reject({L"--driver", selected_text, L"--wav", L""});
    reject({L"--driver", L"not-a-clsid", L"--wav", L"file.wav"});
    reject({L"--driver", selected_text, L"--wav", L"file.wav", L"--wav", L"second.wav"});
    reject({L"--driver", selected_text, L"--driver", selected_text, L"--wav", L"file.wav"});
    reject({L"--driver", selected_text, L"--wav", L"file.wav", L"--unknown"});
}

// The product takes its arguments from the CRT's wide argv, which differs from
// CommandLineToArgvW in quoting edge cases. A child of this executable, whose
// wmain argv comes from the same CRT parser, receives each raw command line and
// reports what parse_player_options made of it (run_player_options_child).
void test_command_line_quoting()
{
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) { wavplay::core::throw_last_error("GetModuleFileNameW(argv test)"); }
    executable.resize(length);
    struct Case {
        std::wstring_view command_line;
        std::optional<std::wstring_view> path; // nullopt: rejected as an argument error.
    };
    const Case cases[]{
        {LR"(--wav "C:\music with spaces\日本語.wav")", LR"(C:\music with spaces\日本語.wav)"},
        {LR"(--wav C:\music" with "spaces\track.wav)", LR"(C:\music with spaces\track.wav)"},
        {LR"(--wav "\\server\share\a b.wav")", LR"(\\server\share\a b.wav)"},
        {LR"(--wav "C:\trailing backslash\\")", LR"(C:\trailing backslash\)"},
        {LR"(--wav "C:\escaped \"quotes\".wav")", LR"(C:\escaped "quotes".wav)"},
        {LR"(--wav "C:\doubled ""quotes"" stay quoted.wav")", LR"(C:\doubled "quotes" stay quoted.wav)"},
        {L"\t--wav\t\"C:\\tab\tand  spaces.wav\"  ", L"C:\\tab\tand  spaces.wav"},
        {LR"(--wav "")", std::nullopt},
        {LR"(--wav "C:\a b.wav" extra)", std::nullopt},
        {LR"(--wav)", std::nullopt},
    };
    for (std::size_t index = 0; index < std::size(cases); ++index) {
        const auto& item = cases[index];
        std::wstring command = L'"' + executable + L"\" --player-options-child ";
        if (item.path) {
            for (const auto unit : *item.path) { command += std::format(L"{:04x}", static_cast<unsigned>(unit)); }
        } else {
            command += L'-';
        }
        command += L' ';
        command += item.command_line;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &created)) {
            wavplay::core::throw_last_error("CreateProcessW(argv test)");
        }
        const wavplay::core::UniqueHandle process(created.hProcess), thread(created.hThread);
        if (WaitForSingleObject(process.get(), static_cast<DWORD>(hang_budget(5s).count())) != WAIT_OBJECT_0) {
            (void)TerminateProcess(process.get(), 2);
            fail("argv child did not exit in case " + std::to_string(index));
        }
        DWORD code{};
        expect_true("argv child exit code can be read", GetExitCodeProcess(process.get(), &code) != FALSE);
        expect("CRT argv yields the expected --wav path, or an argument error, in case " + std::to_string(index),
            code, item.path ? DWORD{0} : DWORD{3});
    }
}

void test_error_messages()
{
    expect("empty exception has no diagnostic", static_cast<int>(ui::exception_message({}).empty()), 1);
    const auto unicode = ui::exception_message(std::make_exception_ptr(std::runtime_error("日本語のエラー")));
    expect("UTF-8 exception text remains Unicode", static_cast<int>(unicode == L"日本語のエラー"), 1);
    // Text that is not UTF-8 (it starts with a continuation byte) falls back to
    // the ANSI code page, which decodes these bytes.
    constexpr char narrow[] = "\x93\xfa\x96\x7b";
    wchar_t ansi[8]{};
    const auto decoded = MultiByteToWideChar(CP_ACP, 0, narrow, 4, ansi, static_cast<int>(std::size(ansi)));
    if (!decoded) { throw std::runtime_error("Cannot decode an independent ANSI expectation"); }
    const auto fallback = ui::exception_message(std::make_exception_ptr(std::runtime_error(narrow)));
    expect_true("invalid UTF-8 exception text is decoded with the ANSI code page",
        fallback == std::wstring_view(ansi, static_cast<std::size_t>(decoded)) &&
        fallback != L"Cannot decode the application error message.");
    wchar_t system_text[1024]{};
    const auto count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        ERROR_FILE_NOT_FOUND, 0, system_text, static_cast<DWORD>(std::size(system_text)), nullptr);
    if (!count) { throw std::runtime_error("Cannot obtain an independent Windows error description"); }
    std::wstring expected(system_text, count);
    while (!expected.empty() && (expected.back() == L'\r' || expected.back() == L'\n' || expected.back() == L' ')) {
        expected.pop_back();
    }
    const auto error = ui::exception_message(std::make_exception_ptr(
        std::system_error(ERROR_FILE_NOT_FOUND, std::system_category(), "Open WAV")));
    expect("Windows error description uses the wide system message",
        static_cast<int>(error.find(expected) != std::wstring::npos), 1);
    const auto unknown = ui::exception_message(std::make_exception_ptr(17));
    expect("non-standard exception remains understandable",
        static_cast<int>(unknown == L"Unexpected application failure."), 1);

    // A cause attached by std::throw_with_nested follows on its own line and is
    // formatted like the outer error, so a system_error keeps the wide text.
    const auto nest = [](const std::exception_ptr& cause, const std::string& outer) {
        std::exception_ptr chain;
        try {
            std::rethrow_exception(cause);
        } catch (...) {
            try {
                std::throw_with_nested(std::runtime_error(outer)); // Nests the handled `cause`.
            } catch (...) {
                chain = std::current_exception();
            }
        }
        return chain;
    };
    const std::wstring cleanup = L"ASIO disposeBuffers failed during cleanup: ASE_HWMalfunction (-999)";
    const auto chained = ui::exception_message(nest(std::make_exception_ptr(
        std::system_error(ERROR_FILE_NOT_FOUND, std::system_category(), "Open WAV")),
        "ASIO disposeBuffers failed during cleanup: ASE_HWMalfunction (-999)"));
    expect_true("a nested cause follows the outer error",
        chained.starts_with(cleanup + L"\r\nCause: Open WAV: ") && chained.ends_with(expected));
    auto deep = std::make_exception_ptr(std::runtime_error("level 5"));
    for (int level = 4; level >= 0; --level) {
        deep = nest(deep, "level " + std::to_string(level));
    }
    expect_true("nested causes are bounded",
        ui::exception_message(deep) == L"level 0\r\nCause: level 1\r\nCause: level 2\r\nCause: level 3");

    // The error field. A read error keeps its fixed text (the failure and the
    // restart it needs) and shows the producer's exception below it as a cause;
    // any other exception, and a refusal's reason, is shown alone.
    using wavplay::playback::PlaybackResult;
    wavplay::control::PlaybackSnapshot field;
    field.result = PlaybackResult::read_error;
    expect_true("a read error without a cause shows its fixed text",
        ui::error_text(field) == L"Cannot read WAV data. Restart the application to retry.");
    field.error = std::make_exception_ptr(std::system_error(ERROR_FILE_NOT_FOUND, std::system_category(), "ReadFile"));
    const auto read = ui::error_text(field);
    expect_true("a read error's cause follows its fixed text",
        read.starts_with(L"Cannot read WAV data. Restart the application to retry.\r\nCause: ReadFile: ") &&
        read.ends_with(expected));
    field.result = PlaybackResult::driver_error;
    field.error = std::make_exception_ptr(std::runtime_error("ASIO start failed: ASE_HWMalfunction (-999)"));
    expect_true("another failure shows its exception alone",
        ui::error_text(field) == L"ASIO start failed: ASE_HWMalfunction (-999)");
    field.error = {};
    field.result = PlaybackResult::next_track_rejected;
    expect_true("a refusal without a reason shows its fixed text", ui::error_text(field) == L"The next track was rejected.");
    field.track_error = std::make_exception_ptr(std::invalid_argument("Unsupported ASIO buffer size: requested=8"));
    expect_true("a refusal shows its own reason", ui::error_text(field) == L"Unsupported ASIO buffer size: requested=8");
}

void test_tab_enter_mnemonics_and_modifiers()
{
    TempFile file;
    const auto path = source(file);
    GuiDriver driver;
    Gui gui(driver, path, SW_SHOWNORMAL);
    const auto settled = [&](const char* operation, std::wstring_view state, GuiDriver* clock = nullptr) {
        try { gui.settled(state, clock); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(operation) + ": " + error.what()); }
    };
    gui.settled(L"Stopped");
    // This test exercises keyboard navigation after activation. Merely showing
    // a background test process does not guarantee Windows activates its GUI.
    // Use the real top-level transition; let the product choose the child focus.
    gui.reactivate();
    (void)gui.require_keyboard_focus("keyboard navigation activation");
    expect("file control contains only the actual path", static_cast<int>(gui.text(ui::control_id::file) == path), 1);
    for (const auto id : {ui::control_id::buffer, ui::control_id::file, ui::control_id::error}) {
        const auto style = GetWindowLongPtrW(GetDlgItem(gui.window, id), GWL_STYLE);
        expect("read-only text can be reached with Tab", static_cast<int>((style & WS_TABSTOP) != 0), 1);
        expect("read-only text cannot be edited", static_cast<int>((style & ES_READONLY) != 0), 1);
    }
    gui.mnemonic(L'b');
    expect("Alt+B reaches the buffer diagnostics", GetDlgCtrlID(gui.focused_window()), ui::control_id::buffer);
    gui.focused_key(VK_TAB);
    gui.drain_input();
    expect("Tab leaves the readonly buffer and reaches the list", GetDlgCtrlID(gui.focused_window()), ui::control_id::playlist);
    gui.focused_key(VK_TAB);
    gui.drain_input();
    expect("Tab leaves the list and reaches the copyable path", GetDlgCtrlID(gui.focused_window()), ui::control_id::file);
    gui.focus(ui::control_id::file);
    gui.focused_key(VK_TAB);
    gui.drain_input();
    expect("Tab reaches Play from the file", GetDlgCtrlID(gui.focused_window()), ui::control_id::play_pause);
    gui.modified_key(VK_TAB, Gui::shift);
    expect("Shift+Tab returns to the file", static_cast<int>(gui.focused_window() ==
        GetDlgItem(gui.window, ui::control_id::file)), 1);
    gui.mnemonic(L'p');
    settled("Alt+P from the file", L"Playing");
    for (const auto modifier : {Gui::control, Gui::shift, Gui::alt}) {
        gui.modified_key(VK_SPACE, modifier);
        gui.modified_key(VK_ESCAPE, modifier);
        gui.state(L"Playing");
    }
    gui.focus(ui::control_id::play_pause);
    gui.focused_key(VK_TAB);
    gui.drain_input();
    expect("Tab reaches Stop while playing", static_cast<int>(gui.focused_window() ==
        GetDlgItem(gui.window, ui::control_id::stop)), 1);
    gui.focused_key(VK_RETURN);
    settled("Enter on Stop", L"Stopped");
    gui.focus(ui::control_id::play_pause);
    gui.focused_key(VK_RETURN);
    settled("Enter on Play", L"Playing");
    gui.mnemonic(L'p');
    settled("Alt+P to pause", L"Paused", &driver);
    gui.mnemonic(L's');
    settled("Alt+S to stop", L"Stopped");
    gui.focus(ui::control_id::play_pause);
    gui.focused_key(VK_TAB);
    gui.drain_input();
    expect("Tab skips disabled Stop and reaches the error text", static_cast<int>(gui.focused_window() ==
        GetDlgItem(gui.window, ui::control_id::error)), 1);
    gui.request_close();
    expect("keyboard navigation exits successfully", gui.finish(), 0);
    expect("mnemonics and Enter start only the requested runs", driver.starts, 2);
    driver.check_closed();
}

void test_command_sources()
{
    TempFile file;
    GuiDriver driver;
    CallGate start;
    driver.on_call = [&](MockAsio::Call call) { if (call == MockAsio::Call::start) { start.block(); } };
    Gui gui(driver, source(file));
    ReleaseGate release{start};
    gui.settled(L"Stopped");
    const auto file_control = GetDlgItem(gui.window, ui::control_id::file);
    (void)Gui::send(gui.window, WM_COMMAND, MAKEWPARAM(ui::control_id::play_pause, BN_CLICKED));
    (void)Gui::send(gui.window, WM_COMMAND, MAKEWPARAM(ui::control_id::play_pause, BN_CLICKED),
        reinterpret_cast<LPARAM>(file_control));
    (void)Gui::send(gui.window, WM_COMMAND, MAKEWPARAM(ui::control_id::play_pause, EN_SETFOCUS),
        reinterpret_cast<LPARAM>(GetDlgItem(gui.window, ui::control_id::play_pause)));
    gui.click(ui::control_id::stop); // Disabled controls cannot issue a command.
    gui.state(L"Stopped");
    expect("unrelated notifications never enqueue playback", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 1);
    (void)Gui::send(GetDlgItem(gui.window, ui::control_id::play_pause), BM_CLICK);
    start.wait();
    start.open();
    gui.settled(L"Playing");
    (void)Gui::send(gui.window, WM_COMMAND, MAKEWPARAM(ui::control_id::stop, BN_CLICKED),
        reinterpret_cast<LPARAM>(file_control));
    gui.state(L"Playing");
    gui.request_close();
    expect("valid button notification exits successfully", gui.finish(), 0);
    expect("only the real button starts playback", driver.starts, 1);
    driver.check_closed();
}

void check_control_bounds(HWND window)
{
    RECT client{};
    if (!GetClientRect(window, &client)) { throw std::runtime_error("Cannot query the player client area"); }
    for (const auto id : {ui::control_id::device, ui::control_id::file, ui::control_id::play_pause,
        ui::control_id::stop, ui::control_id::status, ui::control_id::pending, ui::control_id::error,
        ui::control_id::file_label, ui::control_id::error_label, ui::control_id::buffer,
        ui::control_id::output, ui::control_id::buffer_label, ui::control_id::playlist, ui::control_id::playlist_label,
        ui::control_id::time}) {
        const auto child = GetDlgItem(window, id);
        if (!child) { throw std::runtime_error("A required player control is missing"); }
        RECT bounds{};
        if (!GetWindowRect(child, &bounds)) { throw std::runtime_error("Cannot query a player control rectangle"); }
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
        expect("control has positive dimensions", static_cast<int>(bounds.right > bounds.left && bounds.bottom > bounds.top), 1);
        expect("control fits inside the client area", static_cast<int>(bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= client.right && bounds.bottom <= client.bottom), 1);
    }
}

void test_dpi_settings_and_resources()
{
    TempFile file;
    const auto path = source(file);
    const auto current = GetCurrentProcess();
    DWORD baseline_gdi{}, baseline_user{};
    // The first pass warms up registered classes and Windows' shared caches.
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        GuiDriver driver;
        {
            Gui gui(driver, path);
            gui.settled(L"Stopped");
            const auto actual_dpi = GetDpiForWindow(gui.window);
            for (const UINT dpi : {actual_dpi, 144u, 192u, actual_dpi}) {
                RECT bounds{};
                if (!GetWindowRect(gui.window, &bounds)) { throw std::runtime_error("Cannot query the player rectangle"); }
                bounds.right = bounds.left + MulDiv(760, static_cast<int>(dpi), 96);
                bounds.bottom = bounds.top + MulDiv(700, static_cast<int>(dpi), 96);
                (void)Gui::send(gui.window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
                NONCLIENTMETRICSW metrics{sizeof(metrics)};
                if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
                    throw std::runtime_error("Cannot query the independent system font");
                }
                const auto font = reinterpret_cast<HFONT>(Gui::send(
                    GetDlgItem(gui.window, ui::control_id::play_pause), WM_GETFONT));
                LOGFONTW actual{};
                expect("button has a valid system font", GetObjectW(font, sizeof(actual), &actual), sizeof(actual));
                expect("button font follows DPI and system settings", actual.lfHeight, metrics.lfMessageFont.lfHeight);
                check_control_bounds(gui.window);
                MINMAXINFO limits{};
                (void)Gui::send(gui.window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
                if (!SetWindowPos(gui.window, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                    throw std::runtime_error("Cannot resize the player to its minimum dimensions");
                }
                check_control_bounds(gui.window);
            }
            // A synthetic DPI message does not change GetDpiForWindow. Test
            // settings notifications only after returning to the actual DPI.
            (void)Gui::send(gui.window, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS);
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, actual_dpi)) {
                throw std::runtime_error("Cannot query the system font after a settings notification");
            }
            LOGFONTW actual{};
            const auto font = reinterpret_cast<HFONT>(Gui::send(
                GetDlgItem(gui.window, ui::control_id::play_pause), WM_GETFONT));
            expect("settings notification keeps a valid font", GetObjectW(font, sizeof(actual), &actual), sizeof(actual));
            expect("settings notification follows the actual window DPI", actual.lfHeight, metrics.lfMessageFont.lfHeight);
            check_control_bounds(gui.window);
            gui.request_close();
            expect("repeated DPI window exits successfully", gui.finish(), 0);
        }
        driver.check_closed();
        const auto gdi = GetGuiResources(current, GR_GDIOBJECTS);
        const auto user = GetGuiResources(current, GR_USEROBJECTS);
        if (iteration == 0) { baseline_gdi = gdi; baseline_user = user; }
        else {
            expect("repeated DPI changes release all GDI resources", gdi, baseline_gdi);
            expect("repeated windows release all USER resources", user, baseline_user);
        }
    }
}

void test_unchanged_settings_and_layout()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    const auto button = GetDlgItem(gui.window, ui::control_id::play_pause);
    const auto font = Gui::send(button, WM_GETFONT);
    const auto before = gui.control_work();
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        (void)Gui::send(gui.window, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS);
        (void)Gui::send(gui.window, WM_SETTINGCHANGE, SPI_SETMOUSE);
        (void)Gui::send(gui.window, WM_THEMECHANGED);
        (void)Gui::send(gui.window, WM_SIZE, SIZE_RESTORED);
    }
    const auto unchanged = gui.control_work();
    expect("unchanged settings retain the shared font", Gui::send(button, WM_GETFONT), font);
    expect("unchanged settings send no font updates", unchanged.fonts, before.fonts);
    expect("unchanged layout sends no child positioning", unchanged.placements, before.placements);

    RECT bounds{};
    if (!GetWindowRect(gui.window, &bounds)) { throw std::runtime_error("Cannot query the player rectangle"); }
    const UINT dpi = GetDpiForWindow(gui.window) == 192 ? 144 : 192;
    bounds.right = bounds.left + MulDiv(760, static_cast<int>(dpi), 96);
    bounds.bottom = bounds.top + MulDiv(700, static_cast<int>(dpi), 96);
    (void)Gui::send(gui.window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
    const auto changed = gui.control_work();
    expect("a changed DPI still updates all control fonts", changed.fonts - unchanged.fonts, ui::control_id::count);
    expect("a changed DPI still repositions every control", changed.placements - unchanged.placements, ui::control_id::count);
    check_control_bounds(gui.window);
    (void)Gui::send(gui.window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
    const auto repeated = gui.control_work();
    expect("a repeated DPI preserves all fonts", repeated.fonts, changed.fonts);
    expect("a repeated DPI preserves all positions", repeated.placements, changed.placements);
    gui.request_close();
    expect("cached-layout window exits successfully", gui.finish(), 0);
    driver.check_closed();
}

void test_read_only_input_method_scope()
{
    // Keep a different thread's ordinary window alive. Disabling the whole
    // process would also remove its IME window on an IME-enabled system.
    struct OtherWindow {
        HWND handle{CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
        ~OtherWindow() { if (handle) { DestroyWindow(handle); } }
    } other;
    if (!other.handle) { throw std::runtime_error("Cannot create the independent IME probe window"); }
    const auto other_ime = ImmGetDefaultIMEWnd(other.handle);
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    gui.check_no_input_method();
    expect_true("GUI leaves another thread's input method unchanged", ImmGetDefaultIMEWnd(other.handle) == other_ime);
    gui.request_close();
    expect("read-only GUI exits successfully", gui.finish(), 0);
    expect_true("GUI closure leaves another thread's input method unchanged", ImmGetDefaultIMEWnd(other.handle) == other_ime);
    driver.check_closed();
}

// The process's state when the suite started, before its first window.
std::optional<ProcessState> suite_process;

void test_ui_thread_speed_scope()
{
    const auto inherited_priorities = new_thread_priorities();
    // Initialization waits on the controller STA until the UI loop runs. The
    // STA, named by the product, then reports its own speed and that of a
    // thread it starts as a driver would; the UI thread is checked meanwhile.
    TempFile file;
    GuiDriver driver;
    CallGate initialization;
    std::optional<ThreadSpeedState> sta_speed, driver_thread_speed;
    ThreadPriorities sta_priorities;
    std::wstring sta_name;
    bool query_failed{};
    driver.on_init = [&] {
        initialization.block();
        try {
            sta_speed = thread_speed_state(GetCurrentThread());
            sta_priorities = thread_priorities(GetCurrentThread());
            sta_name = thread_description(GetCurrentThread());
            std::thread started([&] {
                try {
                    driver_thread_speed = thread_speed_state(GetCurrentThread());
                } catch (...) {
                    query_failed = true; // Leaving this thread with an exception would terminate.
                }
            });
            started.join();
        } catch (...) {
            query_failed = true; // Never throw into the product's driver call.
        }
    };
    // Only thread speeds change: no priority, background mode or process-wide
    // call. A process-wide change outlives its case, so compare with the state
    // before the suite's first window.
    const auto& process = suite_process.value();
    expect_true("the earlier windows left the process's priority class and power throttling",
        process_state() == process);
    Gui gui(driver, source(file));
    ReleaseGate release{initialization};
    initialization.wait();
    gui.drain_input(); // Retrieved by window.run(), so the speed scope is active.
    expect_priorities("the UI thread starts with inherited priorities", gui.entry_priorities(), inherited_priorities);
    const auto ui_thread = open_thread_for_query(gui.thread());
    const bool reported = expect_thread_speed("the UI thread requests its configured QoS while its window exists", ui_thread.get(),
        ui::ui_thread_speed);
    expect_priorities("the UI thread preserves its entry priorities", thread_priorities(ui_thread.get()), gui.entry_priorities());
    expect_true("the window leaves the process's priority class and power throttling", process_state() == process);
    initialization.open();
    gui.settled(L"Stopped");
    gui.responsive();
    expect("the queries on the controller STA and the thread it starts succeed", static_cast<int>(query_failed), 0);
    expect_priorities("the controller STA preserves inherited priorities", sta_priorities, inherited_priorities);
    expect_true("the controller STA is named", sta_name == L"WAV Player control");
    if (reported) {
        expect_speed_state("the controller STA keeps the default speed", sta_speed, wavplay::core::ExecutionSpeed::system);
        expect_speed_state("a thread started from the STA keeps the default speed", driver_thread_speed,
            wavplay::core::ExecutionSpeed::system);
        expect_thread_speed("the UI thread keeps its configured QoS after initialization", ui_thread.get(),
            ui::ui_thread_speed);
    }
    gui.request_close();
    expect("the QoS-scoped GUI exits successfully", gui.finish(), 0);
    expect_true("closing leaves the process's priority class and power throttling", process_state() == process);
    expect("the UI thread's query after run_player_window succeeds", static_cast<int>(gui.exit_query_failed()), 0);
    expect_priorities("the UI thread exits with its entry priorities", gui.exit_priorities(), gui.entry_priorities());
    if (reported) {
        expect_speed_state("the UI thread returns to the default before run_player_window returns", gui.exit_speed(),
            wavplay::core::ExecutionSpeed::system);
    }
    driver.check_closed();
}

void test_state_changes_inside_modal_loop()
{
    TempFile file;
    GuiDriver driver;
    CallGate initialization;
    driver.on_init = [&] { initialization.block(); };
    Gui gui(driver, source(file));
    ReleaseGate release{initialization};
    initialization.wait();
    gui.begin_modal_loop();
    initialization.open();
    gui.settled(L"Stopped");
    gui.responsive();
    gui.end_modal_loop();
    gui.request_close();
    expect("GUI closes after nested state delivery", gui.finish(), 0);
    driver.check_closed();
}

void test_failed_creation_does_not_post_quit()
{
    TempFile file;
    const auto path = source(file);
    expect_throw<std::invalid_argument>("invalid controller construction propagates", [&] {
        (void)ui::run_player_window({selected, path}, SW_HIDE, nullptr);
    });
    MSG message{};
    expect("failed GUI creation leaves no thread quit message", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), FALSE);
    GuiDriver driver;
    Gui gui(driver, path);
    gui.settled(L"Stopped");
    gui.request_close();
    expect("GUI can start after failed construction", gui.finish(), 0);
    driver.check_closed();
}

void test_thread_quit(bool fault)
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    driver.sample_rate = fault ? 44100 : 48000;
    driver.refuse_restore = fault;
    Gui gui(driver, source(file));
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    if (fault) { gui.click(ui::control_id::play_pause); gui.settled(L"Playing"); }
    gui.quit(42);
    release_call.wait();
    gui.responsive();
    expect("thread quit retains the window while release is pending", IsWindow(gui.window), TRUE);
    release_call.open();
    expect("thread quit preserves its code unless playback failed", gui.finish(), fault ? 1 : 42);
    driver.check_closed();
}

void test_initializing_close()
{
    TempFile file;
    GuiDriver driver;
    CallGate initialization;
    driver.on_init = [&] { initialization.block(); };
    Gui gui(driver, source(file));
    ReleaseGate release{initialization};
    initialization.wait();
    gui.state(L"Initializing");
    expect("Play is disabled during initialization", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    expect("Stop is disabled during initialization", static_cast<int>(gui.enabled(ui::control_id::stop)), 0);
    for (unsigned i = 0; i < 40; ++i) {
        gui.click(ui::control_id::play_pause);
        gui.key(VK_SPACE);
        gui.key(VK_ESCAPE);
    }
    gui.responsive();
    gui.request_close();
    gui.wait_until([&] { return gui.text(ui::control_id::pending).find(L"Close requested") != std::wstring::npos; },
        "Close acceptance during initialization");
    gui.responsive();
    expect("initialization cleanup keeps window alive", IsWindow(gui.window), TRUE);
    initialization.open();
    expect("clean GUI exits successfully", gui.finish(), 0);
    expect("disabled initialization controls never start audio", driver.starts, 0);
    driver.check_closed();
}

void test_transport_and_keyboard()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    // Even with a Stop child as the keyboard target, Space is Play/Pause.
    gui.key(VK_SPACE, ui::control_id::stop);
    gui.settled(L"Paused", &driver);
    expect("paused button offers Play", static_cast<int>(gui.text(ui::control_id::play_pause).find(L"Play") != std::wstring::npos), 1);
    for (unsigned i = 0; i < 40; ++i) { gui.key(VK_SPACE, ui::control_id::stop, true); }
    gui.drain_input();
    gui.state(L"Paused");
    gui.key(VK_SPACE, ui::control_id::stop);
    gui.settled(L"Playing");
    gui.key(VK_ESCAPE, ui::control_id::play_pause);
    gui.settled(L"Stopped");
    gui.key(VK_SPACE);
    gui.settled(L"Playing");
    gui.play_to_end(driver);
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.request_close();
    expect("clean GUI exits successfully", gui.finish(), 0);
    expect("GUI Play pause resume Stop EOF replay start count", driver.starts, 4);
    expect("GUI stops every started stream", driver.stops, 4);
    expect("GUI pause leaves a shorter first run", static_cast<int>(driver.captures.at(0).size() < driver.captures.at(2).size()), 1);
    // The first submitted PCM after Stop and EOF replay is identical.
    expect("GUI Stop and EOF both replay the first PCM frame", static_cast<int>(driver.captures.at(2).front() == driver.captures.at(3).front()), 1);
    driver.check_closed();
}

void test_notification_reinitialization()
{
    TempFile file;
    GuiDriver driver;
    CallGate initialization;
    driver.on_init = [&] {
        if (driver.inits == 2) { initialization.block(); }
    };
    Gui gui(driver, source(file));
    ReleaseGate release{initialization};
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
    initialization.wait();
    gui.state(L"Reinitializing");
    expect_true("GUI disables transport during driver reinitialization",
        !gui.enabled(ui::control_id::play_pause) && !gui.enabled(ui::control_id::stop));
    gui.responsive();
    gui.click(ui::control_id::play_pause);
    initialization.open();
    gui.settled(L"Stopped");
    expect("GUI recovery has not restarted audio", driver.starts, 1);
    expect("GUI recovery opened a fresh driver", driver.inits, 2);
    expect_true("successful GUI recovery clears the error field", gui.text(ui::control_id::error).empty());
    expect_true("GUI clears stale allocation details after recovery",
        gui.text(ui::control_id::buffer).find(L"not allocated") != std::wstring::npos);
    expect_true("GUI clears capabilities from the retired driver",
        gui.text(ui::control_id::buffer).find(L"Capabilities: unavailable") != std::wstring::npos);
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.play_to_end(driver);
    gui.request_close();
    expect("recovered GUI closes successfully", gui.finish(), 0);
    expect("only an explicit Play restarts the recovered GUI", driver.starts, 2);
    expect_true("recovered GUI restarts from the first PCM frame",
        driver.captures.at(0).front() == driver.captures.at(1).front());
    driver.check_closed();
}

void test_configured_buffer_display()
{
    TempFile audio, configuration;
    constexpr std::string_view text = "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nBufferSize=96\nStartSilence=0\n";
    const auto settings_path = configuration.write(Bytes(text.begin(), text.end()));
    GuiDriver driver;
    driver.buffer_size = {48, 192, 48, 48};
    Gui gui(driver, source(audio), SW_HIDE, 100ms, settings_path);
    gui.settled(L"Stopped");
    expect_true("GUI distinguishes requested settings from a verified allocation",
        gui.text(ui::control_id::buffer) == L"Requested: 96 frames\r\nAllocation: not allocated\r\nCapabilities: unavailable");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    const auto actual = gui.text(ui::control_id::buffer);
    expect_true("GUI keeps the requested width beside the verified width", actual.starts_with(L"Requested: 96 frames\r\n"));
    expect_true("GUI shows exact allocated width, confirmed rate and block time",
        actual.find(L"Allocated: 96 frames / 2.000 ms at 48000 Hz") != std::wstring::npos);
    expect_true("GUI shows the rate-specific driver range and granularity",
        actual.find(L"Capabilities at 48000 Hz: min 48; preferred 48\r\nMax 192; granularity 48 (step from min)") != std::wstring::npos);
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Paused", &driver);
    expect_true("GUI identifies the released allocation after Pause",
        gui.text(ui::control_id::buffer).find(L"Last allocation: 96 frames / 2.000 ms at 48000 Hz") != std::wstring::npos);
    driver.buffer_size = {48, 240, 48, 48};
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    expect_true("GUI refreshes the capabilities after Resume",
        gui.text(ui::control_id::buffer).find(L"Max 240; granularity 48") != std::wstring::npos);
    gui.click(ui::control_id::stop);
    gui.settled(L"Stopped");
    expect_true("GUI retains the released allocation after Stop without calling it current",
        gui.text(ui::control_id::buffer).find(L"Last allocation: 96 frames / 2.000 ms at 48000 Hz") != std::wstring::npos);
    check_control_bounds(gui.window);
    gui.request_close();
    expect("configured GUI closes successfully", gui.finish(), 0);
    driver.check_closed();
}

void test_rejected_buffer_display()
{
    for (const bool refuse_create : {false, true}) {
        TempFile audio, configuration;
        constexpr std::string_view text = "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nOutputPair=3\nBufferSize=96\nStartSilence=0\n";
        const auto settings_path = configuration.write(Bytes(text.begin(), text.end()));
        GuiDriver driver;
        driver.outputs = 4;
        driver.first_output = 2;
        driver.buffer_size = {32, 256, 64, refuse_create ? 0 : -1};
        if (refuse_create) {
            driver.on_call = [&](MockAsio::Call call) {
                if (call == MockAsio::Call::create) { driver.buffer_size = {48, 48, 48, 0}; }
            };
        }
        Gui gui(driver, source(audio), SW_HIDE, 0ms, settings_path);
        gui.settled(L"Stopped");
        gui.click(ui::control_id::play_pause);
        gui.state(L"Error");
        const auto diagnostic = gui.text(ui::control_id::buffer);
        expect_true("rejected allocation keeps the requested width", diagnostic.starts_with(L"Requested: 96 frames\r\nAllocation: not allocated"));
        expect_true("rejected allocation retains confirmed capabilities",
            diagnostic.find(L"Capabilities at 48000 Hz: min 32; preferred 64\r\nMax 256; granularity ") != std::wstring::npos);
        expect_true("GUI explains the zero and power-of-two granularity rules", diagnostic.find(refuse_create ?
            L"granularity 0 (no extra step)" : L"granularity -1 (powers of two)") != std::wstring::npos);
        expect_true("rejected allocation never invents block time", diagnostic.find(L" ms ") == std::wstring::npos);
        expect_true("configured output remains visible when a width is rejected",
            gui.text(ui::control_id::output) == L"Output: 3 / 4 (configured)");
        expect_true("GUI explains the rejected operation", gui.text(ui::control_id::error).find(refuse_create ?
            L"ASIO createBuffers failed" : L"Unsupported ASIO buffer size") != std::wstring::npos);
        expect("invalid width never starts playback", driver.starts, 0);
        expect("driver refusal never retries another width", std::count(driver.calls.begin(), driver.calls.end(), MockAsio::Call::create),
            refuse_create ? 1 : 0);
        gui.request_close();
        expect("rejected buffer GUI exits with failure", gui.finish(), 1);
        driver.check_closed();
    }
}

void check_output_text_fits(const Gui& gui)
{
    const auto output = GetDlgItem(gui.window, ui::control_id::output);
    if (!output) { throw std::runtime_error("The output control is missing"); }
    const auto text = gui.text(ui::control_id::output);
    const auto font = reinterpret_cast<HFONT>(Gui::send(output, WM_GETFONT));
    if (!font) { throw std::runtime_error("The output control has no font"); }
    const auto dc = GetDC(output);
    if (!dc) { throw std::runtime_error("Cannot measure the output control text"); }
    const auto previous = SelectObject(dc, font);
    if (!previous || previous == HGDI_ERROR) {
        ReleaseDC(output, dc);
        throw std::runtime_error("Cannot select the output control font");
    }
    SIZE measured{};
    const auto result = GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &measured);
    SelectObject(dc, previous);
    ReleaseDC(output, dc);
    expect("output text can be measured", result, TRUE);
    RECT area{};
    expect("output control bounds can be read", GetClientRect(output, &area), TRUE);
    expect_true("the complete output pair remains visible", measured.cx <= area.right && measured.cy <= area.bottom);
}

void check_readonly_final_line(const Gui& gui, int id)
{
    const auto control = GetDlgItem(gui.window, id);
    if (!control) { throw std::runtime_error("The readonly text control is missing"); }
    const auto text = gui.text(id);
    const auto font = reinterpret_cast<HFONT>(Gui::send(control, WM_GETFONT));
    if (!font) { throw std::runtime_error("The readonly text control has no font"); }
    const auto dc = GetDC(control);
    if (!dc) { throw std::runtime_error("Cannot measure the readonly text font"); }
    const auto previous = SelectObject(dc, font);
    if (!previous || previous == HGDI_ERROR) {
        ReleaseDC(control, dc);
        throw std::runtime_error("Cannot select the readonly text font");
    }
    TEXTMETRICW metrics{};
    const auto measured = GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
    expect("readonly text font can be measured", measured, TRUE);
    RECT visible{};
    (void)Gui::send(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&visible));
    expect_true("each readonly field fits at least one complete text line", visible.bottom - visible.top >= metrics.tmHeight);
    if (!text.empty()) {
        // Hidden, unfocused readonly edits have no visible caret to scroll.
        // Use the same operation as the standard vertical scrollbar instead.
        (void)Gui::send(control, WM_VSCROLL, SB_BOTTOM);
        const auto position = Gui::send(control, EM_POSFROMCHAR, text.size() - 1);
        const auto last_line = static_cast<short>(HIWORD(position));
        if (position == -1 || last_line < visible.top || last_line + metrics.tmHeight > visible.bottom) {
            fail("The complete final readonly line is clipped: control=" + std::to_string(id) +
                ", position=" + std::to_string(position) + ", line_y=" + std::to_string(last_line) +
                ", visible_top=" + std::to_string(visible.top) + ", visible_bottom=" + std::to_string(visible.bottom) +
                ", text_height=" + std::to_string(metrics.tmHeight) + ", first_visible_line=" +
                std::to_string(Gui::send(control, EM_GETFIRSTVISIBLELINE)) + ", final_line=" +
                std::to_string(Gui::send(control, EM_LINEFROMCHAR, text.size() - 1)));
        }
    }
    expect_true("scrolling preserves the complete readonly text", gui.text(id) == text);
}

void test_large_buffer_capabilities_display()
{
    TempFile audio, configuration;
    const std::string text = "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nName=" + std::string(400, 'N') +
        "日本語 DAC\nOutputPair=2147483645\nBufferSize=1\nStartSilence=0\n";
    const auto settings_path = configuration.write(Bytes(text.begin(), text.end()));
    GuiDriver driver;
    constexpr auto largest = std::numeric_limits<long>::max();
    driver.outputs = largest;
    driver.first_output = largest - 3;
    driver.buffer_size = {largest - 2, largest, largest - 2, 2};
    Gui gui(driver, source(audio), SW_HIDE, 0ms, settings_path);
    gui.settled(L"Stopped");
    expect_true("long device names do not contain or hide the independent output field",
        gui.text(ui::control_id::device).find(L"日本語 DAC") != std::wstring::npos &&
        gui.text(ui::control_id::device).find(L"Output:") == std::wstring::npos &&
        gui.text(ui::control_id::output) == L"Output: 2147483645 / 2147483646 (configured)");
    gui.click(ui::control_id::play_pause);
    gui.state(L"Error");
    const auto buffer = GetDlgItem(gui.window, ui::control_id::buffer);
    const auto diagnostic = gui.text(ui::control_id::buffer);
    expect_true("large capabilities retain every digit",
        diagnostic.find(L"min 2147483645; preferred 2147483645\r\nMax 2147483647; granularity 2") != std::wstring::npos);
    const auto style = GetWindowLongPtrW(buffer, GWL_STYLE);
    expect_true("wrapped diagnostics can be scrolled without editing", (style &
        (ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL)) == (ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL));
    for (const UINT dpi : {GetDpiForWindow(gui.window), 144u, 192u}) {
        RECT suggested{0, 0, MulDiv(760, static_cast<int>(dpi), 96), MulDiv(700, static_cast<int>(dpi), 96)};
        (void)Gui::send(gui.window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        MINMAXINFO limits{};
        (void)Gui::send(gui.window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        expect("diagnostic window can use its minimum size", SetWindowPos(gui.window, nullptr, 0, 0,
            limits.ptMinTrackSize.x, limits.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), TRUE);
        check_control_bounds(gui.window);
        check_output_text_fits(gui);
        for (const auto id : {ui::control_id::buffer, ui::control_id::file, ui::control_id::error}) {
            check_readonly_final_line(gui, id);
        }
        if (dpi == 144) {
            // Model the outer bounds left after a 720/768-pixel monitor's work
            // area limits a 150%-scaled window. Do not rely on this test host's
            // actual monitor being small enough to expose clipped controls.
            expect("small work-area height can limit the window", SetWindowPos(gui.window, nullptr, 0, 0,
                suggested.right, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), TRUE);
            RECT limited{};
            expect("the limited window bounds can be read", GetWindowRect(gui.window, &limited), TRUE);
            expect("minimum tracking size does not exceed the small work-area height", limited.bottom - limited.top, 640);
            check_control_bounds(gui.window);
            check_output_text_fits(gui);
            for (const auto id : {ui::control_id::buffer, ui::control_id::file, ui::control_id::error}) {
                check_readonly_final_line(gui, id);
            }
        }
        expect_true("DPI and scrolling retain the complete diagnostic text", gui.text(ui::control_id::buffer) == diagnostic);
    }
    gui.request_close();
    expect("large rejected buffer GUI closes with failure", gui.finish(), 1);
    driver.check_closed();
}

void test_failed_reinitialization_device_display()
{
    TempFile file;
    GuiDriver driver;
    driver.on_init = [&] { driver.fail_init = driver.inits == 2; };
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
    gui.state(L"Error");
    expect_true("failed reinitialization clears the available device display",
        gui.text(ui::control_id::device) == L"Device: NO DEVICE");
    expect_true("failed reinitialization keeps its reason visible", !gui.text(ui::control_id::error).empty());
    expect_true("failed reinitialization disables Play", !gui.enabled(ui::control_id::play_pause));
    gui.request_close();
    expect("failed reinitialization exits with failure", gui.finish(), 1);
    driver.check_closed();
}

void test_invalid_configuration_display()
{
    TempFile audio, configuration;
    constexpr std::string_view text = "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\nStartSilence=1001\n";
    GuiDriver driver;
    Gui gui(driver, source(audio), SW_HIDE, 0ms, configuration.write(Bytes(text.begin(), text.end())));
    gui.state(L"Error");
    expect_true("GUI names the rejected configuration setting", gui.text(ui::control_id::error).find(L"StartSilence") != std::wstring::npos);
    expect_true("GUI never offers Play after a configuration error", !gui.enabled(ui::control_id::play_pause));
    gui.responsive();
    gui.request_close();
    expect("configuration error is returned to the caller", gui.finish(), 1);
    expect("bad configuration leaves ASIO unopened", driver.releases, 0);
}

constexpr std::string_view selected_config =
    "[General]\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\n"
    "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\n"
    "Name=日本語 DAC\nOutputPair=1\nBufferSize=0\nStartSilence=0\n";

std::string read_configuration(const wchar_t* path)
{
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot read test configuration"); }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

RECT window_bounds(HWND window)
{
    RECT bounds{};
    expect("test reads the actual window rectangle", GetWindowRect(window, &bounds), TRUE);
    return bounds;
}

RECT work_area(HWND window)
{
    MONITORINFO info{sizeof(info)};
    expect("test reads the window's monitor", GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info), TRUE);
    return info.rcWork;
}

void expect_bounds(const RECT& actual, const wavplay::settings::WindowPlacement& expected)
{
    expect("normal window left is restored", actual.left, expected.x);
    expect("normal window top is restored", actual.top, expected.y);
    expect("normal window width is restored", actual.right - actual.left, expected.width);
    expect("normal window height is restored", actual.bottom - actual.top, expected.height);
}

void test_window_persistence()
{
    TempFile audio, configuration;
    const auto source_path = source(audio);
    const std::string initial(selected_config);
    const auto path = configuration.write(Bytes(initial.begin(), initial.end()));
    wavplay::settings::WindowPlacement saved;
    {
        GuiDriver driver;
        Gui gui(driver, source_path, SW_HIDE, 100ms, path, true);
        gui.settled(L"Stopped");
        expect_true("INI-selected device and its Unicode label are shown",
            gui.text(ui::control_id::device).find(L"日本語 DAC") != std::wstring::npos &&
            gui.text(ui::control_id::device).find(selected_text) != std::wstring::npos &&
            gui.text(ui::control_id::output) == L"Output: 1 / 2 (configured)");
        const auto work = work_area(gui.window);
        const auto width = (std::min)(1000L, work.right - work.left);
        const auto height = (std::min)(700L, work.bottom - work.top);
        expect("user can move and resize the window", SetWindowPos(gui.window, nullptr,
            work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE), TRUE);
        const auto actual = window_bounds(gui.window);
        saved = {actual.left, actual.top, actual.right - actual.left, actual.bottom - actual.top, false};
        // A completed manual edit while the application is running must survive
        // its later position save, and apply only to the next controller.
        const std::string edited = initial + "\n; manual edit kept\n[Future]\n日本語=value\n";
        configuration.write(Bytes(edited.begin(), edited.end()));
        gui.request_close();
        expect("window settings save succeeds", gui.finish(), 0);
        driver.check_closed();
        expect_true("manual edits survive the GUI close", read_configuration(path).starts_with(edited));
    }
    const auto stored = wavplay::settings::load_application_config(path).window.value();
    expect_bounds(RECT{stored.x, stored.y, stored.x + stored.width, stored.y + stored.height}, saved);
    {
        GuiDriver driver;
        Gui gui(driver, source_path, SW_HIDE, 100ms, path, true);
        gui.settled(L"Stopped");
        expect_bounds(window_bounds(gui.window), saved);
        check_control_bounds(gui.window);
        gui.request_close();
        expect("restored window closes successfully", gui.finish(), 0);
        driver.check_closed();
    }
}

void test_maximized_and_minimized_persistence()
{
    TempFile audio, configuration;
    const auto source_path = source(audio);
    const std::string initial(selected_config);
    const auto path = configuration.write(Bytes(initial.begin(), initial.end()));
    RECT normal{};
    {
        GuiDriver driver;
        Gui gui(driver, source_path, SW_SHOWNOACTIVATE, 0ms, path, true);
        gui.settled(L"Stopped");
        normal = window_bounds(gui.window);
        (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_MAXIMIZE);
        gui.wait_until([&] { return IsZoomed(gui.window) != FALSE; }, "maximize before save");
        (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_MINIMIZE);
        gui.wait_until([&] { return IsIconic(gui.window) != FALSE; }, "minimize before save");
        gui.request_close();
        expect("minimized window closes successfully", gui.finish(), 0);
    }
    const auto saved = wavplay::settings::load_application_config(path).window.value();
    expect_true("minimizing does not erase the maximized state", saved.maximized);
    expect_bounds(normal, saved);
    {
        GuiDriver driver;
        Gui gui(driver, source_path, SW_SHOWMINIMIZED, 0ms, path, true);
        gui.settled(L"Stopped");
        expect_true("explicit launch-minimized state is honored", IsIconic(gui.window) != FALSE);
        (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_RESTORE);
        gui.wait_until([&] { return IsZoomed(gui.window) != FALSE; }, "restore minimized launch to maximized");
        gui.request_close();
        expect("launch-minimized window retains the saved maximized state", gui.finish(), 0);
    }
    {
        GuiDriver driver;
        Gui gui(driver, source_path, SW_SHOWNOACTIVATE, 0ms, path, true);
        gui.settled(L"Stopped");
        expect_true("saved maximized state is restored", IsZoomed(gui.window) != FALSE);
        (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_RESTORE);
        gui.wait_until([&] { return IsZoomed(gui.window) == FALSE; }, "restore normal bounds");
        expect_bounds(window_bounds(gui.window), saved);
        gui.request_close();
        expect("restored normal window closes successfully", gui.finish(), 0);
    }
    expect_true("restoring to normal clears the saved maximized state",
        !wavplay::settings::load_application_config(path).window->maximized);
}

void test_quiet_progress_while_minimized()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file, 48000 * 4), SW_SHOWNOACTIVATE);
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    expect_true("playback starts at zero", gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_MINIMIZE);
    gui.wait_until([&] { return IsIconic(gui.window) != FALSE; }, "minimize during playback");
    gui.drain_input();
    const auto posted = gui.posted_notifications();
    // 1,100 blocks of 48 frames reach the next whole second. Wake the control
    // thread so that it publishes that progress now rather than at its next
    // periodic wake.
    for (unsigned i = 0; i < 1100; ++i) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    // Each read wakes the UI, whose refresh() renders the revision the quiet
    // progress still stored. Its snapshot waits for the controller's lock, under
    // which the listener would have posted, so once the second shows, any post
    // is queued ahead of the input barrier and has been counted after it.
    gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04"; },
        "elapsed time published while minimized");
    gui.drain_input();
    expect("minimized playback posts no progress notification", gui.posted_notifications(), posted);
    const auto queries_before_restore = gui.control_updates().selection_queries;
    (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_RESTORE);
    gui.wait_until([&] { return IsIconic(gui.window) == FALSE; }, "restore during playback");
    expect_true("the restored window shows the second published while minimized",
        gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04");
    // The time already matched before restoring. The input barrier only
    // acknowledges message retrieval, before the outer loop's forced refresh.
    // Wait for its actual list query without sending another window message.
    gui.wait_until([&] { return gui.control_updates().selection_queries > queries_before_restore; },
        "full refresh after restoring an already rendered revision");
    // Nothing was published since: the restore itself posts nothing.
    gui.drain_input();
    expect("restoring needs no new notification", gui.posted_notifications(), posted);
    for (unsigned i = 0; i < 1000; ++i) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    gui.wait_until([&] { return gui.posted_notifications() > posted; }, "progress notification after restoring");
    gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:02 / 0:04"; },
        "elapsed time while restored");
    (void)Gui::send(gui.window, WM_SYSCOMMAND, SC_MINIMIZE);
    gui.wait_until([&] { return IsIconic(gui.window) != FALSE; }, "minimize again");
    const auto before_pause = gui.posted_notifications();
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Paused", &driver);
    gui.wait_until([&] { return gui.posted_notifications() > before_pause; }, "state notification while minimized");
    gui.request_close(); // Closed still reaches the minimized window and destroys it.
    expect("minimized playback closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

// A new second on the same track refreshes only time: other texts are neither
// rebuilt nor sent, and the list is not queried again. Track changes also use
// progress publications, but must take the full path (checked below).
void test_progress_refresh_rewrites_only_time()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file, 48000 * 4));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    expect_true("playback starts at zero", gui.text(ui::control_id::time) == L"Time: 0:00 / 0:04");
    gui.drain_input();
    const auto before = gui.control_updates();
    for (unsigned i = 0; i < 1100; ++i) { driver.pulse(); } // The next whole second, as above.
    wavplay::playback::wake_playback_control();
    gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04"; }, "elapsed time");
    gui.drain_input(); // Retrieved only after the refresh that set the second has returned.
    const auto after = gui.control_updates();
    for (int id = ui::control_id::first; id <= ui::control_id::last; ++id) {
        const auto slot = ui::control_id::slot(id);
        expect("WM_SETTEXT to control " + std::to_string(id) + " for a new second", after.texts[slot] - before.texts[slot],
            id == ui::control_id::time ? 1 : 0);
    }
    expect("a progress refresh enables or disables no control", after.enables - before.enables, 0);
    expect("same-track progress makes no list selection query", after.selection_queries - before.selection_queries, 0);
    gui.request_close();
    expect("the progress window closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

void test_progress_refresh_at_track_boundary()
{
    TempFile first, second, playlist;
    const auto first_path = source(first, 48000 + 13);
    const auto second_path = source(second, 48000 * 3 + 29);
    GuiDriver driver;
    Gui gui(driver, {}, SW_HIDE, 0ms, {}, false, playlist_file(playlist, {first_path, second_path}));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    expect_true("the first track starts with its own duration", gui.text(ui::control_id::time) == L"Time: 0:00 / 0:01");
    gui.drain_input();
    const auto before = gui.control_updates();
    // The same-rate boundary falls inside a block. No transport command or
    // new ASIO start announces it: progress must refresh the selected path,
    // list row and duration together, without changing the existing allocation.
    // Consume less than the initial two-second fill, so producer scheduling
    // cannot turn this UI check into an artificial underrun.
    for (unsigned i = 0; i < 1100; ++i) { driver.pulse(); }
    wavplay::playback::wake_playback_control();
    gui.wait_until([&] {
        return gui.text(ui::control_id::file) == second_path &&
            gui.text(ui::control_id::time) == L"Time: 0:00 / 0:03";
    }, "same-rate track boundary display");
    gui.drain_input();
    const auto after = gui.control_updates(); // Before selected_track() itself sends LB_GETCURSEL.
    expect_true("a track-boundary progress update queries the selection",
        after.selection_queries > before.selection_queries);
    expect("the new track is selected", gui.selected_track(), 1);
    expect("same-rate track progress keeps one ASIO start", driver.starts, 1);
    gui.request_close();
    expect("the track-boundary window closes cleanly", gui.finish(), 0);
    driver.check_closed();
}

// A UI failure is terminal: the window then shows only the state and Close.
// Like a normal render, the failed one sets those texts once per published
// revision, and unrelated wakes (mouse moves, sent messages) set none.
// DefWindowProc destroys a control on WM_CLOSE, on the UI thread. The next
// layout cannot place it and fails the UI, without a product hook.
void test_failed_ui_renders_once_per_revision(bool playing)
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file, playing ? 48000 * 4 : 1000));
    gui.settled(L"Stopped");
    if (playing) {
        gui.click(ui::control_id::play_pause);
        gui.settled(L"Playing");
        // Establish a successful lightweight progress refresh before entering
        // the terminal UI failure path, which must still close without copying
        // another snapshot or attempting normal rendering.
        for (unsigned index = 0; index < 1100; ++index) { driver.pulse(); }
        wavplay::playback::wake_playback_control();
        gui.wait_until([&] { return gui.text(ui::control_id::time) == L"Time: 0:01 / 0:04"; },
            "progress before the UI failure");
    }
    const auto pending = GetDlgItem(gui.window, ui::control_id::pending);
    (void)Gui::send(pending, WM_CLOSE);
    expect("the pending control is destroyed", IsWindow(pending), FALSE);
    RECT bounds{};
    if (!GetWindowRect(gui.window, &bounds)) { throw std::runtime_error("Cannot query the player rectangle"); }
    // The new client size makes WM_SIZE lay out every control again.
    if (!SetWindowPos(gui.window, nullptr, 0, 0, bounds.right - bounds.left + 16, bounds.bottom - bounds.top + 16,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        throw std::runtime_error("Cannot resize the player");
    }
    gui.wait_until([&] { return gui.text(ui::control_id::error).find(L"DeferWindowPos") != std::wstring::npos; },
        "UI failure display");
    // The failure requested Close. Closed then awaits its acknowledgment.
    gui.state(L"Closed");
    gui.wait_until([&] { return gui.enabled(ui::control_id::stop); }, "Close after the UI failure");
    expect_true("the failed UI offers Close and no other operation", gui.text(ui::control_id::stop) == L"&Close" &&
        !gui.enabled(ui::control_id::play_pause) && !gui.enabled(ui::control_id::playlist));
    // After the input barrier nothing is queued, so the second round trip is
    // dispatched only after the refresh that followed the first has returned.
    const auto settle = [&] { gui.drain_input(); gui.responsive(); gui.responsive(); };
    settle();
    const auto before = gui.control_updates();
    for (unsigned i = 0; i < 16; ++i) {
        expect("a mouse move is queued", PostMessageW(gui.window, WM_MOUSEMOVE, 0, MAKELPARAM(i, i)), TRUE);
        gui.responsive();
    }
    settle();
    const auto after = gui.control_updates();
    for (int id = ui::control_id::first; id <= ui::control_id::last; ++id) {
        const auto slot = ui::control_id::slot(id);
        expect("WM_SETTEXT to control " + std::to_string(id) + " on unrelated wakes after a UI failure",
            after.texts[slot] - before.texts[slot], 0);
    }
    expect("unrelated wakes after a UI failure enable or disable no control", after.enables - before.enables, 0);
    expect_true("the failed UI still shows Closed with Close enabled",
        gui.text(ui::control_id::status) == L"Closed" && gui.enabled(ui::control_id::stop));
    gui.click(ui::control_id::stop);
    expect("the failed UI exits with failure after Close", gui.finish(), 1);
    driver.check_closed();
}

// DestroyWindow from outside the product discards the window's queued state
// notification, which leaves the coalescing flag set. Only the cleared posting
// target (WM_NCDESTROY) still lets the later closing and closed publications
// wake the UI, which then ends with the external quit's contract. With no
// window left, a failed rate restoration during that cleanup can only show in
// the exit code.
void test_external_destroy_with_queued_notification(bool fault)
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    driver.sample_rate = fault ? 44100 : 48000;
    driver.refuse_restore = fault;
    Gui gui(driver, source(file, 48000 * 4), SW_SHOWNOACTIVATE);
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    // The visible window posts the next second while its thread is held.
    gui.destroy_with_queued_notification([&] {
        for (unsigned i = 0; i < 1100; ++i) { driver.pulse(); }
        wavplay::playback::wake_playback_control();
    });
    expect("the window is destroyed", IsWindow(gui.window), FALSE);
    // WM_DESTROY requested Close. Closed is published only once Release
    // returns, after the UI has handled the destruction.
    release_call.wait();
    release_call.open();
    expect(fault ? "a cleanup fault after an external destruction fails the exit code" :
        "an externally destroyed window exits successfully", gui.finish(), fault ? 1 : 0);
    expect("the destruction stops the stream once", driver.stops, 1);
    driver.check_closed();
}

void test_offscreen_placement()
{
    for (const auto geometry : {"", "X=2000000000\nY=2000000000\nWidth=1\nHeight=1\n",
        "X=-20000000\nY=-20000000\nWidth=10000000\nHeight=10000000\n"}) {
        TempFile audio, configuration;
        const auto text = std::string(selected_config) + "[Window]\n" + geometry;
        GuiDriver driver;
        Gui gui(driver, source(audio), SW_HIDE, 0ms, configuration.write(Bytes(text.begin(), text.end())), true);
        gui.settled(L"Stopped");
        const auto actual = window_bounds(gui.window), area = work_area(gui.window);
        expect_true("disconnected-monitor and oversized placement fits the current work area",
            actual.left >= area.left && actual.top >= area.top && actual.right <= area.right && actual.bottom <= area.bottom);
        check_control_bounds(gui.window);
        gui.request_close();
        expect("corrected placement can be saved", gui.finish(), 0);
    }
}

void test_missing_device_display()
{
    TempFile audio, configuration;
    const auto path = configuration.write({});
    expect("test starts with a missing configuration", DeleteFileW(path), TRUE);
    GuiDriver driver;
    Gui gui(driver, source(audio), SW_HIDE, 0ms, path, true);
    gui.state(L"Error");
    expect_true("missing selection explicitly shows NO DEVICE", gui.text(ui::control_id::device) == L"Device: NO DEVICE");
    expect_true("missing selection explains the INI setting", gui.text(ui::control_id::error).find(L"SelectedDevice") != std::wstring::npos);
    expect_true("missing selection disables playback", !gui.enabled(ui::control_id::play_pause));
    gui.request_close();
    expect("unconfigured application exits with failure", gui.finish(), 1);
    expect("missing selection never activates another driver", driver.activations, 0);
    const auto created = wavplay::settings::load_application_config(path);
    expect_true("first close creates a window record without inventing a device", created.window && !created.selected_device);
}

void test_settings_save_error_display()
{
    TempFile audio, configuration;
    const std::string original(selected_config);
    const auto path = configuration.write(Bytes(original.begin(), original.end()));
    GuiDriver driver;
    Gui gui(driver, source(audio), SW_HIDE, 0ms, path, true);
    gui.settled(L"Stopped");
    const wavplay::core::UniqueHandle lock(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    expect_true("test locks replacement of the settings file", lock.get() != INVALID_HANDLE_VALUE);
    gui.request_close();
    gui.state(L"Closed");
    expect_true("save failure remains visible after the driver has been released", !gui.text(ui::control_id::error).empty());
    expect("driver is released before reporting save failure", driver.references, 0);
    expect_true("save failure preserves the existing file", read_configuration(path) == original);
    gui.responsive();
    gui.click(ui::control_id::stop);
    expect("acknowledging save failure exits with failure", gui.finish(), 1);
}

// The GUI and the controller never branch on the WAV or ASIO sample format.
// Each format, output type and channel layout is compared with independent
// expectations below the GUI (wav_reader, prefetch_ring, output_format and the
// model DAC of track_player, track_source_formats and track_output_types).
// These end-to-end cases use one representative: stereo PCM24 at 48 kHz to
// Int24LSB, with the product's default start silence.
constexpr std::size_t sample_frames = 4099;
constexpr std::size_t sample_bytes = 3;
constexpr auto sample_output = ASIOSTInt24LSB;
constexpr auto sample_start_silence = 100ms;

Chunk pcm24_gui_samples()
{
    // Include every low bit, signed extremes, internal zeros, a sign crossing,
    // reader staging boundaries, and a nonzero tail within a device block.
    constexpr std::uint32_t sign = 0x800000, mask = 0xffffff;
    constexpr std::array<std::uint32_t, 8> edges{
        0, 1, mask, sign, sign - 1, 0x12345678 & mask, 0xabcdef01 & mask, 0};
    Chunk data{"data", Bytes(sample_frames * 2 * sample_bytes)};
    for (std::size_t frame = 0; frame < sample_frames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto bits = static_cast<std::uint32_t>(sign - 16 + frame);
            if (channel != 0) { bits = ~bits; }
            if (frame < edges.size()) { bits = edges[(frame + channel) % edges.size()]; }
            for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                data.payload[(frame * 2 + channel) * sample_bytes + byte] =
                    static_cast<std::uint8_t>(bits >> (8 * byte));
            }
        }
    }
    return data;
}

void prepare_sample_driver(GuiDriver& driver)
{
    // Play changes the clock to the WAV's 48 kHz; Close must restore 44.1 kHz.
    driver.sample_rate = 44100;
    driver.left_type = driver.right_type = driver.next_left_type = driver.next_right_type = sample_output;
}

std::size_t check_sample_capture(const std::vector<MockAsio::Block>& blocks, const Bytes& source,
    std::size_t first_frame, std::size_t silence_blocks)
{
    constexpr std::size_t width = GuiDriver::frames;
    expect("PCM capture includes the initial buffers", static_cast<int>(blocks.size() >= 2), 1);
    std::array<Bytes, 2> output;
    for (std::size_t block = 0; block < blocks.size(); ++block) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            expect("PCM ASIO buffer byte count", blocks[block][channel].size(), width * sample_bytes);
            if (block < silence_blocks) {
                for (const auto byte : blocks[block][channel]) { expect("PCM start silence", byte, 0); }
            } else {
                output[channel].append_range(blocks[block][channel]);
            }
        }
    }
    // Only trim trailing all-zero frames, never zero-valued samples inside the
    // track. This fixture ends nonzero, so missing/duplicated frames are visible
    // when the pause and resume lengths and bytes are checked together below.
    auto frames = output[0].size() / sample_bytes;
    while (frames != 0) {
        bool silent = true;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                silent &= output[channel][(frames - 1) * sample_bytes + byte] == 0;
            }
        }
        if (!silent) { break; }
        --frames;
    }
    expect("PCM capture stays inside the WAV", static_cast<int>(first_frame + frames <= sample_frames), 1);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto input = ((first_frame + frame) * 2 + channel) * sample_bytes;
            const auto expected = expected_output_sample(source.data() + input, 24, sample_output);
            for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                const auto actual = output[channel][frame * sample_bytes + byte];
                if (actual != expected[byte]) {
                    expect("PCM GUI sample frame " + std::to_string(first_frame + frame) +
                        " channel " + std::to_string(channel) + " byte " + std::to_string(byte), actual, expected[byte]);
                }
            }
        }
    }
    return frames;
}

void test_sample_transport()
{
    TempFile file;
    const auto data = pcm24_gui_samples();
    const auto path = file.write(wave({pcm24(2, 48000), data}));
    constexpr auto total_frames = sample_frames;
    constexpr auto silence_blocks = static_cast<std::size_t>(sample_start_silence.count()); // 48 frames at 48 kHz = 1 ms.
    GuiDriver driver;
    prepare_sample_driver(driver);
    Gui gui(driver, path, SW_HIDE, sample_start_silence);
    const auto play = [&] {
        gui.click(ui::control_id::play_pause);
        gui.settled(L"Playing");
    };
    const auto advance = [&] {
        for (std::size_t tick = 0; tick < silence_blocks + 3; ++tick) { driver.pulse(); }
    };
    const auto pause = [&] {
        advance();
        gui.key(VK_SPACE);
        gui.settled(L"Paused", &driver);
        gui.responsive();
    };
    gui.settled(L"Stopped");
    play();
    pause();
    play();
    gui.play_to_end(driver);
    play(); // EOF replay starts at the beginning.
    gui.play_to_end(driver);
    play();
    advance();
    gui.key(VK_ESCAPE);
    gui.settled(L"Stopped"); // Stop requires no further callback.
    play();
    gui.play_to_end(driver);
    play();
    pause();
    gui.click(ui::control_id::stop); // Stop also resets a retained pause position.
    gui.settled(L"Stopped");
    play();
    gui.play_to_end(driver);
    play();
    pause();
    expect("PCM GUI keeps a clear error field", static_cast<int>(gui.text(ui::control_id::error).empty()), 1);
    gui.request_close();
    expect("PCM paused Close succeeds", gui.finish(), 0);
    expect("PCM transport creates exactly the requested runs", driver.captures.size(), 8);
    expect("PCM transport stops every run", driver.stops, 8);
    const auto check = [&](std::size_t run, std::size_t first = 0) {
        return check_sample_capture(driver.captures.at(run), data.payload, first, silence_blocks);
    };
    const auto paused = check(0);
    expect("PCM pause retains an interior block boundary",
        static_cast<int>(paused > 0 && paused < total_frames && paused % GuiDriver::frames == 0), 1);
    expect("PCM pause and resume have no missing or duplicate frames", paused + check(1, paused), total_frames);
    expect("PCM EOF then Play reproduces the entire track", check(2), total_frames);
    for (const auto run : {3u, 5u, 7u}) {
        const auto prefix = check(run);
        expect("PCM interrupted run is an exact proper prefix", static_cast<int>(prefix > 0 && prefix < total_frames), 1);
    }
    expect("PCM Stop then Play reproduces the entire track", check(4), total_frames);
    expect("PCM paused Stop then Play reproduces the entire track", check(6), total_frames);
    expect("PCM Close restores the original sample rate", static_cast<long>(driver.sample_rate), 44100);
    driver.check_closed();
    expect("PCM Close releases the selected WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_sample_close_playing()
{
    TempFile file;
    const auto data = pcm24_gui_samples();
    const auto path = file.write(wave({pcm24(2, 48000), data}));
    GuiDriver driver;
    prepare_sample_driver(driver);
    Gui gui(driver, path);
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    driver.pulse();
    gui.request_close(); // No callback is needed after Close either.
    expect("PCM playing Close succeeds", gui.finish(), 0);
    expect("PCM playing Close stops the one stream", driver.captures.size(), 1);
    const auto frames = check_sample_capture(driver.captures.front(), data.payload, 0, 0);
    expect("PCM playing Close truncates an exact prefix", static_cast<int>(frames > 0 && frames < sample_frames), 1);
    expect("PCM playing Close restores the original sample rate", static_cast<long>(driver.sample_rate), 44100);
    driver.check_closed();
    expect("PCM playing Close releases the WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_insufficient_output_rejected()
{
    TempFile file;
    // Even silence that happens to fit in 20 bits must be rejected: compatibility
    // is a property of the input format, not of this particular sample content.
    // The 32-bit container is wide enough; only its 20 valid bits are not.
    const auto path = file.write(wave({pcm24(2, 48000), {"data", Bytes(100 * 2 * sample_bytes, 0)}}));
    GuiDriver driver;
    driver.left_type = driver.right_type = ASIOSTInt32LSB20;
    Gui gui(driver, path);
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.state(L"Error");
    expect("GUI explains the output rejection",
        static_cast<int>(gui.text(ui::control_id::error).find(L"lossless") != std::wstring::npos), 1);
    expect("rejected output disables Play", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    gui.responsive();
    gui.request_close();
    expect("rejected output returns failure", gui.finish(), 1);
    expect("output rejection precedes buffer creation", driver.creates, 0);
    expect("rejected output never starts ASIO", driver.starts, 0);
    driver.check_closed();
    expect("rejected output releases the WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_invalid_input_rejected()
{
    // A dirty unused bit in the final sample, beyond a complete two-second
    // prefetch ring: the whole file is validated before ASIO opens, so no
    // prefix may be played.
    auto data = pcm24_in32_ramp(2, 132303, 0x7ffff0);
    data.payload[data.payload.size() - 4] = 0x80;
    TempFile file;
    const auto path = file.write(wave({extensible_pcm(2, 48000, 24, 32), data}));
    GuiDriver driver;
    Gui gui(driver, path);
    gui.state(L"Error"); // Invalid data is refused while opening, before any Play.
    expect("invalid WAV explains the error", static_cast<int>(gui.text(ui::control_id::error).empty()), 0);
    expect("invalid WAV disables Play", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    gui.key(VK_SPACE);
    gui.responsive();
    gui.request_close();
    expect("invalid WAV exits with failure", gui.finish(), 1);
    expect("invalid WAV never opens ASIO", driver.references + driver.releases, 0);
    expect("invalid WAV never creates buffers", driver.creates, 0);
    expect("invalid WAV never starts", driver.starts, 0);
    expect("invalid WAV releases the WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_keyboard_focus_survives_transport()
{
    TempFile file;
    GuiDriver driver;
    Gui gui(driver, source(file), SW_SHOWNORMAL);
    gui.settled(L"Stopped");
    (void)gui.require_keyboard_focus("initial activation"); // No text-box click is required.
    gui.focus(ui::control_id::play_pause);
    gui.click(ui::control_id::play_pause);
    (void)gui.require_keyboard_focus("Play acceptance"); // The focused button was just disabled.
    gui.settled(L"Playing");
    gui.focus(ui::control_id::play_pause);
    gui.focused_key(VK_SPACE);
    gui.drain_input();
    (void)gui.require_keyboard_focus("Pause acceptance"); // Keep focus while audio drains.
    gui.settled(L"Paused", &driver);
    gui.focused_key(VK_SPACE);
    gui.settled(L"Playing");
    gui.focus(ui::control_id::stop);
    gui.focused_key(VK_ESCAPE);
    gui.drain_input();
    (void)gui.require_keyboard_focus("Stop acceptance"); // Stop disables itself when accepted.
    gui.settled(L"Stopped");
    gui.reactivate();
    (void)gui.require_keyboard_focus("reactivation"); // Restore a missing focus target.
    gui.focused_key(VK_SPACE);
    gui.settled(L"Playing");
    gui.focused_key(VK_ESCAPE);
    gui.settled(L"Stopped");
    gui.request_close();
    expect("clean GUI exits successfully", gui.finish(), 0);
    expect("focused transport starts exactly the requested three runs", driver.starts, 3);
    driver.check_closed();
}

void test_focus_policy()
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    Gui gui(driver, source(file, 48000 * 4), SW_SHOWNORMAL);
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    const auto focused = [&] { return GetDlgCtrlID(gui.focused_window()); };
    // A background test process is not always activated when its window is
    // shown. Either way the frame never hands the focus to the Buffer text.
    gui.reactivate();
    expect("initialization focuses the enabled playlist", focused(), ui::control_id::playlist);
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.focus(ui::control_id::stop);
    gui.reactivate();
    expect("reactivation restores Stop", focused(), ui::control_id::stop);
    gui.focus(ui::control_id::playlist);
    gui.reactivate();
    expect("reactivation restores the playlist", focused(), ui::control_id::playlist);
    gui.focus(ui::control_id::stop);
    gui.focused_key(VK_ESCAPE);
    gui.drain_input();
    gui.settled(L"Stopped");
    expect("disabling the focused Stop hands the focus to the playlist, not the Error text",
        focused(), ui::control_id::playlist);
    gui.focused_key(VK_RETURN); // Enter on the list plays its selected track.
    gui.settled(L"Playing");
    // Close disables the list too. From the focused Stop the keyboard then
    // stays on the frame, not on a read-only text, while Release is held.
    gui.focus(ui::control_id::stop);
    gui.request_close();
    release_call.wait();
    gui.state(L"Closing");
    expect_true("with the list disabled, disabling the focused Stop keeps the focus on the frame",
        gui.require_keyboard_focus("Close") == gui.window);
    release_call.open();
    expect("focus policy window closes cleanly", gui.finish(), 0);
    expect("Play and Enter on the list started two runs", driver.starts, 2);
    driver.check_closed();
}

void test_starting_and_rapid_commands()
{
    TempFile file;
    GuiDriver driver;
    CallGate start;
    driver.on_call = [&](MockAsio::Call call) { if (call == MockAsio::Call::start) { start.block(); } };
    Gui gui(driver, source(file));
    ReleaseGate release{start};
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    start.wait();
    gui.state(L"Starting");
    expect("Play is disabled while start is pending", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    for (unsigned i = 0; i < 80; ++i) { gui.click(ui::control_id::play_pause); gui.key(VK_SPACE); }
    gui.drain_input();
    gui.responsive();
    gui.click(ui::control_id::stop);
    for (unsigned i = 0; i < 80; ++i) { gui.click(ui::control_id::stop); gui.key(VK_ESCAPE); }
    gui.drain_input();
    gui.responsive();
    expect("Stop acceptance does not invent Stopped", static_cast<int>(gui.text(ui::control_id::status) != L"Stopped"), 1);
    start.open();
    gui.settled(L"Stopped");
    gui.request_close();
    expect("clean GUI exits successfully", gui.finish(), 0);
    expect("rapid GUI commands cannot create extra playback runs", driver.starts, 1);
    driver.check_closed();
}

void test_stopping_close(MockAsio::Call blocked_call)
{
    TempFile file;
    GuiDriver driver;
    CallGate teardown;
    driver.on_call = [&](MockAsio::Call call) { if (call == blocked_call) { teardown.block(); } };
    Gui gui(driver, source(file));
    ReleaseGate release{teardown};
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.click(ui::control_id::stop);
    teardown.wait();
    gui.state(L"Stopping");
    gui.responsive();
    expect("stopping keeps Play disabled", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    gui.request_close();
    gui.responsive();
    expect("window survives blocked driver teardown", IsWindow(gui.window), TRUE);
    teardown.open();
    expect("clean GUI exits successfully", gui.finish(), 0);
    driver.check_closed();
}

void test_release_close()
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    Gui gui(driver, source(file));
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    gui.request_close();
    release_call.wait();
    gui.state(L"Closing");
    gui.responsive();
    expect("window survives blocked driver Release", IsWindow(gui.window), TRUE);
    release_call.open();
    expect("clean GUI exits successfully", gui.finish(), 0);
    driver.check_closed();
}

void test_errors(bool initialization)
{
    TempFile file;
    GuiDriver driver;
    driver.fail_init = initialization;
    driver.fail_start = !initialization;
    Gui gui(driver, source(file));
    if (!initialization) { gui.settled(L"Stopped"); gui.click(ui::control_id::play_pause); }
    gui.state(L"Error");
    expect("GUI failure has an error explanation", static_cast<int>(!gui.text(ui::control_id::error).empty()), 1);
    if (initialization) {
        expect_true("failed driver initialization shows NO DEVICE", gui.text(ui::control_id::device) == L"Device: NO DEVICE");
    } else {
        expect_true("failed start retains the verified allocation as history",
            gui.text(ui::control_id::buffer).find(L"Last allocation: 48 frames / 1.000 ms at 48000 Hz") != std::wstring::npos);
        expect_true("failed start retains the preferred request independently",
            gui.text(ui::control_id::buffer).starts_with(L"Requested: driver preferred (BufferSize=0)"));
    }
    expect("GUI fault rejects further Play", static_cast<int>(gui.enabled(ui::control_id::play_pause)), 0);
    gui.responsive();
    gui.request_close();
    expect("faulted GUI returns failure", gui.finish(), 1);
    driver.check_closed();
}

void test_shutdown_error()
{
    TempFile file;
    GuiDriver driver;
    driver.sample_rate = 44100;
    driver.refuse_restore = true;
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.request_close();
    gui.state(L"Closed");
    expect("new shutdown error stays visible", static_cast<int>(!gui.text(ui::control_id::error).empty()), 1);
    expect("closed failure offers Close", static_cast<int>(gui.text(ui::control_id::stop).find(L"Close") != std::wstring::npos), 1);
    gui.responsive();
    gui.click(ui::control_id::stop);
    expect("cleanup failure returns failure", gui.finish(), 1);
    driver.check_closed();
}

enum class SessionEnd { playing, nested_loop, failed_restore };

void test_session_end(SessionEnd variant)
{
    TempFile audio, configuration;
    const std::string initial(selected_config);
    const auto path = configuration.write(Bytes(initial.begin(), initial.end()));
    GuiDriver driver;
    driver.sample_rate = 44100; // Play changes the clock to the WAV's 48 kHz.
    driver.refuse_restore = variant == SessionEnd::failed_restore;
    Gui gui(driver, source(audio, 48000 * 4), SW_SHOWNOACTIVATE, 0ms, path, true);
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    expect("a session-end query is never vetoed", gui.session_end(WM_QUERYENDSESSION, false), TRUE);
    expect("a cancelled session end returns zero", gui.session_end(WM_ENDSESSION, false), 0);
    gui.state(L"Playing");
    expect_true("the query and the cancellation leave playback running",
        gui.enabled(ui::control_id::play_pause) && gui.enabled(ui::control_id::stop) && driver.stops == 0);
    const auto bounds = window_bounds(gui.window);
    if (variant == SessionEnd::nested_loop) { gui.begin_modal_loop(); }
    const auto started = std::chrono::steady_clock::now();
    expect("a session end returns zero", gui.session_end(WM_ENDSESSION, true), 0);
    // The closed publication ends the wait, not the product's 4-second bound.
    expect_true("the session end returns once the device is released",
        std::chrono::steady_clock::now() - started < 3500ms);
    // It returned only after the release: the stream is stopped, the buffers
    // disposed and the driver released.
    expect("the session end stops the stream", driver.stops, 1);
    driver.check_closed();
    if (variant == SessionEnd::failed_restore) {
        // As after an external WM_QUIT, the new cleanup error awaits no acknowledgment.
        expect("a cleanup error during a session end exits with failure", gui.finish(), 1);
        return;
    }
    expect("the session end restores the original rate", static_cast<long>(driver.sample_rate), 44100);
    const auto saved = wavplay::settings::load_application_config(path).window;
    expect_true("the session end saves the window", saved.has_value());
    expect_bounds(bounds, *saved);
    if (variant == SessionEnd::nested_loop) {
        // The nested loop never waits on the product's event: the window must
        // still learn of closed there and destroy itself.
        gui.wait_until([&] { return IsWindow(gui.window) == FALSE; }, "window destruction inside the nested loop");
        gui.end_modal_loop();
    }
    expect("a session end exits like an external quit", gui.finish(), 0);
}

// Close came first and the UI has consumed its notification, so only the
// notification after the session-end wait can tell a nested loop of closed.
void test_session_end_after_close()
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    Gui gui(driver, source(file), SW_SHOWNOACTIVATE);
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    gui.request_close();
    release_call.wait();
    gui.state(L"Closing");
    // The loop retrieves every earlier posted notification before entering.
    gui.begin_modal_loop();
    TestThread<LRESULT> ended([&] { return gui.session_end(WM_ENDSESSION, true); });
    // The product unsets its posting target before it sets the block reason,
    // so the closed publication now sets only the event.
    gui.wait_until([&] { return gui.blocking_shutdown(); }, "session-end wait");
    release_call.open();
    expect("a session end after Close returns zero", ended.get(), 0);
    gui.wait_until([&] { return IsWindow(gui.window) == FALSE; }, "window destruction inside the nested loop");
    gui.end_modal_loop();
    expect("a session end after Close exits like an external quit", gui.finish(), 0);
    driver.check_closed();
}

// A driver whose Release does not return cannot hold a logoff past the
// product's 4-second bound: WM_ENDSESSION returns at the deadline without
// the release and removes its block reason. The release completes later.
void test_session_end_deadline()
{
    TempFile file;
    GuiDriver driver;
    CallGate release_call;
    driver.on_release = [&] { release_call.block(); };
    Gui gui(driver, source(file), SW_SHOWNOACTIVATE);
    ReleaseGate release{release_call};
    gui.settled(L"Stopped");
    const auto started = std::chrono::steady_clock::now();
    TestThread<LRESULT> ended([&] { return gui.session_end(WM_ENDSESSION, true); });
    release_call.wait();
    gui.wait_until([&] { return gui.blocking_shutdown(); }, "session-end wait");
    expect("a session end at its deadline returns zero", ended.get(), 0);
    // The product's deadline starts after `started` and the wait ends only
    // once it has passed, so the lower bound needs no margin.
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect_true("the session end waits out its 4-second bound", elapsed >= 4s);
    expect_true("the session end returns at its bound without the release", elapsed < 4s + hang_budget(2s));
    expect_true("the session end removes its block reason at the deadline", !gui.blocking_shutdown());
    gui.state(L"Closing");
    release_call.open();
    expect("a release after the deadline still exits like an external quit", gui.finish(), 0);
    driver.check_closed();
}

void test_session_end_after_cleanup_error()
{
    TempFile file;
    GuiDriver driver;
    driver.sample_rate = 44100;
    driver.refuse_restore = true;
    Gui gui(driver, source(file));
    gui.settled(L"Stopped");
    gui.click(ui::control_id::play_pause);
    gui.settled(L"Playing");
    gui.request_close();
    gui.state(L"Closed");
    expect_true("the cleanup error awaits acknowledgment",
        !gui.text(ui::control_id::error).empty() && IsWindow(gui.window));
    expect("a session end returns zero", gui.session_end(WM_ENDSESSION, true), 0);
    expect("a session end needs no acknowledgment and keeps the fault's exit code", gui.finish(), 1);
    driver.check_closed();
}

} // namespace

// Exit codes: 0 the expected --wav path, 1 another result, 2 a malformed request,
// 3 an argument error. The expected path arrives as UTF-16 hex digits, or "-"
// for an expected argument error, so that its own parsing is unambiguous.
int wavplay_test::run_player_options_child(std::span<wchar_t* const> operands)
{
    if (operands.empty()) { return 2; }
    try {
        const std::wstring_view hex(operands[0]);
        std::optional<std::wstring> expected;
        if (hex != L"-") {
            if (hex.size() % 4 != 0) { return 2; }
            expected.emplace();
            for (std::size_t index = 0; index < hex.size(); index += 4) {
                expected->push_back(static_cast<wchar_t>(std::stoul(std::wstring(hex.substr(index, 4)), nullptr, 16)));
            }
        }
        const std::vector<std::wstring_view> arguments(operands.begin() + 1, operands.end());
        try {
            const auto options = ui::parse_player_options(arguments);
            return expected && options.path == *expected ? 0 : 1;
        } catch (const std::invalid_argument&) {
            return 3;
        }
    } catch (...) {
        return 2;
    }
}

int wavplay_test::run_player_window_tests()
{
    const UiDesktopGuard desktop;
    TestSuite suite;
    try {
        suite_process = process_state();
    } catch (...) {
        // Left empty: the case that compares with it then fails.
    }
    suite.run("GUI command line defaults to playlist and keeps the single-WAV entry point", test_arguments);
    suite.run("GUI --wav paths keep spaces and quotes as the CRT argv parser splits them", test_command_line_quoting);
    suite.run("GUI message failures include a bounded diagnostic and source location", test_message_failure_diagnostic);
    suite.run("GUI command availability preserves pending receipts, refusals and faults", test_transport_availability);
    suite.run("GUI timing formats all rates and 64-bit lengths without overflow", test_playback_time_format);
    suite.run("GUI integer text equals the former std::format text and omits unverified rates", test_integer_text);
    suite.run("GUI text formatters reuse storage across steady updates and state boundaries", test_reused_text_storage);
    suite.run_timed("GUI text updates reuse storage and resynchronize the display cache", test_control_text_storage_reuse);
    suite.run("GUI label redraw equals a complete draw and touches only the changed characters", test_changed_label_redraw);
    suite.run_timed("GUI time field paints as a standard label does", test_time_field_matches_standard_label);
    suite.run_timed("GUI time field draws a new text at once without invalidating its row", test_time_field_redraws_on_screen);
    suite.run_timed("GUI timing follows live PCM, start silence, Pause, Stop and completion", test_single_track_timing);
    suite.run_timed("GUI timing follows the playing track and adopts stopped selection and refused tracks", test_playlist_timing_selection_and_refusal);
    suite.run_timed("GUI displays the immutable filtered playlist and plays all rows gaplessly", test_playlist_display_and_continuous_playback);
    suite.run_timed("GUI playlist selection supports click Enter double-click Pause and Stop", test_playlist_selection_and_transport);
    suite.run_timed("GUI selects a refused next track and accepts another selection", test_playlist_next_refusal);
    suite.run_timed("GUI disables empty playlists and reports invalid UTF-8", test_playlist_empty_and_invalid_utf8);
    suite.run("GUI diagnostics preserve Unicode and Windows error messages", test_error_messages);
    suite.run_timed("GUI stays responsive during initialization and accepts Close", test_initializing_close);
    suite.run_timed("GUI buttons and child-targeted keyboard follow actual playback", test_transport_and_keyboard);
    suite.run_timed("GUI reinitializes notifications and enables Play only after stopped", test_notification_reinitialization);
    suite.run_timed("GUI shows NO DEVICE after failed driver reinitialization", test_failed_reinitialization_device_display);
    suite.run_timed("GUI shows configured and allocated buffer sizes without inventing capabilities", test_configured_buffer_display);
    suite.run_timed("GUI keeps buffer capabilities after invalid or refused allocation", test_rejected_buffer_display);
    suite.run_timed("GUI displays large capabilities and output pairs independently of long device names", test_large_buffer_capabilities_display);
    suite.run_timed("GUI displays startup configuration errors without opening ASIO", test_invalid_configuration_display);
    suite.run_timed("GUI reads the configured device and persists normal bounds without losing manual edits", test_window_persistence);
    suite.run_timed("GUI retains normal bounds through maximize minimize and restart", test_maximized_and_minimized_persistence);
    suite.run_timed("GUI stays quiet on progress while minimized and shows it on restore", test_quiet_progress_while_minimized);
    suite.run_timed("GUI progress refresh rewrites only the time text", test_progress_refresh_rewrites_only_time);
    suite.run_timed("GUI progress refresh updates selection and duration at a track boundary", test_progress_refresh_at_track_boundary);
    suite.run_timed("GUI failed UI renders once per revision, not on unrelated wakes", [] {
        test_failed_ui_renders_once_per_revision(false);
    });
    suite.run_timed("GUI failure during playback releases audio before offering Close", [] {
        test_failed_ui_renders_once_per_revision(true);
    });
    suite.run_timed("GUI closes after an external DestroyWindow that discards a queued notification", [] {
        test_external_destroy_with_queued_notification(false);
    });
    suite.run_timed("GUI reports a cleanup fault after an external DestroyWindow in its exit code", [] {
        test_external_destroy_with_queued_notification(true);
    });
    suite.run_timed("GUI corrects offscreen and oversized saved window bounds", test_offscreen_placement);
    suite.run_timed("GUI shows NO DEVICE and creates an unselected first-run settings file", test_missing_device_display);
    suite.run_timed("GUI reports settings-save failures after releasing the device", test_settings_save_error_display);
    suite.run_timed("PCM24 stereo GUI transport preserves samples with start silence", test_sample_transport);
    suite.run_timed("PCM24 stereo GUI closes during playback", test_sample_close_playing);
    suite.run_timed("PCM24 GUI rejects an output with insufficient precision before creating buffers", test_insufficient_output_rejected);
    suite.run_timed("Extensible GUI rejects dirty final padding before opening ASIO", test_invalid_input_rejected);
    suite.run_timed("GUI preserves real keyboard focus through activation and transport", test_keyboard_focus_survives_transport);
    suite.run_timed("GUI supports Tab Enter mnemonics and modifier-safe transport", test_tab_enter_mnemonics_and_modifiers);
    suite.run_timed("GUI prefers the playlist and restores the focused control across activation", test_focus_policy);
    suite.run_timed("GUI accepts transport only from its enabled buttons", test_command_sources);
    suite.run_timed("GUI rescales system fonts and releases window resources", test_dpi_settings_and_resources);
    suite.run_timed("GUI skips unchanged fonts and control placement", test_unchanged_settings_and_layout);
    suite.run_timed("GUI disables only its read-only UI thread input method", test_read_only_input_method_scope);
    suite.run_timed("GUI requests its configured QoS only on the UI thread while the window exists", test_ui_thread_speed_scope);
    suite.run_timed("GUI and its controller preserve inherited low memory priority", [] {
        with_low_process_memory_priority(test_ui_thread_speed_scope);
    });
    suite.run_timed("GUI receives state changes inside a modal message loop", test_state_changes_inside_modal_loop);
    suite.run_timed("GUI failed creation leaves no stale quit message", test_failed_creation_does_not_post_quit);
    suite.run_timed("GUI thread quit waits for release and preserves exit code", [] { test_thread_quit(false); });
    suite.run_timed("GUI thread quit reports cleanup faults without waiting for acknowledgment", [] { test_thread_quit(true); });
    suite.run_timed("GUI start and rapid input cannot invent state or queue replay", test_starting_and_rapid_commands);
    suite.run_timed("GUI stays responsive during ASIO stop and accepts Close", [] { test_stopping_close(MockAsio::Call::stop); });
    suite.run_timed("GUI stays responsive during ASIO dispose and accepts Close", [] { test_stopping_close(MockAsio::Call::dispose); });
    suite.run_timed("GUI waits for driver Release without freezing", test_release_close);
    suite.run_timed("GUI displays initialization errors and closes", [] { test_errors(true); });
    suite.run_timed("GUI displays playback errors and closes", [] { test_errors(false); });
    suite.run_timed("GUI preserves a new shutdown error until acknowledgment", test_shutdown_error);
    suite.run_timed("GUI releases the device and saves the window before a session end returns",
        [] { test_session_end(SessionEnd::playing); });
    suite.run_timed("GUI completes a session end inside a nested message loop", [] { test_session_end(SessionEnd::nested_loop); });
    suite.run_timed("GUI ends the session with a new cleanup error and exit code 1",
        [] { test_session_end(SessionEnd::failed_restore); });
    suite.run_timed("GUI renders closed inside a nested loop after a session end that follows Close",
        test_session_end_after_close);
    suite.run_timed("GUI ends the session at its bound while the driver's Release is held", test_session_end_deadline);
    suite.run_timed("GUI ends the session without awaiting a shown cleanup error", test_session_end_after_cleanup_error);
    return suite.result("Player window");
}
