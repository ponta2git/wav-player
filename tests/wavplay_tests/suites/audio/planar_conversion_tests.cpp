#include "suites/suites.h"
#include "framework/test_suite.h"
#include "wavplay/audio/planar_conversion.h"

#include <windows.h>
#include <isa_availability.h>
#include <immintrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace wavplay_test;
using wavplay::audio::BitAlignment;
using wavplay::audio::find_planar_converter;
using wavplay::audio::SampleFormat;
using wavplay::audio::SampleKind;
using wavplay::audio::StreamFormat;

constexpr std::uint8_t guard = 0xa5;
constexpr std::size_t guard_bytes = 3; // Default padding also exercises unaligned destinations.

constexpr std::array source_formats{
    wavplay::audio::int16_le, wavplay::audio::int24_le, wavplay::audio::int24_in32_le, wavplay::audio::int32_le,
    wavplay::audio::float32_le};

// Declared independently from ASIO mapping and product dispatch.
constexpr std::array<SampleFormat, 14> integer_outputs{{
    {SampleKind::integer, 2, 16, std::endian::little},
    {SampleKind::integer, 3, 24, std::endian::little},
    {SampleKind::integer, 4, 32, std::endian::little},
    {SampleKind::integer, 4, 16, std::endian::little, BitAlignment::lsb},
    {SampleKind::integer, 4, 18, std::endian::little, BitAlignment::lsb},
    {SampleKind::integer, 4, 20, std::endian::little, BitAlignment::lsb},
    {SampleKind::integer, 4, 24, std::endian::little, BitAlignment::lsb},
    {SampleKind::integer, 2, 16, std::endian::big},
    {SampleKind::integer, 3, 24, std::endian::big},
    {SampleKind::integer, 4, 32, std::endian::big},
    {SampleKind::integer, 4, 16, std::endian::big, BitAlignment::lsb},
    {SampleKind::integer, 4, 18, std::endian::big, BitAlignment::lsb},
    {SampleKind::integer, 4, 20, std::endian::big, BitAlignment::lsb},
    {SampleKind::integer, 4, 24, std::endian::big, BitAlignment::lsb},
}};

constexpr std::array<SampleFormat, 4> float_outputs{{
    {SampleKind::floating_point, 4, 32, std::endian::little},
    {SampleKind::floating_point, 8, 64, std::endian::little},
    {SampleKind::floating_point, 4, 32, std::endian::big},
    {SampleKind::floating_point, 8, 64, std::endian::big}}};

constexpr std::array output_groups{std::span<const SampleFormat>{integer_outputs}, std::span<const SampleFormat>{float_outputs}};

// This acceptance table is independent of the converter's descriptor dispatch.
bool accepts(const SampleFormat& source, const SampleFormat& output)
{
    if (output.kind == SampleKind::integer) {
        return source.kind == SampleKind::integer && output.valid_bits >= source.valid_bits;
    }
    return source.kind == SampleKind::floating_point || output.bytes() == 8 || source.valid_bits <= 24;
}

// Distinct samples per frame and channel, covering all bytes and sign boundaries.
std::uint32_t left_bits(std::size_t frame, std::size_t valid_bytes)
{
    if (valid_bytes == 4) {
        constexpr std::array<std::uint32_t, 10> edges{
            0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x80000001, 0x12345678, 0xfedcba98, 0x7f800001, 0xff800001};
        if (frame >= edges.size() && frame < edges.size() + 32) { return std::uint32_t{1} << (frame - edges.size()); }
        if (frame >= edges.size() + 32 && frame < edges.size() + 64) { return ~(std::uint32_t{1} << (frame - edges.size() - 32)); }
        return frame < edges.size() ? edges[frame] : static_cast<std::uint32_t>(0x80000001u + 0x01234567u * frame);
    }
    if (valid_bytes == 3) {
        constexpr std::array<std::uint32_t, 10> edges{
            0, 1, 0x7fffff, 0x800000, 0xffffff, 0x800001, 0x123456, 0xfedcba, 0x7f8001, 0xff8001};
        if (frame >= edges.size() && frame < edges.size() + 24) { return std::uint32_t{1} << (frame - edges.size()); }
        if (frame >= edges.size() + 24 && frame < edges.size() + 48) { return 0xffffffu ^ (std::uint32_t{1} << (frame - edges.size() - 24)); }
        return frame < edges.size() ? edges[frame] : static_cast<std::uint32_t>((0x800001u + 0x012345u * frame) & 0xffffffu);
    }
    return static_cast<std::uint16_t>(0x8001 + 0x0123 * frame);
}

std::uint32_t right_bits(std::size_t frame, std::size_t valid_bytes)
{
    if (valid_bytes == 4) {
        return ~left_bits(frame, valid_bytes);
    }
    if (valid_bytes == 3) {
        return ~left_bits(frame, valid_bytes) & 0xffffffu;
    }
    return static_cast<std::uint16_t>(0x7ffe - 0x0311 * frame);
}

