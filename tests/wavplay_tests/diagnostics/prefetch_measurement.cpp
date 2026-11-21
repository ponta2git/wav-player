#include "diagnostics/prefetch_measurement.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/audio/planar_conversion.h"
#include "wavplay/core/win32.h"
#include "wavplay/stream/prefetch_policy.h"
#include "wavplay/stream/prefetch_ring.h"
#include "fixtures/ring_fixture.h"
#include "fixtures/wav_builder.h"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct MeasurementInput {
    std::wstring_view name;
    const char* label;
    wavplay::audio::SampleFormat sample;
    ASIOSampleType default_output;
    bool extensible;
};

using namespace wavplay::audio;
constexpr std::array inputs{
    MeasurementInput{L"pcm16", "PCM16", int16_le, ASIOSTInt16LSB, false},
    MeasurementInput{L"pcm24", "PCM24", int24_le, ASIOSTInt24LSB, false},
    MeasurementInput{L"pcm32", "PCM32", int32_le, ASIOSTInt32LSB, false},
    MeasurementInput{L"ext24in32", "Extensible PCM24/32", int24_in32_le, ASIOSTInt24LSB, true},
    MeasurementInput{L"float32", "Float32", float32_le, ASIOSTFloat32LSB, false},
    MeasurementInput{L"extfloat32", "Extensible Float32", float32_le, ASIOSTFloat32LSB, true},
};

struct MeasurementFormat {
    const MeasurementInput& input;
    ASIOSampleType output_type;
    SampleFormat output;
};

MeasurementFormat measurement_format(std::wstring_view input_name, std::wstring_view output_name,
    std::uint32_t rate)
{
    const auto input = std::ranges::find(inputs, input_name, &MeasurementInput::name);
    if (input == inputs.end()) {
        throw std::invalid_argument("The optional prefetch input format must be pcm16, pcm24, pcm32, ext24in32, float32 or extfloat32");
    }
    ASIOSampleType output_type = input->default_output;
    if (!output_name.empty()) {
        bool found = false;
        for (ASIOSampleType type = 0; type < ASIOSTLastEntry; ++type) {
            const auto format = wavplay::asio::to_sample_format(type);
            if (format &&
                std::ranges::equal(output_name, std::string_view(wavplay::asio::sample_type_name(type)))) {
                output_type = type;
                found = true;
                break;
            }
        }
        if (!found) { throw std::invalid_argument("Expected an ASIO integer or Float32/64 output name"); }
    }
    const auto output = *wavplay::asio::to_sample_format(output_type);
    if (!wavplay::audio::find_planar_converter({rate, 2, input->sample}, output)) {
        throw std::invalid_argument("Prefetch input cannot be converted losslessly to the requested output");
    }
    return {*input, output_type, output};
}

wavplay_test::Bytes measurement_wave(const MeasurementInput& input, std::uint32_t rate, std::uint32_t frames)
{
    using namespace wavplay_test;
    if (input.sample.kind == SampleKind::floating_point) {
        return wave({input.extensible ? extensible_float32(2, rate, 3) : float32(2, rate),
            fact(frames), float32_ramp(2, frames)});
    }
    if (input.extensible) {
        return wave({extensible_pcm(2, rate, 24, 32, 3), pcm24_in32_ramp(2, frames)});
    }
    const auto bits = input.sample.valid_bits;
    return wave({pcm_integer(2, rate, bits), bits == 16 ? pcm16_ramp(2, frames) :
        bits == 24 ? pcm24_ramp(2, frames) : pcm32_ramp(2, frames)});
}

IO_COUNTERS process_io()
{
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        wavplay::core::throw_last_error("GetProcessIoCounters");
    }
    return counters;
}

double timestamp_cycles_per_us()
{
    LARGE_INTEGER frequency{}, start{}, end{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start)) {
        wavplay::core::throw_last_error("QueryPerformanceCounter calibration");
    }
    const auto first = __rdtsc();
    Sleep(100);
    const auto last = __rdtsc();
    if (!QueryPerformanceCounter(&end)) { wavplay::core::throw_last_error("QueryPerformanceCounter"); }
    if (end.QuadPart <= start.QuadPart || last <= first) { throw std::runtime_error("Invalid calibration interval"); }
    const double us = static_cast<double>(end.QuadPart - start.QuadPart) * 1e6 /
        static_cast<double>(frequency.QuadPart);
    return static_cast<double>(last - first) / us;
}

struct Counters {
    std::uint64_t reads, bytes, cycles;
};

Counters sample(HANDLE producer)
{
    const auto io = process_io();
    ULONG64 cycles{};
    if (!QueryThreadCycleTime(producer, &cycles)) { wavplay::core::throw_last_error("QueryThreadCycleTime"); }
    return {io.ReadOperationCount, io.ReadTransferCount, cycles};
}

struct Range {
    double median;
    std::uint64_t min, max;
};

