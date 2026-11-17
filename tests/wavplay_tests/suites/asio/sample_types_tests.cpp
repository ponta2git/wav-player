#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_builder.h"
#include "wavplay/asio/sample_types.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace {

using namespace wavplay_test;
using wavplay::audio::BitAlignment;
using wavplay::audio::SampleKind;

struct Expected {
    ASIOSampleType type;
    const char* name;
    SampleKind kind;
    std::uint8_t bytes, valid_bits;
    std::endian order;
    BitAlignment alignment;
};

// Written from SDK 2.3 PDF pp. 33-34, independently of the product table.
constexpr auto integer = SampleKind::integer;
constexpr auto floating = SampleKind::floating_point;
constexpr auto le = std::endian::little;
constexpr auto be = std::endian::big;
constexpr auto high = BitAlignment::msb;
constexpr auto low = BitAlignment::lsb;
constexpr Expected described[]{
    {ASIOSTInt16LSB, "ASIOSTInt16LSB", integer, 2, 16, le, high},
    {ASIOSTInt24LSB, "ASIOSTInt24LSB", integer, 3, 24, le, high},
    {ASIOSTInt32LSB, "ASIOSTInt32LSB", integer, 4, 32, le, high},
    {ASIOSTFloat32LSB, "ASIOSTFloat32LSB", floating, 4, 32, le, high},
    {ASIOSTFloat64LSB, "ASIOSTFloat64LSB", floating, 8, 64, le, high},
    {ASIOSTInt32LSB16, "ASIOSTInt32LSB16", integer, 4, 16, le, low},
    {ASIOSTInt32LSB18, "ASIOSTInt32LSB18", integer, 4, 18, le, low},
    {ASIOSTInt32LSB20, "ASIOSTInt32LSB20", integer, 4, 20, le, low},
    {ASIOSTInt32LSB24, "ASIOSTInt32LSB24", integer, 4, 24, le, low},
    {ASIOSTInt16MSB, "ASIOSTInt16MSB", integer, 2, 16, be, high},
    {ASIOSTInt24MSB, "ASIOSTInt24MSB", integer, 3, 24, be, high},
    {ASIOSTInt32MSB, "ASIOSTInt32MSB", integer, 4, 32, be, high},
    {ASIOSTFloat32MSB, "ASIOSTFloat32MSB", floating, 4, 32, be, high},
    {ASIOSTFloat64MSB, "ASIOSTFloat64MSB", floating, 8, 64, be, high},
    {ASIOSTInt32MSB16, "ASIOSTInt32MSB16", integer, 4, 16, be, low},
    {ASIOSTInt32MSB18, "ASIOSTInt32MSB18", integer, 4, 18, be, low},
    {ASIOSTInt32MSB20, "ASIOSTInt32MSB20", integer, 4, 20, be, low},
    {ASIOSTInt32MSB24, "ASIOSTInt32MSB24", integer, 4, 24, be, low},
};

void test_pcm_types()
{
    for (const auto& want : described) {
        const auto format = wavplay::asio::to_sample_format(want.type);
        const std::string name(want.name);
        if (!format) {
            throw std::runtime_error(name + ": expected a sample format");
        }
        expect(name + " kind", static_cast<int>(format->kind), static_cast<int>(want.kind));
        expect(name + " container bytes", format->container_bytes, want.bytes);
        expect(name + " valid bits", format->valid_bits, want.valid_bits);
        expect(name + " byte order", static_cast<int>(format->byte_order), static_cast<int>(want.order));
        expect(name + " alignment", static_cast<int>(format->alignment), static_cast<int>(want.alignment));
        expect(name + " name", std::strcmp(wavplay::asio::sample_type_name(want.type), want.name), 0);
    }
    // These established LSB descriptors retain their exact identity.
    expect("Int16LSB is int16_le", static_cast<int>(*wavplay::asio::to_sample_format(ASIOSTInt16LSB) ==
        wavplay::audio::int16_le), 1);
    expect("Int24LSB is int24_le", static_cast<int>(*wavplay::asio::to_sample_format(ASIOSTInt24LSB) ==
        wavplay::audio::int24_le), 1);
    expect("Int32LSB is int32_le", static_cast<int>(*wavplay::asio::to_sample_format(ASIOSTInt32LSB) ==
        wavplay::audio::int32_le), 1);
}

