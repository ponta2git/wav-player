#include "suites/suites.h"
#include "wavplay/playback/track_player.h"
#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/track_player_fixture.h"
#include "fixtures/wav_builder.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;

void check_raw_dac_run(const DacQueueAsio& driver, std::size_t run_index, const Bytes& source,
    std::uint8_t channels, std::uint8_t source_bits, bool source_is_float32,
    std::size_t first, std::size_t count, std::size_t silence)
{
    // Keep the entire hardware timeline: a source +0 is not removable padding,
    // and -0, NaNs and subnormals must never be compared as floating values.
    const auto& run = driver.dac_runs.at(run_index);
    expect("raw DAC emitted every expected source frame", static_cast<int>(run.size() >= silence + count), 1);
    for (std::size_t frame = 0; frame < run.size(); ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            std::array<std::uint8_t, 8> expected{};
            if (frame >= silence && frame - silence < count) {
                const auto offset = ((first + frame - silence) * channels + (channels == 1 ? 0 : channel)) * (source_bits / 8);
                expected = expected_output_sample(source.data() + offset, source_bits, driver.left_type, source_is_float32);
            }
            for (std::size_t byte = 0; byte < expected.size(); ++byte) {
                if (run[frame][channel][byte] != expected[byte]) {
                    expect("raw DAC frame " + std::to_string(frame) + " channel " + std::to_string(channel) +
                        " byte " + std::to_string(byte), run[frame][channel][byte], expected[byte]);
                }
            }
        }
    }
    expect("raw DAC queue starvation", driver.starved, 0);
    expect("raw DAC does not depend on sample position", driver.position_queries, 0);
}

void submit_raw_until(DacQueueAsio& driver, std::size_t frames)
{
    for (unsigned tick = 0; driver.submitted_frames() < frames; ++tick) {
        if (tick == 1000) { throw std::runtime_error("Mock driver did not submit the raw frame boundary"); }
        driver.advance_dac_block();
    }
}