Range summarize(std::vector<Counters>& wakes, std::uint64_t Counters::*field)
{
    std::ranges::sort(wakes, {}, field);
    const auto n = wakes.size();
    return {(static_cast<double>(wakes[(n - 1) / 2].*field) + static_cast<double>(wakes[n / 2].*field)) / 2,
        wakes.front().*field, wakes.back().*field};
}

} // namespace

void measure_prefetch(std::wstring_view input_name, std::wstring_view output_name)
{
    using namespace wavplay_test;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t rate = 384000;
    constexpr std::size_t repetitions = 9;
    const auto [input, output_type, output] = measurement_format(input_name, output_name, rate);
    TempFile file;
    const auto* path = file.write(measurement_wave(input, rate, 2 * rate));

    std::printf("%u kHz stereo %s, 2 seconds, median of %zu primings after warm-up\n",
        rate / 1000, input.label, repetitions);
    if (input.sample == int24_in32_le) { std::puts("Includes whole-file unused-bit validation before initial fill."); }
    std::printf("Output: %s\n", wavplay::asio::sample_type_name(output_type));
    std::puts("block_frames  prime_us  process_reads");
    for (const std::size_t width : {47, 64, 96, 256}) {
        {
            const auto warmup = make_ring(path, {}, width, output);
        }
        std::array<std::int64_t, repetitions> times;
        std::array<std::uint64_t, repetitions> reads;
        for (std::size_t i = 0; i < repetitions; ++i) {
            const auto before = process_io().ReadOperationCount;
            const auto start = Clock::now();
            const auto primed = make_ring(path, {}, width, output);
            times[i] = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            reads[i] = process_io().ReadOperationCount - before;
        }
        std::ranges::sort(times);
        std::ranges::sort(reads);
        std::printf("%12zu  %8lld  %13llu\n", width, times[repetitions / 2], reads[repetitions / 2]);
    }
}

namespace {

struct StreamCondition { std::uint32_t rate, width; };
constexpr StreamCondition stream_conditions[]{
    {44100, 8}, {44100, 16}, {44100, 64}, {88200, 16}, {176400, 16}, {96000, 256}};
constexpr std::uint32_t comparison_seconds = 30;

void measure_stream_condition(const MeasurementFormat& format, StreamCondition condition,
    double cycles_per_us, std::optional<std::size_t> wakes_per_ring)
{
    using namespace wavplay_test;
    using namespace wavplay::stream;
    using Clock = std::chrono::steady_clock;
    const auto [rate, width] = condition;
    // The longer comparison gives the least frequent setting at least 20 refills.
    const std::uint64_t seconds = wakes_per_ring ? comparison_seconds : 13;
    constexpr std::uint64_t ns_per_second = 1000000000;
    const auto blocks = seconds * rate / width;
    const auto frames = static_cast<std::uint32_t>(blocks * width);
    TempFile file;
    const auto* path = file.write(measurement_wave(format.input, rate, frames));
    Bytes left(width * format.output.bytes()), right(left.size());

    DWORD producer_id{};
    PrefetchRing ring([&] {
        producer_id = GetCurrentThreadId();
        return PrefetchRing::PreparedTracks{retain_playable_track(path)};
    }, width, format.output, 0, {}, wakes_per_ring.value_or(default_prefetch_policy.producer_wakes_per_ring));
    wavplay::core::UniqueHandle producer(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, producer_id));
    if (!producer) { wavplay::core::throw_last_error("OpenThread"); }
    auto consumer = ring.consumer();
    const auto primed = sample(producer.get());
    auto previous = primed;
    std::optional<Counters> wake_start;
    std::vector<Counters> wakes;
    std::uint64_t consumed_blocks{}, consumed_frames{}, empty_count{};
    bool eof{};

    const auto start = Clock::now();
    while (consumed_blocks < blocks) {
        Sleep(10);
        // Sample at the end of the preceding batch's sleep, before the next pop
        // can wake the producer. Close on the first quiet period to include the
        // conversion and return to waiting after the last ReadFile.
        const auto now = sample(producer.get());
        if (now.reads != previous.reads) {
            if (!wake_start) { wake_start = previous; }
        } else if (wake_start) {
            wakes.push_back({now.reads - wake_start->reads, now.bytes - wake_start->bytes,
                now.cycles - wake_start->cycles});
            wake_start.reset();
        }
        previous = now;

        const auto elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        const auto due_blocks = std::min(elapsed_ns, seconds * ns_per_second) * rate / (width * ns_per_second);
        while (consumed_blocks < due_blocks) {
            const auto popped = consumer.pop(left.data(), right.data());
            if (popped.status == BlockStatus::empty) {
                ++empty_count;
                throw std::runtime_error("A due block was empty");
            }
            if (popped.status == BlockStatus::read_error) { throw std::runtime_error("Producer read failure"); }
            ++consumed_blocks;
            consumed_frames += popped.valid_frames;
            eof = popped.status == BlockStatus::last;
            if (popped.valid_frames != width || eof != (consumed_blocks == blocks)) {
                throw std::runtime_error("Unexpected valid-frame count or EOF position");
            }
        }
    }
    // Normally the ring drains for many quiet periods after the final read.
    const auto drained = sample(producer.get());
    if (wake_start || drained.reads != previous.reads) {
        const auto from = wake_start.value_or(previous);
        wakes.push_back({drained.reads - from.reads, drained.bytes - from.bytes, drained.cycles - from.cycles});
    }
    ring.stop();
    if (ring.read_failed() || consumed_frames != frames || !eof) { throw std::runtime_error("Incomplete stream"); }
    if (wakes.size() < 20) { throw std::runtime_error("Fewer than 20 refill wakes observed"); }

