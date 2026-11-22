#include "wavplay/stream/ring_layout.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wavplay::stream {

BlockShape plan_block_shape(std::size_t block_frames, std::size_t sample_bytes, const PrefetchPolicy& policy)
{
    if (policy.target_seconds == 0 || policy.batch_source_bytes == 0 || policy.producer_wakes_per_ring == 0) {
        throw std::invalid_argument("Invalid prefetch policy");
    }
    if (policy.refill_pattern != RefillPattern::uniform && policy.producer_wakes_per_ring != 8) {
        throw std::invalid_argument("Nonuniform refill intervals require eight wakes per ring");
    }
    if (sample_bytes == 0 || block_frames == 0 || block_frames > policy.ring_budget_bytes / 4 / sample_bytes) {
        throw std::invalid_argument("Playback block does not fit the prefetch ring");
    }
    const auto bytes = block_frames * sample_bytes;
    const BlockShape shape{block_frames, bytes, core::align_up(bytes, core::cache_line_size)};
    // Keep different blocks on different lines. Small left/right payloads share
    // the two halves of one line; larger channels each need their own stride.
    // Reserve at least two blocks, including their alignment gaps.
    const auto strides_per_two_blocks = bytes <= core::cache_line_size / 2 ? 2u : 4u;
    if (shape.stride > policy.ring_budget_bytes / strides_per_two_blocks) {
        throw std::invalid_argument("Playback block does not fit the prefetch ring");
    }
    return shape;
}

std::array<std::size_t, 4> RingLayout::wake_intervals(RefillPattern pattern) const
{
    const auto delta = wake_interval / 4;
    const auto shorter = wake_interval - delta;
    const auto longer = wake_interval + delta;
    switch (pattern) {
    case RefillPattern::uniform:     return {wake_interval, wake_interval, wake_interval, wake_interval};
    case RefillPattern::alternating: return {shorter, longer, shorter, longer};
    case RefillPattern::paired:      return {shorter, shorter, longer, longer};
    }
    throw std::invalid_argument("Unknown refill pattern");
}

RingLayout plan_ring_layout(const BlockShape& block, std::uint64_t run_frames, std::uint32_t sample_rate,
    const PrefetchPolicy& policy)
{
    if (run_frames == 0 || sample_rate == 0 || block.frames == 0 ||
        block.bytes % block.frames != 0) {
        throw std::invalid_argument("Invalid prefetch ring dimensions");
    }
    const auto checked = plan_block_shape(block.frames, block.bytes / block.frames, policy);
    if (block.stride != checked.stride) {
        throw std::invalid_argument("Invalid prefetch block stride");
    }
    RingLayout layout{block};
    layout.total_blocks = 1 + (run_frames - 1) / block.frames;
    layout.final_frames = static_cast<std::size_t>(1 + (run_frames - 1) % block.frames);
    // Saturating the duration still fills the memory budget; wrapping it would
    // silently turn a large requested duration into an undersized ring.
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    const auto target_frames = policy.target_seconds > max_size / sample_rate
        ? max_size : policy.target_seconds * sample_rate;
    const auto target = 1 + (target_frames - 1) / block.frames;
    const bool shared_line = block.bytes <= core::cache_line_size / 2;
    const auto max_capacity = policy.ring_budget_bytes / block.stride / (shared_line ? 1 : 2);
    layout.capacity = static_cast<std::size_t>(
        std::min<std::uint64_t>(layout.total_blocks, std::clamp<std::size_t>(target, 2, max_capacity)));
    layout.plane_bytes = block.stride * layout.capacity;
    layout.channel_offset = shared_line ? core::cache_line_size / 2 : layout.plane_bytes;
    layout.wake_interval = std::max<std::size_t>(1, layout.capacity / policy.producer_wakes_per_ring);
    return layout;
}

} // namespace wavplay::stream
