#include "wavplay/media/wav_header.h"

#include "wavplay/core/byte_order.h"
#include "wavplay/media/read_only_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace wavplay::media {
namespace {

using core::load_le;

constexpr std::uint16_t wave_format_pcm = 1;
constexpr std::uint16_t wave_format_ieee_float = 3;
constexpr std::uint16_t wave_format_extensible = 0xfffe;
constexpr std::array<std::uint8_t, 16> pcm_subformat{
    1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
constexpr std::array<std::uint8_t, 16> float_subformat{
    3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
// WAVEFORMATEXTENSIBLE (40 bytes) is the largest header the parser inspects.
constexpr std::size_t max_fmt_bytes = 40;
constexpr std::uint32_t size_placeholder = 0xffffffffU;
using ChunkHeader = std::array<std::uint8_t, 8>;

bool chunk_is(const ChunkHeader& chunk, const char* id) noexcept
{
    return std::memcmp(chunk.data(), id, 4) == 0;
}

ChunkHeader read_chunk_header(ReadOnlyFile& file, std::uint64_t offset, std::uint64_t end)
{
    ChunkHeader chunk;
    if (offset > end || end - offset < chunk.size()) {
        throw std::runtime_error("Truncated WAV chunk header");
    }
    file.read_exact_at(offset, std::span(chunk));
    return chunk;
}

std::uint64_t chunk_end(std::uint64_t payload, std::uint64_t size, std::uint64_t end)
{
    // Bound the payload and odd-byte pad before either addition. This also
    // rejects a ds64 length near UINT64_MAX without wrapping a file offset.
    if (payload > end || size > end - payload || ((size & 1U) != 0 && size == end - payload)) {
        throw std::runtime_error("WAV chunk exceeds container boundary");
    }
    return payload + size + (size & 1U);
}

struct Rf64Sizes {
    std::uint64_t riff_size{};
    std::uint64_t data_size{};
    std::uint64_t sample_count{};
    std::uint64_t table_offset{};
    std::uint32_t table_length{};

    struct TableCursor {
        std::uint32_t id;
        std::uint32_t next_index;
    };
    // Only IDs actually requested by sentinel chunks allocate cursor state.
    // An arbitrarily large unused table costs neither a heap buffer nor reads.
    std::vector<TableCursor> cursors;

    std::uint64_t table_size(ReadOnlyFile& file, const ChunkHeader& chunk)
    {
        const auto id = load_le<std::uint32_t>(chunk.data());
        auto cursor = std::find_if(cursors.begin(), cursors.end(), [id](const auto& entry) { return entry.id == id; });
        if (cursor == cursors.end()) {
            cursors.push_back({id, 0});
            cursor = cursors.end() - 1;
        }
        // Table records are 4-byte IDs followed by 64-bit little-endian sizes.
        // Keep reads bounded and preserve occurrence order for repeated IDs.
        constexpr std::uint32_t entries_per_read = 8192 / 12;
        std::array<std::uint8_t, entries_per_read * 12> entries;
        for (auto index = cursor->next_index; index < table_length;) {
            const auto count = std::min(entries_per_read, table_length - index);
            const auto bytes = static_cast<std::size_t>(count) * 12;
            // read_rf64_sizes bounded the whole table inside ds64.
            file.read_exact_at(table_offset + std::uint64_t{index} * 12, std::span(entries).first(bytes));
            for (std::uint32_t item = 0; item < count; ++item) {
                const auto* entry = entries.data() + static_cast<std::size_t>(item) * 12;
                if (load_le<std::uint32_t>(entry) == id) {
                    cursor->next_index = index + item + 1;
                    return load_le<std::uint64_t>(entry + 4);
                }
            }
            index += count;
        }
        throw std::runtime_error("Missing RF64 ds64 table entry for chunk");
    }
};

// Reads the ds64 chunk that starts at next_chunk and advances next_chunk past it.
Rf64Sizes read_rf64_sizes(ReadOnlyFile& file, std::uint64_t file_size, std::uint64_t& next_chunk)
{
    const auto chunk = read_chunk_header(file, next_chunk, file_size);
    const auto payload = next_chunk + chunk.size();
    const auto size = load_le<std::uint32_t>(&chunk[4]);
    if (!chunk_is(chunk, "ds64") || size == size_placeholder) {
        throw std::runtime_error("RF64 must start with a ds64 chunk of known size");
    }
    next_chunk = chunk_end(payload, size, file_size);
    std::array<std::uint8_t, 28> sizes;
    if (size < sizes.size()) {
        throw std::runtime_error("Truncated RF64 ds64 chunk");
    }
    file.read_exact_at(payload, std::span(sizes));
    Rf64Sizes result;
    result.riff_size = load_le<std::uint64_t>(sizes.data());
    result.data_size = load_le<std::uint64_t>(&sizes[8]);
    result.sample_count = load_le<std::uint64_t>(&sizes[16]);
    result.table_offset = payload + sizes.size();
    result.table_length = load_le<std::uint32_t>(&sizes[24]);
    if (result.table_length > (size - sizes.size()) / 12) {
        throw std::runtime_error("RF64 ds64 table exceeds its chunk");
    }
    // Additional reserved bytes and the pad belong to ds64 but are not table
    // entries. The common walker will skip them with the rest of this payload.
    return result;
}

// The caller has bounded the payload [offset, offset + size) inside the file.
audio::StreamFormat read_format(ReadOnlyFile& file, std::uint64_t offset, std::uint64_t size)
{
    // Every format starts with the 16 bytes of PCMWAVEFORMAT.
    if (size < 16) {
        throw std::runtime_error("Unsupported WAV fmt chunk size");
    }
    std::array<std::uint8_t, max_fmt_bytes> fmt{};
    const auto consumed = static_cast<std::size_t>(std::min<std::uint64_t>(size, fmt.size()));
    file.read_exact_at(offset, std::span(fmt).first(consumed));
    const auto tag = load_le<std::uint16_t>(fmt.data());
    const auto channels = load_le<std::uint16_t>(&fmt[2]);
    const auto rate = load_le<std::uint32_t>(&fmt[4]);
    const auto bits = load_le<std::uint16_t>(&fmt[14]);
    if ((channels != 1 && channels != 2) || !audio::is_supported_sample_rate(rate)) {
        throw std::runtime_error("Expected mono or stereo PCM16/24/32 or Float32 at a supported rate");
    }
    auto valid_bits = bits;
    bool floating = tag == wave_format_ieee_float;
    if (tag == wave_format_pcm) {
        // PCMWAVEFORMAT, or WAVEFORMATEX whose cbSize PCM ignores.
        if (size != 16 && size != 18) {
            throw std::runtime_error("Unsupported PCM fmt chunk size");
        }
    } else if (floating) {
        // Accept the common 16-byte form as well as WAVEFORMATEX with cbSize=0.
        if ((size != 16 && size != 18) || (size == 18 && load_le<std::uint16_t>(&fmt[16]) != 0)) {
            throw std::runtime_error("Unsupported Float32 fmt chunk size or extension");
        }
    } else if (tag == wave_format_extensible) {
        if (size < max_fmt_bytes) {
            throw std::runtime_error("Truncated Extensible WAV fmt chunk");
        }
        const auto extension_bytes = load_le<std::uint16_t>(&fmt[16]);
        if (extension_bytes < 22 || 18ULL + extension_bytes > size) {
            throw std::runtime_error("Inconsistent Extensible WAV extension size");
        }
        floating = std::memcmp(&fmt[24], float_subformat.data(), float_subformat.size()) == 0;
        if (!floating && std::memcmp(&fmt[24], pcm_subformat.data(), pcm_subformat.size()) != 0) {
            throw std::runtime_error("Unsupported Extensible WAV subformat");
        }
        const auto mask = load_le<std::uint32_t>(&fmt[20]);
        if (mask != 0 && mask != (channels == 1 ? 0x4U : 0x3U)) {
            throw std::runtime_error("Unsupported Extensible WAV channel mask");
        }
        valid_bits = load_le<std::uint16_t>(&fmt[18]);
    } else {
        throw std::runtime_error("Unsupported WAV format tag");
    }
    if (floating && (valid_bits != 32 || bits != 32)) {
        throw std::runtime_error("Unsupported Float32 valid bits or container size");
    }
    if (!floating && !((valid_bits == 16 && bits == 16) || (valid_bits == 24 && (bits == 24 || bits == 32)) ||
        (valid_bits == 32 && bits == 32))) {
        throw std::runtime_error("Unsupported PCM valid bits or container size");
    }
    const auto sample = floating ? audio::float32_le : valid_bits == 16 ? audio::int16_le : valid_bits == 32 ? audio::int32_le :
        bits == 24 ? audio::int24_le : audio::int24_in32_le;
    const audio::StreamFormat format{rate, channels, sample};
    if (load_le<std::uint16_t>(&fmt[12]) != format.frame_bytes() ||
        load_le<std::uint32_t>(&fmt[8]) != rate * format.frame_bytes()) {
        throw std::runtime_error("Inconsistent WAV block alignment or byte rate");
    }
    return format;
}

} // namespace

WavInfo parse_wav_header(ReadOnlyFile& file)
{
    const auto file_size = file.size();
    std::array<std::uint8_t, 12> riff;
    file.read_exact_at(0, std::span(riff));
    const bool rf64 = std::memcmp(riff.data(), "RF64", 4) == 0;
    if ((!rf64 && std::memcmp(riff.data(), "RIFF", 4) != 0) || std::memcmp(&riff[8], "WAVE", 4) != 0) {
        throw std::runtime_error("Expected a RIFF/WAVE or RF64/WAVE file");
    }
    // Every read names its own offset; next_chunk is the only walk position.
    std::uint64_t next_chunk = riff.size();
    std::optional<Rf64Sizes> sizes;
    if (rf64) {
        sizes = read_rf64_sizes(file, file_size, next_chunk);
    }
    const auto size32 = load_le<std::uint32_t>(&riff[4]);
    const std::uint64_t riff_size = sizes && size32 == size_placeholder ? sizes->riff_size : size32;
    // EBU Tech 3306 uses ds64 only when the corresponding 32-bit field is
    // FFFFFFFF. Comparing before adding 8 also handles hostile 64-bit lengths.
    if (file_size < riff.size() || riff_size != file_size - 8) {
        throw std::runtime_error("Invalid WAV container size");
    }
    const auto end = file_size;

    WavInfo info;
    std::uint64_t data_bytes{};
    std::uint64_t fact_offset{}, fact_size{};
    bool duplicate_fact{};
    while (next_chunk < end) {
        const auto chunk = read_chunk_header(file, next_chunk, end);
        const auto payload = next_chunk + chunk.size();
        if (sizes && chunk_is(chunk, "ds64")) {
            throw std::runtime_error("Duplicate RF64 ds64 chunk");
        }
        std::uint64_t size = load_le<std::uint32_t>(&chunk[4]);
        if (sizes && size == size_placeholder) {
            size = chunk_is(chunk, "data") ? sizes->data_size : sizes->table_size(file, chunk);
        }
        // Bound the payload before any of it is read.
        next_chunk = chunk_end(payload, size, end);
        if (chunk_is(chunk, "fmt ")) {
            if (info.format.channels != 0) {
                throw std::runtime_error("Duplicate WAV fmt chunk");
            }
            info.format = read_format(file, payload, size);
        } else if (chunk_is(chunk, "data")) {
            if (info.data_offset != 0) {
                throw std::runtime_error("Duplicate WAV data chunk");
            }
            info.data_offset = payload;
            data_bytes = size;
        } else if (chunk_is(chunk, "fact")) {
            // Defer its contents until fmt is known; PCM keeps its existing
            // unknown-chunk behavior, regardless of fact's order or payload.
            if (fact_offset != 0) { duplicate_fact = true; }
            else { fact_offset = payload; fact_size = size; }
        }
    }
    if (info.format.channels == 0 || info.data_offset == 0) {
        throw std::runtime_error("Missing WAV fmt or data chunk");
    }
    const auto frame_bytes = info.format.frame_bytes();
    if (data_bytes % frame_bytes != 0) {
        throw std::runtime_error("Incomplete WAV frame");
    }
    info.frame_count = data_bytes / frame_bytes;
    if (info.format.sample.kind == audio::SampleKind::floating_point && fact_offset != 0) {
        // Float's uncompressed data length is authoritative. Missing fact is
        // accepted for compatibility, but a supplied declaration must agree.
        if (duplicate_fact || fact_size < 4) {
            throw std::runtime_error("Invalid or duplicate Float32 fact chunk");
        }
        std::array<std::uint8_t, 4> frames;
        file.read_exact_at(fact_offset, std::span(frames));
        const auto frames32 = load_le<std::uint32_t>(frames.data());
        const std::uint64_t declared_frames = sizes && frames32 == size_placeholder ? sizes->sample_count : frames32;
        if (declared_frames != info.frame_count) {
            throw std::runtime_error("Float32 fact frame count disagrees with data");
        }
    }
    return info;
}

} // namespace wavplay::media
