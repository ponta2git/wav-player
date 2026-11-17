#pragma once

#include "doubles/dac_queue_asio.h"
#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_builder.h"
#include "wavplay/playback/track_player.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>

// Shared by the suites that play one TrackPlayer through the modeled DAC queue
// (doubles/dac_queue_asio.h) and compare what it heard with the source WAV.
namespace wavplay_test {

// Each play() runs in a session of its own, on the thread that owns its STA.
inline wavplay::playback::PlaybackResult play_in_session(DacQueueAsio& driver, wavplay::playback::TrackPlayer& playback,
    std::chrono::milliseconds silence = {})
{
    MockSession session(driver, {.start_silence = silence});
    return playback.play(session);
}

inline TestThread<wavplay::playback::PlaybackResult> start(DacQueueAsio& driver, wavplay::playback::TrackPlayer& playback,
    std::chrono::milliseconds silence = std::chrono::milliseconds(3))
{
    using namespace std::chrono_literals;
    driver.started = std::promise<void>{};
    auto started = driver.started.get_future();
    TestThread<wavplay::playback::PlaybackResult> done([&driver, &playback, silence] {
        return play_in_session(driver, playback, silence);
    });
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    while (started.wait_for(0ms) != std::future_status::ready || !wavplay::playback::is_streaming()) {
        if (done.wait_for(0ms) == std::future_status::ready) {
            return done; // Preserve an exception or a completed, already-finished track.
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            abandon_hung("Pause test did not start");
        }
        std::this_thread::yield();
    }
    return done;
}

inline wavplay::playback::PlaybackResult finish(DacQueueAsio& driver, TestThread<wavplay::playback::PlaybackResult>& done,
    unsigned ticks_per_wait = 1)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    while (done.wait_for(0ms) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= deadline) {
            abandon_hung("Pause test did not stop");
        }
        for (unsigned tick = 0; tick < ticks_per_wait; ++tick) {
            driver.advance_dac_block();
        }
        std::this_thread::sleep_for(1ms); // Let the producer and real-time final queue drain progress.
    }
    return done.get();
}

inline void submit_until(DacQueueAsio& driver, std::uint64_t frames)
{
    for (unsigned tick = 0; driver.source_submitted() < frames; ++tick) {
        if (tick == 1000) {
            throw std::runtime_error("Mock driver did not submit the requested frame");
        }
        driver.advance_dac_block();
    }
}

inline void check_heard(const DacQueueAsio& driver, const Bytes& pcm, std::size_t frames,
    std::size_t start_silence_frames = 3 * DacQueueAsio::block, bool allow_discarded = false,
    std::size_t source_bytes = 2, std::uint8_t channels = 2)
{
    expect("frames actually heard", driver.heard.size(), frames);
    const auto bytes = driver.sample_bytes();
    const auto& format = integer_output_case(driver.left_type);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto source_channel = channels == 1 ? 0 : channel;
            const auto expected = expected_integer_sample(pcm.data() + (frame * channels + source_channel) * source_bytes,
                static_cast<std::uint8_t>(source_bytes * 8), format);
            for (std::size_t byte = 0; byte < bytes; ++byte) {
                const auto actual = driver.heard[frame][channel][byte];
                if (actual != expected[byte]) {
                    expect("heard frame " + std::to_string(frame) + " channel " + std::to_string(channel) +
                        " byte " + std::to_string(byte), actual, expected[byte]);
                }
            }
        }
    }
    if (!allow_discarded) {
        expect("unplayed PCM discarded by stop", driver.discarded_pcm, 0);
    }
    expect("DAC queue starvation", driver.starved, 0);
    expect("sample position API dependency", driver.position_queries, 0);
    for (const auto& run : driver.dac_runs) {
        bool audio{}, trailing_silence{};
        std::size_t leading_silence{};
        for (const auto& frame : run) {
            if (DacQueueAsio::silent_frame(frame)) {
                if (audio) {
                    trailing_silence = true;
                } else {
                    ++leading_silence;
                }
            } else {
                expect("no silent hole within a resumed stream", static_cast<int>(trailing_silence), 0);
                audio = true;
            }
        }
        if (audio) {
            expect("start silence on each stream", leading_silence, start_silence_frames);
        }
    }
}

