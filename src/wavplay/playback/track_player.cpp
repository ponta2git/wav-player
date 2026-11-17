#include "wavplay/playback/track_player.h"

#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/playback/playback_signal.h"
#include "wavplay/playback/request.h"
#include "wavplay/playback/segment.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace wavplay::playback {

static_assert(std::is_same_v<PlaybackSignal::Token, std::uint64_t>, "TrackPlayer stores the token without the signal header");

TrackPlayer::TrackPlayer(const wchar_t* path)
    : TrackPlayer(media::WavReader(path))
{
}

TrackPlayer::TrackPlayer(media::WavReader track)
    : track_(stream::retain_playable_track(std::move(track)))
{
}

TrackPlayer::TrackPlayer(std::shared_ptr<const std::vector<std::wstring>> paths, std::size_t start_index)
    : paths_(std::move(paths))
{
    if (!playlist()) {
        throw std::invalid_argument("A playlist player requires a path list");
    }
    select(start_index);
}

void TrackPlayer::select(std::size_t index)
{
    if (playing_) {
        throw std::logic_error("Cannot select a track during playback");
    }
    if (!playlist() || index >= paths_->size()) {
        throw std::out_of_range("Playlist selection is out of range");
    }
    observe_idle_fault();
    if (index != track_index_ || !track_) {
        auto selected = stream::retain_playable_track((*paths_)[index].c_str());
        track_ = std::move(selected);
    }
    track_index_ = index;
    position_ = live_position_ = 0;
}

TrackPlayer::~TrackPlayer() = default;

void wake_playback_control() noexcept
{
    notifications().wake();
}

void TrackPlayer::request_stop() noexcept
{
    if (!requests_->stop.exchange(true, std::memory_order_acq_rel)) {
        notifications().wake();
    }
}

void TrackPlayer::observe_idle_fault() noexcept
{
    if (!idle_fault_ || *idle_fault_ == PlaybackResult::reinitialize_required) {
        const auto current = notifications().idle_fault(idle_token_);
        if (current && current != idle_fault_) {
            idle_fault_ = current;
            idle_fault_reported_ = false;
        }
        failed_ |= idle_fault_.has_value();
    }
}

HANDLE TrackPlayer::idle_fault_event() const noexcept
{
    return idle_token_ != PlaybackSignal::no_request && !idle_fault_ ? notifications().event_handle() : nullptr;
}

std::optional<PlaybackResult> TrackPlayer::take_idle_fault() noexcept
{
    observe_idle_fault();
    if (!idle_fault_ || std::exchange(idle_fault_reported_, true)) {
        return std::nullopt;
    }
    return idle_fault_;
}

void TrackPlayer::prepare_release() noexcept
{
    notifications().prepare_release(idle_token_);
}

PlaybackResult TrackPlayer::reinitialize(asio::DriverSession& session, PlaybackObserver observer)
{
    if (playing_) {
        throw std::logic_error("Cannot reinitialize during playback");
    }
    const RequestClaim claim;
    auto& signal = notifications();
    const auto observed = signal.snapshot();
    if (!idle_token_ || !PlaybackSignal::same_request(observed, idle_token_)) {
        throw std::logic_error("No retained ASIO reinitialization request");
    }
    auto result = PlaybackSignal::result_of(observed);
    if (result != PlaybackResult::reinitialize_required) {
        return result; // A real fault racing recovery must not be erased.
    }
    requests_->pause.store(false, std::memory_order_release);
    requests_->stop.store(false, std::memory_order_release);
    idle_fault_reported_ = true;
    const auto cancelled = [&]() noexcept {
        if (observer.poll) {
            observer.poll(observer.context);
        }
        return requests_->stop.load(std::memory_order_acquire);
    };

    // Normal restoration notifications belong to the retiring driver. Keep
    // reset coalesced, but allow overload/clock loss to supersede recovery.
    prepare_release();
    session.close();
    const bool closing = cancelled();
    const auto retired = signal.snapshot();
    result = PlaybackSignal::result_of(retired);
    if (result != PlaybackResult::reinitialize_required) {
        idle_fault_ = result;
        return result;
    }
    if (closing) {
        return PlaybackResult::stopped;
    }

    // Release has returned: delayed old-generation writers cannot affect the
    // new instance. Do not clear a notification raised by its init or queries.
    const auto begun = signal.try_begin(0, retired);
    if (!begun) {
        idle_fault_ = begun.error();
        return begun.error();
    }
    auto token = *begun;
    idle_token_ = token;
    auto& driver = session.reopen();
    if (cancelled()) {
        signal.stop(token);
    } else if (signal.snapshot() == token) {
        asio::inquire_overload_reporting(driver);
        if (cancelled()) {
            signal.stop(token);
        }
        if (signal.snapshot() == token) {
            const auto rate = asio::current_sample_rate(driver);
            const bool stopping = cancelled();
            if (!rate) {
                signal.invalidate(token);
            } else if (stopping) {
                signal.stop(token);
            } else {
                (void)signal.advance_clock(token, *rate, PlaybackResult::playing);
            }
        }
    }
    signal.finish(token, PlaybackResult::stopped);
    if (signal.result() == PlaybackResult::reinitialize_required) {
        signal.invalidate(token); // A fresh instance that asks again has failed; never spin on reopen.
    }
    idle_token_ = signal.snapshot();
    result = PlaybackSignal::result_of(idle_token_);
    failed_ = result != PlaybackResult::stopped;
    idle_fault_ = failed_ ? std::optional{result} : std::nullopt;
    idle_fault_reported_ = failed_;
    if (!failed_) {
        position_ = 0;
    }
    return result;
}

