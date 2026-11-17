#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_builder.h"
#include "fixtures/wav_reader_fixture.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/audio/planar_conversion.h"
#include "wavplay/core/win32.h"
#include "wavplay/media/read_only_file.h"
#include "wavplay/media/wav_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// Only find_planar_converter creates converters, so a reader checks just their
// source format. Its staging buffer holds at least one of the widest source
// frames, stereo 32-bit, so every batch makes progress.
static_assert(!std::is_aggregate_v<wavplay::audio::PlanarConverter>);
static_assert(!std::is_default_constructible_v<wavplay::audio::PlanarConverter>);
static_assert(8 <= wavplay::media::WavReader::staging_bytes);
// The read-size contract: the Extensible PCM24/32 scan reads up to 64 KiB per
// request, playback 8 KiB (test_read_request_sizes counts the requests).
static_assert(wavplay::media::WavReader::validation_bytes == 64 * 1024);
static_assert(wavplay::media::WavReader::staging_bytes == 8 * 1024);

// The frame cursor over a validated WAV: reads, seeks, reopening, handle
// ownership, I/O failures, destination checks, conversion refusals, the
// complete Extensible unused-bit scan and the file's positional-read offsets.
// Container conformance is wav_header's.
namespace {

using namespace wavplay_test;

void test_reader_lifetime()
{
    TempFile source_file, replaced_file;
    const auto* source_path = source_file.write(wave({pcm16(2, 48000), {"data", {1, 0, 2, 0, 3, 0, 4, 0}}}));
    const auto* replaced_path = replaced_file.write(wave({pcm16(1, 44100), {"data", {5, 0}}}));
    std::array<std::int16_t, 2> left{}, right{};
    {
        wavplay::media::WavReader destination(replaced_path);
        {
            wavplay::media::WavReader source(source_path);
            expect("write access to open WAV", probe_file_access(source_path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
            expect("delete access to open WAV", probe_file_access(source_path, DELETE), ERROR_SHARING_VIOLATION);
            expect("first frame before move", read_pcm16(source, std::span(left).first(1), std::span(right).first(1)), 1);
            wavplay::media::WavReader moved(std::move(source));
            destination = std::move(moved);
        }
        expect("replaced WAV released", probe_file_access(replaced_path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        expect("moved WAV still held", probe_file_access(source_path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
        expect("moved WAV rate", destination.info().format.sample_rate, 48000);
        expect("frames remaining after move", read_pcm16(destination, left, right), 1);
        expect("moved left frame", left[0], 3);
        expect("moved right frame", right[0], 4);
    }
    expect("WAV released after destruction", probe_file_access(source_path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_seek_and_reopen()
{
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        TempFile file;
        const auto* path = file.write(wave({{"JUNK", {1, 2, 3}}, pcm16(channels, 48000), pcm16_ramp(channels, 7)}));
        {
            wavplay::media::WavReader original(path);
            std::array<std::int16_t, 1> left{}, right{};
            expect("initial remaining frames", original.remaining_frames(), 7);
            (void)read_pcm16(original, left, right);
            auto reader = original.reopen();
            expect("reopened reader at start", static_cast<int>(reader.at_start()), 1);
            expect("original cursor after reopen", original.remaining_frames(), 6);
            reader.seek_frame(3);
            expect("remaining frames after seek", reader.remaining_frames(), 4);
            (void)read_pcm16(reader, left, right);
            expect("seek left frame", left[0], 3);
            expect("seek right frame", right[0], channels == 1 ? 3 : -4);
            (void)read_pcm16(original, left, right);
            expect("original independent left cursor", left[0], 1);
            expect("original independent right cursor", right[0], channels == 1 ? 1 : -2);

            expect_throw<std::out_of_range>("seek beyond EOF", [&] { reader.seek_frame(8); });
            expect_throw<std::out_of_range>("seek overflow", [&] { reader.seek_frame(std::numeric_limits<std::uint64_t>::max()); });
            expect("invalid seek preserves position", reader.remaining_frames(), 3);
            (void)read_pcm16(reader, left, right);
            expect("invalid seek preserves file cursor", left[0], 4);

            reader.seek_frame(7);
            expect("seek to EOF", static_cast<int>(reader.at_end()), 1);
            expect("read at sought EOF", read_pcm16(reader, left, right), 0);
            reader.seek_frame(6);
            expect("last frame after backward seek", read_pcm16(reader, left, right), 1);
            expect("last left frame", left[0], 6);
            reader.seek_frame(0);
            expect("seek to first frame", static_cast<int>(reader.at_start()), 1);
            expect("seek does not revalidate or replace the file", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);

            auto moved = std::move(reader);
            expect_throw<std::logic_error>("seek on moved reader", [&] { reader.seek_frame(0); });
            expect_throw<std::logic_error>("reopen moved reader", [&] { (void)reader.reopen(); });
            expect("moved reopened reader", read_pcm16(moved, left, right), 1);
            expect("moved reopened first frame", left[0], 0);
        }
        expect("reopened handles released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    }
}

void test_failed_reader_cannot_seek_or_reopen()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3)}));
    wavplay::media::WavReader reader(path);
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open seek failure lock");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    if (!LockFile(lock.get(), static_cast<DWORD>(reader.info().data_offset), 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock reader frame");
    }
    std::array<std::int16_t, 1> left{}, right{};
    expect_throw<std::system_error>("locked read", [&] { (void)read_pcm16(reader, left, right); });
    expect_throw<std::logic_error>("seek failed reader", [&] { reader.seek_frame(0); });
    expect_throw<std::logic_error>("reopen failed reader", [&] { (void)reader.reopen(); });
}

void test_reopen_reuses_validated_metadata()
{
    TempFile file;
    const auto* path = file.write(wave({{"JUNK", {1, 2, 3}}, pcm16(2, 48000), pcm16_ramp(2, 3)}));
    wavplay::media::WavReader original(path);
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open metadata reuse lock");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    const auto data = static_cast<DWORD>(original.info().data_offset);
    // The protected file was already fully validated. Resuming needs no header
    // reads, even when another reader temporarily locks the header bytes.
    if (!LockFile(lock.get(), 0, 0, data, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock validated WAV header");
    }
    auto reader = original.reopen();
    expect("reopen retains rate", reader.info().format.sample_rate, 48000);
    expect("reopen retains channels", reader.info().format.channels, 2);
    expect("reopen retains data offset", reader.info().data_offset, original.info().data_offset);
    expect("reopen retains frame count", reader.info().frame_count, 3);
    std::array<std::int16_t, 1> left{}, right{};
    expect("reopen starts at protected data", read_pcm16(reader, left, right), 1);
    expect("reopened first left", left[0], 0);
    expect("reopened first right", right[0], -1);

    if (!LockFile(lock.get(), data + 4, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock reopened PCM frame");
    }
    expect_throw<std::system_error>("reopened reader detects PCM I/O failure", [&] { (void)read_pcm16(reader, left, right); });
    expect_throw<std::logic_error>("failed reopened cursor stays closed", [&] { (void)reader.reopen(); });
    expect("failed reopened cursor does not consume original", original.remaining_frames(), 3);
    expect("original cursor survives reopened read failure", read_pcm16(original, left, right), 1);
    expect("original first left", left[0], 0);
    expect("original first right", right[0], -1);
}

// The final frame lies beyond the first staging read. Native and widened reads
// both fail with the lock's own error, close their readers and release the WAV.
void test_read_failure(const wavplay::audio::SampleFormat& output)
{
    constexpr std::size_t frames = 4097;
    const auto output_bytes = output.bytes();
    Bytes samples(4 * (frames - 1), 0);
    samples[0] = 7;
    samples[2] = 9;
    samples.insert(samples.end(), {11, 0, 13, 0});
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", std::move(samples)}}));
    wavplay::media::WavReader reader(path);
    wavplay::media::WavReader wide_reader(path);
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open lock handle");
    }
    std::unique_ptr<void, decltype(&CloseHandle)> lock_file(handle, &CloseHandle);
    const auto offset = static_cast<DWORD>(reader.info().data_offset + 4 * (frames - 1));
    if (!LockFile(lock_file.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock audio frame");
    }
    const auto expect_locked = [](auto read) {
        try {
            read();
        } catch (const std::system_error& error) {
            if (error.code() != std::error_code(ERROR_LOCK_VIOLATION, std::system_category())) {
                throw;
            }
            return;
        }
        throw std::runtime_error("expected a locked frame read to fail, call succeeded");
    };
    std::vector<std::int16_t> left(frames), right(frames);
    Bytes wide_left(output_bytes * frames), wide_right(output_bytes * frames);
    const auto read_wide = [&](wavplay::media::WavReader& source) {
        return source.read_planar(output, std::as_writable_bytes(std::span(wide_left)),
            std::as_writable_bytes(std::span(wide_right)));
    };
    expect_locked([&] { (void)read_pcm16(reader, left, right); });
    expect_locked([&] { (void)read_wide(wide_reader); });
    expect("failed reader cannot start another run", static_cast<int>(wide_reader.at_start()), 0);
    if (!UnlockFile(lock_file.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "unlock audio frame");
    }
    lock_file.reset();
    expect("failed readers released WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    expect_throw<std::logic_error>("empty request after failure", [&] { (void)read_pcm16(reader, {}, {}); });
    expect_throw<std::logic_error>("retry after unlock", [&] { (void)read_pcm16(reader, left, right); });
    expect_throw<std::logic_error>("empty widened request after failure", [&] {
        (void)wide_reader.read_planar(output, {}, {});
    });
    expect_throw<std::logic_error>("widened retry after unlock", [&] { (void)read_wide(wide_reader); });
    std::ranges::fill(wide_left, 0xa5);
    std::ranges::fill(wide_right, 0xa5);
    wavplay::media::WavReader reopened(path);
    expect("read after reopening", read_wide(reopened), frames);
    expect("reopened first left", wide_left[output_bytes - 2], 7);
    expect("reopened first right", wide_right[output_bytes - 2], 9);
    expect("reopened final left", wide_left[wide_left.size() - 2], 11);
    expect("reopened final right", wide_right[wide_right.size() - 2], 13);
}

void test_output_buffers()
{
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), {"data", {1, 0, 2, 0, 3, 0, 4, 0}}})));
    std::array<std::int16_t, 4> samples{};
    const auto left = std::span(samples).first(2);
    const auto right = std::span(samples).last(2);
    const auto overlap = std::span(samples).subspan(1, 2);
    expect_throw<std::invalid_argument>("different sizes", [&] { (void)read_pcm16(reader, left, right.first(1)); });
    expect_throw<std::invalid_argument>("identical buffers", [&] { (void)read_pcm16(reader, left, left); });
    expect_throw<std::invalid_argument>("overlap on right", [&] { (void)read_pcm16(reader, left, overlap); });
    expect_throw<std::invalid_argument>("overlap on left", [&] { (void)read_pcm16(reader, overlap, left); });
    expect("empty request", read_pcm16(reader, {}, {}), 0);
    expect("adjacent buffers", read_pcm16(reader, left, right), 2);
    expect("left[0]", left[0], 1);
    expect("right[0]", right[0], 2);
    expect("left[1]", left[1], 3);
    expect("right[1]", right[1], 4);
}

void test_invalid_conversion_layouts()
{
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4)})));
    const auto converter = wavplay::audio::find_planar_converter(reader.info().format, wavplay::audio::int16_le).value();
    std::array<std::byte, 64> planes;
    planes.fill(std::byte{0xa5});
    const wavplay::audio::PlanarBlocks target{.left = planes.data(), .right = planes.data() + 32,
        .block_frames = 2, .stride = 8, .frames = 4};
    const auto reject_layout = [&](const char* name, const wavplay::audio::PlanarBlocks& layout) {
        expect_throw<std::invalid_argument>(name, [&] { (void)reader.read_planar(converter, layout); });
    };
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    auto layout = target;
    layout.left = nullptr;
    reject_layout("null nonempty left plane", layout);
    layout = target;
    layout.right = nullptr;
    reject_layout("null nonempty right plane", layout);
    layout = target;
    layout.block_frames = 0;
    reject_layout("zero block width", layout);
    layout = target;
    layout.first_frame = max_size;
    reject_layout("frame end overflows", layout);
    layout = target;
    layout.block_frames = 1;
    layout.stride = max_size / 2 + 1;
    layout.first_frame = 2;
    layout.frames = 1;
    reject_layout("block offset multiplication overflows", layout);
    layout.stride = max_size - 1;
    layout.first_frame = 1;
    reject_layout("last sample end overflows", layout);
    layout = target;
    layout.left = reinterpret_cast<std::byte*>(std::numeric_limits<std::uintptr_t>::max() - 1);
    reject_layout("plane address end overflows", layout);

    expect("invalid descriptors preserve the frame cursor", reader.remaining_frames(), 4);
    for (const auto byte : planes) {
        expect("invalid descriptors leave destinations untouched", static_cast<int>(byte), 0xa5);
    }
    expect("valid read after invalid descriptors", reader.read_planar(converter, target), 4);
    expect("rejected descriptors preserve the file cursor", static_cast<int>(planes[0]), 0);
    expect("valid read still reaches the last frame", static_cast<int>(planes[10]), 3);
}

