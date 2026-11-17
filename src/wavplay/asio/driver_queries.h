#pragma once

#include "wavplay/asio/asio_sdk.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace wavplay::asio {

// getBufferSize: the driver's buffer widths in frames.
struct BufferSizeRange {
    long minimum, maximum, preferred, granularity;
};

// What a stopped driver reports for outputs 1/2, without allocating buffers.
struct DriverCapabilities {
    ASIOSampleRate sample_rate;
    long output_channels;
    std::array<ASIOSampleType, 2> output_types;
    BufferSizeRange buffer_size;
};

// Control-thread inquiries of an initialized driver. None of them allocates
// buffers, sets the clock or starts the device.

// Borrow an initialized driver and inspect outputs 1/2.
[[nodiscard]] DriverCapabilities query_capabilities(IASIO& driver);

// The current clock, or nullopt for ASE_NoClock or a value that is not a positive
// finite rate. Other errors throw. Never compares the driver's untrusted double
// in floating point.
[[nodiscard]] std::optional<ASIOSampleRate> current_sample_rate(IASIO& driver);

// Zero when the driver reports no channels (ASE_NotPresent).
[[nodiscard]] long output_channel_count(IASIO& driver);

// Rejects a preferred width outside [minimum, maximum] or contradicting the granularity.
[[nodiscard]] BufferSizeRange query_buffer_size(IASIO& driver);
// Select preferred for zero, or validate an exact positive request. Never rounds
// or falls back. An invalid request throws invalid_argument with
// buffer_frames_refusal's text; a contradictory driver range throws
// runtime_error, as query_buffer_size does.
[[nodiscard]] std::size_t select_buffer_frames(const BufferSizeRange& range, long requested);
// The same rule without exceptions: true exactly when select_buffer_frames
// returns a width, so a caller can refuse a request without catching.
[[nodiscard]] bool accepts_buffer_frames(const BufferSizeRange& range, long requested) noexcept;
// Why `requested` is unsupported: the requested width and the driver's minimum,
// preferred, maximum and granularity. A nonzero `sample_rate` (the WAV's
// integral rate the range was queried at) is appended, for a caller that refuses
// the width through accepts_buffer_frames instead of the selector's exception.
[[nodiscard]] std::string buffer_frames_refusal(const BufferSizeRange& range, long requested,
    std::uint32_t sample_rate = 0);

// Queries one output. Throws if the driver answers for a different channel.
[[nodiscard]] ASIOError query_output_info(IASIO& driver, long channel, ASIOChannelInfo& info);

// getLatencies: the raw output latency in frames, before any drain accounting.
[[nodiscard]] long output_latency(IASIO& driver);

// ASIO 2.3 drivers only have to report overloads after this inquiry. Older
// drivers may decline it.
void inquire_overload_reporting(IASIO& driver);

// After createBuffers: the configured outputs in order, four non-null, mutually disjoint
// halves, each writable for channel_bytes. Throws before any half is touched.
void validate_output_buffers(const ASIOBufferInfo (&buffers)[2], std::size_t channel_bytes, long output_pair = 1);

} // namespace wavplay::asio
