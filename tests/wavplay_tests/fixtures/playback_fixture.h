#pragma once

#include "wavplay/playback/play_tracks.h"
#include "wavplay/playback/track_player.h"
#include "doubles/mock_asio.h"
#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_builder.h"

#include <chrono>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wavplay_test {

inline Bytes samples(unsigned frames, unsigned first = 1)
{
    Bytes bytes;
    for (unsigned i = 0; i < frames; ++i) {
        const auto n = first + i;
        bytes.insert(bytes.end(), {static_cast<std::uint8_t>(n), 0x12,
            static_cast<std::uint8_t>(0xff - n), 0x89});
    }
    return bytes;
}

// One request on this thread, in a session of its own. Closing the session
// restores a clock the request changed.
inline wavplay::playback::PlaybackResult play_in_session(MockAsio& driver, wavplay::stream::TrackInput first,
    wavplay::stream::TrackInput second = {})
{
    MockSession session(driver);
    return wavplay::playback::play_tracks(session, std::move(first), std::move(second));
}

// Plays one request in a session of its own, opened on the playing thread.
inline TestThread<wavplay::playback::PlaybackResult> start_track(MockAsio& driver, const wchar_t* path, const wchar_t* next = nullptr)
{
    auto started = driver.started.get_future();
    TestThread<wavplay::playback::PlaybackResult> done([&driver, path, next] {
        MockSession session(driver);
        return wavplay::playback::play_tracks(session, path, next);
    });
    if (started.wait_for(hang_budget(std::chrono::seconds(5))) != std::future_status::ready &&
        done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        abandon_hung("Playback timed out before start returned");
    }
    while (!wavplay::playback::is_streaming() &&
        done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        std::this_thread::yield();
    }
    return done;
}

inline wavplay::playback::PlaybackResult run_to_tick(MockAsio& driver, TestThread<wavplay::playback::PlaybackResult> done, long final_tick)
{
    const long width = driver.buffer_size.preferred;
    for (long t = width; t <= final_tick; t += width) {
        driver.tick();
    }
    if (done.wait_for(hang_budget(std::chrono::seconds(5))) != std::future_status::ready) {
        // The control thread is blocked and cannot be joined.
        abandon_hung("Playback did not stop at mock frame " + std::to_string(final_tick) +
            " (latency " + std::to_string(driver.latency) + ")");
    }
    return done.get();
}

inline void check_pcm16_capture(std::span<const MockAsio::Block> submitted, std::size_t width,
    std::span<const std::uint8_t> pcm, std::size_t sample_bytes = 2)
{
    // The PCM16 input word occupies the two most significant bytes. Compare the
    // entire driver buffer, including the low byte of packed 24-bit samples,
    // the tail padding and every extra buffer sent while the device drains.
    expect_true("PCM capture block width is positive", width != 0);
    expect_true("PCM capture sample width holds PCM16", sample_bytes >= 2 && sample_bytes <= 4);
    expect_true("PCM input contains complete stereo frames", pcm.size() % 4 == 0);
    const auto frames = pcm.size() / 4;
    expect_true("PCM capture covers every source frame", submitted.size() >= frames / width + (frames % width != 0));
    for (std::size_t block = 0; block < submitted.size(); ++block) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto& channel = submitted[block][ch];
            expect("submitted channel bytes", channel.size(), width * sample_bytes);
            for (std::size_t slot = 0; slot < width; ++slot) {
                const auto frame = block * width + slot;
                const auto offset = frame * 4 + ch * 2;
                for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                    const auto low_padding = sample_bytes - 2;
                    const auto wanted = offset < pcm.size() && byte >= low_padding ?
                        pcm[offset + byte - low_padding] : 0;
                    const auto actual = channel[slot * sample_bytes + byte];
                    if (actual != wanted) {
                        expect("frame " + std::to_string(frame) + " channel " + std::to_string(ch) +
                            " byte " + std::to_string(byte), actual, wanted);
                    }
                }
            }
        }
    }
}

inline void check_submitted_pcm(std::span<const MockAsio::Block> submitted, long width,
    std::span<const std::uint8_t> pcm, long safe_tick, std::size_t sample_bytes = 2)
{
    expect_true("submitted block width is positive", width > 0);
    expect("submitted blocks including prefilled B", submitted.size(), 2 + safe_tick / width);
    check_pcm16_capture(submitted, static_cast<std::size_t>(width), pcm, sample_bytes);
}

// The session's start silence precedes the first PCM block of every stream start.
// Everything after it is the exact PCM sequence of a run without silence.
inline void check_silence_then_pcm(std::span<const MockAsio::Block> submitted, std::size_t silent_blocks,
    std::span<const std::uint8_t> pcm, long width)
{
    if (submitted.size() < silent_blocks + 2) {
        throw std::runtime_error("Too few submitted blocks after the start silence");
    }
    for (std::size_t i = 0; i < silent_blocks; ++i) {
        expect("start silence block (0 is prefilled B)", static_cast<int>(MockAsio::silent(submitted[i])), 1);
    }
    const auto audio = submitted.subspan(silent_blocks);
    check_submitted_pcm(audio, width, pcm, static_cast<long>(audio.size() - 2) * width, 3);
}

inline void check_fault(const wchar_t* path, long latency, long tick, long selector,
    const wchar_t* next = nullptr, ASIOSampleRate rate = 48000)
{
    MockAsio driver;
    driver.sample_rate = rate;
    driver.latency = latency;
    driver.fault_at = tick;
    driver.fault_selector = selector;
    const auto result = run_to_tick(driver, start_track(driver, path, next), tick);
    expect("still playing when fault was injected", static_cast<int>(driver.state_before_fault.value()),
        static_cast<int>(wavplay::playback::PlaybackResult::playing));
    // MockAsio follows each injected notification with overload. That fatal
    // fault supersedes reinitialization, but never replaces another fatal fault.
    const auto expected = selector == kAsioOverload || selector == kAsioResetRequest || selector == kAsioBufferSizeChange ?
        wavplay::playback::PlaybackResult::underrun :
        wavplay::playback::PlaybackResult::driver_error;
    const bool supported = selector == kAsioOverload || selector == kAsioResetRequest;
    expect("fault selector " + std::to_string(selector) + " supported", driver.fault_supported, static_cast<long>(supported));
    if (selector != kAsioOverload) {
        expect("notification acknowledged", driver.fault_reply, static_cast<long>(supported));
    }
    expect("first fatal stop reason", static_cast<int>(result), static_cast<int>(expected));
    expect("stops after fault", driver.stops, 1);
    expect("disposals after fault", driver.disposals, 1);
    expect("next rate changes after fault", driver.set_rates.size(), 0);
    expect("silence after fault", static_cast<int>(MockAsio::silent(driver.submitted.back())), 1);
    expect("silence during stop after fault", static_cast<int>(driver.silent_on_stop), 1);
}

inline std::string expect_rejected(MockAsio& driver, const wchar_t* path)
{
    try {
        MockSession session(driver);
        (void)wavplay::playback::play_tracks(session, path);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw std::runtime_error("expected playback to reject this setup");
}

} // namespace wavplay_test