std::uint32_t source_bits(std::size_t frame, bool right, const SampleFormat& source)
{
    if (source.kind == SampleKind::integer) {
        return right ? right_bits(frame, source.valid_bits / 8U) : left_bits(frame, source.valid_bits / 8U);
    }
    constexpr std::array<std::uint32_t, 32> edges{
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
        0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x3f800001, 0xbf800001, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7f800001, 0xff800001, 0x7fc00000, 0xffc00000, 0x7fc12345, 0xffc12345,
        0x7fbfffff, 0xffbfffff, 0x00400000, 0x80400000, 0x00010001, 0x80010001, 0x3eaaaaab, 0xbeaaaaab};
    std::uint32_t bits;
    if (frame < edges.size()) {
        bits = edges[frame];
    } else if (frame < 64) {
        bits = std::uint32_t{1} << (frame - 32);
    } else if (frame < 96) {
        bits = ~(std::uint32_t{1} << (frame - 64));
    } else if (frame >= 608 && frame < 654) {
        bits = 0x7f800000u | (std::uint32_t{1} << ((frame - 608) / 2));
        if (frame & 1) { bits |= 0x80000000u; }
    } else {
        // Every exponent, both signs and both quiet/signaling payload forms.
        bits = (static_cast<std::uint32_t>((frame - 96) / 2) & 255u) << 23;
        bits |= (frame & 1) ? 0x80400123u : 0x00054321u;
    }
    return right ? bits ^ 0x80000000u : bits;
}

// Numeric integer oracle, separate from product normalization/shuffles: locate
// the greatest power of two by division, then place the exact remainder in the
// fraction. All accepted finite values fit without a rounding operation.
std::uint64_t exact_float_bits(std::uint64_t magnitude, bool negative, int scale, std::size_t output_bytes)
{
    const auto fraction_bits = output_bytes == 4 ? 23U : 52U;
    const auto sign = std::uint64_t{negative} << (output_bytes * 8 - 1);
    if (magnitude == 0) { return sign; }
    std::uint64_t power = 1;
    unsigned position = 0;
    while (power <= magnitude / 2) {
        power *= 2;
        ++position;
    }
    const auto exponent = static_cast<unsigned>(static_cast<int>(position) + scale + (output_bytes == 4 ? 127 : 1023));
    return sign | (std::uint64_t{exponent} << fraction_bits) | ((magnitude - power) << (fraction_bits - position));
}

std::uint64_t expected_float_bits(std::uint32_t bits, const SampleFormat& source, std::size_t output_bytes)
{
    if (source.kind == SampleKind::floating_point) {
        if (output_bytes == 4) { return bits; }
        const auto exponent = (bits >> 23) & 255u;
        const auto fraction = bits & 0x7fffffu;
        if (exponent == 255) {
            return (std::uint64_t{bits >> 31} << 63) | 0x7ff0000000000000ull | (std::uint64_t{fraction} << 29);
        }
        return exact_float_bits(exponent ? 0x800000u + fraction : fraction, (bits >> 31) != 0,
            exponent ? static_cast<int>(exponent) - 150 : -149, output_bytes);
    }
    const auto signed_value = static_cast<std::int64_t>(bits) -
        ((bits & (std::uint32_t{1} << (source.valid_bits - 1))) ? (std::int64_t{1} << source.valid_bits) : 0);
    return exact_float_bits(static_cast<std::uint64_t>(signed_value < 0 ? -signed_value : signed_value), signed_value < 0,
        1 - source.valid_bits, output_bytes);
}

// Independent numeric oracle: recover the signed input, scale it at the output
// precision in int64, and serialize the sign-extended result in the chosen order.
std::uint8_t expected_byte(std::uint32_t bits, std::size_t byte, const SampleFormat& output, const SampleFormat& source)
{
    const auto index = output.byte_order == std::endian::big ? output.bytes() - 1 - byte : byte;
    if (output.kind == SampleKind::floating_point) {
        return static_cast<std::uint8_t>(expected_float_bits(bits, source, output.bytes()) >> (8 * index));
    }
    const auto source_valid_bits = source.valid_bits;
    const auto signed_value = static_cast<std::int64_t>(bits) -
        ((bits & (std::uint32_t{1} << (source_valid_bits - 1))) ? (std::int64_t{1} << source_valid_bits) : 0);
    const auto scaled = signed_value * (std::int64_t{1} << (output.valid_bits - source_valid_bits));
    return static_cast<std::uint8_t>(static_cast<std::uint64_t>(scaled) >> (8 * index));
}

std::vector<std::byte> make_source(std::uint16_t channels, std::size_t frames, const SampleFormat& source_format,
    std::size_t prefix = 0)
{
    const auto source_bytes = source_format.bytes();
    const auto valid_bytes = source_format.valid_bits / 8U;
    const auto low_padding = source_bytes - valid_bytes;
    std::vector<std::byte> source(prefix + frames * source_bytes * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto bits = source_bits(frame, channel != 0, source_format);
            // The value and its storage width are independent: 24/32 has a
            // zero low byte followed by the same three bytes as packed PCM24.
            for (std::size_t byte = low_padding; byte < source_bytes; ++byte) {
                source[prefix + (frame * channels + channel) * source_bytes + byte] =
                    static_cast<std::byte>(bits >> (8 * (byte - low_padding)));
            }
        }
    }
    return source;
}

