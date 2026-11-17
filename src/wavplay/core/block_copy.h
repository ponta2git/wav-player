#pragma once

#include <cstddef>
#include <cstring>

#include <emmintrin.h>

namespace wavplay::core {

// Block copy and zero for the real-time path, also used for the producer's
// same-width mono duplication: fixed-width 128-bit integer loads and stores,
// compiled with the product's AVX2 target (VEX encoding). Unlike CRT copy/zero
// there is no
// runtime dispatch (no __isa_available/__favor reads, no .rdata jump table, no
// far code page), no rep movs and no non-temporal stores; the driver
// reads the callback's destination next, so it stays in the cache.
//
// Preconditions: [src, src + bytes) and [dst, dst + bytes) do not overlap.
// Exactly [dst, dst + bytes) is written and [src, src + bytes) is read. Every
// load and store is unaligned, so correctness never depends on
// alignment; 64-byte slot alignment is a performance property only.
//
// From 16 bytes an ascending 16-byte loop ends with one final 16-byte store
// that may overlap the previous one. Shorter blocks use overlapping
// constant-size pieces, which MSVC emits as plain moves: a byte loop or a
// variable-size std::memcpy/std::memset would become a CRT call again. The
// bodies are forced inline because MSVC keeps them out of line for a
// run-time size, as a separate function the linker places anywhere.

[[msvc::forceinline]] inline void copy_block(void* dst, const void* src, std::size_t bytes) noexcept
{
    auto* const to = static_cast<std::byte*>(dst);
    const auto* const from = static_cast<const std::byte*>(src);
    if (bytes >= 16) {
        const auto last = bytes - 16;
        for (std::size_t offset = 0; offset < last; offset += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + offset),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + offset)));
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + last),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + last)));
    } else if (bytes >= 8) {
        std::memcpy(to, from, 8);
        std::memcpy(to + bytes - 8, from + bytes - 8, 8);
    } else if (bytes >= 4) {
        std::memcpy(to, from, 4);
        std::memcpy(to + bytes - 4, from + bytes - 4, 4);
    } else if (bytes >= 2) {
        std::memcpy(to, from, 2);
        std::memcpy(to + bytes - 2, from + bytes - 2, 2);
    } else if (bytes == 1) {
        std::memcpy(to, from, 1);
    }
}

// The two destination spans are disjoint, and neither overlaps a source span.
// All spans contain exactly `bytes` accessible bytes; no alignment is required.
// Copy channels in separate passes using the same bounded copy operation.
// Keep the channel contract here without a size-dependent dispatch policy.
[[msvc::forceinline]] inline void copy_stereo_blocks(void* left, void* right,
    const void* source_left, const void* source_right, std::size_t bytes) noexcept
{
    copy_block(left, source_left, bytes);
    copy_block(right, source_right, bytes);
}

[[msvc::forceinline]] inline void duplicate_block(void* left, void* right,
    const void* source, std::size_t bytes) noexcept
{
    copy_block(left, source, bytes);
    copy_block(right, source, bytes);
}

[[msvc::forceinline]] inline void zero_block(void* dst, std::size_t bytes) noexcept
{
    auto* const to = static_cast<std::byte*>(dst);
    if (bytes >= 16) {
        const auto zero = _mm_setzero_si128();
        const auto last = bytes - 16;
        for (std::size_t offset = 0; offset < last; offset += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + offset), zero);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + last), zero);
    } else if (bytes >= 8) {
        std::memset(to, 0, 8);
        std::memset(to + bytes - 8, 0, 8);
    } else if (bytes >= 4) {
        std::memset(to, 0, 4);
        std::memset(to + bytes - 4, 0, 4);
    } else if (bytes >= 2) {
        std::memset(to, 0, 2);
        std::memset(to + bytes - 2, 0, 2);
    } else if (bytes == 1) {
        std::memset(to, 0, 1);
    }
}

} // namespace wavplay::core