inline wavplay::playback::PlaybackResult finish_without_callbacks(TestThread<wavplay::playback::PlaybackResult>& done)
{
    using namespace std::chrono_literals;
    // A user Stop must wake the control thread even if no further buffer switch
    // arrives. A watchdog fault is not a successful user Stop.
    if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
        abandon_hung("User Stop required another ASIO callback");
    }
    return done.get();
}

inline void clear_capture(DacQueueAsio& driver)
{
    // Called only after play() returned and joined all writers. Each restart is
    // compared independently; a user Stop may intentionally discard its tail.
    driver.heard.clear();
    driver.dac_runs.clear();
    driver.discarded_pcm = 0;
    driver.starved = 0;
}

inline Chunk source_ramp(std::uint8_t channels, std::size_t frames, std::uint8_t source_bits)
{
    return source_bits == 16 ? pcm16_ramp(channels, frames, 0x1234) : source_bits == 24 ?
        pcm24_ramp(channels, frames, 0x7fff80) : pcm32_ramp(channels, frames, 0x7fffff80);
}

inline Bytes source_wave(std::uint8_t channels, std::size_t frames, std::uint8_t source_bits, bool extensible24_in32)
{
    // DAC expectations always come from source_ramp's packed valid bytes.
    // Only the actual file stores PCM24 in a wider, MSB-aligned container.
    return extensible24_in32 ? wave({extensible_pcm(channels, 48000, 24, 32), pcm24_in32_ramp(channels, frames, 0x7fff80)}) :
        wave({pcm_integer(channels, 48000, source_bits), source_ramp(channels, frames, source_bits)});
}

inline void test_stop_boundaries(std::uint8_t source_bits = 16, bool extensible24_in32 = false,
    std::initializer_list<ASIOSampleType> output_types = {ASIOSTInt16LSB, ASIOSTInt24LSB, ASIOSTInt32LSB})
{
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = source_ramp(2, frames, source_bits);
    for (const auto position : {DacQueueAsio::Position::advancing, DacQueueAsio::Position::zero, DacQueueAsio::Position::unsupported}) {
        for (const auto type : output_types) {
            if (source_bits > integer_output_case(type).valid_bits) { continue; }
            for (const std::uint64_t boundary : {0ULL, 3ULL * DacQueueAsio::block, 7ULL * DacQueueAsio::block,
                     static_cast<unsigned long long>(frames)}) {
                TempFile file;
                wavplay::playback::TrackPlayer playback(file.write(source_wave(2, frames, source_bits, extensible24_in32)));
                DacQueueAsio driver(position, static_cast<ASIOSampleType>(type));
                auto done = start(driver, playback);
                submit_until(driver, boundary);
                playback.request_stop();
                expect("user Stop result without another callback", static_cast<int>(finish_without_callbacks(done)),
                    static_cast<int>(wavplay::playback::PlaybackResult::stopped));
                expect("Stop rewinds confirmed position", playback.position_frames(), 0);
                expect("Stop called driver stop once", driver.stops, 1);
                expect("Stop disposed buffers once", driver.disposals, 1);
                expect("Stop output is a bounded WAV prefix", static_cast<int>(driver.heard.size() <= frames), 1);
                check_heard(driver, data.payload, driver.heard.size(), 3 * DacQueueAsio::block, true, source_bits / 8);
                const auto heard = driver.heard.size();
                driver.advance_dac_block();
                expect("stopped driver emits no further samples", driver.heard.size(), heard);
                clear_capture(driver);
                done = start(driver, playback);
                expect("Play after Stop completes from the beginning", static_cast<int>(finish(driver, done)),
                    static_cast<int>(wavplay::playback::PlaybackResult::completed));
                expect("replayed position reaches EOF", playback.position_frames(), frames);
                expect("restart has a fresh ASIO allocation", driver.creates, 2);
                expect("restart disposes its ASIO allocation", driver.disposals, 2);
                check_heard(driver, data.payload, frames, 3 * DacQueueAsio::block, false, source_bits / 8);
            }
        }
    }
}

