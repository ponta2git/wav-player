#pragma once

#include <bit>
#include <cstdint>
#include <limits>

namespace wavplay::asio {

// Driver clocks arrive as untrusted doubles (ASIOSampleRate). Inspect their bits,
// never their values in floating point: a signaling NaN must not raise an
// exception or change the thread's MXCSR, and every supported rate is an integer
// exactly representable as a double.

// Positive and finite. Positive finite IEEE-754 values sort below +infinity in
// unsigned bit order; zero, negatives, infinities and NaNs do not.
[[nodiscard]] constexpr bool is_valid_clock(double rate) noexcept
{
    const auto bits = std::bit_cast<std::uint64_t>(rate);
    return bits != 0 && bits < std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity());
}

[[nodiscard]] constexpr bool same_clock(double left, double right) noexcept
{
    return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
}

} // namespace wavplay::asio
