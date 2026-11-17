#include "suites/suites.h"
#include "doubles/registered_driver.h"
#include "fixtures/playback_fixture.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/playback/play_tracks.h"
#include "wavplay/playback/playback_signal.h"
#include "wavplay/playback/request.h"

#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace wavplay_test;

void test_first_track_same_rate()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.ticks_after_start = {1};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        wavplay::playback::PlaybackDiagnostics report{};
        const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(path), {}, &report);
        expect("first track at the current clock", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        expect("single-file audition records its allocation", report.segments[0].value().buffer_frames, driver.width);
        expect("single-file audition has no second allocation", static_cast<int>(report.segments[1].has_value()), 0);
        session.close();
    }
    expect("single run at the current clock", driver.starts, 1);
    expect("clock settings including device release", driver.set_rates.size(), 0);
    driver.check_closed();
}

void test_idle_fault_at_request_handoff()
{
    using namespace wavplay::playback;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    for (const auto terminal : {PlaybackResult::stopped, PlaybackResult::paused, PlaybackResult::completed}) {
        for (const auto fault : {PlaybackResult::reinitialize_required, PlaybackResult::underrun}) {
            RegisteredDriver driver;
            {
                wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
                const RequestClaim claim;
                wavplay::asio::DriverLease lease(session);
                const std::atomic<bool> pause{}, stop{};
                SegmentControl control{pause, stop};
                auto& signal = notifications();
                const auto token = signal.begin(48000);
                signal.finish(token, terminal);
                control.previous_idle_token = signal.snapshot();
                // The player has checked its retained position, but the request
                // has not atomically claimed the next generation yet.
                signal.invalidate(token, fault);
                const auto result = run_request(lease, path, {}, control);
                expect("handoff returns the device's reason without a generic exception", result, fault);
                expect("handoff retains the affected generation", control.phase_token, control.previous_idle_token);
                expect("handoff cannot erase the notification", signal.result(), fault);
                expect("handoff never queries or changes the driver", driver.calls.size(), 0);
                expect("handoff never creates buffers", driver.creates, 0);
                expect("handoff never starts playback", driver.starts, 0);
            }
            driver.check_closed();
        }
    }
}

void test_playlist_refusals_require_progress()
{
    using namespace wavplay::playback;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    RegisteredDriver driver;
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
        const RequestClaim claim;
        wavplay::asio::DriverLease lease(session);
        const std::atomic<bool> pause{}, stop{};
        SegmentControl control{pause, stop};
        const auto before = notifications().snapshot();
        expect_throw<std::invalid_argument>("a playlist without progress cannot name a refused item", [&] {
            std::vector<wavplay::stream::TrackInput> inputs;
            inputs.emplace_back(path);
            (void)run_request(lease, std::move(inputs), control, {.refusals = RefusalPolicy::record_each});
        });
        expect("refused options never begin a request generation", notifications().snapshot(), before);
        expect("refused options never query or change the driver", driver.calls.size(), 0);
        expect("refused options never create buffers", driver.creates, 0);
    }
    driver.check_closed();
}

void check_buffer_setup(const wavplay::playback::BufferSetup& setup, long width, double rate,
    ASIOSampleType type, long latency)
{
    expect("width passed to createBuffers", setup.buffer_frames, width);
    expect("clock confirmed after creation", static_cast<int>(setup.sample_rate == rate), 1);
    expect("active left output type", setup.output_types[0], type);
    expect("active right output type", setup.output_types[1], type);
    expect("raw driver output latency", setup.output_latency_frames, latency);
    expect("prefetch ring locked in the working set", static_cast<int>(setup.ring_pages_locked), 1);
    expect_true("prefetch metadata locked in the working set", setup.ring_metadata_pages_locked);
    expect_true("renderer locked in the working set", setup.renderer_pages_locked);
    expect_true("transport requests locked in the working set", setup.request_pages_locked);
    expect_true("a suite process reports its never-locked image as pageable", !setup.image_pages_locked);
}

void check_buffer_range(const wavplay::playback::BufferCapabilities& caps, const MockAsio::BufferSize& range,
    double rate)
{
    expect_true("capabilities belong to the confirmed clock", caps.sample_rate == rate);
    expect("minimum buffer width before allocation", caps.minimum, range.minimum);
    expect("maximum buffer width before allocation", caps.maximum, range.maximum);
    expect("preferred buffer width before allocation", caps.preferred, range.preferred);
    expect("buffer granularity before allocation", caps.granularity, range.granularity);
}

