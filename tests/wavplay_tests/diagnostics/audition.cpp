#include "diagnostics/audition.h"
#include "diagnostics/capability_probe.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/playback/play_tracks.h"
#include "wavplay/playback/track_player.h"
#include "wavplay/playback/segment.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/media/wav_reader.h"

#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace {

void print_buffer_setups(const wavplay::playback::PlaybackDiagnostics& diagnostics)
{
    for (std::size_t i = 0; i < diagnostics.segments.size(); ++i) {
        if (const auto& caps = diagnostics.buffer_capabilities[i]) {
            std::printf("Segment %zu buffer capabilities at %.15g Hz:\n"
                "  Buffer range: %ld..%ld frames; preferred %ld; granularity %ld\n",
                i + 1, caps->sample_rate, caps->minimum, caps->maximum, caps->preferred, caps->granularity);
        }
        if (const auto& setup = diagnostics.segments[i]) {
            std::printf("Segment %zu (verified after createBuffers, before start):\n"
                "  createBuffers width: %ld frames\n  Actual sample rate: %.15g Hz\n",
                i + 1, setup->buffer_frames, setup->sample_rate);
            print_output_types(setup->output_types);
            std::printf("  Driver output latency: %ld frames\n", setup->output_latency_frames);
            std::printf("  Start silence: %ld blocks (%.1f ms)\n", setup->start_silence_blocks,
                1000.0 * setup->start_silence_blocks * setup->buffer_frames / setup->sample_rate);
            std::printf("  Prefetch ring: %s\n", setup->ring_pages_locked ? "locked in the working set" : "pageable");
            std::printf("  Callback metadata: ring %s, renderer %s, requests %s, image %s\n",
                setup->ring_metadata_pages_locked ? "locked" : "pageable",
                setup->renderer_pages_locked ? "locked" : "pageable",
                setup->request_pages_locked ? "locked" : "pageable",
                setup->image_pages_locked ? "locked" : "pageable");
        }
    }
    std::fflush(stdout);
}

wavplay::media::WavReader open_audition_track(const wchar_t* path)
{
    wavplay::media::WavReader reader(path);
    const auto& info = reader.info();
    if ((info.format.sample_rate != 44100 && info.format.sample_rate != 48000) ||
        (info.format.channels != 1 && info.format.channels != 2) || info.frame_count == 0) {
        throw std::runtime_error("Audition requires nonempty 44.1 or 48 kHz mono or stereo PCM16/24/32 WAVs");
    }
    return reader;
}

void check_audition_result(wavplay::playback::PlaybackResult result)
{
    using enum wavplay::playback::PlaybackResult;
    switch (result) {
    case completed: return;
    case underrun: throw std::runtime_error("Audition stopped after an underrun");
    case read_error: throw std::runtime_error("Audition stopped after a WAV read error");
    case reinitialize_required: throw std::runtime_error("ASIO requested reinitialization; audition will not restart automatically");
    case first_rate_rejected: throw std::runtime_error("Could not establish the sample rate at the start of the playback request");
    case next_rate_rejected: throw std::runtime_error("Could not establish the second audition track's sample rate");
    case next_track_rejected: throw std::runtime_error("Second audition track was rejected");
    default: throw std::runtime_error("ASIO driver interrupted the audition");
    }
}

wavplay::playback::PlaybackResult play_reported(wavplay::playback::TrackPlayer& playback, wavplay::asio::DriverSession& session)
{
    wavplay::playback::PlaybackDiagnostics diagnostics{};
    wavplay::playback::PlaybackResult result;
    try {
        result = playback.play(session, &diagnostics);
    } catch (...) {
        print_buffer_setups(diagnostics);
        throw;
    }
    print_buffer_setups(diagnostics);
    return result;
}

using PlaybackRequest = void (wavplay::playback::TrackPlayer::*)() noexcept;