void test_interleaved_block_aliases()
{
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 24)})));
    const auto converter = wavplay::audio::find_planar_converter(reader.info().format, wavplay::audio::int16_le).value();
    std::array<std::byte, 512> storage;
    storage.fill(std::byte{0xa5});
    for (const bool reverse : {false, true}) {
        // B=16, S=64. Both phases near zero and near the next slot must be
        // rejected, including aliases separated by one or two whole strides.
        for (const std::size_t distance : {0, 1, 15, 49, 63, 64, 65, 79, 113, 127, 128, 143}) {
            auto* first = storage.data();
            auto* second = first + distance;
            const wavplay::audio::PlanarBlocks target{.left = reverse ? second : first,
                .right = reverse ? first : second, .block_frames = 8, .stride = 64, .frames = 24};
            expect_throw<std::invalid_argument>("same-slot or neighbouring-slot channel alias", [&] {
                (void)reader.read_planar(converter, target);
            });
        }
    }
    expect("channel alias rejection preserves the cursor", reader.remaining_frames(), 24);
    for (const auto byte : storage) {
        expect("channel alias rejection preserves all bytes", static_cast<int>(byte), 0xa5);
    }
    for (const std::size_t distance : {16, 48, 80, 112}) {
        reader.seek_frame(0);
        storage.fill(std::byte{0xa5});
        const wavplay::audio::PlanarBlocks target{.left = storage.data(), .right = storage.data() + distance,
            .block_frames = 8, .stride = 64, .frames = 24};
        expect("touching slot boundaries remain disjoint", reader.read_planar(converter, target), 24);
        auto expected = storage;
        expected.fill(std::byte{0xa5});
        for (std::size_t frame = 0; frame < 24; ++frame) {
            const auto offset = frame / 8 * 64 + frame % 8 * 2;
            const auto left = static_cast<std::uint16_t>(frame);
            const auto right = static_cast<std::uint16_t>(~frame);
            expected[offset] = static_cast<std::byte>(left & 255);
            expected[offset + 1] = static_cast<std::byte>(left >> 8);
            expected[distance + offset] = static_cast<std::byte>(right & 255);
            expected[distance + offset + 1] = static_cast<std::byte>(right >> 8);
        }
        expect("accepted periodic spans preserve every sample and gap", static_cast<int>(storage == expected), 1);
    }
    // Partial ranges whose hulls do not overlap retain the original exact-range
    // contract; unused full-block space need not separate these destinations.
    reader.seek_frame(0);
    storage.fill(std::byte{0xa5});
    const wavplay::audio::PlanarBlocks partial{.left = storage.data(), .right = storage.data() + 2,
        .block_frames = 8, .stride = 64, .frames = 1};
    expect("adjacent partial hulls remain valid", reader.read_planar(converter, partial), 1);
    auto expected = storage;
    expected.fill(std::byte{0xa5});
    expected[0] = expected[1] = std::byte{0};
    expected[2] = expected[3] = std::byte{0xff};
    expect("partial hulls touch only the requested samples", static_cast<int>(storage == expected), 1);
}

void test_interleaved_block_reads()
{
    using namespace wavplay::audio;
    constexpr std::size_t stride = 64, leading = 17, first_frame = 3;
    const std::array outputs{int16_le, int24_le, int32_le, float32_le,
        SampleFormat{SampleKind::floating_point, 8, 64}};
    for (const std::uint8_t channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto staging_frames = wavplay::media::WavReader::staging_bytes / (channels * 2);
        const auto frames = 3 * staging_frames + 13;
        TempFile file;
        const auto* path = file.write(wave({pcm16(channels, 48000), pcm16_ramp(channels, frames)}));
        for (const auto output : outputs) {
            const std::size_t width = output.bytes() == 8 ? 4 : 8;
            const auto slots = (first_frame + frames + 11 + width - 1) / width;
            const auto converter = find_planar_converter({48000, channels, int16_le}, output).value();
            std::vector<std::byte> reference_left(frames * output.bytes()), reference_right(reference_left.size());
            wavplay::media::WavReader reference(path);
            expect("contiguous reference for interleaved reads", reference.read_planar(converter, reference_left, reference_right), frames);
            for (const std::size_t distance : {32, 96}) {
                for (const bool reverse : {false, true}) {
                    std::vector<std::byte> actual(leading + slots * stride + distance + 64, std::byte{0xa5});
                    auto expected = actual;
                    auto* first = actual.data() + leading;
                    auto* second = first + distance;
                    auto* left = reverse ? second : first;
                    auto* right = reverse ? first : second;
                    wavplay::media::WavReader reader(path);
                    std::size_t done = 0;
                    const auto read = [&](std::size_t count) {
                        const auto result = reader.read_planar(converter, PlanarBlocks{.left = left, .right = right,
                            .block_frames = width, .stride = stride, .first_frame = first_frame + done, .frames = count});
                        done += result;
                        return result;
                    };
                    expect("interleaved first split block", read(width - 1), width - 1);
                    expect("interleaved read crosses staging and block boundaries", read(staging_frames + 5), staging_frames + 5);
                    const auto remaining = frames - done;
                    expect("interleaved short final read", read(remaining + 11), remaining);
                    expect("interleaved EOF writes nothing", read(1), 0);
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        const auto absolute = first_frame + frame;
                        const auto offset = leading + absolute / width * stride + absolute % width * output.bytes();
                        for (std::size_t byte = 0; byte < output.bytes(); ++byte) {
                            expected[offset + (reverse ? distance : 0) + byte] = reference_left[frame * output.bytes() + byte];
                            expected[offset + (reverse ? 0 : distance) + byte] = reference_right[frame * output.bytes() + byte];
                        }
                    }
                    expect("interleaved reads preserve samples, slot gaps and outer guards", static_cast<int>(actual == expected), 1);
                }
            }
        }
    }
}

void test_foreign_converter()
{
    using namespace wavplay::audio;
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), {"data", {1, 2, 3, 4}}})));
    // Genuine converters, each chosen for another channel count, rate or
    // encoding than this stereo 48 kHz PCM16 WAV, including wider frames.
    const auto mono = find_planar_converter({48000, 1, int16_le}, int16_le).value();
    const std::array foreign{mono,
        find_planar_converter({44100, 2, int16_le}, int16_le).value(),
        find_planar_converter({48000, 2, int24_le}, int32_le).value(),
        find_planar_converter({48000, 2, int32_le}, int32_le).value(),
        find_planar_converter({48000, 2, float32_le}, SampleFormat{SampleKind::floating_point, 8, 64}).value()};
    std::array<std::byte, 2> left{}, right{};
    expect_throw<std::invalid_argument>("mono converter on a stereo WAV", [&] { (void)reader.read_planar(mono, left, right); });
    expect("refused converter leaves the cursor", reader.remaining_frames(), 1);
    std::array<std::byte, 32> planes;
    planes.fill(std::byte{0xa5});
    for (const auto& converter : foreign) {
        const auto bytes = converter.output_sample_bytes();
        expect_throw<std::invalid_argument>("converter for another format into planes", [&] {
            (void)reader.read_planar(converter, std::span(planes).first(bytes), std::span(planes).subspan(16, bytes));
        });
        const PlanarBlocks blocks{.left = planes.data(), .right = planes.data() + 16, .block_frames = 1,
            .stride = bytes, .frames = 1};
        expect_throw<std::invalid_argument>("converter for another format into blocks", [&] {
            (void)reader.read_planar(converter, blocks);
        });
    }
    expect("refused converters leave the cursor", reader.remaining_frames(), 1);
    for (const auto byte : planes) {
        expect("refused converters leave destinations untouched", static_cast<int>(byte), 0xa5);
    }
    const auto own = find_planar_converter(reader.info().format, int16_le).value();
    expect("the WAV's own converter reads after refusals", reader.read_planar(own, left, right), 1);
    expect("own converter left sample", static_cast<int>(left[1]), 2);
    expect("own converter right sample", static_cast<int>(right[1]), 4);
    [[maybe_unused]] const auto moved = std::move(reader);
    expect_throw<std::logic_error>("a closed reader refuses its own converter", [&] { (void)reader.read_planar(own, left, right); });
    expect_throw<std::logic_error>("closure is reported before a foreign source", [&] {
        (void)reader.read_planar(mono, left, right);
    });
}

