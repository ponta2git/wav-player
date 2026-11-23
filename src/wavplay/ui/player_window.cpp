#include "wavplay/ui/player_window.h"

#include "wavplay/ui/error_message.h"
#include "wavplay/ui/player_controls.h"
#include "wavplay/ui/player_text.h"
#include "wavplay/ui/single_instance.h"
#include "wavplay/control/playback_controller.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/core/thread_hints.h"
#include "wavplay/core/win32.h"

#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

namespace wavplay::ui {
namespace {

using control::PlaybackCommand;
using control::PlaybackSnapshot;
using control::PlaybackState;
constexpr UINT changed_message = WM_APP + 1;
// Bounds the WM_ENDSESSION wait, so that a hung driver cannot hold up a
// logoff or shutdown for long.
constexpr std::chrono::seconds session_end_timeout{4};

MONITORINFO monitor_info(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) { core::throw_last_error("GetMonitorInfoW(player)"); }
    return info;
}

// A coalesced window message wakes the outer loop (QS_ALLINPUT) and is also
// dispatched inside Windows' modal move/size/menu loops, so a change needs no
// second wake. The event wakes the UI only when no message can carry the
// change (no target window, or a failed post) and for the UI's own forced
// refreshes. Both objects outlive the controller, including its last change
// notification.
struct StateNotification {
    core::UniqueHandle event{core::create_event()};
    std::atomic<HWND> target{};
    std::atomic<bool> posted{};
    // Only a hint to avoid taking the controller lock on unrelated UI traffic.
    // snapshot() remains the authoritative synchronized copy of the state.
    std::atomic<std::uint64_t> revision{1};
    // Written by the UI thread from WM_SIZE. The listener runs under the
    // controller's lock, so it reads this flag instead of calling IsIconic.
    std::atomic<bool> minimized{};

    static void changed(void* context, std::uint64_t revision) noexcept
    {
        auto& self = *static_cast<StateNotification*>(context);
        self.revision.store(revision, std::memory_order_release);
        self.signal();
    }

    // Progress alone does not wake a minimized window: the revision is kept,
    // and leaving the minimized state forces one synchronized refresh. Every
    // other change, closed included, still signals while minimized.
    static void progressed(void* context, std::uint64_t revision) noexcept
    {
        auto& self = *static_cast<StateNotification*>(context);
        self.revision.store(revision, std::memory_order_release);
        if (!self.minimized.load()) {
            self.signal();
        }
    }

    void signal() noexcept
    {
        if (const auto window = target.load()) {
            // Until the queued message is consumed, later changes share it.
            if (!posted.exchange(true) && !PostMessageW(window, changed_message, 0, 0)) {
                posted.store(false);
                SetEvent(event.get());
            }
        } else {
            // After the window's destruction, or during the session-end wait,
            // which dispatches no posted message.
            SetEvent(event.get());
        }
    }
};

class PlayerWindow {
public:
    explicit PlayerWindow(SingleInstance* instance) : instance_(instance) {}
    PlayerWindow(const PlayerWindow&) = delete; // The HWND and the listener keep this address.
    PlayerWindow& operator=(const PlayerWindow&) = delete;
    ~PlayerWindow()
    {
        notification_.target.store(nullptr);
        controller_.reset(); // run() waited for closed, or construction never started a worker.
        if (window_) {
            DestroyWindow(window_);
        }
    }

