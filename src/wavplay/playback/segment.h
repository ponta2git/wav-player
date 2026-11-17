#pragma once

#include "wavplay/asio/asio_sdk.h"
#include "wavplay/playback/playback_signal.h"
#include "wavplay/playback/playback_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace wavplay::stream {
class PrefetchRing;
}

namespace wavplay::playback {

// True between a successful start return and the control thread's stop call.
[[nodiscard]] bool is_streaming() noexcept;

// The transport requests of one playback request, shared by its control thread
// and callbacks. Requests that cannot pause or stop pass atomics that are never set.
// The callback only reads the requests and counts valid PCM in its renderer.
// After all writers detach, the control thread accumulates that segment's count
// in submitted_frames, including partial-start and fault exits.
struct SegmentControl {
    const std::atomic<bool>& pause_requested;
    const std::atomic<bool>& stop_requested;
    std::uint64_t submitted_frames{};
    // A renderer publishes its count without exposing its mutable audio state.
    // Pointer is assigned and cleared only on the control thread.
    const std::atomic<std::uint64_t>* live_frames{};
    [[nodiscard]] std::uint64_t progress_frames() const noexcept
    {
        return submitted_frames + (live_frames ? live_frames->load(std::memory_order_relaxed) : 0);
    }
    PlaybackSignal::Token phase_token{};
    // The clean stop the previous request of this track ended with, if any. Checked
    // atomically when rearming after pause/Stop/EOF, before touching the driver.
    PlaybackSignal::Token previous_idle_token{};
    PlaybackObserver observer{};
    // Describes only the two referenced atomic flags, not this control object.
    bool request_pages_locked{};

    // Control thread: a Stop request ends `token`'s phase. Callbacks check the
    // request themselves (segment_renderer.cpp) before consuming another block.
    void accept_stop(PlaybackSignal& signal, PlaybackSignal::Token token) const noexcept
    {
        if (stop_requested.load(std::memory_order_acquire)) {
            signal.stop(token);
        }
    }

    // Control thread: deliver queued controls to the observer, then honor Stop.
    void poll(PlaybackSignal& signal, PlaybackSignal::Token token) const noexcept
    {
        if (observer.poll) {
            observer.poll(observer.context);
        }
        accept_stop(signal, token);
    }
};

// One segment: a single ASIO buffer allocation at one sample rate, from
// createBuffers through prefill, start, drain, stop and disposeBuffers on the
// control thread. Borrows the primed queue until the segment has stopped and
// detached all writers. start_silence is rounded up to whole blocks returned
// before the first PCM block. The request has already selected the queue's
// exact width from the driver's limits at this segment's confirmed rate.
[[nodiscard]] PlaybackResult run_segment(IASIO& driver, stream::PrefetchRing& queue, PlaybackSignal::Token token,
    SegmentControl& control, std::optional<BufferSetup>* setup = nullptr,
    std::chrono::milliseconds start_silence = {}, long output_pair = 1);

} // namespace wavplay::playback
