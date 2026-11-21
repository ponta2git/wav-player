#include "suites/suites.h"
#include "wavplay/playback/playback_signal.h"
#include "wavplay/playback/track_player.h"
#include "doubles/mock_asio.h"
#include "framework/test_suite.h"
#include "fixtures/call_gate.h"
#include "fixtures/wav_builder.h"
#include "fixtures/playback_fixture.h"
#include "fixtures/track_player_fixture.h"
#include "wavplay/media/wav_reader.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;

void test_buffer_size_on_resume()
{
    // Pause inside start before A is filled, retaining exactly the four PCM
    // frames in prefilled B. A subsequent Play must re-query the stopped device.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        TempFile file;
        const auto pcm = samples(7);
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), {"data", pcm}})));
        MockAsio driver;
        driver.buffer_size = {2, 16, 4, 2};
        driver.latency = 4;
        driver.ticks_after_start = {2, 1};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::start && driver.starts == 0) {
                playback.request_pause();
            }
        };
        MockSession session(driver, {.start_silence = no_start_silence, .buffer_frames = scenario == 0 ? 0 : 4});
        wavplay::playback::PlaybackDiagnostics report{};
        expect("pause retains manual-width prefill", playback.play(session, &report), wavplay::playback::PlaybackResult::paused);
        expect("pause position uses actual submitted width", playback.position_frames(), 4);
        expect("first allocation uses four frames", report.segments[0]->buffer_frames, 4);
        const Bytes prefix(pcm.begin(), pcm.begin() + 4 * 4);
        check_submitted_pcm(driver.runs.at(0).submitted, 4, prefix, 8);

        const bool refused = scenario == 2;
        driver.buffer_size = refused ? MockAsio::BufferSize{8, 16, 8, 8} : MockAsio::BufferSize{2, 16, 8, 2};
        const long resumed_width = scenario == 0 ? 8 : 4;
        driver.latency = resumed_width;
        if (refused) {
            expect_throw<std::invalid_argument>("resumed range rejects the retained manual width", [&] {
                (void)playback.play(session, &report);
            });
            expect("refused resume preserves confirmed position", playback.position_frames(), 4);
            expect("refused resume never creates a second allocation", driver.creates, 1);
            expect("refused resume never starts", driver.starts, 1);
            expect_true("refused resume publishes no allocation", !report.segments[0]);
        } else {
            expect("resume completes at the fresh range", playback.play(session, &report), wavplay::playback::PlaybackResult::completed);
            expect("zero follows changed preferred; manual stays exact", report.segments[0]->buffer_frames, resumed_width);
            expect("resumed track reaches EOF", playback.position_frames(), 7);
            const Bytes suffix(pcm.begin() + 4 * 4, pcm.end());
            check_submitted_pcm(driver.runs.at(1).submitted, resumed_width, suffix, resumed_width);
        }
        expect("successful or refused resume replaces the previous capabilities", report.buffer_capabilities[0].value().preferred, 8);
        expect("resumed range retains the current minimum", report.buffer_capabilities[0]->minimum, driver.buffer_size.minimum);
        expect_true("resumed capabilities retain the confirmed clock", report.buffer_capabilities[0]->sample_rate == 48000);
        expect("resume re-queries the device buffer range", driver.size_queries, 2);
    }
}

void test_resident_transport_lifetime()
{
    const auto minimum_working_set = [] {
        SIZE_T minimum{}, maximum{};
        if (!GetProcessWorkingSetSize(GetCurrentProcess(), &minimum, &maximum)) {
            throw std::runtime_error("Cannot inspect transport working-set quota");
        }
        return minimum;
    };
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1)}));
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto before = minimum_working_set();
    {
        wavplay::playback::TrackPlayer playback(path);
        expect("retained transport owns one private locked page", minimum_working_set(), before + system.dwPageSize);
        for (unsigned pass = 0; pass < 2; ++pass) {
            MockAsio driver;
            driver.ticks_after_start = {1};
            MockSession session(driver);
            wavplay::playback::PlaybackDiagnostics diagnostics;
            playback.stop();
            expect("resident transport plays again", playback.play(session, &diagnostics),
                wavplay::playback::PlaybackResult::completed);
            expect_true("TrackPlayer reports its request flags as locked", diagnostics.segments[0]->request_pages_locked);
            expect_true("TrackPlayer reports renderer pages as locked", diagnostics.segments[0]->renderer_pages_locked);
            expect("only retained request page survives completed playback", minimum_working_set(), before + system.dwPageSize);
        }
    }
    expect("transport destruction returns its private page quota", minimum_working_set(), before);
}

void test_idle_stop_and_replay()
{
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x4567);
    for (const bool paused : {false, true}) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
        DacQueueAsio driver(DacQueueAsio::Position::zero, true);
        auto done = start(driver, playback);
        if (paused) {
            submit_until(driver, 2 * DacQueueAsio::block);
            playback.request_pause();
        }
        expect("normal idle state before Stop", static_cast<int>(finish(driver, done)),
            static_cast<int>(paused ? wavplay::playback::PlaybackResult::paused : wavplay::playback::PlaybackResult::completed));
        playback.stop();
        playback.stop();
        expect("idle Stop resets position", playback.position_frames(), 0);
        expect("idle Stop needs no new buffers", driver.creates, 1);
        expect("idle Stop does not stop driver twice", driver.stops, 1);
        expect("idle Stop does not dispose twice", driver.disposals, 1);
        clear_capture(driver);
        // Asynchronous requests made while idle must not cancel the next Play.
        playback.request_pause();
        playback.request_stop();
        done = start(driver, playback);
        expect("Play after idle Stop restarts", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::completed));
        check_heard(driver, data.payload, frames);
    }
}

void test_stop_pause_priority()
{
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x2345);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
    DacQueueAsio driver(DacQueueAsio::Position::zero);
    for (unsigned pass = 0; pass < 4; ++pass) {
        clear_capture(driver);
        auto done = start(driver, playback);
        submit_until(driver, 2 * DacQueueAsio::block);
        if (pass % 2 == 0) {
            playback.request_pause();
            playback.request_stop();
        } else {
            playback.request_stop();
            playback.request_pause();
        }
        playback.request_stop();
        expect("Stop wins over Pause in either order", static_cast<int>(finish_without_callbacks(done)),
            static_cast<int>(wavplay::playback::PlaybackResult::stopped));
        expect("repeated Stop rewinds once", playback.position_frames(), 0);
        expect("one stop per active run", driver.stops, pass + 1);
        expect("one dispose per active run", driver.disposals, pass + 1);
        check_heard(driver, data.payload, driver.heard.size(), 3 * DacQueueAsio::block, true);
    }
    clear_capture(driver);
    auto done = start(driver, playback);
    expect("repeated Stop leaves a reusable request", static_cast<int>(finish(driver, done)),
        static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_heard(driver, data.payload, frames);
}

