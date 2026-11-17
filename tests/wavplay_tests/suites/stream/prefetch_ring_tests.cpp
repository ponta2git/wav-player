#include "suites/suites.h"
#include "wavplay/stream/gapless_run.h"
#include "wavplay/stream/prefetch_ring.h"
#include "wavplay/stream/ring_layout.h"
#include "framework/test_suite.h"
#include "fixtures/wav_builder.h"
#include "fixtures/sample_oracle.h"
#include "fixtures/wav_reader_fixture.h"
#include "fixtures/ring_fixture.h"
#include "fixtures/thread_state.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/core/memory.h"
#include "wavplay/core/win32.h"

#include <objbase.h>
#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ntdll.lib") // NtQuerySystemInformation

namespace {

using namespace wavplay;
using namespace wavplay_test;

// At 44.1 kHz each slot holds one second. Four blocks must pass through two slots.
constexpr std::size_t block = 44100;
constexpr std::size_t frames = 3 * block + 3;

void check_sequence(const wchar_t* path, std::size_t width,
    const wchar_t* next = nullptr, std::size_t first_frames = frames)
{
    auto ring = make_ring(path, next, width, audio::int16_le);
    expect("prefetch sample rate", ring.info().format.sample_rate, 44100);
    expect("first WAV frame count", ring.info().frame_count, first_frames);
    std::vector<std::uint16_t> left(width), right(width);
    for (std::size_t start = 0; start < frames; start += width) {
        const auto result = await_block(ring, left.data(), right.data());
        const auto count = std::min(width, frames - start);
        const auto status = start + count == frames ? stream::BlockStatus::last : stream::BlockStatus::ready;
        expect("prefetch status", static_cast<int>(result.status), static_cast<int>(status));
        expect("prefetch valid frames", result.valid_frames, count);
        for (std::size_t i = 0; i < width; ++i) {
            expect("prefetch left", left[i], i < count ? static_cast<std::uint16_t>(start + i) : 0);
            expect("prefetch right", right[i], i < count ? static_cast<std::uint16_t>(~(start + i)) : 0);
        }
    }
}

void expect_refill_failure(const wchar_t* path, std::size_t width, std::size_t good_blocks, const wchar_t* next = nullptr)
{
    auto ring = make_ring(path, next, width, audio::int16_le);
    std::vector<std::uint16_t> left(width), right(width);
    constexpr std::uint16_t poison = 0xa55a;
    for (std::size_t i = 0; i <= good_blocks; ++i) {
        std::ranges::fill(left, poison);
        std::ranges::fill(right, poison);
        const auto result = await_block(ring, left.data(), right.data());
        if (result.status == stream::BlockStatus::read_error) {
            expect("valid frames on first read error", result.valid_frames, 0);
            const auto again = ring.consumer().pop(left.data(), right.data());
            expect("latched read error", static_cast<int>(again.status), static_cast<int>(stream::BlockStatus::read_error));
            expect("valid frames on read error", again.valid_frames, 0);
            expect("left untouched on read error", std::ranges::count(left, poison), width);
            expect("right untouched on read error", std::ranges::count(right, poison), width);
            return;
        }
        expect("buffer before failed refill", static_cast<int>(result.status), static_cast<int>(stream::BlockStatus::ready));
        expect("left before failed refill", left.front(), static_cast<std::uint16_t>(i * width));
        expect("right before failed refill", right.back(), static_cast<std::uint16_t>(~(i * width + width - 1)));
    }
    throw std::runtime_error("prefetch published a block containing a locked PCM frame");
}

void check_packed_sequence(const wchar_t* path, const wchar_t* next)
{
    // A 544-frame block gives 1632 bytes per channel, with a gap between aligned slots.
    constexpr std::size_t width = 544;
    auto ring = make_ring(path, next, width, audio::int24_le);
    expect("packed bytes per channel", ring.block_bytes(), 3 * width);
    std::array<std::uint8_t, 3 * width> left{}, right{};
    auto sample = [](const std::uint8_t* bytes) {
        return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
    };
    for (std::size_t start = 0; start < frames; start += width) {
        const auto result = await_block(ring, left.data(), right.data());
        const auto count = std::min(width, frames - start);
        const auto status = start + count == frames ? stream::BlockStatus::last : stream::BlockStatus::ready;
        expect("packed prefetch status", static_cast<int>(result.status), static_cast<int>(status));
        expect("packed valid frames", result.valid_frames, count);
        for (std::size_t i = 0; i < width; ++i) {
            const auto bits = static_cast<std::uint16_t>(start + i);
            expect("packed left", sample(left.data() + 3 * i), i < count ? bits * 256 : 0);
            expect("packed right", sample(right.data() + 3 * i), i < count ? (bits ^ 0xffff) * 256 : 0);
        }
    }
}

void expect_locked_read(auto operation)
{
    try {
        operation();
    } catch (const std::system_error& error) {
        if (error.code() != std::error_code(ERROR_LOCK_VIOLATION, std::system_category())) {
            throw;
        }
        return;
    }
    throw std::runtime_error("expected initial prefetch to reject locked data");
}

void check_gapless_refill()
{
    TempFile first, second;
    // The seam falls inside a slot after the initial two seconds have wrapped.
    constexpr auto first_frames = 2 * block + 1001;
    const auto* path = first.write(wave({pcm16(2, 44100), pcm16_ramp(2, first_frames)}));
    const auto* next = second.write(wave({pcm16(2, 44100), pcm16_ramp(2, frames - first_frames, first_frames)}));
    check_sequence(path, 96, next, first_frames); // Contiguous planar batches.
    check_sequence(path, 47, next, first_frames); // Reads split across the gaps between aligned slots.
    check_sequence(path, 1, next, first_frames);  // One stereo frame per padded slot.
    check_packed_sequence(path, next);

    // The second header still validates, but its first frame cannot complete
    // the mixed slot. That entire slot must stay private to the failed producer.
    const auto data = static_cast<DWORD>(media::WavReader(next).info().data_offset);
    auto lock = open_lock_handle(next);
    if (!LockFile(lock.get(), data, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock next track PCM frame");
    }
    expect_refill_failure(path, 96, first_frames / 96, next);
    expect_refill_failure(path, 47, first_frames / 47, next);
}

void check_source_batch_sizes()
{
    struct Source { std::uint8_t bits, channels; std::size_t width, blocks; };
    for (const auto& source : {Source{16, 1, 544, 7}, Source{16, 2, 544, 3},
        Source{24, 1, 544, 5}, Source{24, 2, 544, 2}, Source{32, 1, 544, 3}, Source{32, 2, 544, 1},
        Source{16, 2, 8, 256}, Source{24, 2, 8, 170}, Source{32, 2, 8, 128}, Source{32, 2, 4096, 1}}) {
        TempFile file;
        const auto samples = source.bits == 16 ? pcm16_ramp(source.channels, 16384) :
            source.bits == 24 ? pcm24_ramp(source.channels, 16384) : pcm32_ramp(source.channels, 16384);
        auto reader = stream::retain_playable_track(file.write(wave({pcm_integer(source.channels, 48000, source.bits), samples})));
        stream::GaplessRun run(std::move(reader), audio::int32_le, source.width, 8192);
        expect("source bytes determine the preferred batch", run.next_batch_blocks(), source.blocks);
    }
    TempFile file;
    const auto* path = file.write(wave({pcm16(1, 48000), pcm16_ramp(1, 13)}));
    expect_throw<std::invalid_argument>("gapless batch needs a block width", [&] {
        const stream::GaplessRun run(stream::retain_playable_track(path), audio::int32_le, 0, 8192);
    });
    expect_throw<std::invalid_argument>("gapless batch needs a byte budget", [&] {
        const stream::GaplessRun run(stream::retain_playable_track(path), audio::int32_le, 4, 0);
    });
    stream::GaplessRun huge(stream::retain_playable_track(path), audio::int32_le, 4,
        std::numeric_limits<std::size_t>::max());
    expect("a huge source budget is bounded by the track end without overflow", huge.next_batch_blocks(), 4);
}

std::uint64_t source_read_operations()
{
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessIoCounters");
    }
    return counters.ReadOperationCount;
}

void check_format_following_batches(bool narrow_first, bool partial_boundary)
{
    // Mono PCM16 and stereo PCM32 differ fourfold in input bytes per frame.
    // Fixed first-track batching either quarters the later ReadFile requests
    // or delays publication until four reads finish. Both orders are checked.
    constexpr std::size_t width = 8;
    const std::size_t first_frames = partial_boundary ? (narrow_first ? 4099 : 1027) : 8192;
    const std::size_t second_frames = partial_boundary ? (narrow_first ? 2053 : 8197) : 8192;
    const auto first_data = narrow_first ? pcm16_ramp(1, first_frames, 32760) : pcm32_ramp(2, first_frames, 0x7ffffff0);
    const auto second_data = narrow_first ? pcm32_ramp(2, second_frames, 0x7ffffff0) : pcm16_ramp(1, second_frames, 32760);
    TempFile first, second;
    const auto* first_path = first.write(wave({narrow_first ? pcm16(1, 48000) : pcm_integer(2, 48000, 32), first_data}));
    const auto* second_path = second.write(wave({narrow_first ? pcm_integer(2, 48000, 32) : pcm16(1, 48000), second_data}));
    stream::GaplessRun run(stream::retain_playable_track(first_path), audio::int32_le, width, 8192);
    expect_true("a different input width joins the same output run", run.append(stream::retain_playable_track(second_path)));
    std::vector<std::size_t> expected_batches;
    if (partial_boundary) {
        expected_batches = narrow_first ? std::vector<std::size_t>{512, 1, 128, 128} :
            std::vector<std::size_t>{128, 1, 512, 512};
    } else {
        expected_batches.insert(expected_batches.end(), narrow_first ? 2 : 8, narrow_first ? 512 : 128);
        expected_batches.insert(expected_batches.end(), narrow_first ? 8 : 2, narrow_first ? 128 : 512);
    }
    std::array<std::byte, media::WavReader::staging_bytes> staging;
    std::vector<std::uint8_t> left(4096 * 4 + 2), right(left.size());
    constexpr std::uint8_t guard = 0xa5;
    std::size_t position{};
    const auto before_reads = source_read_operations();
    for (const auto expected_blocks : expected_batches) {
        expect("next batch follows this source and its boundary", run.next_batch_blocks(), expected_blocks);
        const auto count = expected_blocks * width;
        std::ranges::fill(left, guard);
        std::ranges::fill(right, guard);
        const audio::PlanarBlocks target{
            .left = reinterpret_cast<std::byte*>(left.data() + 1),
            .right = reinterpret_cast<std::byte*>(right.data() + 1), .block_frames = width,
            .stride = width * 4, .frames = count};
        expect("format-following batch completes whole blocks", run.read(target, staging), count);
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto global = position + frame;
            const bool in_first = global < first_frames;
            const bool narrow = in_first == narrow_first;
            const auto local = in_first ? global : global - first_frames;
            const auto& data = in_first ? first_data : second_data;
            const std::size_t channels = narrow ? 1 : 2;
            const std::size_t sample_bytes = narrow ? 2 : 4;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto* source = data.payload.data() + (local * channels + (narrow ? 0 : channel)) * sample_bytes;
                const auto expected = expected_output_sample(source, static_cast<std::uint8_t>(narrow ? 16 : 32), ASIOSTInt32LSB);
                const auto& actual = channel == 0 ? left : right;
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    expect("mixed-width batch sample", actual[1 + frame * 4 + byte], expected[byte]);
                }
            }
        }
        expect("mixed-width left prefix", left.front(), guard);
        expect("mixed-width right prefix", right.front(), guard);
        expect("mixed-width left suffix", std::ranges::count(std::span(left).subspan(1 + count * 4), guard),
            left.size() - 1 - count * 4);
        expect("mixed-width right suffix", std::ranges::count(std::span(right).subspan(1 + count * 4), guard),
            right.size() - 1 - count * 4);
        position += count;
    }
    const auto reads = source_read_operations() - before_reads;
    // Header reads finished before the counter. Allow two unrelated reads by
    // this process, as in the media suite; fixed wide-first batching takes 16.
    const auto expected_reads = partial_boundary ? 5u : 10u;
    expect_true("format-following ReadFile count, got " + std::to_string(reads),
        reads >= expected_reads && reads <= expected_reads + 2);
    expect("mixed-width run has no omitted frames", position, first_frames + second_frames);
    expect("completed run offers no next batch", run.next_batch_blocks(), 0);
}