void test_manual_buffer_size()
{
    TempFile file;
    const auto pcm = samples(1);
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", pcm}}));
    for (const long requested : {0, 1, 7, 64, 544}) {
        RegisteredDriver driver;
        driver.buffer_size = {1, 544, 8, 0};
        const auto actual = requested == 0 ? driver.buffer_size.preferred : requested;
        driver.latency = actual;
        driver.ticks_after_start = {1};
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .buffer_frames = requested});
            expect("manual-width playback completes", wavplay::playback::play_tracks(session, path, {}, &report),
                wavplay::playback::PlaybackResult::completed);
            check_buffer_setup(report.segments[0].value(), actual, 48000, ASIOSTInt16LSB, actual);
            check_buffer_range(report.buffer_capabilities[0].value(), driver.buffer_size, 48000);
            expect("manual width is queried once", driver.size_queries, 1);
            expect("manual width never changes the reported preferred width", driver.buffer_size.preferred, 8);
            expect("mock device advances at the actual buffer width", driver.stopped_at, actual);
            expect("run reports actual width", driver.runs.at(0).width, actual);
            check_submitted_pcm(driver.submitted, actual, pcm, actual);
        }
        driver.check_closed();
    }
}

void test_manual_buffer_rejections()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    for (const long requested : {3, 5, 17}) {
        RegisteredDriver driver;
        driver.buffer_size = {4, 16, 8, 4};
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .buffer_frames = requested});
            expect_throw<std::invalid_argument>("unsupported manual width rejects playback", [&] {
                (void)wavplay::playback::play_tracks(session, path, {}, &report);
            });
            expect("unsupported width is queried once", driver.size_queries, 1);
            expect("unsupported width never allocates buffers", driver.creates, 0);
            expect("unsupported width never starts", driver.starts, 0);
            expect_true("unsupported width publishes no allocation", !report.segments[0]);
            check_buffer_range(report.buffer_capabilities[0].value(), driver.buffer_size, 48000);
        }
        driver.check_closed();
    }
    class RefusingDriver : public RegisteredDriver {
    public:
        unsigned buffer_attempts{};
        long requested_frames{};
        ASIOError createBuffers(ASIOBufferInfo*, long, long frames, ASIOCallbacks*) override
        {
            ++buffer_attempts;
            requested_frames = frames;
            return ASE_InvalidParameter;
        }
    } driver;
    driver.buffer_size = {4, 16, 8, 4};
    wavplay::playback::PlaybackDiagnostics report{};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
            {.start_silence = no_start_silence, .buffer_frames = 12});
        expect_throw<std::runtime_error>("driver can refuse a supported manual width", [&] {
            (void)wavplay::playback::play_tracks(session, path, {}, &report);
        });
        expect("driver refusal never retries preferred", driver.buffer_attempts, 1);
        expect("driver receives exact requested width", driver.requested_frames, 12);
        expect("driver refusal never starts", driver.starts, 0);
        expect("untouched failed allocation needs no disposal", driver.disposals, 0);
        expect_true("driver refusal publishes no allocation", !report.segments[0]);
        check_buffer_range(report.buffer_capabilities[0].value(), driver.buffer_size, 48000);
    }
    driver.check_closed();
}

void test_manual_buffer_rate_transition()
{
    TempFile first, second;
    const auto* at48 = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto* at44 = second.write(wave({pcm16(2, 44100), {"data", samples(1, 0x61)}}));
    for (const bool refuse_second : {false, true}) {
        RegisteredDriver driver;
        driver.latency = driver.next_latency = 8;
        driver.ticks_after_start = {1, 1};
        const MockAsio::BufferSize first_range{2, 32, 4, 2};
        const MockAsio::BufferSize second_range = refuse_second ? MockAsio::BufferSize{2, 4, 4, 2}
                                                             : MockAsio::BufferSize{2, 32, 16, 2};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::size) {
                driver.buffer_size = driver.sample_rate == 48000 ? first_range : second_range;
            }
        };
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .buffer_frames = 8});
            const auto play = [&] { return wavplay::playback::play_tracks(session, at48, at44, &report); };
            if (refuse_second) {
                expect_throw<std::invalid_argument>("new clock's range can reject the manual width", play);
            } else {
                expect("manual width survives driver reopen and new clock", play(), wavplay::playback::PlaybackResult::completed);
                check_buffer_setup(report.segments[1].value(), 8, 44100, ASIOSTInt16LSB, 8);
                check_submitted_pcm(driver.runs.at(1).submitted, 8, samples(1, 0x61), 8);
            }
            check_buffer_setup(report.segments[0].value(), 8, 48000, ASIOSTInt16LSB, 8);
            check_buffer_range(report.buffer_capabilities[0].value(), first_range, 48000);
            check_buffer_range(report.buffer_capabilities[1].value(), second_range, 44100);
            expect("range re-queried after reopen and rate setting", driver.size_queries, 2);
            expect("manual width retains required driver reopen", driver.inits, 2);
            expect("start only at supported manual widths", driver.starts, refuse_second ? 1 : 2);
            expect("allocation only at supported manual widths", driver.creates, refuse_second ? 1 : 2);
            expect_true("only a supported second width publishes an allocation", report.segments[1].has_value() != refuse_second);
            session.close();
        }
        expect_true("manual-width failure or completion restores the clock", driver.sample_rate == 48000);
        driver.check_closed();
    }
}

