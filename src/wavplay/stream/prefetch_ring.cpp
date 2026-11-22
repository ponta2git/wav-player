#include "wavplay/stream/prefetch_ring.h"

#include "wavplay/core/block_copy.h"
#include "wavplay/core/memory.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/core/thread_hints.h"
#include "wavplay/stream/gapless_run.h"
#include "wavplay/stream/ring_layout.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wavplay::stream {
namespace {

// GaplessRun checks each sum before appending another track, keeping the run's
// frames (and therefore blocks) strictly below this reserved terminal cursor.
constexpr auto terminal = std::numeric_limits<std::uint64_t>::max();
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
// A default batch is one ReadFile into the reader's staging buffer.
static_assert(default_prefetch_policy.batch_source_bytes == media::WavReader::staging_bytes);

// Built in a register: a 64-bit load of two separately stored halves would stall
// on store forwarding behind the block copies. The bit_cast places the status in
// the low half, PoppedBlock's first member, only on a little-endian target.
static_assert(std::endian::native == std::endian::little);
[[nodiscard]] PoppedBlock make_block(BlockStatus status, std::uint64_t frames) noexcept
{
    return std::bit_cast<PoppedBlock>(frames << 32 | std::to_underlying(status));
}

// Reserve a ring's private metadata inside the 16 MiB budget. Round the
// remaining plane budget down to whole pages so its allocation stays within it.
[[nodiscard]] PrefetchPolicy plane_policy(std::size_t metadata_bytes, std::size_t producer_wakes_per_ring,
    RefillPattern pattern)
{
    auto policy = default_prefetch_policy;
    policy.producer_wakes_per_ring = producer_wakes_per_ring;
    policy.refill_pattern = pattern;
    if (metadata_bytes >= policy.ring_budget_bytes) {
        throw std::length_error("Prefetch metadata exhausts the ring budget");
    }
    const auto page = core::page_size();
    policy.ring_budget_bytes = (policy.ring_budget_bytes - metadata_bytes) / page * page;
    return policy;
}

} // namespace

// Intentional padding (C4324) starts the producer's and the consumer's cursor
// groups on 128-byte line pairs of their own (core::adjacent_line_pair), so
// they lie in different pairs whatever members precede them. Impl lives in its
// own ResidentObject pages, so the padding costs no shared memory; with MSVC x64
// sizeof(Impl) is 512 bytes, within one 4 KiB page.
#pragma warning(push)
#pragma warning(disable : 4324)
struct PrefetchRing::Impl {
    // Declared first: everything pop() reads besides the cursors and the samples.
    // Impl starts its own page, so these lie in its first 128-byte pair. The
    // producer writes layout and pages before the first publication; pop() reads
    // them, immutable, only after acquiring a publication.
    RingLayout layout{};
    core::PageAllocation pages;
    // Auto-reset: the consumer sets it at wake thresholds and stop() on shutdown,
    // while the producer waits for free slots or, parked after a streaming run's
    // last block, for stop(). A kernel event keeps the callback free of the
    // user-mode lock behind WaitOnAddress/WakeByAddress.
    const core::UniqueHandle space_available{core::create_event()};

    const audio::SampleFormat output;
    // The shape, known before the producer plans the layout: start() revalidates
    // it, produce() plans from it, and the producer's fill_slots and the control
    // accessors read it. layout.block holds the same value for pop().
    const BlockShape block;
    // Auto-reset: set by the producer at the first publication and when it exits,
    // so the constructing control thread can wait while dispatching its messages.
    const core::UniqueHandle primed{core::create_event()};

    media::WavInfo info;
    std::thread worker;
    // The producer's failure, stored once before `published` becomes terminal.
    std::exception_ptr read_error;

    // Producer group: blocks published so far, or terminal after a read failure.
    alignas(core::adjacent_line_pair) std::atomic<std::uint64_t> published{};

    // Consumer group: blocks consumed so far, or terminal after shutdown.
    alignas(core::adjacent_line_pair) std::atomic<std::uint64_t> consumed{};
    std::size_t read_offset{};
    std::uint64_t next_wake{};
    std::size_t wake_index{};
    std::array<std::size_t, 4> wake_intervals{};

