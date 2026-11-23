#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <new>

namespace wavplay::core {

// Producer and consumer cursors, callback state and each audio block start on a
// line of their own. 64 bytes on every x86-64 CPU the app targets.
inline constexpr std::size_t cache_line_size = std::hardware_destructive_interference_size;
// The ring layout (two channels of at most 32 bytes share a line), the resident
// structures' line maps and the requirements all state 64 bytes. A toolset
// that reports another size must fail here, not shift those layouts.
static_assert(cache_line_size == 64);

// An adjacent-line prefetcher completes 128-byte-aligned pairs of lines, so a
// line written by one thread can pull its neighbour from another core. Not
// every targeted core documents such a prefetcher; the pair costs only
// padding where none exists. Use it only to separate data written by
// different threads, never for audio slots.
inline constexpr std::size_t adjacent_line_pair = 2 * cache_line_size;

// Rounds value up to a multiple of alignment. Preconditions: alignment is
// nonzero, and value + alignment - 1 does not overflow (callers bound value
// first). The arithmetic needs no power of two, but an alignment applied to
// addresses must be one. Debug builds assert both preconditions.
[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
{
    assert(alignment != 0 && value <= std::numeric_limits<std::size_t>::max() - (alignment - 1));
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace wavplay::core
