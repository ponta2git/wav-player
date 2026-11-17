#pragma once

#include "wavplay/core/alignment.h"
#include "wavplay/playback/segment.h"
#include "wavplay/stream/prefetch_ring.h"

#include <atomic>
#include <chrono>
#include <cstddef>

namespace wavplay::playback {

// The callback side of one segment: copying published blocks into the driver's
// halves, counting start silence and the drain, and recording the first stop
// reason. The comment at the top of segment_renderer.cpp lists the code an ASIO
// callback can reach; none of it may allocate, lock, wait, perform I/O, log or
// touch the GUI. The control thread prepares the renderer before start. While
// writers are attached it only reads the switch count and the progress_frames_
// count; the rest of the audio state is inspected after every writer has detached.
//
// Layout (C4324: intentional padding). The first 128-byte pair holds the two
// lines private to the callback; a steady PCM switch touches both. The second
// pair holds a cold line and the line the control thread reads, so neither that
// line nor its adjacent-line partner is a private hot line (core/alignment.h).
// A steady PCM switch touches three renderer lines.
#pragma warning(push)
#pragma warning(disable : 4324)
class alignas(core::adjacent_line_pair) SegmentRenderer {
public:
    SegmentRenderer(stream::PrefetchRing& queue, PlaybackSignal::Token token, std::chrono::milliseconds start_silence,
        const SegmentControl& control) noexcept;

    [[nodiscard]] ASIOBufferInfo (&buffers() noexcept)[2] { return buffers_; }
    [[nodiscard]] PlaybackSignal::Token token() const noexcept { return token_; }
    // Control thread, before attach: width_ shares hot line 1 with state every switch writes.
    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t start_silence_blocks() const noexcept { return start_silence_blocks_; }
    // Control thread only, after detaching every callback writer: detach acquires
    // through the writer flags, so this reading includes the last block submitted.
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept
    {
        return progress_frames_.load(std::memory_order_relaxed);
    }
    // The same count, read live by the control thread while writers are attached.
    [[nodiscard]] const std::atomic<std::uint64_t>& progress_frames() const noexcept { return progress_frames_; }

    // Control thread, after the allocation is validated and before start.
    void set_output_latency(long frames) noexcept;
    // Control thread, before start. Silences A and fills B with the first block:
    // start silence when requested, else the first PCM block. Returns read_error,
    // with B silenced, when the producer has already failed.
    [[nodiscard]] stream::BlockStatus prefill();
    // Watchdog evidence: the switches that reached render. Only callbacks write
    // it; the control thread compares two readings. Change detection only:
    // renders of the two halves can overlap and lose an increment.
    [[nodiscard]] std::uint64_t switches() const noexcept { return switches_.load(std::memory_order_relaxed); }

    // Real-time: one bufferSwitch while this renderer is attached. `half` is the
    // buffer index (0 or 1) that the callback validated. `observed` is the signal
    // sampled at callback entry, already known to be of this phase.
    void render(std::size_t half, PlaybackSignal::Token observed) noexcept;
    void silence(std::size_t half) noexcept;

private:
    // Real-time steps of render, in its order.
    void note_switch() noexcept;
    [[nodiscard]] bool accept_sequence(std::size_t half) noexcept;
    [[nodiscard]] bool accept_stop() noexcept;
    [[nodiscard]] bool deliver(std::size_t half) noexcept;
    // Reached from deliver in real time and from prefill on the control thread before start.
    stream::BlockStatus pop(std::size_t half) noexcept;
    [[nodiscard]] std::size_t drain_latency() const noexcept;

    // Members are declared in initialization order: start_silence_blocks_ uses width_.

    // Hot line 0. The SDK packs ASIOBufferInfo to 4 bytes (see the static_assert in
    // asio_sdk.h); alignas keeps the buffer address loads 8-byte aligned.
    alignas(void*) ASIOBufferInfo buffers_[2]{{ASIOFalse, 0, {}}, {ASIOFalse, 1, {}}};
    // Cached while preparing the segment: callbacks never follow the pageable
    // control-thread PrefetchRing or SegmentControl wrapper objects.
    stream::PrefetchRing::Consumer queue_;
    const std::atomic<bool>& pause_requested_;
    // Hot line 1.
    const std::atomic<bool>& stop_requested_;
    const PlaybackSignal::Token token_;
    std::size_t next_half_{};
    std::size_t drain_remaining_{};
    std::size_t silent_switches_left_{}; // Lead-in still owed to callbacks after prefilled B.
    const std::size_t width_, channel_bytes_;
    bool hardware_started_{}, draining_pause_{}, final_in_prefill_{};

    // Cold line 2: written before start or only during pre-roll, read when the
    // drain starts and by the control thread.
    alignas(core::adjacent_line_pair) std::size_t latency_frames_{};
    std::size_t preroll_lead_{};
    const std::size_t start_silence_blocks_;
    // Shared line 3: the only one the control thread reads while writers are
    // attached. The renderer that pops is the only writer of progress_frames_
    // (valid PCM frames submitted): prefill before attach, then the callback that
    // holds the sequence. Callbacks are the only writers of switches_.
    alignas(core::cache_line_size) std::atomic<std::uint64_t> progress_frames_{};
    std::atomic<std::uint64_t> switches_{};
};
#pragma warning(pop)
// With the cold line starting a pair and the shared line starting a line, this
// bound keeps the hot lines in the first pair and both other lines in the second.
static_assert(alignof(SegmentRenderer) == core::adjacent_line_pair &&
    sizeof(SegmentRenderer) <= 4 * core::cache_line_size);

// The SDK's callbacks carry no context, so the process-wide callback table is
// routed to one renderer at a time. Control thread only.
namespace callback_routing {

[[nodiscard]] ASIOCallbacks* callbacks() noexcept;
// Publish before start: start may enter the first callback synchronously.
void attach(SegmentRenderer& renderer) noexcept;
// Stop routing and wait for any switch still copying into the renderer's halves.
// Returns false when no renderer was attached. Later switches write nothing.
bool detach() noexcept;
void set_streaming(bool streaming) noexcept;

} // namespace callback_routing
} // namespace wavplay::playback
