#pragma once

#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_builder.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/audio/sample_format.h"
#include "wavplay/media/wav_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace wavplay_test {

// Native 16-bit samples of a PCM16 WAV, through the product's planar read.
inline std::size_t read_pcm16(wavplay::media::WavReader& reader, std::span<std::int16_t> left,
    std::span<std::int16_t> right)
{
    return reader.read_planar(wavplay::audio::int16_le, std::as_writable_bytes(left), std::as_writable_bytes(right));
}

// Opening must refuse the WAV as malformed or unsupported and release it. An
// I/O failure is rethrown: a fixture that could not be opened would otherwise
// mask a parser accepting bad input.
inline void expect_wav_rejected(const char* name, const Bytes& bytes)
{
    TempFile file;
    const auto* path = file.write(bytes);
    try {
        const wavplay::media::WavReader reader(path);
    } catch (const std::system_error&) {
        throw;
    } catch (const std::runtime_error&) {
        expect("rejected WAV released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        return;
    }
    throw std::runtime_error(std::string(name) + ": expected rejection, got a valid reader");
}

// RF64 sources, formats, sample data and byte checks shared by the container
// and reader suites.
struct Rf64Source {
    wavplay::audio::SampleFormat sample;
    bool extensible;
};

inline constexpr std::array rf64_sources{
    Rf64Source{wavplay::audio::int16_le, false}, Rf64Source{wavplay::audio::int24_le, false},
    Rf64Source{wavplay::audio::int32_le, false}, Rf64Source{wavplay::audio::int16_le, true},
    Rf64Source{wavplay::audio::int24_le, true}, Rf64Source{wavplay::audio::int24_in32_le, true},
    Rf64Source{wavplay::audio::int32_le, true}, Rf64Source{wavplay::audio::float32_le, false},
    Rf64Source{wavplay::audio::float32_le, true}};

inline Chunk rf64_format(const Rf64Source& source, std::uint8_t channels, std::uint32_t rate)
{
    const auto mask = channels == 1 ? 4U : 3U;
    if (source.sample == wavplay::audio::float32_le) {
        return source.extensible ? extensible_float32(channels, rate, mask) : float32(channels, rate);
    }
    return source.extensible ? extensible_pcm(channels, rate, source.sample.valid_bits,
        static_cast<std::uint8_t>(8 * source.sample.bytes()), mask) : pcm_integer(channels, rate, source.sample.valid_bits);
}

inline Chunk rf64_samples(const Rf64Source& source, std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    using namespace wavplay::audio;
    if (source.sample == float32_le) { return float32_ramp(channels, frames, start); }
    if (source.sample == int24_in32_le) { return pcm24_in32_ramp(channels, frames, start); }
    return source.sample == int16_le ? pcm16_ramp(channels, frames, start) :
        source.sample == int24_le ? pcm24_ramp(channels, frames, start) : pcm32_ramp(channels, frames, start);
}

// Only this independent byte oracle sees the fixture's sample representation.
// Requests include guards and, at EOF, an extra frame that must stay untouched.
inline void check_rf64_read(wavplay::media::WavReader& reader, const Chunk& data, const Rf64Source& source,
    std::uint8_t channels, bool final = false)
{
    const auto input_bytes = source.sample.bytes();
    const auto frames = data.payload.size() / (channels * input_bytes);
    const bool floating = source.sample == wavplay::audio::float32_le;
    const auto type = floating ? ASIOSTFloat64MSB : source.sample.valid_bits == 16 ? ASIOSTInt16MSB :
        source.sample.valid_bits == 24 ? ASIOSTInt24MSB : ASIOSTInt32MSB;
    const auto output = *wavplay::asio::to_sample_format(type);
    expect("oracle container bytes", output.bytes(), output_sample_bytes(type));
    const auto count = (frames + (final ? 1 : 0)) * output.bytes();
    Bytes left(count + 2, 0xa5), right(count + 4, 0xa5);
    const auto lspan = std::as_writable_bytes(std::span(left).subspan(1, count));
    const auto rspan = std::as_writable_bytes(std::span(right).subspan(3, count));
    expect("RF64 exact frame count", reader.read_planar(output, lspan, rspan), frames);
    if (final) { expect("RF64 EOF leaves the output unchanged", reader.read_planar(output, lspan, rspan), 0); }
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto& actual = channel == 0 ? left : right;
        const std::size_t prefix = channel == 0 ? 1 : 3;
        for (std::size_t byte = 0; byte < actual.size(); ++byte) {
            unsigned expected = 0xa5;
            if (byte >= prefix && byte < prefix + frames * output.bytes()) {
                const auto frame = (byte - prefix) / output.bytes();
                const auto offset = (frame * channels + (channels == 1 ? 0 : channel)) * input_bytes +
                    (source.sample == wavplay::audio::int24_in32_le ? 1 : 0);
                expected = expected_output_sample(data.payload.data() + offset, source.sample.valid_bits, type, floating)
                    [(byte - prefix) % output.bytes()];
            }
            if (actual[byte] != expected) { expect("RF64 sample bytes and guards", actual[byte], expected); }
        }
    }
}

inline void set_rf64_u32(Bytes& bytes, std::size_t offset, std::uint32_t value)
{
    std::ranges::copy(le32(value), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

inline void set_rf64_u64(Bytes& bytes, std::size_t offset, std::uint64_t value)
{
    std::ranges::copy(le64(value), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

inline void lock_rf64_bytes(HANDLE handle, std::uint64_t offset, std::uint64_t count)
{
    if (!LockFile(handle, static_cast<DWORD>(offset), static_cast<DWORD>(offset >> 32),
        static_cast<DWORD>(count), static_cast<DWORD>(count >> 32))) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock RF64 fixture region");
    }
}

} // namespace wavplay_test
