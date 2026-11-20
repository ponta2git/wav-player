#include "wavplay/audio/planar_conversion.h"

#include "wavplay/core/block_copy.h"
#include "wavplay/core/byte_order.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace wavplay::audio {
namespace {

using core::load_le;
using core::store_le;

// Startup requires AVX2 and OS support for XMM/YMM state. Kernels use integer
// operations and unaligned loads/stores, even for IEEE encodings. A pshufb
// index with its high bit set writes a zero byte.
constexpr char pshufb_zero = -1;

// Exact-width SIMD loops compare against a rounded-down end. Packed stores
// that also write slack bytes retain their larger remaining-frame checks.
// A scalar tail after a SIMD loop of 4 or 8 frames counts down `frames & 3` or
// `frames & 7` frames. With the trip count visibly below the step, MSVC does not
// auto-vectorize the tail into a wide loop that can never run, together with its
// alias checks and the register saves that every call with a tail would pay.

// Same-width little-endian mono samples use core::duplicate_block for two
// fixed copies, keeping runtime copy dispatch out of this path.
// Its loads are unaligned: each block's source follows the
// previous block's input bytes in the staging buffer (94 bytes for 47 mono PCM16
// frames), so it is not on a cache line. It reads and writes exactly the given
// bytes, so guard pages and slot gaps stay untouched.

// Four left and four right samples in the working layout of a kernel.
struct Planes {
    __m128i left;
    __m128i right;
};

template<std::size_t Bytes, bool BigEndian>
void store_sample(std::byte* target, std::uint32_t bits) noexcept
{
    if constexpr (Bytes == 2) {
        auto value = static_cast<std::uint16_t>(bits);
        if constexpr (BigEndian) { value = std::byteswap(value); }
        store_le(target, value);
    } else if constexpr (Bytes == 4) {
        if constexpr (BigEndian) { bits = std::byteswap(bits); }
        store_le(target, bits);
    } else {
        static_assert(Bytes == 3);
        if constexpr (BigEndian) {
            store_le(target, std::byteswap(static_cast<std::uint16_t>(bits >> 8)));
            target[2] = static_cast<std::byte>(bits);
        } else {
            store_le(target, static_cast<std::uint16_t>(bits));
            target[2] = static_cast<std::byte>(bits >> 16);
        }
    }
}

// The common working value occupies bits 31..0 at full Int32 scale. Arithmetic
// right shift preserves its sign and places an N-bit ASIO integer in low bits;
// precision expansion leaves zeros below the original least significant bit.
template<unsigned ValidBits, bool BigEndian>
__m128i format_int32(__m128i samples) noexcept
{
    if constexpr (ValidBits < 32) { samples = _mm_srai_epi32(samples, 32 - ValidBits); }
    if constexpr (BigEndian) {
        samples = _mm_shuffle_epi8(samples, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    }
    return samples;
}

template<unsigned ValidBits, bool BigEndian>
void store_int32(std::byte* target, std::uint32_t msb_bits) noexcept
{
    const auto scaled = std::bit_cast<std::int32_t>(msb_bits) >> (32 - ValidBits);
    store_sample<4, BigEndian>(target, std::bit_cast<std::uint32_t>(scaled));
}

template<bool BigEndian>
void pcm16_stereo_to_int16(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    // L0 L1 L2 L3 | R0 R1 R2 R3 from four interleaved stereo words.
    const auto split = BigEndian
        ? _mm_setr_epi8(1, 0, 5, 4, 9, 8, 13, 12, 3, 2, 7, 6, 11, 10, 15, 14)
        : _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15);
    std::size_t i = 0;
    for (const auto simd_end = frames & ~std::size_t{7}; i < simd_end; i += 8) {
        const auto first = _mm_shuffle_epi8(load_le<__m128i>(source + 4 * i), split);
        const auto second = _mm_shuffle_epi8(load_le<__m128i>(source + 4 * i + 16), split);
        store_le(left + 2 * i, _mm_unpacklo_epi64(first, second));
        store_le(right + 2 * i, _mm_unpackhi_epi64(first, second));
    }
    for (auto n = frames & 7; n != 0; --n, ++i) {
        store_sample<2, BigEndian>(left + 2 * i, load_le<std::uint16_t>(source + 4 * i));
        store_sample<2, BigEndian>(right + 2 * i, load_le<std::uint16_t>(source + 4 * i + 2));
    }
}

template<bool BigEndian>
void pcm16_mono_to_int16(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    if constexpr (!BigEndian) {
        core::duplicate_block(left, right, source, frames * sizeof(std::int16_t));
    } else {
        const auto swap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        std::size_t i = 0;
        for (const auto simd_end = frames & ~std::size_t{7}; i < simd_end; i += 8) {
            const auto samples = _mm_shuffle_epi8(load_le<__m128i>(source + 2 * i), swap);
            store_le(left + 2 * i, samples);
            store_le(right + 2 * i, samples);
        }
        for (auto n = frames & 7; n != 0; --n, ++i) {
            const auto sample = load_le<std::uint16_t>(source + 2 * i);
            store_sample<2, true>(left + 2 * i, sample);
            store_sample<2, true>(right + 2 * i, sample);
        }
    }
}

template<bool Mono, bool BigEndian>
void pcm16_to_int24(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    constexpr std::size_t frame_bytes = Mono ? 2 : 4;
    constexpr std::size_t right_offset = Mono ? 0 : 2;
    // Four frames become 12 packed bytes per channel: a zero low byte, then the
    // PCM16 word, so full-scale PCM16 occupies bits 23..8. Each 16-byte store also
    // writes 4 zero bytes past the 12; the next store or the scalar tail
    // overwrites them, and the loop keeps every store inside the destination.
    // 3 * (frames - i) >= 16 bytes remain for each store while i + 6 <= frames.
    std::size_t i = 0;
    if constexpr (Mono) {
        const auto pack = BigEndian
            ? _mm_setr_epi8(1, 0, pshufb_zero, 3, 2, pshufb_zero, 5, 4, pshufb_zero, 7, 6, pshufb_zero,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero)
            : _mm_setr_epi8(pshufb_zero, 0, 1, pshufb_zero, 2, 3, pshufb_zero, 4, 5, pshufb_zero, 6, 7,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero);
        for (; i + 6 <= frames; i += 4) {
            const auto packed = _mm_shuffle_epi8(_mm_cvtsi64_si128(load_le<std::int64_t>(source + 2 * i)), pack);
            store_le(left + 3 * i, packed);
            store_le(right + 3 * i, packed);
        }
    } else {
        const auto pack_left = BigEndian
            ? _mm_setr_epi8(1, 0, pshufb_zero, 5, 4, pshufb_zero, 9, 8, pshufb_zero, 13, 12, pshufb_zero,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero)
            : _mm_setr_epi8(pshufb_zero, 0, 1, pshufb_zero, 4, 5, pshufb_zero, 8, 9, pshufb_zero, 12, 13,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero);
        const auto pack_right = BigEndian
            ? _mm_setr_epi8(3, 2, pshufb_zero, 7, 6, pshufb_zero, 11, 10, pshufb_zero, 15, 14, pshufb_zero,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero)
            : _mm_setr_epi8(pshufb_zero, 2, 3, pshufb_zero, 6, 7, pshufb_zero, 10, 11, pshufb_zero, 14, 15,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero);
        for (; i + 6 <= frames; i += 4) {
            const auto pairs = load_le<__m128i>(source + 4 * i);
            store_le(left + 3 * i, _mm_shuffle_epi8(pairs, pack_left));
            store_le(right + 3 * i, _mm_shuffle_epi8(pairs, pack_right));
        }
    }
    for (; i < frames; ++i) {
        const auto* frame = source + i * frame_bytes;
        // Full-scale PCM16 occupies bits 23..8 of the packed 24-bit integer.
        if constexpr (!BigEndian) {
            auto* l = left + 3 * i;
            auto* r = right + 3 * i;
            l[0] = r[0] = std::byte{};
            std::memcpy(l + 1, frame, 2);
            std::memcpy(r + 1, frame + right_offset, 2);
        } else {
            store_sample<3, true>(left + 3 * i, std::uint32_t{load_le<std::uint16_t>(frame)} << 8);
            store_sample<3, true>(right + 3 * i, std::uint32_t{load_le<std::uint16_t>(frame + right_offset)} << 8);
        }
    }
}

// Four PCM16 frames at full Int32 scale, optionally with each word already
// byte-reversed for a 32-bit big-endian output.
template<bool Mono, bool Reversed>
Planes load_pcm16_words(const std::byte* frame) noexcept
{
    if constexpr (Mono) {
        // Interleave four input words with zero low words.
        const auto words = _mm_cvtsi64_si128(load_le<std::int64_t>(frame));
        if constexpr (Reversed) {
            const auto samples = _mm_shuffle_epi8(words, _mm_setr_epi8(1, 0, pshufb_zero, pshufb_zero,
                3, 2, pshufb_zero, pshufb_zero, 5, 4, pshufb_zero, pshufb_zero, 7, 6, pshufb_zero, pshufb_zero));
            return {samples, samples};
        } else {
            const auto samples = _mm_unpacklo_epi16(_mm_setzero_si128(), words);
            return {samples, samples};
        }
    } else {
        // Each little-endian stereo pair already has its right sample in the high word.
        const auto pairs = load_le<__m128i>(frame);
        if constexpr (Reversed) {
            return {_mm_shuffle_epi8(pairs, _mm_setr_epi8(1, 0, pshufb_zero, pshufb_zero,
                        5, 4, pshufb_zero, pshufb_zero, 9, 8, pshufb_zero, pshufb_zero, 13, 12, pshufb_zero, pshufb_zero)),
                _mm_shuffle_epi8(pairs, _mm_setr_epi8(3, 2, pshufb_zero, pshufb_zero,
                    7, 6, pshufb_zero, pshufb_zero, 11, 10, pshufb_zero, pshufb_zero, 15, 14, pshufb_zero, pshufb_zero))};
        } else {
            return {_mm_slli_epi32(pairs, 16), _mm_and_si128(pairs, _mm_set1_epi32(static_cast<int>(0xffff0000u)))};
        }
    }
}

template<bool Mono, unsigned ValidBits, bool BigEndian>
void pcm16_to_int32(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    constexpr std::size_t frame_bytes = Mono ? 2 : 4;
    std::size_t i = 0;
    for (const auto simd_end = frames & ~std::size_t{3}; i < simd_end; i += 4) {
        const auto samples = load_pcm16_words<Mono, BigEndian && ValidBits == 32>(source + i * frame_bytes);
        store_le(left + 4 * i, format_int32<ValidBits, BigEndian && ValidBits != 32>(samples.left));
        store_le(right + 4 * i, format_int32<ValidBits, BigEndian && ValidBits != 32>(samples.right));
    }
    for (auto n = frames & 3; n != 0; --n, ++i) {
        const auto* frame = source + i * frame_bytes;
        const auto left_bits = std::uint32_t{load_le<std::uint16_t>(frame)} << 16;
        const auto right_bits = Mono ? left_bits : std::uint32_t{load_le<std::uint16_t>(frame + 2)} << 16;
        store_int32<ValidBits, BigEndian>(left + 4 * i, left_bits);
        store_int32<ValidBits, BigEndian>(right + 4 * i, right_bits);
    }
}

template<bool Mono, bool BigEndian>
void pcm24_to_int24(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    if constexpr (Mono && !BigEndian) {
        core::duplicate_block(left, right, source, frames * 3);
    } else {
        constexpr std::size_t frame_bytes = Mono ? 3 : 6;
        constexpr std::size_t right_offset = Mono ? 0 : 3;
        std::size_t i = 0;
        if constexpr (Mono) {
            const auto pack = _mm_setr_epi8(2, 1, 0, 5, 4, 3, 8, 7, 6, 11, 10, 9,
                pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero);
            // Four frames consume 12 bytes, but the load reads 16. Six remaining
            // frames provide 18 readable bytes and 18 output bytes, including
            // the store slack. Final 2..5 frames are scalar.
            for (; i + 6 <= frames; i += 4) {
                const auto samples = _mm_shuffle_epi8(load_le<__m128i>(source + 3 * i), pack);
                store_le(left + 3 * i, samples);
                store_le(right + 3 * i, samples);
            }
        } else {
            // Two frames become L0,L1 in the low 64 bits and R0,R1 in the high
            // 64 bits, each lane with two zero slack bytes.
            const auto split = BigEndian
                ? _mm_setr_epi8(2, 1, 0, 8, 7, 6, pshufb_zero, pshufb_zero, 5, 4, 3, 11, 10, 9, pshufb_zero, pshufb_zero)
                : _mm_setr_epi8(0, 1, 2, 6, 7, 8, pshufb_zero, pshufb_zero, 3, 4, 5, 9, 10, 11, pshufb_zero, pshufb_zero);
            // Three remaining frames provide 18 input bytes and 9 bytes per
            // output. Thus the 16-byte load and both 8-byte stores stay inside
            // their spans. The next iteration or scalar tail overwrites the two
            // slack bytes without touching a gap.
            for (; i + 3 <= frames; i += 2) {
                const auto pairs = _mm_shuffle_epi8(load_le<__m128i>(source + 6 * i), split);
                store_le(left + 3 * i, _mm_cvtsi128_si64(pairs));
                store_le(right + 3 * i, _mm_cvtsi128_si64(_mm_srli_si128(pairs, 8)));
            }
        }
        for (; i < frames; ++i) {
            const auto* frame = source + i * frame_bytes;
            if constexpr (!BigEndian) {
                std::memcpy(left + 3 * i, frame, 3);
                std::memcpy(right + 3 * i, frame + right_offset, 3);
            } else {
                const auto read = [](const std::byte* sample) {
                    return std::uint32_t{load_le<std::uint16_t>(sample)} | (std::uint32_t{static_cast<std::uint8_t>(sample[2])} << 16);
                };
                const auto l = read(frame), r = Mono ? l : read(frame + right_offset);
                store_sample<3, true>(left + 3 * i, l);
                store_sample<3, true>(right + 3 * i, r);
            }
        }
    }
}

// Two 16-byte loads at offsets 0 and 8 read exactly the 24 bytes of four stereo
// or eight mono frames and place each sample in the high three bytes of a
// 32-bit lane: L0 L1 R0 R1 | L2 L3 R2 R3, or S0..S3 | S4..S7. Big-endian
// outputs reverse each word in format_int32.
template<bool Mono>
Planes load_pcm24_group(const std::byte* group) noexcept
{
    const auto first = Mono
        ? _mm_setr_epi8(pshufb_zero, 0, 1, 2, pshufb_zero, 3, 4, 5, pshufb_zero, 6, 7, 8, pshufb_zero, 9, 10, 11)
        : _mm_setr_epi8(pshufb_zero, 0, 1, 2, pshufb_zero, 6, 7, 8, pshufb_zero, 3, 4, 5, pshufb_zero, 9, 10, 11);
    const auto second = Mono
        ? _mm_setr_epi8(pshufb_zero, 4, 5, 6, pshufb_zero, 7, 8, 9, pshufb_zero, 10, 11, 12, pshufb_zero, 13, 14, 15)
        : _mm_setr_epi8(pshufb_zero, 4, 5, 6, pshufb_zero, 10, 11, 12, pshufb_zero, 7, 8, 9, pshufb_zero, 13, 14, 15);
    return {_mm_shuffle_epi8(load_le<__m128i>(group), first), _mm_shuffle_epi8(load_le<__m128i>(group + 8), second)};
}

template<bool Mono, unsigned ValidBits, bool BigEndian>
void pcm24_to_int32(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    constexpr std::size_t frame_bytes = Mono ? 3 : 6;
    constexpr std::size_t group_frames = Mono ? 8 : 4;
    std::size_t i = 0;
    for (const auto simd_end = frames & ~(group_frames - 1); i < simd_end; i += group_frames) {
        const auto halves = load_pcm24_group<Mono>(source + i * frame_bytes);
        if constexpr (Mono) {
            const auto first = format_int32<ValidBits, BigEndian>(halves.left);
            const auto second = format_int32<ValidBits, BigEndian>(halves.right);
            store_le(left + 4 * i, first);
            store_le(left + 4 * i + 16, second);
            store_le(right + 4 * i, first);
            store_le(right + 4 * i + 16, second);
        } else {
            store_le(left + 4 * i, format_int32<ValidBits, BigEndian>(_mm_unpacklo_epi64(halves.left, halves.right)));
            store_le(right + 4 * i, format_int32<ValidBits, BigEndian>(_mm_unpackhi_epi64(halves.left, halves.right)));
        }
    }
    for (auto n = frames & (group_frames - 1); n != 0; --n, ++i) {
        const auto* frame = source + i * frame_bytes;
        const auto read = [](const std::byte* sample) {
            return (std::uint32_t{load_le<std::uint16_t>(sample)} << 8) | (std::uint32_t{static_cast<std::uint8_t>(sample[2])} << 24);
        };
        const auto l = read(frame), r = Mono ? l : read(frame + 3);
        store_int32<ValidBits, BigEndian>(left + 4 * i, l);
        store_int32<ValidBits, BigEndian>(right + 4 * i, r);
    }
}

template<bool Mono, bool BigEndian>
void pcm24_in32_to_int24(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    constexpr std::size_t frame_bytes = Mono ? 4 : 8;
    // The right sample's container; its three valid bytes start one byte later.
    constexpr std::size_t right_container = Mono ? 0 : 4;
    std::size_t i = 0;
    if constexpr (Mono) {
        const auto pack = BigEndian
            ? _mm_setr_epi8(3, 2, 1, 7, 6, 5, 11, 10, 9, 15, 14, 13, pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero)
            : _mm_setr_epi8(1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, pshufb_zero, pshufb_zero, pshufb_zero, pshufb_zero);
        // Four complete input samples occupy exactly 16 bytes. The 16-byte
        // stores pack 12 valid bytes and four slack bytes, so six remaining
        // frames (18 output bytes) keep both stores within their destinations.
        for (; i + 6 <= frames; i += 4) {
            const auto samples = _mm_shuffle_epi8(load_le<__m128i>(source + 4 * i), pack);
            store_le(left + 3 * i, samples);
            store_le(right + 3 * i, samples);
        }
    } else {
        const auto split = BigEndian
            ? _mm_setr_epi8(3, 2, 1, 11, 10, 9, pshufb_zero, pshufb_zero, 7, 6, 5, 15, 14, 13, pshufb_zero, pshufb_zero)
            : _mm_setr_epi8(1, 2, 3, 9, 10, 11, pshufb_zero, pshufb_zero, 5, 6, 7, 13, 14, 15, pshufb_zero, pshufb_zero);
        // Two whole stereo frames occupy exactly 16 input bytes. Each 8-byte
        // store contains six packed bytes and two slack bytes; three remaining
        // frames leave nine output bytes. The next iteration or scalar tail
        // overwrites the slack without touching a gap or crossing an endpoint.
        for (; i + 3 <= frames; i += 2) {
            const auto pairs = _mm_shuffle_epi8(load_le<__m128i>(source + 8 * i), split);
            store_le(left + 3 * i, _mm_cvtsi128_si64(pairs));
            store_le(right + 3 * i, _mm_cvtsi128_si64(_mm_srli_si128(pairs, 8)));
        }
    }
    for (; i < frames; ++i) {
        const auto* frame = source + i * frame_bytes;
        // The source's low byte was validated as zero before conversion.
        if constexpr (!BigEndian) {
            std::memcpy(left + 3 * i, frame + 1, 3);
            std::memcpy(right + 3 * i, frame + right_container + 1, 3);
        } else {
            store_sample<3, true>(left + 3 * i, load_le<std::uint32_t>(frame) >> 8);
            store_sample<3, true>(right + 3 * i, load_le<std::uint32_t>(frame + right_container) >> 8);
        }
    }
}

template<bool Mono, unsigned ValidBits, bool BigEndian>
void pcm32_to_int32(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    if constexpr (Mono && ValidBits == 32 && !BigEndian) {
        core::duplicate_block(left, right, source, frames * 4);
    } else if constexpr (Mono) {
        std::size_t i = 0;
        for (const auto simd_end = frames & ~std::size_t{3}; i < simd_end; i += 4) {
            const auto samples = format_int32<ValidBits, BigEndian>(load_le<__m128i>(source + 4 * i));
            store_le(left + 4 * i, samples);
            store_le(right + 4 * i, samples);
        }
        for (auto n = frames & 3; n != 0; --n, ++i) {
            const auto bits = load_le<std::uint32_t>(source + 4 * i);
            store_int32<ValidBits, BigEndian>(left + 4 * i, bits);
            store_int32<ValidBits, BigEndian>(right + 4 * i, bits);
        }
    } else {
        std::size_t i = 0;
        for (const auto simd_end = frames & ~std::size_t{3}; i < simd_end; i += 4) {
            // Each load holds L0,R0,L1,R1. Integer shuffles group the channels,
            // then the low/high 64-bit halves join four samples per output.
            // Four whole frames provide exactly the 32 input and 16 output
            // bytes accessed here; no read-ahead or overlapping store is needed.
            const auto split = [](const std::byte* input) {
                const auto samples = load_le<__m128i>(input);
                if constexpr (BigEndian && ValidBits == 32) {
                    // Group and reverse each word in the same shuffle.
                    return _mm_shuffle_epi8(samples,
                        _mm_setr_epi8(3, 2, 1, 0, 11, 10, 9, 8, 7, 6, 5, 4, 15, 14, 13, 12));
                } else {
                    return _mm_shuffle_epi32(samples, _MM_SHUFFLE(3, 1, 2, 0));
                }
            };
            const auto first = split(source + 8 * i), second = split(source + 8 * i + 16);
            store_le(left + 4 * i, format_int32<ValidBits, BigEndian && ValidBits != 32>(_mm_unpacklo_epi64(first, second)));
            store_le(right + 4 * i, format_int32<ValidBits, BigEndian && ValidBits != 32>(_mm_unpackhi_epi64(first, second)));
        }
        for (auto n = frames & 3; n != 0; --n, ++i) {
            store_int32<ValidBits, BigEndian>(left + 4 * i, load_le<std::uint32_t>(source + 8 * i));
            store_int32<ValidBits, BigEndian>(right + 4 * i, load_le<std::uint32_t>(source + 8 * i + 4));
        }
    }
}

// Each *_converter selector names every source, output width and 32-bit integer
// container valid-bit count it maps and returns nullptr for any other, so no
// unnamed combination falls through to a default kernel. Alignment, the valid
// bits of 16-bit, 24-bit and Float outputs, and lossy pairs (fewer output valid
// bits than the source, PCM32 to Float32) are checked only by
// find_planar_converter; the selectors return a kernel for them.
template<bool Mono, unsigned ValidBits, bool BigEndian>
PlanarConvertFn int32_converter(const SampleFormat& source) noexcept
{
    if (source == int16_le) { return pcm16_to_int32<Mono, ValidBits, BigEndian>; }
    if (source == int24_le) { return pcm24_to_int32<Mono, ValidBits, BigEndian>; }
    // PCM32 and validated MSB-aligned PCM24/32 share the Int32 working layout.
    if (source == int24_in32_le || source == int32_le) { return pcm32_to_int32<Mono, ValidBits, BigEndian>; }
    return nullptr;
}

struct NormalizedWords {
    __m128i significand;
    __m128i leading;
};

// The Float encoders and their helpers are forced inline: MSVC otherwise kept
// the Float32->Float64 encoder out of line, passing vectors through memory and
// saving xmm registers on every call.
__forceinline __m128i select_words(__m128i mask, __m128i yes, __m128i no) noexcept
{
    return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
}

template<int Shift>
__forceinline void normalize_step(NormalizedWords& value) noexcept
{
    const auto move = _mm_cmpeq_epi32(_mm_srli_epi32(value.significand, 32 - Shift), _mm_setzero_si128());
    // Each lane shifts by zero or Shift; the same count updates its exponent.
    const auto count = _mm_and_si128(move, _mm_set1_epi32(Shift));
    value.significand = _mm_sllv_epi32(value.significand, count);
    value.leading = _mm_add_epi32(value.leading, count);
}

// Integer leading-zero normalization for four unsigned magnitudes. Zero has
// no leading one; callers mask that case separately. The fixed normalization
// stages use integer operations only and never depend on FP state.
template<bool UpperHalfOnly = false>
__forceinline NormalizedWords normalize_words(__m128i magnitude) noexcept
{
    NormalizedWords result{magnitude, _mm_setzero_si128()};
    // PCM16 at Int32 scale has at most 15 leading zeros when nonzero.
    // Zero is still masked by the caller, so it needs no full normalization.
    if constexpr (!UpperHalfOnly) { normalize_step<16>(result); }
    normalize_step<8>(result);
    normalize_step<4>(result);
    normalize_step<2>(result);
    normalize_step<1>(result);
    return result;
}

struct FloatWords {
    __m128i first;
    __m128i second;
};

template<int FractionShift>
__forceinline FloatWords pack_float64(__m128i sign, __m128i exponent, __m128i fraction) noexcept
{
    const auto empty = _mm_setzero_si128();
    const auto high = _mm_or_si128(sign, _mm_slli_epi32(exponent, 20));
    return {
        _mm_or_si128(_mm_unpacklo_epi32(empty, high),
            _mm_slli_epi64(_mm_unpacklo_epi32(fraction, empty), FractionShift)),
        _mm_or_si128(_mm_unpackhi_epi32(empty, high),
            _mm_slli_epi64(_mm_unpackhi_epi32(fraction, empty), FractionShift))};
}

template<bool FromFloat, std::size_t SourceBytes, std::size_t OutputBytes, bool BigEndian>
__forceinline FloatWords encode_float_words(__m128i samples) noexcept
{
    const auto empty = _mm_setzero_si128();
    const auto sign = _mm_and_si128(samples, _mm_set1_epi32(INT32_MIN));
    FloatWords result{};
    if constexpr (FromFloat) {
        static_assert(OutputBytes == 8);
        const auto exponent = _mm_and_si128(_mm_srli_epi32(samples, 23), _mm_set1_epi32(255));
        const auto fraction = _mm_and_si128(samples, _mm_set1_epi32(0x7fffff));
        const auto special = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(255));
        const auto exponent_zero = _mm_cmpeq_epi32(exponent, empty);
        // Signed zero keeps a zero exponent here, so silence stays on this path.
        const auto widened_exponent = select_words(special, _mm_set1_epi32(2047),
            _mm_andnot_si128(exponent_zero, _mm_add_epi32(exponent, _mm_set1_epi32(896))));
        // The same shift preserves normal fractions and every NaN payload bit,
        // including the quiet/signaling bit. Infinity has a zero fraction.
        result = pack_float64<29>(sign, widened_exponent, fraction);
        // Only Float32 subnormals (magnitude below 2^-126) become normal Float64
        // values and need the integer normalization.
        const auto subnormal = _mm_andnot_si128(_mm_cmpeq_epi32(fraction, empty), exponent_zero);
        if (_mm_movemask_epi8(subnormal) != 0) {
            const auto normalized = normalize_words(fraction);
            const auto tiny_exponent = _mm_sub_epi32(_mm_set1_epi32(905), normalized.leading);
            const auto tiny_fraction = _mm_and_si128(normalized.significand, _mm_set1_epi32(INT32_MAX));
            const auto widened = pack_float64<21>(sign, tiny_exponent, tiny_fraction);
            result.first = select_words(_mm_unpacklo_epi32(subnormal, subnormal), widened.first, result.first);
            result.second = select_words(_mm_unpackhi_epi32(subnormal, subnormal), widened.second, result.second);
        }
    } else {
        // Samples are signed PCM at Int32 scale. Unsigned two's-complement
        // magnitude also represents INT32_MIN without signed overflow.
        // PABSD leaves INT32_MIN as 0x80000000, exactly its unsigned magnitude.
        const auto magnitude = _mm_abs_epi32(samples);
        const auto normalized = normalize_words<SourceBytes == 2>(magnitude);
        const auto is_zero = _mm_cmpeq_epi32(magnitude, empty);
        if constexpr (OutputBytes == 4) {
            const auto exponent = _mm_slli_epi32(_mm_sub_epi32(_mm_set1_epi32(127), normalized.leading), 23);
            const auto fraction = _mm_and_si128(_mm_srli_epi32(normalized.significand, 8), _mm_set1_epi32(0x7fffff));
            result.first = _mm_or_si128(sign, _mm_andnot_si128(is_zero, _mm_or_si128(exponent, fraction)));
        } else {
            const auto exponent = _mm_andnot_si128(is_zero, _mm_sub_epi32(_mm_set1_epi32(1023), normalized.leading));
            result = pack_float64<21>(sign, exponent, _mm_and_si128(normalized.significand, _mm_set1_epi32(INT32_MAX)));
        }
    }
    if constexpr (BigEndian) {
        if constexpr (OutputBytes == 4) {
            result.first = format_int32<32, true>(result.first);
        } else {
            const auto reverse = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
            result.first = _mm_shuffle_epi8(result.first, reverse);
            result.second = _mm_shuffle_epi8(result.second, reverse);
        }
    }
    return result;
}

