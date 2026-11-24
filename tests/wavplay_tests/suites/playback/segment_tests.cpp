#include "suites/suites.h"
#include "wavplay/playback/playback_signal.h"
#include "wavplay/playback/segment_renderer.h"
#include "fixtures/call_gate.h"
#include "fixtures/image_residency.h"
#include "fixtures/playback_fixture.h"
#include "fixtures/ring_fixture.h"
#include "framework/supervisor.h"
#include "wavplay/core/memory.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/stream/ring_layout.h"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;

void play_and_check(MockAsio& driver, const wchar_t* path, const Bytes& pcm, long safe_tick,
    const wchar_t* next = nullptr, wavplay::playback::PlaybackResult expected = wavplay::playback::PlaybackResult::completed,
    std::size_t sample_bytes = 2)
{
    const auto creates = driver.creates, starts = driver.starts, stops = driver.stops;
    const auto result = run_to_tick(driver, start_track(driver, path, next), safe_tick);
    expect("playback result", static_cast<int>(result), static_cast<int>(expected));
    expect("stop frame on the mock device clock", driver.stopped_at, safe_tick);
    expect("createBuffers calls", driver.creates, creates + 1);
    expect("start calls", driver.starts, starts + 1);
    expect("stop calls", driver.stops, stops + 1);
    expect("dispose calls", driver.disposals, driver.creates);
    expect("rate changes within a single-rate run", driver.set_rates.size(), 0);
    expect("unsupported buffer-size change", driver.size_change_supported, 0);
    expect("other half untouched", static_cast<int>(driver.peer_untouched), 1);
    expect("first A silent before start", static_cast<int>(driver.first_a_silent), 1);
    expect("silence during stop", static_cast<int>(driver.silent_on_stop), 1);
    check_submitted_pcm(driver.submitted, driver.buffer_size.preferred, pcm, safe_tick, sample_bytes);
}

void test_track(unsigned frames, long latency, long safe_tick)
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(frames)}}));
    // A hardware dropout before the final switch must interrupt playback.
    check_fault(path, latency, safe_tick - MockAsio::width, kAsioOverload);
    MockAsio driver;
    driver.latency = latency;
    play_and_check(driver, path, samples(frames), safe_tick);
}

void test_gapless(unsigned first_frames, unsigned next_frames, long safe_tick)
{
    TempFile first, second;
    auto pcm = samples(first_frames);
    const auto next_pcm = samples(next_frames, 0x61);
    const auto* first_path = first.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const auto* next_path = second.write(wave({pcm16(2, 48000), {"data", next_pcm}}));
    pcm.append_range(next_pcm);
    MockAsio driver;
    check_fault(first_path, driver.latency, safe_tick - MockAsio::width, kAsioOverload, next_path);
    play_and_check(driver, first_path, pcm, safe_tick, next_path);
}

void test_packed24_gapless()
{
    // Use a 544-frame block at 48 kHz. The join moves from the last slot in B
    // to the start of A and then one slot inside A.
    constexpr long width = 544;
    for (const auto first_frames : {width - 1, width, width + 1}) {
        TempFile first, second;
        auto first_pcm = pcm16_ramp(2, first_frames, 0x7ffe);
        const auto second_pcm = pcm16_ramp(2, 2 * width + 1 - first_frames, 0xfffe);
        const auto* first_path = first.write(wave({pcm16(2, 48000), first_pcm}));
        const auto* next_path = second.write(wave({pcm16(2, 48000), second_pcm}));
        first_pcm.payload.append_range(second_pcm.payload);
        MockAsio driver;
        driver.buffer_size = {width, width, width, 0};
        driver.latency = width;
        driver.left_type = driver.right_type = ASIOSTInt24LSB;
        play_and_check(driver, first_path, first_pcm.payload, 3 * width, next_path,
            wavplay::playback::PlaybackResult::completed, 3);
    }
}

void test_mono_gapless()
{
    // Mono reaches both outputs unchanged, and a same-rate stereo WAV joins it one
    // frame into a 544-frame block with 16-bit and packed 24-bit outputs.
    constexpr long width = 544;
    for (const long type : {ASIOSTInt16LSB, ASIOSTInt24LSB}) {
        TempFile first, second;
        const auto mono = pcm16_ramp(1, width + 1, 0x7ffe);
        const auto stereo = pcm16_ramp(2, width, 0xfffe);
        const auto* first_path = first.write(wave({pcm16(1, 48000), mono}));
        const auto* next_path = second.write(wave({pcm16(2, 48000), stereo}));
        Bytes heard;
        for (std::size_t i = 0; i < mono.payload.size(); i += 2) {
            heard.insert(heard.end(), {mono.payload[i], mono.payload[i + 1], mono.payload[i], mono.payload[i + 1]});
        }
        heard.append_range(stereo.payload);
        MockAsio driver;
        driver.buffer_size = {width, width, width, 0};
        driver.latency = width;
        driver.left_type = driver.right_type = type;
        play_and_check(driver, first_path, heard, 3 * width, next_path, wavplay::playback::PlaybackResult::completed,
            type == ASIOSTInt24LSB ? 3 : 2);
    }
}

void test_duplicate_track()
{
    TempFile file;
    const auto data = samples(3);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", data}}));
    auto pcm = data;
    pcm.append_range(data);
    MockAsio driver;
    play_and_check(driver, path, pcm, 8, path);
}

void test_owned_tracks()
{
    TempFile first, next;
    auto pcm = samples(3);
    const auto second_pcm = samples(2, 0x76);
    const auto* first_path = first.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const auto* second_path = next.write(wave({pcm16(2, 48000), {"data", second_pcm}}));
    pcm.append_range(second_pcm);
    MockAsio driver;
    driver.ticks_after_start = {3};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::create) {
            // Both short files are fully buffered. No duplicate validation
            // handles should pin either file throughout the audition.
            expect("first handle released after priming", probe_file_access(first_path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            expect("second handle released after priming", probe_file_access(second_path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        }
    };
    const auto result = play_in_session(driver, wavplay::media::WavReader(first_path), wavplay::media::WavReader(second_path));
    expect("play from validated handles", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_submitted_pcm(driver.submitted, driver.width, pcm, 6);
}

void test_rejected_next_track()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second;
    const auto pcm = samples(3);
    const auto* path = first.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const auto check = [&](const Bytes& wav, wavplay::playback::PlaybackResult expected) {
        const auto* next = second.write(wav);
        MockAsio driver;
        play_and_check(driver, path, pcm, 4, next, expected);
        return next;
    };
    // Three channels are unsupported at the same rate and at another rate.
    check(wave({pcm16(3, 48000), {"data", {1, 2, 3, 4, 5, 6}}}), next_track_rejected);
    check(wave({pcm16(3, 44100), {"data", {1, 2, 3, 4, 5, 6}}}), next_track_rejected);
    check(wave({pcm16(2, 48000), {"data", {}}}), next_track_rejected);
    const auto* malformed = check(wave({pcm16(2, 48000)}), next_track_rejected);
    // While the first track is draining, the pending rejection must not mask a driver fault.
    check_fault(path, 3, 2, kAsioOverload, malformed);
}

void test_extensible_next_scan()
{
    using enum wavplay::playback::PlaybackResult;
    constexpr long width = 544;
    constexpr std::size_t first_frames = 3;
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const ASIOSampleType output : {ASIOSTInt24LSB, ASIOSTInt32LSB}) {
            for (const bool invalid : {false, true}) {
                for (const auto rate : {48000U, 44100U}) {
                    if (!invalid && rate != 48000) {
                        continue; // Valid different-rate runs have their own contract tests.
                    }
                    TempFile first, second;
                    const auto* path = first.write(wave({pcm24(2, 48000), pcm24_ramp(2, first_frames, 0x7ffffe)}));
                    // Valid data crosses a 64 KiB scan-read boundary
                    // (WavReader::validation_bytes) but fits in the initial ring,
                    // making callback timing deterministic. Invalid
                    // data extends beyond that ring, so checking only its prefix
                    // or waiting for the track boundary cannot pass this test.
                    const std::size_t next_frames = invalid ? 132303 : 20003;
                    auto data = pcm24_in32_ramp(channels, next_frames, 0x800010);
                    if (invalid) {
                        data.payload[data.payload.size() - 4] = 0x80;
                    }
                    const auto* next = second.write(wave({extensible_pcm(channels, rate, 24, 32), data}));
                    MockAsio driver;
                    driver.buffer_size = {width, width, width, 0};
                    driver.latency = width;
                    driver.left_type = driver.right_type = output;
                    driver.on_call = [&](MockAsio::Call call) {
                        if (call == MockAsio::Call::create) {
                            expect("short first track finished priming", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
                            expect("next track validated and buffered or rejected before create",
                                probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
                        }
                    };
                    const auto total_frames = first_frames + (invalid ? 0 : next_frames);
                    const auto safe_tick = static_cast<long>((total_frames + width - 1) / width * width);
                    const auto result = run_to_tick(driver, start_track(driver, path, next), safe_tick);
                    expect("Extensible next-track outcome", static_cast<int>(result),
                        static_cast<int>(invalid ? next_track_rejected : completed));
                    expect("Extensible next track creates once", driver.creates, 1);
                    expect("Extensible next track starts once", driver.starts, 1);
                    expect("Extensible next track stops once", driver.stops, 1);
                    expect("Extensible next track disposes once", driver.disposals, 1);
                    expect("Extensible invalid next never changes rate", driver.set_rates.size(), 0);
                    expect("Extensible next stops at drained boundary", driver.stopped_at, safe_tick);
                    expect("Extensible next submitted block count", driver.submitted.size(), 2 + safe_tick / width);
                    const std::size_t sample_bytes = output == ASIOSTInt32LSB ? 4 : 3;
                    for (std::size_t block = 0; block < driver.submitted.size(); ++block) {
                        for (std::size_t channel = 0; channel < 2; ++channel) {
                            const auto& submitted = driver.submitted[block][channel];
                            expect("Extensible next output width", submitted.size(), width * sample_bytes);
                            for (std::size_t slot = 0; slot < static_cast<std::size_t>(width); ++slot) {
                                const auto frame = block * width + slot;
                                std::uint32_t bits{};
                                if (frame < total_frames) {
                                    bits = static_cast<std::uint32_t>(frame < first_frames ?
                                        0x7ffffe + frame : 0x800010 + frame - first_frames);
                                    if (channel == 1 && (frame < first_frames || channels == 2)) {
                                        bits = ~bits;
                                    }
                                    bits &= 0xffffff;
                                }
                                for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                                    const auto padding = sample_bytes - 3;
                                    const auto expected = byte < padding ? 0 :
                                        static_cast<std::uint8_t>(bits >> (8 * (byte - padding)));
                                    expect("Extensible next every output byte and tail", submitted[slot * sample_bytes + byte], expected);
                                }
                            }
                        }
                    }
                    expect("Extensible next stop is silent", static_cast<int>(driver.silent_on_stop), 1);
                    expect("Extensible next handle released", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
                }
            }
        }
    }
}

void check_rejected_buffers(MockAsio& driver, const wchar_t* path)
{
    const auto before = driver.creates;
    expect_rejected(driver, path);
    expect("buffers created before rejection", driver.creates, before + 1);
    expect("start after invalid buffer setup", driver.starts, 0);
    expect("disposals after invalid buffer setup", driver.disposals, driver.creates);
    expect("buffers untouched at rejection", static_cast<int>(driver.untouched_on_dispose), 1);
}

void test_session_lifetime()
{
    // A second session cannot touch the driver while a request owns the callbacks,
    // even while the active driver keeps notifying from another thread.
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(5)}}));
    MockAsio driver, contender;
    auto done = start_track(driver, path);
    if (!driver.rate_notification) {
        done.get(); // Surface a setup exception.
        throw std::runtime_error("missing ASIO rate notification");
    }
    std::promise<void> first;
    std::jthread notifications([&first, notify = driver.rate_notification](std::stop_token stop) {
        notify(48000);
        first.set_value();
        while (!stop.stop_requested()) {
            notify(48000);
        }
        notify(48000); // One more notification after the control call returned.
    });
    if (first.get_future().wait_for(hang_budget(5s)) != std::future_status::ready) {
        abandon_hung("Rate notification did not return");
    }
    contender.fail_start = true;
    expect_rejected(contender, path);
    const auto result = run_to_tick(driver, std::move(done), 6);
    notifications.request_stop();
    notifications.join();
    expect("second session touched the driver", contender.size_queries, 0);
    expect("completion with concurrent notifications", static_cast<int>(result),
        static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("disposals with concurrent notifications", driver.disposals, 1);
}

