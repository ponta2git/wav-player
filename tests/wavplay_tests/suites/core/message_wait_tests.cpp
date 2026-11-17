#include "suites/suites.h"
#include "framework/test_suite.h"
#include "wavplay/core/message_wait.h"
#include "wavplay/core/win32.h"

#include <objbase.h>

#include <chrono>
#include <stdexcept>
#include <thread>

// core::wait_for_event_until, pump_messages and quit_pending on a COM STA of
// their own: the deadline, dispatch-batch and WM_QUIT contracts every control
// thread (playback waits, ring priming, the controller) relies on.
namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
using wavplay::core::WaitResult;

class SingleThreadedApartment {
public:
    SingleThreadedApartment()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            throw std::runtime_error("Could not initialize a test STA");
        }
        drain(); // Defensive: each case runs on a TestThread of its own, so its queue starts empty.
    }
    ~SingleThreadedApartment()
    {
        drain();
        CoUninitialize();
    }
    static void drain() noexcept
    {
        MSG leftover{};
        while (PeekMessageW(&leftover, nullptr, 0, 0, PM_REMOVE)) {
        }
    }
    SingleThreadedApartment(const SingleThreadedApartment&) = delete;
    SingleThreadedApartment& operator=(const SingleThreadedApartment&) = delete;
};

class ThreadTimer {
public:
    explicit ThreadTimer(TIMERPROC callback = nullptr) : id_(SetTimer(nullptr, 0, USER_TIMER_MINIMUM, callback))
    {
        if (!id_) {
            throw std::runtime_error("Could not start a thread timer");
        }
    }
    ~ThreadTimer() { KillTimer(nullptr, id_); }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;

private:
    UINT_PTR id_;
};

void CALLBACK post_quit(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    PostQuitMessage(37);
}

thread_local unsigned dispatched_probes{};
// The least wall time each probe message takes to dispatch.
thread_local std::chrono::milliseconds probe_dispatch_time{};

LRESULT CALLBACK probe_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    if (message == WM_APP) {
        ++dispatched_probes;
        if (probe_dispatch_time > 0ms) {
            std::this_thread::sleep_for(probe_dispatch_time);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// A message-only window of the calling thread that counts the WM_APP messages
// dispatched to it.
class ProbeWindow {
public:
    ProbeWindow()
    {
        WNDCLASSW type{};
        type.lpfnWndProc = probe_proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WavplayMessageWaitProbe";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            wavplay::core::throw_last_error("RegisterClassW(message wait probe)");
        }
        window_ = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
            type.hInstance, nullptr);
        if (!window_) {
            wavplay::core::throw_last_error("CreateWindowExW(message wait probe)");
        }
        dispatched_probes = 0;
    }
    ~ProbeWindow() { DestroyWindow(window_); }
    ProbeWindow(const ProbeWindow&) = delete;
    ProbeWindow& operator=(const ProbeWindow&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

private:
    HWND window_{};
};

void test_deadline_despite_message_traffic()
{
    const SingleThreadedApartment apartment;
    const auto event = wavplay::core::create_event();
    // A regularly posted WM_TIMER must not keep extending the original deadline.
    const ThreadTimer recurring;
    const auto deadline = std::chrono::steady_clock::now() + 150ms;
    expect("wait with pending messages", wavplay::core::wait_for_event_until(event.get(), deadline),
        WaitResult::timed_out);
    expect("deadline met despite message traffic", static_cast<int>(std::chrono::steady_clock::now() >= deadline), 1);
}

void test_expired_deadline_wins()
{
    const SingleThreadedApartment apartment;
    const auto event = wavplay::core::create_event();
    SetEvent(event.get());
    // A signaled event must not turn an expired deadline into an early wake, and
    // expiry leaves the event for the next wait.
    expect("expired deadline wins over a set event",
        wavplay::core::wait_for_event_until(event.get(), std::chrono::steady_clock::now() - 1s), WaitResult::timed_out);
    expect("future deadline receives the set event",
        wavplay::core::wait_for_event_until(event.get(), std::chrono::steady_clock::now() + 5s), WaitResult::signaled);
}

void test_failed_wait()
{
    // A wait that cannot start is reported as failed, with its cause left in
    // GetLastError, both without and with message dispatch.
    const auto expect_failed = [] {
        SetLastError(ERROR_SUCCESS);
        const auto wake = wavplay::core::wait_for_event_until(nullptr, std::chrono::steady_clock::now() + 5s);
        const auto error = GetLastError();
        expect("a wait on no event fails", wake, WaitResult::failed);
        expect("GetLastError describes the failed wait", error, ERROR_INVALID_HANDLE);
    };
    expect_failed();
    const SingleThreadedApartment apartment;
    expect_failed();
}

void test_event_wake_dispatches_messages()
{
    const SingleThreadedApartment apartment;
    const ProbeWindow probe;
    const auto event = wavplay::core::create_event();
    SetEvent(event.get());
    expect("probe message queued", PostMessageW(probe.get(), WM_APP, 0, 0), TRUE);
    // Whichever of the two MsgWait reports first, the same call must dispatch
    // the STA's pending message and still return the event.
    expect("event wake", wavplay::core::wait_for_event_until(event.get(), std::chrono::steady_clock::now() + 5s),
        WaitResult::signaled);
    expect("event wake also dispatched the pending message", dispatched_probes, 1);
}

void test_bounded_message_batch()
{
    const SingleThreadedApartment apartment;
    constexpr unsigned extra = 8;
    for (unsigned i = 0; i < wavplay::core::message_batch_limit + extra; ++i) {
        expect("thread message queued", PostThreadMessageW(GetCurrentThreadId(), WM_APP, i, 0), TRUE);
    }
    expect("batch completes without WM_QUIT", static_cast<int>(wavplay::core::pump_messages()), 1);
    unsigned remaining{};
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_APP, WM_APP, PM_REMOVE)) {
        ++remaining;
    }
    expect("one batch leaves the rest queued", remaining, extra);
}