    Impl(std::size_t frames, audio::SampleFormat format)
        : output(format), block(plan_block_shape(frames, output.bytes()))
    {
    }

    ~Impl() { stop(); }

    void start(PrepareTracks prepare, std::uint64_t first_frame, const PrefetchPolicy& policy,
        const core::WakeHook& hook)
    {
        // Revalidate even the largest block with metadata reserved before a worker can open a file.
        (void)plan_block_shape(block.frames, output.bytes(), policy);
        // The producer owns `prepare` and calls it once, before priming. What
        // it borrows from the caller stays valid: the constructor waits until
        // that call has returned or thrown.
        worker = std::thread(&Impl::run_producer, this, std::move(prepare), first_frame, policy);
        await_priming(hook);
        const auto written = published.load(std::memory_order_acquire);
        if (written == terminal) {
            std::rethrow_exception(read_error);
        }
        if (written == 0) {
            throw std::logic_error("Prefetch producer ended before priming");
        }
        if (written == layout.total_blocks) {
            // The whole run is buffered. Release its reader and thread handle
            // before playback; only a run with unread PCM needs a live worker,
            // which then stays until stop(). run_producer parks exactly when
            // this join is skipped.
            worker.join();
        }
    }

    void stop() noexcept
    {
        // A short run already joined its worker during priming. Its buffered
        // samples must become unavailable on stop just like a streaming run.
        consumed.store(terminal, std::memory_order_release);
        if (worker.joinable()) {
            SetEvent(space_available.get());
            worker.join();
        }
    }