void test_stop_handoffs()
{
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x3456);
    // The request can arrive inside start, after a pause drain, after an EOF
    // drain, or while the allocation is being retired. It still wins until return.
    for (unsigned where = 0; where < 4; ++where) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
        DacQueueAsio driver(DacQueueAsio::Position::zero, true);
        if (where == 0) {
            driver.before_preroll = [&] {
                expect_throw<std::logic_error>("idle stop API rejects an active request", [&] { playback.stop(); });
                playback.request_stop();
            };
        } else if (where < 3) {
            driver.before_stop = [&] { playback.request_stop(); };
        } else {
            driver.on_call = [&](MockAsio::Call call) {
                if (call == MockAsio::Call::dispose) {
                    playback.request_stop();
                }
            };
        }
        auto done = start(driver, playback);
        if (where == 1) {
            submit_until(driver, 2 * DacQueueAsio::block);
            playback.request_pause();
        }
        expect("Stop accepted during driver handoff", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::stopped));
        expect("handoff Stop resets position", playback.position_frames(), 0);
        expect("handoff Stop stops once", driver.stops, 1);
        expect("handoff Stop disposes once", driver.disposals, 1);
        driver.before_preroll = {};
        driver.before_stop = {};
        driver.on_call = {};
        clear_capture(driver);
        done = start(driver, playback);
        expect("handoff Stop restarts from the beginning", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::completed));
        check_heard(driver, data.payload, frames);
    }
}

void test_stop_during_setup()
{
    constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x5678);
    for (const bool packed24 : {false, true}) {
        for (const auto boundary : {MockAsio::Call::rate, MockAsio::Call::create, MockAsio::Call::latency}) {
            TempFile file;
            wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
            DacQueueAsio driver(DacQueueAsio::Position::zero, packed24);
            bool requested{};
            driver.on_call = [&](MockAsio::Call call) {
                if (call == boundary && !requested) {
                    requested = true;
                    playback.request_stop();
                }
            };
            // This hook runs on the control thread inside the driver call.
            // No callback or timer is needed to make progress through setup.
            expect("Stop during setup returns stopped", static_cast<int>(play_in_session(driver, playback, 3ms)),
                static_cast<int>(wavplay::playback::PlaybackResult::stopped));
            expect("setup Stop boundary was reached", static_cast<int>(requested), 1);
            expect("setup Stop rewinds position", playback.position_frames(), 0);
            expect("setup Stop never starts the driver", driver.starts, 0);
            expect("setup Stop does not stop an unstarted driver", driver.stops, 0);
            const unsigned allocations = boundary == MockAsio::Call::rate ? 0 : 1;
            expect("setup Stop allocates only when required", driver.creates, allocations);
            expect("setup Stop disposes only allocated buffers", driver.disposals, allocations);
            expect("setup Stop submits no PCM", driver.heard.size(), 0);
            if (allocations) {
                expect("setup Stop leaves unstarted buffers untouched", static_cast<int>(driver.untouched_on_dispose), 1);
            }
            driver.on_call = {};
            auto done = start(driver, playback);
            expect("Play after setup Stop completes", static_cast<int>(finish(driver, done)),
                static_cast<int>(wavplay::playback::PlaybackResult::completed));
            check_heard(driver, data.payload, frames);
        }
    }
}

void test_stop_setup_faults()
{
    for (const auto boundary : {MockAsio::Call::rate, MockAsio::Call::create, MockAsio::Call::latency}) {
        for (const long selector : {kAsioResetRequest, kAsioOverload}) {
            for (const bool fault_first : {false, true}) {
                TempFile file;
                wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
                DacQueueAsio driver(DacQueueAsio::Position::zero);
                // Retain the registered notification entry points after one
                // healthy Stop, as a real driver can between allocations.
                auto done = start(driver, playback);
                playback.request_stop();
                expect("initial Stop before setup fault", static_cast<int>(finish_without_callbacks(done)),
                    static_cast<int>(wavplay::playback::PlaybackResult::stopped));
                bool notified{};
                driver.on_call = [&](MockAsio::Call call) {
                    if (call != boundary || notified) {
                        return;
                    }
                    notified = true;
                    if (!fault_first) {
                        playback.request_stop();
                    }
                    expect("setup fault notification acknowledged", driver.message_notification(selector, 0, nullptr, nullptr), 1);
                    if (fault_first) {
                        playback.request_stop();
                    }
                };
                const auto expected = selector == kAsioOverload ? wavplay::playback::PlaybackResult::underrun :
                    wavplay::playback::PlaybackResult::reinitialize_required;
                expect("setup fault wins over Stop in either order", static_cast<int>(play_in_session(driver, playback, 3ms)),
                    static_cast<int>(expected));
                expect("setup fault boundary was reached", static_cast<int>(notified), 1);
                expect("faulted setup never starts a new stream", driver.starts, 1);
                expect("faulted setup does not stop an unstarted stream", driver.stops, 1);
                const unsigned allocations = boundary == MockAsio::Call::rate ? 1 : 2;
                expect("faulted setup allocation count", driver.creates, allocations);
                expect("faulted setup disposes every allocation", driver.disposals, allocations);
                driver.on_call = {};
                playback.stop();
                expect_throw<std::logic_error>("Stop cannot clear a setup fault", [&] { (void)play_in_session(driver, playback); });
            }
        }
    }
}

