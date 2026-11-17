#pragma once

#include "wavplay/audio/sample_format.h"

#include <cstdint>

namespace wavplay::media {

class ReadOnlyFile;

// The validated layout of one WAV file's uncompressed sample data.
struct WavInfo {
    audio::StreamFormat format;
    std::uint64_t data_offset{};
    std::uint64_t frame_count{};
};

// Validates the complete RIFF/RF64 container against the file size. The fixed
// 12-byte RIFF header is read at offset 0 before any bound, so a shorter file is
// refused as an unexpected end of file; every chunk read names its own offset
// inside bounds already checked. No read relies on the handle's file pointer:
// readers start from data_offset themselves. Unknown
// chunks, any chunk order and odd-size padding are accepted; missing or duplicate
// fmt/data chunks, truncation, inconsistent sizes and unsupported formats are
// rejected before any PCM is read.
// RF64 requires ds64 first; later chunks retain arbitrary ordering. A 32-bit
// FFFFFFFF size uses ds64, while ordinary 32-bit values remain authoritative.
// Additional sentinel chunks use same-ID table entries in occurrence order.
// Unused table entries and reserved ds64 bytes are ignored without loading the
// whole table. Float fact's sentinel count uses ds64.sampleCount; otherwise the
// existing optional-Float-fact and ignored-PCM-fact contracts remain unchanged.
// Supported samples: mono/stereo PCM16/24/32 and Extensible PCM valid/container
// widths 16/16, 24/24, 24/32 and 32/32. Extensible masks are unspecified (0),
// mono front-centre (4), or stereo front-left/right (3). The reader validates
// unused sample bits after this structural validation. Plain and Extensible
// Float32 share the same container validation and preserve all IEEE encodings.
[[nodiscard]] WavInfo parse_wav_header(ReadOnlyFile& file);

} // namespace wavplay::media