void test_quit_interrupts_wait()
{
    const SingleThreadedApartment apartment;
    const auto event = wavplay::core::create_event();
    const ThreadTimer quit(post_quit);
    expect("WM_QUIT interrupts the wait",
        wavplay::core::wait_for_event_until(event.get(), std::chrono::steady_clock::now() + 5s), WaitResult::quit);
    expect("WM_QUIT stays pending for the outer loop", static_cast<int>(wavplay::core::quit_pending()), 1);
    expect("a batch stops at the pending WM_QUIT", static_cast<int>(wavplay::core::pump_messages()), 0);
    MSG message{};
    expect("WM_QUIT preserved", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
    expect("exit code preserved", message.wParam, 37);
    expect("no WM_QUIT left", static_cast<int>(wavplay::core::quit_pending()), 0);
}

void test_quit_behind_full_batch_outlives_deadline()
{
    const SingleThreadedApartment apartment;
    const ProbeWindow probe;
    const auto event = wavplay::core::create_event();
    // More than one batch of slow messages precedes the WM_QUIT. The batch the
    // first wake dispatches outlasts the deadline, so no batch reaches the quit.
    probe_dispatch_time = 1ms;
    constexpr unsigned extra = 4;
    for (unsigned i = 0; i < wavplay::core::message_batch_limit + extra; ++i) {
        expect("probe message queued", PostMessageW(probe.get(), WM_APP, i, 0), TRUE);
    }
    // A posted WM_QUIT, as another thread sends it. (PostQuitMessage only sets a
    // flag that Windows reports once no posted message remains, so no wait can
    // see that quit behind these messages without dispatching them.)
    expect("WM_QUIT queued", PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 43, 0), TRUE);
    // A quit queued before the deadline is a shutdown request, even though the
    // batch limit postponed it past the deadline.
    expect("a postponed WM_QUIT is reported at the deadline",
        wavplay::core::wait_for_event_until(event.get(), std::chrono::steady_clock::now() + 5ms), WaitResult::quit);
    expect_true("at most one bounded batch was dispatched", dispatched_probes <= wavplay::core::message_batch_limit);
    expect("WM_QUIT stays pending for the outer loop", static_cast<int>(wavplay::core::quit_pending()), 1);
    MSG message{};
    expect("WM_QUIT preserved", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
    expect("exit code preserved", message.wParam, 43);
}

void test_consumed_event_survives_quit()
{
    const SingleThreadedApartment apartment;
    const DWORD self = GetCurrentThreadId();
    // While this thread blocks in the wait, SetEvent satisfies the wait and
    // resets the auto-reset event at once; WM_QUIT must then reach the same
    // wake's message batch. (With both pending before the call, MsgWait may
    // report the input first and leave the event set.) Both threads share one logical
    // processor and the signaller runs at a higher priority, so this thread,
    // readied by SetEvent, cannot resume before the quit is posted.
    const DWORD_PTR processor = DWORD_PTR{1} << (GetCurrentProcessorNumber() % (8 * sizeof(DWORD_PTR)));
    const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), processor);
    if (!previous) {
        wavplay::core::throw_last_error("SetThreadAffinityMask");
    }
    struct RestoreAffinity {
        DWORD_PTR mask;
        ~RestoreAffinity() { SetThreadAffinityMask(GetCurrentThread(), mask); }
    } restore{previous};
    for (int round = 0; round < 5; ++round) {
        const auto event = wavplay::core::create_event();
        std::jthread signaller([&event, self, processor] {
            SetThreadAffinityMask(GetCurrentThread(), processor);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            std::this_thread::sleep_for(50ms);
            SetEvent(event.get());
            PostThreadMessageW(self, WM_QUIT, 41, 0);
        });
        const auto wake = wavplay::core::wait_for_event_until(event.get(),
            std::chrono::steady_clock::now() + hang_budget(5s));
        signaller.join();
        if (wake == WaitResult::signaled) {
            // The quit missed this wake's batch; it wakes the next wait alone.
            expect("the next wait reports the pending WM_QUIT", wavplay::core::wait_for_event_until(event.get(),
                std::chrono::steady_clock::now() + hang_budget(5s)), WaitResult::quit);
        } else {
            // WM_QUIT is reported at once, and the consumed signal is set again.
            expect("WM_QUIT interrupts the wait", wake, WaitResult::quit);
            expect("a signal not reported stays set", WaitForSingleObject(event.get(), 0), WAIT_OBJECT_0);
        }
        expect("WM_QUIT stays pending", static_cast<int>(wavplay::core::quit_pending()), 1);
        MSG message{};
        expect("WM_QUIT preserved", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
        expect("exit code preserved", message.wParam, 41);
    }
}

