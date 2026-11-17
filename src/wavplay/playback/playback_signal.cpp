#include "wavplay/playback/playback_signal.h"

#include "wavplay/core/message_wait.h"
#include "wavplay/core/win32.h"

#include <stdexcept>

namespace wavplay::playback {
namespace {
constexpr const char* failed_while_idle = "Playback failed while idle; create a new request before retrying";
} // namespace

PlaybackSignal::~PlaybackSignal()
{
    // Unpublish before closing, so a late wake() sees no handle instead of a
    // closed (or recycled) one.
    if (const auto event = changed_.exchange(nullptr, std::memory_order_acq_rel)) {
        CloseHandle(event);
    }
}

std::expected<PlaybackSignal::Token, PlaybackResult> PlaybackSignal::try_begin(std::uint32_t rate, Token previous_idle)
{
    // The control thread creates this before the first callback registration.
    // Keep it across runs so a late notification never signals a retired handle.
    if (!changed_.load(std::memory_order_acquire)) {
        changed_.store(core::create_event().release(), std::memory_order_release);
    }
    auto observed = snapshot();
    for (;;) {
        // A notification may arrive after play() checked the idle state,
        // while it is reopening the file. Never erase that fault on resume.
        if (previous_idle && same_request(observed, previous_idle) && observed != previous_idle) {
            if (const auto fault = idle_fault(previous_idle)) {
                return std::unexpected(*fault);
            }
            throw std::logic_error(failed_while_idle);
        }
        Token next = (observed | request_flags) + 1;
        // Publish the new rate in the inactive slot. A notification that
        // still observes the paused phase must retain its old clock check.
        if (rate_index(next) == rate_index(observed)) {
            next += phase_step;
        }
        store_rate(next, rate);
        if (value_.compare_exchange_strong(observed, next, std::memory_order_acq_rel)) {
            return next;
        }
    }
}

PlaybackSignal::Token PlaybackSignal::begin(std::uint32_t rate, Token previous_idle)
{
    const auto begun = try_begin(rate, previous_idle);
    if (!begun) {
        throw std::logic_error(failed_while_idle);
    }
    return *begun;
}

bool PlaybackSignal::advance_clock(Token& token, double rate, PlaybackResult previous) noexcept
{
    if (last_phase(token)) {
        invalidate(token); // Never carry into the generation bits.
        return false;
    }
    Token expected = token | static_cast<Token>(previous);
    const Token next = token + phase_step;
    // A callback that sampled the old phase must not invalidate the new clock.
    store_rate(next, rate);
    if (!value_.compare_exchange_strong(expected, next, std::memory_order_acq_rel)) {
        return false;
    }
    token = next;
    return true;
}

void PlaybackSignal::prepare_release(Token previous_idle) noexcept
{
    auto observed = snapshot();
    if (!previous_idle || !same_request(observed, previous_idle) || last_phase(observed)) {
        return;
    }
    const auto result = result_of(observed);
    if (!is_clean_stop(result) && result != PlaybackResult::reinitialize_required) {
        return; // Never erase a fault already reported while idle.
    }
    const auto next = phase_of(observed) + phase_step;
    store_rate(next, 0);
    // A fault racing publication wins; there is no retry with a new result.
    (void)value_.compare_exchange_strong(observed, next | static_cast<Token>(result), std::memory_order_acq_rel);
}

core::WaitResult PlaybackSignal::wait_until(std::chrono::steady_clock::time_point deadline) const noexcept
{
    // The driver's windows and COM calls can depend on its owning STA while audio is running.
    return core::wait_for_event_until(changed_.load(std::memory_order_acquire), deadline);
}

} // namespace wavplay::playback
