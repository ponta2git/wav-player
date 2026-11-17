#pragma once

#include "wavplay/playback/playback_types.h"
#include "wavplay/stream/gapless_run.h"

namespace wavplay::asio {
class DriverSession;
}

namespace wavplay::playback {

// Blocking control-thread playback of WAVs (paths or validated, unread
// readers) on the session's adjacent output pair at its exact requested width, or the driver's
// preferred width for setting 0. Each segment validates the current clock's range.
// Same-rate WAVs play
// gaplessly in one segment. The clock is adjusted for the first WAV, and for a
// different-rate second WAV after the first has drained: the session restores
// its original clock, releases the driver and initializes it again before that
// second WAV. The session tracks clock changes, including uncertain hardware
// failures, until close(). Every segment starts with the session's start silence.
// Rate refusals identify the unstarted track. Driver API faults throw; quiesce and
// release a failed driver before retrying. Optional diagnostics are reset for each
// accepted request. Verified allocations remain available on return or exception,
// even if a later start or segment fails.
[[nodiscard]] PlaybackResult play_tracks(asio::DriverSession& session, stream::TrackInput first,
    stream::TrackInput second = {}, PlaybackDiagnostics* diagnostics = nullptr);
// Unlimited ordered input list, with the same gapless and rate-transition rules.
[[nodiscard]] PlaybackResult play_tracks(asio::DriverSession& session, std::vector<stream::TrackInput> tracks,
    PlaybackDiagnostics* diagnostics = nullptr);

} // namespace wavplay::playback