    // Control thread. Storage can take a while to prime about two seconds of
    // audio; an STA that owns driver windows keeps dispatching messages meanwhile,
    // and runs the caller's poll on each wake by the hook's event. `primed` is
    // first, so it wins when both are set.
    void await_priming(const core::WakeHook& hook)
    {
        // The hook's event is watched only with a poll to run.
        const auto poll = hook.event ? hook.poll : nullptr;
        const HANDLE events[]{primed.get(), hook.event};
        const std::span<const HANDLE> watched(events, poll ? 2 : 1);
        for (;;) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            const auto [wake, index] = core::wait_for_events_until(watched, deadline);
            if (wake == core::WaitResult::signaled) {
                if (index == 0) {
                    return;
                }
                if (poll) { // Always set: index 1 is watched only with a poll.
                    poll(hook.context);
                }
            } else if (wake == core::WaitResult::quit || wake == core::WaitResult::failed) {
                // A pending WM_QUIT (left for the outer loop) or a failed message
                // wait: finish priming without dispatching or polling. Priming is
                // decided by the ring itself, not by one signal of the auto-reset
                // event: the producer publishes, records a failure, or exits.
                if (published.load(std::memory_order_acquire) != 0) {
                    return;
                }
                const HANDLE ends[]{primed.get(), worker.native_handle()};
                if (WaitForMultipleObjects(2, ends, FALSE, INFINITE) == WAIT_FAILED) {
                    core::throw_last_error("WaitForMultipleObjects(priming)");
                }
                return;
            }
        }
    }

    // ---- Producer thread -------------------------------------------------

    void run_producer(PrepareTracks prepare, std::uint64_t first_frame, const PrefetchPolicy policy) noexcept
    {
        core::name_current_thread(L"WAV Player reader");
        try {
            auto tracks = prepare();
            if (tracks.empty()) {
                throw std::invalid_argument("Playback requires a first track");
            }
            if (std::ranges::contains(tracks, nullptr)) {
                throw std::invalid_argument("Prepared playback tracks must not be null");
            }
            info = tracks.front()->info();
            if (first_frame >= info.frame_count) {
                throw std::out_of_range("Prefetch requires an unplayed WAV frame");
            }
            tracks.front()->seek_frame(first_frame);
            GaplessRun run(std::move(tracks.front()), output, block.frames, policy.batch_source_bytes);
            for (std::size_t index = 1; index < tracks.size(); ++index) {
                if (!run.append(std::move(tracks[index]))) {
                    throw std::logic_error("Unvalidated gapless track");
                }
            }
            tracks.clear();
            produce(run, policy);
            // Park a streaming run until stop(), keeping `run` and so its last
            // reader. While a segment streams, the producer creates no thread
            // and frees no pages itself (the ring lives from before priming to
            // after stop; reads stage on the stack), and its thread does not
            // exit (no stack/TEB release, DLL detach or final file close)
            // unless a read or this wait fails. Releasing a finished track
            // frees at most a small heap block. pop() stops setting the event once
            // it sees every block published, so a leftover signal, or one from
            // a pop that raced the final publication, only causes another
            // check. The condition must stay the exact complement of start()'s
            // join (the whole run buffered at priming): a fully buffered run
            // that parked would deadlock that join. A failed wait only ends
            // the thread early and never touches `published`.
            if (layout.total_blocks != layout.capacity) {
                while (consumed.load(std::memory_order_acquire) != terminal) {
                    if (WaitForSingleObject(space_available.get(), INFINITE) != WAIT_OBJECT_0) {
                        break;
                    }
                }
            }
        } catch (...) {
            read_error = std::current_exception();
            published.store(terminal, std::memory_order_release);
        }
        SetEvent(primed.get()); // Also after a failure or an early stop.
    }

    // The producer owns the whole run of slots in both planes until publication.
    // Samples are converted straight into the aligned slots; the gaps between
    // slots are never read.
    void fill_slots(GaplessRun& run, std::size_t slot, std::size_t blocks, bool last,
        media::WavReader::StagingBuffer staging)
    {
        const auto frames = blocks * block.frames;
        auto* left = pages.data() + slot * block.stride;
        auto* right = left + layout.channel_offset;
        const auto count = run.read({.left = left, .right = right, .block_frames = block.frames,
            .stride = block.stride, .frames = frames}, staging);
        if (count != (last ? frames - block.frames + layout.final_frames : frames)) {
            throw std::runtime_error("Unexpected end of playback data");
        }
        if (count < frames) {
            // Zero-pad the final block after its valid frames: once per segment,
            // with the callback's primitive for one uniform instruction path.
            const auto valid = layout.final_frames * output.bytes();
            const auto offset = (blocks - 1) * block.stride + valid;
            core::zero_block(left + offset, block.bytes - valid);
            core::zero_block(right + offset, block.bytes - valid);
        }
    }

    void produce(GaplessRun& run, const PrefetchPolicy& policy)
    {
        // One producer-owned scratch buffer for all fills, including track and
        // ring boundaries. Readers borrow it only until their synchronous read
        // returns. No input remains here once produce finishes and parks.
        alignas(core::cache_line_size) std::array<std::byte, media::WavReader::staging_bytes> staging;
        layout = plan_ring_layout(block, run.frame_count(), info.format.sample_rate, policy);
        wake_intervals = layout.wake_intervals(policy.refill_pattern);
        next_wake = wake_intervals.front();
        pages = core::PageAllocation(layout.allocation_bytes());
        const auto capacity = layout.capacity;
        const auto total_blocks = layout.total_blocks;
        std::size_t slot = 0;
        for (std::uint64_t written = 0; written < total_blocks;) {
            const auto read = consumed.load(std::memory_order_acquire);
            if (read == terminal) {
                return;
            }
            const auto buffered = written - read;
            if (buffered == capacity) {
                // A set left over from an earlier threshold only causes one more check.
                if (WaitForSingleObject(space_available.get(), INFINITE) == WAIT_FAILED) {
                    core::throw_last_error("WaitForSingleObject");
                }
                continue;
            }
            // After startup, bound time to publication by the audio still
            // queued. An empty ring gets its first completed block immediately.
            const auto batch = run.next_batch_blocks();
            const auto limit = read == 0 ? batch : std::min(batch, std::max<std::size_t>(1, buffered));
            const auto free_run = std::min(capacity - buffered, capacity - slot);
            const auto blocks = std::min(free_run, std::min(limit, total_blocks - written));
            fill_slots(run, slot, blocks, written + blocks == total_blocks, staging);
            written += blocks;
            slot += blocks;
            if (slot == capacity) {
                slot = 0;
            }
            // The first publication completes priming: all slots, including
            // final padding, have been filled and every page has been touched.
            if (written >= capacity) {
                published.store(written, std::memory_order_release);
                if (written == capacity) {
                    SetEvent(primed.get());
                    // Steady refills only: preparation, validation and priming
                    // ran at the thread's default. A hint; failure is ignored.
                    (void)core::set_current_thread_speed(reader_refill_speed);
                }
            }
        }
    }

    // ---- Real-time consumer ----------------------------------------------

    // Forced inline into pop_block, its only caller: the inlined block copies
    // make it too large for MSVC to inline by itself, and as an out-of-line
    // member function it would return PoppedBlock through memory, not in RAX.
    [[msvc::forceinline]] PoppedBlock pop(void* left, void* right) noexcept
    {
        const auto written = published.load(std::memory_order_acquire);
        if (written == terminal) {
            return make_block(BlockStatus::read_error, 0);
        }
        const auto read = consumed.load(std::memory_order_relaxed);
        if (read >= written) { // Also after stop(), when consumed is terminal.
            return make_block(BlockStatus::empty, 0);
        }
        // Geometry from layout (the first lines of Impl), not from Impl::block.
        const auto& shape = layout.block;
        const auto* data = pages.data() + read_offset;
        core::copy_stereo_blocks(left, right, data, data + layout.channel_offset, shape.bytes);
        const auto next_offset = read_offset + shape.stride;
        read_offset = next_offset == layout.plane_bytes ? 0 : next_offset;
        const auto next = read + 1;
        consumed.store(next, std::memory_order_release);
        if (next == next_wake && written < layout.total_blocks) {
            SetEvent(space_available.get());
            wake_index = (wake_index + 1) % wake_intervals.size();
            next_wake += wake_intervals[wake_index];
        }
        const bool last = next == layout.total_blocks;
        return last ? make_block(BlockStatus::last, layout.final_frames) : make_block(BlockStatus::ready, shape.frames);
    }
};
#pragma warning(pop)