void test_preferred_buffer_size()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    using Size = MockAsio::BufferSize;
    for (const auto size : {Size{1, 9, 4, 2}, Size{2, 16, 6, -1},
                            Size{2, 8, 4, -2}, Size{0, 8, 4, 1}}) {
        MockAsio driver;
        driver.buffer_size = size;
        driver.fail_start = true; // Keep a regression from waiting for callbacks.
        expect_rejected(driver, path);
        expect("buffers created for an inconsistent preferred size", driver.creates, 0);
        expect("starts for an inconsistent preferred size", driver.starts, 0);
    }
    for (const auto size : {Size{3, 3, 3, 0}, Size{1, 9, 3, 2}, Size{2, 16, 8, -1}}) {
        MockAsio driver;
        driver.buffer_size = size;
        driver.ticks_after_start = {1};
        const auto result = play_in_session(driver, path);
        expect("valid preferred buffer size", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        expect("negotiated block width", driver.runs.at(0).width, size.preferred);
    }
}

void test_prepare_and_retry()
{
    TempFile file;
    MockAsio driver;
    driver.fail_start = true;
    expect_rejected(driver, file.write(wave({pcm16(2, 48000), {"data", {}}})));
    expect("start for an empty track", driver.starts, 0);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(5)}}));
    for (const auto types : {std::pair{ASIOSTInt16LSB, ASIOSTInt24LSB},
            std::pair{ASIOSTInt24LSB, ASIOSTInt16LSB}, std::pair{ASIOSTDSDInt8MSB1, ASIOSTDSDInt8MSB1},
            std::pair{ASIOSTDSDInt8LSB1, ASIOSTDSDInt8LSB1}}) {
        driver.left_type = types.first;
        driver.right_type = types.second;
        expect_rejected(driver, path);
        expect("createBuffers calls for an incompatible pair",
            std::ranges::count(driver.calls, MockAsio::Call::create), 0);
    }
    driver.left_type = driver.right_type = ASIOSTInt16LSB;
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::create) {
            // Both channels still agree, but the ring was prepared for the old type.
            driver.left_type = driver.right_type = ASIOSTInt24LSB;
        }
    };
    check_rejected_buffers(driver, path);
    driver.on_call = {};
    driver.left_type = driver.right_type = ASIOSTInt16LSB;
    using Fault = MockAsio::BufferFault;
    for (const auto fault : {Fault::overlap, Fault::changed_type, Fault::changed_rate,
                             Fault::swapped_channels, Fault::input_buffer}) {
        driver.buffer_fault = fault;
        check_rejected_buffers(driver, path);
    }
    driver.left_type = driver.right_type = ASIOSTInt24LSB;
    driver.buffer_fault = Fault::overlap; // Only the sixth byte of the two-frame half overlaps.
    check_rejected_buffers(driver, path);
    driver.left_type = driver.right_type = ASIOSTInt16LSB;
    driver.buffer_fault = Fault::none;
    expect_rejected(driver, path);
    expect("failed starts", driver.starts, 1);
    expect("buffers disposed after failed start", driver.disposals, driver.creates);
    expect("stop after partial start", driver.stops, 1);
    expect("silence while stopping partial start", static_cast<int>(driver.silent_on_stop), 1);
    driver.fail_start = false;
    play_and_check(driver, path, samples(5), 6);
    // One block of audio remains unread when the driver reports the fault.
    for (const long selector : {kAsioOverload, kAsioLatenciesChanged, kAsioResetRequest, kAsioResyncRequest, kAsioBufferSizeChange}) {
        check_fault(path, driver.latency, 0, selector);
    }

    MockAsio stop_failure;
    stop_failure.fail_start = stop_failure.fail_stop = true;
    // The outer code is stop's; the earlier start failure remains its cause.
    if (expect_rejected(stop_failure, path) != "ASIO stop failed after start error: ASE_NotPresent (-1000)") {
        throw std::runtime_error("An ASIO start failure masked the stop failure");
    }
    expect("disposed after both start and stop failed", stop_failure.disposals, 1);

    MockAsio broken;
    broken.fail_start = broken.fail_stop = broken.fail_dispose = true;
    const auto error = expect_rejected(broken, path);
    if (error.find("disposeBuffers") == std::string::npos) {
        throw std::runtime_error("expected disposal error after failed start, got " + error);
    }
    expect("failed disposal attempts", broken.disposals, 1);
    expect("callbacks during failed start and disposal", broken.submitted.size(), 3);
    expect("retiring buffers untouched by a late driver thread", static_cast<int>(broken.late_switch_untouched), 1);
    expect("silence during implicit stop", static_cast<int>(MockAsio::silent(broken.submitted.back())), 1);
    expect("detached callback left output untouched", static_cast<int>(broken.detached_callbacks_preserve_output()), 1);

    // The disposal failure is reported with the failure it interrupted, the stop
    // failure after the start error, kept with its type as the nested cause.
    MockAsio nested;
    nested.fail_start = nested.fail_stop = nested.fail_dispose = true;
    try {
        MockSession session(nested);
        (void)wavplay::playback::play_tracks(session, path);
        fail("expected the failed start, stop and disposal to be rejected");
    } catch (const std::runtime_error& error) {
        expect_true(std::string("the cleanup failure is the outer error: ") + error.what(),
            std::string_view(error.what()) == "ASIO disposeBuffers failed during cleanup: ASE_HWMalfunction (-999)");
        try {
            std::rethrow_if_nested(error);
            fail("the cleanup failure replaced the original failure instead of nesting it");
        } catch (const std::runtime_error& cause) {
            expect_true(std::string("the nested cause is the stop failure: ") + cause.what(),
                std::string_view(cause.what()) == "ASIO stop failed after start error: ASE_NotPresent (-1000)");
            try {
                std::rethrow_if_nested(cause);
                fail("the failed stop lost the original start error");
            } catch (const std::runtime_error& start) {
                expect_true(std::string("the root cause is the start failure: ") + start.what(),
                    std::string_view(start.what()) == "ASIO start failed: ASE_HWMalfunction (-999)");
                // A leaf must not carry an empty nested_exception: following it
                // with rethrow_if_nested would otherwise terminate the process.
                std::rethrow_if_nested(start);
            }
        }
    }
}

