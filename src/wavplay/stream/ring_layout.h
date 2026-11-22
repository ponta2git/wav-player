#pragma once

#include "wavplay/stream/prefetch_policy.h"

#include <cstddef>
#include <cstdint>

namespace wavplay::stream {

// One channel's slot for a block of planar samples.
struct BlockShape {
    std::size_t frames{};
    std::size_t bytes{};  // Valid bytes of one channel's block.
    std::size_t stride{}; // Distance between blocks of either channel, rounded to a cache line.
};

// Validates the policy and the block width before any file is opened. Throws
// std::invalid_argument unless two stereo blocks fit the policy's ring budget.
// Channels of at most half a cache line share one line within each block;
// larger channels occupy separate cache-line-aligned planes.
[[nodiscard]] BlockShape plan_block_shape(std::size_t block_frames, std::size_t sample_bytes,
    const PrefetchPolicy& policy = default_prefetch_policy);

// Geometry of a ring for one gapless run. Left block i starts at i * stride;
// the corresponding right block is channel_offset bytes later. Different blocks
// never share a cache line, including the small-channel interleaved layout.
struct RingLayout {
    BlockShape block;
    std::size_t capacity{};       // Slots per plane.
    std::size_t plane_bytes{};    // stride * capacity: consumer wrap offset, not the channel offset.
    std::size_t channel_offset{}; // Half a line for small channels, otherwise plane_bytes.
    std::uint64_t total_blocks{}; // Blocks in the whole run, the last one zero-padded.
    std::size_t final_frames{};   // Valid frames of the last block.
    std::size_t wake_interval{};  // Mean number of consumed blocks between producer wakes.

    // The consumer wakes the producer after alternating shorter and longer
    // intervals, the shorter first. Each pair spans two mean intervals; below
    // a mean of four blocks the two are equal.
    [[nodiscard]] std::size_t short_wake_interval() const noexcept { return wake_interval - wake_interval / 4; }
    [[nodiscard]] std::size_t long_wake_interval() const noexcept { return wake_interval + wake_interval / 4; }

    [[nodiscard]] std::size_t allocation_bytes() const noexcept
    {
        return channel_offset == plane_bytes ? 2 * plane_bytes : plane_bytes;
    }
};

// Sizes the ring to hold the policy's target duration within its memory budget,
// but never more blocks than the run itself. Rejects zero run_frames or sample_rate,
// an invalid policy, and a block that does not match plan_block_shape with that policy.
[[nodiscard]] RingLayout plan_ring_layout(const BlockShape& block, std::uint64_t run_frames, std::uint32_t sample_rate,
    const PrefetchPolicy& policy = default_prefetch_policy);

} // namespace wavplay::stream