void test_stop_with_live_producer()
{
    // One partial block beyond the two-second ring keeps the producer alive
    // and waiting for consumption; the short boundary cases buffer all PCM.
    constexpr std::size_t frames = 2 * 48000 + 11;
    const auto data = pcm16_ramp(2, frames, 0x6789);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
    DacQueueAsio driver(DacQueueAsio::Position::zero, true);
    auto done = start(driver, playback);
    playback.request_stop();
    expect("Stop wakes and joins the waiting producer", static_cast<int>(finish_without_callbacks(done)),
        static_cast<int>(wavplay::playback::PlaybackResult::stopped));
    expect("Stop with unread WAV resets position", playback.position_frames(), 0);
    expect("Stop with unread WAV disposes buffers", driver.disposals, 1);
    clear_capture(driver);
    done = start(driver, playback);
    // A Windows scheduling wait can exceed the requested millisecond. Advance
    // several virtual DAC periods per wait so 2,001 blocks do not consume the
    // watchdog solely because of timer granularity; keep the five-second bound.
    expect("Play after stopping a live producer completes", static_cast<int>(finish(driver, done, 16)),
        static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("restarted long WAV reaches EOF", playback.position_frames(), frames);
    check_heard(driver, data.payload, frames);
}

void test_stop_faults()
{
    for (unsigned where = 0; where < 3; ++where) {
        for (const long selector : {kAsioOverload, kAsioResetRequest}) {
            TempFile file;
            wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
            DacQueueAsio driver(DacQueueAsio::Position::zero);
            auto done = start(driver, playback);
            if (where == 0) {
                driver.fault(selector);
            } else if (where == 1) {
                driver.fault_on_stop = selector;
            } else {
                driver.fault_on_dispose = selector;
            }
            playback.request_stop();
            const auto wanted = selector == kAsioOverload ? wavplay::playback::PlaybackResult::underrun : wavplay::playback::PlaybackResult::reinitialize_required;
            expect("driver fault wins over Stop", static_cast<int>(finish_without_callbacks(done)), static_cast<int>(wanted));
            expect("faulted Stop disposes buffers", driver.disposals, 1);
            playback.stop();
            expect_throw<std::logic_error>("Stop cannot clear a driver fault", [&] { (void)play_in_session(driver, playback); });
            expect("faulted Stop never restarts", driver.starts, 1);
        }
    }
    for (const bool failed_stop : {false, true}) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
        DacQueueAsio driver(DacQueueAsio::Position::zero);
        driver.fail_stop = failed_stop;
        driver.fail_dispose = !failed_stop;
        auto done = start(driver, playback);
        playback.request_stop();
        expect_throw<std::runtime_error>("Stop surfaces a driver teardown failure", [&] { (void)finish_without_callbacks(done); });
        expect("failed Stop attempts buffer disposal", driver.disposals, 1);
        playback.stop();
        expect_throw<std::logic_error>("Stop cannot reset teardown failure", [&] { (void)play_in_session(driver, playback); });
    }
}

void test_stop_preserves_idle_faults()
{
    for (unsigned where = 0; where < 4; ++where) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
        DacQueueAsio driver(DacQueueAsio::Position::zero);
        auto done = start(driver, playback);
        if (where == 2) {
            playback.request_stop();
            expect("active Stop before idle fault", static_cast<int>(finish_without_callbacks(done)),
                static_cast<int>(wavplay::playback::PlaybackResult::stopped));
        } else {
            if (where != 3) {
                playback.request_pause();
            }
            expect("healthy state before idle Stop", static_cast<int>(finish(driver, done)),
                static_cast<int>(where == 3 ? wavplay::playback::PlaybackResult::completed : wavplay::playback::PlaybackResult::paused));
        }
        if (where == 0) {
            driver.rate_notification(0);
        }
        playback.stop();
        expect("idle Stop clears displayed position", playback.position_frames(), 0);
        if (where != 0) {
            driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        }
        // Both notifications abort the request, but only reset permits reinitialization.
        const auto fault = where == 0 ? wavplay::playback::PlaybackResult::driver_error :
            wavplay::playback::PlaybackResult::reinitialize_required;
        if (where == 3) {
            // Idle supervision reported the fault first, so Play only refuses.
            expect("idle supervision reports the fault preserved by Stop",
                static_cast<int>(playback.take_idle_fault() == fault), 1);
        } else {
            expect("idle Stop preserves fault through next Play", static_cast<int>(play_in_session(driver, playback)),
                static_cast<int>(fault));
            expect("Play reporting an idle fault keeps the stopped position", playback.position_frames(), 0);
        }
        expect("an idle fault is reported once", static_cast<int>(playback.take_idle_fault().has_value()), 0);
        expect_throw<std::logic_error>("reported idle fault refuses a later Play", [&] { (void)play_in_session(driver, playback); });
        expect("idle fault never creates another allocation", driver.creates, 1);
        expect("idle fault never starts another stream", driver.starts, 1);
    }
}

void test_boundaries()
{
    for (const auto position : {DacQueueAsio::Position::advancing, DacQueueAsio::Position::zero, DacQueueAsio::Position::unsupported}) {
        for (const auto output_type : {ASIOSTInt16LSB, ASIOSTInt24LSB, ASIOSTInt32LSB}) {
            for (const std::uint64_t boundary : {0ULL, 3ULL * DacQueueAsio::block, 7ULL * DacQueueAsio::block}) {
                test_pause_boundary(position, output_type, boundary);
            }
        }
    }
}

void test_preroll_and_latency()
{
    // No synchronous first callback, sub-block reports, and extra startup
    // submissions all change the relationship between submitted and heard PCM.
    test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt24LSB, 0, true, 0, 0);
    test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt24LSB, 3 * DacQueueAsio::block, true, 0, DacQueueAsio::block - 1);
    test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt24LSB, 2 * DacQueueAsio::block, false, 0, 193);
    test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt24LSB, 6 * DacQueueAsio::block, false, 5, 73);
    test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt24LSB, 7 * DacQueueAsio::block, true, 5, 193);
}

void test_pause_during_start()
{
    for (const auto silence : {0ms, 3ms}) {
        constexpr std::size_t frames = 7 * DacQueueAsio::block + 11;
        const auto data = pcm16_ramp(2, frames, 0x1234);
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
        DacQueueAsio driver(DacQueueAsio::Position::zero, true);
        driver.preroll_switches = 5;
        driver.before_preroll = [&] { playback.request_pause(); };
        auto done = start(driver, playback, silence);
        expect("pause inside start", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::paused));
        const std::size_t expected = silence == 0ms ? DacQueueAsio::block : 0;
        expect("only prefilled PCM retained", playback.position_frames(), expected);
        check_heard(driver, data.payload, expected, silence == 0ms ? 0 : 3 * DacQueueAsio::block);
        driver.before_preroll = {};
        done = start(driver, playback, silence);
        expect("resume after pause inside start", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::completed));
        check_heard(driver, data.payload, frames, silence == 0ms ? 0 : 3 * DacQueueAsio::block);
    }
}

