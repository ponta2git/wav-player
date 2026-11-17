#pragma once

#include "wavplay/audio/sample_format.h"

#include <cstddef>
#include <optional>

namespace wavplay::audio {

// Deinterleaves `frames` source frames into two planar channel buffers, converting
// every sample to the output format. Mono input is written to both channels.
// Requires the AVX2/XMM/YMM startup check to have succeeded; no ISA fallback.
// The source and both destinations may be unaligned but must not overlap.
// Each destination holds frames * output sample bytes.
// Unused source bits must already be validated as zero by the input layer.
using PlanarConvertFn = void (*)(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) noexcept;

// A bit-transparent conversion from one interleaved stream format to a planar
// stereo output format. Kernels are pure functions on memory, so they can be
// tested, vectorized or replaced without touching file I/O or the ring.
// Only find_planar_converter creates one, so the kernel, both formats and the
// sizes derived from them always describe the same supported pair.
class PlanarConverter {
public:
    [[nodiscard]] PlanarConvertFn kernel() const noexcept { return kernel_; }
    [[nodiscard]] const StreamFormat& source() const noexcept { return source_; } // The interleaved format read.
    [[nodiscard]] const SampleFormat& output() const noexcept { return output_; } // The planar format written.
    [[nodiscard]] std::size_t source_frame_bytes() const noexcept { return source_.frame_bytes(); }
    [[nodiscard]] std::size_t output_sample_bytes() const noexcept { return output_.bytes(); }

    void operator()(const std::byte* source, std::size_t frames, std::byte* left, std::byte* right) const noexcept
    {
        kernel_(source, frames, left, right);
    }

private:
    friend std::optional<PlanarConverter> find_planar_converter(
        const StreamFormat& source, const SampleFormat& output) noexcept;

    PlanarConverter(PlanarConvertFn kernel, const StreamFormat& source, const SampleFormat& output) noexcept
        : kernel_(kernel), source_(source), output_(output)
    {
    }

    PlanarConvertFn kernel_;
    StreamFormat source_;
    SampleFormat output_;
};

// Returns the lossless converter for this pair, or nullopt when the pair is not
// supported. Inputs are little-endian PCM16, packed PCM24, MSB-aligned PCM24/32
// and PCM32, or IEEE Float32, mono or stereo. Integer outputs are 16/24/32-bit or signed low
// 16/18/20/24 bits in a 32-bit container, in either byte order. Expansion appends
// zero low bits to the input value; low-aligned containers sign-extend above
// their valid bits. Outputs with fewer valid bits are not accepted. Float32/64
// outputs accept PCM up to 24/32 valid bits respectively, normalized exactly by
// 2^(valid_bits - 1). Float32 input accepts only Float32/64 output. All floating
// encodings use integer operations: signed zero, subnormals, infinities and NaN
// sign/quiet bit/payload are preserved, with Float32's fraction shifted left 29
// when widened. No floating-point environment state is read or modified.
[[nodiscard]] std::optional<PlanarConverter> find_planar_converter(
    const StreamFormat& source, const SampleFormat& output) noexcept;

// Planar destination made of fixed-size blocks whose starts are `stride` bytes
// apart in each channel, such as cache-line-aligned ring slots, so samples are
// converted straight into place. A dense pair of buffers is a single block.
// Frames are counted from the first block; the bytes between blocks are untouched.
// The requested channel ranges must not overlap. Their hulls may interleave
// when full block payloads fit disjointly between the other channel's blocks,
// including neighbouring slots. Disjoint partial hulls need no extra spacing.
// WavReader validates these ranges before entering the converter.
struct PlanarBlocks {
    std::byte* left{};
    std::byte* right{};
    std::size_t block_frames{};
    std::size_t stride{};      // At least block_frames * the output sample size.
    std::size_t first_frame{}; // The next frame to write.
    std::size_t frames{};      // Frames left to write from first_frame.

    [[nodiscard]] PlanarBlocks advanced(std::size_t count) const noexcept
    {
        auto rest = *this;
        rest.first_frame += count;
        rest.frames -= count;
        return rest;
    }
};

// Converts `frames` (at most target.frames) source frames into the next frames of
// `target`, splitting the conversion at block boundaries, and advances `target`.
void convert_into(const PlanarConverter& converter, const std::byte* source, std::size_t frames,
    PlanarBlocks& target) noexcept;

} // namespace wavplay::audio
