#include "suites/suites.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/playback/output_format.h"
#include "wavplay/playback/track_player.h"
#include "fixtures/playback_fixture.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// playback::negotiate_output_format chooses one format for an ASIO output pair,
// and playback refuses a pair it cannot use before allocating any buffer. The
// reader's own planar output for each format is tested in wav_reader.
namespace {

using namespace wavplay_test;

void test_pcm32_playback_rejection()
{
    TempFile file;
    const auto* path = file.write(wave({pcm32(2, 48000), {"data", Bytes(8, 0)}}));
    for (const auto type : {ASIOSTInt16LSB, ASIOSTInt16MSB, ASIOSTInt24LSB, ASIOSTInt24MSB,
        ASIOSTInt32LSB16, ASIOSTInt32MSB16, ASIOSTInt32LSB18, ASIOSTInt32MSB18,
        ASIOSTInt32LSB20, ASIOSTInt32MSB20, ASIOSTInt32LSB24, ASIOSTInt32MSB24,
        ASIOSTFloat32LSB, ASIOSTFloat32MSB}) {
        for (const bool right_only : {false, true}) {
            MockAsio driver;
            driver.left_type = right_only ? ASIOSTInt32LSB : type;
            driver.right_type = type;
            MockSession session(driver);
            wavplay::playback::TrackPlayer playback(path);
            expect_throw<std::runtime_error>("PCM32 playback rejects precision reduction", [&] { (void)playback.play(session); });
            expect("PCM32 precision rejection precedes buffer creation", driver.creates, 0);
            expect("PCM32 precision rejection precedes start", driver.starts, 0);
        }
    }
}

void test_all_integer_negotiation()
{
    using namespace wavplay::audio;
    for (const auto sample : {int16_le, int24_le, int24_in32_le, int32_le}) {
        const StreamFormat source{48000, 2, sample};
        for (const auto output : integer_output_cases) {
            MockAsio driver;
            driver.left_type = driver.right_type = output.type;
            const auto actual = wavplay::playback::negotiate_output_format(driver, source);
            const bool supported = output.valid_bits >= sample.valid_bits;
            expect("integer output negotiation follows valid precision", static_cast<int>(actual.has_value()), static_cast<int>(supported));
            if (actual) {
                expect("negotiated output bytes", actual->bytes(), output.bytes);
                expect("negotiated output precision", actual->valid_bits, output.valid_bits);
                expect("negotiated output byte order", static_cast<int>(actual->byte_order == std::endian::big), static_cast<int>(output.big_endian));
                const auto alignment = output.valid_bits == 8 * output.bytes ? BitAlignment::msb : BitAlignment::lsb;
                expect("negotiated output alignment", static_cast<int>(actual->alignment), static_cast<int>(alignment));
            }
        }
        for (const ASIOSampleType type : {ASIOSTDSDInt8LSB1, ASIOSTDSDInt8MSB1, ASIOSTDSDInt8NER8}) {
            MockAsio driver;
            driver.left_type = driver.right_type = type;
            expect("DSD output remains unsupported",
                static_cast<int>(wavplay::playback::negotiate_output_format(driver, source).has_value()), 0);
        }
    }
}

void test_float_negotiation()
{
    using namespace wavplay::audio;
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
        for (const auto sample : {int16_le, int24_le, int24_in32_le, int32_le, float32_le}) {
            for (const auto output : float_output_cases) {
                MockAsio driver;
                driver.left_type = driver.right_type = output.type;
                const auto actual = wavplay::playback::negotiate_output_format(driver, {48000, channels, sample});
                const bool supported = sample != int32_le || output.bytes == 8;
                expect("floating output uses significand precision", static_cast<int>(actual.has_value()), static_cast<int>(supported));
                if (actual) {
                    expect("floating output kind", static_cast<int>(actual->kind), static_cast<int>(SampleKind::floating_point));
                    expect("floating container bytes", actual->bytes(), output.bytes);
                    expect("floating container bits", actual->valid_bits, output.bytes * 8);
                    expect("floating byte order", static_cast<int>(actual->byte_order == std::endian::big), static_cast<int>(output.big_endian));
                }
            }
        }
        for (const auto output : integer_output_cases) {
            MockAsio driver;
            driver.left_type = driver.right_type = output.type;
            expect("Float32 cannot negotiate integer output",
                static_cast<int>(wavplay::playback::negotiate_output_format(driver, {48000, channels, float32_le}).has_value()), 0);
        }
    }
    std::vector<ASIOSampleType> types;
    for (const auto output : integer_output_cases) { types.push_back(output.type); }
    for (const auto output : float_output_cases) { types.push_back(output.type); }
    for (const auto left : types) {
        for (const auto right : types) {
            if (left == right) { continue; }
            MockAsio driver;
            driver.left_type = left;
            driver.right_type = right;
            // PCM16 fits all 18 types; only pair incompatibility can refuse it.
            expect("distinct ASIO types cannot share a pair",
                static_cast<int>(wavplay::playback::negotiate_output_format(driver, {48000, 2, int16_le}).has_value()), 0);
        }
    }
}

