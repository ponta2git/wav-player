#include "wavplay/control/playback_controller.h"

#include "wavplay/asio/driver_session.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/core/thread_hints.h"
#include "wavplay/core/win32.h"
#include "wavplay/playback/track_player.h"
#include "wavplay/playlist/playlist.h"
#include "wavplay/settings/device_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace wavplay::control {
namespace {

using playback::PlaybackDiagnostics;
using playback::PlaybackResult;
static_assert(std::is_nothrow_move_assignable_v<PlaybackDiagnostics>);
constexpr wchar_t window_class[] = L"WavplayPlaybackController";
constinit std::atomic_flag controller_reserved{};

struct ControllerClaim {
    ControllerClaim()
    {
        if (controller_reserved.test_and_set(std::memory_order_acquire)) {
            throw std::logic_error("Only one playback controller may own the ASIO notification loop");
        }
    }
    ~ControllerClaim() { controller_reserved.clear(std::memory_order_release); }
};

struct QueuedCommand {
    PlaybackCommand command;
    std::uint64_t sequence;
    std::optional<std::size_t> track;
};

// The bounded submission queue. Callers hold the controller's state lock.
class CommandQueue {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool full() const noexcept { return count_ == items_.size(); }
    [[nodiscard]] const QueuedCommand& front() const noexcept { return items_[head_]; }
    void push(const QueuedCommand& command) noexcept
    {
        items_[(head_ + count_) % items_.size()] = command;
        ++count_;
    }
    QueuedCommand pop() noexcept
    {
        const auto command = items_[head_];
        head_ = (head_ + 1) % items_.size();
        --count_;
        return command;
    }
    void clear() noexcept { count_ = 0; }

private:
    std::array<QueuedCommand, PlaybackController::queue_capacity> items_{};
    std::size_t head_{}, count_{};
};

} // namespace

struct PlaybackController::Impl {
    enum class Interrupt { none, pause, stop };
    const ControllerClaim claim;
    core::UniqueHandle commands_changed;
    const CLSID clsid;
    const std::wstring path;
    const asio::ActivateDriver activate;
    const asio::DeviceSettings settings;
    const std::wstring settings_path;
    const std::wstring playlist_path;
    const ChangeListener listener;
    mutable std::mutex gate;
    mutable std::condition_variable changed;
    PlaybackSnapshot status;
    CommandQueue queue;
    std::optional<wavplay::settings::WindowPlacement> window_to_save;
    bool close_requested{}, faulted{};
    // Set with close_requested, so that the startup list validation reads it
    // before each entry without gate.
    std::atomic<bool> close_flag{};
    std::thread worker;

    // Worker-only state. WndProc may request Close, but never reenters play.
    // The worker's own reference to the list it published in status, so it
    // never reads status without the lock.
    std::shared_ptr<const std::vector<std::wstring>> track_paths;
    std::optional<playback::TrackPlayer> player;
    // The request play() runs, engaged only while TrackPlayer::play runs.
    struct ActiveRequest {
        std::uint64_t play_sequence;
        // The newest receipt delivered to this request (its Play, a Select, a
        // Pause or a Stop). The request's end settles through it, and so
        // does its start unless a Pause or Stop is pending.
        std::uint64_t delivered_sequence;
        Interrupt interrupt{Interrupt::none};
        bool started{};
    };
    std::optional<ActiveRequest> active;
    PlaybackDiagnostics diagnostics;

    Impl(const CLSID& id, std::wstring file, asio::ActivateDriver factory, const asio::DeviceSettings& device,
        ChangeListener observer, std::wstring configuration, std::wstring list)
        : clsid(id), path(std::move(file)), activate(factory), settings(device), settings_path(std::move(configuration)),
          playlist_path(std::move(list)), listener(observer)
    {
        if (!activate) {
            throw std::invalid_argument("Invalid playback controller configuration");
        }
        settings.validate();
        commands_changed = core::create_event();
        worker = std::thread([this] { run(); });
    }

    ~Impl()
    {
        (void)submit(PlaybackCommand::close);
        if (worker.joinable()) {
            worker.join();
        }
    }

    enum class Publication { state, progress };

