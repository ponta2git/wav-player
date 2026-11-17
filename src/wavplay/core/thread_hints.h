#pragma once

#include <cstdint>

namespace wavplay::core {

// Scheduling hints for the application's own threads only: never apply them
// to a thread the ASIO driver created. They change neither priority, affinity,
// MMCSS, memory nor I/O priority. Every hint is best-effort; Windows may
// ignore it or change its policy, and the product ignores failure.
enum class ExecutionSpeed : std::uint8_t {
    system, // Windows decides from the process state (every thread's default).
    high,   // HighQoS whatever the window's visibility (the explicit opt-out).
    eco,    // EcoQoS: the most efficient frequency and, on hybrid CPUs, efficient cores.
};

// Sets the calling thread's execution-speed power throttling
// (SetThreadInformation with ThreadPowerThrottling). False if Windows refused.
bool set_current_thread_speed(ExecutionSpeed speed) noexcept;

// Applies a speed to the calling thread for the scope's lifetime, then returns
// it to ExecutionSpeed::system. Not for nesting: the exit does not restore an
// earlier explicit speed.
class ThreadSpeedScope {
public:
    explicit ThreadSpeedScope(ExecutionSpeed speed) noexcept { (void)set_current_thread_speed(speed); }
    ~ThreadSpeedScope() { (void)set_current_thread_speed(ExecutionSpeed::system); }
    ThreadSpeedScope(const ThreadSpeedScope&) = delete;
    ThreadSpeedScope& operator=(const ThreadSpeedScope&) = delete;
};

// Names the calling thread for debuggers, crash dumps and ETW traces
// (SetThreadDescription). The name stays for the thread's lifetime; failure
// is ignored.
void name_current_thread(const wchar_t* name) noexcept;

} // namespace wavplay::core
