#include "wavplay/playback/segment.h"

#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/core/memory.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/core/system_sleep.h"
#include "wavplay/playback/output_format.h"
#include "wavplay/playback/segment_renderer.h"
#include "wavplay/stream/prefetch_ring.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace wavplay::playback {

static_assert(std::is_same_v<decltype(BufferSetup::sample_rate), ASIOSampleRate>);
static_assert(std::is_same_v<decltype(BufferSetup::output_types)::value_type, ASIOSampleType>);

namespace {

// No real driver reports seconds of output latency.
constexpr std::uint32_t max_output_latency_seconds = 2;

// start() itself has no specified time limit. Once it returns, the switch
// watchdog allows four full buffer periods, with the control wake period (see
// control_wake_period) as a floor for scheduling and USB jitter.
constexpr std::uint64_t switch_watchdog_periods = 4;
// Silence returned after the reported latency for a driver that queues more
// (see play_out_driver_queue), in block periods of wall time.
constexpr std::uint64_t driver_queue_linger_periods = 2;

// The wall time of `periods` blocks of `width` frames at `rate`, rounded up to
// a whole tick of Duration.
template<class Duration>
constexpr Duration block_periods(std::uint64_t periods, std::size_t width, std::uint32_t rate) noexcept
{
    static_assert(Duration::period::num == 1, "a tick must be a whole fraction of a second");
    constexpr std::uint64_t ticks_per_second = Duration::period::den;
    return Duration(static_cast<typename Duration::rep>((periods * ticks_per_second * width + rate - 1) / rate));
}

// A 512-frame block at 44.1 kHz lasts 11.61 ms.
static_assert(block_periods<std::chrono::milliseconds>(4, 512, 44100) == std::chrono::milliseconds(47));
static_assert(block_periods<std::chrono::nanoseconds>(2, 512, 44100) == std::chrono::nanoseconds(23'219'955));

// The control thread's periodic wake while streaming, derived once per segment:
// the progress interval and the switch watchdog's floor. A wait can end up to
// one 15.625 ms default timer tick late, and submitted PCM advances in whole
// blocks. With a period of exactly 1 s the sampled position therefore drifts
// and periodically crosses two second boundaries between wakes, so the display
// skips a second. Stay below 1 s by a tick-sized margin plus one block, but
// never wake more often than twice a second. Blocks up to 47 ms (2048 frames at
// 44.1 kHz) give 933-979 ms: the watchdog floor is slightly below the former
// 1 s, still far above four such blocks (at most 188 ms). The floor shrinks as
// blocks grow and meets four blocks at about 196 ms blocks (8640 frames at
// 44.1 kHz: 784 ms, formerly 1 s). From there four blocks alone set the window
// and leave three block periods of slack, as they did from 250 ms blocks under
// the former floor.
constexpr std::chrono::milliseconds control_wake_period(std::size_t width, std::uint32_t rate) noexcept
{
    constexpr std::chrono::milliseconds displayed_second{1000}, timer_margin{20}, shortest{500};
    return std::max(shortest, displayed_second - timer_margin - block_periods<std::chrono::milliseconds>(1, width, rate));
}

static_assert(control_wake_period(8, 384000) == std::chrono::milliseconds(979));
static_assert(control_wake_period(512, 44100) == std::chrono::milliseconds(968));
static_assert(control_wake_period(2048, 44100) == std::chrono::milliseconds(933));
static_assert(control_wake_period(8640, 44100) == std::chrono::milliseconds(784) &&
    block_periods<std::chrono::milliseconds>(switch_watchdog_periods, 8640, 44100) == std::chrono::milliseconds(784));
static_assert(control_wake_period(24000, 48000) == std::chrono::milliseconds(500)); // Half-second blocks.

// Ends the request when WM_QUIT or a failure interrupted its wait; the caller
// handles the other results. WM_QUIT ends it the way a user Stop does: it is a
// shutdown, not a fault.
void end_interrupted_wait(PlaybackSignal& signal, PlaybackSignal::Token token, core::WaitResult wake) noexcept
{
    if (wake == core::WaitResult::quit) {
        signal.stop(token);
    } else if (wake == core::WaitResult::failed) {
        signal.invalidate(token);
    }
}

ASIOSampleRate check_rate(IASIO& driver)
{
    const auto rate = asio::current_sample_rate(driver);
    if (!rate || !notifications().matches_rate(*rate)) {
        throw std::runtime_error("ASIO rate differs from WAV rate");
    }
    return *rate;
}

// The control side of one segment. The renderer it owns receives the callbacks
// between attach and detach; every driver call stays on this thread.
class SegmentRunner {
public:
    SegmentRunner(IASIO& driver, stream::PrefetchRing& queue, PlaybackSignal::Token token,
        std::chrono::milliseconds start_silence, SegmentControl& control, long output_pair)
        : driver_(driver), queue_(queue), signal_(notifications()), token_(token), control_(control),
          renderer_(queue, token, start_silence, control), width_(renderer_->width()), output_pair_(output_pair)
    {
        // Only setup maps the physical outputs; callback copying remains pair-local.
        renderer_->buffers()[0].channelNum = output_pair - 1;
        renderer_->buffers()[1].channelNum = output_pair;
        control_.live_frames = &renderer_->progress_frames();
    }

