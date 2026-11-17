#include "suites/suites.h"
#include "wavplay/media/wav_reader.h"
#include "wavplay/playback/playback_signal.h"
#include "fixtures/playback_fixture.h"
#include "fixtures/ring_fixture.h"

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace wavplay_test;

// Both segments are small enough to drive their entire mock clocks inside start().
// Each pair is one request on its own session: the session reopens the mock before
// the 48 kHz track and, if that track changed the clock, restores 44.1 kHz at close.
struct RatePair {
    TempFile first, second;
    const Bytes first_pcm = samples(5);
    const Bytes second_pcm = samples(7, 0x61);
    const wchar_t* first_path = first.write(wave({pcm16(2, 44100), {"data", first_pcm}}));
    const wchar_t* second_path = second.write(wave({pcm16(2, 48000), {"data", second_pcm}}));

    void play(MockAsio& driver, wavplay::playback::PlaybackResult expected, unsigned runs = 1) const
    {
        driver.sample_rate = 44100;
        driver.next_latency = 6;
        driver.ticks_after_start = {3, 4}; // Frames 0,2,4,6 in run one and 0,3,6,9,12 in run two.
        const auto result = play_in_session(driver, first_path, second_path);
        expect("rate transition result", static_cast<int>(result), static_cast<int>(expected));
        expect("prepared runs", driver.creates, runs);
        expect("started runs", driver.starts, runs);
        expect("recorded stops", driver.runs.size(), runs);
        expect("released runs", driver.disposals, runs);
        const auto& old = driver.runs.front();
        expect("first run at 44.1 kHz", static_cast<int>(old.rate == 44100), 1);
        expect("first width", old.width, 2);
        expect("old tail drained before reconfiguration", old.stopped_at, 6);
        check_submitted_pcm(old.submitted, old.width, first_pcm, 6);
    }
};

void test_rate_transition()
{
    using enum MockAsio::Call;
    using enum wavplay::playback::PlaybackResult;
    RatePair pair;
    // The first run drains at frame 6. An overload one switch earlier must
    // still stop this run before any attempt to set the next clock.
    check_fault(pair.first_path, 3, 4, kAsioOverload, pair.second_path, 44100);
    MockAsio driver, contender;
    driver.next_left_type = driver.next_right_type = ASIOSTInt24LSB;
    contender.fail_start = true;
    bool claim_checked = false;
    driver.on_call = [&](MockAsio::Call call) {
        if (call != set_rate || claim_checked) {
            return; // Restoring the clock at close comes after the request released its claim.
        }
        expect_rejected(contender, pair.first_path);
        expect("contender touched a driver during the rate change", contender.calls.size(), 0);
        claim_checked = true;
    };
    pair.play(driver, completed, 2);
    expect("driver claim checked at the rate change", static_cast<int>(claim_checked), 1);
    expect("clock change and restoration at close", driver.set_rates.size(), 2);
    expect("requested 48 kHz", static_cast<int>(driver.set_rates.front() == 48000), 1);
    expect("restored 44.1 kHz", static_cast<int>(driver.set_rates.back() == 44100), 1);
    expect_call_order(driver, {create, start, stop, dispose, can_rate, set_rate, create, start, stop, dispose});
    // The reopened driver reports its clock before the capability query.
    expect_call_order(driver, {dispose, rate, can_rate, set_rate});
    expect_call_order(driver, {set_rate, rate, start});
    expect_call_order(driver, {set_rate, size, create});
    expect_call_order(driver, {set_rate, channel, channel, create});
    expect_call_order(driver, {set_rate, create, latency, start});
    const auto& changed = driver.runs.back();
    expect("second run at 48 kHz", static_cast<int>(changed.rate == 48000), 1);
    expect("new preferred width", changed.width, 3);
    expect("new tail drained", changed.stopped_at, 12);
    check_submitted_pcm(changed.submitted, changed.width, pair.second_pcm, 12, 3);
    // A stale first-run latency would finish at frame 9, masking this overload.
    MockAsio late_fault;
    late_fault.fault_at = 9;
    pair.play(late_fault, underrun, 2);
}