void test_configured_output_pairs()
{
    using wavplay::playback::PlaybackResult;
    TempFile first, second;
    const auto first_pcm = samples(5);
    const auto second_pcm = samples(7, 0x61);
    const auto* at48 = first.write(wave({pcm16(2, 48000), {"data", first_pcm}}));
    const auto* at44 = second.write(wave({pcm16(2, 44100), {"data", second_pcm}}));
    {
        RegisteredDriver driver;
        driver.outputs = 4;
        driver.first_output = 2;
        driver.next_latency = 6;
        driver.ticks_after_start = {3, 4};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .output_pair = 3});
            expect("configured physical outputs survive a different-rate reopen",
                wavplay::playback::play_tracks(session, at48, at44), PlaybackResult::completed);
            expect("both rates allocate the configured pair", driver.created_outputs.size(), 2);
            for (const auto& pair : driver.created_outputs) {
                expect("physical left is channel 3", pair[0], 2);
                expect("physical right is channel 4", pair[1], 3);
            }
            for (const auto channel : driver.queried_outputs) {
                expect_true("format queries use only the configured physical pair", channel == 2 || channel == 3);
            }
            check_submitted_pcm(driver.runs.at(0).submitted, 2, first_pcm, 6);
            check_submitted_pcm(driver.runs.at(1).submitted, 3, second_pcm, 12);
            session.close();
        }
        driver.check_closed();
    }
    for (const auto fault : {MockAsio::BufferFault::none, MockAsio::BufferFault::swapped_channels,
        MockAsio::BufferFault::inactive_output, MockAsio::BufferFault::changed_type}) {
        RegisteredDriver driver;
        driver.outputs = fault == MockAsio::BufferFault::none ? 2 : 4;
        driver.first_output = 2;
        driver.buffer_fault = fault;
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .output_pair = 3});
            expect_throw<std::runtime_error>("missing or changed physical output pair is refused", [&] {
                (void)wavplay::playback::play_tracks(session, at48);
            });
            expect("invalid output never starts or falls back", driver.starts, 0);
            expect("created buffers are released on validation failure", driver.disposals, driver.creates);
        }
        driver.check_closed();
    }
    // A refused single track names the driver's types for the configured pair. A
    // pair beyond the output count keeps the plain text and is never queried.
    const std::string refusal = "Configured ASIO output pair is unavailable or has no common lossless format for this WAV";
    for (const long outputs : {2L, 4L}) {
        RegisteredDriver driver;
        driver.outputs = outputs;
        driver.first_output = 2;
        driver.right_type = ASIOSTInt24LSB; // Differs from the 16-bit left output.
        std::string reason;
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .output_pair = 3});
            try {
                (void)wavplay::playback::play_tracks(session, at48);
            } catch (const std::runtime_error& error) {
                reason = error.what();
            }
        }
        if (reason != (outputs == 4 ? refusal + ": ASIOSTInt16LSB/ASIOSTInt24LSB" : refusal)) {
            throw std::runtime_error("Unexpected output pair refusal: " + reason);
        }
        for (const auto channel : driver.queried_outputs) {
            expect_true("refusal queries only the configured pair", channel == 2 || channel == 3);
        }
        expect_true("missing pair never queried by channel", driver.queried_outputs.empty() == (outputs == 2));
        expect("refused pair never allocates", driver.creates, 0);
        driver.check_closed();
    }
    // The types only annotate the refusal. A failed type query, or a driver fault
    // during these extra queries, keeps the plain refusal text.
    for (const bool faulting : {false, true}) {
        RegisteredDriver driver;
        driver.outputs = 4;
        driver.first_output = 2;
        if (faulting) {
            driver.right_type = ASIOSTInt24LSB;
            driver.on_call = [&](MockAsio::Call call) {
                if (call == MockAsio::Call::channels && driver.queried_outputs.size() == 2) {
                    throw std::runtime_error("ASIO getChannels failed: ASE_HWMalfunction (-999)");
                }
            };
        } else {
            driver.left_type = ASIOSTDSDInt8LSB1; // Refused after the left output alone.
            driver.right_info_result = ASE_HWMalfunction;
        }
        std::string reason;
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(),
                {.start_silence = no_start_silence, .output_pair = 3});
            try {
                (void)wavplay::playback::play_tracks(session, at48);
            } catch (const std::runtime_error& error) {
                reason = error.what();
            }
        }
        if (reason != refusal) {
            throw std::runtime_error("Type annotation replaced the output pair refusal: " + reason);
        }
        expect_true("refusal queried the right output", faulting ||
            std::count(driver.queried_outputs.begin(), driver.queried_outputs.end(), 3) == 1);
        expect("refused pair never allocates", driver.creates, 0);
        driver.check_closed();
    }
    RegisteredDriver driver;
    for (const long pair : {-1L, 0L, 2L, (std::numeric_limits<long>::max)()}) {
        expect_throw<std::invalid_argument>("invalid pair is refused before driver activation", [&] {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.output_pair = pair});
        });
    }
    expect("bad pair never opens a driver", driver.attempts, 0);
}

