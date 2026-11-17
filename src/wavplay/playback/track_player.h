#pragma once

#include "wavplay/core/memory.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/playback/playback_types.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wavplay::asio {
class DriverLease;
class DriverSession;
}

namespace wavplay::playback {

struct RequestProgress;

// A retained WAV or ordered playlist with Play, Pause and Stop on one thread. play()
// blocks until paused, stopped, completed or failed. Play after pause resumes;
// play after Stop starts at zero. request_pause() and request_stop() may be
// called from another thread. Pause fences PCM at a callback boundary, drains
// already submitted audio, then calls stop, so it takes the output queue's drain
// time (including two extra periods). No driver position estimate or prefetch
// cursor is used as the resume position. Stop cancels queued PCM without
// draining it. Faults win over Stop; Stop wins over Pause and EOF. A failed
// request cannot resume or restart, except after successful reinitialize() for
// reinitialize_required. Idle requests are discarded by play(); use
// stop(). Once EOF has been submitted the track completes normally instead of
// pausing. Keep the object alive until play() returns.
class TrackPlayer {
public:
    explicit TrackPlayer(const wchar_t* path);
    // Transfer a validated, unread file instead of opening and parsing it again.
    explicit TrackPlayer(media::WavReader track);
    // Co-owns the immutable list, so no path is copied and every path it
    // hands to a request stays valid while this object lives.
    explicit TrackPlayer(std::shared_ptr<const std::vector<std::wstring>> paths, std::size_t start_index = 0);
    explicit TrackPlayer(std::vector<std::wstring> paths, std::size_t start_index = 0)
        : TrackPlayer(std::make_shared<const std::vector<std::wstring>>(std::move(paths)), start_index)
    {
    }
    ~TrackPlayer();
    TrackPlayer(const TrackPlayer&) = delete;
    TrackPlayer& operator=(const TrackPlayer&) = delete;

    [[nodiscard]] PlaybackResult play(asio::DriverSession& session, PlaybackDiagnostics* diagnostics = nullptr,
        PlaybackObserver observer = {});
    void request_pause() noexcept { requests_->pause.store(true, std::memory_order_release); }
    void request_stop() noexcept;
    // Control thread only, after play() returns. Reset to the beginning without
    // touching the idle driver or clearing a failure. Keep this WAV protected.
    void stop();
    // Idle control thread only. A new selection starts at zero while keeping
    // device notification supervision. A refused file can be selected again.
    void select(std::size_t index);
    [[nodiscard]] std::size_t track_index() const noexcept { return track_index_; }
    // Control thread only: confirmed pause/EOF/Stop position while idle.
    // Observers see submitted PCM during play, excluding stream-start silence.
    [[nodiscard]] std::uint64_t position_frames() const noexcept { return playing_ ? live_position_ : position_; }
    [[nodiscard]] std::uint64_t frame_count() const noexcept { return track_ ? track_->info().frame_count : 0; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return track_ ? track_->info().format.sample_rate : 0; }
    // Control thread only, after play() returns: the exception behind its
    // result, when one is known. For read_error the producer's failure (e.g.
    // ReadFile's system_error); for a playlist refusal a failed open or
    // validation, or an unsupported buffer width with the WAV rate. Null for a
    // rate or format refusal and after any other result. Each play() replaces it.
    [[nodiscard]] const std::exception_ptr& result_cause() const noexcept { return cause_; }

    // Idle supervision, control thread only, while play() is not running. After a
    // clean stop the still-open device can report a reset, an overload or a lost
    // clock against that request. A control loop that waits while idle includes
    // idle_fault_event() (auto-reset; nullptr while nothing is watched) and then
    // calls take_idle_fault(), which reports such a fault once. An idle fault also
    // refuses the next play(): if take_idle_fault() has not reported it yet, that
    // play() returns the fault's result instead, without touching the driver,
    // buffers or position, and take_idle_fault() no longer reports it. Every later
    // play() throws std::logic_error, as a failed request cannot resume or restart.
    [[nodiscard]] HANDLE idle_fault_event() const noexcept;
    [[nodiscard]] std::optional<PlaybackResult> take_idle_fault() noexcept;
    // Before the session's final release, which may restore the original clock:
    // that restoration is not a fault of the last request. No play() may follow.
    void prepare_release() noexcept;
    // After reinitialize_required, retire the old driver and notification
    // generation, then initialize the same session without starting playback.
    // A successful recovery retains the validated WAV and resets its position.
    // poll runs at driver-call boundaries; request_stop cancels further setup
    // for Close. The caller must close the session after such cancellation.
    [[nodiscard]] PlaybackResult reinitialize(asio::DriverSession& session, PlaybackObserver observer = {});

private:
    void observe_idle_fault() noexcept;
    [[nodiscard]] bool playlist() const noexcept { return paths_ != nullptr; }
    [[nodiscard]] PlaybackResult settle_track_refusal(asio::DriverLease& device, PlaybackResult result,
        RequestProgress& progress, std::size_t initial_index);

    struct Requests {
        std::atomic<bool> pause{};
        std::atomic<bool> stop{};
    };
    // Protects the file and holds its validated metadata; each play() reads an independent reopened cursor.
    std::shared_ptr<media::WavReader> track_;
    std::shared_ptr<const std::vector<std::wstring>> paths_; // Null for a single WAV; see playlist().
    std::size_t track_index_{};
    std::uint64_t live_position_{};
    // Callback-visible flags have private pages; locking never affects another
    // heap owner. The file and the rest of this control-side object stay pageable.
    core::ResidentObject<Requests> requests_;
    std::uint64_t position_{};
    // Signal token of the last clean stop or retained reinitialization request, or 0.
    std::uint64_t idle_token_{};
    std::optional<PlaybackResult> idle_fault_;
    std::exception_ptr cause_; // See result_cause().
    bool idle_fault_reported_{}, failed_{}, playing_{};
};

// Any thread: wake the control thread's playback or idle wait so that it polls
// queued controls (see PlaybackObserver::poll).
void wake_playback_control() noexcept;

} // namespace wavplay::playback