void test_repeated_pause()
{
    constexpr std::size_t frames = 9 * DacQueueAsio::block + 7;
    const auto data = pcm16_ramp(2, frames, 0x4321);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
    DacQueueAsio driver(DacQueueAsio::Position::zero);
    driver.preroll_switches = 3;
    for (unsigned pass = 1; pass <= 3; ++pass) {
        auto done = start(driver, playback);
        submit_until(driver, 2 * DacQueueAsio::block);
        playback.request_pause();
        playback.request_pause(); // Repeated requests must not consume another source block.
        expect("repeated pause", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::paused));
        expect("repeated pause position", playback.position_frames(), pass * 2 * DacQueueAsio::block);
        check_heard(driver, data.payload, pass * 2 * DacQueueAsio::block);
    }
    auto done = start(driver, playback);
    expect("finish after repeated pause", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    check_heard(driver, data.payload, frames);
}

void test_pause_after_eof()
{
    constexpr std::size_t frames = 2 * DacQueueAsio::block + 11;
    const auto data = pcm16_ramp(2, frames, 0x1234);
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), data})));
    DacQueueAsio driver(DacQueueAsio::Position::unsupported);
    auto done = start(driver, playback);
    submit_until(driver, frames);
    playback.request_pause();
    expect("EOF drain takes precedence", static_cast<int>(finish(driver, done)), static_cast<int>(wavplay::playback::PlaybackResult::completed));
    expect("EOF position", playback.position_frames(), frames);
    check_heard(driver, data.payload, frames);
}

void test_pause_faults()
{
    for (unsigned where = 0; where < 3; ++where) {
        for (const long selector : {kAsioOverload, kAsioResetRequest}) {
            TempFile file;
            wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
            DacQueueAsio driver(DacQueueAsio::Position::zero);
            auto done = start(driver, playback);
            submit_until(driver, 2 * DacQueueAsio::block);
            playback.request_pause();
            driver.advance_dac_block(); // Accept pause, but leave the queued PCM in the drain.
            if (where == 0) {
                driver.fault(selector);
            } else if (where == 1) {
                driver.fault_on_stop = selector;
            } else {
                driver.fault_on_dispose = selector;
            }
            const auto wanted = selector == kAsioOverload ? wavplay::playback::PlaybackResult::underrun : wavplay::playback::PlaybackResult::reinitialize_required;
            expect("fault overrides pause", static_cast<int>(finish(driver, done)), static_cast<int>(wanted));
            expect("fault does not commit source position", playback.position_frames(), 0);
            expect("fault disposes buffers", driver.disposals, 1);
            expect_throw<std::logic_error>("faulted playback cannot resume", [&] { (void)play_in_session(driver, playback); });
            expect("faulted playback never restarts", driver.starts, 1);
        }
    }
    for (const bool start_failure : {false, true}) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
        DacQueueAsio driver(DacQueueAsio::Position::zero);
        driver.fail_start = start_failure;
        driver.fail_stop = !start_failure;
        auto done = start(driver, playback);
        if (!start_failure) {
            playback.request_pause();
        }
        expect_throw<std::runtime_error>("failed ASIO start or stop", [&] { (void)finish(driver, done); });
        expect("failed stop does not commit position", playback.position_frames(), 0);
        expect("failed ASIO call disposes buffers", driver.disposals, 1);
        expect_throw<std::logic_error>("failed playback cannot resume", [&] { (void)play_in_session(driver, playback); });
    }
}

void test_read_failure_with_request(bool stop)
{
    TempFile file;
    constexpr std::size_t two_seconds = 2 * 48000;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3 * 48000 + 11, 1)}));
    wavplay::playback::TrackPlayer playback(path);
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open pause failure lock");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    const auto offset = static_cast<DWORD>(wavplay::media::WavReader(path).info().data_offset + two_seconds * 4);
    // The initial two seconds remain readable, but the first refill fails.
    if (!LockFile(lock.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock pause refill");
    }
    DacQueueAsio driver(DacQueueAsio::Position::zero);
    bool request_during_failed_stop{};
    driver.before_stop = [&] {
        // Synchronize on the actual failure, not an assumed producer delay.
        // This is the control thread's stop handoff, before disposal and before
        // play() can decide whether the position is resumable.
        request_during_failed_stop = wavplay::playback::notifications().result() == wavplay::playback::PlaybackResult::read_error;
        if (stop) {
            playback.request_stop();
            playback.request_stop();
        } else {
            playback.request_pause();
            playback.request_pause();
        }
    };
    auto done = start(driver, playback);
    // Advance past the first refill threshold while PCM is still buffered.
    for (unsigned tick = 0; tick < 1004; ++tick) {
        driver.advance_dac_block();
    }
    expect("read failure takes precedence over user request", static_cast<int>(finish(driver, done)),
        static_cast<int>(wavplay::playback::PlaybackResult::read_error));
    expect("user request coincides with failed stop", static_cast<int>(request_during_failed_stop), 1);
    const auto cause = playback.result_cause();
    expect_true("read failure keeps the producer's cause", static_cast<bool>(cause));
    try {
        std::rethrow_exception(cause);
    } catch (const std::system_error& error) {
        expect_true("read failure cause is ReadFile's lock violation",
            error.code() == std::error_code(ERROR_LOCK_VIOLATION, std::system_category()) &&
            std::string_view(error.what()).starts_with("ReadFile"));
    }
    expect("read failure does not commit source position", playback.position_frames(), 0);
    expect("read failure stops the driver", driver.stops, 1);
    expect("read failure disposes the buffers", driver.disposals, 1);
    if (stop) {
        playback.stop();
    }
    expect_throw<std::logic_error>("read failure cannot resume", [&] { (void)play_in_session(driver, playback); });
    expect("read failure never restarts", driver.starts, 1);
}

