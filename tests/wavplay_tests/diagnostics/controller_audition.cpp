#include "diagnostics/audition.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/control/playback_controller.h"
#include "wavplay/media/wav_reader.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using wavplay::control::PlaybackCommand;
using wavplay::control::PlaybackController;
using wavplay::control::PlaybackSnapshot;
using wavplay::control::PlaybackState;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

const char* state_name(PlaybackState state) noexcept
{
    using enum PlaybackState;
    switch (state) {
    case initializing: return "initializing";
    case reinitializing: return "reinitializing";
    case stopped: return "stopped";
    case starting: return "starting";
    case playing: return "playing";
    case pausing: return "pausing";
    case stopping: return "stopping";
    case paused: return "paused";
    case completed: return "completed";
    case failed: return "failed";
    case closing: return "closing";
    case closed: return "closed";
    }
    return "unknown";
}

const char* command_name(PlaybackCommand command) noexcept
{
    using enum PlaybackCommand;
    switch (command) {
    case play: return "Play";
    case pause: return "Pause";
    case stop: return "Stop";
    case select_play: return "SelectPlay";
    case close: return "Close";
    }
    return "unknown";
}

const char* result_name(wavplay::playback::PlaybackResult result) noexcept
{
    using enum wavplay::playback::PlaybackResult;
    switch (result) {
    case playing: return "playing";
    case completed: return "completed";
    case underrun: return "underrun";
    case read_error: return "read_error";
    case driver_error: return "driver_error";
    case reinitialize_required: return "reinitialize_required";
    case next_rate_rejected: return "next_rate_rejected";
    case next_track_rejected: return "next_track_rejected";
    case first_rate_rejected: return "first_rate_rejected";
    case paused: return "paused";
    case stopped: return "stopped";
    }
    return "unknown";
}

class Reporter {
public:
    void show(const PlaybackSnapshot& status)
    {
        if (status.revision == revision_) {
            return;
        }
        revision_ = status.revision;
        std::printf("State revision %llu: %s; accepted=%llu settled=%llu; frame=%llu of %llu; result=%s\n",
            static_cast<unsigned long long>(status.revision), state_name(status.state),
            static_cast<unsigned long long>(status.accepted), static_cast<unsigned long long>(status.settled),
            static_cast<unsigned long long>(status.position_frames), static_cast<unsigned long long>(status.frame_count),
            result_name(status.result));
        for (std::size_t index = 0; index < status.diagnostics.segments.size(); ++index) {
            if (const auto& setup = status.diagnostics.segments[index]) {
                std::printf("  Segment %zu: %.15g Hz, outputs %s / %s, width %ld, output latency %ld frames, "
                    "start silence %ld blocks (%.1f ms), prefetch ring %s\n", index + 1, setup->sample_rate,
                    wavplay::asio::sample_type_name(setup->output_types[0]), wavplay::asio::sample_type_name(setup->output_types[1]),
                    setup->buffer_frames, setup->output_latency_frames, setup->start_silence_blocks,
                    1000.0 * setup->start_silence_blocks * setup->buffer_frames / setup->sample_rate,
                    setup->ring_pages_locked ? "locked" : "pageable");
                std::printf("    Callback metadata: ring %s, renderer %s, requests %s, image %s\n",
                    setup->ring_metadata_pages_locked ? "locked" : "pageable",
                    setup->renderer_pages_locked ? "locked" : "pageable",
                    setup->request_pages_locked ? "locked" : "pageable",
                    setup->image_pages_locked ? "locked" : "pageable");
            }
        }
        if (status.error) {
            try {
                std::rethrow_exception(status.error);
            } catch (const std::exception& error) {
                std::printf("  Controller error: %s\n", error.what());
            } catch (...) {
                std::puts("  Controller error: non-standard exception");
            }
        }
        std::fflush(stdout);
    }

private:
    std::uint64_t revision_{};
};

void check_failure(const PlaybackSnapshot& status)
{
    if (status.error) {
        std::rethrow_exception(status.error);
    }
    using enum wavplay::playback::PlaybackResult;
    if (status.result != playing && status.result != completed && status.result != paused && status.result != stopped) {
        // Close can finish releasing the device while retaining a non-exception
        // fault reported during stop/dispose. A closed state is not success by itself.
        throw std::runtime_error(std::string("Playback controller failed: ") + result_name(status.result));
    }
    if (status.state == PlaybackState::failed) {
        throw std::runtime_error("Playback controller failed");
    }
}