void test_buffer_setup_diagnostics()
{
    using enum MockAsio::Call;
    TempFile first, second;
    const auto* at48 = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto* at44 = second.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    wavplay::playback::PlaybackDiagnostics report{};
    // Reuse the report for a same-rate pair after recording a two-segment transition.
    for (const bool transition : {true, false}) {
        RegisteredDriver driver;
        driver.buffer_size = {544, 544, 544, 0};
        driver.latency = 101;
        driver.next_width = 512;
        driver.next_left_type = driver.next_right_type = ASIOSTInt24LSB;
        driver.next_latency = 17; // Less than one block: drain policy must not change this reading.
        driver.ticks_after_start = {1, 1};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == create) {
                const auto& caps = report.buffer_capabilities.at(driver.starts).value();
                check_buffer_range(caps, driver.buffer_size, driver.sample_rate);
                expect_true("capabilities precede allocation", !report.segments.at(driver.starts));
            }
            if (call == latency || call == start) {
                expect("setup published after getLatencies and before start",
                    static_cast<int>(report.segments.at(driver.starts).has_value()), static_cast<int>(call == start));
            } else if (call == dispose && driver.sample_rate == 44100) {
                // The stopped device can report different capabilities once its buffers go away.
                driver.buffer_size = {544, 544, 544, 0};
                driver.left_type = driver.right_type = ASIOSTInt32LSB;
            }
        };
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(transition ? at48 : at44),
                wavplay::media::WavReader(at44), &report);
            expect("diagnosed playback completed", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
            for (const auto inquiry : {size, latency}) {
                expect("one setup inquiry per segment", std::ranges::count(driver.calls, inquiry), driver.creates);
            }
            const auto idle = wavplay::asio::query_capabilities(session.driver());
            expect("idle preferred width at 44.1 kHz", idle.buffer_size.preferred, 544);
            expect("idle output type", idle.output_types[0], static_cast<long>(ASIOSTInt32LSB));
            session.close();
        }
        driver.check_closed();
        expect("one record per created segment", static_cast<int>(report.segments[1].has_value()), static_cast<int>(transition));
        expect_true("new request discards capabilities from an earlier second segment",
            report.buffer_capabilities[1].has_value() == transition);
        if (transition) {
            check_buffer_setup(report.segments[0].value(), 544, 48000, ASIOSTInt16LSB, 101);
            check_buffer_range(report.buffer_capabilities[0].value(), {544, 544, 544, 0}, 48000);
        }
        check_buffer_setup(report.segments[transition ? 1 : 0].value(), 512, 44100, ASIOSTInt24LSB, 17);
        check_buffer_range(report.buffer_capabilities[transition ? 1 : 0].value(), {512, 512, 512, 0}, 44100);
    }
}

void test_buffer_setup_diagnostics_on_failure()
{
    TempFile first, second;
    const auto* at48 = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto* at44 = second.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    for (const bool fail_start : {false, true}) {
        RegisteredDriver driver;
        driver.ticks_after_start = {1, 1};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::create && driver.creates == 1) {
                driver.fail_start = fail_start;
                driver.buffer_fault = fail_start ? MockAsio::BufferFault::none : MockAsio::BufferFault::inactive_output;
            }
        };
        wavplay::playback::PlaybackDiagnostics report{};
        expect_throw<std::runtime_error>("fault preparing or starting the second segment", [&] {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
            (void)wavplay::playback::play_tracks(session, at48, at44, &report);
        });
        expect("allocated segments released", driver.disposals, 2);
        expect("first validated setup survives a later failure", static_cast<int>(report.segments[0].has_value()), 1);
        expect("second record only after complete validation", static_cast<int>(report.segments[1].has_value()),
            static_cast<int>(fail_start));
        expect_true("first capability record survives later failure", report.buffer_capabilities[0]->sample_rate == 48000);
        expect_true("second capability record survives allocation validation or start failure",
            report.buffer_capabilities[1]->sample_rate == 44100);
        driver.check_closed();
    }
}