void check_kernel(std::uint16_t channels, const SampleFormat& source_format, const SampleFormat& output, std::size_t frames,
    std::size_t source_alignment = 1, std::size_t left_alignment = 3, std::size_t right_alignment = 3)
{
    const auto source_bytes = source_format.bytes();
    const auto converter = find_planar_converter({48000, channels, source_format}, output);
    if (!converter || !converter->kernel()) {
        throw std::runtime_error("expected a lossless sample converter");
    }
    expect("source frame bytes", converter->source_frame_bytes(), source_bytes * channels);
    expect("output sample bytes", converter->output_sample_bytes(), output.bytes());
    expect("source descriptor retained", static_cast<int>(converter->source() == StreamFormat{48000, channels, source_format}), 1);
    expect("output descriptor retained", static_cast<int>(converter->output() == output), 1);

    // Choose actual address residues, not just offsets from a heap allocation
    // whose own alignment can change between sizes and test configurations.
    const auto prefix = [](const void* data, std::size_t alignment) {
        return guard_bytes + ((alignment + 32 - ((reinterpret_cast<std::uintptr_t>(data) + guard_bytes) & 31)) & 31);
    };
    constexpr auto slack = 2 * guard_bytes + 31;
    const auto samples = make_source(channels, frames, source_format);
    std::vector<std::byte> source(samples.size() + slack, std::byte{guard});
    const auto source_prefix = prefix(source.data(), source_alignment);
    std::ranges::copy(samples, source.data() + source_prefix);
    const auto sample_bytes = output.bytes();
    std::vector<std::uint8_t> left(slack + frames * sample_bytes, guard), right(left.size(), guard);
    const auto left_prefix = prefix(left.data(), left_alignment), right_prefix = prefix(right.data(), right_alignment);
    (*converter)(source.data() + source_prefix, frames, reinterpret_cast<std::byte*>(left.data() + left_prefix),
        reinterpret_cast<std::byte*>(right.data() + right_prefix));

    const auto check = [&](const char* channel, const std::vector<std::uint8_t>& actual, std::size_t start, bool is_right) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            std::uint8_t wanted = guard;
            if (i >= start && i < start + frames * sample_bytes) {
                const auto offset = i - start;
                wanted = expected_byte(source_bits(offset / sample_bytes, is_right, source_format), offset % sample_bytes, output, source_format);
            }
            if (actual[i] != wanted) {
                expect(std::string(channel) + " byte " + std::to_string(i) + " of " + std::to_string(frames) +
                    " frames", actual[i], wanted);
            }
        }
    };
    check("left", left, left_prefix, false);
    check("right", right, right_prefix, channels == 2);
}

void test_byte_alignments()
{
    for (const auto source : source_formats) {
        for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
            for (const auto outputs : output_groups) {
                for (const auto output : outputs) {
                    if (!accepts(source, output)) { continue; }
                    for (const std::size_t frames : {1U, 3U, 4U, 7U, 8U, 9U, 17U}) {
                        check_kernel(channels, source, output, frames, 0, 0, 0);
                        for (std::size_t offset = 0; offset < 32; ++offset) {
                            // Each address visits every residue independently;
                            // the output pointers need not share an alignment.
                            check_kernel(channels, source, output, frames,
                                offset, (7 * offset + 3) & 31, (13 * offset + 11) & 31);
                        }
                    }
                }
            }
        }
    }
}

void test_pcm16_kernels()
{
    // Frame counts cover each SIMD width (4 and 8 frames), the 24-bit kernel's two-frame
    // store slack (6, 10, ...), scalar tails and both together.
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto& output : integer_outputs) {
            for (std::size_t frames = 0; frames <= 65; ++frames) {
                check_kernel(channels, wavplay::audio::int16_le, output, frames);
            }
        }
    }
}

void test_wide_pcm_kernels(const SampleFormat& source_format, std::span<const SampleFormat> outputs = integer_outputs)
{
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto& output : outputs) {
            if (!accepts(source_format, output)) {
                continue;
            }
            for (std::size_t frames = 0; frames <= 65; ++frames) {
                check_kernel(channels, source_format, output, frames);
            }
            check_kernel(channels, source_format, output, 74); // Every valid bit, set alone and cleared alone.
            if (source_format.kind == SampleKind::floating_point) {
                check_kernel(channels, source_format, output, 654); // All exponents and every NaN payload bit, either sign.
            }
        }
    }
}

// A readable page between two inaccessible pages exposes SIMD loads/stores that
// run before or after an exact-size span, even in builds without AddressSanitizer.
class GuardedPage {
public:
    GuardedPage()
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        size_ = info.dwPageSize;
        allocation_ = static_cast<std::byte*>(VirtualAlloc(nullptr, 3 * size_, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        if (!allocation_) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "allocate guarded page");
        }
        DWORD previous{};
        if (!VirtualProtect(data(), size_, PAGE_READWRITE, &previous)) {
            const auto error = GetLastError();
            VirtualFree(allocation_, 0, MEM_RELEASE);
            throw std::system_error(static_cast<int>(error), std::system_category(), "open guarded page");
        }
    }

    ~GuardedPage() { VirtualFree(allocation_, 0, MEM_RELEASE); }
    GuardedPage(const GuardedPage&) = delete;
    GuardedPage& operator=(const GuardedPage&) = delete;

    std::byte* data() const noexcept { return allocation_ + size_; }
    std::size_t size() const noexcept { return size_; }

    void protect(DWORD access)
    {
        DWORD previous{};
        if (!VirtualProtect(data(), size_, access, &previous)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "protect guarded page");
        }
    }

