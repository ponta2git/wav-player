#pragma once

#include "wavplay/asio/asio_sdk.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

// Expected ASIO output samples written from the SDK type descriptions and
// IEEE 754, independently of every product format and conversion descriptor.
namespace wavplay_test {

struct IntegerOutputCase {
    ASIOSampleType type;
    std::uint8_t bytes, valid_bits;
    bool big_endian;
};

// Independent of to_sample_format and every product conversion descriptor.
// ASIO SDK 2.3 pp.33-34: Int32xxxN holds signed N-bit data in its low bits,
// with sign extension above N; MSB/LSB selects the byte order only.
inline constexpr std::array<IntegerOutputCase, 14> integer_output_cases{{
    {ASIOSTInt16LSB, 2, 16, false}, {ASIOSTInt24LSB, 3, 24, false}, {ASIOSTInt32LSB, 4, 32, false},
    {ASIOSTInt32LSB16, 4, 16, false}, {ASIOSTInt32LSB18, 4, 18, false},
    {ASIOSTInt32LSB20, 4, 20, false}, {ASIOSTInt32LSB24, 4, 24, false},
    {ASIOSTInt16MSB, 2, 16, true}, {ASIOSTInt24MSB, 3, 24, true}, {ASIOSTInt32MSB, 4, 32, true},
    {ASIOSTInt32MSB16, 4, 16, true}, {ASIOSTInt32MSB18, 4, 18, true},
    {ASIOSTInt32MSB20, 4, 20, true}, {ASIOSTInt32MSB24, 4, 24, true},
}};

inline const IntegerOutputCase& integer_output_case(ASIOSampleType type)
{
    for (const auto& output : integer_output_cases) {
        if (output.type == type) { return output; }
    }
    throw std::invalid_argument("Expected an integer ASIO output case");
}

// The input is one canonical packed LE sample, never a product-converted value.
// Restore its signed mathematical value in int64, multiply to the target full
// scale, then serialize the two's-complement result in the specified byte order.
inline std::array<std::uint8_t, 4> expected_integer_sample(const std::uint8_t* packed_le_sample,
    std::uint8_t source_bits, const IntegerOutputCase& output)
{
    if ((source_bits != 16 && source_bits != 24 && source_bits != 32) || output.valid_bits < source_bits ||
        output.valid_bits > 32 || output.bytes < 2 || output.bytes > 4 || output.valid_bits > 8 * output.bytes) {
        throw std::invalid_argument("Invalid lossless integer oracle dimensions");
    }
    std::int64_t sample{};
    for (unsigned byte = 0; byte < source_bits / 8U; ++byte) {
        sample += std::int64_t{packed_le_sample[byte]} << (8 * byte);
    }
    if ((sample & (std::int64_t{1} << (source_bits - 1))) != 0) {
        sample -= std::int64_t{1} << source_bits;
    }
    const auto scaled = sample * (std::int64_t{1} << (output.valid_bits - source_bits));
    const auto bits = static_cast<std::uint32_t>(scaled);
    std::array<std::uint8_t, 4> result{};
    for (unsigned byte = 0; byte < output.bytes; ++byte) {
        const auto significance = output.big_endian ? output.bytes - 1 - byte : byte;
        result[byte] = static_cast<std::uint8_t>(bits >> (8 * significance));
    }
    return result;
}

struct FloatOutputCase {
    ASIOSampleType type;
    std::uint8_t bytes;
    bool big_endian;
};

inline constexpr std::array<FloatOutputCase, 4> float_output_cases{{
    {ASIOSTFloat32LSB, 4, false}, {ASIOSTFloat32MSB, 4, true},
    {ASIOSTFloat64LSB, 8, false}, {ASIOSTFloat64MSB, 8, true},
}};

inline const FloatOutputCase& float_output_case(ASIOSampleType type)
{
    for (const auto& output : float_output_cases) {
        if (output.type == type) { return output; }
    }
    throw std::invalid_argument("Expected a floating ASIO output case");
}

// Independent numeric oracle. All arithmetic is integer arithmetic: the input
// can contain signaling NaNs and the caller's floating-point mode is untouched.
// PCM input is canonical packed LE, including the valid 24 bits of PCM24/32.
inline std::array<std::uint8_t, 8> expected_float_sample(const std::uint8_t* source,
    std::uint8_t source_bits, const FloatOutputCase& output, bool source_is_float32 = false)
{
    if ((source_bits != 16 && source_bits != 24 && source_bits != 32) ||
        (output.bytes != 4 && output.bytes != 8) || (source_is_float32 && source_bits != 32) ||
        (!source_is_float32 && source_bits == 32 && output.bytes == 4)) {
        throw std::invalid_argument("Invalid lossless floating oracle dimensions");
    }
    std::uint64_t raw{};
    for (unsigned byte = 0; byte < source_bits / 8U; ++byte) {
        raw += std::uint64_t{source[byte]} << (8 * byte);
    }
    std::uint64_t encoded{};
    if (source_is_float32) {
        if (output.bytes == 4) {
            encoded = raw;
        } else {
            const auto sign = (raw & 0x80000000ULL) << 32;
            const auto exponent = static_cast<int>((raw >> 23) & 0xff);
            auto fraction = raw & 0x7fffff;
            if (exponent == 255) {
                // Preserves NaN payload, sign, and its quiet/signaling bit.
                encoded = sign | 0x7ff0000000000000ULL | (fraction << 29);
            } else if (exponent != 0) {
                encoded = sign | (std::uint64_t{static_cast<unsigned>(exponent - 127 + 1023)} << 52) | (fraction << 29);
            } else if (fraction == 0) {
                encoded = sign;
            } else {
                int power = -126;
                while (fraction < 0x800000) { fraction *= 2; --power; }
                encoded = sign | (std::uint64_t{static_cast<unsigned>(power + 1023)} << 52) |
                    ((fraction - 0x800000) << 29);
            }
        }
    } else {
        const bool negative = (raw & (std::uint64_t{1} << (source_bits - 1))) != 0;
        auto magnitude = negative ? (std::uint64_t{1} << source_bits) - raw : raw;
        if (magnitude != 0) {
            unsigned highest{};
            for (auto rest = magnitude; rest > 1; rest /= 2) { ++highest; }
            const unsigned fraction_bits = output.bytes == 4 ? 23 : 52;
            const int bias = output.bytes == 4 ? 127 : 1023;
            const auto exponent = static_cast<unsigned>(static_cast<int>(highest) - (source_bits - 1) + bias);
            magnitude -= std::uint64_t{1} << highest;
            encoded = (std::uint64_t{negative} << (8 * output.bytes - 1)) |
                (std::uint64_t{exponent} << fraction_bits) | (magnitude << (fraction_bits - highest));
        }
    }
    std::array<std::uint8_t, 8> result{};
    for (unsigned byte = 0; byte < output.bytes; ++byte) {
        const auto significance = output.big_endian ? output.bytes - 1 - byte : byte;
        result[byte] = static_cast<std::uint8_t>(encoded >> (8 * significance));
    }
    return result;
}

inline std::uint8_t output_sample_bytes(ASIOSampleType type)
{
    for (const auto& output : float_output_cases) {
        if (output.type == type) { return output.bytes; }
    }
    return integer_output_case(type).bytes;
}

inline std::array<std::uint8_t, 8> expected_output_sample(const std::uint8_t* source,
    std::uint8_t source_bits, ASIOSampleType type, bool source_is_float32 = false)
{
    for (const auto& output : float_output_cases) {
        if (output.type == type) { return expected_float_sample(source, source_bits, output, source_is_float32); }
    }
    if (source_is_float32) { throw std::invalid_argument("Float32 cannot be converted losslessly to integer"); }
    const auto packed = expected_integer_sample(source, source_bits, integer_output_case(type));
    std::array<std::uint8_t, 8> result{};
    std::ranges::copy(packed, result.begin());
    return result;
}

} // namespace wavplay_test