wavplay::playback::PlaybackResult play_until_request(wavplay::playback::TrackPlayer& playback, wavplay::asio::DriverSession& session,
    std::chrono::milliseconds delay, PlaybackRequest request)
{
    // Allocate cancellation resources on the control thread, where failures
    // unwind normally. Nothing in the timer thread may throw and terminate.
    const std::unique_ptr<void, decltype(&CloseHandle)> cancelled(
        CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
    if (!cancelled) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateEventW");
    }
    DWORD timer_error{}; // Published by joining the timer before reading it.
    wavplay::playback::PlaybackResult result;
    // The driver stays on its owning STA. Only the request crosses threads.
    {
        std::jthread timer([&](std::stop_token stop) noexcept {
            const std::stop_callback cancel(stop, [event = cancelled.get()]() noexcept { SetEvent(event); });
            const auto wait = [&](DWORD milliseconds) noexcept {
                const auto wake = WaitForSingleObject(cancelled.get(), milliseconds);
                if (wake == WAIT_FAILED) {
                    timer_error = GetLastError();
                }
                return wake;
            };
            while (!wavplay::playback::is_streaming()) {
                if (wait(1) != WAIT_TIMEOUT) {
                    return;
                }
            }
            if (wait(static_cast<DWORD>(delay.count())) == WAIT_TIMEOUT) {
                (playback.*request)();
            }
        });
        result = play_reported(playback, session);
        // Cancel and join even when EOF, a fault or an exception wins the timer.
    }
    if (timer_error) {
        throw std::system_error(static_cast<int>(timer_error), std::system_category(), "Playback control timer wait");
    }
    return result;
}

void wait_before_restart(wavplay::playback::PlaybackResult expected)
{
    // Keep pumping the driver's STA while stopped. A fault or WM_QUIT cancels
    // the pending restart even though no callback stream is currently active.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        const auto wake = wavplay::playback::notifications().wait_until(until);
        if (wake == wavplay::core::WaitResult::quit || wake == wavplay::core::WaitResult::failed) {
            throw std::runtime_error("Audition interrupted while waiting to restart");
        }
        const auto result = wavplay::playback::notifications().result();
        if (result != expected) {
            check_audition_result(result);
            throw std::runtime_error("Playback state changed while waiting to restart");
        }
        if (wake == wavplay::core::WaitResult::timed_out) {
            return;
        }
    }
}

} // namespace

void audition_asio(const wchar_t* clsid_text, const wchar_t* first_path, const wchar_t* second_path,
    bool transition, wavplay::asio::ActivateDriver activate, wavplay::asio::DeviceSettings settings)
{
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    auto first = open_audition_track(first_path);
    std::optional<wavplay::media::WavReader> second;
    if (second_path) {
        second.emplace(open_audition_track(second_path));
    }
    const auto first_rate = first.info().format.sample_rate;
    const auto final_rate = second ? second->info().format.sample_rate : first_rate;
    if (transition && first_rate == final_rate) {
        throw std::runtime_error("A rate transition requires WAVs at different sample rates");
    }
    if (!transition && first_rate != final_rate) {
        throw std::runtime_error("Both audition WAVs must have the same sample rate");
    }
    const auto window = make_asio_probe_window();
    wavplay::asio::DriverSession session(clsid, window.get(), activate, settings);
    const auto before = wavplay::asio::query_capabilities(session.driver());
    if (second) {
        std::printf("Before playback (first WAV %u Hz, second WAV %u Hz):\n", first_rate, final_rate);
    } else {
        std::printf("Before playback (one WAV %u Hz):\n", first_rate);
    }
    print_capabilities(before);
    std::fflush(stdout);
    if (transition && before.sample_rate != first_rate) {
        throw std::runtime_error("Rate transition audition requires the device to start at the first WAV's rate");
    }
    wavplay::playback::PlaybackDiagnostics diagnostics{};
    wavplay::playback::PlaybackResult result;
    try {
        result = second ? wavplay::playback::play_tracks(session, std::move(first), std::move(*second), &diagnostics)
                        : wavplay::playback::play_tracks(session, std::move(first), {}, &diagnostics);
    } catch (...) {
        print_buffer_setups(diagnostics);
        throw;
    }
    print_buffer_setups(diagnostics);
    check_audition_result(result);
    const auto after = wavplay::asio::query_capabilities(session.driver());
    std::puts("After playback, before device release:");
    print_capabilities(after);
    session.close();
    if (after.sample_rate != final_rate) {
        throw std::runtime_error("Device clock after playback differs from the final audition WAV's rate");
    }
    std::puts("Audition completed and device released.");
}