void check_short_track_batches()
{
    TempFile mono_file, stereo_file;
    const auto mono_data = pcm16_ramp(1, 1, 32760);
    const auto stereo_data = pcm32_ramp(2, 1, 0x7ffffff0);
    auto mono = stream::retain_playable_track(mono_file.write(wave({pcm16(1, 48000), mono_data})));
    auto stereo = stream::retain_playable_track(stereo_file.write(wave({pcm_integer(2, 48000, 32), stereo_data})));
    stream::GaplessRun run(mono, audio::int32_le, 8, 8192);
    for (std::size_t track = 1; track < 19; ++track) {
        expect_true("short repeated source joins the run", run.append(track % 2 == 0 ? mono : stereo));
    }
    std::array<std::byte, media::WavReader::staging_bytes> staging;
    std::array<std::uint8_t, 32> left{}, right{};
    for (std::size_t first = 0; first < 19; first += 8) {
        expect("short tracks still complete one boundary block", run.next_batch_blocks(), 1);
        std::ranges::fill(left, 0xa5);
        std::ranges::fill(right, 0xa5);
        const auto count = std::min<std::size_t>(8, 19 - first);
        expect("one block crosses several short tracks", run.read({
            .left = reinterpret_cast<std::byte*>(left.data()), .right = reinterpret_cast<std::byte*>(right.data()),
            .block_frames = 8, .stride = 32, .frames = 8}, staging), count);
        for (std::size_t frame = 0; frame < 8; ++frame) {
            const bool narrow = (first + frame) % 2 == 0;
            const auto& data = narrow ? mono_data : stereo_data;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto expected = expected_output_sample(data.payload.data() + (narrow ? 0 : channel * 4),
                    static_cast<std::uint8_t>(narrow ? 16 : 32), ASIOSTInt32LSB);
                const auto& actual = channel == 0 ? left : right;
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    expect("short track samples and untouched final tail", actual[frame * 4 + byte],
                        frame < count ? expected[byte] : 0xa5);
                }
            }
        }
    }
    expect("short track sequence reaches EOF", run.next_batch_blocks(), 0);
}

void check_multiple_wraps()
{
    // Non-power-of-two capacities and wake intervals do not generally divide
    // the source batch. Include a two-slot ring (one wake per copy),
    // and enough audio to wrap three times with an incomplete final batch.
    for (const std::size_t width : {std::size_t{8}, std::size_t{47}, std::size_t{64}, block}) {
        const auto shape = stream::plan_block_shape(width, 2);
        const auto layout = stream::plan_ring_layout(shape, 8 * block, 44100);
        const auto total = 3 * layout.capacity * width + width / 2 + 1;
        TempFile file;
        const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, total)}));
        auto ring = make_ring(path, {}, width, audio::int16_le);
        std::vector<std::uint16_t> left(width + 2, 0xa55a), right(width + 2, 0xa55a);
        for (std::size_t start = 0; start < total; start += width) {
            const auto result = await_block(ring, left.data() + 1, right.data() + 1);
            const auto count = std::min(width, total - start);
            expect("multi-wrap status", static_cast<int>(result.status), static_cast<int>(
                start + count == total ? stream::BlockStatus::last : stream::BlockStatus::ready));
            expect("multi-wrap frames", result.valid_frames, count);
            for (std::size_t i = 0; i < width; ++i) {
                expect("multi-wrap left sample", left[i + 1], i < count ? static_cast<std::uint16_t>(start + i) : 0);
                expect("multi-wrap right sample", right[i + 1], i < count ? static_cast<std::uint16_t>(~(start + i)) : 0);
            }
            for (const auto* plane : {&left, &right}) {
                expect("multi-wrap leading guard", plane->front(), 0xa55a);
                expect("multi-wrap trailing guard", plane->back(), 0xa55a);
            }
        }
        expect("multi-wrap final block is consumed once",
            static_cast<int>(ring.consumer().pop(left.data() + 1, right.data() + 1).status),
            static_cast<int>(stream::BlockStatus::empty));
    }
}

