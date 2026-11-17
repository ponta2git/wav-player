#include "suites/suites.h"
#include "framework/test_suite.h"
#include "wavplay/asio/clock.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/playback/playback_signal.h"

#include <windows.h>
#include <xmmintrin.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

// The lock-free request signal shared by the control thread and every ASIO
// callback: generations, phases, the first stop reason and the expected clock.
namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
using wavplay::playback::PlaybackResult;
using wavplay::playback::PlaybackSignal;

// Clocks are inspected as IEEE-754 bits; none of these raises an FP exception.
static_assert(wavplay::asio::is_valid_clock(44100.0) && wavplay::asio::is_valid_clock(384000.0));
static_assert(wavplay::asio::is_valid_clock(std::numeric_limits<double>::denorm_min()));
static_assert(!wavplay::asio::is_valid_clock(0.0) && !wavplay::asio::is_valid_clock(-0.0));
static_assert(!wavplay::asio::is_valid_clock(-44100.0));
static_assert(!wavplay::asio::is_valid_clock(std::numeric_limits<double>::infinity()));
static_assert(!wavplay::asio::is_valid_clock(-std::numeric_limits<double>::infinity()));
static_assert(!wavplay::asio::is_valid_clock(std::numeric_limits<double>::quiet_NaN()));
static_assert(!wavplay::asio::is_valid_clock(std::bit_cast<double>(UINT64_C(0x7ff0000000000001)))); // Signaling NaN.
static_assert(wavplay::asio::same_clock(48000.0, 48000.0) && !wavplay::asio::same_clock(0.0, -0.0));
static_assert(!wavplay::asio::same_clock(44100.0, std::bit_cast<double>(std::bit_cast<std::uint64_t>(44100.0) + 1))); // One ULP.
void test_resume_signal()
{
    PlaybackSignal signal;
    auto token = signal.begin(48000);
    signal.finish(token, PlaybackResult::paused);
    const auto first_pause = signal.snapshot();
    token = signal.begin(0, first_pause);
    expect("resume begins a new generation", static_cast<int>(PlaybackSignal::same_request(token, first_pause)), 0);
    signal.finish(token, PlaybackResult::paused);
    const auto checked_pause = signal.snapshot(); // play() has checked this value.
    signal.invalidate(checked_pause); // Fault arrives before the new request is armed.
    const auto fault = signal.snapshot();
    expect_throw<std::logic_error>("resume cannot erase intervening paused fault", [&] {
        (void)signal.begin(0, checked_pause);
    });
    expect("resume preserves pending fault", signal.snapshot(), fault);
    const auto retry = signal.begin(48000); // A new user request may retry after recovery.
    signal.invalidate(checked_pause);
    expect("late fault does not cancel explicit new request", signal.snapshot(), retry);
}

void test_phase_budget()
{
    PlaybackSignal signal;
    const auto first = signal.begin(48000);
    auto token = first;
    for (unsigned index = 0; index < 4096; ++index) {
        expect_true("long playlist advances its clock phase", signal.advance_clock(token,
            index % 2 == 0 ? 44100 : 48000, PlaybackResult::playing));
    }
    expect_true("many clock phases remain one request", PlaybackSignal::same_request(token, first));
    signal.invalidate(first, PlaybackResult::underrun);
    expect("an earlier phase still faults its request", signal.result(), PlaybackResult::underrun);
}

void test_signal_deadline()
{
    PlaybackSignal signal;
    (void)signal.begin(48000);
    signal.wake();
    // A queued event must not turn an already expired drain/restart deadline
    // into another early wake. Expiry also leaves the event for the next wait.
    expect("expired deadline wins over a queued notification",
        signal.wait_until(std::chrono::steady_clock::now() - 1s), wavplay::core::WaitResult::timed_out);
    expect("future deadline receives the queued notification",
        signal.wait_until(std::chrono::steady_clock::now() + 5s), wavplay::core::WaitResult::signaled);
}

void test_release_signal()
{
    using enum PlaybackResult;
    for (const auto terminal : {paused, stopped, completed, reinitialize_required}) {
        PlaybackSignal signal;
        const auto token = signal.begin(48000);
        signal.finish(token, terminal);
        const auto idle = signal.snapshot();
        signal.prepare_release(idle);
        const auto releasing = signal.snapshot();
        expect("release preserves terminal result", static_cast<int>(signal.result()), static_cast<int>(terminal));
        expect("release keeps faults in the same request", static_cast<int>(PlaybackSignal::same_request(releasing, idle)), 1);
        signal.rate_changed(releasing, 44100);
        expect("restoring the original clock is permitted", signal.snapshot(), releasing);
        signal.rate_changed(idle, 0); // A delayed old-phase clock report cannot undo the handoff.
        expect("old-phase clock report does not corrupt release", signal.snapshot(), releasing);
        signal.invalidate(idle, underrun);
        expect("old-phase overload still faults final release", static_cast<int>(signal.result()), static_cast<int>(underrun));
        const auto fault = signal.snapshot();
        signal.prepare_release(idle);
        expect("release cannot erase an existing fault", signal.snapshot(), fault);

        const auto next = signal.begin(48000);
        signal.finish(next, completed);
        const auto next_idle = signal.snapshot();
        signal.prepare_release(idle);
        expect("old request cannot relax a new clock expectation", signal.snapshot(), next_idle);
        signal.prepare_release(next_idle);
        signal.rate_changed(signal.snapshot(), 0);
        expect("clock loss during release remains a fault", static_cast<int>(signal.result()), static_cast<int>(driver_error));
    }
}