void test_large_read(std::uint8_t channels)
{
    constexpr int frames = 4101;
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(channels, 48000), pcm16_ramp(channels, frames)})));
    std::vector<std::int16_t> left(frames + 1, 12345), right(frames + 1, -12345);
    expect("short read", read_pcm16(reader, left, right), frames);
    for (int i = 0; i < frames; ++i) {
        expect("left frame", left[i], i);
        expect("right frame", right[i], channels == 1 ? i : -i - 1);
    }
    expect("unused left slot", left.back(), 12345);
    expect("unused right slot", right.back(), -12345);
    expect("EOF", read_pcm16(reader, left, right), 0);
}

void test_mono()
{
    TempFile file;
    const Chunk data{"data", {0xff, 0xff, 0x78, 0x56}};
    for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u}) {
        wavplay::media::WavReader reader(file.write(wave({pcm16(1, rate), data})));
        expect("rate", reader.info().format.sample_rate, rate);
        expect("channels", reader.info().format.channels, 1);
        expect("data offset", reader.info().data_offset, 44);
        expect("frame count", reader.info().frame_count, 2);
        std::array<std::int16_t, 2> left{}, right{};
        expect("read", read_pcm16(reader, left, right), 2);
        expect("left[0]", left[0], -1);
        expect("right[0]", right[0], -1);
        expect("left[1]", left[1], 0x5678);
        expect("right[1]", right[1], 0x5678);
        expect("EOF", read_pcm16(reader, left, right), 0);
    }
}

void test_wide_pcm_reader(std::uint8_t bits)
{
    const auto sample = bits == 24 ? wavplay::audio::int24_le : wavplay::audio::int32_le;
    const auto sample_bytes = sample.bytes();
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u}) {
            TempFile file;
            auto fmt = pcm_integer(channels, rate, bits);
            fmt.payload.insert(fmt.payload.end(), {0xff, 0xff}); // PCM ignores cbSize.
            const auto data = bits == 24 ? pcm24_ramp(channels, 3, 0x7fffff) : pcm32_ramp(channels, 3, 0x7fffffff);
            const auto* path = file.write(wave({{"JUNK", {1, 2, 3}}, data, fmt}));
            wavplay::media::WavReader reader(path);
            expect("wide PCM rate", reader.info().format.sample_rate, rate);
            expect("wide PCM channels", reader.info().format.channels, channels);
            expect("wide PCM format", static_cast<int>(reader.info().format.sample == sample), 1);
            expect("wide PCM frame count excludes padding", reader.info().frame_count, 3);
            expect("wide PCM data before fmt", reader.info().data_offset, 32);
            std::vector<std::byte> left(sample_bytes), right(sample_bytes);
            const auto read = [&](wavplay::media::WavReader& source) {
                return source.read_planar(sample, left, right);
            };
            const auto check = [&](std::size_t frame) {
                for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                    expect("wide PCM left byte", static_cast<unsigned>(left[byte]), data.payload[frame * channels * sample_bytes + byte]);
                    expect("wide PCM right byte", static_cast<unsigned>(right[byte]),
                        data.payload[(frame * channels + channels - 1) * sample_bytes + byte]);
                }
            };
            expect("wide PCM first read", read(reader), 1);
            check(0);
            auto reopened = reader.reopen();
            reopened.seek_frame(2);
            expect("wide PCM reopened last read", read(reopened), 1);
            check(2);
            expect("wide PCM reopened EOF", read(reopened), 0);
            expect("wide PCM original cursor preserved", read(reader), 1);
            check(1);
            reader.seek_frame(0);
            expect("wide PCM backward seek", read(reader), 1);
            check(0);
        }
    }
}

void test_pcm32_large_read()
{
    constexpr std::size_t frames = 4101;
    constexpr std::uint32_t start = 0x7fffff00;
    constexpr std::uint8_t guard = 0xa5;
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        TempFile file;
        const auto* path = file.write(wave({pcm32(channels, 48000), pcm32_ramp(channels, frames, start)}));
        wavplay::media::WavReader reader(path);
        Bytes left(2 + (frames + 1) * 4, guard), right(left.size(), guard);
        const auto left_span = std::as_writable_bytes(std::span(left).subspan(1, (frames + 1) * 4));
        const auto right_span = std::as_writable_bytes(std::span(right).subspan(1, (frames + 1) * 4));
        expect("PCM32 multi-staging read", reader.read_planar(wavplay::audio::int32_le, left_span, right_span), frames);
        expect("PCM32 multi-staging EOF", reader.read_planar(wavplay::audio::int32_le, left_span, right_span), 0);
        for (std::size_t byte = 0; byte < left.size(); ++byte) {
            std::uint8_t expected_left = guard, expected_right = guard;
            if (byte > 0 && byte <= frames * 4) {
                const auto frame = (byte - 1) / 4;
                const auto shift = 8 * ((byte - 1) % 4);
                const auto bits = static_cast<std::uint32_t>(start + frame);
                expected_left = static_cast<std::uint8_t>(bits >> shift);
                expected_right = static_cast<std::uint8_t>((channels == 1 ? bits : ~bits) >> shift);
            }
            expect("PCM32 staged left byte", left[byte], expected_left);
            expect("PCM32 staged right byte", right[byte], expected_right);
        }
        reader.seek_frame(2047); // Just before a mono staging boundary; both sample signs were crossed.
        expect("PCM32 seek and short read", reader.read_planar(wavplay::audio::int32_le,
            left_span.first(4), right_span.first(4)), 1);
        constexpr auto sought = start + 2047u;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            expect("PCM32 sought left byte", left[1 + byte], static_cast<std::uint8_t>(sought >> (8 * byte)));
            expect("PCM32 sought right byte", right[1 + byte],
                static_cast<std::uint8_t>((channels == 1 ? sought : ~sought) >> (8 * byte)));
        }
    }
}

// Literal planar bytes of three PCM16/24/32 frames, read in two-frame requests
// into unaligned planes between guards, including a request at EOF.
void test_planar_output(std::uint8_t channels, const wavplay::audio::SampleFormat& output,
    const Bytes& expected_left, const Bytes& expected_stereo_right, std::uint8_t source_bits = 16)
{
    const Bytes pcm16_samples = channels == 1
        ? Bytes{0x00, 0x80, 0x34, 0x12, 0xff, 0xff}
        : Bytes{0x00, 0x80, 0xff, 0x7f, 0x34, 0x12, 0xde, 0xbc, 0xff, 0xff, 0x00, 0x00};
    const Bytes pcm24_samples = channels == 1
        ? Bytes{0x00, 0x00, 0x80, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff}
        : Bytes{0x00, 0x00, 0x80, 0xff, 0xff, 0x7f, 0x56, 0x34, 0x12, 0xba, 0xdc, 0xfe, 0xff, 0xff, 0xff, 0, 0, 0};
    const Bytes pcm32_samples = channels == 1
        ? Bytes{0, 0, 0, 0x80, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff}
        : Bytes{0, 0, 0, 0x80, 0xff, 0xff, 0xff, 0x7f, 0x78, 0x56, 0x34, 0x12,
            0x98, 0xba, 0xdc, 0xfe, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm_integer(channels, 48000, source_bits),
        {"data", source_bits == 16 ? pcm16_samples : source_bits == 24 ? pcm24_samples : pcm32_samples}})));

    const std::size_t width = expected_left.size() / 3;
    expect("literal sample width", output.bytes(), width);
    constexpr std::uint8_t guard = 0xa5;
    // One leading guard leaves the outputs unaligned. Five slots allow a
    // two-frame request even after all three frames have been consumed.
    Bytes left(1 + 5 * width, guard), right(left.size(), guard);
    const auto read_two = [&](std::size_t start_frame) {
        const auto start = 1 + start_frame * width;
        return reader.read_planar(output, std::as_writable_bytes(std::span(left).subspan(start, 2 * width)),
            std::as_writable_bytes(std::span(right).subspan(start, 2 * width)));
    };
    expect("first output block", read_two(0), 2);
    expect("final output block", read_two(2), 1);
    expect("output EOF", read_two(3), 0);

    const auto check = [&](const char* channel, const Bytes& actual, const Bytes& expected) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto wanted = i > 0 && i <= expected.size() ? expected[i - 1] : guard;
            if (actual[i] != wanted) {
                expect(std::string(channel) + " buffer byte " + std::to_string(i), actual[i], wanted);
            }
        }
    };
    check("left", left, expected_left);
    check("right", right, channels == 1 ? expected_left : expected_stereo_right);
}

void test_partial_output_samples()
{
    using namespace wavplay::audio;
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), {"data", {1, 0, 2, 0}}})));
    std::array<std::uint8_t, 8> buffer{};
    const auto left = std::as_writable_bytes(std::span(buffer).first(4));
    const auto right = std::as_writable_bytes(std::span(buffer).last(4));
    const auto reject = [&](const char* field, const SampleFormat& output, std::span<std::byte> lbuf,
                            std::span<std::byte> rbuf) {
        expect_throw<std::invalid_argument>(field, [&] { (void)reader.read_planar(output, lbuf, rbuf); });
    };
    reject("partial 32-bit sample", int32_le, left.first(2), right.first(2));
    reject("partial Float64 sample", SampleFormat{SampleKind::floating_point, 8, 64}, left, right);
    reject("partial packed 24-bit sample", int24_le, left, right);
    reject("unequal buffers", int16_le, left, right.first(2));
    reject("overlapping buffers", int16_le, left, std::as_writable_bytes(std::span(buffer).subspan(2, 4)));
    for (const auto byte : buffer) {
        expect("byte after rejected request", byte, 0);
    }
    expect("empty request", reader.read_planar(int32_le, {}, {}), 0);
    expect("read after rejected requests", reader.read_planar(int32_le, left, right), 1);
    expect("first left sample after rejection", buffer[2], 1);
    expect("first right sample after rejection", buffer[4 + 2], 2);
}

