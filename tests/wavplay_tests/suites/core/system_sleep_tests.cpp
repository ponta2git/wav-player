#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/thread_state.h"
#include "wavplay/core/system_sleep.h"
#include "wavplay/core/thread_hints.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

using namespace wavplay_test;
using wavplay::core::ExecutionSpeed;
using wavplay::core::SystemSleepGuard;

DWORD handle_count()
{
    DWORD count{};
    if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
        wavplay::core::throw_last_error("GetProcessHandleCount");
    }
    return count;
}

void check_request_lifetime()
{
    { const SystemSleepGuard warmup; }
    const auto before = handle_count();
    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        {
            const SystemSleepGuard first;
            expect("active sleep guard owns one request handle", handle_count(), before + 1);
            {
                const SystemSleepGuard nested;
                expect("nested operations own independent requests", handle_count(), before + 2);
            }
            expect("ending nested operation keeps the first request", handle_count(), before + 1);
        }
        expect("completed operation releases its request", handle_count(), before);
        expect_throw<std::runtime_error>("failure unwinds the operation", [] {
            const SystemSleepGuard failed_operation;
            throw std::runtime_error("operation failed");
        });
        expect("failed operation releases its request", handle_count(), before);
    }
}

void check_thread_state_is_untouched()
{
    // Add a pre-existing caller-owned display requirement. The guard must not
    // clear it, add a thread requirement, or replace it during teardown.
    const auto previous = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    if (!previous) {
        throw std::runtime_error("SetThreadExecutionState failed");
    }
    struct RestoreThreadState {
        EXECUTION_STATE state;
        ~RestoreThreadState() { (void)SetThreadExecutionState(state); }
    } restore{previous};
    {
        const SystemSleepGuard guard;
        const auto during = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
        expect("sleep request does not replace caller execution flags", during,
            ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    }
    const auto after = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    expect("request teardown does not replace caller execution flags", after,
        ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
}

// Run by run_timed on a thread of its own, created at the system default.
void check_thread_speed_round_trip()
{
    const HANDLE self = GetCurrentThread();
    if (!expect_thread_speed("a new thread leaves its speed to Windows", self, ExecutionSpeed::system)) {
        return;
    }
    // Another thread keeps the default whatever this thread requests.
    const auto release = wavplay::core::create_event();
    std::jthread other([event = release.get()] {
        (void)WaitForSingleObject(event, static_cast<DWORD>(hang_budget(std::chrono::seconds(30)).count()));
    });
    struct Release {
        HANDLE event;
        ~Release() { SetEvent(event); } // Before `other` joins, also on a failed check.
    } released{release.get()};
    const auto other_thread = static_cast<HANDLE>(other.native_handle());
    for (const auto speed : {ExecutionSpeed::high, ExecutionSpeed::eco, ExecutionSpeed::system, ExecutionSpeed::eco,
        ExecutionSpeed::high, ExecutionSpeed::system}) {
        expect_true("Windows accepts the speed hint", wavplay::core::set_current_thread_speed(speed));
        expect_thread_speed("the calling thread reports the requested speed", self, speed);
        expect_thread_speed("another thread keeps the default", other_thread, ExecutionSpeed::system);
    }
    {
        const wavplay::core::ThreadSpeedScope eco(ExecutionSpeed::eco);
        expect_thread_speed("a speed scope applies its speed", self, ExecutionSpeed::eco);
    }
    expect_thread_speed("leaving the scope returns the speed to Windows", self, ExecutionSpeed::system);
    (void)wavplay::core::set_current_thread_speed(ExecutionSpeed::high);
    {
        const wavplay::core::ThreadSpeedScope eco(ExecutionSpeed::eco);
    }
    expect_thread_speed("a scope ends at the default, not at an earlier speed", self, ExecutionSpeed::system);
}

// Also on a thread of its own: the name stays for the thread's lifetime.
void check_thread_name()
{
    const HANDLE self = GetCurrentThread();
    wavplay::core::name_current_thread(L"WAV Player test thread");
    expect_true("the thread reports its name", thread_description(self) == L"WAV Player test thread");
    wavplay::core::name_current_thread(L"WAV Player renamed test thread");
    expect_true("a later name replaces the earlier one", thread_description(self) == L"WAV Player renamed test thread");
}

} // namespace

int wavplay_test::run_system_sleep_tests()
{
    TestSuite suite;
    suite.run("independent system sleep requests end on completion and exception", check_request_lifetime);
    suite.run("system sleep requests preserve caller thread execution flags", check_thread_state_is_untouched);
    suite.run_timed("thread speed hints round-trip through the thread's power throttling", check_thread_speed_round_trip);
    suite.run_timed("thread names read back through GetThreadDescription", check_thread_name);
    return suite.result("System sleep and thread hint");
}