private:
    std::byte* allocation_{};
    std::size_t size_{};
};

void test_guarded_spans(const SampleFormat& source_format, std::span<const SampleFormat> outputs = integer_outputs)
{
    GuardedPage input, left, right;
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto output : outputs) {
            if (!accepts(source_format, output)) {
                continue;
            }
            const auto converter = find_planar_converter({48000, channels, source_format}, output).value();
            for (const bool at_end : {false, true}) {
                for (std::size_t frames = 0; frames <= 65; ++frames) {
                    const auto source = make_source(channels, frames, source_format);
                    const auto output_bytes = frames * output.bytes();
                    const auto input_offset = at_end ? input.size() - source.size() : 0;
                    const auto output_offset = at_end ? left.size() - output_bytes : 0;
                    input.protect(PAGE_READWRITE);
                    if (!source.empty()) {
                        std::memcpy(input.data() + input_offset, source.data(), source.size());
                    }
                    input.protect(PAGE_READONLY);
                    std::memset(left.data(), guard, left.size());
                    std::memset(right.data(), guard, right.size());
                    converter(input.data() + input_offset, frames,
                        left.data() + output_offset, right.data() + output_offset);
                    for (std::size_t byte = 0; byte < left.size(); ++byte) {
                        std::uint8_t expected_left = guard, expected_right = guard;
                        if (byte >= output_offset && byte - output_offset < output_bytes) {
                            const auto offset = byte - output_offset;
                            const auto frame = offset / output.bytes();
                            const auto in_sample = offset % output.bytes();
                            expected_left = expected_byte(source_bits(frame, false, source_format), in_sample, output, source_format);
                            expected_right = expected_byte(source_bits(frame, channels == 2, source_format), in_sample, output, source_format);
                        }
                        if (static_cast<std::uint8_t>(left.data()[byte]) != expected_left) {
                            expect("guarded PCM left byte", static_cast<std::uint8_t>(left.data()[byte]), expected_left);
                        }
                        if (static_cast<std::uint8_t>(right.data()[byte]) != expected_right) {
                            expect("guarded PCM right byte", static_cast<std::uint8_t>(right.data()[byte]), expected_right);
                        }
                    }
                }
            }
        }
    }
}

void test_zero_frames()
{
    const auto converter = find_planar_converter({44100, 2, wavplay::audio::int16_le}, wavplay::audio::int24_le);
    const std::byte source[4]{};
    std::vector<std::uint8_t> left(4, guard), right(4, guard);
    converter.value()(source, 0, reinterpret_cast<std::byte*>(left.data()),
        reinterpret_cast<std::byte*>(right.data()));
    for (const auto byte : left) {
        expect("left untouched without frames", byte, guard);
    }
    for (const auto byte : right) {
        expect("right untouched without frames", byte, guard);
    }
    wavplay::audio::PlanarBlocks empty;
    wavplay::audio::convert_into(converter.value(), nullptr, 0, empty);
    expect("empty blocked conversion keeps its position", empty.first_frame, 0);
}

void check_blocked_destination(std::uint16_t channels, const SampleFormat& source_format, const SampleFormat& output, std::size_t block_frames,
    std::size_t first, std::size_t frames, std::size_t gap)
{
    const auto sample_bytes = output.bytes();
    const auto stride = block_frames * sample_bytes + gap;
    const auto blocks = 1 + (first + frames - 1) / block_frames;
    const auto source_bytes = source_format.bytes();
    const auto converter = find_planar_converter({48000, channels, source_format}, output).value();
    const auto source = make_source(channels, frames, source_format);
    std::vector<std::uint8_t> left(blocks * stride + guard_bytes, guard), right(left.size(), guard);
    wavplay::audio::PlanarBlocks target{.left = reinterpret_cast<std::byte*>(left.data()),
        .right = reinterpret_cast<std::byte*>(right.data()), .block_frames = block_frames, .stride = stride,
        .first_frame = first, .frames = frames};
    const auto split = frames / 2;
    wavplay::audio::convert_into(converter, source.data(), split, target);
    expect("advanced to the next frame", target.first_frame, first + split);
    expect("frames left after the first call", target.frames, frames - split);
    wavplay::audio::convert_into(converter, source.data() + split * source_bytes * channels, frames - split, target);
    expect("target exhausted", target.frames, 0);
    expect("target ends after the last frame", target.first_frame, first + frames);

    const auto check = [&](const char* channel, const std::vector<std::uint8_t>& actual, bool is_right) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto block = i / stride, in_block = i % stride;
            const auto slot = block * block_frames + in_block / sample_bytes;
            std::uint8_t wanted = guard;
            if (in_block < sample_bytes * block_frames && slot >= first && slot < first + frames) {
                wanted = expected_byte(source_bits(slot - first, is_right, source_format), in_block % sample_bytes, output, source_format);
            }
            if (actual[i] != wanted) {
                expect(std::string(channel) + " blocked byte " + std::to_string(i), actual[i], wanted);
            }
        }
    };
    check("left", left, false);
    check("right", right, channels == 2);
}