void test_large_planar_output(std::uint8_t channels, const wavplay::audio::SampleFormat& output, std::size_t width,
    std::uint8_t source_bits = 16)
{
    expect("literal sample width", output.bytes(), width);
    // Every PCM16 word, or a PCM24/32 sign transition, plus a short final read.
    const std::size_t frames = 65536 + (width == 2 ? 15 : 7);
    const std::size_t source_start = source_bits == 16 ? 0 : source_bits == 24 ? 0x7fff00 : 0x7fffff00;
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm_integer(channels, 48000, source_bits),
        source_bits == 16 ? pcm16_ramp(channels, frames) : source_bits == 24 ?
            pcm24_ramp(channels, frames, source_start) : pcm32_ramp(channels, frames, source_start)})));
    const std::size_t request_bytes = width * (frames + 1);
    constexpr std::uint8_t guard = 0xa5;
    Bytes left(1 + request_bytes, guard), right(3 + request_bytes, guard);
    expect("multi-block output", reader.read_planar(output, std::as_writable_bytes(std::span(left).subspan(1)),
        std::as_writable_bytes(std::span(right).subspan(3))), frames);
    const auto check = [&](const Bytes& actual, std::size_t start, bool inverted) {
        for (std::size_t i = 0; i < start; ++i) {
            expect("leading guard", actual[i], guard);
        }
        for (std::size_t i = 0; i < frames; ++i) {
            const auto bits = inverted ? ~(source_start + i) : source_start + i;
            for (std::size_t j = 0; j < width; ++j) {
                const auto padding = width - source_bits / 8;
                const auto expected = j < padding ? 0 : static_cast<std::uint8_t>(bits >> (8 * (j - padding)));
                const auto byte = actual[start + width * i + j];
                if (byte != expected) {
                    expect("output at frame " + std::to_string(i), byte, expected);
                }
            }
        }
        for (std::size_t i = start + width * frames; i < actual.size(); ++i) {
            expect("unused output byte", actual[i], guard);
        }
    };
    check(left, 1, false);
    check(right, 3, channels == 2);
}

void test_all_integer_reader_outputs()
{
    constexpr std::size_t frames = 4101;
    constexpr std::uint8_t guard = 0xa5;
    for (const auto bits : {std::uint8_t{16}, std::uint8_t{24}, std::uint8_t{32}}) {
        const std::size_t sample_bytes = bits / 8;
        const auto sign = std::uint32_t{1} << (bits - 1);
        const auto mask = static_cast<std::uint32_t>((std::uint64_t{1} << bits) - 1);
        const std::array<std::uint32_t, 8> edges{0, 1, mask, sign, sign - 1, sign + 1,
            0x12345678 & mask, 0xfedcba98 & mask};
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            Bytes source(frames * channels * sample_bytes);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto value = frame < edges.size() ? edges[frame] :
                    static_cast<std::uint32_t>(sign - 128 + 0x123 * frame) & mask;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    const auto raw = channel == 0 ? value : ~value & mask;
                    for (unsigned byte = 0; byte < sample_bytes; ++byte) {
                        source[(frame * channels + channel) * sample_bytes + byte] = static_cast<std::uint8_t>(raw >> (8 * byte));
                    }
                }
            }
            TempFile file;
            wavplay::media::WavReader original(file.write(wave({pcm_integer(channels, 48000, bits), {"data", source}})));
            for (const auto output : integer_output_cases) {
                const auto format = wavplay::asio::to_sample_format(output.type).value();
                expect("oracle container bytes", format.bytes(), output.bytes);
                auto reader = original.reopen();
                if (output.valid_bits < bits) {
                    std::array<std::uint8_t, 12> left, right;
                    left.fill(guard); right.fill(guard);
                    expect_throw<std::invalid_argument>("all integer outputs reject insufficient precision", [&] {
                        (void)reader.read_planar(format, std::as_writable_bytes(std::span(left)),
                            std::as_writable_bytes(std::span(right)));
                    });
                    expect("integer rejection preserves input cursor", reader.remaining_frames(), frames);
                    for (const auto byte : left) { expect("integer rejection preserves left", byte, guard); }
                    for (const auto byte : right) { expect("integer rejection preserves right", byte, guard); }
                    continue;
                }
                const auto count = frames * output.bytes;
                Bytes left(count + output.bytes + 2, guard), right(count + output.bytes + 4, guard);
                expect("all integer outputs read across staging boundaries", reader.read_planar(format,
                    std::as_writable_bytes(std::span(left).subspan(1, count + output.bytes)),
                    std::as_writable_bytes(std::span(right).subspan(3, count + output.bytes))), frames);
                expect("all integer outputs reach EOF", reader.read_planar(format,
                    std::as_writable_bytes(std::span(left).subspan(1, output.bytes)),
                    std::as_writable_bytes(std::span(right).subspan(3, output.bytes))), 0);
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const auto& actual = channel == 0 ? left : right;
                    const std::size_t prefix = channel == 0 ? 1 : 3;
                    for (std::size_t byte = 0; byte < prefix; ++byte) { expect("integer output leading guard", actual[byte], guard); }
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        const auto offset = (frame * channels + (channels == 1 ? 0 : channel)) * sample_bytes;
                        const auto expected = expected_integer_sample(source.data() + offset, bits, output);
                        for (unsigned byte = 0; byte < output.bytes; ++byte) {
                            expect("integer output bit pattern", actual[prefix + frame * output.bytes + byte], expected[byte]);
                        }
                    }
                    for (std::size_t byte = prefix + count; byte < actual.size(); ++byte) {
                        expect("integer output EOF and trailing guard", actual[byte], guard);
                    }
                }
            }
        }
    }
}

// Every oracle output that the lossless table refuses for this WAV is refused
// before reading, even for silence whose value would fit. The cursor and both
// planes stay unchanged, and the same cursor then reads a lossless output.
void check_lossy_outputs_refused(const Bytes& wav, std::uint8_t source_bits, bool floating,
    const wavplay::audio::SampleFormat& lossless)
{
    TempFile file;
    wavplay::media::WavReader reader(file.write(wav));
    constexpr std::uint8_t guard = 0xa5;
    std::array<std::uint8_t, 64> buffer;
    buffer.fill(guard);
    // 24-byte planes hold whole 2/3/4/8-byte samples, so only the format
    // contract can refuse these requests.
    const auto left = std::as_writable_bytes(std::span(buffer).first(24));
    const auto right = std::as_writable_bytes(std::span(buffer).subspan(32, 24));
    const auto refuse = [&](ASIOSampleType type, std::size_t oracle_bytes) {
        const auto output = wavplay::asio::to_sample_format(type).value();
        expect("oracle container bytes", output.bytes(), oracle_bytes);
        expect_throw<std::invalid_argument>("lossy output is refused before reading", [&] {
            (void)reader.read_planar(output, left, right);
        });
        expect("refusal preserves the input position", reader.remaining_frames(), 1);
    };
    for (const auto& output : integer_output_cases) {
        if (floating || output.valid_bits < source_bits) { refuse(output.type, output.bytes); }
    }
    for (const auto& output : float_output_cases) {
        if (!floating && source_bits == 32 && output.bytes == 4) { refuse(output.type, output.bytes); }
    }
    for (const auto byte : buffer) { expect("refusal preserves both planes", byte, guard); }
    const auto bytes = lossless.bytes();
    expect("lossless read after refusals", reader.read_planar(lossless, left.first(bytes), right.first(bytes)), 1);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const bool written = i < bytes || (i >= 32 && i < 32 + bytes);
        expect("silence after refusals and surrounding guards", buffer[i], written ? 0 : guard);
    }
}

void test_lossy_outputs_refused()
{
    using namespace wavplay::audio;
    // Even zero fits a narrower destination, but precision is a format contract.
    check_lossy_outputs_refused(wave({pcm24(2, 48000), {"data", Bytes(6, 0)}}), 24, false, int24_le);
    check_lossy_outputs_refused(wave({pcm32(2, 48000), {"data", Bytes(8, 0)}}), 32, false, int32_le);
    const auto float64_msb = wavplay::asio::to_sample_format(ASIOSTFloat64MSB).value();
    for (const bool extensible : {false, true}) {
        // +0 fits any integer numerically, but the format is still incompatible.
        const auto fmt = extensible ? extensible_float32(2, 48000, 3) : float32(2, 48000);
        check_lossy_outputs_refused(wave({fmt, {"data", Bytes(8, 0)}}), 32, true, float64_msb);
        check_lossy_outputs_refused(wave({fmt, fact(1), {"data", Bytes(8, 0)}}), 32, true, float64_msb);
    }
}

void test_extensible_reader()
{
    using namespace wavplay::audio;
    for (const auto sample : {int16_le, int24_le, int24_in32_le, int32_le}) {
        const auto valid = sample.valid_bits;
        const auto start = (std::uint64_t{1} << (valid - 1)) - 1;
        const auto mask = (std::uint64_t{1} << valid) - 1;
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            for (const auto rate : supported_sample_rates) {
                for (const auto channel_mask : {0U, channels == 1 ? 4U : 3U}) {
                    TempFile file;
                    const auto data = sample == int24_in32_le ? pcm24_in32_ramp(channels, 3, start) :
                        valid == 16 ? pcm16_ramp(channels, 3, start) :
                        valid == 24 ? pcm24_ramp(channels, 3, start) : pcm32_ramp(channels, 3, start);
                    const auto* path = file.write(wave({{"JUNK", {1, 2, 3}}, data,
                        extensible_pcm(channels, rate, valid, static_cast<std::uint8_t>(8 * sample.bytes()), channel_mask)}));
                    wavplay::media::WavReader reader(path);
                    expect("Extensible rate", reader.info().format.sample_rate, rate);
                    expect("Extensible channels", reader.info().format.channels, channels);
                    expect("Extensible sample descriptor", static_cast<int>(reader.info().format.sample == sample), 1);
                    expect("Extensible frame count", reader.info().frame_count, 3);
                    expect("Extensible data before fmt", reader.info().data_offset, 32);
                    for (const auto target : integer_output_cases) {
                        const SampleFormat output{SampleKind::integer, target.bytes, target.valid_bits,
                            target.big_endian ? std::endian::big : std::endian::little,
                            target.valid_bits == 8 * target.bytes ? BitAlignment::msb : BitAlignment::lsb};
                        auto cursor = reader.reopen();
                        std::array<std::byte, 16> left, right;
                        left.fill(std::byte{0xa5});
                        right.fill(std::byte{0xa5});
                        if (target.valid_bits < valid) {
                            expect_throw<std::invalid_argument>("Extensible refuses precision loss before reading", [&] {
                                (void)cursor.read_planar(output, std::span(left).first(12), std::span(right).first(12));
                            });
                            expect("Extensible precision rejection preserves position", cursor.remaining_frames(), 3);
                            for (std::size_t byte = 0; byte < left.size(); ++byte) {
                                expect("Extensible rejection preserves left", static_cast<unsigned>(left[byte]), 0xa5);
                                expect("Extensible rejection preserves right", static_cast<unsigned>(right[byte]), 0xa5);
                            }
                            continue;
                        }
                        const auto count = 3 * output.bytes();
                        expect("Extensible decoded frames", cursor.read_planar(output,
                            std::span(left).subspan(1, count), std::span(right).subspan(1, count)), 3);
                        for (std::size_t byte = 0; byte < left.size(); ++byte) {
                            unsigned expected_left = 0xa5, expected_right = 0xa5;
                            if (byte > 0 && byte <= count) {
                                const auto frame = (byte - 1) / output.bytes();
                                const auto bits = (start + frame) & mask;
                                const auto other = channels == 1 ? bits : ~bits & mask;
                                std::array<std::uint8_t, 4> packed_left{}, packed_right{};
                                for (unsigned index = 0; index < valid / 8U; ++index) {
                                    packed_left[index] = static_cast<std::uint8_t>(bits >> (8 * index));
                                    packed_right[index] = static_cast<std::uint8_t>(other >> (8 * index));
                                }
                                const auto index = (byte - 1) % output.bytes();
                                expected_left = expected_integer_sample(packed_left.data(), valid, target)[index];
                                expected_right = expected_integer_sample(packed_right.data(), valid, target)[index];
                            }
                            expect("Extensible left value and guards", static_cast<unsigned>(left[byte]), expected_left);
                            expect("Extensible right value and guards", static_cast<unsigned>(right[byte]), expected_right);
                        }
                        expect("Extensible EOF", cursor.read_planar(output,
                            std::span(left).subspan(1, count), std::span(right).subspan(1, count)), 0);
                        cursor.seek_frame(2);
                        expect("Extensible backward seek", cursor.read_planar(output,
                            std::span(left).subspan(1, output.bytes()), std::span(right).subspan(1, output.bytes())), 1);
                        expect("Extensible original cursor retained", reader.remaining_frames(), 3);
                    }
                }
            }
        }
    }
}