void test_rate_rejections()
{
    using enum MockAsio::Call;
    using enum wavplay::playback::PlaybackResult;
    RatePair pair;
    MockAsio unsupported;
    unsupported.can_rate_result = ASE_NoClock;
    pair.play(unsupported, next_rate_rejected);
    expect("set after capability rejection", unsupported.set_rates.size(), 0);
    for (const auto rejection : {ASE_NoClock, ASE_InvalidMode}) {
        MockAsio refused;
        refused.set_rate_result = rejection;
        pair.play(refused, next_rate_rejected);
        expect("set attempts after clock rejection", refused.set_rates.size(), 1);
    }
    // A clock the driver changed before failing is restored when the session closes.
    const auto check_driver_failure = [&](MockAsio& broken, const char* expected, unsigned settings) {
        std::string failure;
        try {
            pair.play(broken, next_rate_rejected);
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
        if (failure != expected) {
            throw std::runtime_error(std::string("Expected ") + expected + ", got " + failure);
        }
        expect("set attempts on driver failure, including restoration", broken.set_rates.size(), settings);
        expect("runs started on driver failure", broken.starts, 1);
        expect("buffers released on driver failure", broken.disposals, 1);
        expect("device clock after the session closed", static_cast<int>(broken.sample_rate == 44100), 1);
        expect("deferred WAV released on driver failure", probe_file_access(pair.second_path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
    };
    MockAsio broken_query;
    broken_query.can_rate_result = ASE_HWMalfunction;
    check_driver_failure(broken_query, "ASIO canSampleRate failed: ASE_HWMalfunction (-999)", 0);
    MockAsio broken_set;
    broken_set.set_rate_result = ASE_NotPresent;
    check_driver_failure(broken_set, "ASIO setSampleRate failed: ASE_NotPresent (-1000)", 1);
    MockAsio wrong_clock;
    wrong_clock.keep_old_rate = true;
    pair.play(wrong_clock, next_rate_rejected);
    expect_call_order(wrong_clock, {set_rate, rate});
    MockAsio missing_clock;
    missing_clock.on_call = [&](MockAsio::Call call) {
        if (call == set_rate) {
            // The 48 kHz setting loses the clock; restoring 44.1 kHz at close regains it.
            missing_clock.no_clock_after_set = missing_clock.set_rates.empty();
        }
    };
    pair.play(missing_clock, next_rate_rejected);
    expect("lost clock restored at close", missing_clock.set_rates.size(), 2);
    MockAsio wrong_type;
    wrong_type.next_right_type = ASIOSTInt32LSB;
    pair.play(wrong_type, next_track_rejected);
    expect_call_order(wrong_type, {set_rate, channel});
    for (const long count : {0, 1}) {
        MockAsio missing_pair;
        missing_pair.on_call = [&](MockAsio::Call call) {
            if (call == set_rate) {
                missing_pair.outputs = count;
            }
        };
        pair.play(missing_pair, next_track_rejected);
        expect_call_order(missing_pair, {set_rate, channels});
        const auto changed = std::find(missing_pair.calls.begin(), missing_pair.calls.end(), set_rate);
        expect("channel indices queried without a stereo pair", std::count(changed, missing_pair.calls.end(), channel), 0);
    }
    MockAsio broken_channel;
    broken_channel.on_call = [&](MockAsio::Call call) {
        if (call == set_rate) {
            broken_channel.right_info_result = ASE_NotPresent; // Both outputs still exist.
        }
    };
    check_driver_failure(broken_channel, "ASIO getChannelInfo failed: ASE_NotPresent (-1000)", 2);

    MockAsio disconnected;
    disconnected.on_call = [&](MockAsio::Call call) {
        if (call == channel && !disconnected.set_rates.empty()) {
            disconnected.outputs = 0; // The pair vanished after the count was read.
        }
    };
    pair.play(disconnected, next_track_rejected);
    expect_call_order(disconnected, {set_rate, channels, channel, channels});
}

// The next rate's WAV, opened by the old run's preparation, is protected (write
// and delete access fail with a sharing violation) at the old run's start and at
// the new rate's setting, and is released after the request. Whether the handle
// is the original one is not observed. A read failure in the new run's priming
// ends the request with the reader's own error before buffers exist at the new
// rate, and releases the file.
void test_next_rate_reader()
{
    using enum MockAsio::Call;
    RatePair pair;
    MockAsio driver;
    std::vector<DWORD> held; // Probed at the old run's start and at the new rate's setting.
    driver.on_call = [&](MockAsio::Call call) {
        if ((call == start && driver.starts == 0) || (call == set_rate && driver.set_rates.empty())) {
            held.push_back(probe_file_access(pair.second_path, GENERIC_WRITE | DELETE));
        }
    };
    pair.play(driver, wavplay::playback::PlaybackResult::completed, 2);
    expect("next rate's WAV probed during and after the old run", held.size(), 2);
    for (const auto access : held) {
        expect("next rate's WAV protected at the old run's start and the new rate's setting", access,
            ERROR_SHARING_VIOLATION);
    }
    expect("next rate's WAV released after the request", probe_file_access(pair.second_path, GENERIC_WRITE | DELETE),
        ERROR_SUCCESS);

    {
        auto lock = open_lock_handle(pair.second_path);
        const auto data = static_cast<DWORD>(wavplay::media::WavReader(pair.second_path).info().data_offset);
        if (!LockFile(lock.get(), data, 0, 4, 0)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "lock next rate's PCM frame");
        }
        MockAsio failing;
        failing.sample_rate = 44100;
        failing.next_latency = 6;
        failing.ticks_after_start = {3, 4};
        bool locked_read{};
        try {
            (void)play_in_session(failing, pair.first_path, pair.second_path);
        } catch (const std::system_error& error) {
            locked_read = error.code() == std::error_code(ERROR_LOCK_VIOLATION, std::system_category()) &&
                std::string_view(error.what()).starts_with("ReadFile");
        }
        expect_true("the new run's priming fails with the reader's lock violation", locked_read);
        expect("old run played", failing.starts, 1);
        expect("no buffers at the new rate", failing.creates, 1);
        expect("old run released", failing.disposals, 1);
        expect("clock changed for the new run and restored at close", failing.set_rates.size(), 2);
    }
    expect("failed priming released the next rate's WAV", probe_file_access(pair.second_path, GENERIC_WRITE | DELETE),
        ERROR_SUCCESS);
}

void test_inactive_output()
{
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    MockAsio driver;
    driver.buffer_fault = MockAsio::BufferFault::inactive_output;
    driver.ticks_after_start = {1};
    expect_rejected(driver, path);
    expect("buffers created before checking activation", driver.creates, 1);
    expect("starts with an inactive right output", driver.starts, 0);
    expect("inactive buffers released", driver.disposals, 1);
    expect("inactive buffers not prefilled", static_cast<int>(driver.untouched_on_dispose), 1);
    driver.buffer_fault = MockAsio::BufferFault::none;
    const auto result = play_in_session(driver, path);
    expect("retry with an active pair", static_cast<int>(result), static_cast<int>(wavplay::playback::PlaybackResult::completed));
}

void test_changed_buffer_size()
{
    TempFile first, second;
    const auto* old_rate = first.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    const auto* new_rate = second.write(wave({pcm16(2, 48000), {"data", samples(1)}}));
    MockAsio changed;
    changed.sample_rate = 44100;
    changed.ticks_after_start = {1, 1};
    changed.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::size && !changed.set_rates.empty()) {
            changed.buffer_size = {2, 8, 3, 2};
        }
    };
    expect_throw<std::runtime_error>("inconsistent size after rate change",
        [&] { (void)play_in_session(changed, old_rate, new_rate); });
    expect("second set of buffers for an invalid size", changed.creates, 1);
    expect("first run released before rejecting the new size", changed.disposals, 1);
}