template<bool FromFloat, std::size_t OutputBytes>
std::uint64_t encode_float_sample(std::uint32_t sample) noexcept
{
    const auto sign = std::uint64_t{sample & 0x80000000u} << (OutputBytes == 8 ? 32 : 0);
    if constexpr (FromFloat) {
        const auto exponent = (sample >> 23) & 255u;
        const auto fraction = sample & 0x7fffffu;
        if (exponent != 0) {
            return sign | (std::uint64_t{exponent == 255 ? 2047 : exponent + 896} << 52) | (std::uint64_t{fraction} << 29);
        }
        if (fraction == 0) { return sign; }
        const auto leading = static_cast<unsigned>(std::countl_zero(fraction));
        return sign | (std::uint64_t{905u - leading} << 52) |
            (std::uint64_t{(fraction << leading) & 0x7fffffffu} << 21);
    } else {
        const auto magnitude = (sample & 0x80000000u) ? 0u - sample : sample;
        if (magnitude == 0) { return 0; }
        const auto leading = static_cast<unsigned>(std::countl_zero(magnitude));
        const auto fraction = (magnitude << leading) & 0x7fffffffu;
        if constexpr (OutputBytes == 4) {
            return sign | (std::uint64_t{127u - leading} << 23) | (fraction >> 8);
        } else {
            return sign | (std::uint64_t{1023u - leading} << 52) | (std::uint64_t{fraction} << 21);
        }
    }
}

