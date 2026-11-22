#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

// --measure-prefetch [pcm16|pcm24|pcm32|ext24in32|float32|extfloat32] [ASIOST...]: prints median time and read operations needed to
// prime a PrefetchRing from a generated two-second 384 kHz WAV at several widths.
// Extensible 24/32 includes the whole-file unused-bit validation before priming.
void measure_prefetch(std::wstring_view input_name = L"pcm16", std::wstring_view output_name = {});

// --measure-prefetch-stream accepts the same formats. Pace six rate/width
// conditions and report producer cycles, reads and bytes per refill wake.
// Each stream must reach EOF with every expected frame and no empty due block.
// Specifying a wake frequency selects a longer, common-duration comparison.
void measure_prefetch_stream(std::wstring_view input_name = L"pcm16", std::wstring_view output_name = {},
    std::optional<std::size_t> wakes_per_ring = {});

// Compare a notification pattern on the same paced stereo PCM24 stream.
void measure_refill_pattern(std::wstring_view pattern_name);

// Check the full stereo PCM16/24 ramps against their 32-bit output outside timing.
void check_prefetch_pcm(std::wstring_view input_name, std::size_t wakes_per_ring,
    std::optional<std::wstring_view> pattern_name = {});
