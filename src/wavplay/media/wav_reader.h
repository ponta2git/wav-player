#pragma once

#include "wavplay/media/read_only_file.h"
#include "wavplay/media/wav_header.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wavplay::audio {
struct PlanarBlocks;
class PlanarConverter;
}

namespace wavplay::media {

// One validated WAV and a frame cursor over its PCM data. The cursor is the count
// of remaining frames; every read derives its file offset from it, so nothing
// depends on the handle's file pointer. The open handle denies writes and
// deletion, so the validated layout stays true for the reader's lifetime.
// Construction scans all Extensible PCM24/32 samples for zero unused low bits
// before any output; reopening reuses that validation without scanning again.
// After an I/O failure the reader is closed: discard any output and open the file
// again before retrying.
class WavReader {
public:
    // Interleaved PCM that one ReadFile request moves into the cache-line-aligned
    // staging buffer before conversion: the unit to retune for storage throughput.
    static constexpr std::size_t staging_bytes = 8 * 1024;
    using StagingBuffer = std::span<std::byte, staging_bytes>;
    // The largest ReadFile request of the Extensible PCM24/32 unused-bit scan,
    // which never overlaps streaming. A multiple of the 16-byte scan vector.
    static constexpr std::size_t validation_bytes = 64 * 1024;

    explicit WavReader(const wchar_t* path);

    [[nodiscard]] const WavInfo& info() const noexcept { return info_; }
    [[nodiscard]] bool at_start() const noexcept { return file_.is_open() && remaining_frames_ == info_.frame_count; }
    [[nodiscard]] bool at_end() const noexcept { return remaining_frames_ == 0; }
    [[nodiscard]] std::uint64_t remaining_frames() const noexcept { return remaining_frames_; }

    // Open the same protected file with an independent cursor at its first frame,
    // reusing the validated metadata. This reader's cursor is unchanged.
    // Closed or failed readers cannot reopen.
    [[nodiscard]] WavReader reopen() const;
    // Move the cursor to a frame boundary, including EOF, without file I/O.
    // Invalid positions leave the reader unchanged.
    void seek_frame(std::uint64_t frame);

    // Convert frames into two equally sized, non-overlapping planar buffers in the
    // output format. Buffers may be unaligned but must hold whole samples. Returns
    // the frames written to each; a short count marks the end of data. Throws
    // std::invalid_argument when no lossless converter exists for the output.
    [[nodiscard]] std::size_t read_planar(const audio::SampleFormat& output, std::span<std::byte> left,
        std::span<std::byte> right);
    // The same with a converter already chosen for this WAV's format, as a
    // producer reads every batch. Throws std::invalid_argument when the
    // converter was chosen for another format.
    [[nodiscard]] std::size_t read_planar(const audio::PlanarConverter& converter, std::span<std::byte> left,
        std::span<std::byte> right);
    // The same into the next frames of blocked planes, as the producer fills
    // aligned ring slots in place. Returns at most target.frames. Throws
    // std::invalid_argument for null nonempty planes, overflowing dimensions,
    // blocks that cannot hold whole samples or planes that overlap. The caller
    // provides enough storage for the entire requested layout.
    [[nodiscard]] std::size_t read_planar(const audio::PlanarConverter& converter, const audio::PlanarBlocks& target);
    // Producer-owned scratch reused for one run. It must be disjoint from the
    // destinations and remain valid for this synchronous call; it is not kept.
    // The fixed extent preserves the same ReadFile limit as the owning entry.
    [[nodiscard]] std::size_t read_planar(const audio::PlanarConverter& converter,
        const audio::PlanarBlocks& target, StagingBuffer staging);

private:
    WavReader(ReadOnlyFile file, const WavInfo& info);
    void check_source(const audio::PlanarConverter& converter) const;
    [[nodiscard]] std::size_t read_checked_planar(const audio::PlanarConverter& converter,
        const audio::PlanarBlocks& target);
    [[nodiscard]] std::size_t read_checked_planar(const audio::PlanarConverter& converter,
        const audio::PlanarBlocks& target, StagingBuffer staging);

    ReadOnlyFile file_;
    WavInfo info_;
    std::uint64_t remaining_frames_;
};

} // namespace wavplay::media