void test_extensible_validated_reopen()
{
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        TempFile file;
        constexpr std::size_t frames = 4099;
        const auto* path = file.write(wave({extensible_pcm(channels, 48000, 24, 32), pcm24_in32_ramp(channels, frames, 0x7fffff)}));
        {
            wavplay::media::WavReader original(path);
            expect("Extensible scanned file denies writing", probe_file_access(path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
            expect("Extensible scanned file denies deletion", probe_file_access(path, DELETE), ERROR_SHARING_VIOLATION);
            const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open Extensible validation lock");
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
            const auto data = static_cast<DWORD>(original.info().data_offset);
            const auto last = data + static_cast<DWORD>((frames - 1) * channels * 4);
            // Header and late PCM are inaccessible to reads after validation.
            // Reopen must reuse both validations, while later PCM I/O still fails.
            if (!LockFile(lock.get(), 0, 0, data, 0) || !LockFile(lock.get(), last, 0, 4, 0)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock validated Extensible data");
            }
            auto cursor = original.reopen();
            expect("Extensible reopen retains source format",
                static_cast<int>(cursor.info().format.sample == wavplay::audio::int24_in32_le), 1);
            expect("Extensible reopen retains total frames", cursor.remaining_frames(), frames);
            std::array<std::byte, 4> left{}, right{};
            expect("Extensible reopened first frame", cursor.read_planar(wavplay::audio::int32_le, left, right), 1);
            expect("Extensible reopened first high byte", static_cast<unsigned>(left[3]), 0x7f);
            expect("Extensible reopened cursor is independent", original.remaining_frames(), frames);
            cursor.seek_frame(frames - 1);
            expect_throw<std::system_error>("Extensible reopened data I/O remains checked", [&] {
                (void)cursor.read_planar(wavplay::audio::int32_le, left, right);
            });
            expect_throw<std::logic_error>("Extensible failed cursor cannot reopen", [&] { (void)cursor.reopen(); });
            expect("Extensible original survives reopened failure", original.read_planar(wavplay::audio::int32_le, left, right), 1);
        }
        expect("Extensible handles released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    }
}

void test_extensible_unused_bit_boundaries()
{
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        // Empty files and every short tail leave valid high bits untouched.
        // Varying the total length also catches a scan of stale staging bytes.
        for (std::size_t frames = 0; frames <= 9; ++frames) {
            const auto data = pcm24_in32_ramp(channels, frames, 0x7ffffc);
            TempFile file;
            const wavplay::media::WavReader reader(file.write(wave({extensible_pcm(channels, 48000, 24, 32), data})));
            expect("short unused-bit validation preserves frame count", reader.remaining_frames(), frames);
            if (frames == 0) { continue; }
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto invalid = data;
                invalid.payload[invalid.payload.size() - 4] = static_cast<std::uint8_t>(1U << bit);
                expect_wav_rejected("each unused tail bit is rejected",
                    wave({extensible_pcm(channels, 48000, 24, 32), invalid}));
            }
        }
        // Check every lane's mask independently; no other low byte is nonzero.
        const auto data = pcm24_in32_ramp(channels, 8, 0x7ffffc);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto invalid = data;
                invalid.payload[4 * lane] = static_cast<std::uint8_t>(1U << bit);
                expect_wav_rejected("each unused vector lane bit is rejected",
                    wave({extensible_pcm(channels, 48000, 24, 32), invalid}));
            }
        }
    }
}

// The scan reads validation_bytes per ReadFile. A violation in the last sample of
// the first read or the first sample of the second, or anywhere in a 1-3 frame
// tail after a full read (1-3 mono or 2-6 stereo samples), is still rejected.
void test_extensible_validation_reads()
{
    using wavplay::media::WavReader;
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto read_frames = WavReader::validation_bytes / (4 * std::size_t{channels});
        for (const auto tail : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
            const auto frames = read_frames + tail;
            const auto data = pcm24_in32_ramp(channels, frames, 0x7ffff0);
            {
                TempFile file;
                WavReader reader(file.write(wave({extensible_pcm(channels, 48000, 24, 32), data})));
                expect("scan across validation reads accepts zero unused bits", reader.remaining_frames(), frames);
                reader.seek_frame(frames - 1);
                std::array<std::byte, 4> left{}, right{};
                expect("final frame after a multi-read scan", reader.read_planar(wavplay::audio::int32_le, left, right), 1);
                const auto final_frame = (frames - 1) * channels * 4;
                for (std::size_t byte = 0; byte < left.size(); ++byte) {
                    expect("final left container", static_cast<unsigned>(left[byte]), data.payload[final_frame + byte]);
                }
            }
            for (const auto& [offset, bit] : {std::pair{WavReader::validation_bytes - 4, 0U},
                std::pair{WavReader::validation_bytes, 7U}, std::pair{data.payload.size() - 4, 3U}}) {
                auto invalid = data;
                invalid.payload[offset] = static_cast<std::uint8_t>(1U << bit);
                expect_wav_rejected("unused bits at a validation read boundary or in a short tail",
                    wave({extensible_pcm(channels, 48000, 24, 32), invalid}));
            }
        }
    }
}

// The scope adds only SEM_FAILCRITICALERRORS to the caller's mode and nested
// scopes restore in turn; successful or failed opens, reopens and reads leave the
// caller's thread error mode as it was. Whether the product's file access runs
// inside a scope is not observable here; it rests on code review and the manual
// no-media check (test specification). Runs on a dedicated thread (run_timed),
// so the modes set here never reach other cases.
void test_thread_error_mode_restored()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3)}));
    // Keep the reserved temporary name alive so that the missing path stays unused.
    const auto missing = std::wstring(path) + L".missing";
    for (const DWORD mode : {DWORD{0}, DWORD{SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX}, DWORD{SEM_FAILCRITICALERRORS}}) {
        if (!SetThreadErrorMode(mode, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetThreadErrorMode");
        }
        {
            const wavplay::core::CriticalErrorScope outer;
            expect("scope adds only SEM_FAILCRITICALERRORS", GetThreadErrorMode(), mode | SEM_FAILCRITICALERRORS);
            {
                const wavplay::core::CriticalErrorScope inner;
                expect("nested scope keeps the flag", GetThreadErrorMode(), mode | SEM_FAILCRITICALERRORS);
            }
            expect("nested scope restores the outer mode", GetThreadErrorMode(), mode | SEM_FAILCRITICALERRORS);
        }
        expect("scope restores the caller's mode", GetThreadErrorMode(), mode);
        {
            wavplay::media::WavReader reader(path);
            expect("open and validation restore the mode", GetThreadErrorMode(), mode);
            auto cursor = reader.reopen();
            std::array<std::int16_t, 3> left{}, right{};
            expect("reopened read", read_pcm16(cursor, left, right), 3);
            expect("reopen and read restore the mode", GetThreadErrorMode(), mode);
        }
        expect_throw<std::system_error>("missing WAV", [&] { const wavplay::media::WavReader reader(missing.c_str()); });
        expect("failed open restores the mode", GetThreadErrorMode(), mode);
    }
}

// A positional read that reaches the end of the file before its buffer is full
// is a format error, "Unexpected end of file", never an I/O failure. Plain
// expect_throw<std::runtime_error> would also accept std::system_error.
void expect_unexpected_end(wavplay::media::ReadOnlyFile& source, std::uint64_t offset, std::span<std::byte> buffer)
{
    const auto read = "read of " + std::to_string(buffer.size()) + " bytes at " + std::to_string(offset);
    try {
        source.read_exact_at(offset, buffer);
    } catch (const std::system_error& error) {
        fail(read + ": an I/O failure instead of an unexpected end of file: " + error.what());
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "Unexpected end of file") {
            fail(read + ": unexpected text: " + error.what());
        }
        return;
    }
    fail(read + ": succeeded past the end of the file");
}