void check_stop_at_ring_boundaries()
{
    constexpr std::size_t width = 47;
    const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, 2), 8 * block, 44100);
    constexpr std::size_t batch_blocks = 43; // 8 KiB of stereo PCM16 in 47-frame blocks.
    expect("batch and wake boundaries are distinct", static_cast<int>(batch_blocks < layout.wake_interval), 1);
    expect("wake threshold splits a source batch", static_cast<int>(layout.wake_interval % batch_blocks != 0), 1);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 8 * block)}));
    for (const auto threshold : {batch_blocks, layout.wake_interval, layout.capacity}) {
        for (const auto count : {threshold - 1, threshold, threshold + 1}) {
            auto ring = make_ring(path, {}, width, audio::int16_le);
            std::array<std::uint16_t, width> left{}, right{};
            for (std::size_t index = 0; index < count; ++index) {
                const auto result = await_block(ring, left.data(), right.data());
                expect("ring-boundary pre-stop status", static_cast<int>(result.status),
                    static_cast<int>(stream::BlockStatus::ready));
                expect("ring-boundary pre-stop left", left.front(), static_cast<std::uint16_t>(index * width));
                expect("ring-boundary pre-stop right", right.back(), static_cast<std::uint16_t>(~(index * width + width - 1)));
            }
            ring.stop();
            ring.stop();
            std::ranges::fill(left, 0xa55a);
            std::ranges::fill(right, 0xa55a);
            const auto result = ring.consumer().pop(left.data(), right.data());
            expect("remaining blocks are revoked on stop", static_cast<int>(result.status), static_cast<int>(stream::BlockStatus::empty));
            expect("stopped ring has no frames", result.valid_frames, 0);
            expect("ring-boundary stop preserves left", std::ranges::count(left, 0xa55a), left.size());
            expect("ring-boundary stop preserves right", std::ranges::count(right, 0xa55a), right.size());
            expect("ring-boundary stop releases the WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        }
    }
}

void check_mono_join()
{
    // Mono plays its samples on both channels and joins a same-rate stereo WAV
    // gaplessly, with the seam inside a slot after the ring has wrapped.
    TempFile mono, stereo;
    constexpr std::size_t first_frames = 2 * block + 17;
    const auto* path = mono.write(wave({pcm16(1, 44100), pcm16_ramp(1, first_frames)}));
    const auto* next = stereo.write(wave({pcm16(2, 44100), pcm16_ramp(2, frames - first_frames, first_frames)}));
    for (const std::size_t width : {std::size_t{96}, std::size_t{47}}) {
        auto ring = make_ring(path, next, width, audio::int16_le);
        expect("mono first WAV channels", ring.info().format.channels, 1);
        std::vector<std::uint16_t> left(width), right(width);
        for (std::size_t start = 0; start < frames; start += width) {
            const auto result = await_block(ring, left.data(), right.data());
            const auto count = std::min(width, frames - start);
            expect("mono join status", static_cast<int>(result.status),
                static_cast<int>(start + count == frames ? stream::BlockStatus::last : stream::BlockStatus::ready));
            expect("mono join valid frames", result.valid_frames, count);
            for (std::size_t i = 0; i < width; ++i) {
                const auto frame = start + i;
                const auto wanted_right = frame < first_frames ? static_cast<std::uint16_t>(frame)
                                                               : static_cast<std::uint16_t>(~frame);
                expect("mono join left", left[i], i < count ? static_cast<std::uint16_t>(frame) : 0);
                expect("mono join right", right[i], i < count ? wanted_right : 0);
            }
        }
    }
}

void check_wide_pcm_refill(std::uint8_t source_bits, bool extensible_first = false, bool extensible_second = false,
    std::initializer_list<ASIOSampleType> output_types = {ASIOSTInt24LSB, ASIOSTInt32LSB},
    bool rf64_first = false, bool rf64_second = false)
{
    constexpr std::size_t first_frames = 2 * block + 2 * 544 + 17;
    const std::size_t source_start = source_bits == 16 ? 0x7ff0 : source_bits == 24 ? 0x7ffff0 : 0x7ffffff0;
    const std::size_t source_bytes = source_bits / 8;
    const std::size_t first_storage_bytes = extensible_first ? 4 : source_bytes;
    constexpr std::uint8_t guard = 0xa5;
    const auto data = [&](std::uint8_t channels, std::size_t count, std::size_t first, bool extensible) {
        if (extensible) { return pcm24_in32_ramp(channels, count, first); }
        return source_bits == 16 ? pcm16_ramp(channels, count, first) :
            source_bits == 24 ? pcm24_ramp(channels, count, first) : pcm32_ramp(channels, count, first);
    };
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        TempFile first, second;
        const auto first_chunks = {extensible_first ? extensible_pcm(channels, 44100, 24, 32) : pcm_integer(channels, 44100, source_bits),
            data(channels, first_frames, source_start, extensible_first)};
        const auto second_chunks = {extensible_second ? extensible_pcm(2, 44100, 24, 32) : pcm_integer(2, 44100, source_bits),
            data(2, frames - first_frames, source_start + first_frames, extensible_second)};
        const auto* path = first.write(rf64_first ? rf64(first_chunks, first_frames) : wave(first_chunks));
        const auto* next = second.write(rf64_second ? rf64(second_chunks, frames - first_frames) : wave(second_chunks));
        for (const auto type : output_types) {
            const auto& oracle = integer_output_case(type);
            if (oracle.valid_bits < source_bits) { continue; }
            const auto output = asio::to_sample_format(type).value();
            expect("oracle container bytes", output.bytes(), oracle.bytes);
            // Eight frames place small left/right payloads in one cache line.
            // 64 is dense for all integer widths; 47 always has gaps. At 544,
            // packed output has a gap while 16/32-bit output remains dense.
            for (const std::size_t width : {8, 64, 47, 544}) {
                auto ring = make_ring(path, next, width, output);
                expect("wide PCM ring pages locked", static_cast<int>(ring.pages_locked()), 1);
                const auto sample_bytes = output.bytes();
                const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, sample_bytes),
                    frames, 44100);
                expect("wide PCM seam follows the initially filled ring",
                    static_cast<int>(layout.capacity * width < first_frames), 1);
                expect("wide PCM seam splits a slot", static_cast<int>(first_frames % width != 0), 1);
                expect("wide PCM source frame width", ring.info().format.frame_bytes(), channels * first_storage_bytes);
                expect("wide PCM output block bytes", ring.block_bytes(), width * sample_bytes);
                Bytes left(ring.block_bytes() + 2, guard), right(left.size(), guard);
                // The run exceeds the two-second ring. Its mono/stereo seam
                // comes after the wrap, inside a slot and an 8 KiB source batch.
                for (std::size_t start = 0; start < frames; start += width) {
                    const auto result = await_block(ring, left.data() + 1, right.data() + 1);
                    const auto count = std::min(width, frames - start);
                    expect("wide PCM refill status", static_cast<int>(result.status), static_cast<int>(
                        start + count == frames ? stream::BlockStatus::last : stream::BlockStatus::ready));
                    expect("wide PCM refill valid frames", result.valid_frames, count);
                    for (std::size_t i = 0; i < width; ++i) {
                        const auto frame = start + i;
                        const auto left_bits = source_start + frame;
                        const auto right_bits = channels == 1 && frame < first_frames ? left_bits : ~left_bits;
                        for (const auto channel : {0, 1}) {
                            std::array<std::uint8_t, 4> canonical{};
                            const auto bits = channel == 0 ? left_bits : right_bits;
                            for (std::size_t byte = 0; byte < source_bytes; ++byte) {
                                canonical[byte] = static_cast<std::uint8_t>(bits >> (8 * byte));
                            }
                            const auto expected = i < count ? expected_integer_sample(canonical.data(), source_bits, oracle) :
                                std::array<std::uint8_t, 4>{};
                            const auto& actual = channel == 0 ? left : right;
                            for (std::size_t byte = 0; byte < sample_bytes; ++byte) {
                                expect("wide PCM refill byte", actual[1 + i * sample_bytes + byte], expected[byte]);
                            }
                        }
                    }
                    for (const auto* channel : {&left, &right}) {
                        expect("wide PCM refill leading guard", channel->front(), guard);
                        expect("wide PCM refill trailing guard", channel->back(), guard);
                    }
                }
                ring.stop();
                expect("wide PCM completed producer has no read failure", static_cast<int>(ring.read_failed()), 0);
                expect("wide PCM first source released after wrap", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
                expect("wide PCM second source released after wrap", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            }
        }
    }
}

enum class FloatRingJoin { floats, pcm16_to_float, float_to_pcm32 };

