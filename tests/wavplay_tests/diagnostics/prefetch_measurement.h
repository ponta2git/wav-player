#pragma once

#include <string_view>

// --measure-prefetch [pcm16|pcm24|pcm32|ext24in32|float32|extfloat32] [ASIOST...]: prints median time and read operations needed to
// prime a PrefetchRing from a generated two-second 384 kHz WAV at several widths.
// Extensible 24/32 includes the whole-file unused-bit validation before priming.
void measure_prefetch(std::wstring_view input_name = L"pcm16", std::wstring_view output_name = {});

// --measure-prefetch-stream accepts the same formats. Consume a generated
// eight-second 96 kHz WAV in Sleep(10) batches at the default timer resolution.
// Reports refill CPU/read counts and harness wake lateness, fails immediately
// on an empty due block, and verifies frame count/EOF. No ASIO or power measurement.
void measure_prefetch_stream(std::wstring_view input_name = L"pcm16", std::wstring_view output_name = {});