    // Every publication advances the revision and wakes wait_for_change.
    // Only the progress poll labels its publication as progress.
    void publish_locked(Publication kind = Publication::state) noexcept
    {
        ++status.revision;
        if (kind == Publication::state) { status.state_revision = status.revision; }
        changed.notify_all();
        const auto notify = kind == Publication::progress && listener.progressed ?
            listener.progressed : listener.changed;
        if (notify) {
            notify(listener.context, status.revision);
        }
    }

    std::uint64_t submit(PlaybackCommand command, std::optional<wavplay::settings::WindowPlacement> placement = {},
        std::optional<std::size_t> track = {})
    {
        std::uint64_t sequence{};
        bool needs_wake{};
        {
            const std::lock_guard lock(gate);
            if (command < PlaybackCommand::play || command > PlaybackCommand::close ||
                ((command == PlaybackCommand::select || command == PlaybackCommand::select_play) != track.has_value()) ||
                (track && (!status.playlist || *track >= status.playlist->size())) ||
                (command == PlaybackCommand::play && status.playlist && status.playlist->empty()) ||
                close_requested || status.state == PlaybackState::closed ||
                ((faulted || status.state == PlaybackState::reinitializing) && command != PlaybackCommand::close) ||
                (queue.full() && command != PlaybackCommand::close)) {
                return 0;
            }
            // Once a batch is pending the worker already has a wake to consume.
            // Close must still interrupt a Play barrier and bypass a full queue.
            needs_wake = queue.empty() || command == PlaybackCommand::close;
            sequence = ++status.accepted;
            if (command == PlaybackCommand::close) {
                window_to_save = placement;
                close_requested = true;
                close_flag.store(true, std::memory_order_relaxed); // A hint only: gate orders the state.
            } else {
                queue.push({command, sequence, track});
            }
            publish_locked();
        }
        // Signal after unlocking, so the woken worker does not block on gate.
        // Both events are auto-reset and stay set until consumed, and the
        // worker reads the queue under gate after every wake: a late signal
        // can only cause a spurious wake, which both waits tolerate.
        if (needs_wake) {
            SetEvent(commands_changed.get());
            // The idle loop and active playback wait on separate events.
            playback::wake_playback_control();
        }
        return sequence;
    }

    bool closing() const
    {
        const std::lock_guard lock(gate);
        return close_requested;
    }

    PlaybackSnapshot snapshot() const
    {
        const std::lock_guard lock(gate);
        return status;
    }

    void snapshot(PlaybackSnapshot& into) const
    {
        const std::lock_guard lock(gate);
        into = status;
    }

    bool refresh_snapshot(PlaybackSnapshot& into, std::uint64_t rendered_revision) const
    {
        const std::lock_guard lock(gate);
        // State publications may coalesce with later progress. Compare the
        // synchronized state stamp, never the kind of the last notification.
        if (rendered_revision != 0 && into.revision == rendered_revision &&
            !close_requested && !faulted && status.state == PlaybackState::playing &&
            into.state == PlaybackState::playing && status.accepted == status.settled &&
            into.state_revision == status.state_revision && into.current_index == status.current_index &&
            into.selected_index == status.selected_index && into.sample_rate == status.sample_rate &&
            into.frame_count == status.frame_count) {
            into.revision = status.revision;
            into.position_frames = status.position_frames;
            return true;
        }
        into = status;
        return false;
    }

    void fail_locked(std::exception_ptr error, PlaybackResult result)
    {
        faulted = true;
        if (error) {
            status.error = std::move(error);
        }
        status.result = result;
        status.state = close_requested ? PlaybackState::closing : PlaybackState::failed;
        if (!close_requested) {
            status.settled = status.accepted;
        }
        queue.clear();
        publish_locked();
    }

    void fail(std::exception_ptr error, PlaybackResult result = PlaybackResult::driver_error)
    {
        const std::lock_guard lock(gate);
        fail_locked(std::move(error), result);
    }