void test_preroll(unsigned frames, unsigned preroll, long latency, long stop_frame)
{
    struct PrerollDriver : MockAsio {
        ASIOError start() override
        {
            // Pre-roll can run on a driver thread before ASIOStart returns.
            return std::async(std::launch::async, [this] { return MockAsio::start(); }).get();
        }
    };
    TempFile file;
    const auto pcm = samples(frames);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    PrerollDriver early;
    early.preroll_switches = preroll;
    early.latency = latency;
    early.fault_at = stop_frame - early.width;
    early.ticks_after_start = {static_cast<unsigned>(early.fault_at / early.width)};
    const auto interrupted = play_in_session(early, path);
    expect("still draining before the safe preroll boundary", static_cast<int>(early.state_before_fault.value()),
        static_cast<int>(wavplay::playback::PlaybackResult::playing));
    expect("not drained one physical switch early", static_cast<int>(interrupted), static_cast<int>(wavplay::playback::PlaybackResult::underrun));

    PrerollDriver driver;
    driver.preroll_switches = preroll;
    driver.latency = latency;
    driver.ticks_after_start = {static_cast<unsigned>(stop_frame / driver.width)};
    const auto result = play_in_session(driver, path);
    expect("preroll completion", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("physical frames elapsed before stopping", driver.stopped_at, stop_frame);
    expect("first A initialized before preroll", static_cast<int>(driver.first_a_silent), 1);
    // check_submitted_pcm counts every submitted block, including the extra pre-roll halves.
    const long extra_frames = (static_cast<long>(driver.preroll_switches) - 1) * driver.width;
    check_submitted_pcm(driver.submitted, driver.width, pcm, extra_frames + stop_frame);
}

// After the frame-counted drain, silence continues for two block periods before stop.
void test_driver_queue_play_out()
{
    using Clock = std::chrono::steady_clock;
    constexpr long width = 2400; // 50 ms at 48 kHz.
    TempFile file;
    const auto pcm = samples(1);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    for (const bool overload : {false, true}) {
        MockAsio driver;
        driver.buffer_size = {width, width, width, 0};
        driver.latency = width;
        Clock::time_point stopped{};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::stop) {
                stopped = Clock::now();
            }
        };
        auto done = start_track(driver, path);
        const auto drained = Clock::now();
        driver.tick(); // B held the only frame: this switch completes the counted drain.
        if (overload) {
            driver.message_notification(kAsioOverload, 0, nullptr, nullptr); // Arrives while silence continues.
        }
        if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
            abandon_hung("Playback did not stop after the driver-queue allowance");
        }
        const auto result = done.get();
        expect("result after the allowance", static_cast<int>(result),
            static_cast<int>(overload ? wavplay::playback::PlaybackResult::underrun : wavplay::playback::PlaybackResult::completed));
        expect("stopped at the counted drain frame", driver.stopped_at, width);
        expect("silence during stop", static_cast<int>(driver.silent_on_stop), 1);
        check_submitted_pcm(driver.submitted, width, pcm, width);
        if (!overload) {
            expect("two full block periods before stop", static_cast<int>(stopped - drained >= 100ms), 1);
        }
    }
}

void test_callback_contract()
{
    TempFile file;
    const auto pcm = samples(5);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    MockAsio driver;
    driver.on_call = [&](MockAsio::Call call) {
        if (call != MockAsio::Call::start) {
            return;
        }
        const auto message = driver.message_notification;
        expect("engine version", message(kAsioEngineVersion, 0, nullptr, nullptr), 2);
        for (const long selector : {kAsioEngineVersion, kAsioResetRequest, kAsioOverload}) {
            expect("advertised selector", message(kAsioSelectorSupported, selector, nullptr, nullptr), 1);
        }
        for (const long selector : {kAsioSupportsTimeInfo, kAsioSupportsTimeCode, kAsioBufferSizeChange,
                                    kAsioResyncRequest, kAsioLatenciesChanged}) {
            expect("unadvertised selector", message(kAsioSelectorSupported, selector, nullptr, nullptr), 0);
        }
        expect("classic switch selected", message(kAsioSupportsTimeInfo, 0, nullptr, nullptr), 0);
        expect("time code declined", message(kAsioSupportsTimeCode, 0, nullptr, nullptr), 0);
        expect("unknown message ignored", message(kAsioNumMessageSelectors + 100, 0, nullptr, nullptr), 0);
    };
    play_and_check(driver, path, pcm, 6); // Capability queries must not interrupt streaming.

    for (const long index : {-1, 2, 1, 0}) {
        MockAsio broken;
        broken.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::start) {
                // Illegal index, first B, or a duplicate A when the regular A follows.
                broken.switch_notification(index, ASIOTrue);
            }
        };
        broken.ticks_after_start = {0};
        const auto result = play_in_session(broken, path);
        expect("invalid switch stops playback", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::driver_error));
        expect("no stale samples on following callback", static_cast<int>(MockAsio::silent(broken.submitted.back())), 1);
        expect("invalid switch released buffers", broken.disposals, 1);
    }
}

void test_buffer_allocation_failure()
{
    struct FailingDriver : MockAsio {
        bool fail = true, return_partial{};
        ASIOError cleanup{};
        unsigned cleanup_calls{};
        std::uint16_t witness = 0x953a;

        ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
        {
            if (!fail) {
                return MockAsio::createBuffers(infos, count, size, callbacks);
            }
            if (return_partial) {
                infos[1].buffers[1] = &witness;
            }
            return ASE_NoMemory;
        }
        ASIOError disposeBuffers() override
        {
            if (!fail) {
                return MockAsio::disposeBuffers();
            }
            ++cleanup_calls;
            return cleanup;
        }
    };
    TempFile file;
    const auto pcm = samples(5);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    for (const bool partial : {false, true}) {
        for (const auto cleanup : {ASE_OK, ASE_InvalidMode, ASE_HWMalfunction}) {
            FailingDriver driver;
            driver.return_partial = partial;
            driver.cleanup = cleanup;
            const auto expected = partial && cleanup == ASE_HWMalfunction ?
                "ASIO disposeBuffers failed after createBuffers error: ASE_HWMalfunction (-999)" :
                "ASIO createBuffers failed: ASE_NoMemory (-994)";
            try {
                MockSession session(driver);
                (void)wavplay::playback::play_tracks(session, path);
                fail("expected allocation failure");
            } catch (const std::runtime_error& error) {
                expect_true(std::string("allocation failure: ") + error.what(), std::string_view(error.what()) == expected);
                if (partial && cleanup == ASE_HWMalfunction) {
                    try {
                        std::rethrow_if_nested(error);
                        fail("failed disposal lost the original allocation error");
                    } catch (const std::runtime_error& cause) {
                        expect_true(std::string("allocation remains the root cause: ") + cause.what(),
                            std::string_view(cause.what()) == "ASIO createBuffers failed: ASE_NoMemory (-994)");
                        std::rethrow_if_nested(cause);
                    }
                } else {
                    std::rethrow_if_nested(error); // No empty nested cause on the original failure.
                }
            }
            expect("partial buffer untouched", driver.witness, 0x953a);
            expect("partial buffer cleanup calls", driver.cleanup_calls, partial ? 1 : 0);
            expect("no start after failed creation", driver.starts, 0);
            if (partial && cleanup == ASE_OK) {
                driver.fail = false;
                play_and_check(driver, path, pcm, 6);
            }
        }
    }
}

void test_latency_failure()
{
    struct LatencyDriver : MockAsio {
        ASIOError reply = ASE_OK;
        ASIOError getLatencies(long* input, long* output) override
        {
            const auto prepared = MockAsio::getLatencies(input, output);
            return prepared == ASE_OK ? reply : prepared;
        }
    };
    TempFile file;
    const auto pcm = samples(5);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    for (const auto reply : {ASE_OK, ASE_HWMalfunction}) {
        LatencyDriver driver;
        driver.latency = -1;
        driver.reply = reply;
        const auto error = expect_rejected(driver, path);
        const auto expected = reply == ASE_OK ? "Invalid ASIO output latency" : "ASIO getLatencies failed: ASE_HWMalfunction (-999)";
        if (error != expected) {
            throw std::runtime_error("unexpected latency failure: " + error);
        }
        expect("invalid latency prevented start", driver.starts, 0);
        expect("invalid latency released buffers", driver.disposals, 1);
        expect("no output written with unknown latency", static_cast<int>(driver.untouched_on_dispose), 1);
        driver.latency = 3;
        driver.reply = ASE_OK;
        play_and_check(driver, path, pcm, 6);
    }
    // Pause and EOF wait out the reported latency, so a bogus value would drain for
    // hours while switches keep arriving. Two seconds at the stream rate is accepted.
    for (const long latency : {2L * 48000 + 1, 0x7fffffffL}) {
        MockAsio driver;
        driver.latency = latency;
        const auto error = expect_rejected(driver, path);
        if (error != "Implausible ASIO output latency") {
            throw std::runtime_error("unexpected latency failure: " + error);
        }
        expect("implausible latency prevented start", driver.starts, 0);
        expect("implausible latency released buffers", driver.disposals, 1);
    }
    // Two seconds at the stream rate passes validation and reaches start.
    MockAsio longest;
    longest.latency = 2 * 48000;
    longest.fail_start = true;
    const auto accepted = expect_rejected(longest, path);
    if (accepted != "ASIO start failed: ASE_HWMalfunction (-999)") {
        throw std::runtime_error("two seconds of latency should reach start: " + accepted);
    }
    expect("two seconds of latency starts", longest.starts, 1);
}

void test_playback_resource_lifetime()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto handles = [] {
        DWORD count{};
        if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
            throw std::runtime_error("Cannot inspect playback handle lifetime");
        }
        return count;
    };
    // Warm each path first so one-time CRT/COM initialization and the
    // process-lifetime playback event are outside the measured interval.
    for (const unsigned faults : {0U, 1U, 2U, 4U, 7U}) {
        const auto play = [&] {
            MockAsio driver;
            driver.fail_start = (faults & 1) != 0;
            driver.fail_stop = (faults & 2) != 0;
            driver.fail_dispose = (faults & 4) != 0;
            driver.ticks_after_start = {1};
            if (faults) {
                (void)expect_rejected(driver, path);
            } else {
                expect("resource-lifetime playback completed", static_cast<int>(play_in_session(driver, path)),
                    static_cast<int>(wavplay::playback::PlaybackResult::completed));
            }
            expect("resource-lifetime buffers disposed once", driver.disposals, 1);
        };
        play();
        const auto before = handles();
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            play();
        }
        expect("no retained handles after playback outcome " + std::to_string(faults), handles(), before);
    }
}

