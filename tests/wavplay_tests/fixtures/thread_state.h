#pragma once

#include "framework/test_suite.h"
#include "wavplay/core/thread_hints.h"
#include "wavplay/core/win32.h"

#include <cstdio>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wavplay_test {

// A thread's execution-speed power throttling, as GetThreadInformation reports it.
struct ThreadSpeedState {
    ULONG control{}, state{};
    friend constexpr bool operator==(const ThreadSpeedState&, const ThreadSpeedState&) = default;
};

// The documented masks of each speed: no control for the system default,
// control without the throttled state for HighQoS, both for EcoQoS.
constexpr ThreadSpeedState documented_state(wavplay::core::ExecutionSpeed speed) noexcept
{
    constexpr ULONG execution_speed = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    return {speed == wavplay::core::ExecutionSpeed::system ? 0 : execution_speed,
        speed == wavplay::core::ExecutionSpeed::eco ? execution_speed : 0};
}

// Another thread of this process, opened for the queries below.
inline wavplay::core::UniqueHandle open_thread_for_query(DWORD id)
{
    wavplay::core::UniqueHandle thread(OpenThread(THREAD_QUERY_INFORMATION, FALSE, id));
    if (!thread) {
        wavplay::core::throw_last_error("OpenThread(THREAD_QUERY_INFORMATION)");
    }
    return thread;
}

// nullopt where this Windows build cannot report the state; any other failure throws.
inline std::optional<ThreadSpeedState> thread_speed_state(HANDLE thread)
{
    THREAD_POWER_THROTTLING_STATE value{};
    value.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    if (GetThreadInformation(thread, ThreadPowerThrottling, &value, sizeof value)) {
        return ThreadSpeedState{value.ControlMask, value.StateMask};
    }
    const auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED) {
        return std::nullopt;
    }
    wavplay::core::throw_win32_error(error, "GetThreadInformation(ThreadPowerThrottling)");
}

inline void note_unreported_speed(std::string_view field)
{
    std::printf("[NOTE] %.*s: this Windows build does not report thread power throttling; not checked.\n",
        static_cast<int>(field.size()), field.data());
}

// Compares a reported state with a speed's documented masks. False, after a
// note, where the state cannot be reported.
inline bool expect_speed_state(std::string_view field, const std::optional<ThreadSpeedState>& actual,
    wavplay::core::ExecutionSpeed speed, std::source_location location = std::source_location::current())
{
    if (!actual) {
        note_unreported_speed(field);
        return false;
    }
    const auto expected = documented_state(speed);
    expect(std::string(field) + " (control mask)", actual->control, expected.control, location);
    expect(std::string(field) + " (state mask)", actual->state, expected.state, location);
    return true;
}

inline bool expect_thread_speed(std::string_view field, HANDLE thread, wavplay::core::ExecutionSpeed speed,
    std::source_location location = std::source_location::current())
{
    return expect_speed_state(field, thread_speed_state(thread), speed, location);
}

// A thread's scheduling and memory priority. The product requests only the
// execution speed, so it must preserve the priorities supplied by its caller
// or the environment; an inherited memory priority need not be NORMAL.
struct ThreadPriorities {
    int scheduling{};
    ULONG memory{};
};

inline ThreadPriorities thread_priorities(HANDLE thread)
{
    const auto scheduling = GetThreadPriority(thread);
    if (scheduling == THREAD_PRIORITY_ERROR_RETURN) {
        wavplay::core::throw_last_error("GetThreadPriority");
    }
    MEMORY_PRIORITY_INFORMATION memory{};
    if (!GetThreadInformation(thread, ThreadMemoryPriority, &memory, sizeof memory)) {
        wavplay::core::throw_last_error("GetThreadInformation(ThreadMemoryPriority)");
    }
    return {scheduling, memory.MemoryPriority};
}

// Observe a separate, unmodified thread before invoking the product. This is
// an independent baseline for threads the product will create, including a
// process or job which supplied a lower default memory priority.
inline ThreadPriorities new_thread_priorities()
{
    return TestThread<ThreadPriorities>([] { return thread_priorities(GetCurrentThread()); }).get();
}

inline void expect_priorities(std::string_view field, const ThreadPriorities& actual, const ThreadPriorities& expected,
    std::source_location location = std::source_location::current())
{
    expect(std::string(field) + " (scheduling priority)", actual.scheduling, expected.scheduling, location);
    expect(std::string(field) + " (memory priority)", actual.memory, expected.memory, location);
}

inline ULONG process_memory_priority()
{
    MEMORY_PRIORITY_INFORMATION memory{};
    if (!GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory, sizeof memory)) {
        wavplay::core::throw_last_error("GetProcessInformation(ProcessMemoryPriority)");
    }
    return memory.MemoryPriority;
}

// SetProcessInformation documents ProcessMemoryPriority as the default for
// the process's threads. Reproduce a non-NORMAL inherited value, rather than
// depending on the CI host to provide it. Each suite owns a separate process.
inline void with_low_process_memory_priority(auto test)
{
    const auto before = process_memory_priority();
    {
        const struct Restore {
            MEMORY_PRIORITY_INFORMATION memory;
            ~Restore() { (void)SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory, sizeof memory); }
        } restore{{before}};
        MEMORY_PRIORITY_INFORMATION memory{MEMORY_PRIORITY_LOW};
        if (!SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory, sizeof memory)) {
            wavplay::core::throw_last_error("SetProcessInformation(ProcessMemoryPriority fixture)");
        }
        expect("the fixture requests low process memory priority", process_memory_priority(), ULONG{MEMORY_PRIORITY_LOW});
        expect("an independent new thread inherits low memory priority", new_thread_priorities().memory, ULONG{MEMORY_PRIORITY_LOW});
        test();
    }
    expect("the fixture restores process memory priority", process_memory_priority(), before);
}

// This process's priority class and power throttling, which the product
// never sets. The throttling is nullopt where this Windows build cannot report it.
struct ProcessState {
    DWORD priority_class{};
    std::optional<ThreadSpeedState> throttling;
    friend bool operator==(const ProcessState&, const ProcessState&) = default;
};

inline ProcessState process_state()
{
    ProcessState state{GetPriorityClass(GetCurrentProcess())};
    if (!state.priority_class) {
        wavplay::core::throw_last_error("GetPriorityClass");
    }
    PROCESS_POWER_THROTTLING_STATE value{};
    value.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &value, sizeof value)) {
        state.throttling = ThreadSpeedState{value.ControlMask, value.StateMask};
    } else if (const auto error = GetLastError(); error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED) {
        wavplay::core::throw_win32_error(error, "GetProcessInformation(ProcessPowerThrottling)");
    }
    return state;
}

// The thread's description (SetThreadDescription); empty if it has none.
inline std::wstring thread_description(HANDLE thread)
{
    PWSTR text{};
    const auto result = GetThreadDescription(thread, &text);
    const wavplay::core::LocalPtr<wchar_t> owned(text);
    if (FAILED(result)) {
        throw std::runtime_error("GetThreadDescription failed: HRESULT " +
            std::to_string(static_cast<unsigned long>(result)));
    }
    return text ? std::wstring(text) : std::wstring();
}

} // namespace wavplay_test
