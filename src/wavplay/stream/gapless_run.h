#pragma once

#include "wavplay/audio/planar_conversion.h"
#include "wavplay/media/wav_reader.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace wavplay::stream {

// No track (std::monostate, or a null path), a borrowed path, or an opened
// reader whose validated handle is transferred. A borrowed path stays valid
// until the call that receives the input returns: run_request keys its shared
// handles by these paths for the whole request, not only until each opens.
using TrackInput = std::variant<std::monostate, const wchar_t*, media::WavReader,
    std::shared_ptr<media::WavReader>>;

// True for std::monostate or a null path.
[[nodiscard]] bool is_no_track(const TrackInput& input) noexcept;

// Shared sources are already validated and are used sequentially by one producer.
// Repeated playlist entries can therefore keep one protected file handle.
// Exceptions separate API misuse from media that cannot play:
// - std::invalid_argument: no track (std::monostate or a null path), a null or
//   unplayable shared reader, or a reader that is no longer at its start.
// - std::runtime_error: a path that does not open as a local disk file or cannot
//   be read (often std::system_error; an empty string is such a path), a
//   malformed or unsupported WAV, or one without frames or with over two channels.
// Callers that turn unplayable media into a track refusal (the playback
// request's run preparation, TrackPlayer::play's selection) deliberately catch
// only std::runtime_error, so misuse and allocation failure still propagate.
[[nodiscard]] std::shared_ptr<media::WavReader> retain_playable_track(TrackInput input);

// Producer-only source for one uninterrupted sample rate and output format.
// Following tracks at the same rate are read back to back, so a track boundary
// inside a block is filled from the next track without inserting silence.
class GaplessRun {
public:
    // The first reader has passed retain_playable_track and is positioned at
    // the run's first frame. Throws std::invalid_argument for a zero block width
    // or source budget, or when the source cannot be converted to `output` losslessly.
    GaplessRun(std::shared_ptr<media::WavReader> first, const audio::SampleFormat& output,
        std::size_t block_frames, std::size_t batch_source_bytes);

    // Mono plays the same samples on both outputs (the converters duplicate it).
    [[nodiscard]] static bool playable(const media::WavInfo& info) noexcept
    {
        return (info.format.channels == 1 || info.format.channels == 2) && info.frame_count != 0;
    }

    // Append an already validated source. False means another sample rate or no
    // lossless conversion to this run's output.
    [[nodiscard]] bool append(std::shared_ptr<media::WavReader> reader);

    // Size the ring once, after the last append and before reading.
    [[nodiscard]] std::uint64_t frame_count() const noexcept
    {
        return frames_;
    }

    // Preferred whole blocks for the current source's byte width. Stop at the
    // block containing its end, so the next fill uses the next track's width.
    // A block wider than the source budget still needs one complete block;
    // read() splits its I/O into staging-sized requests. Zero means run EOF.
    [[nodiscard]] std::size_t next_batch_blocks() const noexcept;

    // Converts the next frames into `target`, crossing into the next track
    // mid-block. A count below target.frames marks the end of the run. Once its
    // last frame is read, a track other than the run's last is released at once,
    // also at an exact block end, so its file closes promptly where this run
    // holds the last reference. The last track lives as long as the run.
    [[nodiscard]] std::size_t read(const audio::PlanarBlocks& target, media::WavReader::StagingBuffer staging);

private:
    struct Track {
        std::shared_ptr<media::WavReader> reader;
        audio::PlanarConverter converter;
        std::size_t batch_blocks;
    };

    [[nodiscard]] static Track make_track(std::shared_ptr<media::WavReader> reader,
        const audio::SampleFormat& output, std::size_t block_frames, std::size_t batch_source_bytes);
    void advance_track();

    audio::SampleFormat output_;
    std::size_t block_frames_;
    std::size_t batch_source_bytes_;
    Track current_;
    std::vector<Track> next_;
    std::size_t next_index_{};
    std::uint64_t frames_{};
};

} // namespace wavplay::stream