void check_float_ring(const FloatOutputCase& target, FloatRingJoin join,
    bool rf64_first = false, bool rf64_second = false)
{
    constexpr std::size_t first_frames = 2 * block + 2 * 544 + 17;
    constexpr std::size_t total_frames = first_frames + 2 * 544 + 3;
    constexpr std::uint8_t guard = 0xa5;
    const bool first_float = join != FloatRingJoin::pcm16_to_float;
    const bool second_float = join != FloatRingJoin::float_to_pcm32;
    const auto first_bits = static_cast<std::uint8_t>(first_float ? 32 : 16);
    const audio::SampleFormat output{audio::SampleKind::floating_point, target.bytes,
        static_cast<std::uint8_t>(8 * target.bytes), target.big_endian ? std::endian::big : std::endian::little};
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto first_data = first_float ? float32_ramp(channels, first_frames) : pcm16_ramp(channels, first_frames, 0x7ff0);
        const auto second_data = second_float ? float32_ramp(2, total_frames - first_frames) :
            pcm32_ramp(2, total_frames - first_frames, 0x7ffffff0);
        const auto first_format = first_float ? (target.big_endian ? extensible_float32(channels, 44100) :
            float32(channels, 44100)) : pcm16(channels, 44100);
        const auto second_format = second_float ? (target.big_endian ? float32(2, 44100) :
            extensible_float32(2, 44100)) : pcm32(2, 44100);
        TempFile first, second;
        const auto first_chunks = {first_format, first_data};
        const auto second_chunks = {second_format, second_data};
        const auto* path = first.write(rf64_first ? rf64(first_chunks, first_frames) : wave(first_chunks));
        const auto* next = second.write(rf64_second ? rf64(second_chunks, total_frames - first_frames) : wave(second_chunks));
        for (const std::size_t width : {8, 47, 64, 544}) {
            auto ring = make_ring(path, next, width, output);
            const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, target.bytes),
                total_frames, 44100);
            expect("float ring wraps before the join", static_cast<int>(layout.capacity * width < first_frames), 1);
            expect("float join splits a slot", static_cast<int>(first_frames % width != 0), 1);
            expect("float ring source frame width", ring.info().format.frame_bytes(), channels * (first_bits / 8));
            expect("float ring output bytes", ring.block_bytes(), width * target.bytes);
            expect("float ring pages locked", static_cast<int>(ring.pages_locked()), 1);
            Bytes left(ring.block_bytes() + 2, guard), right(left.size(), guard);
            for (std::size_t start = 0; start < total_frames; start += width) {
                const auto popped = await_block(ring, left.data() + 1, right.data() + 1);
                const auto count = std::min(width, total_frames - start);
                expect("float ring status", static_cast<int>(popped.status), static_cast<int>(
                    start + count == total_frames ? stream::BlockStatus::last : stream::BlockStatus::ready));
                expect("float ring valid frames include signed zeros", popped.valid_frames, count);
                for (std::size_t i = 0; i < width; ++i) {
                    const auto frame = start + i;
                    const bool in_first = frame < first_frames;
                    const auto source_channels = in_first ? channels : std::uint8_t{2};
                    const auto source_bits = in_first ? first_bits : std::uint8_t{32};
                    const auto& data = in_first ? first_data.payload : second_data.payload;
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        std::array<std::uint8_t, 8> expected{};
                        if (i < count) {
                            const auto source_frame = in_first ? frame : frame - first_frames;
                            const auto offset = (source_frame * source_channels + (source_channels == 1 ? 0 : channel)) * (source_bits / 8);
                            expected = expected_float_sample(data.data() + offset, source_bits, target,
                                in_first ? first_float : second_float);
                        }
                        const auto& actual = channel == 0 ? left : right;
                        for (std::size_t byte = 0; byte < target.bytes; ++byte) {
                            if (actual[1 + i * target.bytes + byte] != expected[byte]) {
                                expect("float ring frame " + std::to_string(frame) + " channel " + std::to_string(channel) +
                                    " byte " + std::to_string(byte), actual[1 + i * target.bytes + byte], expected[byte]);
                            }
                        }
                    }
                }
                for (const auto* data : {&left, &right}) {
                    expect("float ring leading guard", data->front(), guard);
                    expect("float ring trailing guard", data->back(), guard);
                }
            }
            ring.stop();
            expect("float ring producer completed without read failure", static_cast<int>(ring.read_failed()), 0);
            expect("float ring releases first input", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            expect("float ring releases second input", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        }
    }
}

void check_wide_pcm_stop(std::uint8_t source_bits, bool extensible = false)
{
    constexpr std::size_t width = 47;
    constexpr std::uint8_t guard = 0xa5;
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        // A short run joins before playback; the long run blocks a live
        // producer with unread PCM. Stop must revoke both forms of buffering.
        for (const auto count : {std::size_t{7}, frames}) {
            TempFile file;
            const auto* path = file.write(wave({extensible ? extensible_pcm(channels, 44100, 24, 32) : pcm_integer(channels, 44100, source_bits),
                extensible ? pcm24_in32_ramp(channels, count) : source_bits == 24 ? pcm24_ramp(channels, count) : pcm32_ramp(channels, count)}));
            for (const bool consume_first : {false, true}) {
                auto ring = make_ring(path, {}, width, source_bits == 24 ? audio::int24_le : audio::int32_le);
                Bytes left(ring.block_bytes(), guard), right(left.size(), guard);
                if (consume_first) {
                    const auto result = await_block(ring, left.data(), right.data());
                    expect("wide PCM pre-stop frames", result.valid_frames, std::min(width, count));
                }
                ring.stop();
                ring.stop();
                std::ranges::fill(left, guard);
                std::ranges::fill(right, guard);
                const auto result = ring.consumer().pop(left.data(), right.data());
                expect("wide PCM stopped ring has no block", static_cast<int>(result.status), static_cast<int>(stream::BlockStatus::empty));
                expect("wide PCM stopped ring has no frames", result.valid_frames, 0);
                expect("wide PCM stopped ring preserves left", std::ranges::count(left, guard), left.size());
                expect("wide PCM stopped ring preserves right", std::ranges::count(right, guard), right.size());
                expect("wide PCM stopped producer is healthy", static_cast<int>(ring.read_failed()), 0);
                expect("wide PCM stopped producer releases its WAV", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            }
        }
    }
}

SIZE_T minimum_working_set()
{
    SIZE_T minimum{}, maximum{};
    DWORD flags{};
    if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &flags)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessWorkingSetSizeEx");
    }
    return minimum;
}

void check_locked_pages()
{
    // Minimal check, without inducing memory pressure: every page is resident and
    // locked, and the working-set quota taken for the lock is returned afterwards.
    const auto before = minimum_working_set();
    {
        constexpr std::size_t bytes = 1024 * 1024;
        const core::PageAllocation pages(bytes);
        expect("pages locked", static_cast<int>(pages.locked()), 1);
        expect("quota grown by the allocation", static_cast<int>(minimum_working_set() >= before + bytes), 1);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> query(bytes / system.dwPageSize);
        for (std::size_t i = 0; i < query.size(); ++i) {
            query[i].VirtualAddress = pages.data() + i * system.dwPageSize;
        }
        if (!QueryWorkingSetEx(GetCurrentProcess(), query.data(), static_cast<DWORD>(query.size() * sizeof(query[0])))) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "QueryWorkingSetEx");
        }
        for (const auto& page : query) {
            if (!page.VirtualAttributes.Valid || !page.VirtualAttributes.Locked) {
                throw std::runtime_error("a locked allocation page is not resident and locked");
            }
        }
        expect("zero-filled first byte", static_cast<int>(pages.data()[0]), 0);
        expect("zero-filled last byte", static_cast<int>(pages.data()[bytes - 1]), 0);
    }
    expect("quota returned after release", minimum_working_set(), before);

    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 48000)}));
    auto ring = make_ring(path, {}, 512, audio::int16_le);
    expect("ring pages locked", static_cast<int>(ring.pages_locked()), 1);
    expect("ring metadata pages locked", static_cast<int>(ring.metadata_pages_locked()), 1);
    expect("ring pages and metadata fit the budget", static_cast<int>(ring.committed_size() <= 16 * 1024 * 1024), 1);
}

void check_resident_consumer_view()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, 19)}));
    core::PageAllocation wrapper(sizeof(stream::PrefetchRing));
    // Only the outer owner is protected. The borrowed view must reach the
    // separately resident metadata directly, without reading its owner's page.
    const auto destroy = [](stream::PrefetchRing* ring) noexcept {
        DWORD ignored{};
        (void)VirtualProtect(ring, sizeof(*ring), PAGE_READWRITE, &ignored);
        std::destroy_at(ring);
    };
    // Placement new initializes the owner directly from make_ring's prvalue.
    std::unique_ptr<stream::PrefetchRing, decltype(destroy)> ring(
        ::new (static_cast<void*>(wrapper.data())) stream::PrefetchRing(make_ring(path, {}, 8, audio::int16_le)),
        destroy);
    auto consumer = ring->consumer();
    DWORD previous{};
    if (!VirtualProtect(wrapper.data(), wrapper.committed_size(), PAGE_NOACCESS, &previous)) {
        core::throw_last_error("VirtualProtect(prefetch wrapper)");
    }
    std::array<std::uint16_t, 8> left{}, right{};
    for (std::size_t start = 0; start < 19; start += left.size()) {
        expect("resident consumer observes a healthy producer", static_cast<int>(consumer.read_failed()), 0);
        const auto result = consumer.pop(left.data(), right.data());
        const auto count = std::min<std::size_t>(left.size(), 19 - start);
        expect("resident consumer valid frames", result.valid_frames, count);
        expect("resident consumer block status", result.status,
            start + count == 19 ? stream::BlockStatus::last : stream::BlockStatus::ready);
        for (std::size_t i = 0; i < left.size(); ++i) {
            expect("resident consumer left sample", left[i], i < count ? static_cast<std::uint16_t>(start + i) : 0);
            expect("resident consumer right sample", right[i], i < count ? static_cast<std::uint16_t>(~(start + i)) : 0);
        }
    }
    expect("resident consumer final block is consumed once", consumer.pop(left.data(), right.data()).status,
        stream::BlockStatus::empty);
}

void check_resident_ring_budget()
{
    const auto before = minimum_working_set();
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 384000), pcm16_ramp(2, 384000)}));
    {
        // One-frame slots exhaust the memory budget before the two-second
        // duration target. Include actual page-rounded metadata in that bound.
        auto ring = make_ring(path, {}, 1, audio::int16_le);
        expect("budget-limited sample pages locked", static_cast<int>(ring.pages_locked()), 1);
        expect("budget-limited metadata pages locked", static_cast<int>(ring.metadata_pages_locked()), 1);
        expect("all ring pages fit sixteen MiB", static_cast<int>(ring.committed_size() <= 16 * 1024 * 1024), 1);
        expect("resident ring accounting matches quota", minimum_working_set(), before + ring.committed_size());
    }
    expect("resident ring returns all page quota", minimum_working_set(), before);
}

