#pragma once

#include "wavplay/playback/playback_types.h"
#include "wavplay/stream/gapless_run.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace wavplay::asio {
class DriverLease;
}

namespace wavplay::playback {

struct SegmentControl;

// ASIO callbacks have no driver identity, so one playback request at a time owns
// the process-wide callbacks and notification signal. Public entry points hold the
// claim from setup through the final result and position commit, so no other
// request can replace the signal generation in between.
class RequestClaim {
public:
    RequestClaim();
    ~RequestClaim();
    RequestClaim(const RequestClaim&) = delete;
    RequestClaim& operator=(const RequestClaim&) = delete;
};

// One playback request on a leased session driver. Same-rate tracks share one
// gapless segment. Each rate transition drains and stops the preceding segment,
// reopens the session's driver, then changes its clock only when necessary.
// Every segment starts with the lease's start silence. The first track starts at
// first_frame. `control` carries the request's Pause/Stop flags and observer; it
// does not select the tracks.
[[nodiscard]] PlaybackResult run_request(asio::DriverLease& device, stream::TrackInput first_input,
    stream::TrackInput second_input, SegmentControl& control, PlaybackDiagnostics* diagnostics = nullptr,
    std::uint64_t first_frame = 0);

// Control-side view of the current run. Live submitted counts never inspect a
// producer cursor; they are mapped onto these immutable validated frame counts.
struct RequestProgress {
    std::size_t first_track{};
    std::uint64_t first_frame{};
    std::vector<std::shared_ptr<media::WavReader>> tracks;
    std::vector<std::uint64_t> frame_counts;
    std::optional<std::size_t> rejected_track;
    // Preserve known metadata for a refused rate/format/width; a failed open
    // leaves this empty, so the controller cannot display the prior duration.
    std::shared_ptr<media::WavReader> rejected_source;
    // After a read_error result: the producer's failure (PrefetchRing::read_error).
    // Read on the control thread after the ring has stopped.
    std::exception_ptr fault_error{};
    // Why rejected_track was refused, when an exception says so: a failed open
    // or validation of a following item, or an unsupported buffer width with
    // the WAV rate. Rate and format refusals leave it null. Written on the
    // control thread after the reader's priming or join, like `rejected_track`.
    std::exception_ptr rejected_error{};
};

// How a request ends when the device refuses a track.
enum class RefusalPolicy : std::uint8_t {
    // A track list without selection: the first track's unknown or refused clock
    // reports first_rate_rejected and its unavailable output pair throws. A later
    // track reports next_rate_rejected or next_track_rejected. An unsupported
    // buffer width throws for any track.
    throw_first,
    // A playlist: every refusal is a recoverable next_rate_rejected or
    // next_track_rejected that names the refused item in RequestProgress.
    // Requires RequestOptions::progress; run_request throws
    // std::invalid_argument without it.
    record_each,
};

struct RequestOptions {
    PlaybackDiagnostics* diagnostics{};
    std::uint64_t first_frame{}; // Where the first track starts.
    RequestProgress* progress{};
    RefusalPolicy refusals{RefusalPolicy::throw_first};
    // The first input's playlist path, so later entries of the same path share
    // its protected handle. Empty when the first input is not from a playlist.
    std::wstring_view first_path{};
};

// The same request over an ordered input list. Repeated paths share one
// protected handle, so borrowed paths and options.first_path must stay valid
// until the request returns, not only until their track opens.
[[nodiscard]] PlaybackResult run_request(asio::DriverLease& device, std::vector<stream::TrackInput> inputs,
    SegmentControl& control, const RequestOptions& options = {});

} // namespace wavplay::playback