void test_first_track_rate_and_release()
{
    using enum MockAsio::Call;
    TempFile file;
    const auto pcm = samples(2);
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", pcm}}));
    RegisteredDriver driver;
    driver.next_left_type = driver.next_right_type = ASIOSTInt24LSB;
    driver.next_latency = driver.next_width;
    driver.ticks_after_start = {1, 1};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        const auto result = wavplay::playback::play_tracks(session, path);
        expect("first track after changing the clock", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        expect("clock held after playing", driver.set_rates.size(), 1);
        expect("requested WAV clock", static_cast<int>(driver.set_rates.front() == 44100), 1);
        const auto& run = driver.runs.at(0);
        expect("actual playback clock", static_cast<int>(run.rate == 44100), 1);
        expect("new preferred width", run.width, driver.next_width);
        check_submitted_pcm(run.submitted, run.width, pcm, driver.next_width, 3);
        expect_call_order(driver, {rate, can_rate, set_rate, rate, create, start, stop, dispose});
        expect_call_order(driver, {set_rate, size, create});
        expect_call_order(driver, {set_rate, channel, channel, create});
        // Another user playback request must not replace the original clock.
        const auto repeated = wavplay::playback::play_tracks(session, path);
        expect("repeat at the changed clock", static_cast<int>(repeated), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        expect("clock held across playback requests", driver.set_rates.size(), 1);
        session.close();
        expect("device released by close", driver.releases, 1);
    }
    expect("set and restore calls", driver.set_rates.size(), 2);
    expect("original 48 kHz restored", static_cast<int>(driver.sample_rate == 48000), 1);
    expect_call_order(driver, {dispose, dispose, set_rate, rate});
    driver.check_closed();
}

void test_first_track_rate_rejections()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    const auto check_rejection = [&](RegisteredDriver& driver, unsigned attempts) {
        driver.ticks_after_start = {1}; // Keep an unexpected start from blocking the test.
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        const auto result = wavplay::playback::play_tracks(session, path);
        expect("reason for refusing the first track", static_cast<int>(result),
            static_cast<int>(wavplay::playback::PlaybackResult::first_rate_rejected));
        expect("settings before refusing the first track", driver.set_rates.size(), attempts);
        expect("buffer creations after clock rejection", driver.creates, 0);
        expect("starts after clock rejection", driver.starts, 0);
        session.close();
    };
    {
        RegisteredDriver unsupported;
        unsupported.can_rate_result = ASE_NoClock;
        check_rejection(unsupported, 0);
        expect("restores after unsupported clock query", unsupported.set_rates.size(), 0);
        unsupported.check_closed();
    }
    {
        RegisteredDriver refused;
        refused.set_rate_result = ASE_InvalidMode;
        check_rejection(refused, 1);
        expect("settings including release after refusal", refused.set_rates.size(), 1);
        refused.check_closed();
    }
    {
        RegisteredDriver stale;
        stale.keep_old_rate = true; // The set call succeeds but getSampleRate still reports 48 kHz.
        check_rejection(stale, 1);
        stale.check_closed();
    }
    {
        RegisteredDriver no_clock;
        no_clock.sample_rate_result = ASE_NoClock;
        check_rejection(no_clock, 0);
        expect("settings without a known original clock", no_clock.set_rates.size(), 0);
        no_clock.check_closed();
    }
}

void test_clock_negotiation_preserves_mxcsr()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    // Inspect precisely the path from getSampleRate to canSampleRate, before
    // any driver arithmetic, waits or unrelated Windows calls can affect flags.
    // Exercise both ends of the subnormal interval and the smallest normal.
    for (const auto bits : {UINT64_C(1), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000)}) {
        RegisteredDriver driver;
        driver.sample_rate = std::bit_cast<double>(bits);
        driver.can_rate_result = ASE_NoClock;
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
        const auto saved = _mm_getcsr();
        struct RestoreMxcsr {
            unsigned value;
            ~RestoreMxcsr() { _mm_setcsr(value); }
        } restore{saved};
        // Mask all exceptions, clear their flags, and disable DAZ/FTZ so the
        // test observes accidental arithmetic on subnormals on every host.
        const auto baseline = (saved | _MM_MASK_MASK) &
            ~static_cast<unsigned>(_MM_EXCEPT_MASK | _MM_FLUSH_ZERO_MASK | 0x0040);
        unsigned observed{};
        bool queried{};
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::rate) {
                _mm_setcsr(baseline);
            } else if (call == MockAsio::Call::can_rate) {
                observed = _mm_getcsr();
                queried = true;
                _mm_setcsr(saved);
            }
        };
        const auto result = wavplay::playback::play_tracks(session, path);
        _mm_setcsr(saved);
        expect_true("finite clock reaches capability query", queried);
        expect("clock negotiation preserves all MXCSR bits", observed, baseline);
        expect("unsupported clock stays a rate rejection", result, wavplay::playback::PlaybackResult::first_rate_rejected);
        expect("clock rejection never creates buffers", driver.creates, 0);
        expect("clock rejection never sets the clock", driver.set_rates.size(), 0);
        session.close();
    }
}

void test_rate_restore_after_setup_failure()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.buffer_fault = MockAsio::BufferFault::inactive_output;
    driver.ticks_after_start = {1};
    expect_throw<std::runtime_error>("invalid active output after clock setting", [&] {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        (void)wavplay::playback::play_tracks(session, path);
    }); // Unwinding the device owner must restore even if playback never started.
    expect("starts after failed validation", driver.starts, 0);
    expect("released invalid buffers", driver.disposals, 1);
    expect("clock after failed buffer setup", static_cast<int>(driver.sample_rate == 48000), 1);
    using enum MockAsio::Call;
    expect_call_order(driver, {set_rate, create, dispose, set_rate});
    driver.check_closed();
}