void test_renderer_residency_and_lifetime()
{
    struct ResidencyDriver : MockAsio {
        void* renderer_address{};
        void* renderer_allocation{};
        unsigned residency_checks{};
        bool locked_at_every_boundary = true;

        void inspect_renderer()
        {
            MEMORY_BASIC_INFORMATION mapping{};
            if (!VirtualQuery(renderer_address, &mapping, sizeof(mapping))) {
                throw std::runtime_error("Cannot inspect renderer mapping");
            }
            expect("renderer has committed private storage", mapping.State, MEM_COMMIT);
            expect("renderer is outside mapped code/images", mapping.Type, MEM_PRIVATE);
            renderer_allocation = mapping.AllocationBase;
            PSAPI_WORKING_SET_EX_INFORMATION query{};
            query.VirtualAddress = renderer_address;
            if (!QueryWorkingSetEx(GetCurrentProcess(), &query, sizeof(query))) {
                throw std::runtime_error("Cannot inspect renderer residency");
            }
            expect_true("renderer page is resident at the driver boundary", query.VirtualAttributes.Valid != 0);
            locked_at_every_boundary &= query.VirtualAttributes.Locked != 0;
            ++residency_checks;
        }
        ASIOError createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) override
        {
            renderer_address = infos; // ASIO descriptors live inside the renderer.
            inspect_renderer();
            return MockAsio::createBuffers(infos, count, frames, callbacks);
        }
        ASIOError start() override { inspect_renderer(); return MockAsio::start(); }
        ASIOError stop() override { inspect_renderer(); return MockAsio::stop(); }
        ASIOError disposeBuffers() override { inspect_renderer(); return MockAsio::disposeBuffers(); }
    };
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    for (const unsigned faults : {0U, 1U, 2U, 4U, 7U}) {
        ResidencyDriver driver;
        driver.fail_start = (faults & 1) != 0;
        driver.fail_stop = (faults & 2) != 0;
        driver.fail_dispose = (faults & 4) != 0;
        driver.ticks_after_start = {1};
        wavplay::playback::PlaybackDiagnostics diagnostics;
        const auto play = [&] {
            MockSession session(driver);
            return wavplay::playback::play_tracks(session, path, {}, &diagnostics);
        };
        if (faults) {
            expect_throw<std::runtime_error>("failure retains renderer through driver cleanup", play);
        } else {
            expect("resident renderer completes playback", play(), wavplay::playback::PlaybackResult::completed);
        }
        expect("renderer inspected through disposal", driver.residency_checks, 4);
        expect_true("renderer diagnostic matches observed residency",
            diagnostics.segments[0]->renderer_pages_locked == driver.locked_at_every_boundary);
        expect_true("a suite process reports its never-locked image as pageable",
            !diagnostics.segments[0]->image_pages_locked);
        MEMORY_BASIC_INFORMATION released{};
        if (!VirtualQuery(driver.renderer_allocation, &released, sizeof(released))) {
            throw std::runtime_error("Cannot inspect retired renderer mapping");
        }
        expect("detached renderer allocation released on every outcome", released.State, MEM_FREE);
    }
}

// The image lock lasts for the life of the process, so only a child of this
// executable takes it (run_locked_segment_child); the suite process never does.
// After one mock playback cycle the buffer setup reports the lock, and the page
// of CallbackState (notifications()), which the control thread and the callbacks
// write only after the lock, keeps it in its private copy like every other image
// page: the reason core has no relock of written image pages.
void child_plays_with_locked_image()
{
    if (!wavplay::core::lock_process_image()) {
        fail("the image lock was refused (see the memory suite's image lock cases)");
    }
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    MockAsio driver;
    driver.ticks_after_start = {1};
    wavplay::playback::PlaybackDiagnostics diagnostics;
    {
        MockSession session(driver);
        expect("locked-image playback completes", wavplay::playback::play_tracks(session, path, {}, &diagnostics),
            wavplay::playback::PlaybackResult::completed);
    }
    expect("locked-image playback started once", driver.starts, 1);
    const auto& setup = diagnostics.segments[0];
    if (!setup) {
        fail("locked-image playback recorded no buffer setup");
    }
    expect_true("a process that locked its image reports it in the buffer setup", setup->image_pages_locked);
    expect_true("prefetch ring locked beside the image", setup->ring_pages_locked);
    expect_true("prefetch metadata locked beside the image", setup->ring_metadata_pages_locked);
    expect_true("renderer locked beside the image", setup->renderer_pages_locked);
    expect_true("transport requests locked beside the image", setup->request_pages_locked);
    expect_true("the callback state page stays resident and locked after the cycle",
        resident_and_locked(&wavplay::playback::notifications()));
    expect("every image page stays resident and locked after the cycle", locked_image_pages(),
        image_bytes() / wavplay::core::page_size());
}

// Runs the child above and fails with what it wrote unless it exits with 0.
void test_locked_image_in_buffer_setup()
{
    const auto child = run_own_child(L"--locked-segment-child", hang_budget(10s));
    if (child.timed_out) {
        fail("the locked-image playback child did not exit: " + child.output);
    }
    if (child.exit_code != 0) {
        fail("the locked-image playback child exited with " + std::to_string(child.exit_code) + ": " + child.output);
    }
}

void test_missing_switches(bool wake_repeatedly)
{
    TempFile first, second;
    const auto* path = first.write(wave({pcm16(2, 48000), {"data", samples(12)}}));
    const auto* next = second.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    for (const bool omit_first : {true, false}) {
        MockAsio driver;
        driver.preroll_switches = omit_first ? 0 : 1;
        auto done = start_track(driver, path, next);
        if (!omit_first) {
            driver.tick(); // A and B were both accepted before this driver stalled.
        }
        std::jthread unrelated_wakes;
        if (wake_repeatedly) {
            // The process-wide event can wake for a Stop on an unrelated idle
            // request. A wake is not a buffer switch and must not renew the
            // watchdog's deadline. Keep waking until the wait has been judged.
            unrelated_wakes = std::jthread([](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    wavplay::playback::notifications().wake();
                    std::this_thread::sleep_for(10ms);
                }
            });
        }
        const auto finished = done.wait_for(5s);
        unrelated_wakes.request_stop();
        if (unrelated_wakes.joinable()) {
            unrelated_wakes.join();
        }
        if (finished != std::future_status::ready) {
            driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
            if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
                abandon_hung("Stalled ASIO playback ignored watchdog and cleanup reset");
            }
            (void)done.get();
            throw std::runtime_error(wake_repeatedly ? "Unrelated wakes postponed the ASIO switch watchdog" :
                "No control-thread stop after ASIO switches disappeared");
        }
        expect("missing switches stop the run", static_cast<int>(done.get()),
            static_cast<int>(wavplay::playback::PlaybackResult::driver_error));
        expect("stalled driver stopped", driver.stops, 1);
        expect("stalled driver buffers released", driver.disposals, 1);
        expect("silence on final stop callback", static_cast<int>(driver.silent_on_stop), 1);
        expect("stalled first track prevents a rate change", driver.set_rates.size(), 0);
    }
}

// The shape of a RoutedRenderer: the rate of its stereo PCM16 file, its block
// width, the reported output latency in whole blocks and its start silence.
struct RouteShape {
    std::uint32_t rate = 48000;
    std::size_t width = MockAsio::width;
    long latency_blocks = 1;
    std::chrono::milliseconds start_silence{};
};

// The direct harness of the callback cases below: a renderer of a new request
// over a PCM16 file, with test-owned halves, routed to the process-wide
// callback table and driven from the test thread. Every switch is a
// synchronous call, so these cases need no waits, except the case that holds a
// copy on a driver thread of its own while another thread detaches.
struct RoutedRenderer {
    explicit RoutedRenderer(const wchar_t* path, const RouteShape& shape = {})
        : ring(make_ring(path, {}, shape.width, wavplay::audio::int16_le)),
          token(wavplay::playback::notifications().begin(shape.rate)), storage(2 * 2 * shape.width),
          renderer(ring, token, shape.start_silence, control)
    {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t half = 0; half < 2; ++half) {
                output[channel][half] = std::span(storage).subspan((2 * channel + half) * shape.width, shape.width);
                renderer->buffers()[channel].buffers[half] = output[channel][half].data();
            }
        }
        renderer->set_output_latency(shape.latency_blocks * static_cast<long>(shape.width));
    }

    // A nonzero pattern, so a later check sees what a switch wrote.
    static constexpr std::uint16_t poison_sample = 0xa5a5;

    // The samples the renderer writes for `channel` in `half`: the test-owned
    // view in `output`, unless a case pointed that half elsewhere (GuardedHalf).
    [[nodiscard]] std::span<std::uint16_t> written(std::size_t channel, std::size_t half) const
    {
        return {static_cast<std::uint16_t*>(renderer->buffers()[channel].buffers[half]), output[channel][half].size()};
    }

    void poison(std::size_t half)
    {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            std::ranges::fill(written(channel, half), poison_sample);
        }
    }

    // Whether every sample of both channels of `half` is `value`.
    [[nodiscard]] bool holds(std::size_t half, std::uint16_t value) const
    {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            if (!std::ranges::all_of(written(channel, half), [value](std::uint16_t sample) { return sample == value; })) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool silent(std::size_t half) const { return holds(half, 0); }

    wavplay::stream::PrefetchRing ring;
    const wavplay::playback::PlaybackSignal::Token token;
    std::atomic<bool> pause{}, stop{};
    wavplay::playback::SegmentControl control{pause, stop};
    std::vector<std::uint16_t> storage;
    std::array<std::array<std::span<std::uint16_t>, 2>, 2> output{}; // Channel, half: views of storage.
    const wavplay::core::ResidentObject<wavplay::playback::SegmentRenderer> renderer;
};

