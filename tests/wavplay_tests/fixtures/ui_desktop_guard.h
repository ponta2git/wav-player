#pragma once

#include "framework/failure_reporting.h"
#include "wavplay/core/win32.h"

#include <chrono>
#include <stdexcept>

namespace wavplay_test {

// Keyboard focus and foreground activation belong to the shared desktop, not
// a test process. Serialize only suites which exercise that state. Their child
// processes must not acquire this guard while the parent owns it.
class UiDesktopGuard {
public:
    UiDesktopGuard()
        : mutex_(CreateMutexExW(nullptr, L"Local\\WAVPlayer.Tests.UiDesktop.v1", 0,
            SYNCHRONIZE | MUTEX_MODIFY_STATE))
    {
        if (!mutex_) { wavplay::core::throw_last_error("CreateMutexExW(UI test desktop)"); }
        const auto result = WaitForSingleObject(mutex_.get(),
            static_cast<DWORD>(hang_budget(std::chrono::seconds(60)).count()));
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { return; }
        if (result == WAIT_TIMEOUT) { throw std::runtime_error("Timed out waiting for the UI test desktop"); }
        wavplay::core::throw_last_error("WaitForSingleObject(UI test desktop)");
    }

    ~UiDesktopGuard() { (void)ReleaseMutex(mutex_.get()); }
    UiDesktopGuard(const UiDesktopGuard&) = delete;
    UiDesktopGuard& operator=(const UiDesktopGuard&) = delete;

private:
    wavplay::core::UniqueHandle mutex_;
};

} // namespace wavplay_test
