#include "wavplay/asio/sample_types.h"

#include <bit>

namespace wavplay::asio {
namespace {

using audio::BitAlignment;
using audio::SampleFormat;
using audio::SampleKind;

constexpr SampleFormat integer(std::uint8_t bytes, std::uint8_t valid_bits, std::endian order,
    BitAlignment alignment = BitAlignment::msb)
{
    return {SampleKind::integer, bytes, valid_bits, order, alignment};
}

constexpr SampleFormat floating(std::uint8_t bytes, std::endian order)
{
    return {SampleKind::floating_point, bytes, static_cast<std::uint8_t>(8 * bytes), order};
}

constexpr auto lsb = std::endian::little;
constexpr auto msb = std::endian::big;

} // namespace

std::optional<audio::SampleFormat> to_sample_format(ASIOSampleType type) noexcept
{
    switch (type) {
    case ASIOSTInt16LSB: return integer(2, 16, lsb);
    case ASIOSTInt24LSB: return integer(3, 24, lsb);
    case ASIOSTInt32LSB: return integer(4, 32, lsb);
    case ASIOSTFloat32LSB: return floating(4, lsb);
    case ASIOSTFloat64LSB: return floating(8, lsb);
    case ASIOSTInt32LSB16: return integer(4, 16, lsb, BitAlignment::lsb);
    case ASIOSTInt32LSB18: return integer(4, 18, lsb, BitAlignment::lsb);
    case ASIOSTInt32LSB20: return integer(4, 20, lsb, BitAlignment::lsb);
    case ASIOSTInt32LSB24: return integer(4, 24, lsb, BitAlignment::lsb);
    case ASIOSTInt16MSB: return integer(2, 16, msb);
    case ASIOSTInt24MSB: return integer(3, 24, msb);
    case ASIOSTInt32MSB: return integer(4, 32, msb);
    case ASIOSTFloat32MSB: return floating(4, msb);
    case ASIOSTFloat64MSB: return floating(8, msb);
    case ASIOSTInt32MSB16: return integer(4, 16, msb, BitAlignment::lsb);
    case ASIOSTInt32MSB18: return integer(4, 18, msb, BitAlignment::lsb);
    case ASIOSTInt32MSB20: return integer(4, 20, msb, BitAlignment::lsb);
    case ASIOSTInt32MSB24: return integer(4, 24, msb, BitAlignment::lsb);
    default: return std::nullopt;
    }
}

const char* sample_type_name(ASIOSampleType type) noexcept
{
    switch (type) {
    case ASIOSTInt16MSB: return "ASIOSTInt16MSB";
    case ASIOSTInt24MSB: return "ASIOSTInt24MSB";
    case ASIOSTInt32MSB: return "ASIOSTInt32MSB";
    case ASIOSTFloat32MSB: return "ASIOSTFloat32MSB";
    case ASIOSTFloat64MSB: return "ASIOSTFloat64MSB";
    case ASIOSTInt32MSB16: return "ASIOSTInt32MSB16";
    case ASIOSTInt32MSB18: return "ASIOSTInt32MSB18";
    case ASIOSTInt32MSB20: return "ASIOSTInt32MSB20";
    case ASIOSTInt32MSB24: return "ASIOSTInt32MSB24";
    case ASIOSTInt16LSB: return "ASIOSTInt16LSB";
    case ASIOSTInt24LSB: return "ASIOSTInt24LSB";
    case ASIOSTInt32LSB: return "ASIOSTInt32LSB";
    case ASIOSTFloat32LSB: return "ASIOSTFloat32LSB";
    case ASIOSTFloat64LSB: return "ASIOSTFloat64LSB";
    case ASIOSTInt32LSB16: return "ASIOSTInt32LSB16";
    case ASIOSTInt32LSB18: return "ASIOSTInt32LSB18";
    case ASIOSTInt32LSB20: return "ASIOSTInt32LSB20";
    case ASIOSTInt32LSB24: return "ASIOSTInt32LSB24";
    case ASIOSTDSDInt8LSB1: return "ASIOSTDSDInt8LSB1";
    case ASIOSTDSDInt8MSB1: return "ASIOSTDSDInt8MSB1";
    case ASIOSTDSDInt8NER8: return "ASIOSTDSDInt8NER8";
    default: return "Unknown";
    }
}

} // namespace wavplay::asio