void test_fault_while_paused()
{
    enum class IdleFault { reset, overload, lost_clock };
    for (const auto fault : {IdleFault::reset, IdleFault::overload, IdleFault::lost_clock}) {
        TempFile file;
        wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1000, 1)})));
        DacQueueAsio driver(DacQueueAsio::Position::zero);
        auto done = start(driver, playback);
        submit_until(driver, 2 * DacQueueAsio::block);
        playback.request_pause();
        expect("normal pause before idle fault", static_cast<int>(finish(driver, done)),
            static_cast<int>(wavplay::playback::PlaybackResult::paused));
        expect("confirmed position before idle fault", playback.position_frames(), 2 * DacQueueAsio::block);
        if (fault == IdleFault::lost_clock) {
            driver.rate_notification(0);
        } else {
            expect("idle notification acknowledged", driver.message_notification(
                fault == IdleFault::overload ? kAsioOverload : kAsioResetRequest, 0, nullptr, nullptr), 1);
        }
        const auto wanted = fault == IdleFault::overload ? wavplay::playback::PlaybackResult::underrun :
            fault == IdleFault::reset ? wavplay::playback::PlaybackResult::reinitialize_required :
            wavplay::playback::PlaybackResult::driver_error;
        expect("fault while paused ends the next Play instead of resuming", static_cast<int>(play_in_session(driver, playback)),
            static_cast<int>(wanted));
        expect("idle fault does not restart", driver.starts, 1);
        expect("idle fault does not create new buffers", driver.creates, 1);
        expect("idle fault preserves confirmed position", playback.position_frames(), 2 * DacQueueAsio::block);
        expect("Play reported the idle fault", static_cast<int>(playback.take_idle_fault().has_value()), 0);
        if (fault == IdleFault::reset) {
            // Close can race recovery after the reset was already reported.
            // Its normal clock restoration is harmless, but a new fatal fault
            // during Release must supersede the cached reinitialization request.
            playback.prepare_release();
            driver.rate_notification(44100);
            expect_true("restoration does not report reset twice", !playback.take_idle_fault());
            driver.message_notification(kAsioOverload, 0, nullptr, nullptr);
            expect_true("final release reports a fatal fault after a reported reset",
                playback.take_idle_fault() == wavplay::playback::PlaybackResult::underrun);
            expect_true("superseding fatal fault is reported once", !playback.take_idle_fault());
        }
        expect_throw<std::logic_error>("idle fault remains latched", [&] { (void)play_in_session(driver, playback); });
        expect("latched idle fault does not restart", driver.starts, 1);
        expect("latched idle fault preserves confirmed position", playback.position_frames(), 2 * DacQueueAsio::block);
    }
}

void test_owned_pause_request()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    {
        wavplay::media::WavReader reader(path);
        wavplay::playback::TrackPlayer playback(std::move(reader));
        MockAsio driver;
        driver.ticks_after_start = {1};
        MockSession session(driver);
        expect("validated pause request completes", static_cast<int>(playback.play(session)),
            static_cast<int>(wavplay::playback::PlaybackResult::completed));
        check_submitted_pcm(driver.submitted, driver.width, samples(1), 2);
        expect("retained file protects later resume", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);
    }
    expect("request releases retained file", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    wavplay::media::WavReader advanced(path);
    advanced.seek_frame(1);
    expect_throw<std::invalid_argument>("pause request rejects advanced reader", [&] {
        wavplay::playback::TrackPlayer playback(std::move(advanced));
    });
}

void test_playlist_many_short_tracks()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second;
    const auto a = samples(1, 0x31), b = samples(3, 0x71);
    const std::wstring first_path = first.write(wave({pcm16(2, 48000), {"data", a}}));
    const std::wstring second_path = second.write(wave({pcm16(2, 48000), {"data", b}}));
    std::vector<std::wstring> paths;
    Bytes expected;
    for (unsigned index = 0; index < 101; ++index) {
        const bool odd = index % 2 != 0;
        paths.push_back(odd ? second_path : first_path);
        const auto& pcm = odd ? b : a;
        expected.insert(expected.end(), pcm.begin(), pcm.end());
    }
    wavplay::playback::TrackPlayer playback(paths);
    MockAsio driver;
    driver.buffer_size = {16, 16, 16, 0};
    driver.latency = 0;
    driver.ticks_after_start = {20};
    MockSession session(driver);
    DWORD before{}, during{};
    expect_true("read process handles", GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE);
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start) {
            expect_true("read playing process handles", GetProcessHandleCount(GetCurrentProcess(), &during) != FALSE);
        }
    };
    wavplay::playback::PlaybackDiagnostics diagnostics;
    expect("101 short duplicate tracks complete", playback.play(session, &diagnostics), completed);
    expect("same-rate list uses one allocation", driver.creates, 1);
    expect("last duplicate remains selected", playback.track_index(), 100);
    expect("last duplicate has its own EOF position", playback.position_frames(), 1);
    expect_true("duplicate entries share protected handles", during <= before + 12);
    check_submitted_pcm(driver.runs[0].submitted, 16, expected, driver.runs[0].stopped_at);
    playback.stop();
    expect("Stop retains final selected item", playback.track_index(), 100);
    expect("Stop rewinds final item", playback.position_frames(), 0);
}

void test_playlist_pause_across_boundary()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second, third;
    const auto a = samples(3, 0x11), b = samples(10, 0x41), c = samples(4, 0x71);
    const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", a}}));
    const std::wstring pb = second.write(wave({pcm16(2, 48000), {"data", b}}));
    const std::wstring pc = third.write(wave({pcm16(2, 48000), {"data", c}}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb, pc});
    MockAsio driver;
    driver.buffer_size = {8, 8, 8, 0};
    driver.latency = 8;
    driver.ticks_after_start = {4, 4};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start && driver.starts == 0) { playback.request_pause(); }
    };
    MockSession session(driver);
    expect("pause after a boundary in prefilled B", playback.play(session), paused);
    expect("pause selects second track", playback.track_index(), 1);
    expect("pause retains only second-track submitted frames", playback.position_frames(), 5);
    Bytes prefix = a;
    prefix.insert(prefix.end(), b.begin(), b.begin() + 5 * 4);
    check_submitted_pcm(driver.runs[0].submitted, 8, prefix, driver.runs[0].stopped_at);
    expect("resume continues the remainder of the list", playback.play(session), completed);
    expect("resume reaches third track", playback.track_index(), 2);
    expect("resume reaches third-track EOF", playback.position_frames(), 4);
    Bytes suffix(b.begin() + 5 * 4, b.end());
    suffix.insert(suffix.end(), c.begin(), c.end());
    check_submitted_pcm(driver.runs[1].submitted, 8, suffix, driver.runs[1].stopped_at);
    playback.select(1);
    expect("explicit selection resets position", playback.position_frames(), 0);
}