void check_prefetch()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, frames)}));
    // 96 frames fit contiguous planar batches, 47 need alignment gaps. Both
    // wrap through a partial final batch and reuse slots for the padded tail.
    check_sequence(path, block);
    check_sequence(path, 96);
    check_sequence(path, 47);
    DWORD data{};
    {
        // Stop without consuming anything, with a full ring and unread input.
        auto ring = make_ring(path, {}, block, audio::int16_le);
        data = static_cast<DWORD>(ring.info().data_offset);
    }

    auto lock = open_lock_handle(path);
    // Lock the second block in an otherwise contiguous initial batch.
    constexpr DWORD width = 96;
    const DWORD initial = data + width * 4;
    if (!LockFile(lock.get(), initial, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock prefill frame");
    }
    expect_locked_read([&] { auto ring = make_ring(path, {}, width, audio::int16_le); });
    const DWORD refill = data + static_cast<DWORD>(2 * block * 4);
    if (!UnlockFile(lock.get(), initial, 0, 4, 0) || !LockFile(lock.get(), refill, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock first refill frame");
    }
    expect_refill_failure(path, block, 2);
    // Three fully readable blocks precede the bad block after the first wrap.
    constexpr DWORD before_fault = (2 * 44100 + width - 1) / width + 3;
    if (!UnlockFile(lock.get(), refill, 0, 4, 0) || !LockFile(lock.get(), data + before_fault * width * 4, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock batched refill frame");
    }
    expect_refill_failure(path, width, before_fault);
}

void check_track_handle_lifetime()
{
    TempFile first, second;
    const auto* path = first.write(wave({pcm16(2, 44100), pcm16_ramp(2, 2 * block)}));
    const auto* next = second.write(wave({pcm16(2, 44100), pcm16_ramp(2, 1)}));
    {
        // The first track fills both slots exactly. With no consumer, the
        // producer cannot start reading the second track before we check it.
        auto ring = make_ring(media::WavReader(path), media::WavReader(next), block, audio::int16_le);
        expect("finished first WAV released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        expect("pending second WAV protected", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);
    }
    expect("unread second WAV released on shutdown", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);

    // One extra frame per slot lets both WAVs fit in the initial two slots.
    auto primed = make_ring(path, next, block + 1, audio::int16_le);
    expect("fully buffered first WAV released before playback", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    expect("fully buffered second WAV released before playback", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

// Called by a ring's preparation, which runs on the producer: a handle on that
// thread, so the test can observe whether the producer still runs.
core::UniqueHandle current_thread_handle()
{
    HANDLE thread{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, SYNCHRONIZE, FALSE, 0)) {
        core::throw_last_error("DuplicateHandle(producer thread)");
    }
    return core::UniqueHandle(thread);
}

// The Thread State and Thread Wait Reason values that the thread performance
// counters document, as SYSTEM_THREAD_INFORMATION reports them: Waiting, in a
// wait the thread requested itself (WaitForSingleObject and the like).
constexpr ULONG thread_waiting = 5;
constexpr ULONG wait_user_request = 6;

struct ThreadWait {
    ULONG state{}, reason{};
};

// Thread `id` of this process in a SystemProcessInformation snapshot; nullopt
// if the snapshot no longer lists it.
std::optional<ThreadWait> thread_wait(DWORD id, std::vector<std::uint64_t>& buffer)
{
    constexpr auto info_length_mismatch = static_cast<NTSTATUS>(0xC0000004L);
    for (;;) {
        ULONG needed{};
        const auto status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(),
            static_cast<ULONG>(buffer.size() * sizeof(buffer[0])), &needed);
        if (status != info_length_mismatch) {
            if (!NT_SUCCESS(status)) {
                fail("NtQuerySystemInformation(SystemProcessInformation) failed: NTSTATUS " +
                    std::to_string(static_cast<unsigned long>(status)));
            }
            break;
        }
        buffer.resize(needed / sizeof(buffer[0]) + 8192); // Room for threads started meanwhile.
    }
    const auto* entry = reinterpret_cast<const std::byte*>(buffer.data());
    for (;;) {
        const auto& process = *reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(entry);
        if (HandleToULong(process.UniqueProcessId) == GetCurrentProcessId()) {
            // The process's thread entries follow its own entry.
            const std::span threads(reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(&process + 1),
                process.NumberOfThreads);
            for (const auto& thread : threads) {
                if (HandleToULong(thread.ClientId.UniqueThread) == id) {
                    return ThreadWait{thread.ThreadState, thread.WaitReason};
                }
            }
            return std::nullopt;
        }
        if (process.NextEntryOffset == 0) {
            fail("the system process snapshot does not list this process");
        }
        entry += process.NextEntryOffset;
    }
}

// Polls until thread `id` (also open as `thread`) is blocked in a wait it
// requested itself. The kernel shows that state only in a snapshot, so there
// is no event to wait on. The poll is bounded and fails with the last state
// it saw, or as soon as the thread has ended, instead of hanging.
void await_requested_wait(HANDLE thread, DWORD id, std::string_view what)
{
    std::vector<std::uint64_t> buffer(64 * 1024); // 512 KiB to start, 8-byte aligned.
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
    for (;;) {
        const auto seen = thread_wait(id, buffer);
        if (seen && seen->state == thread_waiting && seen->reason == wait_user_request) {
            return;
        }
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
            fail(std::string(what) + ": the thread ended instead");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail(std::string(what) + ": still not waiting at the deadline (thread state " +
                (seen ? std::to_string(seen->state) + ", wait reason " + std::to_string(seen->reason) : "unlisted") + ")");
        }
        std::this_thread::yield();
    }
}

void check_streaming_producer_parks_until_stop()
{
    // As in play_tracks without a progress holder, the producer holds the only
    // readers, and the last one lives in the producer's own frame. Its file stays
    // protected after the final block until stop() ends the parked thread; an
    // exited producer would have closed it, and its thread handle would be
    // signaled, before the 100 ms wait on that handle below ends.
    constexpr std::size_t width = 47;
    constexpr std::size_t first_frames = block + 17;
    constexpr std::size_t total = first_frames + 2 * block;
    const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, 2), total, 44100);
    expect_true("the run streams through the ring", layout.total_blocks > layout.capacity);
    TempFile first, second;
    const auto* path = first.write(wave({pcm16(2, 44100), pcm16_ramp(2, first_frames)}));
    const auto* next = second.write(wave({pcm16(2, 44100), pcm16_ramp(2, total - first_frames, first_frames)}));
    core::UniqueHandle producer;
    stream::PrefetchRing ring([&] {
        producer = current_thread_handle();
        return stream::PrefetchRing::PreparedTracks{stream::retain_playable_track(path), stream::retain_playable_track(next)};
    }, width, audio::int16_le);
    std::array<std::uint16_t, width> left{}, right{};
    for (std::size_t start = 0; start < total; start += width) {
        const auto result = await_block(ring, left.data(), right.data());
        const auto count = std::min(width, total - start);
        expect("parked run status", result.status,
            start + count == total ? stream::BlockStatus::last : stream::BlockStatus::ready);
        expect("parked run valid frames", result.valid_frames, count);
        expect("parked run left", left.front(), static_cast<std::uint16_t>(start));
        expect("parked run right", right[count - 1], static_cast<std::uint16_t>(~(start + count - 1)));
    }
    expect("finished earlier WAV released at its end", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    expect("last WAV protected after its final block", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);
    expect("the parked producer thread is alive", WaitForSingleObject(producer.get(), 100), WAIT_TIMEOUT);
    expect("consumed run has no further block", ring.consumer().pop(left.data(), right.data()).status, stream::BlockStatus::empty);
    expect("parked producer keeps the last WAV until stop", probe_file_access(next, GENERIC_WRITE | DELETE),
        ERROR_SHARING_VIOLATION);
    ring.stop();
    expect("stop ends the parked producer thread", WaitForSingleObject(producer.get(), 0), WAIT_OBJECT_0);
    expect("parked producer completed without read failure", static_cast<int>(ring.read_failed()), 0);
    expect("stop releases the last WAV", probe_file_access(next, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

// The consumer sets space_available only at its wake threshold. Priming fills
// the whole ring and the producer waits; the first refill frame is locked, so a
// woken producer's refill fails and its thread ends. Its handle therefore shows
// whether a pop woke it: still waiting after wake_interval - 1 pops, ended after
// the threshold pop. The pops start only once the producer is seen blocked in
// that wait, so a correct ring passes whatever the scheduling; the 200 ms wait
// before the threshold pop is only the window in which an early wake shows.
void check_producer_wakes_at_threshold()
{
    constexpr std::size_t width = 47;
    constexpr std::size_t total = 3 * block;
    const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, 2), total, 44100);
    expect_true("the run streams through the ring", layout.total_blocks > layout.capacity);
    expect_true("pops precede the wake threshold", layout.wake_interval >= 2);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, total)}));
    auto lock = open_lock_handle(path);
    // Priming reads end exactly before this frame.
    const auto refill = static_cast<DWORD>(media::WavReader(path).info().data_offset + layout.capacity * width * 4);
    if (!LockFile(lock.get(), refill, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock first refill frame");
    }
    core::UniqueHandle producer;
    DWORD producer_id{};
    stream::PrefetchRing ring([&] {
        producer = current_thread_handle();
        producer_id = GetCurrentThreadId();
        return stream::PrefetchRing::PreparedTracks{stream::retain_playable_track(path)};
    }, width, audio::int16_le);
    // Construction returns once priming is signaled, possibly before the
    // producer reaches its wait for space. A pop before then would let it
    // refill without any wake. Past priming, that wait is the only one the
    // producer requests itself, so a requested wait shows it blocked there.
    await_requested_wait(producer.get(), producer_id, "the primed producer waits for space before the first pop");
    std::array<std::uint16_t, width> left{}, right{};
    for (std::size_t index = 0; index < layout.wake_interval; ++index) {
        if (index + 1 == layout.wake_interval) {
            // An earlier wake would have let the producer read the locked frame and end.
            expect("the producer waits through the pops before the threshold",
                WaitForSingleObject(producer.get(), 200), WAIT_TIMEOUT);
            expect("no refill before the threshold", static_cast<int>(ring.read_failed()), 0);
        }
        const auto result = ring.consumer().pop(left.data(), right.data());
        expect("block up to the threshold", result.status, stream::BlockStatus::ready);
        expect("threshold run left", left.front(), static_cast<std::uint16_t>(index * width));
        expect("threshold run right", right.back(), static_cast<std::uint16_t>(~(index * width + width - 1)));
    }
    expect("the threshold pop wakes the producer, whose refill fails and ends it",
        WaitForSingleObject(producer.get(), static_cast<DWORD>(hang_budget(std::chrono::seconds(5)).count())),
        WAIT_OBJECT_0);
    expect("the woken producer's refill failed", static_cast<int>(ring.read_failed()), 1);
    ring.stop();
    const auto cause = ring.read_error();
    expect_true("the refill keeps its failure cause", static_cast<bool>(cause));
    try {
        std::rethrow_exception(cause);
    } catch (const std::system_error& error) {
        expect_true("the refill failure is ReadFile's lock violation",
            error.code() == std::error_code(ERROR_LOCK_VIOLATION, std::system_category()) &&
            std::string_view(error.what()).starts_with("ReadFile"));
    }
}