    ~SegmentRunner()
    {
        callback_routing::detach();
        // Commit once, after every writer is gone, including partial starts
        // and exceptional cleanup. No callback writes to SegmentControl.
        control_.submitted_frames += renderer_->submitted_frames();
        control_.live_frames = nullptr;
    }

    SegmentRunner(const SegmentRunner&) = delete;
    SegmentRunner& operator=(const SegmentRunner&) = delete;

    PlaybackResult run(std::optional<BufferSetup>* setup)
    {
        if (!continue_playback()) {
            return signal_.result();
        }
        check_rate(driver_);
        if (!continue_playback()) {
            return signal_.result();
        }
        create_buffers();
        try {
            if (continue_playback()) {
                prepare_outputs(setup);
                if (prime_outputs()) {
                    start_and_stop();
                }
            }
        } catch (...) {
            signal_.invalidate(token_, PlaybackResult::driver_error);
            if (callback_routing::detach()) {
                // stop failed with live buffers. Retire all asynchronous writes
                // before the driver can free them, including a copy already in flight.
                // Both halves then hold silence for the driver's implicit final
                // switch during disposal, whichever thread requests it.
                renderer_->silence(0);
                renderer_->silence(1);
            }
            // The common ASIO error boundary keeps the original failure as a
            // nested cause, including a failed start followed by a failed stop.
            asio::throw_if_error(driver_.disposeBuffers(), "ASIO disposeBuffers failed during cleanup");
            throw;
        }
        asio::throw_if_error(driver_.disposeBuffers(), "ASIO disposeBuffers failed");
        control_.accept_stop(signal_, token_);
        return signal_.result();
    }

private:
    IASIO& driver_;
    stream::PrefetchRing& queue_;
    PlaybackSignal& signal_;
    const PlaybackSignal::Token token_;
    SegmentControl& control_;
    core::ResidentObject<SegmentRenderer> renderer_;
    // The renderer's block width, copied before attach: its width_ shares a line
    // that every switch writes, and the timed waits run while writers are attached.
    const std::size_t width_;
    const long output_pair_;
    // Created only when playback is about to start. Keep the request until
    // buffers are disposed, including failure cleanup and the final drain.
    std::optional<core::SystemSleepGuard> sleep_guard_;

    void create_buffers()
    {
        auto& buffers = renderer_->buffers();
        const auto error = driver_.createBuffers(buffers, 2, static_cast<long>(width_),
            callback_routing::callbacks());
        if (error == ASE_OK) {
            return;
        }
        try {
            asio::throw_if_error(error, "ASIO createBuffers failed");
        } catch (...) {
            // A driver may leave partial output addresses, even if it already
            // unwound the allocation. Never touch them. An untouched request
            // gives us no buffers to dispose on this driver. If cleanup also
            // fails, retain the allocation failure as its nested cause.
            if (buffers[0].buffers[0] || buffers[0].buffers[1] || buffers[1].buffers[0] || buffers[1].buffers[1]) {
                const auto cleanup = driver_.disposeBuffers();
                if (cleanup != ASE_InvalidMode) {
                    asio::throw_if_error(cleanup, "ASIO disposeBuffers failed after createBuffers error");
                }
            }
            throw;
        }
    }

    // Keep control delivery, Stop priority and generation checking in one order
    // at every setup/wait boundary. Driver calls can dispatch reentrant messages.
    void poll_control_requests() noexcept
    {
        control_.poll(signal_, token_);
    }

    [[nodiscard]] bool continue_playback() noexcept
    {
        poll_control_requests();
        return signal_.snapshot() == token_;
    }

    void prepare_outputs(std::optional<BufferSetup>* setup)
    {
        // The clock and output pair may have changed during buffer negotiation.
        BufferSetup prepared{};
        prepared.buffer_frames = static_cast<long>(width_);
        prepared.sample_rate = check_rate(driver_);
        prepared.output_latency_frames = validate_created_outputs(driver_, renderer_->buffers(),
            width_, queue_.info().format, queue_.output_format(), prepared.output_types, output_pair_);
        // Pause and EOF wait out the reported latency; a bogus value must not
        // leave them draining for hours while switches keep arriving.
        if (static_cast<std::uint64_t>(prepared.output_latency_frames) >
            std::uint64_t{max_output_latency_seconds} * queue_.info().format.sample_rate) {
            throw std::runtime_error("Implausible ASIO output latency");
        }
        renderer_->set_output_latency(prepared.output_latency_frames);
        prepared.start_silence_blocks = static_cast<long>(renderer_->start_silence_blocks());
        prepared.ring_pages_locked = queue_.pages_locked();
        prepared.ring_metadata_pages_locked = queue_.metadata_pages_locked();
        prepared.renderer_pages_locked = renderer_.locked();
        prepared.request_pages_locked = control_.request_pages_locked;
        // Process-wide result of core::lock_process_image; false where it was
        // never called, as in a test suite's process.
        prepared.image_pages_locked = core::process_image_locked();
        if (setup) {
            *setup = prepared;
        }
    }

