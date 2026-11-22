#pragma once

#include "wavplay/core/alignment.h"
#include "wavplay/core/thread_hints.h"

#include <cstddef>

namespace wavplay::stream {

enum class RefillPattern { uniform, alternating, paired };

// Producer-side tuning of the prefetch ring. The ring planner uses the memory
// and wake limits; GaplessRun sizes each fill for its current source. The
// real-time consumer only copies completed blocks.
struct PrefetchPolicy {
    // Page-backed bytes for both channel planes, including alignment gaps.
    std::size_t ring_budget_bytes = 16 * 1024 * 1024;
    // Buffered audio the ring aims to hold, bounded by the budget above.
    std::size_t target_seconds = 2;
    // Target interleaved source bytes per batch, recomputed for each track's
    // byte width and rounded down to whole blocks (at least one). The default
    // matches media::WavReader::staging_bytes: an ordinary fill uses one ReadFile.
    // An oversized block or a block crossing tracks can require multiple reads.
    std::size_t batch_source_bytes = 8 * 1024;
    // Consumer notifications per ring capacity, waking a producer blocked on a full ring.
    std::size_t producer_wakes_per_ring = 8;
    RefillPattern refill_pattern = RefillPattern::uniform;
};

inline constexpr PrefetchPolicy default_prefetch_policy{};

// The producer's speed for the steady refills of a streaming run, requested
// once after priming; preparation, whole-run validation and priming keep the
// thread's default. Without an explicit hint, Windows selects QoS from process
// visibility, audibility and other heuristics. The system setting leaves that
// selection to Windows. An explicit high hint requests HighQoS even when
// unfocused or minimized; it does not fix CPU affinity or guarantee placement.
inline constexpr core::ExecutionSpeed reader_refill_speed = core::ExecutionSpeed::system;

// The width check reserves four equally aligned channel slots (two stereo blocks).
static_assert(default_prefetch_policy.ring_budget_bytes % (4 * core::cache_line_size) == 0);
static_assert(default_prefetch_policy.target_seconds > 0 && default_prefetch_policy.producer_wakes_per_ring > 0);

} // namespace wavplay::stream