void check_reader_thread_hints()
{
    const auto inherited_priorities = new_thread_priorities();
    // The producer prepares under its name at the thread's default speed and
    // later reports stream::reader_refill_speed, for the refills of a run
    // longer than the ring. No consumer pops, so it stays alive, waiting for
    // space, until stop(). Nothing observes the producer during priming, so
    // this does not catch a request moved before the priming fill; its place
    // after the priming event was checked in the Release machine code.
    constexpr std::size_t width = 47;
    constexpr std::size_t total = 3 * block;
    const auto layout = stream::plan_ring_layout(stream::plan_block_shape(width, 2), total, 44100);
    expect_true("the run streams through the ring", layout.total_blocks > layout.capacity);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, total)}));
    DWORD producer{};
    std::optional<ThreadSpeedState> preparing;
    ThreadPriorities preparing_priorities;
    std::wstring name;
    stream::PrefetchRing ring([&] {
        producer = GetCurrentThreadId();
        preparing = thread_speed_state(GetCurrentThread());
        preparing_priorities = thread_priorities(GetCurrentThread());
        name = thread_description(GetCurrentThread());
        return stream::PrefetchRing::PreparedTracks{stream::retain_playable_track(path)};
    }, width, audio::int16_le);
    expect_true("the producer names itself", name == L"WAV Player reader");
    // Only the speed changes: no priority or background mode.
    expect_priorities("the preparing producer preserves inherited priorities", preparing_priorities, inherited_priorities);
    if (!expect_speed_state("preparation runs at the default speed", preparing, core::ExecutionSpeed::system)) {
        return;
    }
    // The request follows the priming event, so it may land just after construction returns.
    const auto thread = open_thread_for_query(producer);
    const auto expected = documented_state(stream::reader_refill_speed);
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
    auto refilling = thread_speed_state(thread.get());
    while (refilling != expected && std::chrono::steady_clock::now() < deadline) {
        Sleep(1);
        refilling = thread_speed_state(thread.get());
    }
    expect_speed_state("refills run at reader_refill_speed", refilling, stream::reader_refill_speed);
    expect_priorities("the refilling producer preserves inherited priorities", thread_priorities(thread.get()), inherited_priorities);
}

void check_exactly_full_ring()
{
    // A run of exactly one ring is fully buffered at priming, so construction
    // joins its producer. The producer must not park, or the join never returns.
    constexpr std::size_t width = 47;
    const auto shape = stream::plan_block_shape(width, 2);
    const auto capacity = stream::plan_ring_layout(shape, 8 * block, 44100).capacity;
    const auto total = capacity * width;
    const auto layout = stream::plan_ring_layout(shape, total, 44100);
    expect("the run fills the ring exactly", layout.total_blocks, layout.capacity);
    expect("the ring keeps its full capacity", layout.capacity, capacity);
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, total)}));
    auto ring = make_ring(path, {}, width, audio::int16_le);
    expect("exactly full run released its WAV before playback", probe_file_access(path, GENERIC_WRITE | DELETE),
        ERROR_SUCCESS);
    std::array<std::uint16_t, width> left{}, right{};
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto result = ring.consumer().pop(left.data(), right.data());
        expect("exactly full run status", result.status,
            index + 1 == capacity ? stream::BlockStatus::last : stream::BlockStatus::ready);
        expect("exactly full run valid frames", result.valid_frames, width);
        expect("exactly full run left", left.front(), static_cast<std::uint16_t>(index * width));
    }
    expect("exactly full run is consumed once", ring.consumer().pop(left.data(), right.data()).status, stream::BlockStatus::empty);
}

void check_track_inputs()
{
    TempFile file;
    // A partial reader has enough data left to prime a ring. Accepting it
    // would drop the first sample and report EOF only during a later refill.
    const auto* path = file.write(wave({pcm16(2, 44100), pcm16_ramp(2, frames)}));
    const auto partial = [&] {
        media::WavReader reader(path);
        std::array<std::int16_t, 1> left{}, right{};
        (void)read_pcm16(reader, left, right);
        return reader;
    };
    expect_throw<std::invalid_argument>("partial reader", [&] { (void)stream::retain_playable_track(partial()); });
    // No track (std::monostate or a null path) only lets a caller omit an
    // optional input; it never becomes a track. An empty path is a path.
    expect_true("monostate is no track", stream::is_no_track(std::monostate{}));
    expect_true("a null path is no track", stream::is_no_track(static_cast<const wchar_t*>(nullptr)));
    expect_true("an empty path is a track", !stream::is_no_track(L""));
    expect_throw<std::invalid_argument>("no track", [] { (void)stream::retain_playable_track(std::monostate{}); });
    expect_throw<std::invalid_argument>("null path", [] {
        (void)stream::retain_playable_track(static_cast<const wchar_t*>(nullptr));
    });
    expect_throw<std::invalid_argument>("null retained reader", [] {
        (void)stream::retain_playable_track(std::shared_ptr<media::WavReader>{});
    });
    expect_throw<std::runtime_error>("empty path", [] { (void)stream::retain_playable_track(L""); });
    expect("rejected readers released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);

    // A ring needs a preparation that names a first track and no null source.
    // Misuse inside the preparation, such as a partial second reader, fails
    // construction with its own exception.
    expect_throw<std::invalid_argument>("no track source", [] {
        stream::PrefetchRing ring(stream::PrefetchRing::PrepareTracks{}, block, audio::int16_le);
    });
    expect_throw<std::invalid_argument>("empty prepared run", [] {
        stream::PrefetchRing ring([] { return stream::PrefetchRing::PreparedTracks{}; }, block, audio::int16_le);
    });
    expect_throw<std::invalid_argument>("null first prepared track", [] {
        stream::PrefetchRing ring([] { return stream::PrefetchRing::PreparedTracks{nullptr}; }, block, audio::int16_le);
    });
    expect_throw<std::invalid_argument>("null later prepared track", [&] {
        stream::PrefetchRing ring([&] {
            return stream::PrefetchRing::PreparedTracks{stream::retain_playable_track(path), nullptr};
        }, block, audio::int16_le);
    });
    expect_throw<std::invalid_argument>("partial second reader", [&] {
        auto ring = make_ring(media::WavReader(path), partial(), block, audio::int16_le);
    });
    expect("rejected prepared readers released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

// make_ring splits a run as the request's preparation does: a second WAV with
// another rate, without a lossless conversion to the output, or that does not
// open ends the run after the first, and construction does not keep its file.
// Joining it would add a frame to the first block or fail construction.
void check_fixture_run_split()
{
    constexpr std::size_t width = 4;
    constexpr std::size_t first_frames = 3;
    TempFile first, faster, wider;
    const auto* path = first.write(wave({pcm16(2, 44100), pcm16_ramp(2, first_frames)}));
    const auto* other_rate = faster.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1)}));
    const auto* lossy = wider.write(wave({pcm24(2, 44100), pcm24_ramp(2, 1)}));
    for (const auto* next : {other_rate, lossy, L""}) {
        auto ring = make_ring(path, next, width, audio::int16_le);
        if (*next) {
            expect("unjoined second WAV released by construction", probe_file_access(next, GENERIC_WRITE | DELETE),
                ERROR_SUCCESS);
        }
        std::array<std::uint16_t, width> left{}, right{};
        const auto result = await_block(ring, left.data(), right.data());
        expect("run ends with the first WAV", result.status, stream::BlockStatus::last);
        expect("only the first WAV's frames", result.valid_frames, first_frames);
        expect("first WAV's last left sample", left[first_frames - 1], static_cast<std::uint16_t>(first_frames - 1));
    }
}