    const auto reads = summarize(wakes, &Counters::reads);
    const auto cpu = summarize(wakes, &Counters::cycles);
    const auto bytes = summarize(wakes, &Counters::bytes);
    std::printf("%7u %12u %5zu %9.1f %9llu %9llu %10.3f %10.3f %10.3f %12.0f %8llu %3u %5llu",
        rate, width, wakes.size(), reads.median, reads.min, reads.max, cpu.median / cycles_per_us,
        static_cast<double>(cpu.min) / cycles_per_us, static_cast<double>(cpu.max) / cycles_per_us,
        bytes.median, consumed_frames, eof ? 1U : 0U, empty_count);
    if (wakes_per_ring) {
        const auto cpu_us = static_cast<double>(drained.cycles - primed.cycles) / cycles_per_us;
        const auto audio_seconds = static_cast<double>(consumed_frames) / rate;
        std::printf(" %12llu %12.3f %12.3f", bytes.max, cpu_us, cpu_us / audio_seconds);
    }
    std::putchar('\n');
    std::fflush(stdout);
}

} // namespace

void measure_prefetch_stream(std::wstring_view input_name, std::wstring_view output_name,
    std::optional<std::size_t> wakes_per_ring)
{
    const auto format = measurement_format(input_name, output_name, stream_conditions[0].rate);
    const auto cycles_per_us = timestamp_cycles_per_us();
    std::printf("Paced stereo %s -> %s, Sleep(10), cached WAV, timestamp cycles/us=%.3f\n",
        format.input.label, wavplay::asio::sample_type_name(format.output_type), cycles_per_us);
    std::puts("CPU us approximate producer on-CPU time. Reads and source bytes are post-prime process I/O.");
    if (wakes_per_ring) {
        std::printf("Refill comparison: wakes_per_ring=%zu seconds=%u\n", *wakes_per_ring, comparison_seconds);
    }
    std::printf("rate_hz block_frames wakes reads_med reads_min reads_max cpu_us_med cpu_us_min cpu_us_max source_B_med frames eof empty%s\n",
        wakes_per_ring ? " source_B_max cpu_us_total cpu_us_per_second" : "");
    for (const auto condition : stream_conditions) {
        measure_stream_condition(format, condition, cycles_per_us, wakes_per_ring);
    }
}

void check_prefetch_pcm(std::wstring_view input_name, std::size_t wakes_per_ring)
{
    using namespace wavplay_test;
    using namespace wavplay::stream;
    if (input_name != L"pcm16" && input_name != L"pcm24") {
        throw std::invalid_argument("Expected a PCM16 or PCM24 stereo ramp");
    }
    const auto format = measurement_format(input_name, L"ASIOSTInt32LSB", stream_conditions[0].rate);
    const unsigned shift = 32U - format.input.sample.valid_bits;
    std::printf("PCM check: %s -> ASIOSTInt32LSB wakes_per_ring=%zu\n", format.input.label, wakes_per_ring);
    for (const auto [rate, width] : stream_conditions) {
        const auto frames = comparison_seconds * rate / width * width;
        TempFile file;
        const auto* path = file.write(measurement_wave(format.input, rate, frames));
        PrefetchRing ring([&] { return PrefetchRing::PreparedTracks{retain_playable_track(path)}; },
            width, format.output, 0, {}, wakes_per_ring);
        std::vector<std::uint32_t> left(width), right(width);
        for (std::uint32_t first = 0; first < frames; first += width) {
            const auto block = await_block(ring, left.data(), right.data());
            const auto expected_status = first + width == frames ? BlockStatus::last : BlockStatus::ready;
            if (block.status != expected_status || block.valid_frames != width) {
                throw std::runtime_error("Unexpected block or EOF at frame " + std::to_string(first));
            }
            // The generated source uses the low 16/24 bits of the frame index on the
            // left and its complement on the right. PCM32 output fills unused low bits with zero.
            for (std::uint32_t index = 0; index < width; ++index) {
                const auto frame = first + index;
                if (left[index] != (frame << shift) || right[index] != (~frame << shift)) {
                    throw std::runtime_error("Unexpected output PCM at frame " + std::to_string(frame));
                }
            }
        }
        std::printf("PASS rate_hz=%u block_frames=%u frames=%u eof=1\n", rate, width, frames);
    }
}
