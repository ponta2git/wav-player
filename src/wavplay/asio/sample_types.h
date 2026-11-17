#pragma once

#include "wavplay/asio/asio_sdk.h"
#include "wavplay/audio/sample_format.h"

#include <optional>

namespace wavplay::asio {

// Describes an ASIO output buffer sample as an audio::SampleFormat: integer and
// float, LSB and MSB, and the 32-bit containers with 16/18/20/24 valid bits.
// Whether audio can be converted to it is decided by audio::find_planar_converter.
// DSD and unknown types return nullopt.
[[nodiscard]] std::optional<audio::SampleFormat> to_sample_format(ASIOSampleType type) noexcept;

// The SDK enumerator name, or "Unknown", for logs and diagnostics.
[[nodiscard]] const char* sample_type_name(ASIOSampleType type) noexcept;

} // namespace wavplay::asio