void test_playlist_many_rates_and_refusal()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second;
    const auto pcm = samples(1, 0x51);
    const std::wstring pa = first.write(wave({pcm16(2, 44100), {"data", pcm}}));
    const std::wstring pb = second.write(wave({pcm16(2, 48000), {"data", pcm}}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb, pa, pb, pa, pb});
    MockAsio driver;
    driver.sample_rate = 44100;
    driver.latency = driver.next_latency = 0;
    driver.ticks_after_start = std::vector<unsigned>(8, 5);
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::start && driver.starts == 1) {
            expect("duplicate protects its handle across another rate", probe_file_access(pa.c_str(), GENERIC_WRITE),
                ERROR_SHARING_VIOLATION);
        }
        if (call == MockAsio::Call::start && driver.starts == 5) {
            expect("final use releases a previous duplicate", probe_file_access(pa.c_str(), GENERIC_WRITE), ERROR_SUCCESS);
        }
    };
    MockSession session(driver);
    wavplay::playback::PlaybackDiagnostics diagnostics;
    expect("six rate segments complete", playback.play(session, &diagnostics), completed);
    expect("six fresh ASIO allocations", driver.creates, 6);
    expect("six diagnostic entries", diagnostics.segments.size(), 6);
    expect("latest diagnostic segment", diagnostics.current_segment, 5);
    expect("sixth track EOF", playback.track_index(), 5);
    for (const auto& run : driver.runs) {
        check_submitted_pcm(run.submitted, run.width, pcm, run.stopped_at);
    }
    playback.select(0);
    driver.sample_rate = 44100;
    driver.can_rate_result = ASE_NoClock;
    expect("later rate refusal drains preceding track", playback.play(session), next_rate_rejected);
    expect("rate refusal selects unstarted second item", playback.track_index(), 1);
    expect("rate refusal resets position", playback.position_frames(), 0);
    expect("rate refusal keeps the rejected WAV rate for timing", playback.sample_rate(), 48000);
    expect("rate refusal keeps the rejected WAV length for timing", playback.frame_count(), 1);
    driver.can_rate_result = ASE_OK;
    playback.select(5);
    expect("explicit selection retries after rate refusal", playback.play(session), completed);
}

void test_playlist_rejected_track_and_retry()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, incompatible, last;
    const auto a = samples(3, 0x21), c = samples(2, 0x61);
    const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", a}}));
    const std::wstring pb = incompatible.write(wave({pcm_integer(2, 48000, 24), pcm24_ramp(2, 1)}));
    const std::wstring pc = last.write(wave({pcm16(2, 48000), {"data", c}}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pa, pb, pc});
    MockAsio driver;
    driver.latency = 0;
    driver.ticks_after_start = {8, 8};
    MockSession session(driver);
    expect("unconvertible third item refuses without skipping", playback.play(session), next_track_rejected);
    expect("third item becomes selected", playback.track_index(), 2);
    Bytes prefix = a;
    prefix.insert(prefix.end(), a.begin(), a.end());
    check_submitted_pcm(driver.runs[0].submitted, 2, prefix, driver.runs[0].stopped_at);
    expect("bad item itself remains retryable", playback.play(session), next_track_rejected);
    expect("refused first item does not start", driver.starts, 1);
    playback.select(3);
    expect("later explicit item can play", playback.play(session), completed);
    check_submitted_pcm(driver.runs[1].submitted, 2, c, driver.runs[1].stopped_at);
}

void test_playlist_refusal_keeps_idle_supervision()
{
    using enum wavplay::playback::PlaybackResult;
    for (unsigned fault = 0; fault < 3; ++fault) {
        TempFile first, incompatible;
        const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
        const std::wstring pb = incompatible.write(wave({pcm_integer(2, 48000, 24), pcm24_ramp(2, 1)}));
        wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb});
        MockAsio driver;
        driver.latency = 0;
        driver.ticks_after_start = {3};
        MockSession session(driver);
        expect("first refusal retains device", playback.play(session), next_track_rejected);
        expect("explicit retry refuses the same item", playback.play(session), next_track_rejected);
        expect_true("refusal remains supervised", playback.idle_fault_event() != nullptr);
        if (fault == 0) { driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr); }
        else if (fault == 1) { driver.message_notification(kAsioOverload, 0, nullptr, nullptr); }
        else { driver.rate_notification(44100); }
        const auto expected = fault == 0 ? reinitialize_required : fault == 1 ? underrun : driver_error;
        expect_true("post-refusal idle fault is observed", playback.take_idle_fault() == expected);
        playback.select(0);
        expect_throw<std::logic_error>("selection cannot erase an idle device fault", [&] { (void)playback.play(session); });
    }
}

void test_playlist_width_refusal()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second, third;
    const auto a = samples(3, 0x21), c = samples(2, 0x61);
    const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", a}}));
    const std::wstring pb = second.write(wave({pcm16(2, 44100), {"data", samples(2, 0x41)}}));
    const std::wstring pc = third.write(wave({pcm16(2, 48000), {"data", c}}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb, pc});
    MockAsio driver;
    driver.latency = driver.next_latency = 0;
    driver.ticks_after_start = {4, 4};
    // The configured width fits only the range the driver reports at 48 kHz.
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::size) {
            driver.buffer_size = driver.sample_rate == 48000 ? MockAsio::BufferSize{2, 32, 4, 2} : MockAsio::BufferSize{2, 4, 4, 2};
        }
    };
    MockSession session(driver, {.start_silence = no_start_silence, .buffer_frames = 8});
    wavplay::playback::PlaybackDiagnostics diagnostics;
    expect("a later item's unsupported width refuses that item", playback.play(session, &diagnostics), next_track_rejected);
    expect("width refusal selects the refused item", playback.track_index(), 1);
    expect("width refusal resets position", playback.position_frames(), 0);
    expect("width refusal keeps the refused WAV rate for timing", playback.sample_rate(), 44100);
    expect_true("width refusal keeps the refused range without an allocation",
        diagnostics.buffer_capabilities[1].value().maximum == 4 && !diagnostics.segments[1]);
    check_submitted_pcm(driver.runs[0].submitted, 8, a, driver.runs[0].stopped_at);
    expect("refused item remains retryable", playback.play(session), next_track_rejected);
    expect("width refusal never allocates or starts the refused item", driver.creates, 1);
    expect_true("width refusal remains supervised", playback.idle_fault_event() != nullptr);
    playback.select(2);
    expect("a later explicit item plays after a width refusal", playback.play(session), completed);
    check_submitted_pcm(driver.runs[1].submitted, 8, c, driver.runs[1].stopped_at);
}

