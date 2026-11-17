#pragma once

#include <chrono>
#include <limits>
#include <stdexcept>

namespace wavplay::asio {

// Silence returned after each start, before the first PCM block. A tested driver
// lost the beginning of a segment and clicked when playback began with audio.
inline constexpr std::chrono::milliseconds default_start_silence{100};
inline constexpr std::chrono::milliseconds max_start_silence{1000};

// The one rule for a configured output pair: a one-based odd first output whose
// adjacent output also fits in a long. Throws std::invalid_argument otherwise.
inline void validate_output_pair(long output_pair)
{
    if (output_pair < 1 || output_pair > (std::numeric_limits<long>::max)() - 2 || output_pair % 2 == 0) {
        throw std::invalid_argument("ASIO OutputPair must be a positive odd channel number with an adjacent output");
    }
}

// Per-device playback settings, as stored in settings.ini [Device.{CLSID}].
// This header carries no SDK dependency, so GUI code can use it.
struct DeviceSettings {
    std::chrono::milliseconds start_silence{default_start_silence};
    long buffer_frames{}; // 0 selects the driver's preferred width; positive values are exact.
    long output_pair{1}; // One-based odd first output of the adjacent pair.

    // Throws std::invalid_argument for values playback must not start with.
    void validate() const
    {
        if (start_silence.count() < 0 || start_silence > max_start_silence) {
            throw std::invalid_argument("ASIO start silence is out of range");
        }
        if (buffer_frames < 0) {
            throw std::invalid_argument("ASIO buffer size must not be negative");
        }
        validate_output_pair(output_pair);
    }
};

} // namespace wavplay::asio