void test_late_faults_and_retry()
{
    // A reset may outlive the segment that received it, but not the user request.
    PlaybackSignal signal;
    const auto old = signal.begin(44100);
    signal.finish(old, PlaybackResult::completed);
    auto next = old;
    expect("second segment armed", static_cast<int>(signal.advance_clock(next, 48000, PlaybackResult::completed)), 1);
    signal.finish(old, PlaybackResult::underrun);
    expect("late segment-local stop", signal.snapshot(), next);
    signal.invalidate(old, PlaybackResult::reinitialize_required);
    expect("first-segment reset delivered to second segment", static_cast<int>(signal.result()),
        static_cast<int>(PlaybackResult::reinitialize_required));
    const auto retry = signal.begin(48000);
    signal.invalidate(old, PlaybackResult::reinitialize_required);
    signal.invalidate(next, PlaybackResult::reinitialize_required);
    signal.finish(next, PlaybackResult::underrun);
    signal.rate_changed(signal.snapshot(), 48000);
    expect("late stop after retry", signal.snapshot(), retry);
    // Drivers own the callback thread's floating-point environment. A corrupt
    // clock value must stop this run without raising an invalid FP operation.
    volatile double invalid_rate = std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
    const auto saved = _mm_getcsr();
    _mm_setcsr((saved | _MM_MASK_INVALID) & ~static_cast<unsigned>(_MM_EXCEPT_INVALID));
    signal.rate_changed(signal.snapshot(), invalid_rate);
    const auto raised = _mm_getcsr() & _MM_EXCEPT_INVALID;
    _mm_setcsr(saved);
    expect("invalid clock changed MXCSR", raised, 0);
    expect("clock loss on current run", static_cast<int>(signal.result()),
        static_cast<int>(PlaybackResult::driver_error));
}

void test_clock_before_wav_rate_is_known()
{
    PlaybackSignal signal;
    for (const auto rate : {0.0, -0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()}) {
        signal.begin();
        signal.rate_changed(signal.snapshot(), rate);
        expect("invalid clock during WAV open", static_cast<int>(signal.result()),
            static_cast<int>(PlaybackResult::driver_error));
    }
    const auto retry = signal.begin();
    signal.rate_changed(signal.snapshot(), 44100);
    expect("valid provisional clock during WAV open", signal.snapshot(), retry);

    // Pause a callback at entry, then deliver it after the device clock changes.
    // Both the stored expectation and the right to invalidate belong to its old phase.
    auto phase = retry;
    using enum PlaybackResult;
    expect("original device clock", static_cast<int>(signal.advance_clock(phase, 48000, playing)), 1);
    const auto original = phase;
    expect("first WAV clock", static_cast<int>(signal.advance_clock(phase, 44100, playing)), 1);
    signal.rate_changed(original, 48000);
    signal.rate_changed(original, 0);
    expect("late original clock reports after first switch", signal.snapshot(), phase);
    expect("stale buffer switch belongs to old clock", static_cast<int>(signal.same_phase(original, phase)), 0);
    const auto first = phase;
    signal.finish(phase, completed);
    expect("stop callbacks belong to drained phase", static_cast<int>(signal.same_phase(signal.snapshot(), first)), 1);
    expect("second WAV clock", static_cast<int>(signal.advance_clock(phase, 96000, completed)), 1);
    signal.rate_changed(first, 44100);
    expect("late first WAV clock after second switch", signal.snapshot(), phase);
    signal.invalidate(original, reinitialize_required);
    expect("device reset spans clock phases", static_cast<int>(signal.result()), static_cast<int>(reinitialize_required));
}

void test_reinitialization_priority()
{
    using enum PlaybackResult;
    PlaybackSignal signal;
    for (const auto terminal : {playing, completed, paused, stopped}) {
        const auto token = signal.begin(48000);
        signal.finish(token, terminal);
        signal.invalidate(token, reinitialize_required);
        signal.stop(token);
        signal.finish(token, paused);
        signal.invalidate(token, reinitialize_required);
        expect("reinitialization overrides clean stops and remains latched", signal.result(), reinitialize_required);
        for (const auto fatal : {underrun, read_error, driver_error}) {
            const auto retry = signal.begin(48000);
            signal.invalidate(retry, reinitialize_required);
            signal.invalidate(retry, fatal);
            signal.invalidate(retry, reinitialize_required);
            signal.stop(retry);
            expect("a fatal fault overrides reinitialization and cannot be hidden", signal.result(), fatal);
            signal.invalidate(retry, fatal == driver_error ? underrun : driver_error);
            expect("the first fatal fault remains final", signal.result(), fatal);
        }
    }
    const auto token = signal.begin(48000);
    signal.invalidate(token, reinitialize_required);
    signal.rate_changed(signal.snapshot(), 0);
    expect("clock loss overrides reinitialization", signal.result(), driver_error);
}

