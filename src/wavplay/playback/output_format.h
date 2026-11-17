#pragma once

#include "wavplay/asio/asio_sdk.h"
#include "wavplay/audio/sample_format.h"

#include <array>
#include <cstddef>
#include <optional>

namespace wavplay::playback {

// The common format of the configured adjacent outputs that `source` converts to losslessly, or
// nullopt when the pair is missing, the two types differ, or no playback format
// matches. Converter availability is the single source of supported pairs.
// Supply active_types after createBuffers to require two active outputs
// and capture their types.
[[nodiscard]] std::optional<audio::SampleFormat> negotiate_output_format(IASIO& driver,
    const audio::StreamFormat& source, std::array<ASIOSampleType, 2>* active_types = nullptr, long output_pair = 1);

// Recheck the output pair and its four buffers after createBuffers. Returns the
// driver's raw output latency in frames.
[[nodiscard]] long validate_created_outputs(IASIO& driver, const ASIOBufferInfo (&buffers)[2], std::size_t width,
    const audio::StreamFormat& source, const audio::SampleFormat& format, std::array<ASIOSampleType, 2>& active_types,
    long output_pair = 1);

} // namespace wavplay::playback