    void create(const PlayerOptions& options, asio::ActivateDriver activate, const asio::DeviceSettings& settings)
    {
        const INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&common)) {
            core::throw_last_error("InitCommonControlsEx");
        }
        static std::once_flag registered;
        std::call_once(registered, [] {
            WNDCLASSW definition{};
            definition.lpfnWndProc = window_proc;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            definition.lpszClassName = player_window_class;
            if (!RegisterClassW(&definition)) {
                core::throw_last_error("RegisterClassW(player)");
            }
        });
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, player_window_class, L"WAV Player",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 680, 430,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!window_) {
            core::throw_last_error("CreateWindowExW(player)");
        }
        controls_.create(window_, options);
        auto size = controls_.window_size(true);
        const auto area = monitor_info(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)).rcWork;
        size.cx = std::min(size.cx, area.right - area.left);
        size.cy = std::min(size.cy, area.bottom - area.top);
        RECT initial{};
        if (!GetWindowRect(window_, &initial)) { core::throw_last_error("GetWindowRect(initial player)"); }
        const auto left = std::clamp(initial.left, area.left, area.right - size.cx);
        const auto top = std::clamp(initial.top, area.top, area.bottom - size.cy);
        if (!SetWindowPos(window_, nullptr, left, top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE)) {
            core::throw_last_error("SetWindowPos(player)");
        }
        controls_.layout();
        if (startup_error_) {
            std::rethrow_exception(startup_error_);
        }
        notification_.target.store(window_);
        // Last potentially throwing startup operation. From here the message
        // pump owns all errors and keeps processing while the worker closes.
        controller_ = std::make_unique<control::PlaybackController>(options.driver, options.path,
            activate, settings, control::ChangeListener{StateNotification::changed, &notification_,
                StateNotification::progressed}, options.settings_path, options.playlist_path);
    }

    int run(int show_command)
    {
        show_command_ = show_command;
        // Settings arrive independently of driver initialization. Place the
        // window before first showing it, without blocking this message pump.
        SetEvent(notification_.event.get());
        bool wait_failed{}, wait_failure_shown{};
        for (;;) {
            const HANDLE event = notification_.event.get();
            const auto wake = MsgWaitForMultipleObjectsEx(wait_failed ? 0 : 1, wait_failed ? nullptr : &event,
                wait_failed ? 50 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            try {
                // A failed wait polls once after 50 ms instead of retrying at
                // once; the next wait that does not fail resumes the event wait.
                // Only the first failure goes to fail_ui(), which is terminal:
                // a lasting failure must not redraw the Error box every poll.
                wait_failed = wake == WAIT_FAILED;
                if (wait_failed && !std::exchange(wait_failure_shown, true)) {
                    core::throw_last_error("MsgWaitForMultipleObjectsEx(player)");
                }
                MSG message{};
                // Same bound as the control STA's pump (core::message_batch_limit):
                // queued input cannot delay a state refresh.
                for (unsigned count = 0; count < core::message_batch_limit &&
                    PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                    if (message.message == WM_QUIT) {
                        if (!quit_code_.has_value()) {
                            quit_code_ = static_cast<int>(message.wParam);
                        }
                        request_close();
                    } else if (!translate(message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                refresh();
                if (!window_ && controller_->closed()) {
                    // Closed is final. A cleanup fault can be published with
                    // it after refresh() copied the state above, and without
                    // a window the exit code is its only report.
                    exit_failure_ |= controller_->command_state().faulted;
                    break;
                }
            } catch (...) {
                fail_ui(std::current_exception());
            }
        }
        return exit_failure_ ? 1 : quit_code_.value_or(0);
    }

private:
    SingleInstance* instance_{};
    StateNotification notification_;
    std::unique_ptr<control::PlaybackController> controller_;
    PlayerControls controls_;
    HWND window_{};
    // The control that had the focus when the window was last deactivated;
    // the next WM_SETFOCUS restores it once if it can still take the focus.
    HWND saved_focus_{};
    // refresh()'s complete view of the state. Normal progress updates only its
    // scalar position/revision; full refreshes reuse its strings' and vectors'
    // storage. Other readers take their own snapshot().
    PlaybackSnapshot latest_;
    // Only refresh() uses this scratch string. Its reentry guard also protects
    // the buffer through SetWindowTextW; successful updates return the old
    // control text's storage for the next formatter instead of freeing it.
    std::wstring text_buffer_;
    std::uint64_t rendered_revision_{}, stop_receipt_{};
    PlaybackCommand pending_command_{PlaybackCommand::play};
    bool pending_selection_{};
    // Close state, set once and never cleared:
    //   close_requested_     controller_->close() was submitted (request_close).
    //   close_acknowledged_  the first close request (Close, Stop as Close, quit, session
    //                        end, destroy or UI failure) after closed; a cleanup error may now exit.
    //   destroying_          WM_DESTROY arrived; refresh() only forwards Close.
    //   exit_failure_        a fault or UI failure was seen; run() returns 1.
    //   ui_failed_           the window cannot render; only Close stays enabled and
    //                        texts go through show_text(), never the text() cache.
    // quit_code_ and the *_before_close_ members are described where declared.
    bool close_requested_{}, close_acknowledged_{}, destroying_{}, exit_failure_{}, ui_failed_{};
    bool rendering_{}; // refresh() is running; a nested refresh returns at once.
    // The window left the minimized state: the next refresh() reads the
    // synchronized snapshot even if the revision hint looks rendered.
    bool refresh_forced_{};
    bool placed_{}, maximized_{};
    int show_command_{SW_SHOWNORMAL};
    // From the first WM_QUIT, or 0 from a session end if none came first. Also
    // skips the cleanup-error acknowledgment.
    std::optional<int> quit_code_;
    std::exception_ptr startup_error_;
    // The fault already visible when the GUI requested Close. If the controller
    // closed by itself, these keep their defaults, so any fault counts as new.
    std::exception_ptr error_before_close_;
    playback::PlaybackResult result_before_close_{playback::PlaybackResult::stopped};

    void restore_placement(const PlaybackSnapshot& snapshot)
    {
        if (placed_ || (!snapshot.settings_loaded && !snapshot.faulted())) { return; }
        placed_ = true; // Placement APIs can reenter the window procedure.
        if (const auto& saved = snapshot.window) {
            const RECT requested{saved->x, saved->y, saved->x + saved->width, saved->y + saved->height};
            const auto area = monitor_info(MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST)).rcWork;
            // Repeat only across a real DPI change: moving to the destination
            // monitor can synchronously update the font and its minimum size.
            for (unsigned pass = 0; pass < 2; ++pass) {
                const auto dpi = GetDpiForWindow(window_);
                const auto minimum = controls_.window_size();
                const auto width = std::clamp<LONG>(saved->width, std::min(minimum.cx, area.right - area.left),
                    area.right - area.left);
                const auto height = std::clamp<LONG>(saved->height, std::min(minimum.cy, area.bottom - area.top),
                    area.bottom - area.top);
                const auto x = std::clamp<LONG>(saved->x, area.left, area.right - width);
                const auto y = std::clamp<LONG>(saved->y, area.top, area.bottom - height);
                if (!SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE)) {
                    core::throw_last_error("SetWindowPos(restore settings)");
                }
                if (GetDpiForWindow(window_) == dpi) { break; }
            }
            maximized_ = saved->maximized;
        }
        const bool saved_maximized = maximized_;
        const bool minimized = show_command_ == SW_SHOWMINIMIZED || show_command_ == SW_MINIMIZE ||
            show_command_ == SW_SHOWMINNOACTIVE;
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(window_, &placement)) { core::throw_last_error("GetWindowPlacement(initial display)"); }
        placement.showCmd = static_cast<UINT>(minimized ? SW_SHOWMINIMIZED :
            saved_maximized && show_command_ != SW_HIDE ? SW_SHOWMAXIMIZED : show_command_);
        if (minimized && saved_maximized) { placement.flags |= WPF_RESTORETOMAXIMIZED; }
        // The first ShowWindow call can ignore its argument in favor of the
        // launcher's STARTUPINFO. SetWindowPlacement applies our saved state.
        if (!SetWindowPlacement(window_, &placement)) { core::throw_last_error("SetWindowPlacement(initial display)"); }
        if (minimized || show_command_ == SW_HIDE) { maximized_ = saved_maximized; }
        controls_.ensure_focus();
        // Publish only after applying the startup placement. An activation
        // request must not subsequently be undone by a minimized launch state.
        if (instance_) { instance_->attach(window_); }
    }

    settings::WindowPlacement capture_placement() const
    {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(window_, &placement)) { core::throw_last_error("GetWindowPlacement(player)"); }
        auto bounds = placement.rcNormalPosition;
        // WINDOWPLACEMENT uses workspace coordinates for a regular top-level
        // window. The INI and SetWindowPos use physical screen coordinates.
        const auto info = monitor_info(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
        OffsetRect(&bounds, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
        return {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, maximized_};
    }

    void request_close()
    {
        if (!controller_) {
            return;
        }
        if (controller_->closed()) {
            if (close_acknowledged_) {
                return;
            }
            close_acknowledged_ = true;
        } else if (!close_requested_) {
            std::optional<settings::WindowPlacement> placement;
            if (!ui_failed_) {
                const auto snapshot = controller_->snapshot();
                error_before_close_ = snapshot.error;
                result_before_close_ = snapshot.result;
                if (window_ && placed_ && snapshot.settings_loaded) { placement = capture_placement(); }
            }
            // A UI failure may be bad_alloc from snapshot or text formatting.
            // Close then needs no snapshot copy or placement capture: send it
            // directly so resource exhaustion cannot leave playback running.
            (void)controller_->close(placement);
            close_requested_ = true;
        } else {
            return;
        }
        rendered_revision_ = 0;
        SetEvent(notification_.event.get());
    }

    // Logoff, shutdown or a Restart Manager close can end the process as soon
    // as WM_ENDSESSION returns. Close as for an external WM_QUIT, then wait a
    // bounded time until the controller has released the device, restored
    // its rate and saved the window: the only wait of this thread for the
    // controller. It dispatches sent messages only, so a driver's SendMessage
    // cannot deadlock and no posted or input message reenters.
    void end_session()
    {
        if (!controller_) { return; }
        if (!quit_code_) { quit_code_ = 0; }
        try {
            request_close(); // Captures the placement, or acknowledges a shown cleanup error.
        } catch (...) {
            fail_ui(std::current_exception()); // Still requests Close, without the placement.
        }
        // Completion needs no snapshot allocation. Keep a failed lock query
        // inside the bounded wait as well, so nothing between unsetting and
        // restoring the target below can throw.
        const auto closed = [this]() noexcept {
            try { return controller_->closed(); } catch (...) { return false; }
        };
        if (closed()) { return; }
        notification_.target.store(nullptr); // A posted change would stay queued: use the event.
        (void)ShutdownBlockReasonCreate(window_, L"Releasing the audio device...");
        const HANDLE event = notification_.event.get();
        const auto deadline = std::chrono::steady_clock::now() + session_end_timeout;
        for (;;) {
            MSG message{};
            (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            const auto now = std::chrono::steady_clock::now();
            if (closed() || now >= deadline ||
                MsgWaitForMultipleObjectsEx(1, &event, static_cast<DWORD>(
                    std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count()),
                    QS_SENDMESSAGE, 0) == WAIT_FAILED) {
                break;
            }
        }
        (void)ShutdownBlockReasonDestroy(window_);
        if (window_) {
            notification_.target.store(window_);
            notification_.signal(); // Renders closed in whichever loop runs next, modal or not.
        }
    }

    void fail_ui(std::exception_ptr error) noexcept
    {
        exit_failure_ = true;
        ui_failed_ = true;
        rendered_revision_ = 0; // Render the failed state once, even without a new revision.
        if (!controller_) {
            startup_error_ = error;
        }
        if (window_ && controls_.ready()) {
            // A placement failure must not leave the cleanup error waiting in
            // an invisible window. SW_HIDE is an explicit caller/test request.
            if (!IsWindowVisible(window_) && show_command_ != SW_HIDE) { ShowWindow(window_, SW_SHOWNORMAL); }
            // Literal fallback remains usable even if the failure was allocation.
            const wchar_t* fallback = L"The window could not update. Close after playback has been released.";
            try {
                controls_.show_text(control_id::error, exception_message(error).c_str());
            } catch (...) {
                controls_.show_text(control_id::error, fallback);
            }
        }
        try { request_close(); } catch (...) {}
        SetEvent(notification_.event.get());
    }

    // Once rendering has failed, only shutdown status and Close remain. Do
    // not copy a snapshot here: the failure may be an allocation failure that
    // persists through driver release. Capture the revision before reading
    // closed(), so a concurrent final publication can never be marked rendered
    // with an older Closing state.
    void refresh_failed()
    {
        const auto revision = notification_.revision.load(std::memory_order_acquire);
        const bool closed = controller_->closed();
        if (!window_ || destroying_) {
            request_close();
            return;
        }
        if (closed && (close_acknowledged_ || quit_code_.has_value())) {
            DestroyWindow(window_);
            return;
        }
        if (revision == rendered_revision_) { return; }
        controls_.show_text(control_id::status, closed ? L"Closed" : L"Closing");
        controls_.enable(control_id::play_pause, false);
        controls_.enable(control_id::playlist, false);
        controls_.show_text(control_id::stop, closed ? L"&Close" : L"&Stop");
        controls_.enable(control_id::stop, closed);
        rendered_revision_ = revision;
    }

    void refresh()
    {
        if (!controller_ || rendering_ || (!refresh_forced_ &&
            notification_.revision.load(std::memory_order_acquire) == rendered_revision_)) {
            return;
        }
        rendering_ = true;
        const bool forced = std::exchange(refresh_forced_, false);
        struct RenderingScope { bool& flag; ~RenderingScope() { flag = false; } } scope{rendering_};
        if (ui_failed_) {
            refresh_failed();
            return;
        }
        const bool close_requested_before = close_requested_;
        const bool close_acknowledged_before = close_acknowledged_;
        // The only update of latest_: placement and text updates below can
        // reenter this window procedure, but a nested refresh returns above.
        // Only a successfully rendered baseline may avoid the full copy. The
        // controller checks it and the current state together under its lock.
        // If a copy throws, rendered_revision_ keeps its old value.
        const bool steady_progress = controller_->refresh_snapshot(latest_,
            forced || close_requested_ ? 0 : rendered_revision_);
        const auto& snapshot = latest_;
        const bool fault = snapshot.faulted();
        exit_failure_ |= fault;
        if (!window_ || destroying_) {
            request_close();
            return;
        }
        if (!close_requested_) { restore_placement(snapshot); }
        // Placement dispatches sent messages; a nested handler may have failed
        // the UI even though the outer placement call itself succeeded.
        if (ui_failed_) {
            refresh_failed();
            return;
        }
        const bool closed = snapshot.state == PlaybackState::closed;
        const bool cleanup_error = closed && fault &&
            (snapshot.error != error_before_close_ || snapshot.result != result_before_close_);
        if (closed && (!cleanup_error || close_acknowledged_ || quit_code_.has_value())) {
            DestroyWindow(window_);
            return;
        }
        if (!forced && snapshot.revision == rendered_revision_) {
            return;
        }
        // Sent messages during a text/focus update can fail or destroy the UI,
        // request Close, or force another refresh. Do not overwrite the nested
        // handler's invalidation, even when the outer Win32 call returns normally.
        const auto mark_rendered = [&] {
            if (window_ && !destroying_ && !ui_failed_ && !refresh_forced_ &&
                close_requested_ == close_requested_before && close_acknowledged_ == close_acknowledged_before) {
                rendered_revision_ = snapshot.revision;
            }
        };
        // Track/state changes and forced refreshes already took the full-copy
        // path. A nested Close can still require the full controls below.
        if (steady_progress && !close_requested_) {
            playback_time_text(text_buffer_, snapshot.position_frames, snapshot.frame_count, snapshot.sample_rate);
            controls_.text_reuse(control_id::time, text_buffer_);
            mark_rendered();
            return;
        }
        auto error = error_text(snapshot);
        const auto operations = available(snapshot.command_state(), stop_receipt_);
        controls_.text(control_id::status, state_text(snapshot.state));
        playback_time_text(text_buffer_, snapshot.position_frames, snapshot.frame_count, snapshot.sample_rate);
        controls_.text_reuse(control_id::time, text_buffer_);
        device_text(text_buffer_, snapshot);
        controls_.text_reuse(control_id::device, text_buffer_);
        output_text(text_buffer_, snapshot);
        controls_.text_reuse(control_id::output, text_buffer_);
        buffer_text(text_buffer_, snapshot);
        controls_.text_reuse(control_id::buffer, text_buffer_);
        controls_.text(control_id::error, std::move(error));
        controls_.playlist(snapshot.playlist);
        playlist_label_text(text_buffer_, snapshot);
        controls_.text_reuse(control_id::playlist_label, text_buffer_);
        if (snapshot.accepted == snapshot.settled) {
            const auto selected = snapshot.selected_index ? static_cast<LRESULT>(*snapshot.selected_index) : LB_ERR;
            const auto list = controls_.child(control_id::playlist);
            if (SendMessageW(list, LB_GETCURSEL, 0, 0) != selected) {
                SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
            }
        }
        const wchar_t* selected_path = snapshot.playlist && snapshot.selected_index &&
            *snapshot.selected_index < snapshot.playlist->size() ?
            (*snapshot.playlist)[*snapshot.selected_index].c_str() : L"";
        controls_.text(control_id::file, selected_path);
        controls_.text(control_id::pending,
            pending_text(snapshot, cleanup_error, close_requested_, pending_selection_, pending_command_));
        controls_.text(control_id::play_pause, snapshot.state == PlaybackState::playing ? L"&Pause" : L"&Play");
        controls_.text(control_id::stop, cleanup_error ? L"&Close" : L"&Stop");
        controls_.enable(control_id::play_pause, !close_requested_ && operations.toggle);
        controls_.enable(control_id::stop, cleanup_error || (!close_requested_ && operations.stop));
        controls_.enable(control_id::playlist, !close_requested_ && operations.select);
        controls_.ensure_focus();
        mark_rendered();
    }

    void command(int id)
    {
        if (!controller_) {
            return;
        }
        if (ui_failed_) {
            if (id == control_id::stop && controller_->closed()) { request_close(); }
            return;
        }
        const auto snapshot = controller_->command_state();
        if (id == control_id::stop && snapshot.state == PlaybackState::closed) {
            request_close();
            return;
        }
        if (close_requested_) {
            return;
        }
        const auto operations = available(snapshot, stop_receipt_);
        PlaybackCommand command;
        if (id == control_id::play_pause && operations.toggle) {
            command = snapshot.state == PlaybackState::playing ? PlaybackCommand::pause : PlaybackCommand::play;
        } else if (id == control_id::stop && operations.stop) {
            command = PlaybackCommand::stop;
        } else {
            return;
        }
        const auto receipt = controller_->submit(command);
        if (receipt) {
            pending_selection_ = false;
            pending_command_ = command;
            if (command == PlaybackCommand::stop) {
                stop_receipt_ = receipt;
            }
        }
        refresh();
    }

    void select_track(bool play)
    {
        if (!controller_ || close_requested_ || ui_failed_) { return; }
        if (!available(controller_->command_state(), stop_receipt_).select) { return; }
        const auto selection = SendMessageW(controls_.child(control_id::playlist), LB_GETCURSEL, 0, 0);
        if (selection == LB_ERR) { return; }
        if (controller_->select_track(static_cast<std::size_t>(selection), play)) {
            pending_selection_ = !play;
            pending_command_ = PlaybackCommand::play;
        }
        refresh();
    }

    bool translate(MSG& message)
    {
        if (!window_ || !message.hwnd || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) {
            return false;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
            (message.wParam == VK_SPACE || message.wParam == VK_ESCAPE ||
                (message.wParam == VK_RETURN && message.hwnd == controls_.child(control_id::playlist)))) {
            const bool modified = (GetKeyState(VK_CONTROL) & 0x8000) ||
                (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000);
            if (message.message == WM_KEYDOWN && !(message.lParam & (1LL << 30)) && !modified) {
                if (message.wParam == VK_RETURN) { select_track(true); }
                else { command(message.wParam == VK_SPACE ? control_id::play_pause : control_id::stop); }
            }
            return true; // Prevent the focused button from treating modified Space as a click.
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (instance_ && instance_->handle_activation(message)) { return 1; }
        switch (message) {
        case DM_GETDEFID: {
            // IsDialogMessage can promote a focused pushbutton. A plain window
            // has no DefDlgProc default-ID storage, so report the current button
            // explicitly instead of letting Enter fall back to the unrelated IDOK.
            const auto focus = GetFocus();
            for (const auto id : {control_id::play_pause, control_id::stop}) {
                const auto item = controls_.child(id);
                if (item && focus == item && IsWindowEnabled(item)) {
                    return MAKELRESULT(id, DC_HASDEFID);
                }
            }
            for (const auto id : {control_id::play_pause, control_id::stop}) {
                if (const auto item = controls_.child(id); item && IsWindowEnabled(item)) {
                    return MAKELRESULT(id, DC_HASDEFID);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            const auto id = LOWORD(wparam);
            if (id == control_id::playlist && reinterpret_cast<HWND>(lparam) == controls_.child(id) &&
                (HIWORD(wparam) == LBN_SELCHANGE || HIWORD(wparam) == LBN_DBLCLK)) {
                select_track(HIWORD(wparam) == LBN_DBLCLK);
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && (id == control_id::play_pause || id == control_id::stop) &&
                reinterpret_cast<HWND>(lparam) == controls_.child(id)) {
                command(id);
                return 0;
            }
            break;
        }
        case changed_message:
            // A read-modify-write, so that it synchronizes with the publisher's
            // exchange and orders refresh()'s read of the revision after it.
            (void)notification_.posted.exchange(false);
            refresh();
            return 0;
        case WM_CLOSE:
            request_close();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE; // Never veto, and do nothing until the session really ends.
        case WM_ENDSESSION:
            if (wparam) { end_session(); }
            return 0;
        case WM_ACTIVATE:
            // As DefDlgProc does, remember the focused control across a
            // deactivation. DefWindowProc then focuses the frame on activation.
            if (LOWORD(wparam) == WA_INACTIVE) {
                const auto focus = GetFocus();
                saved_focus_ = focus && IsChild(window_, focus) ? focus : nullptr;
            }
            break;
        case WM_SETFOCUS:
            if (controls_.ready()) {
                const auto saved = std::exchange(saved_focus_, nullptr);
                if (saved && IsChild(window_, saved) && IsWindowEnabled(saved) && IsWindowVisible(saved)) {
                    SetFocus(saved);
                } else {
                    controls_.ensure_focus();
                }
            }
            return 0;
        case WM_DESTROY:
            saved_focus_ = nullptr;
            destroying_ = true;
            request_close();
            SetEvent(notification_.event.get()); // Also wakes waits entered through sent-message dispatch.
            return 0;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                notification_.minimized.store(true);
            } else {
                maximized_ = wparam == SIZE_MAXIMIZED;
                if (notification_.minimized.exchange(false)) {
                    // Progress published while minimized has not woken this
                    // thread. Let the loop read the synchronized snapshot once;
                    // never refresh() here, because refresh() can cause WM_SIZE.
                    refresh_forced_ = true;
                    SetEvent(notification_.event.get());
                }
                controls_.layout();
            }
            return 0;
        case WM_GETMINMAXINFO:
            if (controls_.ready()) {
                const auto size = controls_.window_size();
                reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {size.cx, size.cy};
                return 0;
            }
            break;
        case WM_DPICHANGED:
            if (controls_.ready()) {
                controls_.update_metrics(HIWORD(wparam));
                const auto& bounds = *reinterpret_cast<RECT*>(lparam);
                if (!SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                    bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE)) {
                    core::throw_last_error("SetWindowPos(DPI)");
                }
                controls_.layout(); // Also when the suggested bounds equal the old bounds.
            }
            return 0;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            if (controls_.ready()) {
                controls_.update_metrics(GetDpiForWindow(window_));
                const auto minimum = controls_.window_size();
                RECT current{};
                if (!GetWindowRect(window_, &current)) {
                    core::throw_last_error("GetWindowRect");
                }
                if (current.right - current.left < minimum.cx || current.bottom - current.top < minimum.cy) {
                    if (!SetWindowPos(window_, nullptr, 0, 0,
                        std::max(minimum.cx, current.right - current.left),
                        std::max(minimum.cy, current.bottom - current.top), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                        core::throw_last_error("SetWindowPos(system font)");
                    }
                }
                controls_.layout();
            }
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == WM_NCDESTROY) {
            if (self->instance_) { self->instance_->detach(); }
            self->notification_.target.store(nullptr);
            // Destroying the HWND can discard its pending change message.
            // Force one synchronized final refresh after unpublishing it.
            self->rendered_revision_ = 0;
            SetEvent(self->notification_.event.get());
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
        try {
            return self->handle(message, wparam, lparam);
        } catch (...) {
            self->fail_ui(std::current_exception());
            return 0;
        }
    }
};

} // namespace

int run_player_window(const PlayerOptions& options, int show_command, asio::ActivateDriver activate,
    asio::DeviceSettings settings, SingleInstance* instance)
{
    // This thread has only buttons and read-only text. Disable its input method
    // before the first top-level window, avoiding unnecessary IME initialization
    // on focus. A zero thread ID affects this UI thread, not the driver's STA.
    (void)ImmDisableIME(0);
    PlayerWindow window(instance);
    window.create(options, activate, settings);
    // Requested after create() has started the controller STA, which in turn
    // starts the reader and any driver thread: no thread starts from this one
    // under the hint. Returns this thread (a test may reuse it) to the system
    // default before returning.
    const core::ThreadSpeedScope speed(ui_thread_speed);
    return window.run(show_command);
}

} // namespace wavplay::ui
