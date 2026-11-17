#include "wavplay/playback/request.h"

#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/clock.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/asio/sample_types.h"
#include "wavplay/playback/output_format.h"
#include "wavplay/playback/segment.h"
#include "wavplay/stream/prefetch_ring.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wavplay::playback {
namespace {

constinit std::atomic_flag reserved{};

// Called while the request is playing: before its first segment, or after the
// driver was reopened for a following rate.
PlaybackResult change_rate(asio::DriverLease& device, ASIOSampleRate previous, std::uint32_t requested,
    PlaybackSignal& signal, PlaybackSignal::Token& token, PlaybackResult rejection)
{
    // A capability inquiry leaves the old clock in force. Publish the new
    // expectation only after the inquiry, immediately before setting the rate.
    auto& driver = device.driver();
    const auto capability = driver.canSampleRate(requested);
    if (capability == ASE_NoClock) {
        // Record the refusal in this phase, so the claim's exit does not stamp
        // the request as a driver fault. A fault that raced the inquiry wins.
        signal.finish(token, rejection);
        return signal.result();
    }
    asio::throw_if_error(capability, "ASIO canSampleRate failed");
    if (!signal.advance_clock(token, requested, PlaybackResult::playing)) {
        return signal.result();
    }
    if (signal.snapshot() != token) {
        return signal.result();
    }
    const auto setting = device.set_sample_rate(requested, previous);
    // An external clock can prevent the driver from setting this rate.
    if (setting == ASE_NoClock || setting == ASE_InvalidMode) {
        signal.finish(token, rejection);
    } else {
        asio::throw_if_error(setting, "ASIO setSampleRate failed");
        if (signal.snapshot() == token) {
            const auto actual = asio::current_sample_rate(driver);
            if (!actual || !signal.matches_rate(*actual)) {
                signal.finish(token, rejection);
            }
        }
    }
    return signal.result();
}

// A refused single track ends the request, so name the configured pair's types
// for diagnosis. A pair beyond the driver's output count keeps the plain text
// and is never queried by channel index.
[[noreturn]] void refuse_output_pair(IASIO& driver, long output_pair)
{
    std::string message = "Configured ASIO output pair is unavailable or has no common lossless format for this WAV";
    ASIOChannelInfo left{}, right{};
    bool named{};
    try {
        named = asio::output_channel_count(driver) > output_pair &&
            asio::query_output_info(driver, output_pair - 1, left) == ASE_OK &&
            asio::query_output_info(driver, output_pair, right) == ASE_OK;
    } catch (const std::runtime_error&) {
        // The types only annotate the refusal; a fault in these queries must not replace it.
    }
    if (named) {
        message += ": ";
        message += asio::sample_type_name(left.type);
        message += '/';
        message += asio::sample_type_name(right.type);
    }
    throw std::runtime_error(message);
}

// The request's inputs, each opened once. Counts every use of a path up front
// and keeps one protected handle only while a later entry of that path still
// needs it, including across rate transitions. Keys borrow the same path
// storage as the request inputs.
// The control thread opens only input 0, before any ring exists. Every later
// input, including the first track of a following rate run, is opened by a
// ring's reader thread in its prepare step, which runs only while the control
// thread waits for priming in that ring's constructor. Such a first track
// reaches the control thread as `next` after that ring has retired, so the two
// threads never use the cache at once.
class SourceCache {
public:
    SourceCache(std::vector<stream::TrackInput> inputs, std::wstring_view first_path)
        : inputs_(std::move(inputs)), first_path_(first_path)
    {
        for (std::size_t index = 0; index < inputs_.size(); ++index) {
            if (const auto key = key_of(index); !key.empty()) { ++entries_[key].remaining; }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return inputs_.size(); }

    // Opens input `index`, or shares the handle of an earlier entry of its path.
    // Call once per index. Throws as stream::retain_playable_track.
    [[nodiscard]] std::shared_ptr<media::WavReader> open(std::size_t index)
    {
        const auto key = key_of(index);
        if (key.empty()) { return stream::retain_playable_track(std::move(inputs_[index])); }
        auto& entry = entries_.at(key);
        if (!entry.reader) { entry.reader = stream::retain_playable_track(std::move(inputs_[index])); }
        if (--entry.remaining == 0) { return std::move(entry.reader); }
        return entry.reader;
    }

private:
    struct Entry {
        std::shared_ptr<media::WavReader> reader;
        std::size_t remaining{};
    };

    // The first input's playlist path, a borrowed path, or empty for an input
    // that cannot be shared (no track, or an already opened reader).
    [[nodiscard]] std::wstring_view key_of(std::size_t index) const noexcept
    {
        const auto* path = std::get_if<const wchar_t*>(&inputs_[index]);
        return index == 0 && !first_path_.empty() ? first_path_ :
            path && *path ? std::wstring_view(*path) : std::wstring_view{};
    }

    std::vector<stream::TrackInput> inputs_;
    std::wstring_view first_path_;
    std::map<std::wstring_view, Entry, std::less<>> entries_;
};

// The request's control poll for a ring's priming wait, run through the ring's
// wake hook on the control thread after each wake by the playback signal's
// event: the same delivery and Stop acceptance as every other setup boundary.
struct PrimingPoll {
    const SegmentControl& control;
    PlaybackSignal& signal;
    const PlaybackSignal::Token token;