    bool prime_outputs()
    {
        if (!continue_playback()) {
            return false;
        }
        if (renderer_->prefill() == stream::BlockStatus::read_error) {
            // The producer failed between priming and prefill: a read error, not a driver fault.
            signal_.finish(token_, PlaybackResult::read_error);
            return false;
        }
        return signal_.snapshot() == token_;
    }

    void start_and_stop()
    {
        if (!continue_playback()) {
            return;
        }
        sleep_guard_.emplace();
        callback_routing::attach(*renderer_); // start may enter the first callback synchronously.
        const auto start_error = driver_.start();
        if (start_error == ASE_OK) {
            callback_routing::set_streaming(true);
            if (control_.observer.started) {
                control_.observer.started(control_.observer.context);
            }
            wait_for_stop();
            play_out_driver_queue();
            callback_routing::set_streaming(false);
        } else {
            signal_.invalidate(token_, PlaybackResult::driver_error);
        }
        // Even a failed start can have launched a writer.
        const auto stop_error = driver_.stop();
        if (stop_error == ASE_OK) {
            callback_routing::detach();
        }
        if (start_error != ASE_OK) {
            try {
                asio::throw_if_error(start_error, "ASIO start failed");
            } catch (...) {
                asio::throw_if_error(stop_error, "ASIO stop failed after start error");
                throw;
            }
        }
        asio::throw_if_error(stop_error, "ASIO stop failed");
    }

    void wait_for_stop() noexcept
    {
        // start() has returned: each watchdog period must now see a buffer switch.
        const auto progress_interval = control_wake_period(width_, queue_.info().format.sample_rate);
        const auto timeout = std::max(progress_interval, block_periods<std::chrono::milliseconds>(
            switch_watchdog_periods, width_, queue_.info().format.sample_rate));
        const auto started_at = std::chrono::steady_clock::now();
        auto deadline = started_at + timeout;
        auto progress_deadline = started_at + progress_interval;
        // Switches during start() do not count: the first period starts here.
        auto seen = renderer_->switches();
        for (;;) {
            control_.accept_stop(signal_, token_);
            if (signal_.snapshot() != token_) {
                return;
            }
            // Normal buffers already wake once per progress interval for the
            // watchdog. Only unusually long blocks need an earlier progress wake;
            // that wake must never postpone the callback watchdog's own deadline.
            const auto wake = signal_.wait_until(control_.observer.poll ?
                std::min(deadline, progress_deadline) : deadline);
            poll_control_requests();
            end_interrupted_wait(signal_, token_, wake); // Includes a quit request racing the final drain.
            if (wake == core::WaitResult::timed_out) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    // One reading per deadline: the period needs a switch after the previous reading.
                    const auto count = renderer_->switches();
                    if (count == seen) {
                        signal_.finish(token_, PlaybackResult::driver_error);
                    }
                    seen = count;
                    // An unrelated control wake is not evidence of a live driver.
                    deadline = now + timeout;
                }
                if (now >= progress_deadline) { progress_deadline = now + progress_interval; }
            }
        }
    }

    // The frame count above trusts the reported latency. A driver can queue
    // more: a tested driver requested about three buffers at once after start() had
    // returned, and stop() discards what is still queued. Keep returning
    // silence for two more block periods of wall time before stopping.
    void play_out_driver_queue() noexcept
    {
        const auto linger = block_periods<std::chrono::nanoseconds>(
            driver_queue_linger_periods, width_, queue_.info().format.sample_rate);
        const auto deadline = std::chrono::steady_clock::now() + linger;
        for (;;) {
            control_.accept_stop(signal_, token_);
            const auto result = signal_.result();
            if (result != PlaybackResult::completed && result != PlaybackResult::paused) {
                return;
            }
            const auto wake = signal_.wait_until(deadline);
            poll_control_requests();
            end_interrupted_wait(signal_, token_, wake); // WM_QUIT stays posted for the outer loop.
            if (wake != core::WaitResult::signaled) {
                return;
            }
        }
    }
};

} // namespace

PlaybackResult run_segment(IASIO& driver, stream::PrefetchRing& queue, PlaybackSignal::Token token,
    SegmentControl& control, std::optional<BufferSetup>* setup,
    std::chrono::milliseconds start_silence, long output_pair)
{
    control.phase_token = token;
    return SegmentRunner(driver, queue, token, start_silence, control, output_pair).run(setup);
}

} // namespace wavplay::playback