void test_playlist_late_invalid_file()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, removed;
    const auto pcm = samples(2, 0x31);
    const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const std::wstring pb = removed.write(wave({pcm16(2, 48000), {"data", pcm}}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pa, pb, pa});
    expect_true("delete unplayed playlist file", DeleteFileW(pb.c_str()) != FALSE);
    MockAsio driver;
    driver.latency = 0;
    driver.ticks_after_start = {5, 5};
    MockSession session(driver);
    // Both the reader's open and the retry's reopen keep CreateFileW's own error.
    const auto expect_missing_cause = [&](const std::string& name) {
        const auto cause = playback.result_cause();
        expect_true(name + " keeps the open failure", static_cast<bool>(cause));
        try {
            std::rethrow_exception(cause);
        } catch (const std::system_error& error) {
            expect_true(name + " is CreateFileW's missing file",
                error.code() == std::error_code(ERROR_FILE_NOT_FOUND, std::system_category()) &&
                std::string_view(error.what()).starts_with("CreateFileW"));
        }
    };
    expect("removed third track refuses after first two", playback.play(session), next_track_rejected);
    expect_missing_cause("removed track refusal");
    expect("removed track is selected", playback.track_index(), 2);
    expect("removed track has zero position", playback.position_frames(), 0);
    expect("removed track has no retained metadata", playback.frame_count(), 0);
    Bytes expected = pcm;
    expected.insert(expected.end(), pcm.begin(), pcm.end());
    check_submitted_pcm(driver.runs[0].submitted, 2, expected, driver.runs[0].stopped_at);
    expect("missing selected item can be retried", playback.play(session), next_track_rejected);
    expect_missing_cause("missing item's retry");
    expect("a failed reopen never starts the driver", driver.starts, 1);
    playback.select(3);
    expect("later valid selection works after missing file", playback.play(session), completed);
}

void test_playlist_validates_whole_run_before_start()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile first, valid, invalid;
    const auto pcm = samples(2, 0x31);
    const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", pcm}}));
    const std::wstring pb = valid.write(wave({extensible_pcm(2, 48000, 24, 32), pcm24_in32_ramp(2, 3)}));
    auto bad = pcm24_in32_ramp(2, 3);
    bad.payload.back() = 0x01;
    bad.payload[bad.payload.size() - 4] = 0x01; // Unused low bits in the final source sample.
    const std::wstring pc = invalid.write(wave({extensible_pcm(2, 48000, 24, 32), bad}));
    wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb, pa, pc, pa});
    MockAsio driver;
    driver.left_type = driver.right_type = ASIOSTInt24LSB;
    driver.latency = 0;
    driver.ticks_after_start = {8};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::create) {
            expect("later same-rate 24/32 source protected before buffers", probe_file_access(pb.c_str(), GENERIC_WRITE),
                ERROR_SHARING_VIOLATION);
            expect("invalid fourth source discarded before buffers", probe_file_access(pc.c_str(), GENERIC_WRITE), ERROR_SUCCESS);
            playback.request_stop();
        }
    };
    MockSession session(driver);
    expect("Stop after preparation cancels start", playback.play(session), stopped);
    expect("prepared list never starts after Stop", driver.starts, 0);
    expect("stopped list remains at first item", playback.track_index(), 0);
    driver.on_call = {};
    expect("bad fourth 24/32 input is a next-item refusal", playback.play(session), next_track_rejected);
    expect("fourth item selected after valid prefix", playback.track_index(), 3);
    expect("valid prefix uses one ASIO run", driver.starts, 1);
}

void test_playlist_stop_while_run_is_prepared()
{
    using enum wavplay::playback::PlaybackResult;
    // The reader validates every following same-rate item before priming. The
    // gate holds its open of the second item while a Stop, or a driver reset
    // that ends the request's phase, arrives. The reader must not open the
    // third item, and the request must end without allocating buffers. With a
    // WM_QUIT already queued on the control STA, the priming wait neither
    // dispatches nor polls, so no control poll moves the signal for the Stop:
    // only the reader's own check of the Stop request ends that run.
    enum class Arrival { stop, reset, stop_with_quit };
    for (const auto arrival : {Arrival::stop, Arrival::reset, Arrival::stop_with_quit}) {
        const bool reset = arrival == Arrival::reset, quit = arrival == Arrival::stop_with_quit;
        TempFile first, second, third;
        const auto a = samples(3, 0x21), b = samples(5, 0x41), c = samples(2, 0x61);
        const std::wstring pa = first.write(wave({pcm16(2, 48000), {"data", a}}));
        const std::wstring pb = second.write(wave({pcm16(2, 48000), {"data", b}}));
        const std::wstring pc = third.write(wave({pcm16(2, 48000), {"data", c}}));
        wavplay::playback::TrackPlayer playback(std::vector<std::wstring>{pa, pb, pc});
        MockAsio driver;
        driver.latency = 0;
        driver.ticks_after_start = {12, 12};
        Bytes expected = a;
        expected.insert(expected.end(), b.begin(), b.end());
        expected.insert(expected.end(), c.begin(), c.end());
        if (reset) {
            // A complete run hands the driver the callbacks it notifies through.
            MockSession session(driver);
            expect("the list plays before the reset case", playback.play(session), completed);
            playback.select(0);
        }
        const auto creates = driver.creates, starts = driver.starts;
        OpenGate second_open(pb.c_str()), third_open(pc.c_str());
        bool quit_left{};
        TestThread<wavplay::playback::PlaybackResult> done([&] {
            MockSession session(driver); // The control thread is a COM STA from here.
            if (quit) {
                PostQuitMessage(47);
            }
            const auto result = playback.play(session);
            if (quit) {
                MSG message{};
                quit_left = PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE && message.wParam == 47;
            }
            return result;
        });
        ReleaseOpen release_second{second_open}, release_third{third_open};
        second_open.wait();
        if (reset) {
            driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
        } else {
            playback.request_stop();
        }
        second_open.open();
        if (done.wait_for(hang_budget(5s)) != std::future_status::ready) {
            fail(reset ? "The prepared run did not end before its third item after a reset"
                 : quit ? "The prepared run did not end before its third item after Stop with a WM_QUIT queued"
                        : "The prepared run did not end before its third item after Stop");
        }
        expect("the prepared run ends with the request's result", done.get(), reset ? reinitialize_required : stopped);
        expect_true("the reader did not open the third item", !third_open.reached());
        expect("the ended run never allocates buffers", driver.creates, creates);
        expect("the ended run never starts", driver.starts, starts);
        if (quit) {
            expect_true("the WM_QUIT stays queued for the outer loop", quit_left);
        }
        if (!reset) {
            expect("the stopped list stays at its first item", playback.track_index(), 0);
            expect("Stop leaves position zero", playback.position_frames(), 0);
            third_open.open();
            MockSession session(driver);
            expect("Play after the cancelled preparation plays the whole list", playback.play(session), completed);
            check_submitted_pcm(driver.runs.at(0).submitted, driver.width, expected, driver.runs[0].stopped_at);
        }
    }
}

