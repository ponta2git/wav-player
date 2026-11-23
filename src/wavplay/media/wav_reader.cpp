#include "wavplay/media/wav_reader.h"

#include "wavplay/audio/planar_conversion.h"
#include "wavplay/core/alignment.h"
#include "wavplay/core/byte_order.h"

#include <intrin.h>
#include <smmintrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace wavplay::media {
namespace {

// Only the final read of a scan can end inside a vector.
static_assert(WavReader::validation_bytes % sizeof(__m128i) == 0);
// A staging buffer holds at least one frame of the widest input (stereo, four
// bytes a sample), so every staged read makes progress.
static_assert(WavReader::staging_bytes >= 2 * 4);

void validate_unused_bits(ReadOnlyFile& file, const WavInfo& info)
{
    if (info.format.sample != audio::int24_in32_le || info.frame_count == 0) {
        return;
    }
    // Validate the complete protected file before it can join a playback run;
    // reopening reuses this result. The scan runs at selection, list load or
    // before priming, never while a segment streams, so unlike playback staging
    // it reads up to validation_bytes per ReadFile. One bounded heap buffer per
    // scan keeps that size off the thread stack.
    auto remaining = info.frame_count * info.format.frame_bytes();
    const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, WavReader::validation_bytes));
    const auto staging = std::make_unique_for_overwrite<std::byte[]>(capacity);
    auto offset = info.data_offset;
    while (remaining != 0) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, capacity));
        file.read_exact_at(offset, std::span(staging.get(), count));
        // Four 32-bit containers share one integer load. OR all lanes before
        // testing just their unused low bytes; valid high bits are irrelevant.
        // Complete vectors only: the final 0..3 samples stay inside this read.
        auto unused_words = _mm_setzero_si128();
        std::size_t byte = 0;
        for (; byte + sizeof(__m128i) <= count; byte += sizeof(__m128i)) {
            unused_words = _mm_or_si128(unused_words, core::load_le<__m128i>(staging.get() + byte));
        }
        unsigned unused_tail{};
        for (; byte < count; byte += 4) {
            unused_tail |= std::to_integer<unsigned>(staging[byte]);
        }
        if (_mm_testz_si128(unused_words, _mm_set1_epi32(0xff)) == 0 || unused_tail != 0) {
            throw std::runtime_error("Extensible PCM24/32 has nonzero unused sample bits");
        }
        offset += count;
        remaining -= count;
    }
}

// Keep validation in the read path: checked products otherwise make LTCG
// outline this helper and add a call plus register saves to every refill.
[[msvc::forceinline]] void check_planar_blocks(const audio::PlanarBlocks& target, std::size_t sample_bytes)
{
    if (target.frames == 0) {
        return;
    }
    unsigned __int64 block_bytes_high;
    const auto block_bytes = _umul128(target.block_frames, sample_bytes, &block_bytes_high);
    if (!target.left || !target.right || target.block_frames == 0 ||
        block_bytes_high != 0 || block_bytes > target.stride) {
        throw std::invalid_argument("Planar blocks must hold whole samples");
    }
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    if (target.frames > max_size - target.first_frame) {
        throw std::invalid_argument("Planar frame range overflows");
    }
    const auto last = target.first_frame + target.frames - 1;
    const auto last_block = last / target.block_frames;
    const auto last_bytes = (last % target.block_frames + 1) * sample_bytes;
    unsigned __int64 block_offset_high;
    const auto block_offset = _umul128(last_block, target.stride, &block_offset_high);
    if (block_offset_high != 0 || block_offset > max_size - last_bytes) {
        throw std::invalid_argument("Planar byte range overflows");
    }
    const auto extent = block_offset + last_bytes;
    // Check address arithmetic as integers before forming any end pointers. An
    // invalid raw layout must be rejected before pointer arithmetic or file I/O.
    const auto left = reinterpret_cast<std::uintptr_t>(target.left);
    const auto right = reinterpret_cast<std::uintptr_t>(target.right);
    constexpr auto max_address = std::numeric_limits<std::uintptr_t>::max();
    if (extent > max_address - left || extent > max_address - right) {
        throw std::invalid_argument("Planar address range overflows");
    }
    if (left < right + extent && right < left + extent) {
        // Strided channel hulls may overlap while their actual block payloads
        // remain disjoint (for example L32|R32 in one 64-byte slot). A full
        // block of either channel must fit between every pair of the other's
        // block starts, including a neighbouring slot. For interleaved hulls,
        // require that spacing even when this read starts or ends mid-block.
        // Disjoint hulls above retain the exact-range partial-read contract.
        const auto distance = left < right ? right - left : left - right;
        const auto phase = distance % target.stride;
        if (phase < block_bytes || target.stride - phase < block_bytes) {
            throw std::invalid_argument("Channel blocks overlap");
        }
    }
}

} // namespace

