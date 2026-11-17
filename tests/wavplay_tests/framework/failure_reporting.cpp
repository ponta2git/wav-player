#include "framework/failure_reporting.h"

#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>

namespace wavplay_test {
namespace {

// Read by the handlers below on whatever thread fails, so the names are
// immutable strings published atomically.
std::atomic<std::shared_ptr<const std::string>> current_suite, current_case;

// Prints "<tag> <suite> / <case>: <what>" to stderr, then flushes both streams
// so nothing a case printed is lost when the process exits without cleanup.
void report(const char* tag, std::string_view what) noexcept
{
    const auto suite = current_suite.load();
    const auto test = current_case.load();
    std::fprintf(stderr, "%s %s / %s: %.*s\n", tag, suite ? suite->c_str() : "(no suite)",
        test ? test->c_str() : "(no case)", static_cast<int>(what.size()), what.data());
    std::fflush(stdout);
    std::fflush(stderr);
}

[[noreturn]] void crash(std::string_view what) noexcept
{
    report("[CRASH]", what);
    std::_Exit(exit_code::crashed);
}

[[noreturn]] void on_terminate() noexcept
{
    if (const auto error = std::current_exception()) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            crash(std::string("std::terminate with ") + typeid(exception).name() + ": " + exception.what());
        } catch (...) {
            crash("std::terminate with a non-standard exception");
        }
    }
    crash("std::terminate without an active exception");
}

void on_abort(int) noexcept
{
    // In Debug, abort()'s own CRT report reaches on_crt_report first. Without this
    // handler abort() exits with 3. The CRT resets the handler before calling it,
    // so a second, concurrent abort can still exit with 3, which is a crash too.
    crash("abort() called");
}

void on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
    unsigned line, std::uintptr_t) noexcept
{
    char what[1024];
    if (expression && function && file) {
        std::snprintf(what, sizeof what, "invalid CRT parameter: %ls in %ls (%ls:%u)", expression, function, file, line);
    } else {
        std::snprintf(what, sizeof what, "invalid CRT parameter"); // Release CRTs pass no details.
    }
    crash(what);
}

#ifdef _DEBUG
// A Debug CRT assertion or error report, such as a failed STL check, detected
// heap corruption or an invalid heap pointer, ends the process as a crash after
// naming the case. Returning would let the CRT continue past the detection, and
// the suite could still exit as passed.
int __cdecl on_crt_report(int type, char* message, int*) noexcept
{
    if (type == _CRT_WARN || !message) {
        return FALSE; // Let the CRT write it to stderr.
    }
    std::string_view text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    report(type == _CRT_ASSERT ? "[CRT ASSERT]" : "[CRT ERROR]", text);
    std::_Exit(exit_code::crashed);
}
#endif

// AddressSanitizer reports the exceptions it handles itself and calls this
// filter only for the others. The process then ends with the exception code.
LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* exception) noexcept
{
    char what[64];
    std::snprintf(what, sizeof what, "unhandled exception 0x%08lX",
        static_cast<unsigned long>(exception->ExceptionRecord->ExceptionCode));
    report("[CRASH]", what);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void configure_unattended_failures()
{
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    (void)_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, on_crt_report);
#endif
    _set_abort_behavior(0, _CALL_REPORTFAULT); // Keep the abort message, skip fault reporting.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_invalid_parameter_handler(on_invalid_parameter);
    std::signal(SIGABRT, on_abort);
    SetUnhandledExceptionFilter(on_unhandled_exception);
    install_thread_failure_handlers();
}

void install_thread_failure_handlers()
{
    std::set_terminate(on_terminate);
}

void set_current_suite(std::wstring_view name)
{
    std::string narrow;
    for (const wchar_t character : name) {
        narrow += static_cast<char>(character); // Suite names are ASCII identifiers.
    }
    current_suite = name.empty() ? nullptr : std::make_shared<const std::string>(std::move(narrow));
}

CurrentCase::CurrentCase(std::string_view name)
    : enclosing_(current_case.exchange(std::make_shared<const std::string>(name)))
{
}

CurrentCase::~CurrentCase()
{
    current_case = std::move(enclosing_);
}

void abandon_hung(std::string_view what)
{
    report("[TIMEOUT]", what);
    std::_Exit(exit_code::hung);
}

} // namespace wavplay_test