void report_submission(PlaybackCommand command, std::uint64_t receipt)
{
    if (receipt == 0) {
        throw std::runtime_error(std::string("Controller rejected ") + command_name(command));
    }
    std::printf("Accepted %s receipt %llu.\n", command_name(command), static_cast<unsigned long long>(receipt));
    std::fflush(stdout);
}

std::uint64_t submit(PlaybackController& controller, PlaybackCommand command)
{
    const auto receipt = controller.submit(command);
    report_submission(command, receipt);
    return receipt;
}

PlaybackSnapshot wait_for_state(PlaybackController& controller, Reporter& reporter, PlaybackState expected,
    std::uint64_t receipt, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    auto status = controller.snapshot();
    for (;;) {
        reporter.show(status);
        check_failure(status);
        if (status.state == expected && status.settled >= receipt) {
            return status;
        }
        if (status.state == PlaybackState::closed ||
            (status.state == PlaybackState::completed && status.settled >= receipt &&
                expected != PlaybackState::closed && expected != PlaybackState::completed)) {
            throw std::runtime_error("Playback ended before the requested controller operation completed");
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) {
            throw std::runtime_error(std::string("Timed out waiting for controller state ") + state_name(expected));
        }
        status = controller.wait_for_change(status.revision, remaining);
    }
}

void hold_state(PlaybackController& controller, Reporter& reporter, PlaybackState expected,
    std::chrono::milliseconds duration)
{
    const auto deadline = Clock::now() + duration;
    auto status = controller.snapshot();
    for (;;) {
        reporter.show(status);
        check_failure(status);
        if (status.state != expected) {
            throw std::runtime_error("Playback state changed before the timed controller operation");
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) {
            return;
        }
        // Explicit waits belong to this CLI thread, never to the UI-facing submit path.
        status = controller.wait_for_change(status.revision, remaining);
    }
}

PlaybackSnapshot wait_for_progress(PlaybackController& controller, Reporter& reporter,
    std::uint64_t frames, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    auto status = controller.snapshot();
    for (;;) {
        reporter.show(status);
        check_failure(status);
        if (status.state != PlaybackState::playing) {
            throw std::runtime_error("Playback ended before the same-item restart progress check");
        }
        if (status.position_frames >= frames) {
            return status;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) {
            throw std::runtime_error("Timed out waiting for progress before the same-item restart");
        }
        status = controller.wait_for_change(status.revision, remaining);
    }
}

} // namespace

