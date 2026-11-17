#pragma once

#include <array>
#include <optional>
#include <vector>

namespace wavplay::playback {

// Why a playback request (or its current segment) stopped. playing is the
// in-progress value; every other value is a terminal reason.
enum class PlaybackResult {
    playing, completed, underrun, read_error, driver_error,
    next_rate_rejected, next_track_rejected, first_rate_rejected, paused, stopped,
    reinitialize_required
};

[[nodiscard]] constexpr bool is_track_refusal(PlaybackResult result) noexcept
{
    return result == PlaybackResult::next_track_rejected || result == PlaybackResult::next_rate_rejected;
}

// A request that ended without a fault: drained or paused, user-stopped, or
// at EOF. Every other non-playing result is a fault or refusal.
[[nodiscard]] constexpr bool is_clean_stop(PlaybackResult result) noexcept
{
    return result == PlaybackResult::paused || result == PlaybackResult::stopped ||
        result == PlaybackResult::completed;
}

// A terminal result the user must see: neither playing nor a clean stop.
// Callers classify results here, never by whether a message text exists.
[[nodiscard]] constexpr bool is_fault(PlaybackResult result) noexcept
{
    return result != PlaybackResult::playing && !is_clean_stop(result);
}

static_assert(!is_fault(PlaybackResult::playing) && !is_fault(PlaybackResult::completed) &&
    !is_fault(PlaybackResult::paused) && !is_fault(PlaybackResult::stopped));
static_assert(is_fault(PlaybackResult::underrun) && is_fault(PlaybackResult::read_error) &&
    is_fault(PlaybackResult::driver_error) && is_fault(PlaybackResult::first_rate_rejected) &&
    is_fault(PlaybackResult::next_rate_rejected) && is_fault(PlaybackResult::next_track_rejected) &&
    is_fault(PlaybackResult::reinitialize_required));

// Optional control-thread observer. Never called on the audio callback and
// must not reenter play or throw. poll delivers queued controls after waking.
struct PlaybackObserver {
    void (*started)(void*) noexcept{};
    void (*poll)(void*) noexcept{};
    void* context{};
};

// Validated limits queried at the confirmed playback rate, before choosing a
// width or allocating buffers. Retained even if the request or creation fails.
struct BufferCapabilities {
    double sample_rate;
    long minimum, maximum, preferred, granularity;
};

// One verified ASIO allocation, sampled on the control thread before start. The
// fields use the SDK's underlying types (ASIOSampleRate is double, ASIOSampleType
// is long; checked in segment.cpp) so GUI code can show them without the SDK.
struct BufferSetup {
    double sample_rate;
    std::array<long, 2> output_types;
    long buffer_frames;         // The width passed to createBuffers.
    long output_latency_frames; // Raw getLatencies result, before drain accounting.
    long start_silence_blocks;  // Silent blocks, including prefilled B, before the first PCM block.
    bool ring_pages_locked;     // Audio planes only; a refusal leaves them pageable.
    bool ring_metadata_pages_locked;
    bool renderer_pages_locked;
    bool request_pages_locked;  // Pause/Stop flags, not the control object or driver buffers.
    // This executable's image (core::lock_process_image), for the whole process.
    // False in a process that never calls it, such as a test suite's.
    bool image_pages_locked;
};

struct PlaybackDiagnostics {
    std::size_t current_segment{};
    // Same-rate runs share an allocation. Grow for each new rate; two initial
    // slots preserve the original single/pair diagnostic API.
    std::vector<std::optional<BufferCapabilities>> buffer_capabilities{2};
    std::vector<std::optional<BufferSetup>> segments{2};

    // Restores the default state in place. Unlike `= {}` it keeps both
    // vectors' storage, whose capacity never drops below the two initial
    // slots, so it does not allocate. Only a moved-from object would
    // allocate them again, and a failure there terminates.
    void reset() noexcept
    {
        current_segment = 0;
        buffer_capabilities.assign(2, std::nullopt);
        segments.assign(2, std::nullopt);
    }
};

} // namespace wavplay::playback
