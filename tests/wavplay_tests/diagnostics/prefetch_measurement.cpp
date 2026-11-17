#include "diagnostics/prefetch_measurement.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/audio/planar_conversion.h"
#include "wavplay/stream/prefetch_ring.h"
#include "fixtures/ring_fixture.h"
#include "fixtures/wav_builder.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <system_error>

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

std::uint64_t process_reads()
{
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessIoCounters");
    }
    return counters.ReadOperationCount;
}

std::uint64_t process_cpu_100ns()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetProcessTimes");
    }
    const auto ticks = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
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
            const auto before = process_reads();
            const auto start = Clock::now();
            const auto primed = make_ring(path, {}, width, output);
            times[i] = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            reads[i] = process_reads() - before;
        }
        std::ranges::sort(times);
        std::ranges::sort(reads);
        std::printf("%12zu  %8lld  %13llu\n", width, times[repetitions / 2], reads[repetitions / 2]);
    }
}

void measure_prefetch_stream(std::wstring_view input_name, std::wstring_view output_name)
{
    using namespace wavplay_test;
    using wavplay::stream::BlockStatus;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t rate = 96000;
    constexpr std::uint32_t frames = 8 * rate;
    constexpr std::size_t width = 256;
    constexpr std::uint64_t blocks = frames / width;
    constexpr std::uint64_t ns_per_second = 1000000000;
    static_assert(frames % width == 0);
    const auto [input, output_type, output] = measurement_format(input_name, output_name, rate);
    TempFile file;
    const auto* path = file.write(measurement_wave(input, rate, frames));
    Bytes left(width * output.bytes()), right(left.size());

    std::printf("%u kHz stereo %s, 8 seconds, %zu frames per block\n", rate / 1000, input.label, width);
    std::printf("Output: %s\n", wavplay::asio::sample_type_name(output_type));
    std::puts("Paced refill diagnostic: Sleep(10), default timer, no ASIO driver opened.");
    std::puts("This is not an ASIO deadline or power measurement.");
    std::puts("CPU is process kernel+user time (coarse OS accounting), including harness and producer.");
    std::puts("Read operations are process counts for a generated cached WAV, not physical disk I/O.");
    std::puts("Maximum harness wake lateness is relative to the earliest due block in each batch.");
    if (input.sample == int24_in32_le) { std::puts("Priming includes whole-file unused-bit validation."); }

    const auto reads_before = process_reads();
    const auto prime_start = Clock::now();
    auto ring = make_ring(path, {}, width, output);
    const auto prime_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - prime_start).count();
    auto consumer = ring.consumer();
    const auto reads_primed = process_reads();
    const auto cpu_start = process_cpu_100ns();
    const auto start = Clock::now();
    std::uint64_t consumed_blocks{}, consumed_frames{}, empty_count{}, max_lateness_ns{};
    bool eof{};
    const char* failure = nullptr;

    // Consume all blocks due at each wake, without polling or changing timer
    // resolution. This deliberately measures paced ring refills, not an ASIO
    // callback's individual deadlines or the machine's electrical activity.
    while (consumed_blocks < blocks && !eof && !failure) {
        Sleep(10);
        const auto elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        // Clamp before multiplication, including after an arbitrarily late wake.
        const auto due_blocks = std::min(elapsed_ns, 8 * ns_per_second) * rate / (width * ns_per_second);
        if (due_blocks > consumed_blocks) {
            const auto next_deadline_ns = (consumed_blocks + 1) * width * ns_per_second / rate;
            max_lateness_ns = std::max(max_lateness_ns, elapsed_ns - next_deadline_ns);
        }
        while (consumed_blocks < due_blocks) {
            const auto popped = consumer.pop(left.data(), right.data());
            if (popped.status == BlockStatus::empty) {
                ++empty_count;
                failure = "A due block was empty; the diagnostic does not wait for recovery";
                break;
            }
            if (popped.status == BlockStatus::read_error) {
                failure = "The producer reported a read failure";
                break;
            }
            ++consumed_blocks;
            consumed_frames += popped.valid_frames;
            eof = popped.status == BlockStatus::last;
            if (popped.valid_frames != width || eof != (consumed_blocks == blocks)) {
                failure = "Unexpected valid-frame count or EOF position";
                break;
            }
        }
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    const auto cpu_100ns = process_cpu_100ns() - cpu_start;
    ring.stop();
    const auto reads_after = process_reads();
    if (ring.read_failed()) { failure = "The producer reported a read failure"; }
    if (!failure && (consumed_frames != frames || !eof)) { failure = "Incomplete stream or missing EOF"; }

    std::printf("prime_us=%lld  post_prime_elapsed_us=%lld  post_prime_process_cpu_ms=%.3f\n",
        prime_us, elapsed_us, static_cast<double>(cpu_100ns) / 10000.0);
    std::printf("process_reads_total=%llu  process_reads_post_prime=%llu\n",
        reads_after - reads_before, reads_after - reads_primed);
    std::printf("frames=%llu/%u  blocks=%llu/%llu  eof=%u  empty=%llu  producer_failed=%u\n",
        consumed_frames, frames, consumed_blocks, blocks, eof ? 1U : 0U, empty_count, ring.read_failed() ? 1U : 0U);
    std::printf("max_harness_wake_lateness_us=%.3f\n", static_cast<double>(max_lateness_ns) / 1000.0);
    if (failure) { throw std::runtime_error(failure); }
}