void test_integer_oracle()
{
    // Literal SDK N-bit full-scale values for PCM16 {-32768, -1, 32767}.
    // The upper bits of a 32-bit N-bit container are sign extensions, not zero.
    struct Golden {
        ASIOSampleType little, big;
        std::uint8_t bytes;
        std::array<std::uint32_t, 3> words;
    };
    constexpr Golden golden[]{
        {ASIOSTInt16LSB, ASIOSTInt16MSB, 2, {0x8000, 0xffff, 0x7fff}},
        {ASIOSTInt24LSB, ASIOSTInt24MSB, 3, {0x800000, 0xffff00, 0x7fff00}},
        {ASIOSTInt32LSB, ASIOSTInt32MSB, 4, {0x80000000, 0xffff0000, 0x7fff0000}},
        {ASIOSTInt32LSB16, ASIOSTInt32MSB16, 4, {0xffff8000, 0xffffffff, 0x00007fff}},
        {ASIOSTInt32LSB18, ASIOSTInt32MSB18, 4, {0xfffe0000, 0xfffffffc, 0x0001fffc}},
        {ASIOSTInt32LSB20, ASIOSTInt32MSB20, 4, {0xfff80000, 0xfffffff0, 0x0007fff0}},
        {ASIOSTInt32LSB24, ASIOSTInt32MSB24, 4, {0xff800000, 0xffffff00, 0x007fff00}},
    };
    constexpr std::array<std::array<std::uint8_t, 2>, 3> input{{{0, 0x80}, {0xff, 0xff}, {0xff, 0x7f}}};
    expect("independent integer case count", integer_output_cases.size(), 14);
    for (std::size_t i = 0; i < integer_output_cases.size(); ++i) {
        for (std::size_t j = i + 1; j < integer_output_cases.size(); ++j) {
            expect("integer case types are unique", static_cast<int>(integer_output_cases[i].type == integer_output_cases[j].type), 0);
        }
    }
    for (const auto& example : golden) {
        for (const bool big : {false, true}) {
            const auto& output = integer_output_case(big ? example.big : example.little);
            expect("oracle golden container bytes", output.bytes, example.bytes);
            for (std::size_t sample = 0; sample < input.size(); ++sample) {
                const auto actual = expected_integer_sample(input[sample].data(), 16, output);
                for (unsigned byte = 0; byte < example.bytes; ++byte) {
                    const auto shift = 8 * (big ? example.bytes - 1 - byte : byte);
                    expect("oracle matches literal SDK word", actual[byte], static_cast<std::uint8_t>(example.words[sample] >> shift));
                }
            }
        }
    }
    for (const auto bits : {std::uint8_t{24}, std::uint8_t{32}}) {
        const std::array<std::uint8_t, 4> minus_one{0xff, 0xff, 0xff, 0xff};
        const auto& output = integer_output_case(ASIOSTInt32LSB);
        const auto actual = expected_integer_sample(minus_one.data(), bits, output);
        const std::array<std::uint8_t, 4> expected = bits == 24 ?
            std::array<std::uint8_t, 4>{0, 0xff, 0xff, 0xff} : minus_one;
        for (std::size_t byte = 0; byte < expected.size(); ++byte) {
            expect("oracle sign and widening for wide inputs", actual[byte], expected[byte]);
        }
    }
}

