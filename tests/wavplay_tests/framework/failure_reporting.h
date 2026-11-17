#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace wavplay_test {

// The exit codes the supervisor classifies. Any other code, such as an
// unhandled SEH exception's status, is a crash as well.
namespace exit_code {
inline constexpr int passed = 0;
inline constexpr int failed = 1;      // At least one case failed an assertion or threw.
inline constexpr int crashed = 2;     // std::terminate, abort, an invalid CRT parameter or a Debug CRT report.
inline constexpr int hung = 4;        // abandon_hung: a watchdog gave up on a stuck thread. Not 3, which
                                      // the UCRT's abort() uses once the SIGABRT handler has been reset.
inline constexpr int asan_report = 86; // ASAN_OPTIONS exitcode the supervisor sets for its children.
} // namespace exit_code

// Debug CRT reports, abort and unhandled SEH exceptions open modal dialogs or
// Windows Error Reporting by default. Route them to stderr instead, and make a
// fatal error name the current suite and case before the process exits as a
// crash. Call first in wmain. AddressSanitizer keeps its own report.
void configure_unattended_failures();

// The terminate handler is per thread in the MSVC runtime. TestThread calls this
// first, so a case's std::terminate names the exception. On other threads
// (driver doubles' clocks and product threads) std::terminate ends
// in abort, which is still reported as a crash of the case: in Debug by the
// CRT's "abort() has been called" report ([CRT ERROR]), otherwise by the SIGABRT
// handler.
void install_thread_failure_handlers();

// The suite and case named in crash, hang and fatal-error reports. Both are
// copied: a case name may be a temporary string's c_str().
void set_current_suite(std::wstring_view name);

// Names the running case for one scope, then restores the enclosing one.
class CurrentCase {
public:
    explicit CurrentCase(std::string_view name);
    ~CurrentCase();
    CurrentCase(const CurrentCase&) = delete;
    CurrentCase& operator=(const CurrentCase&) = delete;

private:
    std::shared_ptr<const std::string> enclosing_;
};

// A watchdog's last resort when a thread is stuck: unwinding would join it.
// Prints the current case and what timed out, flushes stdout and stderr, and
// exits with exit_code::hung without running destructors.
[[noreturn]] void abandon_hung(std::string_view what);

// Debug and AddressSanitizer builds need more headroom for unoptimized and
// instrumented test work. Every watchdog derives its limit from a Release
// base through these configuration factors. Product timing is never scaled.
#if defined(__SANITIZE_ADDRESS__)
inline constexpr int hang_budget_scale = 4;
#elif defined(_DEBUG)
inline constexpr int hang_budget_scale = 3;
#else
inline constexpr int hang_budget_scale = 1;
#endif

constexpr std::chrono::milliseconds hang_budget(std::chrono::milliseconds base) noexcept
{
    return base * hang_budget_scale;
}

} // namespace wavplay_test
