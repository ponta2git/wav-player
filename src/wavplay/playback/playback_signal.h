#pragma once

#include "wavplay/asio/clock.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/playback/playback_types.h"

#include <windows.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>

namespace wavplay::playback {

// The lock-free state shared by the control thread and every ASIO callback of a
// playback request: a request generation, the phase of its current segment, the
// first stop reason of that phase, and the clock the phase expects.
// ASIO can deliver a notification on a different thread from the audio
// callback. Capture the generation at entry so a late stop cannot end a retry.
//
// The inline operations below are the ones callbacks reach; they only load,
// compare-exchange and SetEvent. Control-thread operations live in the .cpp.
class PlaybackSignal {
public:
    using Token = std::uint64_t;
    // try_begin() and begin() never return this value, so it can mark "no
    // request observed".
    static constexpr Token no_request{};

    constexpr PlaybackSignal() noexcept = default;
    ~PlaybackSignal();
    PlaybackSignal(const PlaybackSignal&) = delete;
    PlaybackSignal& operator=(const PlaybackSignal&) = delete;

    // Control thread. Starts a request generation expecting `rate` (0 while the
    // device clock is not yet known). Each request can observe the original
    // clock, set each run's clock, and release/reopen provisionally before
    // observing the fresh driver clock for a following run.
    // previous_idle names the clean stop the caller checked, or no_request. A
    // device notification can still fault that request before this generation
    // starts; that race returns the fault (see idle_fault) and leaves it in place.
    // Any other change of that idle request is a caller error and throws
    // std::logic_error.
    [[nodiscard]] std::expected<Token, PlaybackResult> try_begin(std::uint32_t rate = 0, Token previous_idle = 0);
    // try_begin that throws std::logic_error instead of returning an idle fault.
    // Only tests call it; the product calls try_begin, so the linker drops this.
    Token begin(std::uint32_t rate = 0, Token previous_idle = 0);
    // Control thread. Moves `token` to the next phase expecting `rate`, provided
    // the current phase ended with `previous`. A request out of phases fails as a
    // driver error instead of carrying into the generation bits.
    [[nodiscard]] bool advance_clock(Token& token, double rate, PlaybackResult previous) noexcept;
    // Control thread. Final release may restore the original device clock. Keep the
    // terminal result (including a reinitialization request) and fault reporting,
    // but permit a positive finite restored clock.
    void prepare_release(Token previous_idle) noexcept;
    // Control thread, between requests. The fault that has replaced the clean
    // stop recorded in `idle` (a value returned by snapshot() after the request
    // ended), or nullopt. A later request or the release phase is not a fault.
    [[nodiscard]] std::optional<PlaybackResult> idle_fault(Token idle) const noexcept
    {
        const auto current = snapshot();
        if (idle == no_request || !same_request(current, idle) || is_clean_stop(result_of(current))) {
            return std::nullopt;
        }
        return result_of(current);
    }
    // Control thread. A finite watchdog wait for this signal's event; see
    // core::wait_for_event_until for the deadline, message-dispatch and result
    // contract. On the driver's STA, WM_QUIT interrupts it with quit.
    [[nodiscard]] core::WaitResult wait_until(std::chrono::steady_clock::time_point deadline) const noexcept;

    // The request generation and segment phase of `value`, without its stop reason.
    [[nodiscard]] static constexpr Token phase_of(Token value) noexcept { return value & ~result_mask; }
    [[nodiscard]] static constexpr bool same_phase(Token observed, Token phase) noexcept
    {
        return phase_of(observed) == phase;
    }
    [[nodiscard]] static constexpr bool same_request(Token left, Token right) noexcept
    {
        return (left & ~request_flags) == (right & ~request_flags);
    }
    [[nodiscard]] static constexpr PlaybackResult result_of(Token value) noexcept
    {
        return static_cast<PlaybackResult>(value & result_mask);
    }
    [[nodiscard]] Token snapshot() const noexcept { return value_.load(std::memory_order_acquire); }
    [[nodiscard]] PlaybackResult result() const noexcept { return result_of(snapshot()); }
    // Internal control-loop integration. Process-lifetime event; null until the first try_begin().
    [[nodiscard]] HANDLE event_handle() const noexcept { return changed_.load(std::memory_order_acquire); }

    // A user Stop can wake the control thread even when callbacks have ceased.
    void wake() const noexcept
    {
        if (const auto event = changed_.load(std::memory_order_acquire)) {
            SetEvent(event);
        }
    }

    void stop(Token observed) noexcept { replace_if_current(observed, request_flags, PlaybackResult::stopped); }