    // Windows calls a window procedure only on the thread that created the
    // window, and the worker creates its owner window. This procedure may
    // therefore read worker-only state (active, player) without the lock.
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_CLOSE) {
            try {
                (void)self->submit(PlaybackCommand::close);
            } catch (...) {
                self->fail(std::current_exception());
                if (self->active) {
                    self->player->request_stop();
                }
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND create_window()
    {
        static std::once_flag registered;
        std::call_once(registered, [] {
            WNDCLASSW cls{};
            cls.lpfnWndProc = window_proc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = window_class;
            if (!RegisterClassW(&cls)) {
                core::throw_last_error("RegisterClassW");
            }
        });
        const auto hwnd = CreateWindowExW(0, window_class, L"WAV Player", WS_OVERLAPPEDWINDOW,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) {
            core::throw_last_error("CreateWindowExW");
        }
        return hwnd;
    }

    // Active request only. True when the queue's front must stay queued until
    // TrackPlayer::play returns: Select and Play always replaces the request,
    // and Play passes neither an unstarted request nor an unfinished Pause or Stop.
    [[nodiscard]] bool front_waits_locked() const noexcept
    {
        const auto command = queue.front().command;
        return command == PlaybackCommand::select_play || (command == PlaybackCommand::play &&
            (!active->started || active->interrupt != Interrupt::none));
    }

    void deliver_active()
    {
        if (!active) {
            return;
        }
        for (std::size_t handled = 0; handled < queue_capacity; ++handled) {
            PlaybackCommand command;
            {
                const std::lock_guard lock(gate);
                if (close_requested) {
                    if (status.state != PlaybackState::closing) {
                        status.state = PlaybackState::closing;
                        publish_locked();
                    }
                    queue.clear();
                    command = PlaybackCommand::close;
                } else {
                    if (faulted || queue.empty()) {
                        return;
                    }
                    if (front_waits_locked()) {
                        if (queue.front().command == PlaybackCommand::select_play) {
                            // Leave this request queued until teardown has completed.
                            // Selection/play must never replace a live TrackPlayer.
                            active->interrupt = Interrupt::stop;
                            if (status.state != PlaybackState::stopping) {
                                status.state = PlaybackState::stopping;
                                publish_locked();
                            }
                            player->request_stop();
                        }
                        return; // Preserve the barrier until the current invocation returns.
                    }
                    const auto request = queue.pop();
                    command = request.command;
                    if (command == PlaybackCommand::select) {
                        status.selected_index = request.track;
                        status.track_error = {};
                        active->delivered_sequence = request.sequence;
                        // Selection is immediate, but its receipt cannot pass
                        // an unstarted Play or unfinished Pause/Stop. Keep
                        // dispatching so a following Stop still cancels setup
                        // or the pause drain without waiting for another start.
                        if (active->started && active->interrupt == Interrupt::none) {
                            status.settled = request.sequence;
                        }
                    } else if (command == PlaybackCommand::play) {
                        status.settled = request.sequence; // Already playing: no restart.
                    } else {
                        active->delivered_sequence = request.sequence;
                        if (command == PlaybackCommand::stop) {
                            active->interrupt = Interrupt::stop;
                        } else if (active->interrupt == Interrupt::none) {
                            active->interrupt = Interrupt::pause;
                        }
                        status.state = active->interrupt == Interrupt::stop ?
                            PlaybackState::stopping : PlaybackState::pausing;
                    }
                    publish_locked();
                }
            }
            if (command == PlaybackCommand::close || command == PlaybackCommand::stop) {
                player->request_stop();
                if (command == PlaybackCommand::close) {
                    return;
                }
            } else if (command == PlaybackCommand::pause) {
                player->request_pause();
            }
        }
        // A producer can refill slots while this bounded batch is handled,
        // without ever observing an empty queue. Rearm the active wait for
        // that work, but not for a Play which must wait for this run to end.
        const std::lock_guard lock(gate);
        if (close_requested || (!faulted && !queue.empty() && !front_waits_locked())) {
            playback::wake_playback_control();
        }
    }

    static void poll(void* context) noexcept
    {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (self.publish_progress_and_check_commands()) {
                self.deliver_active();
            }
        } catch (...) {
            self.fail(std::current_exception());
            self.player->request_stop();
        }
    }

    void update_position_locked(bool follow_track = true)
    {
        if (!player) { return; }
        const auto index = player->track_index();
        if (follow_track && status.current_index != index) {
            status.selected_index = index;
        }
        status.current_index = index;
        status.position_frames = player->position_frames();
        status.frame_count = player->frame_count();
        status.sample_rate = player->sample_rate();
    }

    [[nodiscard]] bool publish_progress_and_check_commands()
    {
        const std::lock_guard lock(gate);
        if (player && (status.current_index != player->track_index() ||
            status.sample_rate != player->sample_rate() ||
            (status.sample_rate && status.position_frames / status.sample_rate !=
                player->position_frames() / status.sample_rate))) {
            update_position_locked();
            publish_locked(Publication::progress);
        }
        // A healthy empty queue needs no second lock in deliver_active.
        // submit sees its next empty-to-nonempty transition under this same
        // gate and signals both waits; Close always signals. Pending barriers
        // stay on the existing delivery path.
        return close_requested || (!faulted && !queue.empty());
    }

    static void on_started(void* context) noexcept
    {
        auto& self = *static_cast<Impl*>(context);
        try {
            auto& request = *self.active;
            request.started = true;
            // Copy before changing published state: an allocation failure must
            // not leave a partly updated snapshot with its old state stamp.
            auto prepared_diagnostics = self.diagnostics;
            {
                const std::lock_guard lock(self.gate);
                self.status.settled = std::max(self.status.settled,
                    request.interrupt == Interrupt::none ? request.delivered_sequence : request.play_sequence);
                self.status.diagnostics = std::move(prepared_diagnostics);
                self.update_position_locked();
                if (!self.close_requested && request.interrupt == Interrupt::none && !self.faulted) {
                    self.status.state = PlaybackState::playing;
                }
                self.publish_locked();
            }
            self.deliver_active();
        } catch (...) {
            self.fail(std::current_exception());
            self.player->request_stop();
        }
    }

    void play(asio::DriverSession& session, std::uint64_t sequence)
    {
        // Only the outer worker loop calls this function. WndProc never does.
        std::size_t selected{};
        {
            const std::lock_guard lock(gate);
            if (close_requested || faulted) {
                return;
            }
            if (!status.playlist || status.playlist->empty()) {
                status.settled = sequence;
                publish_locked();
                return;
            }
            selected = status.selected_index.value_or(0);
            status.selected_index = selected;
            if (status.current_index != selected) {
                status.position_frames = status.frame_count = 0;
                status.sample_rate = 0;
            }
            status.track_error = {};
            // Starting this invocation and accepting close are ordered by gate.
            status.state = PlaybackState::starting;
            status.result = PlaybackResult::playing;
            status.diagnostics.reset();
            publish_locked();
        }
        try {
            if (!player) {
                player.emplace(track_paths, selected);
            } else if (player->track_index() != selected) {
                player->select(selected);
            }
            if (player->position_frames() == player->frame_count()) {
                player->stop();
            }
        } catch (const std::runtime_error&) {
            refuse_selection(selected, sequence, std::current_exception());
            return;
        }
        // TrackPlayer::play can return before run_request resets this copy.
        // status.diagnostics was reset with the starting state above.
        diagnostics.reset();
        {
            const std::lock_guard lock(gate);
            update_position_locked(false);
            publish_locked();
        }
        active = ActiveRequest{.play_sequence = sequence, .delivered_sequence = sequence};
        PlaybackResult result{PlaybackResult::driver_error};
        std::exception_ptr error;
        try {
            result = player->play(session, &diagnostics, {on_started, poll, this});
        } catch (...) {
            error = std::current_exception();
        }
        // The request's only end: nothing delivers to it after play() returns.
        const auto delivered = active->delivered_sequence;
        active.reset();
        if (!error && result == PlaybackResult::reinitialize_required) {
            reinitialize(session);
            return;
        }
        finish_playback(result, std::move(error), delivered);
    }

    // Only std::runtime_error from opening or validating a selected file is a
    // recoverable refusal: media errors and Win32 I/O (std::system_error).
    // Other exceptions (bad_alloc, logic errors) reach run() and fault the
    // controller, as in playlist::load.
    void refuse_selection(std::size_t index, std::uint64_t sequence, std::exception_ptr error,
        PlaybackDiagnostics* prepared_diagnostics = nullptr)
    {
        const std::lock_guard lock(gate);
        if (prepared_diagnostics) { status.diagnostics = std::move(*prepared_diagnostics); }
        status.selected_index = index;
        status.current_index = index;
        status.position_frames = status.frame_count = 0;
        status.sample_rate = 0;
        status.track_error = std::move(error);
        status.result = PlaybackResult::next_track_rejected;
        status.state = close_requested ? PlaybackState::closing : PlaybackState::stopped;
        status.settled = sequence;
        publish_locked();
    }

    void prepare_selection(std::size_t index)
    {
        if (player) {
            if (path.empty()) { player->select(index); }
            else { player->stop(); } // The --wav compatibility list has one item.
        } else {
            player.emplace(track_paths, index);
        }
    }

    // Scalar arguments let the caller avoid materializing the entire command.
    bool select_idle(std::size_t index, std::uint64_t sequence, PlaybackCommand command)
    {
        try {
            prepare_selection(index);
        } catch (const std::runtime_error&) {
            refuse_selection(index, sequence, std::current_exception());
            return false;
        }
        const std::lock_guard lock(gate);
        status.selected_index = index;
        update_position_locked(false);
        status.track_error = {};
        status.result = PlaybackResult::stopped;
        status.state = close_requested ? PlaybackState::closing : PlaybackState::stopped;
        // Selection alone is complete with its metadata. Select and Play still
        // waits for start success before settling the same receipt.
        if (command == PlaybackCommand::select) { status.settled = sequence; }
        publish_locked();
        return true;
    }

    static void poll_reinitialization(void* context) noexcept
    {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (!core::pump_messages()) {
                (void)self.submit(PlaybackCommand::close);
            }
            if (self.closing()) {
                self.player->request_stop();
            }
        } catch (...) {
            self.fail(std::current_exception());
            self.player->request_stop();
        }
    }

    void reinitialize(asio::DriverSession& session)
    {
        {
            const std::lock_guard lock(gate);
            if (faulted) {
                return;
            }
            if (close_requested) {
                status.result = PlaybackResult::reinitialize_required;
                return; // Final Close releases the old instance without reopening it.
            }
            queue.clear();
            status.state = PlaybackState::reinitializing;
            status.device_available = false;
            status.result = PlaybackResult::stopped;
            status.diagnostics.reset();
            // Pending Play must never turn recovery into automatic playback.
            // Reject new controls until the stopped snapshot is published.
            publish_locked();
        }
        try {
            const auto result = player->reinitialize(session, {nullptr, poll_reinitialization, this});
            if (result != PlaybackResult::stopped) {
                fail({}, result);
                return;
            }
            const std::lock_guard lock(gate);
            if (!faulted) {
                status.device_available = !close_requested;
                update_position_locked(false);
                status.result = PlaybackResult::stopped;
                status.state = close_requested ? PlaybackState::closing : PlaybackState::stopped;
                if (!close_requested) {
                    status.settled = status.accepted;
                }
                publish_locked();
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    // The idle state after a clean or refused request. EOF completes a --wav
    // item; a playlist's EOF is stopped with the completed result.
    [[nodiscard]] PlaybackState idle_state_after(PlaybackResult result, bool refused) const noexcept
    {
        if (result == PlaybackResult::paused) {
            return PlaybackState::paused;
        }
        return result == PlaybackResult::stopped || refused || path.empty() ?
            PlaybackState::stopped : PlaybackState::completed;
    }

    // `delivered` is the ended request's ActiveRequest::delivered_sequence.
    void finish_playback(PlaybackResult result, std::exception_ptr error, std::uint64_t delivered)
    {
        if (!error && result == PlaybackResult::read_error) {
            // Show why the producer failed (e.g. ReadFile's system error);
            // the result stays read_error.
            error = player->result_cause();
        }
        const bool refused = !error && playback::is_track_refusal(result);
        const bool clean = !error && (playback::is_clean_stop(result) || refused);
        // Prepare every potentially throwing value before changing status.
        // The locked commit below then publishes one complete non-progress state.
        auto prepared_diagnostics = diagnostics;
        std::exception_ptr refusal_error;
        if (refused) {
            const auto& cause = player->result_cause();
            refusal_error = cause ? cause : std::make_exception_ptr(std::runtime_error(
                result == PlaybackResult::next_rate_rejected
                ? "The selected track's sample rate is not supported by this device."
                : "The selected track cannot be read or converted losslessly for this device."));
        }
        // A click during playback changes only selection. After Stop has fully
        // released the stream, reuse idle selection to show that item's length.
        // Never reopen the same item or perform media I/O while holding gate.
        std::optional<std::size_t> stopped_selection;
        std::uint64_t selection_sequence{};
        if (clean && result == PlaybackResult::stopped) {
            const std::lock_guard lock(gate);
            if (!close_requested && !faulted && status.selected_index &&
                status.selected_index != player->track_index()) {
                stopped_selection = status.selected_index;
                selection_sequence = std::max(status.settled, delivered);
            }
        }
        if (stopped_selection) {
            try {
                prepare_selection(*stopped_selection);
            } catch (const std::runtime_error&) {
                refuse_selection(*stopped_selection, selection_sequence, std::current_exception(), &prepared_diagnostics);
                return;
            }
        }
        const std::lock_guard lock(gate);
        // Publish the final position and verified allocation together with the
        // result, including failures before the started observer could run.
        update_position_locked(result != PlaybackResult::stopped);
        status.diagnostics = std::move(prepared_diagnostics);
        if (!clean) {
            // The returned reason is final; a later idle observation must not replace it.
            fail_locked(std::move(error), result);
            return;
        }
        if (!faulted) {
            status.result = result;
            if (result == PlaybackResult::completed) {
                // EOF displays the completed item's full duration even if a
                // different row was highlighted during its final seconds.
                status.selected_index = player->track_index();
            }
            if (refused) {
                status.selected_index = player->track_index();
                // The refusal's own exception when it has one (a failed open or
                // validation, an unsupported buffer width); otherwise the
                // rate or format text.
                status.track_error = std::move(refusal_error);
            }
        }
        if (!faulted && !close_requested) {
            status.state = idle_state_after(result, refused);
            status.settled = std::max(status.settled, delivered);
        }
        publish_locked();
    }

    // Recover only while the command loop owns an open session. Final Close
    // records notifications without starting another device lifetime.
    void report_idle_fault(asio::DriverSession* session = nullptr)
    {
        if (player) {
            if (const auto fault = player->take_idle_fault()) {
                if (session && *fault == PlaybackResult::reinitialize_required && !closing()) {
                    reinitialize(*session);
                } else {
                    fail({}, *fault);
                }
            }
        }
    }

    void wait_for_work()
    {
        const HANDLE events[]{commands_changed.get(), player ? player->idle_fault_event() : nullptr};
        const DWORD handles = events[1] ? 2 : 1;
        const auto wake = MsgWaitForMultipleObjectsEx(handles, events,
            INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wake == WAIT_FAILED) {
            core::throw_last_error("Controller message wait");
        }
        if (!core::pump_messages()) {
            (void)submit(PlaybackCommand::close);
        }
    }

    void control_loop(asio::DriverSession& session)
    {
        std::size_t handled{};
        for (;;) {
            // A continuous command producer must not starve WM_CLOSE or the
            // driver's window/COM traffic. No queue lock is held while dispatching.
            if (handled == 0 && !core::pump_messages()) {
                (void)submit(PlaybackCommand::close);
            }
            report_idle_fault(&session);
            std::optional<QueuedCommand> request;
            {
                const std::lock_guard lock(gate);
                if (close_requested) {
                    return;
                }
                if (!queue.empty() && !faulted) {
                    request = queue.pop();
                    // A Select directly followed by another selection is
                    // superseded: only the last one opens and validates its
                    // file. Its settlement covers the skipped receipts, since
                    // settled is cumulative; fault and close settle them too.
                    while (request->command == PlaybackCommand::select && !queue.empty() &&
                        (queue.front().command == PlaybackCommand::select ||
                            queue.front().command == PlaybackCommand::select_play)) {
                        request = queue.pop();
                    }
                }
            }
            if (!request) {
                wait_for_work();
                handled = 0;
                continue;
            }
            handled = (handled + 1) % queue_capacity;
            if (request->command == PlaybackCommand::play) {
                play(session, request->sequence);
            } else if (request->command == PlaybackCommand::select || request->command == PlaybackCommand::select_play) {
                if (select_idle(*request->track, request->sequence, request->command) &&
                    request->command == PlaybackCommand::select_play) {
                    play(session, request->sequence);
                }
            } else {
                if (request->command == PlaybackCommand::stop && player) {
                    player->stop();
                }
                const std::lock_guard lock(gate);
                if (request->command == PlaybackCommand::stop) {
                    status.state = PlaybackState::stopped;
                    status.result = PlaybackResult::stopped;
                    status.position_frames = 0;
                }
                status.settled = request->sequence; // Idle Pause is a no-op.
                publish_locked();
            }
        }
    }

    // Startup phases, in publication order: settings, list, NO DEVICE, then
    // the device. Each checks for Close on entry and returns early, without
    // throwing, once it has been requested; a phase that is already running
    // completes, except the list validation, which stops before its next
    // entry. Errors reach run()'s single catch.
    struct DeviceChoice {
        std::optional<CLSID> clsid; // Empty: NO DEVICE, reported after the list.
        asio::DeviceSettings settings;
    };

    // Reads settings.ini once and publishes the effective device settings.
    std::optional<DeviceChoice> load_settings()
    {
        if (closing()) {
            return std::nullopt;
        }
        wavplay::settings::ApplicationConfig configured;
        if (!IsEqualCLSID(clsid, CLSID{})) {
            configured.selected_device = clsid;
        }
        if (!settings_path.empty()) {
            configured = wavplay::settings::load_application_config(settings_path.c_str(), configured.selected_device);
        }
        const auto effective = configured.device.apply(settings);
        effective.validate();
        const std::lock_guard lock(gate);
        status.settings_loaded = true;
        status.selected_device = configured.selected_device.value_or(CLSID{});
        status.device_name = std::move(configured.device.name);
        status.output_pair = effective.output_pair;
        status.window = configured.window;
        status.requested_buffer_frames = effective.buffer_frames;
        publish_locked();
        return DeviceChoice{configured.selected_device, effective};
    }

    // Loads and validates the playlist (or the --wav item) once and publishes it.
    // A Close accepted during the validation returns no list at the next entry;
    // nothing is published then, not even the entries validated so far. A Close
    // accepted during the last entry leaves no entry to stop before: the
    // complete list is published, and open_device() then stops at its check.
    bool load_list()
    {
        if (closing()) {
            return false;
        }
        playlist::Playlist loaded;
        if (path.empty()) {
            auto validated = playlist::load(playlist_path.c_str(), close_flag);
            if (!validated) {
                return false;
            }
            loaded = std::move(*validated);
        } else {
            loaded.paths.push_back(path);
        }
        track_paths = std::make_shared<const std::vector<std::wstring>>(std::move(loaded.paths));
        const std::lock_guard lock(gate);
        status.playlist = track_paths;
        status.excluded_count = loaded.excluded_count;
        if (!path.empty()) { status.selected_index = 0; }
        publish_locked();
        return true;
    }

    // Reports NO DEVICE, prepares the --wav item, then creates the owner
    // window immediately before the driver that needs it. During the possibly
    // long settings and list validation no top-level window exists that
    // nothing pumps, so broadcasts never wait for this thread. Close accepted
    // during the list load skips all of it, NO DEVICE included; Close during
    // the --wav validation skips the window and the driver.
    bool open_device(const DeviceChoice& device, std::optional<asio::DriverSession>& session, HWND& window)
    {
        if (closing()) {
            return false;
        }
        if (!device.clsid) {
            throw std::invalid_argument("NO DEVICE: set [General] SelectedDevice to the ASIO driver CLSID in settings.ini and restart");
        }
        // The list is a startup snapshot. Revalidate the selected
        // file when Play/selection opens it, not during device init.
        if (!path.empty()) { player.emplace(path.c_str()); }
        if (closing()) {
            return false;
        }
        window = create_window();
        session.emplace(*device.clsid, window, activate, device.settings);
        const std::lock_guard lock(gate);
        status.device_available = true;
        if (status.selected_index) { update_position_locked(false); }
        status.state = close_requested ? PlaybackState::closing : PlaybackState::stopped;
        publish_locked();
        return true;
    }

    void run() noexcept
    {
        // Named for traces. No speed hint: every driver call runs here, and a
        // driver may create its own threads from this thread.
        core::name_current_thread(L"WAV Player control");
        HWND window{};
        std::optional<asio::DriverSession> session;
        try {
            const auto device = load_settings();
            if (device && load_list() && open_device(*device, session, window)) {
                control_loop(*session);
            }
        } catch (...) {
            fail(std::current_exception());
            // Initialization failure is observable until the caller requests close.
            // No retry or automatic playback follows a failed request.
            try {
                while (!closing()) {
                    wait_for_work();
                }
            } catch (...) {
                fail(std::current_exception());
            }
        }
        release(session, window);
    }

    // Final cleanup on the worker: driver release, WAV release, owner window
    // destruction and STA end, then the window bounds and closed.
    void release(std::optional<asio::DriverSession>& session, HWND window) noexcept
    {
        {
            const std::lock_guard lock(gate);
            close_requested = true;
            status.state = PlaybackState::closing;
            queue.clear();
            publish_locked();
        }
        if (session) {
            if (player) { player->prepare_release(); }
            try {
                session->close();
            } catch (...) {
                fail(std::current_exception());
            }
            report_idle_fault(); // Includes reset/overload/clock loss during final Release.
        }
        player.reset();
        if (window) {
            DestroyWindow(window);
        }
        session.reset(); // STA ends after driver release and owner-window destruction.
        // Close carries only a small value across threads. All settings I/O is
        // here, after releasing audio resources, with no controller lock held.
        std::optional<wavplay::settings::WindowPlacement> placement;
        {
            const std::lock_guard lock(gate);
            if (status.settings_loaded) { placement = window_to_save; }
        }
        if (placement && !settings_path.empty()) {
            try {
                wavplay::settings::save_window_config(settings_path.c_str(), *placement);
            } catch (...) {
                fail(std::current_exception());
            }
        }
        {
            const std::lock_guard lock(gate);
            status.state = PlaybackState::closed;
            status.device_available = false;
            status.settled = status.accepted;
            publish_locked();
        }
    }
};

PlaybackController::PlaybackController(const CLSID& clsid, std::wstring path, asio::ActivateDriver activate,
    asio::DeviceSettings settings, ChangeListener listener, std::wstring settings_path, std::wstring playlist_path)
    : impl_(std::make_unique<Impl>(clsid, std::move(path), activate, settings, listener,
        std::move(settings_path), std::move(playlist_path)))
{
}
PlaybackController::~PlaybackController() = default;
std::uint64_t PlaybackController::submit(PlaybackCommand command) { return impl_->submit(command); }
std::uint64_t PlaybackController::select_track(std::size_t index, bool play)
{
    return impl_->submit(play ? PlaybackCommand::select_play : PlaybackCommand::select, {}, index);
}
std::uint64_t PlaybackController::close(std::optional<settings::WindowPlacement> placement)
{
    return impl_->submit(PlaybackCommand::close, placement);
}
bool PlaybackController::closed() const
{
    const std::lock_guard lock(impl_->gate);
    return impl_->status.state == PlaybackState::closed;
}
PlaybackCommandState PlaybackController::command_state() const
{
    const std::lock_guard lock(impl_->gate);
    return impl_->status.command_state();
}
PlaybackSnapshot PlaybackController::snapshot() const { return impl_->snapshot(); }
void PlaybackController::snapshot(PlaybackSnapshot& into) const { impl_->snapshot(into); }
bool PlaybackController::refresh_snapshot(PlaybackSnapshot& into, std::uint64_t rendered_revision) const
{
    return impl_->refresh_snapshot(into, rendered_revision);
}
PlaybackSnapshot PlaybackController::wait_for_change(std::uint64_t revision, std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(impl_->gate);
    impl_->changed.wait_for(lock, timeout, [&] { return impl_->status.revision != revision; });
    return impl_->status;
}
void PlaybackController::join()
{
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

} // namespace wavplay::control