void test_failed_rate_change(bool partial_change)
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.set_rate_result = ASE_HWMalfunction;
    driver.on_call = [&](MockAsio::Call call) {
        if (partial_change && call == MockAsio::Call::set_rate && driver.set_rates.empty()) {
            driver.sample_rate = 44100; // PLL already switched before the hardware fault.
        }
        if (partial_change && call == MockAsio::Call::rate && driver.set_rates.size() == 1) {
            throw std::runtime_error("Clock query also failed");
        }
    };
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        std::string reason;
        try {
            (void)wavplay::playback::play_tracks(session, path);
        } catch (const std::runtime_error& error) {
            reason = error.what();
        }
        if (reason != "ASIO setSampleRate failed: ASE_HWMalfunction (-999)") {
            throw std::runtime_error("Lost original clock configuration error: " + reason);
        }
        expect("no buffers after failed clock configuration", driver.creates, 0);
        expect("no start after failed clock configuration", driver.starts, 0);
        driver.set_rate_result = ASE_OK;
        if (!partial_change) {
            driver.sample_rate = 88200; // External change after the driver confirmed it stayed at 48 kHz.
        }
        session.close();
    }
    expect("set and restore after a hardware failure", driver.set_rates.size(), partial_change ? 2 : 1);
    expect("release after a hardware failure", static_cast<int>(driver.sample_rate == (partial_change ? 48000 : 88200)), 1);
    driver.check_closed();
}

void test_rate_restore_after_second_track()
{
    TempFile first, second;
    const auto* first_path = first.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    const auto* second_path = second.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.ticks_after_start = {1, 1, 1};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        const auto result = wavplay::playback::play_tracks(session, first_path, second_path);
        expect("device session completed the pair", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        expect("clock changed only for second track", driver.set_rates.size(), 1);
        expect("retained second track clock", static_cast<int>(driver.sample_rate == 44100), 1);
        // A new play returns to the original clock. Closing must leave it there.
        const auto again = wavplay::playback::play_tracks(session, first_path);
        expect("play again at 48 kHz", static_cast<int>(again), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        session.close();
    }
    expect("first track's original device clock restored", static_cast<int>(driver.sample_rate == 48000), 1);
    expect("clock settings after release", driver.set_rates.size(), 2);
    driver.check_closed();
}

void test_rate_restore_failure(bool reject_setting)
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.ticks_after_start = {1};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        const auto result = wavplay::playback::play_tracks(session, path);
        expect("play before refused restoration", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        driver.set_rate_result = reject_setting ? ASE_InvalidMode : ASE_OK;
        driver.keep_old_rate = !reject_setting;
        std::string reason;
        try {
            session.close();
        } catch (const std::runtime_error& error) {
            reason = error.what();
        }
        if (reason.find("restore") == std::string::npos) {
            throw std::runtime_error("Missing rate restoration diagnostic: " + reason);
        }
        expect("device released on restoration failure", driver.references, 0);
        session.close();
    }
    expect("settings including refused restoration", driver.set_rates.size(), 2);
    expect("restore requested the original rate", static_cast<int>(driver.set_rates.back() == 48000), 1);
    driver.check_closed();
    expect("device reopened after restore failure", static_cast<int>(probe(driver).sample_rate == 44100), 1);
    driver.check_closed();
}

// The session reinitializes the driver between different-rate segments of one request.
void test_rate_transition_reopens_session()
{
    using enum MockAsio::Call;
    using enum wavplay::playback::PlaybackResult;
    TempFile at48, at44;
    const auto* first = at48.write(wave({pcm16(2, 48000), {"data", samples(1, 10)}}));
    const auto* second = at44.write(wave({pcm16(2, 44100), {"data", samples(1, 90)}}));
    struct ReopenDriver : LifecycleDriver {
        struct Open { std::size_t calls; unsigned releases, disposals; void* window; };
        std::vector<Open> opens;
        unsigned overload_inquiries{};
        ASIOBool init(void* window) override
        {
            opens.push_back({calls.size(), releases, disposals, window});
            return LifecycleDriver::init(window);
        }
        ASIOError future(long selector, void* params) override
        {
            overload_inquiries += selector == kAsioCanReportOverload;
            return MockAsio::future(selector, params);
        }
    };
    {
        ReopenDriver driver;
        driver.ticks_per_start = 8;
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(first), wavplay::media::WavReader(second), &report);
            expect("reopened transition completed", static_cast<int>(result), static_cast<int>(completed));
            expect("only the second WAV changed the clock", driver.set_rates.size(), 1);
            expect_calls("second init", driver.lifecycle_calls(1), {can_rate, set_rate, create, start, stop, dispose});
            session.close();
        }
        expect("driver initialized per segment", driver.opens.size(), 2);
        expect("first driver released before the second init", driver.opens[1].releases, 1);
        expect("first buffers disposed before release", driver.opens[1].disposals, 1);
        expect("same owner window after reopening",
            static_cast<int>(driver.opens[0].window && driver.opens[0].window == driver.opens[1].window), 1);
        expect("overload inquiry for each initialized driver", driver.overload_inquiries, 2);
        // A width left over from 48 kHz would be 544; the fresh init reports the 44.1 kHz width.
        check_buffer_setup(report.segments[0].value(), 544, 48000, ASIOSTInt24LSB, 544);
        check_buffer_setup(report.segments[1].value(), 512, 44100, ASIOSTInt24LSB, 512);
        expect("callbacks wrote only the requested half", static_cast<int>(driver.peer_untouched), 1);
        expect("first WAV's clock restored on release", static_cast<int>(driver.sample_rate == 48000), 1);
        driver.check_closed();
    }
    {
        // The first WAV changed the clock: the boundary restores 48 kHz before releasing.
        ReopenDriver driver;
        driver.ticks_per_start = 8;
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(second), wavplay::media::WavReader(first));
            expect("transition back to the original clock", static_cast<int>(result), static_cast<int>(completed));
            session.close();
        }
        expect("set, then restore before reopening", driver.set_rates.size(), 2);
        expect("restored the original clock", static_cast<int>(driver.set_rates.back() == 48000), 1);
        const auto first_init = driver.calls.begin() + static_cast<std::ptrdiff_t>(driver.opens.at(1).calls);
        expect("both settings made by the first instance", std::count(driver.calls.begin(), first_init, set_rate), 2);
        expect_calls("reopened at the WAV clock", driver.lifecycle_calls(1), {create, start, stop, dispose});
        driver.check_closed();
    }
    {
        ReopenDriver driver;
        driver.ticks_per_start = 8;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == dispose) {
                driver.fail_init = true;
            }
        };
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
            expect_throw<std::runtime_error>("second init failure", [&] {
                (void)wavplay::playback::play_tracks(session, wavplay::media::WavReader(second), wavplay::media::WavReader(first));
            });
            expect("failed instance released", driver.references, 0);
            expect_throw<std::logic_error>("no driver after a failed reopen", [&] { (void)session.driver(); });
            session.close();
        }
        expect("original clock restored before the failed init", static_cast<int>(driver.sample_rate == 48000), 1);
        expect("no second start after a failed init", driver.starts, 1);
        driver.check_closed();
    }
}