// Declared after the renderer it routes to, so the route ends before the renderer does.
struct DetachOnExit {
    ~DetachOnExit() { (void)wavplay::playback::callback_routing::detach(); }
};

// The watchdog's evidence. Every switch routed to the attached renderer counts,
// whether it submits PCM or only silences its half; prefill, a detached switch
// and a switch of another phase do not. The callbacks' table and signal share
// one 128-byte block.
void test_switch_count()
{
    namespace playback = wavplay::playback;
    auto* const callbacks = playback::callback_routing::callbacks();
    expect("callback table and signal share one 128-byte block",
        reinterpret_cast<std::uintptr_t>(callbacks) / wavplay::core::adjacent_line_pair,
        reinterpret_cast<std::uintptr_t>(&playback::notifications()) / wavplay::core::adjacent_line_pair);
    // The other three entries are the roots of the real-time machine-code audit
    // (Assert-RealTimeCode in scripts/verify.ps1), and MockAsio::createBuffers
    // requires them: a newly wired callback is added to the audit's roots.
    expect_true("the table leaves bufferSwitchTimeInfo unwired, outside the audit's roots",
        callbacks->bufferSwitchTimeInfo == nullptr);

    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(16)}}));
    RoutedRenderer routed(path);
    const auto& renderer = routed.renderer;
    expect("prefilled B", static_cast<int>(renderer->prefill()), static_cast<int>(wavplay::stream::BlockStatus::ready));
    expect("prefill is not a switch", renderer->switches(), 0);

    const DetachOnExit detach_on_exit;
    playback::callback_routing::attach(*renderer);
    for (const long half : {0L, 1L, 0L}) {
        callbacks->bufferSwitch(half, ASIOFalse);
    }
    expect("each routed switch counts", renderer->switches(), 3);
    expect("the routed switches kept the phase playing", playback::notifications().result(),
        playback::PlaybackResult::playing);
    callbacks->bufferSwitch(0, ASIOFalse); // Out of sequence: recorded as a fault, the half silenced.
    expect("a switch that only silences counts", renderer->switches(), 4);
    expect("the broken sequence ended the phase", playback::notifications().result(),
        playback::PlaybackResult::driver_error);
    expect_true("detach finds the renderer", playback::callback_routing::detach());
    callbacks->bufferSwitch(1, ASIOFalse);
    expect("a detached switch does not count", renderer->switches(), 4);
    playback::callback_routing::attach(*renderer);
    (void)playback::notifications().begin(48000);
    callbacks->bufferSwitch(1, ASIOFalse);
    expect("a switch of another phase does not count", renderer->switches(), 4);
    expect("PCM submitted by prefill and the three switches", renderer->submitted_frames(), 4 * MockAsio::width);
}

// Each watchdog deadline compares the switch count with its reading at the
// previous deadline. Switches that keep arriving renew the deadlines they span;
// once they stop, switches before one deadline cannot satisfy the next.
void test_watchdog_compares_counts()
{
    // 1.5 s of switches: past the first deadline (the 979 ms control wake period
    // of these 2-frame blocks), into the second period.
    constexpr unsigned paced_switches = 30;
    struct PacedDriver : MockAsio {
        std::jthread pacer;
        ASIOError start() override
        {
            const auto result = MockAsio::start();
            if (result == ASE_OK) {
                pacer = std::jthread([this](std::stop_token stop) {
                    while (!stop.stop_requested() && !wavplay::playback::is_streaming()) {
                        std::this_thread::yield();
                    }
                    // A fixed schedule: one late wake does not delay the later switches.
                    const auto first = std::chrono::steady_clock::now();
                    for (unsigned i = 1; i <= paced_switches && !stop.stop_requested(); ++i) {
                        std::this_thread::sleep_until(first + i * 50ms);
                        tick();
                    }
                });
                // Raised as a driver's own thread is, so load on other threads
                // does not starve the schedule. Only a hint; failure is ignored.
                (void)SetThreadPriority(pacer.native_handle(), THREAD_PRIORITY_HIGHEST);
            }
            return result;
        }
        ASIOError stop() override
        {
            if (pacer.joinable()) {
                pacer.request_stop();
                pacer.join();
            }
            return MockAsio::stop();
        }
    };
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1000)}}));
    PacedDriver driver;
    auto done = start_track(driver, path);
    if (done.wait_for(hang_budget(8s)) != std::future_status::ready) {
        driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
            abandon_hung("Stalled ASIO playback ignored watchdog and cleanup reset");
        }
        (void)done.get();
        throw std::runtime_error("Switches before an earlier watchdog deadline renewed a later one");
    }
    expect("a stall after paced switches stops the run", static_cast<int>(done.get()),
        static_cast<int>(wavplay::playback::PlaybackResult::driver_error));
    expect("every paced switch arrived while playing (B, pre-roll, paced)", driver.submitted.size(),
        2 + paced_switches);
    expect("stalled driver stopped", driver.stops, 1);
}

// The order of render's entry checks: the sequence check runs before the phase
// match and the Stop request, so a broken sequence is recorded after a pending
// Stop request and after a latched Stop. A fault replaces the clean stop, and
// the half is silenced either way.
void test_sequence_fault_after_stop()
{
    namespace playback = wavplay::playback;
    using enum playback::PlaybackResult;
    auto* const callbacks = playback::callback_routing::callbacks();
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(16)}}));
    {
        RoutedRenderer routed(path);
        const DetachOnExit detach_on_exit;
        expect("prefilled B", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        playback::callback_routing::attach(*routed.renderer);
        routed.poison(0);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect("the first switch keeps the phase playing", playback::notifications().result(), playing);
        expect_true("the first switch submits PCM", !routed.silent(0));
        routed.stop = true; // Requested; no callback has seen it yet.
        routed.poison(0);
        callbacks->bufferSwitch(0, ASIOFalse); // Out of sequence.
        expect("a broken sequence wins over a pending Stop request", playback::notifications().result(), driver_error);
        expect_true("the out-of-sequence half is silenced", routed.silent(0));
        expect_true("detach finds the first renderer", playback::callback_routing::detach());
    }
    {
        RoutedRenderer routed(path);
        const DetachOnExit detach_on_exit;
        routed.stop = true;
        expect("prefilled B before the Stop", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        playback::callback_routing::attach(*routed.renderer);
        routed.poison(0);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect("the Stop request ends the phase", playback::notifications().result(), stopped);
        expect_true("the stopping half is silenced", routed.silent(0));
        routed.poison(1);
        callbacks->bufferSwitch(1, ASIOFalse); // In sequence, observing the latched Stop.
        expect("an in-sequence switch keeps the latched Stop", playback::notifications().result(), stopped);
        expect_true("the half after the Stop is silenced", routed.silent(1));
        routed.poison(1);
        callbacks->bufferSwitch(1, ASIOFalse); // Out of sequence, observing the latched Stop.
        expect("a broken sequence replaces the latched Stop", playback::notifications().result(), driver_error);
        expect_true("the out-of-sequence half after the Stop is silenced", routed.silent(1));
    }
}

// A guard page under the left half of A. The block copy's first access to it
// faults; this handler runs `during` on the copying thread, inside the copy,
// before the access resumes. Only a fault inside that page is handled.
struct GuardedCopy {
    std::uintptr_t begin{}, end{};
    unsigned faults{};
    void (*during)(void* context){};
    void* context{};
} guarded_copy;

LONG CALLBACK run_during_copy(EXCEPTION_POINTERS* exception)
{
    const auto& record = *exception->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_GUARD_PAGE || record.NumberParameters < 2 ||
        record.ExceptionInformation[1] < guarded_copy.begin || record.ExceptionInformation[1] >= guarded_copy.end) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ++guarded_copy.faults;
    guarded_copy.during(guarded_copy.context);
    return EXCEPTION_CONTINUE_EXECUTION; // The fault removed the guard, so the access now completes.
}

// The left half of a RoutedRenderer's A on a page of its own. Construct it
// before prefill; after arm(), the next block copy into that half runs `during`
// (GuardedCopy). Declare it after the RoutedRenderer and after whatever
// `during` uses, and before any DetachOnExit, so the route ends before the
// page is freed and the handler is removed before its context ends.
class GuardedHalf {
public:
    explicit GuardedHalf(RoutedRenderer& routed)
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        bytes_ = system.dwPageSize;
        page_ = VirtualAlloc(nullptr, bytes_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!page_) {
            throw std::runtime_error("Could not allocate the guarded half");
        }
        routed.renderer->buffers()[0].buffers[0] = page_;
    }
    GuardedHalf(const GuardedHalf&) = delete;
    GuardedHalf& operator=(const GuardedHalf&) = delete;
    ~GuardedHalf()
    {
        if (handler_) {
            (void)RemoveVectoredExceptionHandler(handler_);
        }
        VirtualFree(page_, 0, MEM_RELEASE);
    }

    // After the case has written the half for the last time: guards the page.
    void arm(void (*during)(void* context), void* context = nullptr)
    {
        const auto begin = reinterpret_cast<std::uintptr_t>(page_);
        guarded_copy = {begin, begin + bytes_, 0, during, context};
        DWORD previous{};
        if (!VirtualProtect(page_, bytes_, PAGE_READWRITE | PAGE_GUARD, &previous)) {
            throw std::runtime_error("Could not guard the left half");
        }
        handler_ = AddVectoredExceptionHandler(1, run_during_copy);
        if (!handler_) {
            throw std::runtime_error("Could not register the copy-window handler");
        }
    }