void test_float_integer_rejection()
{
    for (const bool extensible : {false, true}) {
        TempFile file;
        // +0 fits any integer numerically, but the format is still incompatible.
        const auto* path = file.write(wave({extensible ? extensible_float32(2, 48000, 3) : float32(2, 48000),
            fact(1), {"data", Bytes(8, 0)}}));
        for (const auto output : integer_output_cases) {
            for (const bool right_only : {false, true}) {
                MockAsio driver;
                driver.left_type = right_only ? ASIOSTFloat32LSB : output.type;
                driver.right_type = output.type;
                MockSession session(driver);
                wavplay::playback::TrackPlayer player(path);
                expect_throw<std::runtime_error>("Float32 playback refuses integer output before allocation", [&] { (void)player.play(session); });
                expect("Float32 rejection does not allocate buffers", driver.creates, 0);
                expect("Float32 rejection does not start ASIO", driver.starts, 0);
            }
        }
    }
}

// The one output-pair rule (asio::validate_output_pair) also guards negotiation
// and the created-buffer check, not only DeviceSettings, whose default pair is
// all these calls see in playback. An invalid pair is refused before any driver
// call, and buffers on exactly the channels it names are still refused.
void test_invalid_output_pairs()
{
    for (const long pair : {-1L, 0L, 2L, (std::numeric_limits<long>::max)()}) {
        MockAsio driver;
        driver.outputs = 4; // Pair 2 would fit: outputs 2 and 3 exist.
        expect_throw<std::invalid_argument>("negotiation refuses the pair", [&] {
            (void)wavplay::playback::negotiate_output_format(driver, {48000, 2, wavplay::audio::int16_le}, nullptr, pair);
        });
        expect("a refused pair calls no driver method", driver.calls.size(), 0);

        std::array<std::array<std::byte, 8>, 4> halves{};
        ASIOBufferInfo buffers[2]{};
        for (long index = 0; index < 2; ++index) {
            buffers[index].isInput = ASIOFalse;
            buffers[index].channelNum = pair - 1 + index; // No overflow for these pairs.
            buffers[index].buffers[0] = halves[static_cast<std::size_t>(2 * index)].data();
            buffers[index].buffers[1] = halves[static_cast<std::size_t>(2 * index + 1)].data();
        }
        expect_throw<std::invalid_argument>("the created-buffer check refuses the pair", [&] {
            wavplay::asio::validate_output_buffers(buffers, halves[0].size(), pair);
        });
    }
}

} // namespace

int wavplay_test::run_output_format_tests()
{
    TestSuite suite;
    suite.run("negotiation and the created-buffer check refuse invalid output pairs before any driver call",
        test_invalid_output_pairs);
    suite.run("all 14 integer types negotiate by valid precision", test_all_integer_negotiation);
    suite.run("all Float32/64 types negotiate losslessly and all 18 types reject mixed pairs", test_float_negotiation);
    suite.run("ordinary and Extensible Float32 reject every integer type before ASIO starts", test_float_integer_rejection);
    suite.run("PCM32 rejects integer and Float32 precision loss before ASIO starts", test_pcm32_playback_rejection);
    return suite.result("ASIO output pair negotiation");
}
