#include "wavplay/stream/gapless_run.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wavplay::stream {
namespace {

std::size_t source_batch_blocks(const audio::PlanarConverter& converter,
    std::size_t block_frames, std::size_t batch_source_bytes)
{
    if (block_frames == 0 || batch_source_bytes == 0) {
        throw std::invalid_argument("Gapless run requires a block width and source batch budget");
    }
    return std::max<std::size_t>(1, batch_source_bytes / converter.source_frame_bytes() / block_frames);
}

} // namespace

bool is_no_track(const TrackInput& input) noexcept
{
    const auto* path = std::get_if<const wchar_t*>(&input);
    return std::holds_alternative<std::monostate>(input) || (path && !*path);
}

std::shared_ptr<media::WavReader> retain_playable_track(TrackInput input)
{
    if (is_no_track(input)) {
        throw std::invalid_argument("Playback requires a first track");
    }
    if (auto* retained = std::get_if<std::shared_ptr<media::WavReader>>(&input)) {
        if (!*retained || !GaplessRun::playable((*retained)->info())) {
            throw std::invalid_argument("Invalid retained WAV");
        }
        return std::move(*retained);
    }
    auto reader = std::holds_alternative<media::WavReader>(input)
        ? std::get<media::WavReader>(std::move(input))
        : media::WavReader(std::get<const wchar_t*>(input));
    if (!reader.at_start()) {
        throw std::invalid_argument("Playback requires an unread WAV");
    }
    if (!GaplessRun::playable(reader.info())) {
        throw std::runtime_error("Playback requires a nonempty mono or stereo WAV");
    }
    return std::make_shared<media::WavReader>(std::move(reader));
}

GaplessRun::GaplessRun(std::shared_ptr<media::WavReader> first, const audio::SampleFormat& output,
    std::size_t block_frames, std::size_t batch_source_bytes)
    : output_(output), block_frames_(block_frames), batch_source_bytes_(batch_source_bytes),
      current_(make_track(std::move(first), output, block_frames, batch_source_bytes)),
      frames_(current_.reader->remaining_frames())
{
}

GaplessRun::Track GaplessRun::make_track(std::shared_ptr<media::WavReader> reader,
    const audio::SampleFormat& output, std::size_t block_frames, std::size_t batch_source_bytes)
{
    const auto converter = audio::find_planar_converter(reader->info().format, output);
    if (!converter) {
        throw std::invalid_argument("Unsupported PCM output format");
    }
    const auto blocks = source_batch_blocks(*converter, block_frames, batch_source_bytes);
    return Track{std::move(reader), *converter, blocks};
}

bool GaplessRun::append(std::shared_ptr<media::WavReader> reader)
{
    const auto converter = audio::find_planar_converter(reader->info().format, output_);
    if (!converter || reader->info().format.sample_rate != current_.reader->info().format.sample_rate) {
        return false;
    }
    const auto frames = reader->info().frame_count;
    if (frames >= std::numeric_limits<std::uint64_t>::max() - frames_) {
        throw std::length_error("Playlist frame count is too large");
    }
    const auto blocks = source_batch_blocks(*converter, block_frames_, batch_source_bytes_);
    next_.push_back(Track{std::move(reader), *converter, blocks});
    frames_ += frames; // Commit the total only after the track was retained successfully.
    return true;
}

std::size_t GaplessRun::next_batch_blocks() const noexcept
{
    const auto remaining = current_.reader->remaining_frames();
    const auto preferred = current_.batch_blocks;
    // The product cannot overflow: preferred is either one or rounded down
    // from batch_source_bytes / source_frame_bytes / block_frames.
    if (remaining >= preferred * block_frames_) {
        return preferred;
    }
    // Only a track's final fill needs division. Complete the boundary block
    // from subsequent tracks, then reconsider their source format next time.
    return remaining == 0 ? 0 : static_cast<std::size_t>(1 + (remaining - 1) / block_frames_);
}

// Keep boundary ownership and metadata updates out of the steady read path.
// noinline also keeps their wider compiler-generated copies local to this helper.
__declspec(noinline) void GaplessRun::advance_track()
{
    current_ = std::move(next_[next_index_++]);
    current_.reader->seek_frame(0);
}

std::size_t GaplessRun::read(const audio::PlanarBlocks& target, media::WavReader::StagingBuffer staging)
{
    std::size_t count{};
    for (;;) {
        if (count < target.frames) {
            count += current_.reader->read_planar(current_.converter, target.advanced(count), staging);
        }
        if (!current_.reader->at_end() || next_index_ == next_.size()) {
            return count;
        }
        advance_track();
        if (count == target.frames) {
            return count;
        }
    }
}

} // namespace wavplay::stream