void check_retained_position()
{
    TempFile file;
    constexpr std::size_t total = 3 * 48000 + 7;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, total)}));
    const media::WavReader retained(path);
    for (const auto first : {std::size_t{0}, std::size_t{5}, std::size_t{48000}, total - 1}) {
        constexpr std::size_t width = 48000;
        auto ring = make_ring(retained.reopen(), {}, width, audio::int16_le, first);
        expect("retained original total frames", ring.info().frame_count, total);
        std::vector<std::uint16_t> left(width), right(width);
        for (auto start = first; start < total; start += width) {
            const auto result = await_block(ring, left.data(), right.data());
            const auto count = std::min(width, total - start);
            expect("retained position block status", static_cast<int>(result.status),
                static_cast<int>(start + count == total ? stream::BlockStatus::last : stream::BlockStatus::ready));
            expect("retained position valid frames", result.valid_frames, count);
            for (std::size_t i = 0; i < width; ++i) {
                expect("retained position left", left[i], i < count ? static_cast<std::uint16_t>(start + i) : 0);
                expect("retained position right", right[i], i < count ? static_cast<std::uint16_t>(~(start + i)) : 0);
            }
        }
        expect("retained handle remains unread", static_cast<int>(retained.at_start()), 1);
        expect("retained file protected after initial read", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SHARING_VIOLATION);
    }
    expect_throw<std::out_of_range>("empty resumed prefetch", [&] {
        auto ring = make_ring(retained.reopen(), {}, 4, audio::int16_le, total);
    });
    expect_throw<std::out_of_range>("resumed position beyond EOF", [&] {
        auto ring = make_ring(retained.reopen(), {}, 4, audio::int16_le, total + 1);
    });
}

void check_stopped_producer_failure()
{
    TempFile file;
    constexpr std::size_t width = 48000;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 3 * width + 1)}));
    {
        auto ring = make_ring(path, {}, width, audio::int16_le);
        ring.stop();
        ring.stop();
        expect("stopped healthy producer", static_cast<int>(ring.read_failed()), 0);
        expect_true("stopped healthy producer has no failure cause", !ring.read_error());
        std::vector<std::uint16_t> left(width), right(width);
        expect("pop after stop finds no block", static_cast<int>(ring.consumer().pop(left.data(), right.data()).status),
            static_cast<int>(stream::BlockStatus::empty));
        expect("stopped producer released reader", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    }

    auto lock = open_lock_handle(path);
    const auto offset = static_cast<DWORD>(media::WavReader(path).info().data_offset + 2 * width * 4);
    if (!LockFile(lock.get(), offset, 0, 4, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock pause refill frame");
    }
    auto ring = make_ring(path, {}, width, audio::int16_le);
    std::vector<std::uint16_t> left(width), right(width);
    expect("initial producer has not failed", static_cast<int>(ring.read_failed()), 0);
    expect("consume first block to wake producer", static_cast<int>(ring.consumer().pop(left.data(), right.data()).status),
        static_cast<int>(stream::BlockStatus::ready));
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
    while (!ring.read_failed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expect("non-consuming producer failure observation", static_cast<int>(ring.read_failed()), 1);
    ring.stop();
    ring.stop();
    expect("joined producer retains failure", static_cast<int>(ring.read_failed()), 1);
    // The cause outlives priming: the refill's own ReadFile error.
    const auto cause = ring.read_error();
    expect_true("joined producer keeps its failure cause", static_cast<bool>(cause));
    try {
        std::rethrow_exception(cause);
    } catch (const std::system_error& error) {
        expect_true("producer failure is ReadFile's lock violation",
            error.code() == std::error_code(ERROR_LOCK_VIOLATION, std::system_category()) &&
            std::string_view(error.what()).starts_with("ReadFile"));
    }
    lock.reset();
    expect("failed producer released reader", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

// A control STA can already hold a WM_QUIT when it builds a ring. Priming must
// still end, without dispatching, and leave the quit for the outer loop.
void check_priming_with_pending_quit()
{
    TempFile file;
    constexpr std::size_t width = 48000;
    // Longer than the two-second ring, so the producer blocks after its first
    // publication and only that publication can end priming.
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 4 * width + 1)}));
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        throw std::runtime_error("Could not initialize a test STA");
    }
    struct Apartment {
        ~Apartment() { CoUninitialize(); }
    } apartment;
    PostQuitMessage(41);
    {
        auto ring = make_ring(path, {}, width, audio::int16_le);
        std::vector<std::uint16_t> left(width), right(width);
        const auto first = ring.consumer().pop(left.data(), right.data());
        expect("primed ring yields its first block", first.status, stream::BlockStatus::ready);
        expect("first block is complete", first.valid_frames, width);
        expect("first block starts at frame 0", left[0], 0);
    }
    MSG message{};
    expect("WM_QUIT left for the outer loop", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
    expect("quit code preserved", message.wParam, 41);
}

void check_stopped_buffered_run()
{
    // Priming joins the worker when the entire track fits. Stop still revokes
    // buffered PCM, before any consumption and after a partial consumption.
    for (const auto count : {1, 4, 7}) {
        for (const bool consume_first : {false, true}) {
            TempFile file;
            const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, count)}));
            auto ring = make_ring(path, {}, 4, audio::int16_le);
            std::array<std::uint16_t, 4> left{}, right{};
            if (consume_first) {
                (void)ring.consumer().pop(left.data(), right.data());
            }
            ring.stop();
            ring.stop();
            left.fill(0x1234);
            right.fill(0xabcd);
            const auto result = ring.consumer().pop(left.data(), right.data());
            expect("stopped buffered run has no block", static_cast<int>(result.status),
                static_cast<int>(stream::BlockStatus::empty));
            expect("stopped buffered run has no frames", result.valid_frames, 0);
            for (const auto value : left) {
                expect("stop leaves left destination untouched", value, 0x1234);
            }
            for (const auto value : right) {
                expect("stop leaves right destination untouched", value, 0xabcd);
            }
            expect("stopped buffered run released reader", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
        }
    }
}

void check_single_slot()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", {0x56, 0x34, 0xdc, 0xfe}}}));
    // A one-frame file still needs a fully padded output at the maximum width
    // for which two stereo blocks and metadata fit in the 16 MiB budget. A tiny
    // probe has one page of samples, exposing the exact metadata commitment.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto metadata_bytes = [&] {
        const auto probe = make_ring(path, {}, 1, audio::int16_le);
        return probe.committed_size() - system.dwPageSize;
    }();
    const std::size_t width = (16 * 1024 * 1024 - metadata_bytes) / (2 * 2 * sizeof(std::uint16_t));
    auto ring = make_ring(path, {}, width, audio::int16_le);
    std::vector<std::uint16_t> left(width, 1), right(width, 2);
    const auto last = ring.consumer().pop(left.data(), right.data());
    expect("single slot status", static_cast<int>(last.status), static_cast<int>(stream::BlockStatus::last));
    expect("single slot valid frames", last.valid_frames, 1);
    expect("single slot left", left.front(), 0x3456);
    expect("single slot right", right.front(), 0xfedc);
    expect("single slot left padding", std::count(left.begin() + 1, left.end(), 0), width - 1);
    expect("single slot right padding", std::count(right.begin() + 1, right.end(), 0), width - 1);
    const auto after = ring.consumer().pop(left.data(), right.data());
    expect("empty after single slot", static_cast<int>(after.status), static_cast<int>(stream::BlockStatus::empty));
    try {
        auto too_wide = make_ring(path, {}, width + 1, audio::int16_le);
        throw std::runtime_error("accepted a block width beyond the two-block memory budget");
    } catch (const std::invalid_argument&) {
    }
    constexpr std::size_t packed_limit = (16 * 1024 * 1024) / (4 * 3);
    expect_throw<std::invalid_argument>("24-bit block over the two-block budget", [&] {
        auto too_wide = make_ring(path, {}, packed_limit + 1, audio::int24_le);
    });
}

