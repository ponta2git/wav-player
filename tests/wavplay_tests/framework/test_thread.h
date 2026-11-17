#pragma once

#include "framework/failure_reporting.h"

#include <chrono>
#include <concepts>
#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace wavplay_test {

// Runs one function on a thread of its own, never a reused pool thread, so the
// thread's Win32 and COM state (message queue, apartment, timers, hooks) ends
// with the case that created it. Like a std::async future, destroying or
// replacing a TestThread waits for the function: lambdas may capture locals by
// reference. A watchdog that gives up on a stuck thread must leave through
// abandon_hung instead of unwinding past it.
template<class R>
class TestThread {
public:
    TestThread() noexcept = default;

    template<class Function>
        requires (!std::same_as<std::remove_cvref_t<Function>, TestThread>) && std::invocable<Function&>
    explicit TestThread(Function&& function)
    {
        std::packaged_task<R()> task(std::forward<Function>(function));
        result_ = task.get_future();
        thread_ = std::jthread([task = std::move(task)]() mutable {
            install_thread_failure_handlers();
            task();
        });
    }

    TestThread(TestThread&&) noexcept = default;
    TestThread& operator=(TestThread&&) noexcept = default; // Joins the replaced thread.

    bool valid() const noexcept { return result_.valid(); }

    template<class Rep, class Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout) const
    {
        return result_.wait_for(timeout);
    }

    // Waits until the function has returned and its thread has exited, then
    // returns the result or rethrows the function's exception.
    R get()
    {
        result_.wait();
        if (thread_.joinable()) {
            thread_.join();
        }
        return result_.get();
    }

private:
    std::future<R> result_;
    std::jthread thread_; // Declared last: destroyed, and so joined, first.
};

} // namespace wavplay_test