enum class RestoreNotice { restored_clock, reset, delayed_reset, no_clock };

// The old instance can report its restored clock before it releases its callback table.
void test_clock_restoration_boundary(RestoreNotice notice)
{
    using enum wavplay::playback::PlaybackResult;
    TempFile at44, at48;
    const auto* first = at44.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    const auto* second = at48.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    LifecycleDriver driver;
    driver.ticks_per_start = 8;
    bool notified{};
    auto& signal = wavplay::playback::notifications();
    wavplay::playback::PlaybackSignal::Token old_phase{};
    driver.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::set_rate && driver.starts == 1 && driver.releases == 0) {
            notified = true;
            old_phase = signal.snapshot();
            driver.rate_notification(notice == RestoreNotice::no_clock ? 0 : 48000);
            if (notice == RestoreNotice::reset) {
                driver.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
            }
        } else if (notice == RestoreNotice::delayed_reset && call == MockAsio::Call::size && driver.releases == 1) {
            // The reset entered during restoration but commits after the fresh clock is known.
            signal.invalidate(old_phase, reinitialize_required);
        }
    };
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        const auto result = wavplay::playback::play_tracks(session, first, second);
        expect("rate restoration callback delivered", static_cast<int>(notified), 1);
        const bool uninterrupted = notice == RestoreNotice::restored_clock;
        expect("restored clock vs hardware fault", static_cast<int>(result),
            static_cast<int>(uninterrupted ? completed : notice == RestoreNotice::no_clock ? driver_error : reinitialize_required));
        expect("second start only with a valid clock and no reset", driver.starts, uninterrupted ? 2 : 1);
        // This instance did not change the device clock. Its owner's earlier
        // obligation was settled before reopening.
        driver.sample_rate = 88200;
        session.close();
    }
    expect("new external setting preserved", static_cast<int>(driver.sample_rate == 88200), 1);
    driver.check_closed();
}

void test_start_silence()
{
    using namespace std::chrono_literals;
    // 100 ms rounds up to 9 blocks of 544 frames at 48 kHz and of 512 frames at 44.1 kHz.
    constexpr std::size_t blocks48 = 9, blocks44 = 9;
    TempFile at48, next48, at44;
    const auto first_pcm = samples(1200, 0x11);
    const auto next_pcm = samples(700, 0x61);
    const auto* first = at48.write(wave({pcm16(2, 48000), {"data", first_pcm}}));
    const auto* next = next48.write(wave({pcm16(2, 48000), {"data", next_pcm}}));
    const auto* other_rate = at44.write(wave({pcm16(2, 44100), {"data", next_pcm}}));
    {
        LifecycleDriver driver;
        driver.ticks_per_start = 24;
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(first), {}, &report);
            expect("single track after start silence", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
            session.close();
        }
        expect("reported start silence", report.segments[0].value().start_silence_blocks, blocks48);
        check_silence_then_pcm(driver.submitted, blocks48, first_pcm, 544);
        driver.check_closed();
    }
    {
        // A same-rate pair is one stream: silence only before the first track, none at the join.
        LifecycleDriver driver;
        driver.ticks_per_start = 24;
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(first), wavplay::media::WavReader(next));
            expect("gapless pair after start silence", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
            session.close();
        }
        expect("one stream for a same-rate pair", driver.starts, 1);
        auto joined = first_pcm;
        joined.append_range(next_pcm);
        check_silence_then_pcm(driver.submitted, blocks48, joined, 544);
        driver.check_closed();
    }
    {
        // A different-rate pair restarts the stream, so both segments begin with silence.
        LifecycleDriver driver;
        driver.ticks_per_start = 24;
        std::vector<MockAsio::Block> first_run;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == MockAsio::Call::start && driver.starts == 1) {
                first_run = driver.submitted; // start() clears the record of the previous run.
            }
        };
        wavplay::playback::PlaybackDiagnostics report{};
        {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
            const auto result = wavplay::playback::play_tracks(session, wavplay::media::WavReader(first),
                wavplay::media::WavReader(other_rate), &report);
            expect("transition after start silence", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
            session.close();
        }
        expect("first segment start silence", report.segments[0].value().start_silence_blocks, blocks48);
        expect("second segment start silence", report.segments[1].value().start_silence_blocks, blocks44);
        check_silence_then_pcm(first_run, blocks48, first_pcm, 544);
        check_silence_then_pcm(driver.submitted, blocks44, next_pcm, 512);
        driver.check_closed();
    }
    {
        LifecycleDriver driver;
        for (const auto invalid : {-1ms, wavplay::asio::max_start_silence + 1ms}) {
            expect_throw<std::invalid_argument>("start silence out of range", [&] {
                wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = invalid});
            });
        }
        expect("no driver opened for an invalid start silence", driver.attempts, 0);
    }
}

