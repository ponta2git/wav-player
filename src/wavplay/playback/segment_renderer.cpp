// Real-time translation unit: every function an ASIO callback can reach, apart
// from stream::PrefetchRing::pop_block (behind the inline Consumer::pop),
// Consumer::read_failed (an atomic load), the inline PlaybackSignal operations
// and the inline block copy and zero of core/block_copy.h, is defined here.
// Review changes against the callback rules in segment_renderer.h.

#include "wavplay/playback/segment_renderer.h"

#include "wavplay/core/block_copy.h"
#include "wavplay/core/memory.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace wavplay::playback {
namespace {

// The driver's table below names these; they are defined after the state they use.
void switch_buffer(long half, ASIOBool) noexcept;
void sample_rate_changed(ASIOSampleRate rate) noexcept;
long asio_message(long selector, long value, void*, double*) noexcept;

// Intentional padding (C4324) gives the route a cache line of its own.
#pragma warning(push)
#pragma warning(disable : 4324)
// Routing to the attached renderer. Every switch writes its writer flag; the
// control thread writes the line at attach, detach and set_streaming.
struct alignas(core::cache_line_size) CallbackRoute {
    std::atomic_flag writers[2]{};
    std::atomic<SegmentRenderer*> active{};
    std::atomic<bool> streaming{};
};
// All process-wide state a callback touches, in one 128-byte block: one page and
// one adjacent-line pair (core/alignment.h). Line 0 is the route, the line written
// on every switch. Line 1 holds the signal and the driver's table, which the
// driver reads to call back. Line 1 is read-mostly from the callback's view only:
// the control thread reads it at every wake (0.5 s to just under 1 s apart, see
// control_wake_period in segment.cpp) and writes it only at request and phase
// boundaries, Stop and faults; a stopping or failing callback compare-exchanges it.
struct alignas(core::adjacent_line_pair) CallbackState {
    CallbackRoute route;
    PlaybackSignal signal;
    // Every non-null entry is a root of the real-time machine-code audit
    // (Assert-RealTimeCode in scripts/verify.ps1); a callback added here is added there.
    ASIOCallbacks table{switch_buffer, sample_rate_changed, asio_message, nullptr};
};
#pragma warning(pop)
static_assert(sizeof(CallbackState) == core::adjacent_line_pair);
static_assert(offsetof(CallbackState, route) == 0 && offsetof(CallbackState, signal) == core::cache_line_size);
static_assert(std::atomic<SegmentRenderer*>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

// Driver threads can still call back after static destruction began (for example
// after a failed stop), so the lifetime of no member may end: the whole block is
// NoDestroy, and the signal's event lives until exit. constinit initializes it as
// a constant; the compiler still registers NoDestroy's empty destructor with
// atexit, which destroys nothing.
constinit core::NoDestroy<CallbackState> callback_state;

// Inline accessors, not stored references: callbacks address the block directly
// instead of first loading a pointer to it.
[[nodiscard]] CallbackRoute& route() noexcept
{
    return callback_state.value.route;
}

[[nodiscard]] PlaybackSignal& callback_signal() noexcept
{
    return callback_state.value.signal;
}

// A detached switch writes nothing: the control thread silences both halves
// before it disposes of buffers that a writer may have touched, so a driver's
// final switch inside disposeBuffers, on any thread, already finds silence.
void switch_buffer(long half, ASIOBool) noexcept
{
    const auto observed = callback_signal().snapshot();
    if (half != 0 && half != 1) {
        callback_signal().invalidate(observed);
        return;
    }
    // Validated once: from here on the half is an index, not the SDK's long.
    const auto index = static_cast<std::size_t>(half);
    // Pin the renderer with the same flag that excludes recursive writes.
    // Sequentially consistent acquisition/peer checks prevent two consumers.
    auto& writer = route().writers[index];
    if (writer.test_and_set()) {
        callback_signal().invalidate(observed, PlaybackResult::underrun);
        return;
    }
    if (auto* renderer = route().active.load()) {
        // A callback paused before pinning its writer can resume after the next
        // segment starts. Only callbacks from this phase may touch its ring or buffers.
        if (PlaybackSignal::same_phase(observed, renderer->token())) {
            renderer->render(index, observed);
        }
    }
    writer.clear(std::memory_order_release);
}

void sample_rate_changed(ASIOSampleRate rate) noexcept
{
    callback_signal().rate_changed(callback_signal().snapshot(), rate);
}

long asio_message(long selector, long value, void*, double*) noexcept
{
    const auto observed = callback_signal().snapshot();
    if (selector == kAsioEngineVersion) {
        return 2;
    }
    const long request = selector == kAsioSelectorSupported ? value : selector;
    const bool supported = request == kAsioSelectorSupported || request == kAsioEngineVersion ||
        request == kAsioResetRequest || request == kAsioOverload;
    if (selector == kAsioSelectorSupported) {
        return supported;
    }
    if (supported || selector == kAsioBufferSizeChange || selector == kAsioResyncRequest ||
        selector == kAsioLatenciesChanged) {
        // A driver can report a dropped block while stop or dispose is finishing the drain.
        callback_signal().invalidate(observed,
            selector == kAsioOverload ? PlaybackResult::underrun :
            selector == kAsioResetRequest || selector == kAsioBufferSizeChange ?
                PlaybackResult::reinitialize_required : PlaybackResult::driver_error);
        return supported;
    }
    return 0;
}

} // namespace

PlaybackSignal& notifications() noexcept
{
    return callback_signal();
}

bool is_streaming() noexcept
{
    return route().streaming.load(std::memory_order_acquire);
}

namespace callback_routing {

ASIOCallbacks* callbacks() noexcept
{
    return &callback_state.value.table;
}

void attach(SegmentRenderer& renderer) noexcept
{
    route().active.store(&renderer);
}

bool detach() noexcept
{
    if (!route().active.exchange(nullptr)) {
        return false;
    }
    for (const auto& writer : route().writers) {
        // A switch that already loaded this renderer can still be copying: one block
        // copy, microseconds. Spin briefly, then yield, and only then sleep, so
        // teardown does not pay a 15.6 ms timer tick for a copy about to finish.
        for (unsigned spins = 0; writer.test(); ++spins) {
            if (spins < 256) {
                YieldProcessor();
            } else if (spins < 512) {
                SwitchToThread();
            } else {
                Sleep(1);
            }
        }
    }
    return true;
}

void set_streaming(bool value) noexcept
{
    route().streaming.store(value, std::memory_order_release);
}

} // namespace callback_routing

SegmentRenderer::SegmentRenderer(stream::PrefetchRing& queue, PlaybackSignal::Token token,
    std::chrono::milliseconds start_silence, const SegmentControl& control) noexcept
    : queue_(queue.consumer()), pause_requested_(control.pause_requested), stop_requested_(control.stop_requested),
      token_(token), width_(queue.block_frames()), channel_bytes_(queue.block_bytes()),
      // Whole blocks covering the requested time at this segment's rate.
      start_silence_blocks_((static_cast<std::size_t>(start_silence.count()) * queue.info().format.sample_rate +
          1000 * width_ - 1) / (1000 * width_))
{
}

void SegmentRenderer::set_output_latency(long frames) noexcept
{
    // Prefilled B is one switch ahead of A. Allow at least that block to sound.
    latency_frames_ = std::max(width_, static_cast<std::size_t>(frames));
}

stream::BlockStatus SegmentRenderer::prefill()
{
    // Only B owns PCM before start. Leave a defined first A even if a
    // driver misses its first switch.
    silence(0);
    // Start silence must not hide a producer failure after initial priming.
    // The same pre-start contract applies whether B would hold silence or PCM.
    if (queue_.read_failed()) {
        silence(1);
        return stream::BlockStatus::read_error;
    }
    if (start_silence_blocks_) {
        // B is the first silent block; switches return the rest before the first PCM block.
        silence(1);
        silent_switches_left_ = start_silence_blocks_ - 1;
        return stream::BlockStatus::ready;
    }
    const auto first = pop(1);
    if (first == stream::BlockStatus::read_error) {
        // The producer failed after priming. B stays defined; the segment does not start.
        silence(1);
        return first;
    }
    if (first == stream::BlockStatus::empty) {
        throw std::runtime_error("Could not prefill ASIO buffer B"); // A primed ring always holds a block.
    }
    final_in_prefill_ = first == stream::BlockStatus::last;
    return first;
}

// ready and last, the only results that copied a block, stay the two lowest
// values: one unsigned compare against last separates them from every other
// result, and a result added later fails closed (deliver silences the half).
static_assert(std::to_underlying(stream::BlockStatus::ready) == 0 &&
    std::to_underlying(stream::BlockStatus::last) == 1);

// Forced inline into deliver, its real-time caller; prefill gets a copy of its own.
[[msvc::forceinline]] stream::BlockStatus SegmentRenderer::pop(std::size_t half) noexcept
{
    const auto block = queue_.pop(buffers_[0].buffers[half], buffers_[1].buffers[half]);
    if (block.status <= stream::BlockStatus::last) {
        // One count, one store per block. The popping renderer is the only writer,
        // so a relaxed load and store publish it without a locked add.
        progress_frames_.store(progress_frames_.load(std::memory_order_relaxed) + block.valid_frames,
            std::memory_order_relaxed);
    }
    if (block.status == stream::BlockStatus::last) [[unlikely]] {
        // Relative to the submission of the final block, including B before
        // start. Exclude its zero padding and count only subsequent switches.
        drain_remaining_ = drain_latency() + block.valid_frames;
    }
    return block.status;
}

std::size_t SegmentRenderer::drain_latency() const noexcept
{
    // Steady-state latency already includes B's one-block lead. Additional
    // startup submissions may sit ahead of that pipeline; retain both that
    // extra lead and any sub-block latency instead of taking only the max.
    // If a driver included all pre-roll in its report this only waits longer.
    return latency_frames_ + (preroll_lead_ > width_ ? preroll_lead_ - width_ : 0);
}

void SegmentRenderer::silence(std::size_t half) noexcept
{
    for (const auto& channel : buffers_) {
        core::zero_block(channel.buffers[half], channel_bytes_);
    }
}

// The steps of render are forced inline, so the steady PCM path calls none of
// them; [[unlikely]] places their fault, pause and drain blocks after its return.
//
// Watchdog evidence. Only callbacks write the count, so a relaxed load and store
// replace a locked increment. Renders of the two halves can overlap (the peer
// half's underrun check) and lose an increment, which is harmless: the control
// thread only asks whether the count changed since its previous reading.
[[msvc::forceinline]] void SegmentRenderer::note_switch() noexcept
{
    switches_.store(switches_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Every switch: records an underrun or a broken sequence, and otherwise advances
// the expected half. Returns whether the sequence held.
[[msvc::forceinline]] bool SegmentRenderer::accept_sequence(std::size_t half) noexcept
{
    if (route().writers[1 - half].test()) [[unlikely]] {
        callback_signal().invalidate(token_, PlaybackResult::underrun);
        return false;
    }
    if (half != next_half_) [[unlikely]] {
        callback_signal().invalidate(token_);
        return false;
    }
    // Keep tracking valid switches until stop returns. A broken
    // sequence during the drain/stop handoff must block the next run.
    next_half_ = 1 - half;
    return true;
}

// Live phase only: a Stop request ends the phase. Returns whether it did.
[[msvc::forceinline]] bool SegmentRenderer::accept_stop() noexcept
{
    if (stop_requested_.load(std::memory_order_acquire)) [[unlikely]] {
        callback_signal().stop(token_);
        return true;
    }
    return false;
}

// Live phase only. Returns true only when `half` holds a PCM block of this
// phase. Otherwise the caller silences it: start silence, a pause or drain
// countdown, or a pop that failed or lost its phase.
[[msvc::forceinline]] bool SegmentRenderer::deliver(std::size_t half) noexcept
{
    if (!hardware_started_) [[unlikely]] {
        hardware_started_ = is_streaming();
        if (!hardware_started_ && !drain_remaining_) {
            preroll_lead_ += width_;
        }
    }
    // Freeze PCM consumption before popping another block. The old ring
    // can be arbitrarily far ahead; only submitted valid PCM is retained.
    // An EOF already in flight wins over a later pause request.
    if (!drain_remaining_ && pause_requested_.load(std::memory_order_acquire)) [[unlikely]] {
        if (progress_frames_.load(std::memory_order_relaxed) == 0) {
            callback_signal().finish(token_, PlaybackResult::paused);
            return false;
        }
        draining_pause_ = true;
        // The previous full block was submitted one switch ago. Count
        // this switch below, just as if that block had been an EOF.
        drain_remaining_ = drain_latency() + width_;
    }
    if (drain_remaining_ || silent_switches_left_) [[unlikely]] {
        // These switches do not consume the ring, and a pause must not mask a
        // producer failure. A pop reports it itself. This is an atomic query.
        if (queue_.read_failed()) {
            callback_signal().invalidate(token_, PlaybackResult::read_error);
        } else if (!drain_remaining_) {
            --silent_switches_left_; // Pre-roll silence also counts toward the queue ahead of hardware.
        } else if (final_in_prefill_ || hardware_started_) {
            // B precedes the first A by one buffer. Any further startup
            // switches can be pre-roll at the same instant (SDK p. 8).
            final_in_prefill_ = false;
            if (drain_remaining_ <= width_) {
                callback_signal().finish(token_, draining_pause_ ? PlaybackResult::paused : PlaybackResult::completed);
            } else {
                drain_remaining_ -= width_;
            }
        }
        return false;
    }
    const auto status = pop(half);
    if (status > stream::BlockStatus::last) [[unlikely]] {
        // No block was copied: empty, read_error or any result added later.
        callback_signal().invalidate(token_, status == stream::BlockStatus::empty ? PlaybackResult::underrun
                                                                                : PlaybackResult::read_error);
        return false;
    }
    // A stop reason recorded during the copy wins: the caller's silence overwrites the block.
    return callback_signal().snapshot() == token_;
}

void SegmentRenderer::render(std::size_t half, PlaybackSignal::Token observed) noexcept
{
    note_switch();
    // Sequence faults are recorded even after a Stop, since faults win. Everything
    // else runs only while the phase observed at entry is still playing, so a
    // latched Stop or fault costs no read-modify-write in later callbacks. The
    // control thread repeats the Stop and read-failure checks after the stream stops.
    if (!accept_sequence(half) || observed != token_ || accept_stop()) [[unlikely]] {
        silence(half);
        return;
    }
    if (!deliver(half)) [[unlikely]] {
        silence(half);
    }
}

} // namespace wavplay::playback