// Two auto-reset events, both unset.
struct EventPair {
    wavplay::core::UniqueHandle first{wavplay::core::create_event()}, second{wavplay::core::create_event()};
    HANDLE handles[2]{first.get(), second.get()};
};

void test_several_events()
{
    // The index names the consumed event, the lowest index wins, and expiry
    // leaves every signal set, both without and with message dispatch.
    const auto check = [] {
        const EventPair events;
        const auto later = [] { return std::chrono::steady_clock::now() + hang_budget(5s); };
        SetEvent(events.handles[1]);
        auto wake = wavplay::core::wait_for_events_until(events.handles, later());
        expect("second event wakes the wait", wake.result, WaitResult::signaled);
        expect("the wake names the second event", wake.index, 1);
        expect("only the reported event was consumed", WaitForSingleObject(events.handles[1], 0), WAIT_TIMEOUT);
        expect("the unset first event stays unset", WaitForSingleObject(events.handles[0], 0), WAIT_TIMEOUT);

        SetEvent(events.handles[0]);
        SetEvent(events.handles[1]);
        wake = wavplay::core::wait_for_events_until(events.handles, later());
        expect("both set events wake the wait", wake.result, WaitResult::signaled);
        expect("the lowest index wins", wake.index, 0);
        wake = wavplay::core::wait_for_events_until(events.handles, later());
        expect("the higher index stays set for the next wait", wake.index, 1);

        SetEvent(events.handles[0]);
        SetEvent(events.handles[1]);
        wake = wavplay::core::wait_for_events_until(events.handles, std::chrono::steady_clock::now() - 1s);
        expect("expired deadline wins over set events", wake.result, WaitResult::timed_out);
        expect("expiry reports no index", wake.index, 0);
        expect("expiry leaves the first event set", WaitForSingleObject(events.handles[0], 0), WAIT_OBJECT_0);
        expect("expiry leaves the second event set", WaitForSingleObject(events.handles[1], 0), WAIT_OBJECT_0);

        const auto deadline = std::chrono::steady_clock::now() + 30ms;
        expect("unset events time out", wavplay::core::wait_for_events_until(events.handles, deadline).result,
            WaitResult::timed_out);
        expect("the deadline was met", static_cast<int>(std::chrono::steady_clock::now() >= deadline), 1);

        SetLastError(ERROR_SUCCESS);
        expect("a wait on no events fails", wavplay::core::wait_for_events_until({}, later()).result, WaitResult::failed);
        expect("GetLastError names the empty list", GetLastError(), ERROR_INVALID_PARAMETER);
        const HANDLE too_many[MAXIMUM_WAIT_OBJECTS]{};
        SetLastError(ERROR_SUCCESS);
        expect("a wait beyond the message wait's limit fails", wavplay::core::wait_for_events_until(too_many, later()).result,
            WaitResult::failed);
        expect("GetLastError names the long list", GetLastError(), ERROR_INVALID_PARAMETER);
    };
    check();
    const SingleThreadedApartment apartment;
    check();
}

