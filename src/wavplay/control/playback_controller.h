#pragma once

#include "wavplay/asio/device_settings.h"
#include "wavplay/asio/driver_registry.h"
#include "wavplay/playback/playback_types.h"
#include "wavplay/settings/device_config.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// No Steinberg SDK header reaches here: GUI code can include the controller alone.
namespace wavplay::control {

enum class PlaybackCommand { play, pause, stop, select, select_play, close };
enum class PlaybackState { initializing, reinitializing, stopped, starting, playing, pausing, stopping,
    paused, completed, failed, closing, closed };

// Current facts needed to decide which transport commands may be offered.
// Contains no owning references, display strings or diagnostic history.
struct PlaybackCommandState {
    PlaybackState state{PlaybackState::initializing};
    std::uint64_t accepted{}, settled{}, position_frames{};
    bool has_tracks{}, faulted{};
};

struct PlaybackSnapshot {
    PlaybackState state{PlaybackState::initializing};
    std::uint64_t revision{1};
    // The latest non-progress publication's revision; zero before the first
    // publication. A later progress update preserves it, including when the
    // listener coalesces both publications into one window message.
    std::uint64_t state_revision{};
    std::uint64_t accepted{};
    // All commands through this number have settled. Play settles when start
    // succeeds; Pause/Stop settle after teardown. Stop may cancel an unstarted
    // Play; reinitialization/failure/close cancels pending commands.
    std::uint64_t settled{};
    // Confirmed idle position; during play, submitted PCM sampled on the control
    // thread and published when its whole second or current track changes.
    // Neither this position nor the current item is a hardware-clock estimate.
    std::uint64_t position_frames{}, frame_count{};
    std::uint32_t sample_rate{}; // WAV rate for these frame counts; 0 means unavailable.
    // Loaded once on the worker; snapshots share the immutable list.
    std::shared_ptr<const std::vector<std::wstring>> playlist;
    std::size_t excluded_count{};
    std::optional<std::size_t> selected_index, current_index;
    // A track refusal is recoverable by selecting another list item.
    std::exception_ptr track_error;
    bool settings_loaded{};
    CLSID selected_device{};
    std::wstring device_name;
    long output_pair{1};
    bool device_available{};
    std::optional<settings::WindowPlacement> window;
    long requested_buffer_frames{}; // 0: driver preferred, not a verified allocation.
    // Current/last playback result. Faults remain visible after Close.
    playback::PlaybackResult result{playback::PlaybackResult::stopped};
    playback::PlaybackDiagnostics diagnostics{};
    std::exception_ptr error;

    // Device/runtime faults block playback. A track refusal remains selectable.
    [[nodiscard]] bool faulted() const noexcept
    {
        return error || (playback::is_fault(result) && !playback::is_track_refusal(result));
    }

    [[nodiscard]] PlaybackCommandState command_state() const noexcept
    {
        return {state, accepted, settled, position_frames, playlist && !playlist->empty(), faulted()};
    }
};

// Told that a new snapshot revision exists, on whichever thread published it
// (the controller's STA or a submitting thread), while the controller's state lock
// is held. It must not block or call back into the controller: a GUI posts a
// window message here and reads snapshot() from its own thread.
struct ChangeListener {
    void (*changed)(void* context, std::uint64_t revision) noexcept{};
    void* context{};
    // Optional. Receives, instead of `changed`, the publications that only
    // report playback progress: the current item (which the selection follows)
    // with its rate and length, or the next whole elapsed second. A view may
    // let them pass silently while nobody can see them. Every other
    // publication still goes to `changed`.
    void (*progressed)(void* context, std::uint64_t revision) noexcept{};
};

// One playlist and device. A dedicated STA owns all file/driver operations
// and its hidden owner window. Construction starts asynchronous initialization.
// One controller per process; do not mix concurrent direct playback API calls.
class PlaybackController {
public:
    static constexpr std::size_t queue_capacity = 32;
    PlaybackController(const CLSID& clsid, std::wstring path,
        asio::ActivateDriver activate = asio::activate_registered_driver,
        asio::DeviceSettings settings = {}, ChangeListener listener = {}, std::wstring settings_path = {},
        std::wstring playlist_path = {});
    ~PlaybackController(); // Requests close and joins; may block during driver cleanup.
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // Briefly locks the control queue; never waits for I/O, playback or teardown.
    // Returns a nonzero receipt, or 0 if full/reinitializing/failed/closing. Close bypasses a
    // full queue, cancels pending commands and rejects subsequent submissions.
    // Selection commands require an index and go through select_track instead.
    std::uint64_t submit(PlaybackCommand command);
    // Selection alone leaves running audio alone. With play=true, stop the
    // current request and start this item from its beginning, then continue.
    std::uint64_t select_track(std::size_t index, bool play = false);
    // Same Close receipt and cancellation policy as submit(close). The worker
    // saves normal window bounds only after releasing playback and the device.
    // A Close that was accepted without bounds (the worker's own, from its
    // owner window or thread) takes the bounds of a later call, which still
    // returns 0; accepted bounds are never replaced.
    std::uint64_t close(std::optional<settings::WindowPlacement> placement);
    // Completion only, under the same lock as snapshot(), without copying
    // strings or allocating. Shutdown must still work after a UI allocation fails.
    [[nodiscard]] bool closed() const;
    // Reads current command facts under the state lock without copying owned
    // data. This does not reserve a command; submit still validates acceptance.
    [[nodiscard]] PlaybackCommandState command_state() const;
    [[nodiscard]] PlaybackSnapshot snapshot() const;
    // Copy-assigns the same snapshot to `into` under the state lock. A reused
    // object keeps its string and vector storage, so a steady-state refresh
    // makes no allocator calls while the lock is held. If a copy throws, `into`
    // is valid but partly updated; do not treat its revision as rendered.
    void snapshot(PlaybackSnapshot& into) const;
    // UI refresh of an unmodified snapshot previously obtained from this controller.
    // Pass the last successfully rendered revision, or zero to force a full copy.
    // Under the same state lock, a matching, settled same-track Playing snapshot
    // needs only revision and position updated; returns true for that path.
    // Every other case copies the full snapshot, with snapshot(into)'s exception
    // guarantee. Never advance the rendered revision until rendering succeeds.
    [[nodiscard]] bool refresh_snapshot(PlaybackSnapshot& into, std::uint64_t rendered_revision) const;
    [[nodiscard]] PlaybackSnapshot wait_for_change(std::uint64_t revision, std::chrono::milliseconds timeout) const;
    void join(); // Caller thread only. First submit close (or observe closed).

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavplay::control