PrefetchRing::PrefetchRing(PrepareTracks prepare, std::size_t block_frames,
    audio::SampleFormat output, std::uint64_t first_frame, core::WakeHook hook, std::size_t producer_wakes_per_ring,
    RefillPattern pattern)
    : impl_(block_frames, output)
{
    if (!prepare) {
        throw std::invalid_argument("Playback requires a track source");
    }
    const auto policy = plane_policy(impl_.committed_size(), producer_wakes_per_ring, pattern);
    impl_->start(std::move(prepare), first_frame, policy, hook);
}

PrefetchRing::~PrefetchRing() = default;

const media::WavInfo& PrefetchRing::info() const noexcept
{
    return impl_->info;
}

std::size_t PrefetchRing::block_frames() const noexcept
{
    return impl_->block.frames;
}

audio::SampleFormat PrefetchRing::output_format() const noexcept
{
    return impl_->output;
}

std::size_t PrefetchRing::block_bytes() const noexcept
{
    return impl_->block.bytes;
}

bool PrefetchRing::pages_locked() const noexcept
{
    return impl_->pages.locked(); // Allocated before the first publication.
}

bool PrefetchRing::read_failed() const noexcept
{
    return consumer().read_failed();
}

bool PrefetchRing::Consumer::read_failed() const noexcept
{
    return ring_->published.load(std::memory_order_acquire) == terminal;
}

std::exception_ptr PrefetchRing::read_error() const noexcept
{
    // Written once, before the release store of terminal; stop() has joined.
    return impl_->read_error;
}

std::size_t PrefetchRing::committed_size() const noexcept
{
    return impl_.committed_size() + impl_->pages.committed_size();
}

void PrefetchRing::stop() noexcept
{
    impl_->stop();
}

PoppedBlock PrefetchRing::pop_block(Impl* ring, void* left, void* right) noexcept
{
    return ring->pop(left, right);
}

} // namespace wavplay::stream