// Reads that fail inside a positional read's scope (an I/O failure, an end of
// file) and a refused validation after it also leave the caller's mode as it
// was, so a set/restore pair that a throw skips would fail here. Runs on a
// dedicated thread (run_timed), as above.
void test_thread_error_mode_restored_on_failure()
{
    TempFile file, short_file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3)}));
    const auto* short_path = short_file.write(Bytes{'A', 'B', 'C', 'D'});
    auto invalid = pcm24_in32_ramp(2, 4, 0x7ffff0);
    invalid.payload[4] = 1; // An unused low bit of the second sample.
    for (const DWORD mode : {DWORD{0}, DWORD{SEM_NOGPFAULTERRORBOX}}) {
        if (!SetThreadErrorMode(mode, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetThreadErrorMode");
        }
        {
            wavplay::media::WavReader reader(path);
            const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open read failure lock");
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
            if (!LockFile(lock.get(), static_cast<DWORD>(reader.info().data_offset), 0, 4, 0)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock reader frame");
            }
            std::array<std::int16_t, 1> left{}, right{};
            expect_throw<std::system_error>("locked read", [&] { (void)read_pcm16(reader, left, right); });
            expect("a failed read restores the mode", GetThreadErrorMode(), mode);
        }
        {
            wavplay::media::ReadOnlyFile source(short_path);
            std::array<std::byte, 8> bytes{};
            expect_unexpected_end(source, 0, bytes);
            expect("a read across the end restores the mode", GetThreadErrorMode(), mode);
        }
        expect_wav_rejected("unused bit refused", wave({extensible_pcm(2, 48000, 24, 32), invalid}));
        expect("a refused validation restores the mode", GetThreadErrorMode(), mode);
    }
}

// This process's file read requests so far. The counter is process-wide; the
// wav_reader suite reads files only on the thread that runs the case.
std::uint64_t read_operations()
{
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessIoCounters");
    }
    return counters.ReadOperationCount;
}

void test_planar_arithmetic_boundaries()
{
    TempFile file;
    wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4)})));
    const auto converter = wavplay::audio::find_planar_converter(reader.info().format, wavplay::audio::int24_le).value();
    std::array<std::byte, 64> planes;
    planes.fill(std::byte{0xa5});
    const auto untouched = planes;
    const wavplay::audio::PlanarBlocks target{.left = planes.data(), .right = planes.data() + 32,
        .block_frames = 2, .stride = 8, .frames = 4};
    const auto reject_layout = [&](const char* name, const wavplay::audio::PlanarBlocks& layout,
        std::string_view reason) {
        const auto before = read_operations();
        try {
            (void)reader.read_planar(converter, layout);
        } catch (const std::invalid_argument& error) {
            if (error.what() != reason) { fail(std::string(name) + ": unexpected rejection: " + error.what()); }
            expect("rejected layout performs no file reads", read_operations(), before);
            expect("rejected layout preserves the cursor", reader.remaining_frames(), 4);
            expect_true("rejected layout preserves every destination byte", planes == untouched);
            return;
        }
        fail(std::string(name) + ": invalid layout was accepted");
    };
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    auto layout = target;
    layout.block_frames = max_size / 3 + 1;
    layout.first_frame = max_size;
    // The full block needs 2^64 + 2 bytes. Its wrapped low part would fit the
    // stride, but the block error must win even over the bad frame range.
    reject_layout("full-block product overflows before the frame range", layout,
        "Planar blocks must hold whole samples");
    layout = target;
    layout.block_frames = max_size / 3;
    layout.stride = max_size - 1;
    reject_layout("maximum nonoverflowing block product exceeds the stride", layout,
        "Planar blocks must hold whole samples");
    layout = target;
    layout.block_frames = 1;
    layout.stride = max_size / 2 + 1;
    layout.first_frame = 2;
    layout.frames = 1;
    reject_layout("block offset wraps to zero", layout, "Planar byte range overflows");
    layout.first_frame = 1;
    layout.stride = max_size - 2;
    reject_layout("last three-byte sample exceeds the byte range by one", layout,
        "Planar byte range overflows");
    layout.stride = max_size - 3;
    // An extent exactly equal to SIZE_MAX is arithmetically valid. A nonnull
    // base then fails the later address check, before any pointer is formed.
    reject_layout("maximum byte extent reaches the address check", layout, "Planar address range overflows");

    const wavplay::audio::PlanarBlocks empty{.block_frames = max_size, .stride = max_size,
        .first_frame = max_size};
    const auto before_empty = read_operations();
    expect("empty layout ignores otherwise invalid dimensions", reader.read_planar(converter, empty), 0);
    expect("empty layout performs no file reads", read_operations(), before_empty);
    expect("empty layout preserves the cursor", reader.remaining_frames(), 4);

    // Only the requested prefix needs storage. A full block of SIZE_MAX bytes
    // is valid here: the one three-byte sample stays inside each small plane.
    layout = target;
    layout.block_frames = max_size / 3;
    layout.stride = max_size;
    layout.frames = 1;
    expect("maximum full-block product permits a partial read", reader.read_planar(converter, layout), 1);
    auto expected = untouched;
    expected[0] = expected[1] = expected[2] = expected[32] = std::byte{0};
    expected[33] = expected[34] = std::byte{0xff};
    expect_true("partial read preserves the first stereo samples and every guard", planes == expected);
    expect("only the valid partial read advances the cursor", reader.remaining_frames(), 3);
}

// The read-size contract by ReadFile count. Scanning 1 MiB of Extensible
// PCM24/32 takes 16 reads of 64 KiB plus the header's few (8 KiB reads would
// take 128, 128 KiB reads 8 plus the header's). Reading 64 KiB of PCM16 for
// playback takes 8 reads of 8 KiB (64 KiB staging would take 1, 4 KiB 16).
void test_read_request_sizes()
{
    constexpr std::size_t scanned_frames = (std::size_t{1} << 20) / 8; // Stereo 32-bit containers.
    TempFile scanned;
    const auto* scanned_path = scanned.write(wave({extensible_pcm(2, 48000, 24, 32),
        pcm24_in32_ramp(2, scanned_frames, 0x7ffff0)}));
    const auto before_scan = read_operations();
    const wavplay::media::WavReader validated(scanned_path);
    const auto scan = read_operations() - before_scan;
    expect("scanned frames", validated.remaining_frames(), scanned_frames);
    expect_true("a 1 MiB scan reads 64 KiB per request (16 to 32 reads), got " + std::to_string(scan),
        scan >= 16 && scan <= 32);

    constexpr std::size_t played_frames = (std::size_t{64} << 10) / 4; // Stereo PCM16.
    TempFile played;
    wavplay::media::WavReader reader(played.write(wave({pcm16(2, 48000), pcm16_ramp(2, played_frames)})));
    std::vector<std::int16_t> left(played_frames), right(played_frames);
    const auto before_playback = read_operations();
    expect("64 KiB of PCM16 read", read_pcm16(reader, left, right), played_frames);
    const auto playback = read_operations() - before_playback;
    expect_true("64 KiB of playback reads at most 8 KiB per request (8 to 12 reads), got " + std::to_string(playback),
        playback >= 8 && playback <= 12);

    reader.seek_frame(0);
    std::array<std::byte, wavplay::media::WavReader::staging_bytes> staging;
    const auto converter = *wavplay::audio::find_planar_converter(reader.info().format, wavplay::audio::int16_le);
    const wavplay::audio::PlanarBlocks target{.left = std::as_writable_bytes(std::span(left)).data(),
        .right = std::as_writable_bytes(std::span(right)).data(), .block_frames = played_frames,
        .stride = played_frames * sizeof(std::int16_t), .frames = played_frames};
    const auto before_borrowed = read_operations();
    expect("64 KiB of PCM16 read with borrowed staging", reader.read_planar(converter, target, staging), played_frames);
    const auto borrowed = read_operations() - before_borrowed;
    expect_true("borrowed staging keeps the 8 KiB read limit (8 to 12 reads), got " + std::to_string(borrowed),
        borrowed >= 8 && borrowed <= 12);
}

void test_borrowed_staging_lifetime()
{
    using wavplay::media::WavReader;
    constexpr auto batch_frames = WavReader::staging_bytes / 4;
    constexpr auto total = 3 * batch_frames + 13;
    TempFile stereo_file, mono_file;
    const auto* stereo_path = stereo_file.write(wave({pcm16(2, 48000), pcm16_ramp(2, total)}));
    WavReader stereo(stereo_path);
    WavReader mono(mono_file.write(wave({pcm16(1, 48000), pcm16_ramp(1, 13, 32760)})));
    // Deliberately unaligned, with guards: the borrowed view promises a size and
    // lifetime, not alignment. A single scratch area serves independent readers.
    std::vector<std::byte> scratch(WavReader::staging_bytes + 2, std::byte{0xa5});
    const WavReader::StagingBuffer staging(scratch.data() + 1, WavReader::staging_bytes);
    const auto check_read = [&](WavReader& reader, std::size_t start, std::size_t requested, std::size_t expected) {
        constexpr std::uint16_t guard = 0xa55a;
        std::vector<std::uint16_t> left(requested + 2, guard), right(requested + 2, guard);
        const auto converter = *wavplay::audio::find_planar_converter(reader.info().format, wavplay::audio::int16_le);
        const wavplay::audio::PlanarBlocks target{
            .left = std::as_writable_bytes(std::span(left).subspan(1, requested)).data(),
            .right = std::as_writable_bytes(std::span(right).subspan(1, requested)).data(),
            .block_frames = requested, .stride = requested * sizeof(std::uint16_t), .frames = requested};
        expect("borrowed staging frame count", reader.read_planar(converter, target, staging), expected);
        for (std::size_t i = 0; i < requested + 2; ++i) {
            const auto valid = i > 0 && i <= expected;
            const auto sample = static_cast<std::uint16_t>(start + i - 1);
            expect("borrowed staging left samples and guards", left[i], valid ? sample : guard);
            expect("borrowed staging right samples and guards", right[i], valid ?
                (reader.info().format.channels == 1 ? sample : static_cast<std::uint16_t>(~sample)) : guard);
        }
        expect("borrowed staging prefix guard", scratch.front(), std::byte{0xa5});
        expect("borrowed staging suffix guard", scratch.back(), std::byte{0xa5});
    };
    check_read(stereo, 0, batch_frames + 7, batch_frames + 7);
    check_read(mono, 32760, 14, 13);
    check_read(stereo, batch_frames + 7, total - batch_frames - 6, total - batch_frames - 7);
    check_read(stereo, total, 1, 0);

    auto failed = stereo.reopen();
    const auto handle = CreateFileW(stereo_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open borrowed staging failure lock");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    lock_rf64_bytes(lock.get(), failed.info().data_offset + WavReader::staging_bytes, 4);
    expect_throw<std::system_error>("borrowed staging failure after a completed batch", [&] {
        check_read(failed, 0, batch_frames + 1, batch_frames + 1);
    });
    expect_throw<std::logic_error>("borrowed staging failure closes the reader", [&] { failed.seek_frame(0); });
    mono.seek_frame(0);
    check_read(mono, 32760, 14, 13); // A failed reader does not own or poison the scratch.
}

void check_float_reader(std::uint8_t channels, std::uint32_t rate, const Chunk& fmt, std::size_t frames)
{
    using namespace wavplay::audio;
    TempFile file;
    const auto source = float32_ramp(channels, frames);
    wavplay::media::WavReader original(file.write(wave({{"JUNK", {1, 2, 3}}, source, fmt, fact(static_cast<std::uint32_t>(frames))})));
    expect("Float32 descriptor", static_cast<int>(original.info().format.sample == float32_le), 1);
    expect("Float32 channels", original.info().format.channels, channels);
    expect("Float32 rate", original.info().format.sample_rate, rate);
    expect("Float32 frame count", original.info().frame_count, frames);
    for (const auto target : float_output_cases) {
        const SampleFormat output{SampleKind::floating_point, target.bytes, static_cast<std::uint8_t>(8 * target.bytes),
            target.big_endian ? std::endian::big : std::endian::little};
        auto cursor = original.reopen();
        const auto count = frames * target.bytes;
        Bytes left(count + target.bytes + 2, 0xa5), right(count + target.bytes + 4, 0xa5);
        const auto left_span = std::as_writable_bytes(std::span(left).subspan(1, count + target.bytes));
        const auto right_span = std::as_writable_bytes(std::span(right).subspan(3, count + target.bytes));
        expect("Float32 requested frames and short EOF", cursor.read_planar(output, left_span, right_span), frames);
        expect("Float32 EOF", cursor.read_planar(output, left_span, right_span), 0);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto& actual = channel == 0 ? left : right;
            const std::size_t prefix = channel == 0 ? 1 : 3;
            for (std::size_t byte = 0; byte < actual.size(); ++byte) {
                unsigned expected = 0xa5;
                if (byte >= prefix && byte < prefix + count) {
                    const auto frame = (byte - prefix) / target.bytes;
                    const auto source_offset = (frame * channels + (channels == 1 ? 0 : channel)) * 4;
                    expected = expected_float_sample(source.payload.data() + source_offset, 32, target, true)[(byte - prefix) % target.bytes];
                }
                if (actual[byte] != expected) { expect("Float32 bit pattern and output guards", actual[byte], expected); }
            }
        }
        cursor.seek_frame(1); // Negative zero, including its sign in widened output.
        expect("Float32 seek", cursor.read_planar(output, left_span.first(target.bytes), right_span.first(target.bytes)), 1);
        const auto expected = expected_float_sample(source.payload.data() + channels * 4, 32, target, true);
        for (std::size_t byte = 0; byte < target.bytes; ++byte) { expect("Float32 sought signed zero", left[1 + byte], expected[byte]); }
        expect("Float32 reopened cursors are independent", original.remaining_frames(), frames);
    }
}