void test_rate_transition_notifications()
{
    using enum MockAsio::Call;
    using enum wavplay::playback::PlaybackResult;
    TempFile first, second;
    const auto* first_path = first.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    const auto* second_path = second.write(wave({pcm16(2, 48000), {"data", samples(1, 0x61)}}));
    const auto check_interrupted = [&](MockAsio& driver, unsigned changes, unsigned disposals = 1,
        wavplay::playback::PlaybackResult expected = driver_error) {
        driver.sample_rate = 44100;
        driver.ticks_after_start = {1, 1};
        MockSession session(driver);
        const auto result = wavplay::playback::play_tracks(session, first_path, second_path);
        expect("configuration change at track boundary", static_cast<int>(result), static_cast<int>(expected));
        expect("starts after boundary notification", driver.starts, 1);
        expect("disposals after boundary notification", driver.disposals, disposals);
        expect("rate changes at interruption", driver.set_rates.size(), changes);

        // A separate user call at the device's current rate may start again.
        driver.fault_at = -1;
        driver.on_call = {};
        const auto* retry = driver.sample_rate == 44100 ? first_path : second_path;
        const auto retried = wavplay::playback::play_tracks(session, retry, nullptr);
        expect("fresh playback after boundary interruption", static_cast<int>(retried), static_cast<int>(completed));
    };

    // The first and only frame was in prefilled B. The reset is delivered
    // immediately after the callback that signalled the completed drain. The
    // mock follows it with overload, which must take priority over recovery.
    MockAsio drained;
    drained.fault_at = 2;
    drained.fault_selector = kAsioResetRequest;
    check_interrupted(drained, 0, 1, underrun);

    // One frame fits in prefilled B. After the first A and the hardware
    // switch to B, stop() may still request A. A duplicate B or an invalid
    // half at this point invalidates the old driver before any rate switch.
    for (const long invalid : {1, 2}) {
        MockAsio broken;
        broken.on_call = [&](MockAsio::Call call) {
            if (call == stop) {
                broken.switch_notification(invalid, ASIOFalse);
            }
        };
        check_interrupted(broken, 0);
    }
    MockAsio trailing;
    trailing.sample_rate = 44100;
    trailing.ticks_after_start = {1, 1};
    trailing.on_call = [&](MockAsio::Call call) {
        if (call == stop) {
            trailing.switch_notification(0, ASIOFalse);
            trailing.switch_notification(1, ASIOFalse);
        }
    };
    const auto drained_pair = play_in_session(trailing, first_path, second_path);
    expect("valid trailing switches during stop", static_cast<int>(drained_pair), static_cast<int>(completed));

    // A reported external-clock loss or unexpected rate can arrive after the final
    // audio block drained, before the next rate has been requested of the reopened driver.
    for (const auto point : {stop, dispose, can_rate}) {
        for (const auto rate_change : {0.0, 96000.0}) {
            MockAsio wrong_clock;
            wrong_clock.on_call = [&](MockAsio::Call call) {
                if (call == point) {
                    wrong_clock.rate_notification(rate_change);
                }
            };
            check_interrupted(wrong_clock, 0);
        }
    }

    for (const auto point : {stop, dispose, can_rate, set_rate, create}) {
        MockAsio driver;
        driver.on_call = [&](MockAsio::Call call) {
            if (call == point && (call != create || !driver.set_rates.empty())) {
                const auto selector = point == dispose ? kAsioBufferSizeChange : kAsioResetRequest;
                driver.message_notification(selector, 0, nullptr, nullptr);
            }
        };
        const bool changed_rate = point == set_rate || point == create;
        check_interrupted(driver, changed_rate ? 1 : 0, point == create ? 2 : 1, reinitialize_required);
    }

    MockAsio previous_clock;
    previous_clock.sample_rate = 44100;
    previous_clock.ticks_after_start = {1, 1};
    previous_clock.on_call = [&](MockAsio::Call call) {
        if (call == can_rate) {
            // A capability query does not change the running hardware clock.
            previous_clock.rate_notification(previous_clock.sample_rate);
        }
    };
    const auto after_query = play_in_session(previous_clock, first_path, second_path);
    expect("current clock notification during capability query", static_cast<int>(after_query),
        static_cast<int>(completed));
    expect("starts after a current clock notification", previous_clock.starts, 2);

    MockAsio expected_clock;
    expected_clock.sample_rate = 44100;
    expected_clock.ticks_after_start = {1, 1};
    expected_clock.on_call = [&](MockAsio::Call call) {
        if (call == set_rate && expected_clock.set_rates.empty()) { // Not the restoration at close.
            expected_clock.rate_notification(48000);
        }
    };
    const auto result = play_in_session(expected_clock, first_path, second_path);
    expect("matching rate notification during setting", static_cast<int>(result), static_cast<int>(completed));
    expect("starts with matching rate notification", expected_clock.starts, 2);

    for (const auto rejection : {can_rate, set_rate}) {
        MockAsio refused_during_reset;
        refused_during_reset.can_rate_result = rejection == can_rate ? ASE_NoClock : ASE_OK;
        refused_during_reset.set_rate_result = ASE_NoClock;
        refused_during_reset.on_call = [&](MockAsio::Call call) {
            if (call == rejection) {
                refused_during_reset.message_notification(kAsioResetRequest, 0, nullptr, nullptr);
            }
        };
        check_interrupted(refused_during_reset, rejection == set_rate ? 1 : 0, 1, reinitialize_required);
    }
}