void test_long_block_control_wakes()
{
    // One-second blocks at 48 kHz and no switch after start(). The control wake
    // period is clamped to 500 ms and the callback watchdog stays at four blocks,
    // so the request ends with driver_error after 4 s. Every control wake polls
    // the observer: seven progress wakes and the watchdog's own, plus at most one
    // for a signal event left set before streaming. A fixed 1 s or 980 ms period
    // gives at most six, a progress wake that is never armed at most two, and the
    // wake period used alone as the watchdog ends the request after 500 ms.
    using wavplay::playback::PlaybackResult;
    constexpr std::uint32_t rate = 48000;
    TempFile file;
    wavplay::playback::TrackPlayer playback(file.write(wave({pcm16(2, rate), pcm16_ramp(2, rate * 8)})));
    MockAsio driver;
    driver.buffer_size = {rate, rate, rate, 0};
    MockSession session(driver);
    // The observer runs on this thread, inside play().
    struct Wakes {
        std::chrono::steady_clock::time_point started{};
        bool streaming{};
        unsigned polls{};
    } wakes;
    const wavplay::playback::PlaybackObserver observer{
        [](void* context) noexcept {
            auto& self = *static_cast<Wakes*>(context);
            self.started = std::chrono::steady_clock::now();
            self.streaming = true;
        },
        [](void* context) noexcept {
            auto& self = *static_cast<Wakes*>(context);
            if (self.streaming) { ++self.polls; }
        },
        &wakes};
    const auto result = playback.play(session, nullptr, observer);
    const auto elapsed = std::chrono::steady_clock::now() - wakes.started;
    expect("a driver without switches ends at the callback watchdog", static_cast<int>(result),
        static_cast<int>(PlaybackResult::driver_error));
    expect_true("the long-block stream started", wakes.streaming);
    expect_true("long blocks wake the control thread twice a second (" + std::to_string(wakes.polls) + " polls)",
        wakes.polls >= 7 && wakes.polls <= 9);
    expect_true("the watchdog keeps four whole blocks, not the wake period", elapsed >= 4s);
    expect("one start", driver.starts, 1);
    expect("one stop", driver.stops, 1);
    expect("one disposal", driver.disposals, 1);
}

} // namespace

int wavplay_test::run_track_player_tests()
{
    TestSuite suite;
    suite.run_timed("whole playlist run validates before start and Stop cancels setup", test_playlist_validates_whole_run_before_start);
    suite.run_timed("Stop while a playlist run is prepared opens no further item", test_playlist_stop_while_run_is_prepared);
    suite.run_timed("playlist refusal preserves reset overload and clock supervision", test_playlist_refusal_keeps_idle_supervision);
    suite.run_timed("playlist deletion after loading refuses exact item", test_playlist_late_invalid_file);
    suite.run_timed("playlist refuses an unsupported width as a recoverable item refusal", test_playlist_width_refusal);
    suite.run_timed("101 short tracks and duplicate handles stay gapless", test_playlist_many_short_tracks);
    suite.run_timed("playlist pause/resume retains per-track boundary position", test_playlist_pause_across_boundary);
    suite.run_timed("playlist spans six rates and retries after rate refusal", test_playlist_many_rates_and_refusal);
    suite.run_timed("playlist selects rejected next track and allows another selection", test_playlist_rejected_track_and_retry);
    suite.run_timed("pause/resume refreshes manual and preferred ASIO buffer widths", test_buffer_size_on_resume);
    suite.run_timed("private transport pages span playback and retire with the track", test_resident_transport_lifetime);
    suite.run_timed("pause request owns a validated unread WAV", test_owned_pause_request);
    suite.run_timed("pause/resume actual DAC samples at silence, middle and near EOF", test_boundaries);
    suite.run_timed("pause/resume with PCM already in B and A and no start silence", [] {
        test_pause_boundary(DacQueueAsio::Position::zero, ASIOSTInt16LSB, 2 * DacQueueAsio::block, false);
    });
    suite.run_timed("pause/resume with zero or extra pre-roll and varying latency", test_preroll_and_latency);
    suite.run_timed("short EOF output across prefill, pre-roll and sub-block latency", [] { test_short_endings(); });
    suite.run_timed("pause requested inside start preserves only prefilled PCM", test_pause_during_start);
    suite.run_timed("repeated pause and fresh start silence", test_repeated_pause);
    suite.run_timed("pause after final PCM submission completes normally", test_pause_after_eof);
    suite.run_timed("pause faults do not publish a resumable position", test_pause_faults);
    suite.run_timed("pause during failed read teardown preserves read error", [] { test_read_failure_with_request(false); });
    suite.run_timed("reset, overload or clock loss while paused ends the next Play without resuming", test_fault_while_paused);
    suite.run_timed("user Stop at silence, middle and EOF restarts all DAC samples", [] { test_stop_boundaries(); });
    suite.run_timed("idle Stop rewinds paused and completed playback", test_idle_stop_and_replay);
    suite.run_timed("Stop wins over Pause and repeated requests", test_stop_pause_priority);
    suite.run_timed("Stop accepted during start, pause and EOF teardown", test_stop_handoffs);
    suite.run_timed("Stop during setup skips start and replays from frame zero", test_stop_during_setup);
    suite.run_timed("setup notifications override Stop in either order", test_stop_setup_faults);
    suite.run_timed("Stop releases a waiting producer before replay", test_stop_with_live_producer);
    suite.run_timed("driver faults and teardown failures override Stop", test_stop_faults);
    suite.run_timed("Stop during failed read teardown preserves read error", [] { test_read_failure_with_request(true); });
    suite.run_timed("Stop preserves faults observed before the next Play", test_stop_preserves_idle_faults);
    suite.run_timed("long blocks wake the control thread twice a second inside the four-block watchdog",
        test_long_block_control_wakes);
    return suite.result("Single-track pause/resume/stop");
}
