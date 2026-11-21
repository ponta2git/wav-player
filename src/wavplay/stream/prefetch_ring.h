#pragma once

#include "wavplay/audio/sample_format.h"
#include "wavplay/core/memory.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/stream/prefetch_policy.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace wavplay::stream {

enum class BlockStatus : std::uint32_t { ready, last, empty, read_error };

struct PoppedBlock {
    BlockStatus status;
    // Bounded by the 16 MiB ring budget. Both fields travel in one x64 register.
    std::uint32_t valid_frames;
};
static_assert(sizeof(PoppedBlock) == 8);

// A fixed single-producer, single-consumer ring of completed planar stereo blocks
// for one gapless run. A dedicated producer thread runs the caller's preparation
// of the run (which may open and validate files), converts samples to the
// output format and fills the ring; the real-time consumer only copies
// published blocks out. Geometry comes from plan_ring_layout and memory
// from core::PageAllocation, so tuning either does not touch the consumer.
// A run that fits the ring is joined during construction. A longer run keeps its
// producer thread, parked after the last block with its last track open, until
// stop(): the thread never ends while the run streams, unless a read or that
// wait fails.
class PrefetchRing {
    struct Impl;

public:
    // A borrowed real-time view of the resident state. Copy it into a resident
    // callback owner before starting, so pop/read_failed do not dereference the
    // outer PrefetchRing (which can live on a pageable control-thread stack).
    // All copies share one consumer cursor; use only one consumer at a time and
    // retire every copy before destroying the owning ring.
    class Consumer {
    public:
        // Real-time. Each destination holds block_bytes() bytes. Only ready and
        // last write either destination. No allocation, lock, wait or I/O.
        [[nodiscard]] PoppedBlock pop(void* left, void* right) noexcept { return pop_block(ring_, left, right); }
        [[nodiscard]] bool read_failed() const noexcept;

    private:
        friend class PrefetchRing;
        explicit Consumer(Impl* ring) noexcept : ring_(ring) {}
        Impl* ring_;
    };

    using PreparedTracks = std::vector<std::shared_ptr<media::WavReader>>;
    using PrepareTracks = std::function<PreparedTracks()>;
    // Prepare the whole same-rate run on this ring's existing producer, before
    // priming: playable sources (retain_playable_track) of one sample rate, each
    // with a lossless converter to `output`. An empty run or any null source
    // is rejected. Repeated entries may share a protected handle.
    // Preparation may publish control-side metadata; it is complete when
    // construction returns. The first track starts at `first_frame`. `output`
    // is the negotiated device format; there is no default, so a ring cannot
    // silently assume one.
    // Construction returns after priming: preparation, validation and initial
    // PCM read failures are rethrown here. On a COM STA the wait dispatches the
    // thread's messages (core::wait_for_events_until) and, after each wake by
    // `hook`'s event, runs its poll; priming wins when both are set. The poll
    // runs while the producer prepares and fills, so it must not touch
    // anything `prepare` uses. The positive number of producer wakes per ring
    // determines the notification interval once, before playback starts.
    PrefetchRing(PrepareTracks prepare, std::size_t block_frames,
        audio::SampleFormat output, std::uint64_t first_frame = 0, core::WakeHook hook = {},
        std::size_t producer_wakes_per_ring = default_prefetch_policy.producer_wakes_per_ring);
    ~PrefetchRing();
    PrefetchRing(const PrefetchRing&) = delete;
    PrefetchRing& operator=(const PrefetchRing&) = delete;

    // Metadata describes the first WAV. Block width is fixed for the ring's lifetime.
    [[nodiscard]] const media::WavInfo& info() const noexcept;
    [[nodiscard]] std::size_t block_frames() const noexcept;
    [[nodiscard]] audio::SampleFormat output_format() const noexcept;
    [[nodiscard]] std::size_t block_bytes() const noexcept;
    // Lock status of sample planes and private metadata pages respectively.
    [[nodiscard]] bool pages_locked() const noexcept;
    [[nodiscard]] bool metadata_pages_locked() const noexcept { return impl_.locked(); }
    // Actual page-rounded sample and metadata storage, together at most 16 MiB.
    [[nodiscard]] std::size_t committed_size() const noexcept;
    // Non-consuming failure observation, including while submitted audio drains.
    [[nodiscard]] bool read_failed() const noexcept;
    // Control thread only, after stop(): the exception that ended the producer
    // (for example ReadFile's std::system_error) when read_failed() is true,
    // otherwise null. A failure before priming is rethrown by the constructor.
    [[nodiscard]] std::exception_ptr read_error() const noexcept;
    // Control thread only, after all pop callers stop. Join the producer before
    // inspecting its final failure state. The ring cannot be consumed again.
    void stop() noexcept;
    // The only way to consume the ring. Stop callbacks before destruction.
    [[nodiscard]] Consumer consumer() const noexcept { return Consumer(impl_.get()); }

private:
    // MSVC x64 returns small aggregates in RAX from static functions. An
    // out-of-line instance method would introduce a hidden result buffer here.
    static PoppedBlock pop_block(Impl* ring, void* left, void* right) noexcept;
    core::ResidentObject<Impl> impl_;
};

} // namespace wavplay::stream