void test_blocked_destination(const SampleFormat& source_format, std::span<const SampleFormat> outputs = integer_outputs)
{
    // Dense and padded slots, scalar/SIMD boundaries and two calls that split a
    // logical block. Independent expected bytes also protect every gap and tail.
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto output : outputs) {
            if (!accepts(source_format, output)) {
                continue;
            }
            for (const std::size_t width : {1, 4, 5, 8}) {
                for (std::size_t first = 0; first < width; ++first) {
                    for (const std::size_t count : {std::size_t{1}, width, 2 * width + 1}) {
                        for (const std::size_t gap : {0, 2}) {
                            check_blocked_destination(channels, source_format, output, width, first, count, gap);
                        }
                    }
                }
            }
        }
    }
}

void test_conversion_matrix()
{
    for (const auto source : source_formats) {
        for (const std::uint16_t channels : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{3}}) {
            for (const auto outputs : output_groups) {
                for (const auto output : outputs) {
                    const bool expected = (channels == 1 || channels == 2) && accepts(source, output);
                    expect("source/channel/output precision matrix",
                        static_cast<int>(find_planar_converter({48000, channels, source}, output).has_value()), static_cast<int>(expected));
                }
            }
        }
    }
}

void test_all_pcm16_patterns(std::span<const SampleFormat> outputs = integer_outputs)
{
    // Both odd-step ramps permute all 65,536 words, including every signed edge.
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto output : outputs) {
            check_kernel(channels, wavplay::audio::int16_le, output, 65536);
        }
    }
}

void test_unsupported_pairs()
{
    const SampleFormat unsupported_outputs[]{
        SampleFormat{SampleKind::floating_point, 4, 24}, {SampleKind::floating_point, 8, 32},
        {SampleKind::floating_point, 4, 32, std::endian::little, BitAlignment::lsb},
        {SampleKind::floating_point, 8, 64, std::endian::big, BitAlignment::lsb},
        {SampleKind::floating_point, 8, 64, static_cast<std::endian>(0x1234)},
        {static_cast<SampleKind>(0xff), 4, 32},
        {SampleKind::integer, 0, 0}, {SampleKind::integer, 1, 8}, {SampleKind::integer, 5, 40},
        {SampleKind::integer, 4, 16}, {SampleKind::integer, 4, 18},
        {SampleKind::integer, 4, 20}, {SampleKind::integer, 4, 24},
        {SampleKind::integer, 2, 16, std::endian::little, BitAlignment::lsb},
        {SampleKind::integer, 3, 24, std::endian::big, BitAlignment::lsb},
        {SampleKind::integer, 4, 32, std::endian::little, BitAlignment::lsb},
        {SampleKind::integer, 4, 17, std::endian::little, BitAlignment::lsb},
        {SampleKind::integer, 4, 22, std::endian::little, BitAlignment::lsb}, // Between two supported low-bit widths.
        {SampleKind::integer, 4, 25, std::endian::big, BitAlignment::lsb},
        {SampleKind::integer, 4, 0, std::endian::little, BitAlignment::lsb},
        {SampleKind::integer, 4, 32, static_cast<std::endian>(0x1234)}};
    for (const auto source : source_formats) {
        for (const auto output : unsupported_outputs) {
            expect("unsupported or noncanonical output descriptor",
                static_cast<int>(find_planar_converter({48000, 2, source}, output).has_value()), 0);
        }
    }
    const SampleFormat unsupported_sources[]{
        SampleFormat{SampleKind::floating_point, 8, 64}, {SampleKind::floating_point, 4, 32, std::endian::big},
        {SampleKind::floating_point, 4, 32, std::endian::little, BitAlignment::lsb},
        {SampleKind::floating_point, 4, 24}, {SampleKind::integer, 2, 16, std::endian::big},
        {SampleKind::integer, 3, 24, std::endian::big}, {SampleKind::integer, 4, 32, std::endian::big},
        {SampleKind::integer, 4, 24, std::endian::big},
        {SampleKind::integer, 4, 24, std::endian::little, BitAlignment::lsb},
        {SampleKind::integer, 4, 16}, {SampleKind::integer, 4, 20}};
    for (const auto source : unsupported_sources) {
        for (const auto outputs : output_groups) {
            for (const auto output : outputs) {
                expect("unsupported source descriptor",
                    static_cast<int>(find_planar_converter({48000, 2, source}, output).has_value()), 0);
            }
        }
    }
}

void test_pcm24_in32_reuses_int32_kernel()
{
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        const StreamFormat source{48000, channels, wavplay::audio::int24_in32_le};
        const auto converter = find_planar_converter(source, wavplay::audio::int32_le).value();
        const auto pcm32 = find_planar_converter({48000, channels, wavplay::audio::int32_le}, wavplay::audio::int32_le).value();
        expect("PCM24/32 keeps its source descriptor", static_cast<int>(converter.source() == source), 1);
        expect("PCM24/32 reuses the lossless PCM32 kernel", static_cast<int>(converter.kernel() == pcm32.kernel()), 1);
    }
}