void test_reentrant_device_close()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    RegisteredDriver driver;
    driver.ticks_after_start = {1};
    unsigned attempts{};
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.start_silence = no_start_silence});
        driver.on_call = [&](MockAsio::Call call) {
            // A driver call can dispatch a message on this same owner thread.
            if ((call == MockAsio::Call::set_rate && driver.set_rates.empty()) || call == MockAsio::Call::dispose) {
                ++attempts;
                expect_throw<std::logic_error>("close while playback leases the driver", [&] { session.close(); });
                expect_throw<std::logic_error>("reopen while playback leases the driver", [&] { (void)session.reopen(); });
                expect("driver reference survived reentrancy", driver.references, 1);
                expect("reentrant reopen kept the initialized driver", driver.inits, 1);
            }
        };
        const auto result = wavplay::playback::play_tracks(session, path);
        expect("play survives reentrant close", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
        session.close();
    }
    expect("reentrant close during setting and disposal", attempts, 2);
    expect("restored after playback releases driver", static_cast<int>(driver.sample_rate == 48000), 1);
    driver.check_closed();
}

} // namespace

int wavplay_test::run_session_playback_tests()
{
    TestSuite suite;
    suite.run_timed("reentrant device close during playback", test_reentrant_device_close);
    suite.run_timed("first WAV at the selected device clock", test_first_track_same_rate);
    suite.run_timed("idle faults racing request handoff return their reason without driver calls", test_idle_fault_at_request_handoff);
    suite.run_timed("playlist refusals require progress before any driver call", test_playlist_refusals_require_progress);
    suite.run_timed("manual ASIO buffer widths preserve PCM and verified range", test_manual_buffer_size);
    suite.run_timed("invalid or refused manual widths never fall back or start", test_manual_buffer_rejections);
    suite.run_timed("manual ASIO buffer range refreshes after rate transition", test_manual_buffer_rate_transition);
    suite.run_timed("configured output pairs survive rate changes and reject unavailable or changed outputs", test_configured_output_pairs);
    suite.run_timed("ASIO buffer setup at first WAV and rate transition", test_buffer_setup_diagnostics);
    suite.run_timed("ASIO buffer setup records on failure", test_buffer_setup_diagnostics_on_failure);
    suite.run_timed("first WAV sets the clock until device release", test_first_track_rate_and_release);
    suite.run_timed("first WAV clock negotiation rejections", test_first_track_rate_rejections);
    suite.run_timed("device clock negotiation preserves MXCSR", test_clock_negotiation_preserves_mxcsr);
    suite.run_timed("unwind restores clock after buffer setup fails", test_rate_restore_after_setup_failure);
    suite.run_timed("restore after a partially failed hardware clock change", [] { test_failed_rate_change(true); });
    suite.run_timed("preserve an external clock after an unchanged refusal", [] { test_failed_rate_change(false); });
    suite.run_timed("original clock restored after second track", test_rate_restore_after_second_track);
    suite.run_timed("rate transition reinitializes the session driver", test_rate_transition_reopens_session);
    suite.run_timed("old instance reports the restored clock", [] { test_clock_restoration_boundary(RestoreNotice::restored_clock); });
    suite.run_timed("reset during clock restoration", [] { test_clock_restoration_boundary(RestoreNotice::reset); });
    suite.run_timed("delayed reset from clock restoration", [] { test_clock_restoration_boundary(RestoreNotice::delayed_reset); });
    suite.run_timed("clock loss during clock restoration", [] { test_clock_restoration_boundary(RestoreNotice::no_clock); });
    suite.run_timed("session start silence before the first PCM block", test_start_silence);
    suite.run_timed("release and reopen after a refused clock restore", [] { test_rate_restore_failure(true); });
    suite.run_timed("release and reopen after an unconfirmed clock restore", [] { test_rate_restore_failure(false); });
    return suite.result("ASIO session playback");
}
