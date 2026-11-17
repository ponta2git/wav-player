#pragma once

#include "framework/test_suite.h"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <future>

namespace wavplay_test {

// Holds one driver call on the product's thread (block()) until the test opens
// it. Declare the gate before the controller or window whose thread enters it,
// so the call can still reach it while that object is destroyed, and declare a
// ReleaseGate after that object, so a failing assertion opens the gate before
// the destructor joins the blocked thread.
class CallGate {
public:
    CallGate() : arrived_(arrival_.get_future()), released_(release_.get_future().share()) {}
    CallGate(const CallGate&) = delete;
    CallGate& operator=(const CallGate&) = delete;
    ~CallGate() { open(); }

    // The product's thread: report arrival, then wait for open().
    void block()
    {
        arrival_.set_value();
        released_.wait();
    }

    // The test thread: wait until the product has entered block().
    void wait()
    {
        if (arrived_.wait_for(hang_budget(std::chrono::seconds(5))) != std::future_status::ready) {
            fail("The product never entered the blocked driver call");
        }
    }

    // The test thread only.
    void open()
    {
        if (!opened_) {
            opened_ = true;
            release_.set_value();
        }
    }

private:
    std::promise<void> arrival_, release_;
    std::future<void> arrived_;
    std::shared_future<void> released_;
    bool opened_{};
};

struct ReleaseGate {
    CallGate& gate;
    ~ReleaseGate() { gate.open(); }
};

// Holds the next open of a file through another handle, on whichever product
// thread opens it, until the test opens the gate. A batch oplock does this
// without a product hook: Windows completes the oplock request when another
// open arrives and finishes that open only once the oplock's handle is closed.
// Create the gate after the file is written and after any earlier open that
// must pass (a playlist load validates every item), on a test thread that
// outlives it (Windows cancels a thread's pending requests when the thread
// exits). Declare it after the object whose thread opens the file, or declare
// a ReleaseOpen after that object, so the gate opens before that object waits
// for its thread. Never open the gated file from the test while it is closed.
class OpenGate {
public:
    explicit OpenGate(const wchar_t* path) : arrived_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (!arrived_) {
            fail("Could not create an open gate event");
        }
        // A scanner can briefly hold a freshly written file, and a batch oplock
        // is granted only to the file's sole open.
        const auto deadline = std::chrono::steady_clock::now() + hang_budget(std::chrono::seconds(5));
        for (;;) {
            file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (file_ != INVALID_HANDLE_VALUE) {
                ResetEvent(arrived_);
                request_ = {};
                request_.hEvent = arrived_;
                if (!DeviceIoControl(file_, FSCTL_REQUEST_BATCH_OPLOCK, nullptr, 0, nullptr, 0, nullptr, &request_) &&
                    GetLastError() == ERROR_IO_PENDING) {
                    return; // Granted: the request stays pending until another open arrives.
                }
                CloseHandle(file_); // Refused or already completed: nothing is pending.
                file_ = INVALID_HANDLE_VALUE;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                CloseHandle(arrived_);
                fail("Could not hold the gated file with a batch oplock");
            }
            Sleep(10);
        }
    }
    ~OpenGate()
    {
        open();
        CloseHandle(arrived_);
    }
    OpenGate(const OpenGate&) = delete;
    OpenGate& operator=(const OpenGate&) = delete;

    // The test thread: wait until another open of the file is held.
    void wait() const
    {
        if (WaitForSingleObject(arrived_, static_cast<DWORD>(hang_budget(std::chrono::seconds(5)).count())) !=
            WAIT_OBJECT_0) {
            fail("The product never opened the gated file");
        }
    }

    // True once another open of the file arrived while the gate was closed.
    [[nodiscard]] bool reached() const noexcept
    {
        return file_ == INVALID_HANDLE_VALUE ? reached_ : WaitForSingleObject(arrived_, 0) == WAIT_OBJECT_0;
    }

    // The test thread: finish a held open and let later opens pass.
    void open() noexcept
    {
        if (file_ == INVALID_HANDLE_VALUE) {
            return;
        }
        reached_ = WaitForSingleObject(arrived_, 0) == WAIT_OBJECT_0;
        // Cancel a request no open has completed, and wait until Windows no
        // longer uses it. Closing the handle then releases the oplock.
        CancelIoEx(file_, &request_);
        DWORD transferred{};
        (void)GetOverlappedResult(file_, &request_, &transferred, TRUE);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE arrived_;
    HANDLE file_{INVALID_HANDLE_VALUE};
    OVERLAPPED request_{};
    bool reached_{};
};

struct ReleaseOpen {
    OpenGate& gate;
    ~ReleaseOpen() { gate.open(); }
};

} // namespace wavplay_test