// A packed group is exactly twelve readable bytes, even at a protected page
// boundary. Combining an 8-byte and a 4-byte load avoids speculative read-ahead
// beyond four mono samples or two stereo frames.
__m128i load_twelve(const std::byte* source) noexcept
{
    return _mm_unpacklo_epi64(_mm_cvtsi64_si128(load_le<std::int64_t>(source)),
        _mm_cvtsi32_si128(load_le<std::int32_t>(source + 8)));
}

template<bool Mono, std::size_t SourceBytes, bool TwoFrames>
Planes load_float_inputs(const std::byte* source) noexcept
{
    if constexpr (SourceBytes == 2) {
        return load_pcm16_words<Mono, false>(source);
    } else if constexpr (SourceBytes == 3) {
        if constexpr (Mono) {
            const auto samples = _mm_shuffle_epi8(load_twelve(source), _mm_setr_epi8(pshufb_zero, 0, 1, 2,
                pshufb_zero, 3, 4, 5, pshufb_zero, 6, 7, 8, pshufb_zero, 9, 10, 11));
            return {samples, samples};
        } else {
            const auto split = _mm_setr_epi8(pshufb_zero, 0, 1, 2, pshufb_zero, 6, 7, 8,
                pshufb_zero, 3, 4, 5, pshufb_zero, 9, 10, 11);
            const auto first = _mm_shuffle_epi8(load_twelve(source), split);
            const auto second = _mm_shuffle_epi8(load_twelve(source + 12), split);
            return {_mm_unpacklo_epi64(first, second), _mm_unpackhi_epi64(first, second)};
        }
    } else {
        static_assert(SourceBytes == 4);
        if constexpr (TwoFrames && Mono) {
            const auto samples = _mm_cvtsi64_si128(load_le<std::int64_t>(source));
            return {samples, samples};
        } else if constexpr (TwoFrames) {
            const auto separated = _mm_shuffle_epi32(load_le<__m128i>(source), _MM_SHUFFLE(3, 1, 2, 0));
            return {_mm_unpacklo_epi64(separated, _mm_setzero_si128()), _mm_srli_si128(separated, 8)};
        } else if constexpr (Mono) {
            const auto samples = load_le<__m128i>(source);
            return {samples, samples};
        } else {
            const auto first = _mm_shuffle_epi32(load_le<__m128i>(source), _MM_SHUFFLE(3, 1, 2, 0));
            const auto second = _mm_shuffle_epi32(load_le<__m128i>(source + 16), _MM_SHUFFLE(3, 1, 2, 0));
            return {_mm_unpacklo_epi64(first, second), _mm_unpackhi_epi64(first, second)};
        }
    }
}