private:
    void* page_{};
    std::size_t bytes_{};
    void* handler_{};
};

// A popped block is kept only if the phase is still current after the copy: a
// stop reason recorded during the copy wins, and silence overwrites the block.
void test_phase_ends_during_copy()
{
    namespace playback = wavplay::playback;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(16)}}));
    RoutedRenderer routed(path);
    GuardedHalf guarded(routed);
    expect("prefilled B", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
    routed.poison(0);
    // The driver's reset request, through the product's asioMessage.
    guarded.arm([](void*) {
        (void)wavplay::playback::callback_routing::callbacks()->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    });
    const DetachOnExit detach_on_exit;
    playback::callback_routing::attach(*routed.renderer);
    playback::callback_routing::callbacks()->bufferSwitch(0, ASIOFalse);
    expect("the block copy met the guard once", guarded_copy.faults, 1);
    expect("the reset recorded during the copy ended the phase", playback::notifications().result(),
        playback::PlaybackResult::reinitialize_required);
    expect_true("the copied block is silenced in both channels", routed.silent(0));
}

// The writer flags admit one consumer of the ring. A switch that enters while
// another is still copying, of the other half or re-entering the same one,
// records an underrun and pops nothing: the copied half is silenced, and the
// ring's next block is the one after the copied block.
void test_overlapping_switches()
{
    namespace playback = wavplay::playback;
    TempFile file;
    const auto pcm = samples(16);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const auto sample_of = [&pcm](std::size_t frame, std::size_t channel) {
        const auto offset = frame * 4 + channel * 2;
        return static_cast<std::uint16_t>(pcm[offset] | pcm[offset + 1] << 8);
    };
    for (long overlapping : {1L, 0L}) {
        RoutedRenderer routed(path);
        GuardedHalf guarded(routed);
        expect("prefilled B with the first block", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        routed.poison(0);
        routed.poison(1);
        // A second switch, on the copying thread, inside the copy of the second block into A.
        guarded.arm([](void* half) {
            wavplay::playback::callback_routing::callbacks()->bufferSwitch(*static_cast<const long*>(half), ASIOFalse);
        }, &overlapping);
        const DetachOnExit detach_on_exit;
        playback::callback_routing::attach(*routed.renderer);
        playback::callback_routing::callbacks()->bufferSwitch(0, ASIOFalse);
        expect("the block copy met the guard once", guarded_copy.faults, 1);
        expect("a switch overlapping a copy is an underrun", playback::notifications().result(),
            playback::PlaybackResult::underrun);
        expect_true("the overlapped copy is silenced", routed.silent(0));
        if (overlapping == 1) {
            expect_true("the switch of the other half silences it", routed.silent(1));
        } else {
            expect_true("the re-entering switch writes nothing", routed.holds(1, RoutedRenderer::poison_sample));
        }
        expect_true("detach finds the renderer", playback::callback_routing::detach());
        // The test thread is the only consumer now.
        std::vector<std::uint16_t> left(MockAsio::width), right(MockAsio::width);
        expect("the ring still holds the third block", await_block(routed.ring, left.data(), right.data()).status,
            wavplay::stream::BlockStatus::ready);
        for (std::size_t slot = 0; slot < left.size(); ++slot) {
            const auto frame = 2 * left.size() + slot;
            expect("third block, left", left[slot], sample_of(frame, 0));
            expect("third block, right", right[slot], sample_of(frame, 1));
        }
    }
}

// detach waits for a switch that pinned the renderer and is still copying: a
// driver thread's copy is held inside the guard page's handler while another
// thread detaches. The silence that the control thread writes after detach, as
// the failed-stop path does before disposal, is not overwritten by that copy.
void test_detach_waits_for_copy()
{
    namespace playback = wavplay::playback;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(16)}}));
    CallGate copy_gate;
    RoutedRenderer routed(path);
    GuardedHalf guarded(routed);
    expect("prefilled B", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
    routed.poison(0);
    guarded.arm([](void* gate) { static_cast<CallGate*>(gate)->block(); }, &copy_gate);
    const DetachOnExit detach_on_exit;
    playback::callback_routing::attach(*routed.renderer);
    std::optional<TestThread<bool>> detacher;
    std::optional<TestThread<void>> driver;
    const ReleaseGate release_copy{copy_gate}; // Opens the gate before either thread is joined.
    driver.emplace([] { wavplay::playback::callback_routing::callbacks()->bufferSwitch(0, ASIOFalse); });
    copy_gate.wait();
    detacher.emplace([&routed] {
        const bool attached = wavplay::playback::callback_routing::detach();
        routed.renderer->silence(0);
        routed.renderer->silence(1);
        return attached;
    });
    // A detection window: a detach that does not wait returns at once.
    expect("detach waits while the switch is copying", detacher->wait_for(100ms), std::future_status::timeout);
    copy_gate.open();
    if (detacher->wait_for(hang_budget(5s)) != std::future_status::ready) {
        fail("detach did not return after the copy finished");
    }
    expect_true("detach finds the renderer", detacher->get());
    driver->get();
    expect("the block copy met the guard once", guarded_copy.faults, 1);
    expect("the finished copy kept the phase playing", playback::notifications().result(),
        playback::PlaybackResult::playing);
    expect_true("the silence written after detach stays in A", routed.silent(0));
    expect_true("the silence written after detach stays in B", routed.silent(1));
}

// Whether `event` was set since the previous check; the zero-timeout wait consumes the signal.
void expect_set(std::string_view what, HANDLE event, bool set,
    std::source_location location = std::source_location::current())
{
    const DWORD expected = set ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    expect(what, WaitForSingleObject(event, 0), expected, location);
}

// The control thread's auto-reset event is set only when a callback records a
// stop reason, once per reason: steady switches, a matching clock report, a
// capability query and every callback after the phase ended set nothing.
void test_control_event_only_on_stop_reasons()
{
    namespace playback = wavplay::playback;
    using enum playback::PlaybackResult;
    auto* const callbacks = playback::callback_routing::callbacks();
    TempFile file, short_file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(16)}}));
    const auto* one_frame = short_file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    (void)playback::notifications().begin(48000); // The first request creates the process-lifetime event.
    const auto changed = playback::notifications().event_handle();
    if (!changed) {
        fail("a request did not create the control event");
    }
    {
        RoutedRenderer routed(path);
        const DetachOnExit detach_on_exit;
        expect("prefilled B", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        playback::callback_routing::attach(*routed.renderer);
        (void)WaitForSingleObject(changed, 0); // Whatever earlier phases left.
        for (const long half : {0L, 1L, 0L, 1L}) {
            callbacks->bufferSwitch(half, ASIOFalse);
            expect_set("a steady PCM switch sets the control event", changed, false);
        }
        callbacks->sampleRateDidChange(48000.0);
        expect_set("a report of the expected clock sets the control event", changed, false);
        expect("reset is an advertised selector",
            callbacks->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr), 1);
        expect_set("a capability query sets the control event", changed, false);
        expect("the steady switches kept the phase playing", playback::notifications().result(), playing);
        callbacks->bufferSwitch(1, ASIOFalse); // Out of sequence.
        expect("the broken sequence ended the phase", playback::notifications().result(), driver_error);
        expect_set("the first fault sets the control event", changed, true);
        callbacks->bufferSwitch(1, ASIOFalse);
        expect_set("a repeated sequence fault sets the control event", changed, false);
        callbacks->bufferSwitch(2, ASIOFalse);
        expect_set("an invalid index after the fault sets the control event", changed, false);
        (void)callbacks->asioMessage(kAsioOverload, 0, nullptr, nullptr);
        expect_set("a dropout report after the fault sets the control event", changed, false);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect_set("an in-sequence switch after the fault sets the control event", changed, false);
        expect("the first fault stays", playback::notifications().result(), driver_error);
    }
    {
        RoutedRenderer stopping(path);
        const DetachOnExit detach_on_exit;
        stopping.stop = true;
        expect("prefilled B before the Stop", stopping.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        playback::callback_routing::attach(*stopping.renderer);
        (void)WaitForSingleObject(changed, 0);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect("the Stop request ended the phase", playback::notifications().result(), stopped);
        expect_set("the Stop sets the control event", changed, true);
        callbacks->bufferSwitch(1, ASIOFalse);
        expect_set("a switch after the Stop sets the control event", changed, false);
    }
    {
        // A Pause before any PCM was submitted ends the phase at once.
        RoutedRenderer pausing(path, {.start_silence = 1ms});
        const DetachOnExit detach_on_exit;
        pausing.pause = true;
        expect("prefilled silent B", pausing.renderer->prefill(), wavplay::stream::BlockStatus::ready);
        playback::callback_routing::attach(*pausing.renderer);
        (void)WaitForSingleObject(changed, 0);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect("the Pause in start silence ended the phase", playback::notifications().result(), paused);
        expect_set("the Pause sets the control event", changed, true);
        callbacks->bufferSwitch(1, ASIOFalse);
        expect_set("a switch after the Pause sets the control event", changed, false);
    }
    {
        // The single frame is in prefilled B: three switches count down the
        // reported three blocks of latency, and the fourth completes the drain.
        RoutedRenderer draining(one_frame, {.latency_blocks = 3});
        const DetachOnExit detach_on_exit;
        expect("the final block in prefilled B", draining.renderer->prefill(), wavplay::stream::BlockStatus::last);
        playback::callback_routing::attach(*draining.renderer);
        struct Streaming {
            Streaming() { wavplay::playback::callback_routing::set_streaming(true); }
            ~Streaming() { wavplay::playback::callback_routing::set_streaming(false); }
        } streaming; // The drain counts only switches after the hardware started.
        (void)WaitForSingleObject(changed, 0);
        for (const long half : {0L, 1L, 0L}) {
            callbacks->bufferSwitch(half, ASIOFalse);
            expect_set("a drain switch sets the control event", changed, false);
        }
        expect("the drain is still counting", playback::notifications().result(), playing);
        callbacks->bufferSwitch(1, ASIOFalse);
        expect("the drained segment completed", playback::notifications().result(), completed);
        expect_set("the completed drain sets the control event", changed, true);
        callbacks->bufferSwitch(0, ASIOFalse);
        expect_set("a switch after the drain sets the control event", changed, false);
    }
}