void audition_asio_controller(const wchar_t* clsid_text, const wchar_t* path, const wchar_t* scenario_text,
    wavplay::asio::ActivateDriver activate)
{
    const std::wstring_view scenario(scenario_text);
    if (scenario != L"pause-resume" && scenario != L"stop-play" && scenario != L"select-play" &&
        scenario != L"close-playing" && scenario != L"close-paused") {
        throw std::invalid_argument("Controller audition scenario must be pause-resume, stop-play, select-play, close-playing or close-paused");
    }
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    // Refuse an unsuitable test track before opening a real driver. Retain this
    // protection until the controller has opened its own WAV on the owning STA.
    std::optional<wavplay::media::WavReader> preflight(std::in_place, path);
    const auto input_frames = preflight->info().frame_count;
    if (preflight->info().format.sample_rate != 48000 || preflight->info().format.channels != 2 || input_frames < 5 * 48000) {
        throw std::invalid_argument("Controller audition requires a 48 kHz stereo PCM16/24/32 WAV of at least five seconds");
    }
    PlaybackController controller(clsid, path, activate);
    Reporter reporter;
    try {
        std::printf("Controller audition: %ls; source %llu frames at 48000 Hz.\n", scenario_text,
            static_cast<unsigned long long>(input_frames));
        std::puts("Receipts acknowledge acceptance; settled numbers acknowledge completion. Frame positions use submitted PCM while playing and are confirmed when idle.");
        std::fflush(stdout);
        const auto ready = wait_for_state(controller, reporter, PlaybackState::stopped, 0, 10s);
        if (ready.frame_count != input_frames) {
            throw std::runtime_error("Controller WAV differs from the protected preflight input");
        }
        preflight.reset();

        const auto play = submit(controller, PlaybackCommand::play);
        (void)wait_for_state(controller, reporter, PlaybackState::playing, play, 10s);
        if (scenario != L"select-play") {
            hold_state(controller, reporter, PlaybackState::playing, scenario == L"stop-play" ? 4300ms : 2500ms);
        }

        if (scenario == L"pause-resume" || scenario == L"close-paused") {
            const auto pause = submit(controller, PlaybackCommand::pause);
            const auto paused = wait_for_state(controller, reporter, PlaybackState::paused, pause, 10s);
            if (paused.position_frames == 0 || paused.position_frames >= input_frames) {
                throw std::runtime_error("Controller did not pause inside the audition track");
            }
            if (scenario == L"pause-resume") {
                hold_state(controller, reporter, PlaybackState::paused, 1s);
                const auto resume = submit(controller, PlaybackCommand::play);
                (void)wait_for_state(controller, reporter, PlaybackState::playing, resume, 10s);
            }
        } else if (scenario == L"stop-play") {
            // Do not wait for Stop completion before enqueueing Play. The control
            // layer must preserve this ordering without reentering playback.
            // Logging can block, so issue both requests before reporting either.
            const auto stop = controller.submit(PlaybackCommand::stop);
            const auto replay = controller.submit(PlaybackCommand::play);
            report_submission(PlaybackCommand::stop, stop);
            report_submission(PlaybackCommand::play, replay);
            const auto restarted = wait_for_state(controller, reporter, PlaybackState::playing, replay, 10s);
            if (restarted.position_frames != 0) {
                throw std::runtime_error("Play immediately after Stop did not restart at frame zero");
            }
            std::puts("Consecutive Stop/Play settled; playback restarted from frame 0.");
            std::fflush(stdout);
        } else if (scenario == L"select-play") {
            const auto progressed = wait_for_progress(controller, reporter, 48000, 10s);
            if (!progressed.current_index || progressed.position_frames >= input_frames) {
                throw std::runtime_error("Controller has no active item inside the audition track");
            }
            const auto index = *progressed.current_index;
            const auto replay = controller.select_track(index, true);
            report_submission(PlaybackCommand::select_play, replay);
            std::printf("Same-item Select/Play progress: index %zu, frame %llu of %llu.\n", index,
                static_cast<unsigned long long>(progressed.position_frames), static_cast<unsigned long long>(input_frames));
            const auto restarted = wait_for_state(controller, reporter, PlaybackState::playing, replay, 10s);
            if (restarted.position_frames != 0 || restarted.current_index != index || restarted.selected_index != index ||
                restarted.sample_rate != 48000 || restarted.frame_count != input_frames) {
                throw std::runtime_error("Same-item Select/Play did not restart the original track at frame zero");
            }
            std::puts("Same-item Select/Play settled; playback restarted from frame 0.");
            std::fflush(stdout);
        }

        if (scenario == L"pause-resume" || scenario == L"stop-play" || scenario == L"select-play") {
            const auto duration = std::chrono::milliseconds((input_frames * 1000 + 47999) / 48000);
            const auto completed = wait_for_state(controller, reporter, PlaybackState::completed, 0, duration + 10s);
            if (completed.position_frames != input_frames) {
                throw std::runtime_error("Controller audition did not reach the final WAV frame");
            }
            std::printf("Completed at frame %llu of %llu.\n", static_cast<unsigned long long>(completed.position_frames),
                static_cast<unsigned long long>(input_frames));
        }

        const auto close = submit(controller, PlaybackCommand::close);
        (void)wait_for_state(controller, reporter, PlaybackState::closed, close, 10s);
        controller.join();
        reporter.show(controller.snapshot());
        std::puts("Controller audition completed; device released and control thread joined.");
        std::fflush(stdout);
    } catch (...) {
        const auto failure = std::current_exception();
        // Never abandon a live controller after a timeout or a rejected operation.
        // An in-progress synchronous driver call cannot be forcibly interrupted.
        (void)controller.submit(PlaybackCommand::close);
        controller.join();
        reporter.show(controller.snapshot());
        std::rethrow_exception(failure);
    }
}