template<bool Mono, std::size_t SourceBytes, bool FromFloat, std::size_t OutputBytes, bool BigEndian>
void to_float(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept
{
    constexpr std::size_t frame_bytes = SourceBytes * (Mono ? 1 : 2);
    // MSVC can merge adjacent stores of widened Float32 samples into a YMM store.
    // Widen two samples per channel at a time to keep the store 128 bits wide.
    constexpr std::size_t batch_frames = FromFloat ? 2 : 4;
    const auto store_words = [](std::byte* destination, const FloatWords& words) {
        store_le(destination, words.first);
        if constexpr (OutputBytes == 8 && !FromFloat) { store_le(destination + 16, words.second); }
    };
    std::size_t i = 0;
    for (const auto simd_end = frames & ~(batch_frames - 1); i < simd_end; i += batch_frames) {
        const auto samples = load_float_inputs<Mono, SourceBytes, FromFloat>(source + i * frame_bytes);
        const auto left_words = encode_float_words<FromFloat, SourceBytes, OutputBytes, BigEndian>(samples.left);
        store_words(left + i * OutputBytes, left_words);
        if constexpr (Mono) {
            store_words(right + i * OutputBytes, left_words);
        } else {
            store_words(right + i * OutputBytes, encode_float_words<FromFloat, SourceBytes, OutputBytes, BigEndian>(samples.right));
        }
    }
    const auto convert_sample = [](std::byte* destination, const std::byte* input) {
        std::uint32_t sample;
        if constexpr (SourceBytes == 2) {
            sample = std::uint32_t{load_le<std::uint16_t>(input)} << 16;
        } else if constexpr (SourceBytes == 3) {
            sample = (std::uint32_t{load_le<std::uint16_t>(input)} << 8) | (std::uint32_t{static_cast<std::uint8_t>(input[2])} << 24);
        } else {
            sample = load_le<std::uint32_t>(input);
        }
        const auto bits = encode_float_sample<FromFloat, OutputBytes>(sample);
        if constexpr (OutputBytes == 4) {
            store_sample<4, BigEndian>(destination, static_cast<std::uint32_t>(bits));
        } else {
            store_le(destination, BigEndian ? std::byteswap(bits) : bits);
        }
    };
    for (; i < frames; ++i) {
        const auto* frame = source + i * frame_bytes;
        convert_sample(left + i * OutputBytes, frame);
        convert_sample(right + i * OutputBytes, frame + (Mono ? 0 : SourceBytes));
    }
}

template<bool Mono, std::size_t OutputBytes, bool BigEndian>
PlanarConvertFn float_converter(const SampleFormat& source) noexcept
{
    if (source == float32_le) {
        if constexpr (OutputBytes == 4) {
            // Identical-width IEEE data is copied/shuffled as opaque bits.
            return pcm32_to_int32<Mono, 32, BigEndian>;
        } else {
            return to_float<Mono, 4, true, 8, BigEndian>;
        }
    }
    if (source == int16_le) { return to_float<Mono, 2, false, OutputBytes, BigEndian>; }
    if (source == int24_le) { return to_float<Mono, 3, false, OutputBytes, BigEndian>; }
    if (source == int24_in32_le || source == int32_le) { return to_float<Mono, 4, false, OutputBytes, BigEndian>; }
    return nullptr;
}

template<bool Mono, bool BigEndian>
PlanarConvertFn float_converter(const SampleFormat& source, const SampleFormat& output) noexcept
{
    if (output.bytes() == 4) { return float_converter<Mono, 4, BigEndian>(source); }
    if (output.bytes() == 8) { return float_converter<Mono, 8, BigEndian>(source); }
    return nullptr;
}

template<bool Mono, bool BigEndian>
PlanarConvertFn integer_converter(const SampleFormat& source, const SampleFormat& output) noexcept
{
    if (output.bytes() == 2) {
        if (source == int16_le) { return Mono ? pcm16_mono_to_int16<BigEndian> : pcm16_stereo_to_int16<BigEndian>; }
        return nullptr;
    }
    if (output.bytes() == 3) {
        if (source == int16_le) { return pcm16_to_int24<Mono, BigEndian>; }
        if (source == int24_le) { return pcm24_to_int24<Mono, BigEndian>; }
        if (source == int24_in32_le) { return pcm24_in32_to_int24<Mono, BigEndian>; }
        return nullptr;
    }
    switch (output.valid_bits) {
    case 16: return int32_converter<Mono, 16, BigEndian>(source);
    case 18: return int32_converter<Mono, 18, BigEndian>(source);
    case 20: return int32_converter<Mono, 20, BigEndian>(source);
    case 24: return int32_converter<Mono, 24, BigEndian>(source);
    case 32: return int32_converter<Mono, 32, BigEndian>(source);
    default: return nullptr;
    }
}

} // namespace