void test_float_oracle()
{
    struct FloatingGolden { std::uint32_t input; std::uint64_t widened; };
    constexpr FloatingGolden floating_values[]{
        {0, 0}, {0x80000000, 0x8000000000000000ULL},
        {1, 0x36a0000000000000ULL}, {0x80000001, 0xb6a0000000000000ULL},
        {0x007fffff, 0x380fffffc0000000ULL}, {0x00800000, 0x3810000000000000ULL},
        {0x3f800000, 0x3ff0000000000000ULL}, {0xbf800000, 0xbff0000000000000ULL},
        {0x7f7fffff, 0x47efffffe0000000ULL}, {0xff7fffff, 0xc7efffffe0000000ULL},
        {0x7f800000, 0x7ff0000000000000ULL}, {0xff800000, 0xfff0000000000000ULL},
        {0x7fc12345, 0x7ff82468a0000000ULL}, {0xffc12345, 0xfff82468a0000000ULL},
        {0x7f812345, 0x7ff02468a0000000ULL}, {0xff812345, 0xfff02468a0000000ULL},
    };
    struct PcmGolden { std::uint8_t bits; std::uint32_t input, single; std::uint64_t doubled; };
    constexpr PcmGolden pcm_values[]{
        {16, 0, 0, 0}, {16, 0x8000, 0xbf800000, 0xbff0000000000000ULL},
        {16, 0x7fff, 0x3f7ffe00, 0x3fefffc000000000ULL}, {16, 0xffff, 0xb8000000, 0xbf00000000000000ULL},
        {24, 0x800000, 0xbf800000, 0xbff0000000000000ULL},
        {24, 0x7fffff, 0x3f7ffffe, 0x3fefffffc0000000ULL}, {24, 0xffffff, 0xb4000000, 0xbe80000000000000ULL},
        {32, 0x80000000, 0, 0xbff0000000000000ULL},
        {32, 0x7fffffff, 0, 0x3fefffffffc00000ULL}, {32, 0xffffffff, 0, 0xbe00000000000000ULL},
    };
    const auto check = [](const std::array<std::uint8_t, 8>& actual, std::uint64_t bits, const FloatOutputCase& output) {
        for (unsigned byte = 0; byte < output.bytes; ++byte) {
            const auto shift = 8 * (output.big_endian ? output.bytes - 1 - byte : byte);
            expect("floating oracle literal bit representation", actual[byte], static_cast<std::uint8_t>(bits >> shift));
        }
    };
    expect("floating ASIO case count", float_output_cases.size(), 4);
    for (const auto output : float_output_cases) {
        expect("common output byte count", output_sample_bytes(output.type), output.bytes);
        for (const auto& value : floating_values) {
            const auto source = le32(value.input);
            check(expected_float_sample(source.data(), 32, output, true), output.bytes == 4 ? value.input : value.widened, output);
            check(expected_output_sample(source.data(), 32, output.type, true), output.bytes == 4 ? value.input : value.widened, output);
        }
        for (const auto& value : pcm_values) {
            const auto source = le32(value.input);
            if (value.bits == 32 && output.bytes == 4) {
                expect_throw<std::invalid_argument>("oracle refuses PCM32 to Float32", [&] {
                    (void)expected_float_sample(source.data(), value.bits, output);
                });
            } else {
                check(expected_float_sample(source.data(), value.bits, output), output.bytes == 4 ? value.single : value.doubled, output);
            }
        }
    }
}

void test_undescribed_types()
{
    for (const ASIOSampleType type : std::initializer_list<ASIOSampleType>{ASIOSTDSDInt8LSB1, ASIOSTDSDInt8MSB1,
             ASIOSTDSDInt8NER8, -1}) {
        expect("no PCM format for " + std::to_string(type),
            static_cast<int>(wavplay::asio::to_sample_format(type).has_value()), 0);
    }
    expect("DSD name", std::strcmp(wavplay::asio::sample_type_name(ASIOSTDSDInt8NER8), "ASIOSTDSDInt8NER8"), 0);
    expect("unknown name", std::strcmp(wavplay::asio::sample_type_name(static_cast<ASIOSampleType>(-1)), "Unknown"), 0);
}

} // namespace

int wavplay_test::run_sample_types_tests()
{
    TestSuite suite;
    suite.run("ASIO PCM sample types as sample formats", test_pcm_types);
    suite.run("independent integer output oracle matches literal SDK words", test_integer_oracle);
    suite.run("independent floating output oracle preserves literal IEEE representations", test_float_oracle);
    suite.run("DSD and unknown ASIO sample types", test_undescribed_types);
    return suite.result("ASIO sample types");
}
