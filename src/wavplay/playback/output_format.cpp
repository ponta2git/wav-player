#include "wavplay/playback/output_format.h"

#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/device_settings.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/audio/planar_conversion.h"

#include <stdexcept>

namespace wavplay::playback {
namespace {

std::optional<audio::SampleFormat> playback_format_of(ASIOSampleType type, const audio::StreamFormat& source) noexcept
{
    const auto format = asio::to_sample_format(type);
    if (!format || !audio::find_planar_converter(source, *format)) {
        return std::nullopt;
    }
    return format;
}

} // namespace

std::optional<audio::SampleFormat> negotiate_output_format(IASIO& driver, const audio::StreamFormat& source,
    std::array<ASIOSampleType, 2>* active_types, long output_pair)
{
    asio::validate_output_pair(output_pair);
    if (asio::output_channel_count(driver) <= output_pair) {
        return std::nullopt;
    }
    std::optional<audio::SampleFormat> pair;
    for (long channel = 0; channel < 2; ++channel) {
        ASIOChannelInfo info{};
        const auto error = asio::query_output_info(driver, output_pair - 1 + channel, info);
        // Topology can change between the count and the per-channel inquiry.
        if ((error == ASE_InvalidParameter || error == ASE_NotPresent) && asio::output_channel_count(driver) <= output_pair) {
            return std::nullopt;
        }
        asio::throw_if_error(error, "ASIO getChannelInfo failed");
        if (active_types) {
            if (info.isActive != ASIOTrue) {
                throw std::runtime_error("ASIO output channel was not activated");
            }
            (*active_types)[channel] = info.type;
        }
        const auto format = playback_format_of(info.type, source);
        if (!format || (pair && pair != format)) {
            return std::nullopt;
        }
        pair = format;
    }
    return pair;
}

long validate_created_outputs(IASIO& driver, const ASIOBufferInfo (&buffers)[2], std::size_t width,
    const audio::StreamFormat& source, const audio::SampleFormat& format, std::array<ASIOSampleType, 2>& active_types,
    long output_pair)
{
    if (negotiate_output_format(driver, source, &active_types, output_pair) != format) {
        throw std::runtime_error("ASIO output pair no longer matches the prepared PCM format");
    }
    asio::validate_output_buffers(buffers, width * format.bytes(), output_pair);
    return asio::output_latency(driver);
}

} // namespace wavplay::playback
