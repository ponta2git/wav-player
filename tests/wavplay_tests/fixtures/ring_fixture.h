#pragma once

#include "wavplay/stream/gapless_run.h"
#include "wavplay/stream/prefetch_ring.h"
#include "framework/failure_reporting.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

// Shared by the prefetch ring, segment and rate transition suites and the
// --measure-prefetch diagnostic: build a ring through the product's
// prepared-list constructor, poll it from the test thread, and hold byte-range
// locks that make the producer's next read fail.
namespace wavplay_test {

// A ring over `first` and, unless `second` is no track (std::monostate or a null
// path), a second track, prepared on the ring's producer as the playback
// request prepares a run. A second track that does not open (std::runtime_error),
// has another rate or cannot be converted losslessly to `output` ends the run
// after the first, as in request.cpp; misuse such as a partly read reader still
// throws from construction. The preparation moves both inputs out of this
// frame's parameters during construction (they outlive it), so no copy of a
// reader stays behind and file release remains observable. PrefetchRing cannot
// move; the prvalue initializes the caller's object directly.
[[nodiscard]] inline wavplay::stream::PrefetchRing make_ring(wavplay::stream::TrackInput first,
    wavplay::stream::TrackInput second, std::size_t block_frames, wavplay::audio::SampleFormat output,
    std::uint64_t first_frame = 0)
{
    using namespace wavplay;
    return stream::PrefetchRing([&first, &second, output] {
        stream::PrefetchRing::PreparedTracks tracks{stream::retain_playable_track(std::move(first))};
        if (stream::is_no_track(second)) {
            return tracks;
        }
        std::shared_ptr<media::WavReader> next;
        try {
            next = stream::retain_playable_track(std::move(second));
        } catch (const std::runtime_error&) {
            return tracks;
        }
        if (next->info().format.sample_rate == tracks.front()->info().format.sample_rate &&
            audio::find_planar_converter(next->info().format, output)) {
            tracks.push_back(std::move(next));
        }
        return tracks;
    }, block_frames, output, first_frame);
}

inline wavplay::stream::PoppedBlock await_block(wavplay::stream::PrefetchRing& ring, void* left, void* right)
{
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
    do {
        const auto result = ring.consumer().pop(left, right);
        if (result.status != wavplay::stream::BlockStatus::empty) {
            return result;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("expected prefetch progress, ring stayed empty");
}

// A second handle on the file whose LockFile ranges deny the reader's ReadFile.
inline auto open_lock_handle(const wchar_t* path)
{
    const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open lock handle");
    }
    return std::unique_ptr<void, decltype(&CloseHandle)>(handle, &CloseHandle);
}

} // namespace wavplay_test