void audition_asio_pause(const wchar_t* clsid_text, const wchar_t* path, std::chrono::milliseconds pause_after,
    wavplay::asio::ActivateDriver activate)
{
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    auto input = open_audition_track(path);
    if (input.info().format.sample_rate != 48000 || pause_after.count() < 0 || pause_after > std::chrono::minutes(1)) {
        throw std::invalid_argument("Pause audition requires a 48 kHz WAV and a delay between 0 and 60000 ms");
    }
    const auto frame_count = input.info().frame_count;
    wavplay::playback::TrackPlayer playback(std::move(input));
    const auto window = make_asio_probe_window();
    wavplay::asio::DriverSession session(clsid, window.get(), activate);
    std::puts("Before pause audition:");
    print_capabilities(wavplay::asio::query_capabilities(session.driver()));
    std::printf("Requesting pause %lld ms after start returns, then resuming after one second.\n",
        static_cast<long long>(pause_after.count()));
    std::fflush(stdout);
    const auto result = play_until_request(playback, session, pause_after, &wavplay::playback::TrackPlayer::request_pause);
    if (result != wavplay::playback::PlaybackResult::paused) {
        check_audition_result(result);
        throw std::runtime_error("Track completed before the pause request; choose an earlier delay");
    }
    std::printf("Paused after stop/dispose at frame %llu of %llu (%.3f seconds).\n",
        static_cast<unsigned long long>(playback.position_frames()),
        static_cast<unsigned long long>(frame_count), playback.position_frames() / 48000.0);
    std::fflush(stdout);
    wait_before_restart(wavplay::playback::PlaybackResult::paused);
    std::puts("Resuming the retained WAV...");
    std::fflush(stdout);
    check_audition_result(play_reported(playback, session));
    std::printf("Completed at frame %llu.\n", static_cast<unsigned long long>(playback.position_frames()));
    session.close();
    std::puts("Pause audition completed and device released.");
}

void audition_asio_stop(const wchar_t* clsid_text, const wchar_t* path, std::chrono::milliseconds stop_after,
    bool paused_first, wavplay::asio::ActivateDriver activate)
{
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    auto input = open_audition_track(path);
    if (input.info().format.sample_rate != 48000 || stop_after.count() < 0 || stop_after > std::chrono::minutes(1)) {
        throw std::invalid_argument("Stop audition requires a 48 kHz WAV and a delay between 0 and 60000 ms");
    }
    const auto frame_count = input.info().frame_count;
    wavplay::playback::TrackPlayer playback(std::move(input));
    const auto window = make_asio_probe_window();
    wavplay::asio::DriverSession session(clsid, window.get(), activate);
    std::puts("Before Stop audition:");
    print_capabilities(wavplay::asio::query_capabilities(session.driver()));
    std::printf("Requesting %s %lld ms after start returns, then playing from frame 0 after one second.\n",
        paused_first ? "Pause followed by Stop" : "Stop", static_cast<long long>(stop_after.count()));
    std::fflush(stdout);
    const auto expected = paused_first ? wavplay::playback::PlaybackResult::paused : wavplay::playback::PlaybackResult::stopped;
    const auto result = play_until_request(playback, session, stop_after,
        paused_first ? &wavplay::playback::TrackPlayer::request_pause : &wavplay::playback::TrackPlayer::request_stop);
    if (result != expected) {
        check_audition_result(result);
        throw std::runtime_error("Track completed before the Stop audition request; choose an earlier delay");
    }
    if (paused_first) {
        std::printf("Paused after stop/dispose at frame %llu of %llu. Applying Stop.\n",
            static_cast<unsigned long long>(playback.position_frames()), static_cast<unsigned long long>(frame_count));
        playback.stop();
    }
    if (playback.position_frames() != 0) {
        throw std::runtime_error("Stop did not reset playback to frame 0");
    }
    std::puts("Stopped at frame 0; buffers released, selected device and WAV retained.");
    std::fflush(stdout);
    wait_before_restart(expected);
    std::puts("Restarting the retained WAV from frame 0...");
    std::fflush(stdout);
    check_audition_result(play_reported(playback, session));
    std::printf("Completed at frame %llu of %llu.\n", static_cast<unsigned long long>(playback.position_frames()),
        static_cast<unsigned long long>(frame_count));
    session.close();
    std::puts("Stop audition completed and device released.");
}