    static void run(void* context) noexcept
    {
        const auto& self = *static_cast<const PrimingPoll*>(context);
        self.control.poll(self.signal, self.token);
    }
};

// Shows `tracks` as the current run, starting at `first_frame` of request item
// `track_index`. Before a ring exists this exposes the current track for a
// refusal; after priming it replaces that with the validated run.
void publish_progress(RequestProgress& progress, std::size_t track_index, std::uint64_t first_frame,
    std::vector<std::shared_ptr<media::WavReader>> tracks)
{
    progress.first_track = track_index;
    progress.first_frame = first_frame;
    progress.frame_counts.clear();
    progress.frame_counts.reserve(tracks.size());
    for (const auto& source : tracks) { progress.frame_counts.push_back(source->info().frame_count); }
    progress.tracks = std::move(tracks);
    progress.rejected_track.reset();
}

} // namespace

RequestClaim::RequestClaim()
{
    if (reserved.test_and_set(std::memory_order_acquire)) {
        throw std::runtime_error("ASIO playback already active");
    }
}

RequestClaim::~RequestClaim()
{
    auto& signal = notifications();
    signal.finish(signal.snapshot(), PlaybackResult::driver_error); // Close exceptional setup exits.
    reserved.clear(std::memory_order_release);
}

PlaybackResult run_request(asio::DriverLease& device, stream::TrackInput first_input,
    stream::TrackInput second_input, SegmentControl& control, PlaybackDiagnostics* diagnostics,
    std::uint64_t first_frame)
{
    std::vector<stream::TrackInput> inputs;
    inputs.push_back(std::move(first_input));
    if (!stream::is_no_track(second_input)) {
        inputs.push_back(std::move(second_input));
    }
    return run_request(device, std::move(inputs), control, {.diagnostics = diagnostics, .first_frame = first_frame});
}

PlaybackResult run_request(asio::DriverLease& device, std::vector<stream::TrackInput> inputs,
    SegmentControl& control, const RequestOptions& options)
{
    if (inputs.empty()) {
        throw std::invalid_argument("Playback requires a first track");
    }
    auto* const diagnostics = options.diagnostics;
    auto* const progress = options.progress;
    const bool playlist = options.refusals == RefusalPolicy::record_each;
    if (playlist && !progress) {
        // A recoverable refusal is useless to a caller that cannot learn which item it was.
        throw std::invalid_argument("Playlist playback requires progress to name a refused item");
    }
    if (diagnostics) {
        diagnostics->reset();
    }
    auto& signal = notifications();
    const auto begun = signal.try_begin(0, control.previous_idle_token);
    if (!begun) {
        // A notification can race TrackPlayer's idle check while its cursor is
        // reopened. Preserve that result without touching the driver or buffers.
        control.phase_token = control.previous_idle_token;
        return begun.error();
    }
    auto token = *begun;
    const auto interrupted = [&]() noexcept {
        control.phase_token = token;
        control.poll(signal, token);
        return signal.snapshot() != token;
    };
    if (interrupted()) {
        return signal.result();
    }
    asio::inquire_overload_reporting(device.driver());
    if (interrupted()) {
        return signal.result();
    }
    SourceCache sources(std::move(inputs), options.first_path);
    const auto record_refusal = [progress](std::size_t index,
        const std::shared_ptr<media::WavReader>& source, std::exception_ptr error = {}) noexcept {
        if (progress) {
            progress->rejected_track = index;
            progress->rejected_source = source;
            progress->rejected_error = std::move(error);
        }
    };
    auto track = sources.open(0);
    const auto original = asio::current_sample_rate(device.driver());
    if (!original) {
        if (playlist) { record_refusal(0, track); }
        signal.finish(token, playlist ? PlaybackResult::next_rate_rejected : PlaybackResult::first_rate_rejected);
        return signal.result();
    }
    if (!signal.advance_clock(token, *original, PlaybackResult::playing)) {
        return signal.result();
    }
    if (interrupted()) {
        return signal.result();
    }

    auto current = *original;
    bool first_track = true;
    std::size_t track_index{}, segment_index{};
    for (;;) {
        // The previous segment has fully retired. Keep counts local to this
        // sample-rate run, so any number of rates cannot overflow a grand total.
        control.submitted_frames = 0;
        if (diagnostics) {
            diagnostics->current_segment = segment_index;
            if (diagnostics->segments.size() <= segment_index) {
                diagnostics->segments.resize(segment_index + 1);
                diagnostics->buffer_capabilities.resize(segment_index + 1);
            }
        }
        const auto start_frame = first_track ? options.first_frame : 0;
        if (progress) {
            publish_progress(*progress, track_index, start_frame, {track});
        }
        const auto requested = track->info().format.sample_rate;
        if (!first_track) {
            // The old instance may report its restored clock before release. A fresh
            // instance has no confirmed clock until getSampleRate returns. Keep reset
            // and clock loss armed throughout this provisional phase of the same request.
            if (!signal.advance_clock(token, 0, PlaybackResult::completed)) {
                return signal.result();
            }
            auto& reopened_driver = device.reopen();
            if (signal.snapshot() != token) {
                return signal.result();
            }
            asio::inquire_overload_reporting(reopened_driver);
            const auto reopened = asio::current_sample_rate(reopened_driver);
            if (!reopened) {
                record_refusal(track_index, track);
                signal.finish(token, PlaybackResult::next_rate_rejected);
                return signal.result();
            }
            current = *reopened;
            if (!signal.advance_clock(token, current, PlaybackResult::playing)) {
                return signal.result();
            }
        }
        // A positive finite driver clock can still be subnormal. Comparing its
        // floating-point value would change MXCSR (or fault with denormals
        // unmasked); use the same bit-only contract as clock notifications.
        if (!asio::same_clock(current, static_cast<ASIOSampleRate>(requested))) {
            const auto refusal = first_track && !playlist ? PlaybackResult::first_rate_rejected : PlaybackResult::next_rate_rejected;
            const auto result = change_rate(device, current, requested, signal, token, refusal);
            if (result != PlaybackResult::playing) {
                if (is_track_refusal(result)) { record_refusal(track_index, track); }
                return result;
            }
        }

        auto& driver = device.driver(); // The same instance until this segment retires.
        const auto format = negotiate_output_format(driver, track->info().format, nullptr, device.output_pair());
        if (interrupted()) {
            return signal.result();
        }
        if (!format) {
            if (first_track && !playlist) {
                refuse_output_pair(driver, device.output_pair());
            }
            record_refusal(track_index, track);
            signal.finish(token, PlaybackResult::next_track_rejected);
            return signal.result();
        }
        const auto buffer_size = asio::query_buffer_size(driver);
        if (interrupted()) {
            return signal.result();
        }
        if (diagnostics) {
            // The clock is confirmed, but no allocation exists yet. Keep the
            // limits even when exact selection or createBuffers is refused.
            diagnostics->buffer_capabilities[segment_index] = BufferCapabilities{
                .sample_rate = static_cast<double>(requested), .minimum = buffer_size.minimum,
                .maximum = buffer_size.maximum, .preferred = buffer_size.preferred,
                .granularity = buffer_size.granularity};
        }
        // A playlist refuses only this item, with the selector's text and the
        // WAV rate as its cause. Other requests, including a track list without
        // selection, keep the selector's exception, which names the requested
        // width and the driver's limits.
        if (playlist && !asio::accepts_buffer_frames(buffer_size, device.buffer_frames())) {
            record_refusal(track_index, track, std::make_exception_ptr(std::invalid_argument(
                asio::buffer_frames_refusal(buffer_size, device.buffer_frames(), requested))));
            signal.finish(token, PlaybackResult::next_track_rejected);
            return signal.result();
        }
        const auto width = asio::select_buffer_frames(buffer_size, device.buffer_frames());
        std::shared_ptr<media::WavReader> next;
        std::vector<std::shared_ptr<media::WavReader>> run{track};
        std::size_t next_index = track_index + 1;
        bool rejected{};
        std::exception_ptr refusal; // Why `next` failed to open, if it did.
        {
            // The existing producer validates the whole run before priming. Its
            // control-thread constructor wait continues to dispatch STA messages
            // and runs the control poll on each wake by the playback signal's
            // event, so Stop, Close and Select-and-Play reach this request while
            // the run is prepared, not only once it has primed. The poll reaches
            // controller state and the TrackPlayer's progress fields (`progress`
            // above, which the reader never touches), and sets the request's
            // atomics and signal. Its wait consumes the auto-reset signal event;
            // that is harmless, because every waiter on that event rechecks its
            // state after each wake (the segment's first step polls again).
            // This step runs on the reader thread and mutates the control thread's
            // sources, run, next, next_index, rejected and refusal. Neither the
            // poll nor a dispatched message touches them, and the control thread
            // itself does not until the constructor has returned after priming
            // (or has joined the reader on failure), so they are never used at once.
            // It also reads the negotiated output format, which the control
            // thread does not change while priming, and the request's Stop
            // atomic and signal. Nothing else is shared; the advancing request
            // token stays with the control thread, so the step checks a copy.
            PrimingPoll priming{control, signal, token};
            stream::PrefetchRing queue([&sources, &run, &next, &next_index, &rejected, &refusal, requested,
                                           &output = *format, &stop = control.stop_requested, &signal, phase = token] {
                for (; next_index < sources.size(); ++next_index) {
                    // A Stop or Close, or a new phase (a driver reset or fault),
                    // ends the run before the next file is opened and scanned.
                    // `next` stays empty and nothing is refused: the truncated run
                    // primes, and the segment's first poll returns the result
                    // before createBuffers. Pause is deliberately not checked.
                    if (stop.load(std::memory_order_acquire) || signal.snapshot() != phase) { break; }
                    try { next = sources.open(next_index); }
                    catch (const std::runtime_error&) {
                        rejected = true;
                        refusal = std::current_exception();
                        break;
                    }
                    if (next->info().format.sample_rate != requested) { break; }
                    if (!audio::find_planar_converter(next->info().format, output)) {
                        rejected = true;
                        break;
                    }
                    run.push_back(std::move(next));
                }
                return run;
            }, width, *format, start_frame, {signal.event_handle(), PrimingPoll::run, &priming});
            if (progress) {
                publish_progress(*progress, track_index, start_frame, std::move(run));
            } else {
                run.clear();
            }
            track.reset();
            auto* setup = diagnostics ? &diagnostics->segments[segment_index] : nullptr;
            const auto result = run_segment(driver, queue, token, control, setup,
                device.start_silence(), device.output_pair());
            queue.stop();
            if (queue.read_failed()) {
                signal.invalidate(token, PlaybackResult::read_error);
                if (progress) { progress->fault_error = queue.read_error(); }
            }
            if (!is_clean_stop(result)) { return signal.result(); }
        }
        (void)interrupted(); // Accept Stop received during stop/dispose or producer join.
        const auto drained = signal.result();
        if (drained != PlaybackResult::completed) {
            return drained;
        }
        if (rejected) {
            // A format refusal has no exception; a failed open keeps its own.
            record_refusal(next_index, next, std::move(refusal));
            return PlaybackResult::next_track_rejected;
        }
        if (!next) { return drained; }
        track = std::move(next);
        track_index = next_index;
        ++segment_index;
        first_track = false;
    }
}

} // namespace wavplay::playback
