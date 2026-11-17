#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace wavplay::audio {

enum class SampleKind : std::uint8_t { integer, floating_point };

// Where the valid bits sit inside a wider integer container. WAVE_FORMAT_EXTENSIBLE
// keeps them in the most significant bits; ASIO's Int32xxx16/18/20/24 types keep
// them in the least significant bits.
enum class BitAlignment : std::uint8_t { msb, lsb };

// One sample as it is stored in memory. Input files and ASIO output buffers share
// this description, so supporting a new encoding means adding a converter, not a
// new type at every layer.
struct SampleFormat {
    SampleKind kind{SampleKind::integer};
    std::uint8_t container_bytes{};
    std::uint8_t valid_bits{};
    std::endian byte_order{std::endian::little};
    BitAlignment alignment{BitAlignment::msb};

    [[nodiscard]] constexpr std::size_t bytes() const noexcept { return container_bytes; }
    constexpr bool operator==(const SampleFormat&) const noexcept = default;
};

inline constexpr SampleFormat int16_le{SampleKind::integer, 2, 16};
inline constexpr SampleFormat int24_le{SampleKind::integer, 3, 24};
inline constexpr SampleFormat int24_in32_le{SampleKind::integer, 4, 24, std::endian::little, BitAlignment::msb};
inline constexpr SampleFormat int32_le{SampleKind::integer, 4, 32};
inline constexpr SampleFormat float32_le{SampleKind::floating_point, 4, 32};

// Interleaved frames of one track.
struct StreamFormat {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    SampleFormat sample{};

    [[nodiscard]] constexpr std::size_t frame_bytes() const noexcept { return std::size_t{channels} * sample.bytes(); }
    constexpr bool operator==(const StreamFormat&) const noexcept = default;
};

// 44.1, 48, 88.2, 96, 176.4, 192, 352.8 and 384 kHz.
inline constexpr std::array<std::uint32_t, 8> supported_sample_rates{
    44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

[[nodiscard]] constexpr bool is_supported_sample_rate(std::uint32_t rate) noexcept
{
    return std::ranges::find(supported_sample_rates, rate) != supported_sample_rates.end();
}

} // namespace wavplay::audio