void test_clock_readback_precision()
{
    using enum wavplay::playback::PlaybackResult;
    TempFile file;
    const auto* path = file.write(wave({pcm16(2, 44100), {"data", samples(1)}}));
    {
        MockAsio invalid;
        invalid.sample_rate = std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
        volatile double signaling_nan = invalid.sample_rate;
        MockSession session(invalid); // Opening and closing the session do not query the clock.
        const auto saved = _mm_getcsr();
        _mm_setcsr((saved | _MM_MASK_INVALID) & ~static_cast<unsigned>(_MM_EXCEPT_INVALID));
        invalid.sample_rate = signaling_nan;
        const auto result = wavplay::playback::play_tracks(session, path);
        const auto raised = _mm_getcsr() & _MM_EXCEPT_INVALID;
        _mm_setcsr(saved);
        expect("signaling NaN in initial device clock", static_cast<int>(result), static_cast<int>(first_rate_rejected));
        expect("initial clock query changed MXCSR", raised, 0);
        expect("no clock setting without a valid original", invalid.set_rates.size(), 0);
    }

    MockAsio off_rate;
    off_rate.on_call = [&](MockAsio::Call call) {
        if (call == MockAsio::Call::rate && off_rate.set_rates.size() == 1) { // Until the restoration at close.
            off_rate.sample_rate = std::nextafter(44100.0, 48000.0);
        }
    };
    const auto mismatch = play_in_session(off_rate, path);
    expect("one ULP clock error after setting", static_cast<int>(mismatch), static_cast<int>(first_rate_rejected));
    expect("unconfirmed clock created no buffers", off_rate.creates, 0);
    expect("original 48 kHz restored at close", static_cast<int>(off_rate.sample_rate == 48000), 1);
}

} // namespace

int wavplay_test::run_rate_transition_tests()
{
    TestSuite suite;
    suite.run_timed("44.1 kHz to 48 kHz in one playback request", test_rate_transition);
    suite.run_timed("ASIO clock and output rejections during a rate transition", test_rate_rejections);
    suite.run_timed("the next rate's WAV is protected before its run and a failed priming releases it",
        test_next_rate_reader);
    suite.run_timed("created ASIO buffers must have two active outputs", test_inactive_output);
    suite.run_timed("inconsistent preferred width after a rate change", test_changed_buffer_size);
    suite.run_timed("driver notifications during automatic rate transitions", test_rate_transition_notifications);
    suite.run_timed("invalid and imprecise ASIO clock readbacks", test_clock_readback_precision);
    return suite.result("ASIO rate transition");
}