void test_recovery_handoff_preserves_fault()
{
    using enum PlaybackResult;
    PlaybackSignal signal;
    for (const auto fatal : {underrun, read_error, driver_error}) {
        const auto token = signal.begin(48000);
        signal.invalidate(token, reinitialize_required);
        signal.prepare_release(signal.snapshot());
        const auto retired = signal.snapshot();
        // Release has completed and recovery checked its result. A late writer
        // commits before the new generation, so this fault still belongs here.
        signal.invalidate(token, fatal);
        expect_throw<std::logic_error>("recovery cannot erase a fatal fault racing its generation handoff", [&] {
            (void)signal.begin(0, retired);
        });
        expect("handoff keeps the fatal reason", signal.result(), fatal);
        expect_true("handoff keeps the old request generation", PlaybackSignal::same_request(signal.snapshot(), retired));
    }
    const auto token = signal.begin(48000);
    signal.invalidate(token, reinitialize_required);
    const auto retired = signal.snapshot();
    const auto fresh = signal.begin(0, retired);
    signal.invalidate(retired, underrun);
    expect("a fault committed after the handoff cannot affect the fresh instance", signal.snapshot(), fresh);
    signal.finish(fresh, stopped);
}

void test_try_begin_idle_race()
{
    using enum PlaybackResult;
    PlaybackSignal signal;
    for (const auto fault : {reinitialize_required, underrun, driver_error, read_error}) {
        const auto token = signal.begin(48000);
        signal.finish(token, paused);
        const auto checked = signal.snapshot(); // play() has checked this clean stop.
        signal.invalidate(checked, fault);
        const auto faulted = signal.snapshot();
        const auto raced = signal.try_begin(0, checked);
        expect_true("an idle fault racing the next request is returned, not thrown", !raced && raced.error() == fault);
        expect("the returned idle fault stays in place", signal.snapshot(), faulted);
    }
    const auto token = signal.begin(48000);
    signal.finish(token, stopped);
    const auto checked = signal.snapshot();
    signal.prepare_release(checked);
    const auto released = signal.snapshot();
    expect_throw<std::logic_error>("a clean idle request that changed phase is a caller error", [&] {
        (void)signal.try_begin(0, checked);
    });
    expect("the refused request leaves the signal unchanged", signal.snapshot(), released);
    const auto resumed = signal.try_begin(48000, released);
    expect_true("an unchanged idle request begins a new generation",
        resumed && *resumed == signal.snapshot() && !PlaybackSignal::same_request(*resumed, released));
}

void test_idle_fault()
{
    // A clean stop stays supervised between requests: only a later fault of the
    // same request is reported, never the release phase or a newer request.
    PlaybackSignal signal;
    expect("no request is never a fault", static_cast<int>(signal.idle_fault(PlaybackSignal::no_request).has_value()), 0);
    const auto token = signal.begin(48000);
    expect("begin never returns the no-request value", static_cast<int>(token != PlaybackSignal::no_request), 1);
    signal.finish(token, PlaybackResult::paused);
    const auto idle = signal.snapshot();
    expect("clean stop is not a fault", static_cast<int>(signal.idle_fault(idle).has_value()), 0);
    signal.prepare_release(idle);
    signal.rate_changed(signal.snapshot(), 44100);
    expect("release phase and restored clock are not faults", static_cast<int>(signal.idle_fault(idle).has_value()), 0);
    signal.invalidate(idle, PlaybackResult::underrun);
    const auto fault = signal.idle_fault(idle);
    expect("overload after a clean stop is reported", static_cast<int>(fault.value_or(PlaybackResult::playing)),
        static_cast<int>(PlaybackResult::underrun));
    (void)signal.begin(48000);
    expect("a newer request hides the old one's fault", static_cast<int>(signal.idle_fault(idle).has_value()), 0);
}

} // namespace

int wavplay_test::run_playback_signal_tests()
{
    TestSuite suite;
    suite.run("resume rearming preserves a concurrent paused fault", test_resume_signal);
    suite.run("long playlists retain notification generations across many clock phases", test_phase_budget);
    suite.run_timed("expired signal deadline wins over a queued wake", test_signal_deadline);
    suite.run("final release permits clock restoration and preserves faults", test_release_signal);
    suite.run("late segment faults reach the request but not a retry", test_late_faults_and_retry);
    suite.run("clock notifications before the WAV rate is known", test_clock_before_wav_rate_is_known);
    suite.run("idle supervision reports only faults of the stopped request", test_idle_fault);
    suite.run("fatal faults win over reinitialization and clean stops cannot erase it", test_reinitialization_priority);
    suite.run("recovery handoff preserves an intervening fatal fault", test_recovery_handoff_preserves_fault);
    suite.run("try_begin returns an idle fault race and throws on another idle change", test_try_begin_idle_race);
    return suite.result("Playback request signal");
}