void test_float_dac_output(const FloatOutputCase& output, std::uint8_t source_bits,
    bool source_is_float32, bool extensible)
{
    const auto channels = static_cast<std::uint8_t>(output.big_endian ? 2 : 1);
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    constexpr std::size_t silence = 3 * DacQueueAsio::block;
    constexpr std::size_t paused = 3 * DacQueueAsio::block;
    const auto data_for = [&](std::size_t count) {
        auto data = source_is_float32 ? float32_ramp(channels, count) : source_ramp(channels, count, source_bits);
        if (source_is_float32) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto offset = ((count - 1) * channels + channel) * 4;
                data.payload[offset] = data.payload[offset + 1] = data.payload[offset + 2] = 0;
                data.payload[offset + 3] = 0x80; // A final -0 must reach the DAC.
            }
        }
        return data;
    };
    const auto wave_for = [&](const Chunk& data, std::size_t count) {
        return source_is_float32 ? wave({extensible ? extensible_float32(channels, 48000) :
            float32(channels, 48000), data}) : source_wave(channels, count, source_bits, extensible);
    };
    const auto data = data_for(frames);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave_for(data, frames)));
    DacQueueAsio driver(output.big_endian ? DacQueueAsio::Position::unsupported : DacQueueAsio::Position::zero, output.type);
    auto done = start(driver, playback);
    submit_raw_until(driver, silence + paused);
    playback.request_pause();
    expect("float output Pause", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::paused));
    expect("float output retained frame includes source zeros", playback.position_frames(), paused);
    check_raw_dac_run(driver, 0, data.payload, channels, source_bits, source_is_float32, 0, paused, silence);
    expect("float output Pause drains queued samples", driver.discarded_pcm, 0);
    done = start(driver, playback);
    expect("float output Resume completes", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_raw_dac_run(driver, 1, data.payload, channels, source_bits, source_is_float32, paused, frames - paused, silence);
    expect("float output Resume reaches EOF", playback.position_frames(), frames);
    expect("float output EOF drains queued samples", driver.discarded_pcm, 0);

    playback.stop();
    clear_capture(driver);
    done = start(driver, playback);
    submit_raw_until(driver, silence + paused);
    playback.request_stop();
    expect("float output Stop needs no callback", static_cast<int>(finish_without_callbacks(done)),
        static_cast<int>(wavplay::playback::PlaybackResult::stopped));
    expect("float output Stop rewinds", playback.position_frames(), 0);
    const auto emitted = driver.dac_runs.at(0).size();
    const auto prefix = emitted > silence ? emitted - silence : 0;
    expect("float output Stop emits a proper prefix", static_cast<int>(prefix > 0 && prefix < frames), 1);
    check_raw_dac_run(driver, 0, data.payload, channels, source_bits, source_is_float32, 0, prefix, silence);
    clear_capture(driver);
    done = start(driver, playback);
    expect("float output Play after Stop completes", static_cast<int>(finish(driver, done)),
        static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_raw_dac_run(driver, 0, data.payload, channels, source_bits, source_is_float32, 0, frames, silence);
    expect("float output replay drains queued samples", driver.discarded_pcm, 0);
    expect("float output disposes every allocation", driver.disposals, driver.creates);

    const auto short_data = data_for(1);
    TempFile short_file;
    wavplay::playback::TrackPlayer short_track(short_file.write(wave_for(short_data, 1)));
    DacQueueAsio short_driver(DacQueueAsio::Position::advancing, output.type);
    short_driver.latency = 193;
    short_driver.preroll_switches = 5;
    auto short_done = start(short_driver, short_track, 0ms);
    expect("one-frame float output completes", static_cast<int>(finish(short_driver, short_done)),
        static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_raw_dac_run(short_driver, 0, short_data.payload, channels, source_bits, source_is_float32, 0, 1, 0);
    expect("one-frame float output retains source length", short_track.position_frames(), 1);
    expect("one-frame float output drains the last sample", short_driver.discarded_pcm, 0);
}

} // namespace

// Output types beyond the LSB integer outputs of track_player and
// track_source_formats, through the same modeled DAC queue.
int wavplay_test::run_track_output_types_tests()
{
    TestSuite suite;
    // Cover each full-width BE and low-valid-bit category through the device's
    // independent latency queue, beyond the GUI's submitted-buffer checks.
    for (const auto type : {ASIOSTInt16MSB, ASIOSTInt24MSB, ASIOSTInt32MSB,
             ASIOSTInt32LSB16, ASIOSTInt32MSB18, ASIOSTInt32LSB20, ASIOSTInt32MSB24}) {
        const auto output = integer_output_case(type);
        const auto bits = static_cast<std::uint8_t>(output.valid_bits < 24 ? 16 : output.valid_bits);
        const bool extensible = type == ASIOSTInt32MSB24;
        const auto name = "ASIO integer type " + std::to_string(type);
        const auto pause_name = name + " pause/resume preserves every DAC byte";
        suite.run_timed(pause_name.c_str(), [type, bits, extensible] {
            for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
                test_pause_boundary(DacQueueAsio::Position::advancing, type, 0, true, 1, 73, bits, channels, extensible);
                test_pause_boundary(DacQueueAsio::Position::zero, type, 3 * DacQueueAsio::block, false, 0, 193, bits, channels, extensible);
                test_pause_boundary(DacQueueAsio::Position::unsupported, type, 7 * DacQueueAsio::block, true, 5,
                    DacQueueAsio::block - 1, bits, channels, extensible);
            }
        });
        const auto stop_name = name + " Stop and replay preserve the DAC prefix and full track";
        suite.run_timed(stop_name.c_str(), [type, bits, extensible] { test_stop_boundaries(bits, extensible, {type}); });
        const auto eof_name = name + " short EOF drains every DAC byte";
        suite.run_timed(eof_name.c_str(), [type, bits, extensible] { test_short_endings(bits, extensible, {type}); });
    }
    for (const auto output : float_output_cases) {
        const auto name = "Float32 raw DAC timeline to float type " + std::to_string(output.type);
        suite.run_timed(name.c_str(), [output] { test_float_dac_output(output, 32, true, output.big_endian); });
    }
    struct IntegerFloatCase { ASIOSampleType type; std::uint8_t bits; bool extensible; };
    constexpr IntegerFloatCase integer_float_cases[]{
        {ASIOSTFloat32LSB, 16, false}, {ASIOSTFloat32MSB, 24, false},
        {ASIOSTFloat64LSB, 24, true}, {ASIOSTFloat64MSB, 32, false}};
    for (const auto sample : integer_float_cases) {
        const auto name = std::string(sample.extensible ? "Extensible " : "") + "PCM" + std::to_string(sample.bits) +
            " raw DAC timeline to float type " + std::to_string(sample.type);
        suite.run_timed(name.c_str(), [sample] {
            test_float_dac_output(float_output_case(sample.type), sample.bits, false, sample.extensible);
        });
    }
    return suite.result("Single-track ASIO output types through the DAC");
}