void check_playlist_preparation_thread()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1)}));
    const auto caller = GetCurrentThreadId();
    DWORD preparing{};
    stream::PrefetchRing ring([&] {
        preparing = GetCurrentThreadId();
        auto source = stream::retain_playable_track(path);
        return stream::PrefetchRing::PreparedTracks{source, source, source};
    }, 4, audio::int16_le);
    expect_true("playlist preparation uses the existing producer", preparing != 0 && preparing != caller);
    std::array<std::uint16_t, 4> left{}, right{};
    const auto popped = ring.consumer().pop(left.data(), right.data());
    expect("three short tracks share one final block", popped.status, stream::BlockStatus::last);
    expect("all three tracks are valid frames", popped.valid_frames, 3);
}

void check_priming_wake_hook()
{
    // The preparation sets the hook's event and then waits for the poll, so the
    // run primes only if the constructing thread polls on that wake. The first
    // poll sets the event again and returns once the producer has primed and
    // exited, leaving both events set for the next wait: priming must win.
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), pcm16_ramp(2, 1)}));
    const auto wake = core::create_event();
    const auto release = core::create_event();
    struct Probe {
        HANDLE wake, release;
        const wchar_t* path;
        DWORD thread{};
        unsigned polls{};
        bool released_file{};
    } probe{wake.get(), release.get(), path};
    const auto poll = [](void* context) noexcept {
        auto& value = *static_cast<Probe*>(context);
        value.thread = GetCurrentThreadId();
        if (++value.polls != 1) {
            return;
        }
        SetEvent(value.wake);
        SetEvent(value.release);
        // The producer holds the only reader. It closes the file after its
        // publication has set `primed`, as its thread ends.
        const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
        while (std::chrono::steady_clock::now() < deadline) {
            const auto handle = CreateFileW(value.path, GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
                value.released_file = true;
                return;
            }
            Sleep(1);
        }
    };
    const auto caller = GetCurrentThreadId();
    stream::PrefetchRing ring([&probe] {
        auto source = stream::retain_playable_track(probe.path);
        SetEvent(probe.wake);
        if (WaitForSingleObject(probe.release, static_cast<DWORD>(hang_budget(std::chrono::seconds(5)).count())) !=
            WAIT_OBJECT_0) {
            throw std::runtime_error("The priming wait did not poll on its hook's event");
        }
        return stream::PrefetchRing::PreparedTracks{std::move(source)};
    }, 4, audio::int16_le, 0, {wake.get(), poll, &probe});
    expect("the hook polled once, while the run was prepared", probe.polls, 1);
    expect_true("the hook polled on the constructing thread", probe.thread == caller);
    expect_true("the producer primed and exited during the poll", probe.released_file);
    expect("priming won over the set hook event", WaitForSingleObject(wake.get(), 0), WAIT_OBJECT_0);
    std::array<std::uint16_t, 4> left{}, right{};
    const auto popped = ring.consumer().pop(left.data(), right.data());
    expect("the hooked run is primed", popped.status, stream::BlockStatus::last);
    expect("the hooked run keeps its frame", popped.valid_frames, 1);
}

} // namespace

int wavplay_test::run_prefetch_ring_tests()
{
    TestSuite suite;
    // Every case builds or stops a ring, so each is watched: a producer or
    // priming hang ends as that case's TIMEOUT. First: a wrong park condition
    // hangs a fully buffered ring's construction or a streaming ring's stop(),
    // and these two cases name it before any other case builds such a ring.
    suite.run_timed("a run that exactly fills the ring joins its producer during construction", check_exactly_full_ring);
    suite.run_timed("a streaming run's producer keeps its last WAV until stop", check_streaming_producer_parks_until_stop);
    suite.run_timed("the consumer wakes a waiting producer only at its wake threshold", check_producer_wakes_at_threshold);
    suite.run_timed("playlist validation runs on the ring producer before priming", check_playlist_preparation_thread);
    suite.run_timed("the named producer prepares at the default speed and refills at reader_refill_speed",
        check_reader_thread_hints);
    suite.run_timed("the producer preserves inherited low memory priority", [] {
        with_low_process_memory_priority(check_reader_thread_hints);
    });
    suite.run_timed("priming polls a caller's hook on its event and primed wins", check_priming_wake_hook);
    suite.run_timed("prefetch wrap, read failure and shutdown", check_prefetch);
    suite.run_timed("gapless refill across a ring wrap and next-track read failure", check_gapless_refill);
    suite.run_timed("source byte widths and oversized blocks choose bounded batches", check_source_batch_sizes);
    suite.run_timed("mono16 to stereo32 batches preserve samples and read count", [] { check_format_following_batches(true, false); });
    suite.run_timed("stereo32 to mono16 batches preserve samples and read count", [] { check_format_following_batches(false, false); });
    suite.run_timed("mono16 to stereo32 recomputes batching after a split block", [] { check_format_following_batches(true, true); });
    suite.run_timed("stereo32 to mono16 recomputes batching after a split block", [] { check_format_following_batches(false, true); });
    suite.run_timed("a boundary block crosses repeated one-frame sources without omission", check_short_track_batches);
    suite.run_timed("samples survive three ring wraps and an incomplete final batch", check_multiple_wraps);
    suite.run_timed("stop before and after batch, wake and wrap boundaries", check_stop_at_ring_boundaries);
    suite.run_timed("mono on both channels, joined gaplessly to stereo", check_mono_join);
    suite.run_timed("PCM24 dense and padded ring refill preserves mono/stereo through wrap and EOF",
        [] { check_wide_pcm_refill(24); });
    suite.run_timed("PCM24 stop revokes short and streaming buffers", [] { check_wide_pcm_stop(24); });
    suite.run_timed("PCM32 dense and padded ring refill preserves mono/stereo through wrap and EOF",
        [] { check_wide_pcm_refill(32); });
    suite.run_timed("PCM32 stop revokes short and streaming buffers", [] { check_wide_pcm_stop(32); });
    suite.run_timed("Extensible 24/32 and packed PCM24 join through padded and dense ring wrap", [] {
        check_wide_pcm_refill(24, true, false);
        check_wide_pcm_refill(24, false, true);
        check_wide_pcm_refill(24, true, true);
    });
    suite.run_timed("Extensible 24/32 stop releases validated short and streaming files", [] { check_wide_pcm_stop(24, true); });
    for (const auto type : {ASIOSTInt16MSB, ASIOSTInt24MSB, ASIOSTInt32MSB,
             ASIOSTInt32LSB16, ASIOSTInt32MSB18, ASIOSTInt32LSB20, ASIOSTInt32MSB24}) {
        const auto output = integer_output_case(type);
        const auto bits = static_cast<std::uint8_t>(output.valid_bits < 24 ? 16 : output.valid_bits);
        const auto name = "ASIO integer type " + std::to_string(type) + " preserves ring wrap, gaps and zero tail";
        suite.run_timed(name.c_str(), [type, bits] {
            check_wide_pcm_refill(bits, type == ASIOSTInt32MSB24, false, {type});
        });
    }
    for (const auto output : float_output_cases) {
        const auto name = "Float32 ring preserves every bit through wrap and joins for float type " + std::to_string(output.type);
        suite.run_timed(name.c_str(), [output] { check_float_ring(output, FloatRingJoin::floats); });
    }
    suite.run_timed("PCM16 to Float32 join preserves the wrapped Float64 LSB ring", [] {
        check_float_ring(float_output_case(ASIOSTFloat64LSB), FloatRingJoin::pcm16_to_float);
    });
    suite.run_timed("Float32 to PCM32 join preserves the wrapped Float64 MSB ring", [] {
        check_float_ring(float_output_case(ASIOSTFloat64MSB), FloatRingJoin::float_to_pcm32);
    });
    suite.run_timed("RIFF PCM24 to RF64 Extensible 24/32 joins across dense and padded ring wrap", [] {
        check_wide_pcm_refill(24, false, true, {ASIOSTInt24LSB}, false, true);
    });
    suite.run_timed("RF64 Extensible Float32 to RIFF Float32 joins across Float64 MSB ring wrap", [] {
        check_float_ring(float_output_case(ASIOSTFloat64MSB), FloatRingJoin::floats, true, false);
    });
    suite.run_timed("ring pages locked in the working set and the quota returned", check_locked_pages);
    suite.run_timed("resident consumer does not read the protected outer ring owner", check_resident_consumer_view);
    suite.run_timed("sample and resident metadata pages share the sixteen MiB budget", check_resident_ring_budget);
    suite.run_timed("track handles at an exact ring boundary and early shutdown", check_track_handle_lifetime);
    suite.run_timed("track inputs and prepared runs reject misuse before priming", check_track_inputs);
    suite.run_timed("the ring fixture ends a run before a second WAV the request would not join", check_fixture_run_split);
    suite.run_timed("single-track prefetch from retained frame through wrap and EOF", check_retained_position);
    suite.run_timed("producer stop and non-consuming read failure status", check_stopped_producer_failure);
    suite.run_timed("priming on an STA with a pending WM_QUIT", check_priming_with_pending_quit);
    suite.run_timed("stop revokes a fully buffered run", check_stopped_buffered_run);
    suite.run_timed("single-slot prefetch and width limit", check_single_slot);
    return suite.result("PCM and Float32 prefetch");
}