void test_float_literals()
{
    struct Literal {
        SampleFormat source;
        std::uint32_t input;
        std::uint32_t float32;
        std::uint64_t float64;
    };
    using namespace wavplay::audio;
    // IEEE encodings fixed independently of the general mathematical oracle.
    constexpr std::array literals{
        Literal{int16_le, 0, 0, 0},
        Literal{int16_le, 1, 0x38000000, 0x3f00000000000000ull},
        Literal{int16_le, 0x7fff, 0x3f7ffe00, 0x3fefffc000000000ull},
        Literal{int16_le, 0x8000, 0xbf800000, 0xbff0000000000000ull},
        Literal{int16_le, 0xffff, 0xb8000000, 0xbf00000000000000ull},
        Literal{int24_le, 1, 0x34000000, 0x3e80000000000000ull},
        Literal{int24_le, 0x7fffff, 0x3f7ffffe, 0x3fefffffc0000000ull},
        Literal{int24_le, 0x800000, 0xbf800000, 0xbff0000000000000ull},
        Literal{int24_le, 0xffffff, 0xb4000000, 0xbe80000000000000ull},
        Literal{int24_in32_le, 1, 0x34000000, 0x3e80000000000000ull},
        Literal{int24_in32_le, 0x7fffff, 0x3f7ffffe, 0x3fefffffc0000000ull},
        Literal{int24_in32_le, 0x800000, 0xbf800000, 0xbff0000000000000ull},
        Literal{int24_in32_le, 0xffffff, 0xb4000000, 0xbe80000000000000ull},
        Literal{int32_le, 1, 0, 0x3e00000000000000ull},
        Literal{int32_le, 0x7fffffff, 0, 0x3fefffffffc00000ull},
        Literal{int32_le, 0x80000000, 0, 0xbff0000000000000ull},
        Literal{int32_le, 0xffffffff, 0, 0xbe00000000000000ull},
        Literal{float32_le, 0, 0, 0},
        Literal{float32_le, 0x80000000, 0x80000000, 0x8000000000000000ull},
        Literal{float32_le, 1, 1, 0x36a0000000000000ull},
        Literal{float32_le, 0x80000001, 0x80000001, 0xb6a0000000000000ull},
        Literal{float32_le, 0x007fffff, 0x007fffff, 0x380fffffc0000000ull},
        Literal{float32_le, 0x00800000, 0x00800000, 0x3810000000000000ull},
        Literal{float32_le, 0x3f800001, 0x3f800001, 0x3ff0000020000000ull},
        Literal{float32_le, 0x7f7fffff, 0x7f7fffff, 0x47efffffe0000000ull},
        Literal{float32_le, 0x7f800000, 0x7f800000, 0x7ff0000000000000ull},
        Literal{float32_le, 0xff800000, 0xff800000, 0xfff0000000000000ull},
        Literal{float32_le, 0x7f800001, 0x7f800001, 0x7ff0000020000000ull},
        Literal{float32_le, 0xff800001, 0xff800001, 0xfff0000020000000ull},
        Literal{float32_le, 0x7fc12345, 0x7fc12345, 0x7ff82468a0000000ull},
        Literal{float32_le, 0xffc12345, 0xffc12345, 0xfff82468a0000000ull},
        Literal{float32_le, 0x7fbfffff, 0x7fbfffff, 0x7ff7ffffe0000000ull}};
    for (const auto& literal : literals) {
        for (const auto output : float_outputs) {
            if (!accepts(literal.source, output)) { continue; }
            const auto expected = output.bytes() == 4 ? literal.float32 : literal.float64;
            expect("mathematical oracle agrees with IEEE literal", expected_float_bits(literal.input, literal.source, output.bytes()), expected);
            for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
                // Four SIMD samples plus a scalar tail, with an unaligned input.
                constexpr std::size_t frames = 5;
                const auto frame_bytes = literal.source.bytes() * channels;
                std::vector<std::byte> source(1 + frames * frame_bytes);
                const auto padding = literal.source.bytes() - literal.source.valid_bits / 8U;
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        for (std::size_t byte = padding; byte < literal.source.bytes(); ++byte) {
                            source[1 + frame * frame_bytes + channel * literal.source.bytes() + byte] =
                                static_cast<std::byte>(literal.input >> (8 * (byte - padding)));
                        }
                    }
                }
                std::array<std::byte, frames * 8> left{}, right{};
                find_planar_converter({48000, channels, literal.source}, output).value()(source.data() + 1, frames, left.data(), right.data());
                for (std::size_t byte = 0; byte < frames * output.bytes(); ++byte) {
                    const auto in_sample = byte % output.bytes();
                    const auto index = output.byte_order == std::endian::little ? in_sample : output.bytes() - 1 - in_sample;
                    const auto wanted = static_cast<std::uint8_t>(expected >> (8 * index));
                    expect("literal left IEEE byte", static_cast<std::uint8_t>(left[byte]), wanted);
                    expect("literal right IEEE byte", static_cast<std::uint8_t>(right[byte]), wanted);
                }
            }
        }
    }
}