void test_overload_inquiry()
{
    struct ReportingDriver : MockAsio {
        ASIOError reply = ASE_SUCCESS;
        unsigned inquiries{};
        ASIOError future(long selector, void* params) override
        {
            if (selector != kAsioCanReportOverload || params) {
                return ASE_InvalidParameter;
            }
            ++inquiries;
            if (reply == ASE_SUCCESS) {
                fault_at = width; // This device sends no dropout reports until queried.
            }
            return reply;
        }
    };
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(5)}}));
    ReportingDriver reporting;
    const auto result = run_to_tick(reporting, start_track(reporting, path), 6);
    expect("negotiated device dropout", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::underrun));
    expect("one capability inquiry", reporting.inquiries, 1);
    expect("reported dropout disposed", reporting.disposals, 1);
    for (const auto unsupported : {ASE_InvalidParameter, ASE_NotPresent, ASE_OK}) {
        ReportingDriver legacy;
        legacy.reply = unsupported;
        play_and_check(legacy, path, samples(5), 6);
    }
    ReportingDriver broken;
    broken.reply = ASE_HWMalfunction;
    if (expect_rejected(broken, path) != "ASIO overload inquiry failed: ASE_HWMalfunction (-999)") {
        throw std::runtime_error("Unexpected error from failed overload inquiry");
    }
    expect("no buffers on hardware fault", broken.creates, 0);
}

void test_overload_at_drain()
{
    using enum MockAsio::Call;
    TempFile first, second;
    const auto* path = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto* next = second.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    for (const auto point : {stop, dispose}) {
        MockAsio driver;
        driver.ticks_after_start = {1, 1};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == point) {
                driver.message_notification(kAsioOverload, 0, nullptr, nullptr);
            }
        };
        const auto result = play_in_session(driver, path, next);
        expect("dropout reported during drain cleanup", static_cast<int>(result),
            static_cast<int>(wavplay::playback::PlaybackResult::underrun));
        expect("buffers released after late dropout", driver.disposals, 1);
        expect("second track clock settings after dropout", driver.set_rates.size(), 0);
        expect("starts after late dropout", driver.starts, 1);
    }
}

void test_unwritable_output()
{
    // The first sample fits in a writable page. The second one crosses into
    // a read-only page, so checking only the first address would be unsafe.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2ULL * system.dwPageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) {
        throw std::runtime_error("Could not allocate the test buffer");
    }
    const auto free_pages = [](void* address) { VirtualFree(address, 0, MEM_RELEASE); };
    const std::unique_ptr<void, decltype(free_pages)> allocation(pages, free_pages);
    auto* boundary = pages + system.dwPageSize;
    DWORD previous{};
    if (!VirtualProtect(boundary, system.dwPageSize, PAGE_READONLY, &previous)) {
        throw std::runtime_error("Could not protect the test buffer");
    }
    struct BrokenBuffer : MockAsio {
        void* right_b{};
        ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
        {
            const auto result = MockAsio::createBuffers(infos, count, size, callbacks);
            if (result == ASE_OK) {
                infos[1].buffers[1] = right_b;
            }
            return result;
        }
    };
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(3)}}));
    for (void* buffer : {static_cast<void*>(nullptr), static_cast<void*>(boundary - 2)}) {
        BrokenBuffer driver;
        driver.right_b = buffer;
        boundary[-2] = 0xa5;
        check_rejected_buffers(driver, path);
        expect("writable prefix untouched", boundary[-2], 0xa5);
    }
}

// At 44.1 kHz each slot holds one second, as in the prefetch ring suite.
constexpr std::size_t ring_block = 44100;

void check_read_failure_before_start(std::chrono::milliseconds start_silence)
{
    // The producer fails after priming but before B is prefilled. The segment does
    // not start and reports a read error, not a driver fault.
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 2 * ring_block + 1)}));
    auto lock = open_lock_handle(path);
    const auto offset = static_cast<DWORD>(wavplay::media::WavReader(path).info().data_offset + 2 * ring_block * 4);
    if (!LockFile(lock.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock first refill");
    }
    auto ring = make_ring(path, {}, ring_block, wavplay::audio::int16_le);
    MockAsio driver;
    driver.sample_rate = 44100;
    driver.buffer_size = {ring_block, ring_block, ring_block, 0};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::latency) {
            // Buffers exist and B is not prefilled yet: drain the primed blocks until
            // the failed refill is the next thing the segment will observe.
            std::vector<std::uint16_t> left(ring_block), right(ring_block);
            for (auto status = wavplay::stream::BlockStatus::ready; status != wavplay::stream::BlockStatus::read_error;) {
                status = await_block(ring, left.data(), right.data()).status;
            }
        }
    };
    const auto token = wavplay::playback::notifications().begin(44100);
    const std::atomic<bool> never_requested{};
    wavplay::playback::SegmentControl control{never_requested, never_requested};
    const auto result = wavplay::playback::run_segment(driver, ring, token, control, nullptr, start_silence);
    expect("read failure before start", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::read_error));
    expect("no start after a failed prefill", driver.starts, 0);
    expect("buffers released after a failed prefill", driver.disposals, 1);
    expect("failed prefill submitted no PCM", control.submitted_frames, 0);
}

// Switches that consume nothing (start silence, the pause and drain countdowns)
// still check the producer: a refill that failed after prefill ends the phase
// as a read error at the next such switch, not only when stop returns.
void test_silent_switch_observes_read_failure()
{
    namespace playback = wavplay::playback;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 2 * ring_block + 1)}));
    auto lock = open_lock_handle(path);
    const auto offset = static_cast<DWORD>(wavplay::media::WavReader(path).info().data_offset + 2 * ring_block * 4);
    if (!LockFile(lock.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock first refill");
    }
    // Two blocks of start silence: prefilled B and the first switch.
    RoutedRenderer routed(path, {.rate = 44100, .width = ring_block, .start_silence = 2000ms});
    expect("start silence blocks", routed.renderer->start_silence_blocks(), 2);
    expect("start silence prefills B without a pop", routed.renderer->prefill(), wavplay::stream::BlockStatus::ready);
    // The test thread is the only consumer: take the primed blocks until the failed refill shows.
    std::vector<std::uint16_t> left(ring_block), right(ring_block);
    for (auto status = wavplay::stream::BlockStatus::ready; status != wavplay::stream::BlockStatus::read_error;) {
        status = await_block(routed.ring, left.data(), right.data()).status;
    }
    routed.poison(0);
    routed.poison(1);
    const DetachOnExit detach_on_exit;
    playback::callback_routing::attach(*routed.renderer);
    playback::callback_routing::callbacks()->bufferSwitch(0, ASIOFalse);
    expect("a start-silence switch records the producer failure", playback::notifications().result(),
        playback::PlaybackResult::read_error);
    expect_true("the failing switch's half is silent", routed.silent(0));
}

// The control thread checks the producer once more after joining it. A refill
// that fails while no switch arrives is seen by no callback, and once a Stop
// has ended the phase, switches only silence their halves. play_tracks offers
// no Stop; a WM_QUIT ends the phase as a Stop does. The request still reports
// read_error. The locked frame is the last one that the refill after the wake
// threshold reads, so no refill before the threshold pop can reach it.
void test_read_failure_after_last_callback()
{
    using wavplay::playback::PlaybackResult;
    constexpr std::uint32_t rate = 48000;
    constexpr long width = 4800; // 100 ms blocks: the switch watchdog then waits 880 ms for a switch.
    constexpr auto block_frames = static_cast<std::size_t>(width);
    constexpr std::size_t frames = 4 * rate;
    const auto layout = wavplay::stream::plan_ring_layout(
        wavplay::stream::plan_block_shape(block_frames, 2), frames, rate);
    const auto locked_block = layout.capacity + layout.wake_interval - 1;
    expect_true("the locked block lies inside the run", locked_block < layout.total_blocks);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, rate), pcm16_ramp(2, frames)}));
    // The lock handle shares writing and deletion, so the probe below fails only while the reader is open.
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open refill lock");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    const auto offset = static_cast<DWORD>(
        wavplay::media::WavReader(path).info().data_offset + locked_block * block_frames * 4);
    if (!LockFile(lock.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock a refill frame");
    }
    std::atomic<DWORD> control_thread{};
    MockAsio driver;
    driver.buffer_size = {width, width, width, 0};
    driver.latency = width;
    driver.on_call = [&control_thread](MockAsio::Call call) {
        if (call == MockAsio::Call::start) {
            control_thread = GetCurrentThreadId();
        }
    };
    auto done = start_track(driver, path);
    // Without start silence every half so far holds a popped block, prefilled B included.
    for (unsigned tick = 0; driver.submitted.size() < layout.wake_interval; ++tick) {
        if (tick == layout.wake_interval) {
            fail("the switches did not reach the wake threshold");
        }
        driver.tick();
    }
    expect("halves up to the threshold pop", driver.submitted.size(), layout.wake_interval);
    expect_true("each of them holds PCM", std::ranges::none_of(driver.submitted, MockAsio::silent));
    // The woken producer refills up to the locked frame, fails and closes its reader.
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    while (probe_file_access(path, GENERIC_WRITE | DELETE) != ERROR_SUCCESS) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("the refill after the threshold pop did not fail");
        }
        Sleep(1);
    }
    expect("no callback has seen the failure", wavplay::playback::notifications().result(), PlaybackResult::playing);
    expect("WM_QUIT posted to the control thread", PostThreadMessageW(control_thread, WM_QUIT, 0, 0), TRUE);
    if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
        abandon_hung("Playback did not stop after WM_QUIT");
    }
    expect("the check after the producer's join reports the failure", done.get(), PlaybackResult::read_error);
    expect("one stop", driver.stops, 1);
    expect("one disposal", driver.disposals, 1);
    expect_true("silence on the final switch", driver.silent_on_stop);
}