void test_float_readers()
{
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const auto rate : wavplay::audio::supported_sample_rates) {
            check_float_reader(channels, rate, float32(channels, rate, false), 19);
            check_float_reader(channels, rate, float32(channels, rate), 19);
            check_float_reader(channels, rate, extensible_float32(channels, rate), 19);
            check_float_reader(channels, rate, extensible_float32(channels, rate, channels == 1 ? 4 : 3), 19);
        }
        for (const bool extensible : {false, true}) {
            check_float_reader(channels, 48000, extensible ? extensible_float32(channels, 48000) : float32(channels, 48000), 4099);
        }
    }
    for (const auto extension : {22U, 24U}) {
        for (const auto trailing : {0U, 1U, 3U}) {
            auto fmt = extensible_float32(2, 48000, 3);
            fmt.payload[16] = static_cast<std::uint8_t>(extension);
            fmt.payload.resize(18 + extension + trailing, 0xa5);
            check_float_reader(2, 48000, fmt, 19);
        }
    }
}

void test_float_reopen_and_failure()
{
    for (const bool extensible : {false, true}) {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            TempFile file;
            const auto fmt = extensible ? extensible_float32(channels, 48000) : float32(channels, 48000);
            constexpr std::size_t frames = 4099;
            const auto* path = file.write(wave({fmt, fact(static_cast<std::uint32_t>(frames)), float32_ramp(channels, frames)}));
            {
                wavplay::media::WavReader original(path);
                expect("Float32 protects against writes", probe_file_access(path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
                expect("Float32 protects against deletion", probe_file_access(path, DELETE), ERROR_SHARING_VIOLATION);
                const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle == INVALID_HANDLE_VALUE) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open Float32 read lock");
                }
                const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
                const auto data = static_cast<DWORD>(original.info().data_offset);
                const auto final_sample = data + static_cast<DWORD>((frames - 1) * channels * 4);
                if (!LockFile(lock.get(), 0, 0, data, 0) || !LockFile(lock.get(), final_sample, 0, 4, 0)) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock Float32 metadata and final sample");
                }
                auto cursor = original.reopen();
                std::array<std::byte, 8> left{}, right{};
                const auto output = *wavplay::asio::to_sample_format(ASIOSTFloat64MSB);
                expect("Float32 reuses protected fmt and fact metadata", cursor.read_planar(output, left, right), 1);
                expect("Float32 independent cursor", original.remaining_frames(), frames);
                cursor.seek_frame(frames - 1);
                expect_throw<std::system_error>("Float32 later I/O failure", [&] { (void)cursor.read_planar(output, left, right); });
                expect_throw<std::logic_error>("Float32 failed reader cannot reopen", [&] { (void)cursor.reopen(); });
                expect_throw<std::logic_error>("Float32 failed reader cannot seek", [&] { cursor.seek_frame(0); });
                auto batch = original.reopen();
                std::vector<std::byte> many_left(frames * 8), many_right(frames * 8);
                expect_throw<std::system_error>("Float32 failure after several staging reads", [&] {
                    (void)batch.read_planar(output, many_left, many_right);
                });
                expect_throw<std::logic_error>("Float32 partially read cursor is closed", [&] { (void)batch.reopen(); });
                expect("Float32 original survives independent cursor failure", original.read_planar(output, left, right), 1);
            }
            expect("Float32 all handles released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        }
    }
}

