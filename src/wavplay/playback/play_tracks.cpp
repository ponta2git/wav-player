#include "wavplay/playback/play_tracks.h"

#include "wavplay/asio/driver_session.h"
#include "wavplay/core/memory.h"
#include "wavplay/playback/request.h"
#include "wavplay/playback/segment.h"

#include <atomic>
#include <utility>
#include <vector>

namespace wavplay::playback {

PlaybackResult play_tracks(asio::DriverSession& session, std::vector<stream::TrackInput> tracks,
    PlaybackDiagnostics* diagnostics)
{
    const RequestClaim claim;
    asio::DriverLease device(session);
    const core::ResidentObject<std::atomic<bool>> never_requested;
    SegmentControl control{*never_requested, *never_requested};
    control.request_pages_locked = never_requested.locked();
    return run_request(device, std::move(tracks), control, {.diagnostics = diagnostics});
}

PlaybackResult play_tracks(asio::DriverSession& session, stream::TrackInput first, stream::TrackInput second,
    PlaybackDiagnostics* diagnostics)
{
    const RequestClaim claim;
    asio::DriverLease device(session); // Released before the claim.
    // This entry point offers no Pause or Stop: neither request is ever set.
    const core::ResidentObject<std::atomic<bool>> never_requested;
    SegmentControl control{*never_requested, *never_requested};
    control.request_pages_locked = never_requested.locked();
    return run_request(device, std::move(first), std::move(second), control, diagnostics);
}

} // namespace wavplay::playback