    [[nodiscard]] bool matches_rate(double rate) const noexcept
    {
        return clock_bits(rate) == rate_bits_[rate_index(snapshot())].load(std::memory_order_relaxed);
    }

    void finish(Token observed, PlaybackResult reason) noexcept
    {
        if ((observed & result_mask) == 0 &&
            value_.compare_exchange_strong(observed, observed | static_cast<Token>(reason), std::memory_order_release)) {
            wake();
        }
    }

    void rate_changed(Token observed, double rate) noexcept
    {
        const auto expected = rate_bits_[rate_index(observed)].load(std::memory_order_relaxed);
        // Before a device clock is known (initial open or reopen), a positive finite clock is provisional.
        if (!asio::is_valid_clock(rate) || (expected != 0 && expected != clock_bits(rate))) {
            // Even a just-drained segment can still advance to another track.
            // A delayed clock report belongs only to the segment it observed.
            replace_if_current(observed, result_mask, PlaybackResult::driver_error);
        }
    }

    // A driver fault invalidates this entire request, even if a segment
    // just drained. A later explicit play has its own generation.
    void invalidate(Token observed, PlaybackResult reason = PlaybackResult::driver_error) noexcept
    {
        replace_if_current(observed, request_flags, reason);
    }

private:
    // Replace the result of `observed`'s phase (or, with request_flags, of any
    // phase of its request) by `reason`, unless a higher-priority result exists.
    // Fatal faults win over reinitialization, which wins over a clean stop.
    // Preserve the first fatal fault; results only move upwards, so the loop ends.
    void replace_if_current(Token observed, Token changeable_flags, PlaybackResult reason) noexcept
    {
        const Token generation = observed & ~changeable_flags;
        do {
            const auto state = result_of(observed);
            const bool replace_reinitialization = state == PlaybackResult::reinitialize_required &&
                is_fault(reason) && reason != PlaybackResult::reinitialize_required;
            const bool replace_refusal = is_track_refusal(state) && !is_track_refusal(reason);
            if (state == reason || (!is_clean_stop_or_playing(state) && !replace_reinitialization && !replace_refusal)) {
                return;
            }
            const Token fault = phase_of(observed) | static_cast<Token>(reason);
            if (value_.compare_exchange_strong(observed, fault, std::memory_order_acq_rel)) {
                wake();
                return;
            }
        } while ((observed & ~changeable_flags) == generation);
    }

    [[nodiscard]] static constexpr bool is_clean_stop_or_playing(PlaybackResult result) noexcept
    {
        return result == PlaybackResult::playing || is_clean_stop(result);
    }

    // High bits identify a user request; the low bits identify its segment
    // and the first stop reason for that segment.
    static constexpr Token result_mask = 15;
    static constexpr Token phase_step = result_mask + 1;
    // More than one billion clock phases per request; no fixed track-count limit.
    // The high thirty bits still fence stale notifications from later requests.
    static constexpr Token phase_mask = ((Token{1} << 30) - 1) * phase_step;
    static constexpr Token request_flags = phase_mask | result_mask;
    static_assert(static_cast<Token>(PlaybackResult::playing) == 0);
    static_assert(static_cast<Token>(PlaybackResult::reinitialize_required) <= result_mask);
    static_assert(std::atomic<Token>::is_always_lock_free);
    static_assert(std::atomic<HANDLE>::is_always_lock_free);
    [[nodiscard]] static constexpr bool last_phase(Token phase) noexcept { return (phase & phase_mask) == phase_mask; }
    [[nodiscard]] static constexpr unsigned rate_index(Token phase) noexcept { return (phase & phase_step) != 0; }
    // A clock is compared by its IEEE-754 bits (see asio/clock.h); 0 means not yet known.
    using ClockBits = std::uint64_t;
    [[nodiscard]] static constexpr ClockBits clock_bits(double rate) noexcept { return std::bit_cast<ClockBits>(rate); }
    void store_rate(Token phase, double rate) noexcept
    {
        rate_bits_[rate_index(phase)].store(clock_bits(rate), std::memory_order_relaxed);
    }

    std::atomic<HANDLE> changed_{};
    std::atomic<Token> value_{static_cast<Token>(PlaybackResult::completed)};
    std::atomic<ClockBits> rate_bits_[2]{};
};

// The SDK has no callback context. Both control and callbacks observe the
// same process-lifetime signal, including while no segment has active buffers.
[[nodiscard]] PlaybackSignal& notifications() noexcept;

} // namespace wavplay::playback