void test_rf64_large_data_and_seeks()
{
    constexpr auto boundary = std::uint64_t{1} << 32;
    constexpr auto frames = boundary + 5;
    for (const auto source : rf64_sources) {
        // This representation requires a complete payload scan. Its large offset
        // and full scan are tested below without reading many GiB of sparse zeros.
        if (source.sample == wavplay::audio::int24_in32_le) { continue; }
        const std::uint8_t channels = source.extensible ? 2 : 1;
        const auto frame_bytes = channels * source.sample.bytes();
        auto header = rf64({rf64_format(source, channels, 48000), fact(0xffffffffU), {"data", {}}}, frames);
        const auto data_offset = static_cast<std::uint64_t>(header.size());
        const auto data_size = frames * frame_bytes;
        const auto logical_size = data_offset + data_size + (data_size & 1);
        set_rf64_u64(header, 20, logical_size - 8);
        set_rf64_u64(header, 28, data_size);
        const auto byte_crossing = (boundary - data_offset) / frame_bytes - 1;
        const auto first = rf64_samples(source, channels, 2, 13);
        const auto at_byte = rf64_samples(source, channels, 3, 0x7ffffffe);
        const auto at_frame = rf64_samples(source, channels, 3, 0x7fffffff);
        const auto last = rf64_samples(source, channels, 3, 7);
        TempFile file;
        const auto* path = file.write_sparse(logical_size, {{0, header}, {data_offset, first.payload},
            {data_offset + byte_crossing * frame_bytes, at_byte.payload},
            {data_offset + (boundary - 1) * frame_bytes, at_frame.payload},
            {data_offset + (frames - 3) * frame_bytes, last.payload}});
        {
            wavplay::media::WavReader reader(path);
            expect("RF64 frame count exceeds 32 bits", reader.info().frame_count, frames);
            expect("RF64 sparse data offset", reader.info().data_offset, data_offset);
            check_rf64_read(reader, first, source, channels);
            reader.seek_frame(byte_crossing);
            check_rf64_read(reader, at_byte, source, channels);
            reader.seek_frame(boundary - 1);
            check_rf64_read(reader, at_frame, source, channels);
            expect("RF64 cursor crosses 32-bit frame number", reader.remaining_frames(), 3);
            expect_throw<std::out_of_range>("RF64 seek beyond 64-bit EOF", [&] { reader.seek_frame(frames + 1); });
            expect_throw<std::out_of_range>("RF64 seek arithmetic cannot overflow", [&] {
                reader.seek_frame(std::numeric_limits<std::uint64_t>::max());
            });
            expect("RF64 refused seeks retain position", reader.remaining_frames(), 3);
            check_rf64_read(reader, last, source, channels, true);
            reader.seek_frame(frames);
            expect("RF64 direct seek to large EOF", static_cast<int>(reader.at_end()), 1);
            reader.seek_frame(0);
            const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open RF64 metadata lock");
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
            lock_rf64_bytes(lock.get(), 0, data_offset);
            auto cursor = reader.reopen();
            expect("RF64 reopen keeps 64-bit frame count", cursor.remaining_frames(), frames);
            expect("RF64 reopen reuses protected ds64/fmt/fact", cursor.info().data_offset, data_offset);
            check_rf64_read(cursor, first, source, channels);
            expect("RF64 reopening retains independent original cursor", reader.remaining_frames(), frames);
            const auto last_offset = data_offset + (frames - 1) * frame_bytes;
            lock_rf64_bytes(lock.get(), last_offset, frame_bytes);
            cursor.seek_frame(frames - 1);
            std::array<std::byte, 8> left{}, right{};
            expect_throw<std::system_error>("RF64 high-offset read failure is detected", [&] {
                (void)cursor.read_planar(source.sample, std::span(left).first(source.sample.bytes()),
                    std::span(right).first(source.sample.bytes()));
            });
            expect_throw<std::logic_error>("RF64 failed high-offset reader stays closed", [&] { (void)cursor.reopen(); });
            check_rf64_read(reader, first, source, channels);
            expect("RF64 large file remains write-protected", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);
        }
        expect("RF64 sparse file handles released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    }
}

} // namespace

int wavplay_test::run_wav_reader_tests()
{
    TestSuite suite;
    suite.run("invalid and overflowing planar layouts preserve the reader", test_invalid_conversion_layouts);
    suite.run("planar product and sum limits preserve validation order and avoid I/O", test_planar_arithmetic_boundaries);
    suite.run("a converter chosen for another WAV format is refused", test_foreign_converter);
    suite.run("large stereo read with a short final block", [] { test_large_read(2); });
    suite.run("large mono read with a short final block", [] { test_large_read(1); });
    suite.run("invalid and adjacent channel buffers", test_output_buffers);
    suite.run("WAV handle ownership and moves", test_reader_lifetime);
    suite.run("WAV frame seeks and independent protected reopen", test_seek_and_reopen);
    suite.run("failed WAV reader cannot seek or reopen", test_failed_reader_cannot_seek_or_reopen);
    suite.run("borrowed staging survives reader switches, EOF and terminal I/O failure", test_borrowed_staging_lifetime);
    suite.run("protected WAV metadata reuse and independent read failure", test_reopen_reuses_validated_metadata);
    suite.run("mono PCM16 at supported rates", test_mono);
    suite.run("PCM24 mono/stereo rates, odd data padding, seeks and independent reopen", [] { test_wide_pcm_reader(24); });
    suite.run("PCM32 mono/stereo rates, seeks and independent reopen", [] { test_wide_pcm_reader(32); });
    suite.run("PCM32 multi-staging reads, sign boundary, guards and seek", test_pcm32_large_read);
    suite.run("Extensible PCM supported formats, masks, rates, output bytes and seeks", test_extensible_reader);
    suite.run("Extensible complete validation survives protected independent reopen", test_extensible_validated_reopen);
    suite.run("Extensible unused-bit vector lanes and short tails", test_extensible_unused_bit_boundaries);
    suite.run("Extensible unused-bit scan across 64 KiB validation reads and short tails", test_extensible_validation_reads);
    suite.run("Float32 normal/Extensible formats, masks, all rates, special values and staged reads", test_float_readers);
    suite.run("Float32 protected metadata reuse and independent read failure", test_float_reopen_and_failure);
    suite.run("RF64 sparse large data, 64-bit seek, EOF and protected reopen", test_rf64_large_data_and_seeks);
    const auto check_pair = [](const wavplay::audio::SampleFormat& output, const Bytes& left, const Bytes& right) {
        test_planar_output(2, output, left, right);
        test_planar_output(1, output, left, right);
    };
    suite.run("PCM16 to Int16 LE output", [&] {
        check_pair(wavplay::audio::int16_le, {0x00, 0x80, 0x34, 0x12, 0xff, 0xff},
            {0xff, 0x7f, 0xde, 0xbc, 0x00, 0x00});
    });
    suite.run("PCM16 to packed Int24 LE output", [&] {
        check_pair(wavplay::audio::int24_le, {0x00, 0x00, 0x80, 0x00, 0x34, 0x12, 0x00, 0xff, 0xff},
            {0x00, 0xff, 0x7f, 0x00, 0xde, 0xbc, 0x00, 0x00, 0x00});
    });
    suite.run("PCM16 to Int32 LE output", [&] {
        check_pair(wavplay::audio::int32_le, {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0xff, 0xff},
            {0x00, 0x00, 0xff, 0x7f, 0x00, 0x00, 0xde, 0xbc, 0x00, 0x00, 0x00, 0x00});
    });
    suite.run("partial output samples, unequal and overlapping buffers leave the reader unchanged", test_partial_output_samples);
    suite.run("all 14 integer outputs preserve signed samples across staged reads", test_all_integer_reader_outputs);
    suite.run("PCM24 samples to packed 24-bit output", [] {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            test_planar_output(channels, wavplay::audio::int24_le, {0, 0, 0x80, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff},
                {0xff, 0xff, 0x7f, 0xba, 0xdc, 0xfe, 0, 0, 0}, 24);
        }
    });
    suite.run("PCM24 samples to 32-bit output", [] {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            test_planar_output(channels, wavplay::audio::int32_le, {0, 0, 0, 0x80, 0, 0x56, 0x34, 0x12, 0, 0xff, 0xff, 0xff},
                {0, 0xff, 0xff, 0x7f, 0, 0xba, 0xdc, 0xfe, 0, 0, 0, 0}, 24);
        }
    });
    suite.run("PCM32 samples preserve all bits in 32-bit output", [] {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            test_planar_output(channels, wavplay::audio::int32_le, {0, 0, 0, 0x80, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff},
                {0xff, 0xff, 0xff, 0x7f, 0x98, 0xba, 0xdc, 0xfe, 0, 0, 0, 0}, 32);
            test_large_planar_output(channels, wavplay::audio::int32_le, 4, 32);
        }
    });
    suite.run("PCM24, PCM32 and Float32 refuse every lossy output before reading", test_lossy_outputs_refused);
    suite.run("multi-block PCM24 to packed 24/32-bit output", [] {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            test_large_planar_output(channels, wavplay::audio::int24_le, 3, 24);
            test_large_planar_output(channels, wavplay::audio::int32_le, 4, 24);
        }
    });
    suite.run("multi-block stereo PCM16 to Int16 LE output", [] { test_large_planar_output(2, wavplay::audio::int16_le, 2); });
    suite.run("multi-block stereo PCM16 to packed Int24 LE output", [] { test_large_planar_output(2, wavplay::audio::int24_le, 3); });
    suite.run("multi-block mono PCM16 to packed Int24 LE output", [] { test_large_planar_output(1, wavplay::audio::int24_le, 3); });
    suite.run("multi-block stereo PCM16 to Int32 LE output", [] { test_large_planar_output(2, wavplay::audio::int32_le, 4); });
    suite.run("multi-block mono PCM16 to Int32 LE output", [] { test_large_planar_output(1, wavplay::audio::int32_le, 4); });
    suite.run("packed 24-bit I/O failure requires reopening", [] { test_read_failure(wavplay::audio::int24_le); });
    suite.run("32-bit I/O failure requires reopening", [] { test_read_failure(wavplay::audio::int32_le); });

    const auto reject = [&](const char* name, const Bytes& bytes) {
        suite.run(name, [&] { expect_wav_rejected(name, bytes); });
    };
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto valid_data = pcm24_in32_ramp(channels, 4099, 0x7ffff0);
        for (const auto offset : {std::size_t{0}, std::size_t{4}, wavplay::media::WavReader::staging_bytes - 4,
            wavplay::media::WavReader::staging_bytes, valid_data.payload.size() - 4}) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto invalid_data = valid_data;
                invalid_data.payload[offset] = static_cast<std::uint8_t>(1U << bit);
                reject("Extensible unused bits rejected at first, staging boundary and final samples",
                    wave({extensible_pcm(channels, 48000, 24, 32), invalid_data}));
            }
        }
    }

    suite.run_timed("critical-error scope nests and file access restores the caller's thread error mode",
        test_thread_error_mode_restored);
    suite.run_timed("a failed read, a read across the end and a refused validation restore the caller's thread error mode",
        test_thread_error_mode_restored_on_failure);
    suite.run("validation reads 64 KiB and playback at most 8 KiB per file read request", test_read_request_sizes);
    suite.run("non-disk paths are refused before parsing", [] {
        for (const auto* device : {L"NUL", L"CONIN$"}) {
            expect_throw<std::runtime_error>("device path", [&] { const wavplay::media::WavReader reader(device); });
        }
    });
    suite.run("positional reads refuse offsets beyond signed 64-bit file positions", [] {
        TempFile file;
        wavplay::media::ReadOnlyFile source(file.write(Bytes{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}));
        std::array<std::byte, 4> bytes{};
        // A fresh handle's file pointer is 0. Offset -2 (FILE_USE_FILE_POINTER_POSITION)
        // would read there; other offsets above INT64_MAX would fail as I/O errors.
        for (const auto offset : {~std::uint64_t{1}, std::uint64_t{1} << 63, ~std::uint64_t{0}}) {
            expect_throw<std::invalid_argument>("offset beyond INT64_MAX", [&] { source.read_exact_at(offset, std::span(bytes)); });
        }
        source.read_exact_at(4, std::span(bytes));
        expect("read at the explicit offset", std::to_integer<int>(bytes[0]), int{'E'});
    });
    suite.run("positional reads that cross or start at the end of the file are an unexpected end of file", [] {
        TempFile file;
        wavplay::media::ReadOnlyFile source(file.write(Bytes{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}));
        std::array<std::byte, 8> bytes{};
        // Across the end ReadFile returns 4 of the 8 bytes; at or past the end it
        // fails with ERROR_HANDLE_EOF. Header parsing never asks for either.
        expect_unexpected_end(source, 4, bytes);
        expect_unexpected_end(source, 8, std::span(bytes).first(4));
        expect_unexpected_end(source, 100, std::span(bytes).first(4));
        source.read_exact_at(0, std::span(bytes));
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            expect("whole file after the refused reads", std::to_integer<int>(bytes[index]), 'A' + static_cast<int>(index));
        }
    });
    suite.run("periodic channel payloads reject same-slot and neighbouring-slot aliases", test_interleaved_block_aliases);
    suite.run("interleaved channels preserve split/staging/EOF reads and all gaps", test_interleaved_block_reads);
    suite.run("blocked planes need whole samples and must not overlap", [] {
        TempFile file;
        wavplay::media::WavReader reader(file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4)})));
        const auto converter = wavplay::audio::find_planar_converter({48000, 2, wavplay::audio::int16_le},
            wavplay::audio::int16_le).value();
        std::array<std::byte, 64> planes{};
        planes.fill(std::byte{0xa5});
        const wavplay::audio::PlanarBlocks blocks{.left = planes.data(), .right = planes.data() + 32,
            .block_frames = 2, .stride = 8, .frames = 4};
        auto narrow = blocks;
        narrow.stride = 3; // Two 16-bit samples need four bytes.
        expect_throw<std::invalid_argument>("stride below one block", [&] { (void)reader.read_planar(converter, narrow); });
        auto overlapping = blocks;
        overlapping.right = planes.data() + 8; // The second left block starts here.
        expect_throw<std::invalid_argument>("overlapping planes", [&] { (void)reader.read_planar(converter, overlapping); });
        expect("refused blocks leave the cursor", reader.remaining_frames(), 4);
        expect("frames into gapped blocks", reader.read_planar(converter, blocks), 4);
        // Frames 0 and 1, a 4-byte gap, then frames 2 and 3, in each plane.
        expect("left frame 2 after the gap", static_cast<int>(planes[8]), 2);
        expect("gap start untouched", static_cast<int>(planes[4]), 0xa5);
        expect("gap end untouched", static_cast<int>(planes[7]), 0xa5);
        expect("right frame 3", static_cast<unsigned>(planes[32 + 10]) | static_cast<unsigned>(planes[32 + 11]) << 8, 0xfffcu);
    });

    return suite.result("PCM and Float32 WAV reader");
}