void test_float64_group_mixes_zeros_and_subnormals()
{
    // Only the subnormal lane is normalized. Every other lane, including
    // signaling NaN payloads, keeps its fast-path encoding throughout the input group.
    constexpr std::array groups{
        std::array<std::uint32_t, 4>{0x00000000, 0x80000000, 0x80000003, 0x3f800001},
        std::array<std::uint32_t, 4>{0x7f800001, 0xffc12345, 0x00000003, 0xff800000}};
    constexpr std::size_t frames = 4;
    for (const auto& values : groups) {
        std::array<std::size_t, frames> order{0, 1, 2, 3};
        do {
            for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
                const auto bits = [&](std::size_t frame, bool right) { return values[order[right ? frames - 1 - frame : frame]]; };
                std::vector<std::byte> source(frames * 4 * channels);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        const auto sample = bits(frame, channel != 0);
                        std::memcpy(source.data() + (frame * channels + channel) * 4, &sample, 4);
                    }
                }
                for (const auto byte_order : {std::endian::little, std::endian::big}) {
                    const SampleFormat output{SampleKind::floating_point, 8, 64, byte_order};
                    std::array<std::byte, frames * 8> left{}, right{};
                    find_planar_converter({48000, channels, wavplay::audio::float32_le}, output).value()(
                        source.data(), frames, left.data(), right.data());
                    for (std::size_t byte = 0; byte < left.size(); ++byte) {
                        const auto frame = byte / 8;
                        expect("mixed zero/subnormal group left byte", static_cast<std::uint8_t>(left[byte]),
                            expected_byte(bits(frame, false), byte % 8, output, wavplay::audio::float32_le));
                        expect("mixed zero/subnormal group right byte", static_cast<std::uint8_t>(right[byte]),
                            expected_byte(bits(frame, channels == 2), byte % 8, output, wavplay::audio::float32_le));
                    }
                }
            }
        } while (std::ranges::next_permutation(order).found);
    }
}

void test_float32_reuses_bitwise_copy()
{
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto order : {std::endian::little, std::endian::big}) {
            const auto integer = find_planar_converter({48000, channels, wavplay::audio::int32_le}, {SampleKind::integer, 4, 32, order}).value();
            const auto floating = find_planar_converter({48000, channels, wavplay::audio::float32_le}, {SampleKind::floating_point, 4, 32, order}).value();
            expect("same-width IEEE samples reuse bitwise copy/shuffle", static_cast<int>(integer.kernel() == floating.kernel()), 1);
        }
    }
}