std::optional<PlanarConverter> find_planar_converter(const StreamFormat& source, const SampleFormat& output) noexcept
{
    const bool integer_source = source.sample == int16_le || source.sample == int24_le ||
        source.sample == int24_in32_le || source.sample == int32_le;
    if ((!integer_source && source.sample != float32_le) || (source.channels != 1 && source.channels != 2) ||
        (output.byte_order != std::endian::little && output.byte_order != std::endian::big)) {
        return std::nullopt;
    }
    const bool mono = source.channels == 1;
    if (output.kind == SampleKind::floating_point) {
        if (output.alignment != BitAlignment::msb ||
            !((output.bytes() == 4 && output.valid_bits == 32) || (output.bytes() == 8 && output.valid_bits == 64)) ||
            (output.bytes() == 4 && source.sample == int32_le)) {
            return std::nullopt;
        }
        const auto convert = output.byte_order == std::endian::big
            ? (mono ? float_converter<true, true>(source.sample, output) : float_converter<false, true>(source.sample, output))
            : (mono ? float_converter<true, false>(source.sample, output) : float_converter<false, false>(source.sample, output));
        if (!convert) { return std::nullopt; }
        return PlanarConverter{convert, source, output};
    }
    const bool full_width = output.alignment == BitAlignment::msb &&
        ((output.bytes() == 2 && output.valid_bits == 16) || (output.bytes() == 3 && output.valid_bits == 24) ||
            (output.bytes() == 4 && output.valid_bits == 32));
    const bool low_bits = output.bytes() == 4 && output.alignment == BitAlignment::lsb &&
        (output.valid_bits == 16 || output.valid_bits == 18 || output.valid_bits == 20 || output.valid_bits == 24);
    if (!integer_source || output.kind != SampleKind::integer ||
        (!full_width && !low_bits) || output.valid_bits < source.sample.valid_bits) {
        return std::nullopt;
    }
    const auto convert = output.byte_order == std::endian::big
        ? (mono ? integer_converter<true, true>(source.sample, output) : integer_converter<false, true>(source.sample, output))
        : (mono ? integer_converter<true, false>(source.sample, output) : integer_converter<false, false>(source.sample, output));
    if (!convert) { return std::nullopt; }
    return PlanarConverter{convert, source, output};
}