WavReader::WavReader(const wchar_t* path)
    : file_(path), info_(parse_wav_header(file_)), remaining_frames_(info_.frame_count)
{
    validate_unused_bits(file_, info_);
}

WavReader::WavReader(ReadOnlyFile file, const WavInfo& info)
    : file_(std::move(file)), info_(info), remaining_frames_(info_.frame_count)
{
}

WavReader WavReader::reopen() const
{
    if (!file_.is_open()) {
        throw std::logic_error("WAV reader is closed");
    }
    return WavReader(file_.reopen(), info_);
}

void WavReader::seek_frame(std::uint64_t frame)
{
    if (!file_.is_open()) {
        throw std::logic_error("WAV reader is closed");
    }
    if (frame > info_.frame_count) {
        throw std::out_of_range("WAV frame exceeds the data range");
    }
    // The cursor is only this count; each read derives its file offset from it.
    remaining_frames_ = info_.frame_count - frame;
}

std::size_t WavReader::read_planar(const audio::SampleFormat& output, std::span<std::byte> left,
    std::span<std::byte> right)
{
    const auto converter = audio::find_planar_converter(info_.format, output);
    if (!converter) {
        throw std::invalid_argument("Unsupported PCM output format");
    }
    return read_planar(*converter, left, right);
}

void WavReader::check_source(const audio::PlanarConverter& converter) const
{
    if (!file_.is_open()) {
        throw std::logic_error("WAV reader is closed");
    }
    // Only find_planar_converter creates converters, so the kernel and sizes
    // agree with the formats; the source must still be this WAV's.
    if (converter.source() != info_.format) {
        throw std::invalid_argument("PCM converter was chosen for another WAV format");
    }
}

std::size_t WavReader::read_planar(const audio::PlanarConverter& converter, std::span<std::byte> left,
    std::span<std::byte> right)
{
    check_source(converter);
    const auto sample_bytes = converter.output_sample_bytes();
    if (left.size() != right.size() || left.size() % sample_bytes != 0) {
        throw std::invalid_argument("Channel buffers must have equal sizes and hold whole samples");
    }
    const auto frames = left.size() / sample_bytes;
    return read_checked_planar(converter, audio::PlanarBlocks{.left = left.data(), .right = right.data(),
        .block_frames = frames, .stride = left.size(), .frames = frames});
}

std::size_t WavReader::read_planar(const audio::PlanarConverter& converter, const audio::PlanarBlocks& target)
{
    check_source(converter);
    return read_checked_planar(converter, target);
}

std::size_t WavReader::read_checked_planar(const audio::PlanarConverter& converter, const audio::PlanarBlocks& target)
{
    alignas(core::cache_line_size) std::array<std::byte, staging_bytes> staging;
    return read_checked_planar(converter, target, staging);
}

std::size_t WavReader::read_planar(const audio::PlanarConverter& converter,
    const audio::PlanarBlocks& target, StagingBuffer staging)
{
    check_source(converter);
    return read_checked_planar(converter, target, staging);
}

std::size_t WavReader::read_checked_planar(const audio::PlanarConverter& converter,
    const audio::PlanarBlocks& target, StagingBuffer staging)
{
    check_planar_blocks(target, converter.output_sample_bytes());
    if (target.frames == 0) {
        return 0;
    }
    const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(target.frames, remaining_frames_));
    if (frames == 0) {
        return 0;
    }
    const auto frame_bytes = converter.source_frame_bytes();
    const auto staging_frames = staging.size() / frame_bytes;
    // The validated RIFF/RF64 data range bounds these offsets. The cursor
    // advances only after the whole request, so each batch adds its own progress.
    const auto base = info_.data_offset + (info_.frame_count - remaining_frames_) * frame_bytes;
    auto rest = target;
    try {
        for (std::size_t done = 0; done < frames;) {
            const auto count = std::min(staging_frames, frames - done);
            file_.read_exact_at(base + std::uint64_t{done} * frame_bytes, staging.first(count * frame_bytes));
            audio::convert_into(converter, staging.data(), count, rest);
            done += count;
        }
    } catch (...) {
        // Earlier batches may already be in the target. I/O failures are terminal
        // by contract (see the class comment): close, so callers discard the
        // output and reopen the file instead of retrying this reader.
        file_.close();
        throw;
    }
    remaining_frames_ -= frames;
    return frames;
}

} // namespace wavplay::media