void check_callback_without_pcm(bool fail_read)
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 2 * ring_block + (fail_read ? 1 : 0))}));
    auto lock = open_lock_handle(path);
    if (fail_read) {
        const auto offset = static_cast<DWORD>(wavplay::media::WavReader(path).info().data_offset + 2 * ring_block * 4);
        if (!LockFile(lock.get(), offset, 0, 4, 0)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock first refill");
        }
    }
    auto ring = make_ring(path, {}, ring_block, wavplay::audio::int16_le);
    MockAsio driver;
    driver.sample_rate = 44100;
    driver.buffer_size = {ring_block, ring_block, ring_block, 0};
    driver.preroll_switches = 2; // Both callbacks arrive before stop can run.
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start) {
            // B has been prefilled; no callback is running. Take the remaining
            // primed slot here so A must observe the intended queue state.
            std::vector<std::uint16_t> left(ring_block), right(ring_block);
            auto result = await_block(ring, left.data(), right.data());
            if (fail_read && result.status == wavplay::stream::BlockStatus::ready) {
                result = await_block(ring, left.data(), right.data());
            }
            expect("queue at first switch", static_cast<int>(result.status),
                static_cast<int>(fail_read ? wavplay::stream::BlockStatus::read_error : wavplay::stream::BlockStatus::last));
        }
    };
    const auto token = wavplay::playback::notifications().begin(44100);
    const std::atomic<bool> never_requested{};
    wavplay::playback::SegmentControl control{never_requested, never_requested};
    constexpr std::uint64_t previous_frames = 19;
    control.submitted_frames = previous_frames;
    std::optional<wavplay::playback::BufferSetup> setup;
    const auto result = wavplay::playback::run_segment(driver, ring, token, control, &setup);
    expect_true("stack-owned request flags are not reported as locked", !setup->request_pages_locked);
    expect("fault exit retains earlier segments and the prefilled PCM",
        control.submitted_frames, previous_frames + ring_block);
    expect("first stop reason", static_cast<int>(result),
        static_cast<int>(fail_read ? wavplay::playback::PlaybackResult::read_error : wavplay::playback::PlaybackResult::underrun));
    expect("submitted halves", driver.submitted.size(), 3);
    expect("prefilled B contains audio", static_cast<int>(MockAsio::silent(driver.submitted[0])), 0);
    expect("silence on fault", static_cast<int>(MockAsio::silent(driver.submitted[1])), 1);
    expect("silence after fault", static_cast<int>(MockAsio::silent(driver.submitted[2])), 1);
    expect("silence at stop", static_cast<int>(driver.silent_on_stop), 1);
    expect("stops on PCM fault", driver.stops, 1);
    expect("disposals on PCM fault", driver.disposals, 1);
}

void test_submitted_frames_after_partial_start()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(5)}}));
    auto ring = make_ring(path, {}, MockAsio::width, wavplay::audio::int16_le);
    MockAsio driver;
    driver.fail_start = true; // Prefilled B and synchronous A were both submitted.
    const auto token = wavplay::playback::notifications().begin(48000);
    const std::atomic<bool> never_requested{};
    wavplay::playback::SegmentControl control{never_requested, never_requested};
    constexpr std::uint64_t previous_frames = 19;
    control.submitted_frames = previous_frames;
    expect_throw<std::runtime_error>("driver failed after the first callback", [&] {
        (void)wavplay::playback::run_segment(driver, ring, token, control);
    });
    expect("partial start retains exactly B and A once", control.submitted_frames,
        previous_frames + 2 * MockAsio::width);
    expect("partial start stops before count is published", driver.stops, 1);
    expect("partial start disposes its buffers", driver.disposals, 1);
    expect("partial start's final callback is silent", static_cast<int>(driver.silent_on_stop), 1);
}

} // namespace

int wavplay_test::run_locked_segment_child(std::span<wchar_t* const> operands)
{
    if (!operands.empty()) {
        return 2;
    }
    try {
        child_plays_with_locked_image();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

int wavplay_test::run_segment_tests()
{
    TestSuite suite;
    suite.run_timed("private renderer pages remain resident through driver cleanup", test_renderer_residency_and_lifetime);
    suite.run_timed("a process that locked its image reports it after a mock cycle and keeps the callback state page locked",
        test_locked_image_in_buffer_setup);
    suite.run_timed("submitted PCM retained after a partial start failure", test_submitted_frames_after_partial_start);
    suite.run_timed("ASIO callback with an empty PCM ring", [] { check_callback_without_pcm(false); });
    suite.run_timed("ASIO callback with a failed PCM refill", [] { check_callback_without_pcm(true); });
    suite.run_timed("read failure between priming and prefill is a read error", [] {
        check_read_failure_before_start(std::chrono::milliseconds(0));
        check_read_failure_before_start(std::chrono::milliseconds(100));
    });
    suite.run_timed("a start-silence switch records a producer failure", test_silent_switch_observes_read_failure);
    suite.run_timed("a refill failure that no callback saw is reported after the producer's join",
        test_read_failure_after_last_callback);
    suite.run_timed("driver pre-roll before hardware starts", [] {
        test_preroll(1, 3, 3, 2);    // All audio in B before start.
        test_preroll(10, 5, 2, 10);  // Ten frames handed over before the hardware starts.
        test_preroll(11, 3, 3, 12);  // Final frame after the hardware starts, with queued pre-roll.
        test_preroll(12, 3, 3, 14);  // Keep sub-block latency in addition to extra startup submissions.
        test_preroll(10, 5, 12, 20); // Conservatively retain extra pre-roll even with a large latency report.
    });
    suite.run_timed("ASIO callback negotiation and invalid indices", test_callback_contract);
    suite.run_timed("partial ASIO buffer allocation failure", test_buffer_allocation_failure);
    suite.run_timed("ASIO latency failure and retry", test_latency_failure);
    suite.run_timed("playback releases stream handles on success and failures", test_playback_resource_lifetime);
    suite.run_timed("ASIO double-buffer drain timing", [] {
        // Pre-filled B starts sounding latency - width frames after switch A.
        test_track(5, 3, 6);
        test_track(5, 5, 8);
        test_track(1, 3, 2);
        test_track(1, 10, 10); // All audio is in prefilled B, followed by several silent switches.
        test_track(2, 2, 2);
        test_track(3, 1, 4);   // The last A is partial; sub-block latency is bounded by the prefill.
        test_track(4, 0, 4);
        test_track(6, 5, 10);  // A full final B and a latency between two switch boundaries.
    });
    suite.run_timed("silence continues for unreported driver queues before stop", test_driver_queue_play_out);
    suite.run_timed("gapless pair within prefilled B", [] { test_gapless(1, 1, 4); });
    suite.run_timed("gapless pair on a buffer boundary", [] { test_gapless(2, 3, 6); });
    suite.run_timed("gapless pair within callback A", [] { test_gapless(3, 2, 6); });
    suite.run_timed("packed 24-bit output across 543/544/545-frame joins", test_packed24_gapless);
    suite.run_timed("mono on both outputs, joined gaplessly to stereo", test_mono_gapless);
    suite.run_timed("gapless repeated file", test_duplicate_track);
    suite.run_timed("gapless playback from validated WAV handles", test_owned_tracks);
    suite.run_timed("drain before a rejected second track", test_rejected_next_track);
    suite.run_timed("Extensible next-track full scan precedes playback and preserves the first track", test_extensible_next_scan);
    suite.run_timed("ASIO preparation and retry", test_prepare_and_retry);
    suite.run_timed("preferred ASIO buffer size and granularity", test_preferred_buffer_size);
    suite.run_timed("ASIO session lifetime", test_session_lifetime);
    suite.run_timed("missing or interrupted ASIO buffer switches", [] { test_missing_switches(false); });
    suite.run_timed("unrelated control wakes cannot renew the ASIO watchdog", [] { test_missing_switches(true); });
    suite.run_timed("every routed ASIO switch advances the watchdog count", test_switch_count);
    suite.run_timed("each watchdog deadline needs a switch after the previous one", test_watchdog_compares_counts);
    suite.run_timed("a sequence fault after a Stop request or a latched Stop is still recorded",
        test_sequence_fault_after_stop);
    suite.run_timed("a block whose phase ends during its copy is silenced", test_phase_ends_during_copy);
    suite.run_timed("a switch overlapping a copy, of either half, is an underrun and pops nothing",
        test_overlapping_switches);
    suite.run_timed("detach waits for a switch still copying", test_detach_waits_for_copy);
    suite.run_timed("steady switches set no control event; each stop reason sets it once",
        test_control_event_only_on_stop_reasons);
    suite.run_timed("ASIO 2.3 overload inquiry and legacy fallback", test_overload_inquiry);
    suite.run_timed("hardware dropout while releasing a drained segment", test_overload_at_drain);
    suite.run_timed("null and partially unwritable ASIO buffers", test_unwritable_output);
    return suite.result("ASIO double-buffer playback");
}