void convert_into(const PlanarConverter& converter, const std::byte* source, std::size_t frames,
    PlanarBlocks& target) noexcept
{
    if (frames == 0) {
        return;
    }
    // The kernel stores bytes, which may alias `converter` and `target` as far as
    // the compiler knows, so the loop works only on local copies of their fields.
    const auto kernel = converter.kernel();
    const auto output_bytes = converter.output_sample_bytes();
    const auto source_frame_bytes = converter.source_frame_bytes();
    const auto block_frames = target.block_frames;
    const auto stride = target.stride;
    const auto first = target.first_frame;
    auto* left = target.left;
    auto* right = target.right;
    target = target.advanced(frames);
    if (stride == block_frames * output_bytes) {
        // Adjacent slots are one dense plane: preserve SIMD groups across the
        // logical block boundaries and invoke the kernel once for this read.
        const auto offset = first * output_bytes;
        kernel(source, frames, left + offset, right + offset);
        return;
    }
    // Only the first block needs division. Every following block starts at the
    // next stride, even when this conversion began in the middle of a block.
    // No pointer past the last written block is formed. Advancing the source and
    // the count before the call leaves no more values live across it than the
    // nonvolatile registers can hold.
    const auto block_start = first / block_frames * stride;
    auto in_block = first % block_frames;
    left += block_start;
    right += block_start;
    for (auto count = std::min(frames, block_frames - in_block);; count = std::min(frames, block_frames)) {
        const auto* const next = source + count * source_frame_bytes;
        frames -= count;
        kernel(source, count, left + in_block * output_bytes, right + in_block * output_bytes);
        if (frames == 0) {
            return;
        }
        source = next;
        left += stride;
        right += stride;
        in_block = 0;
    }
}

} // namespace wavplay::audio