void test_several_events_quit_at_expiry()
{
    const SingleThreadedApartment apartment;
    const EventPair events;
    SetEvent(events.handles[0]);
    SetEvent(events.handles[1]);
    expect("WM_QUIT queued", PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 47, 0), TRUE);
    // A quit queued before the deadline wins over expiry and over set events,
    // and consumes neither signal.
    const auto wake = wavplay::core::wait_for_events_until(events.handles, std::chrono::steady_clock::now() - 1s);
    expect("a pending WM_QUIT is reported at the deadline", wake.result, WaitResult::quit);
    expect("quit leaves the first event set", WaitForSingleObject(events.handles[0], 0), WAIT_OBJECT_0);
    expect("quit leaves the second event set", WaitForSingleObject(events.handles[1], 0), WAIT_OBJECT_0);
    MSG message{};
    expect("WM_QUIT preserved", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
    expect("exit code preserved", message.wParam, 47);
}

void test_consumed_second_event_survives_quit()
{
    const SingleThreadedApartment apartment;
    const DWORD self = GetCurrentThreadId();
    // As in test_consumed_event_survives_quit, but the wake consumes the second
    // of two events: the quit must set exactly that event again.
    const DWORD_PTR processor = DWORD_PTR{1} << (GetCurrentProcessorNumber() % (8 * sizeof(DWORD_PTR)));
    const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), processor);
    if (!previous) {
        wavplay::core::throw_last_error("SetThreadAffinityMask");
    }
    struct RestoreAffinity {
        DWORD_PTR mask;
        ~RestoreAffinity() { SetThreadAffinityMask(GetCurrentThread(), mask); }
    } restore{previous};
    for (int round = 0; round < 5; ++round) {
        const EventPair events;
        std::jthread signaller([second = events.handles[1], self, processor] {
            SetThreadAffinityMask(GetCurrentThread(), processor);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            std::this_thread::sleep_for(50ms);
            SetEvent(second);
            PostThreadMessageW(self, WM_QUIT, 53, 0);
        });
        const auto wake = wavplay::core::wait_for_events_until(events.handles,
            std::chrono::steady_clock::now() + hang_budget(5s));
        signaller.join();
        if (wake.result == WaitResult::signaled) {
            expect("the wake names the second event", wake.index, 1);
            expect("the next wait reports the pending WM_QUIT", wavplay::core::wait_for_events_until(events.handles,
                std::chrono::steady_clock::now() + hang_budget(5s)).result, WaitResult::quit);
        } else {
            expect("WM_QUIT interrupts the wait", wake.result, WaitResult::quit);
            expect("the consumed second event is set again", WaitForSingleObject(events.handles[1], 0), WAIT_OBJECT_0);
            expect("the first event is not set", WaitForSingleObject(events.handles[0], 0), WAIT_TIMEOUT);
        }
        expect("WM_QUIT stays pending", static_cast<int>(wavplay::core::quit_pending()), 1);
        MSG message{};
        expect("WM_QUIT preserved", PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), TRUE);
        expect("exit code preserved", message.wParam, 53);
    }
}

} // namespace

int wavplay_test::run_message_wait_tests()
{
    TestSuite suite;
    suite.run_timed("monotonic deadline is not extended by message traffic", test_deadline_despite_message_traffic);
    suite.run_timed("expired deadline wins over a signaled event", test_expired_deadline_wins);
    suite.run_timed("a failed wait is reported with its error", test_failed_wait);
    suite.run_timed("event wake also dispatches pending STA messages", test_event_wake_dispatches_messages);
    suite.run_timed("message dispatch is bounded per batch", test_bounded_message_batch);
    suite.run_timed("WM_QUIT interrupts a wait and stays queued", test_quit_interrupts_wait);
    suite.run_timed("a WM_QUIT behind a full batch is reported at the deadline",
        test_quit_behind_full_batch_outlives_deadline);
    suite.run_timed("a consumed event is not lost to a WM_QUIT in the same wake", test_consumed_event_survives_quit);
    suite.run_timed("several events: consumed index, lowest index first, expiry", test_several_events);
    suite.run_timed("several events: a WM_QUIT queued before the deadline wins", test_several_events_quit_at_expiry);
    suite.run_timed("several events: a consumed second event is not lost to a WM_QUIT",
        test_consumed_second_event_survives_quit);
    return suite.result("STA message wait");
}
