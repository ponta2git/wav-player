#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/thread_state.h"
#include "wavplay/core/thread_hints.h"

#include <chrono>
#include <thread>

namespace {

using namespace wavplay_test;
using wavplay::core::ExecutionSpeed;

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

int wavplay_test::run_thread_hints_tests()
{
    TestSuite suite;
    suite.run_timed("thread speed hints round-trip through the thread's power throttling", check_thread_speed_round_trip);
    suite.run_timed("thread names read back through GetThreadDescription", check_thread_name);
    return suite.result("Thread hints");
}