void test_kernels_preserve_mxcsr()
{
    constexpr std::size_t frames = 65;
    std::array<std::byte, frames * 8> left{}, right{};
    for (const auto source_format : source_formats) {
        for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
            const auto source = make_source(channels, frames, source_format);
            for (const auto outputs : output_groups) {
                for (const auto output : outputs) {
                    if (!accepts(source_format, output)) { continue; }
                    const auto converter = find_planar_converter({48000, channels, source_format}, output).value();
                    for (const unsigned rounding : {_MM_ROUND_NEAREST, _MM_ROUND_DOWN, _MM_ROUND_UP, _MM_ROUND_TOWARD_ZERO}) {
                        for (const unsigned flush : {0U, static_cast<unsigned>(_MM_FLUSH_ZERO_ON), static_cast<unsigned>(_MM_DENORMALS_ZERO_ON),
                                 static_cast<unsigned>(_MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON)}) {
                            for (const unsigned flags : {0U, static_cast<unsigned>(_MM_EXCEPT_MASK)}) {
                                const auto saved = _mm_getcsr();
                                const auto before = ((saved | _MM_MASK_MASK) & ~static_cast<unsigned>(
                                    _MM_EXCEPT_MASK | _MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK)) | rounding | flush | flags;
                                _mm_setcsr(before);
                                // Includes actual sNaNs/subnormals and integer
                                // samples that would be sNaNs if misinterpreted.
                                converter(source.data(), frames, left.data(), right.data());
                                const auto after = _mm_getcsr();
                                _mm_setcsr(saved);
                                expect("conversion preserves all MXCSR flags and controls", after, before);
                                // Check the bytes too: DAZ/FTZ must not erase a
                                // subnormal even when no status flag is raised.
                                for (std::size_t frame = 0; frame < frames; ++frame) {
                                    for (std::size_t byte = 0; byte < output.bytes(); ++byte) {
                                        expect("MXCSR-independent left sample", static_cast<std::uint8_t>(left[frame * output.bytes() + byte]),
                                            expected_byte(source_bits(frame, false, source_format), byte, output, source_format));
                                        expect("MXCSR-independent right sample", static_cast<std::uint8_t>(right[frame * output.bytes() + byte]),
                                            expected_byte(source_bits(frame, channels == 2, source_format), byte, output, source_format));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// The language and Windows API baseline both projects share (build/Common.props;
// docs/architecture.md, instruction set), as the compiler sees it: a change fails
// the build. The pin equals the default of SDK 10.0.26100, so only a different SDK
// or pin value shows here; scripts/verify.ps1 audits the switches without a macro.
static_assert(_MSVC_LANG == 202302L, "LanguageStandard stdcpp23 keeps the C++23 ceiling");
static_assert(_HAS_CXX26 == 0, "the C++26 library stays disabled (the C++23 ceiling)");
static_assert(__cplusplus == _MSVC_LANG, "/Zc:__cplusplus stays on");
static_assert(_MSVC_TRADITIONAL == 0, "/Zc:preprocessor stays on");
static_assert(NTDDI_VERSION == NTDDI_WIN11_GE, "NTDDI_VERSION stays at the minimum OS of docs/requirements.md");
static_assert(_WIN32_WINNT == _WIN32_WINNT_WIN10 && WINVER == _WIN32_WINNT_WIN10, "_WIN32_WINNT and WINVER stay pinned");
static_assert(sizeof(TCHAR) == sizeof(wchar_t), "CharacterSet stays Unicode");
static_assert(__AVX2__ == 1, "Product conversion kernels require /arch:AVX2");

void test_instruction_set_baseline()
{
    // The baseline startup guard checks the full AVX2 toolchain requirements
    // and OS vector-state support before this suite can run.
    expect("compiled with /arch:AVX2", static_cast<int>(__check_arch_support(__IA_SUPPORT_VECTOR256, 0)), 1);
    expect("CPU reports SSE4.2", static_cast<int>(IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE) != FALSE), 1);
    expect("CPU reports SSSE3", static_cast<int>(IsProcessorFeaturePresent(PF_SSSE3_INSTRUCTIONS_AVAILABLE) != FALSE), 1);
}

} // namespace

int wavplay_test::run_planar_conversion_tests()
{
    TestSuite suite;
    suite.run("all integer/Float input/channel/output precision pairs", test_conversion_matrix);
    suite.run("all kernels at every input/output byte alignment modulo 32", test_byte_alignments);
    suite.run("PCM16 mono/stereo kernels for all 14 integer output types", test_pcm16_kernels);
    suite.run("every PCM16 bit pattern to all 14 integer output types", [] { test_all_pcm16_patterns(); });
    suite.run("PCM24 kernels preserve edges and every bit in all compatible integer outputs", [] { test_wide_pcm_kernels(wavplay::audio::int24_le); });
    suite.run("PCM24/32 kernels preserve edges and every bit in all compatible integer outputs", [] { test_wide_pcm_kernels(wavplay::audio::int24_in32_le); });
    suite.run("PCM32 kernels preserve edges and every bit in either byte order", [] { test_wide_pcm_kernels(wavplay::audio::int32_le); });
    suite.run("PCM16 kernels stay inside exact spans beside inaccessible pages", [] { test_guarded_spans(wavplay::audio::int16_le); });
    suite.run("PCM24 kernels stay inside exact spans beside inaccessible pages", [] { test_guarded_spans(wavplay::audio::int24_le); });
    suite.run("PCM24/32 kernels stay inside exact spans beside inaccessible pages", [] { test_guarded_spans(wavplay::audio::int24_in32_le); });
    suite.run("PCM32 kernels stay inside exact spans beside inaccessible pages", [] { test_guarded_spans(wavplay::audio::int32_le); });
    suite.run("zero-frame conversion leaves outputs untouched", test_zero_frames);
    suite.run("PCM16 conversion into strided blocks leaves the gaps untouched", [] { test_blocked_destination(wavplay::audio::int16_le); });
    suite.run("PCM24 conversion into strided blocks leaves the gaps untouched", [] { test_blocked_destination(wavplay::audio::int24_le); });
    suite.run("PCM24/32 conversion into strided blocks leaves the gaps untouched", [] { test_blocked_destination(wavplay::audio::int24_in32_le); });
    suite.run("PCM32 conversion into strided blocks leaves the gaps untouched", [] { test_blocked_destination(wavplay::audio::int32_le); });
    suite.run("AVX2 build and Windows CPU baseline", test_instruction_set_baseline);
    suite.run("PCM24/32 to Int32 reuses the PCM32 kernel", test_pcm24_in32_reuses_int32_kernel);
    suite.run("IEEE literal encodings include signed zero, subnormals, infinity and signaling NaN payloads", test_float_literals);
    suite.run("Float32 same-width output reuses bitwise copy/shuffle", test_float32_reuses_bitwise_copy);
    suite.run("Float64 groups mixing subnormals with signed zeros, normal values and NaNs", test_float64_group_mixes_zeros_and_subnormals);
    suite.run("every PCM16 bit pattern to all four Float output types", [] { test_all_pcm16_patterns(float_outputs); });
    suite.run("all Float output kernels preserve edges and every valid bit", [] {
        for (const auto source : source_formats) { test_wide_pcm_kernels(source, float_outputs); }
    });
    suite.run("Float kernels stay inside read-only spans beside inaccessible pages", [] {
        for (const auto source : source_formats) { test_guarded_spans(source, float_outputs); }
    });
    suite.run("Float conversion into dense/padded/split blocks preserves samples and gaps", [] {
        for (const auto source : source_formats) { test_blocked_destination(source, float_outputs); }
    });
    suite.run("all kernels preserve MXCSR in every rounding and FTZ/DAZ mode", test_kernels_preserve_mxcsr);
    suite.run("unsupported sources, channel counts and outputs", test_unsupported_pairs);
    return suite.result("Planar conversion");
}