inline void test_pause_boundary(DacQueueAsio::Position position, ASIOSampleType output_type, std::uint64_t boundary, bool lead_in = true,
    unsigned preroll = 1, long latency = 73, std::uint8_t source_bits = 16, std::uint8_t channels = 2,
    bool extensible24_in32 = false)
{
    using namespace std::chrono_literals;
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = source_ramp(channels, frames, source_bits);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(source_wave(channels, frames, source_bits, extensible24_in32)));
    DacQueueAsio driver(position, output_type);
    driver.preroll_switches = preroll;
    driver.latency = latency;
    const auto silence = lead_in ? 3ms : 0ms;
    const std::size_t silent_frames = lead_in ? 3 * DacQueueAsio::block : 0;
    auto done = start(driver, playback, silence);
    submit_until(driver, boundary);
    playback.request_pause();
    expect("pause result", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::paused));
    expect("retained source frame", playback.position_frames(), boundary);
    expect("pause stopped the driver", driver.stops, 1);
    expect("pause disposed the buffers", driver.disposals, 1);
    check_heard(driver, data.payload, static_cast<std::size_t>(boundary), silent_frames, false, source_bits / 8, channels);
    const auto heard = driver.heard.size();
    driver.advance_dac_block();
    expect("paused device does not emit", driver.heard.size(), heard);
    done = start(driver, playback, silence);
    expect("resume result", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("final retained frame", playback.position_frames(), frames);
    expect("resume re-created buffers", driver.creates, 2);
    expect("resume re-started driver", driver.starts, 2);
    expect("all buffers disposed", driver.disposals, 2);
    check_heard(driver, data.payload, frames, silent_frames, false, source_bits / 8, channels);
    expect("completed replay is a no-op", static_cast<int>(play_in_session(driver, playback)), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("completed replay did not start", driver.starts, 2);
}

inline void test_short_endings(std::uint8_t source_bits = 16, bool extensible24_in32 = false,
    std::initializer_list<ASIOSampleType> output_types = {ASIOSTInt16LSB, ASIOSTInt24LSB, ASIOSTInt32LSB})
{
    using namespace std::chrono_literals;
    struct Case {
        std::size_t frames;
        unsigned preroll;
        long latency;
        std::chrono::milliseconds silence;
    };
    constexpr Case cases[]{
        {1, 0, 0, 0ms},
        {DacQueueAsio::block - 1, 1, DacQueueAsio::block - 1, 0ms},
        {DacQueueAsio::block, 3, 73, 0ms},
        {DacQueueAsio::block + 1, 5, 73, 0ms},
        {3 * DacQueueAsio::block + 11, 3, 193, 3ms},
        {1, 0, 193, 3ms},
    };
    for (const auto type : output_types) {
        if (source_bits > integer_output_case(type).valid_bits) { continue; }
        for (const auto& sample : cases) {
            const auto data = source_ramp(2, sample.frames, source_bits);
            TempFile file;
            wavplay::playback::TrackPlayer playback(file.write(source_wave(2, sample.frames, source_bits, extensible24_in32)));
            DacQueueAsio driver(DacQueueAsio::Position::zero, static_cast<ASIOSampleType>(type));
            driver.preroll_switches = sample.preroll;
            driver.latency = sample.latency;
            auto done = start(driver, playback, sample.silence);
            expect("short track completes", static_cast<int>(finish(driver, done)),
                static_cast<int>(wavplay::playback::PlaybackResult::completed));
            expect("short track final position", playback.position_frames(), sample.frames);
            check_heard(driver, data.payload, sample.frames, sample.silence == 0ms ? 0 : 3 * DacQueueAsio::block,
                false, source_bits / 8);
        }
    }
}

} // namespace wavplay_test