void TrackPlayer::stop()
{
    if (playing_) {
        throw std::logic_error("Use request_stop during playback");
    }
    observe_idle_fault();
    position_ = 0;
    requests_->pause.store(false, std::memory_order_release);
    requests_->stop.store(false, std::memory_order_release);
    // Retain idle_token_ so a reset racing this operation still blocks the next play.
}

PlaybackResult TrackPlayer::play(asio::DriverSession& session, PlaybackDiagnostics* diagnostics,
    PlaybackObserver observer)
{
    cause_ = nullptr;
    observe_idle_fault();
    if (idle_fault_ && !std::exchange(idle_fault_reported_, true)) {
        return *idle_fault_; // Report the device's reason once instead of a generic refusal.
    }
    if (playing_ || failed_) {
        throw std::logic_error("Playback is active or failed; create a new request before retrying");
    }
    if (!track_ && playlist()) {
        try { select(track_index_); }
        catch (const std::runtime_error&) {
            cause_ = std::current_exception();
            return PlaybackResult::next_track_rejected;
        }
    }
    if (position_ == track_->info().frame_count) {
        return PlaybackResult::completed;
    }
    requests_->pause.store(false, std::memory_order_release);
    requests_->stop.store(false, std::memory_order_release);
    struct PlayingGuard {
        bool& flag;
        explicit PlayingGuard(bool& value) : flag(value) { flag = true; }
        ~PlayingGuard() { flag = false; }
    } playing(playing_);
    try {
        // Keep other requests from replacing the signal generation before the
        // final Stop/fault observation and retained-position commit below.
        const RequestClaim claim;
        asio::DriverLease device(session);
        SegmentControl control{requests_->pause, requests_->stop};
        control.request_pages_locked = requests_.locked();
        control.previous_idle_token = std::exchange(idle_token_, PlaybackSignal::no_request);
        const auto initial_index = track_index_;
        const auto initial_position = position_;
        live_position_ = position_;
        RequestProgress progress;
        std::size_t progress_run = std::numeric_limits<std::size_t>::max(), current_in_run{};
        std::uint64_t completed_in_run{};
        const auto update = [&]() noexcept {
            if (!playlist()) {
                live_position_ = std::min(initial_position + control.progress_frames(), frame_count());
                return;
            }
            if (progress.tracks.empty()) { return; }
            if (progress_run != progress.first_track) {
                progress_run = progress.first_track;
                current_in_run = 0;
                completed_in_run = 0;
            }
            auto frames = control.progress_frames() + progress.first_frame - completed_in_run;
            while (current_in_run + 1 < progress.tracks.size() && frames >= progress.frame_counts[current_in_run]) {
                const auto count = progress.frame_counts[current_in_run];
                frames -= count;
                completed_in_run += count;
                // The callback has submitted this item's final samples. Release
                // its resume protection here on the control thread; later copies
                // of the same path keep their shared handle until their own use.
                progress.tracks[current_in_run++].reset();
            }
            const auto current_index = initial_index + progress.first_track + current_in_run;
            // The first input retains track_, and each index keeps its source for
            // this play. Reassign only at track boundaries, including duplicate paths.
            if (track_index_ != current_index) {
                track_index_ = current_index;
                track_ = progress.tracks[current_in_run];
            }
            live_position_ = std::min(frames, track_->info().frame_count);
        };
        struct Observation {
            const decltype(update)& refresh;
            PlaybackObserver observer;
        } observation{update, observer};
        control.observer = {
            [](void* context) noexcept {
                auto& value = *static_cast<Observation*>(context);
                value.refresh();
                if (value.observer.started) { value.observer.started(value.observer.context); }
            },
            [](void* context) noexcept {
                auto& value = *static_cast<Observation*>(context);
                value.refresh();
                if (value.observer.poll) { value.observer.poll(value.observer.context); }
            }, &observation};
        // The inputs point into the shared list, which paths_ keeps alive.
        std::vector<stream::TrackInput> inputs;
        inputs.reserve(playlist() ? paths_->size() - initial_index : 1);
        inputs.emplace_back(playlist() ? stream::TrackInput(track_) : stream::TrackInput(track_->reopen()));
        for (auto index = initial_index + 1; playlist() && index < paths_->size(); ++index) {
            inputs.emplace_back((*paths_)[index].c_str());
        }
        auto result = run_request(device, std::move(inputs), control, {.diagnostics = diagnostics,
            .first_frame = position_, .progress = &progress,
            .refusals = playlist() ? RefusalPolicy::record_each : RefusalPolicy::throw_first,
            .first_path = playlist() ? std::wstring_view((*paths_)[initial_index]) : std::wstring_view{}});
        update();
        if (is_clean_stop(result) || (playlist() && is_track_refusal(result))) {
            auto& signal = notifications();
            control.accept_stop(signal, control.phase_token);
            const auto observed = signal.snapshot();
            const bool same_request = PlaybackSignal::same_request(observed, control.phase_token);
            if (same_request && (!is_track_refusal(result) || PlaybackSignal::result_of(observed) != PlaybackResult::completed)) {
                result = PlaybackSignal::result_of(observed);
            }
            // Watch exactly the value the signal holds, so idle supervision and the
            // next request's try_begin() compare against the same clean stop.
            idle_token_ = same_request ? observed : PlaybackSignal::no_request;
        }
        if (is_clean_stop(result)) {
            // Commit only after stop, disposal, producer join and final fault observation.
            position_ = result == PlaybackResult::stopped ? 0 : playlist() ? live_position_ :
                result == PlaybackResult::completed ? track_->info().frame_count : initial_position + control.submitted_frames;
        } else if (playlist() && is_track_refusal(result)) {
            result = settle_track_refusal(device, result, progress, initial_index);
        } else {
            failed_ = true;
            if (result == PlaybackResult::reinitialize_required) {
                idle_token_ = notifications().snapshot();
                idle_fault_ = result;
                idle_fault_reported_ = true;
            }
        }
        if (result == PlaybackResult::read_error) {
            cause_ = std::move(progress.fault_error);
        } else if (is_track_refusal(result)) {
            cause_ = std::move(progress.rejected_error);
        }
        return result;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

// Selects the refused playlist item at position zero. A refusal belongs to that
// file, not to the initialized device: leave a clean watched phase so a later
// device fault still wins. Returns such a fault if one has already replaced the
// refusal, otherwise `result`.
PlaybackResult TrackPlayer::settle_track_refusal(asio::DriverLease& device, PlaybackResult result,
    RequestProgress& progress, std::size_t initial_index)
{
    if (progress.rejected_track) {
        const auto rejected = initial_index + *progress.rejected_track;
        if (rejected != track_index_) { track_ = std::move(progress.rejected_source); }
        track_index_ = rejected;
    }
    position_ = live_position_ = 0;
    auto& signal = notifications();
    const auto token = signal.snapshot();
    const auto reason = PlaybackSignal::result_of(token);
    if (is_track_refusal(reason)) {
        // A refused setSampleRate may leave either the old clock or a
        // changed clock behind. Reconfirm it instead of disarming idle
        // clock supervision with the provisional zero expectation.
        const auto current = asio::current_sample_rate(device.driver());
        auto phase = PlaybackSignal::phase_of(token);
        if (!current) {
            signal.invalidate(token);
        } else if (signal.advance_clock(phase, *current, reason)) {
            signal.finish(phase, PlaybackResult::stopped);
        }
    }
    idle_token_ = signal.snapshot();
    const auto settled = PlaybackSignal::result_of(idle_token_);
    if (is_fault(settled) && !is_track_refusal(settled)) {
        failed_ = true;
        idle_fault_ = settled;
        idle_fault_reported_ = true;
        return settled;
    }
    return result;
}

} // namespace wavplay::playback
