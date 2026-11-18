#include "framework/supervisor.h"

#include "framework/failure_reporting.h"
#include "wavplay/core/win32.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wavplay_test {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using wavplay::core::throw_last_error;
using wavplay::core::throw_win32_error;
using wavplay::core::UniqueHandle;

// A suite's wall-clock limit before hang_budget scales it, for a hang that no
// case watchdog covers. The slowest suite takes about half a minute in Release.
constexpr std::chrono::milliseconds suite_budget = 10min;
// How long a terminated job may take to release the child's process object.
constexpr DWORD termination_wait_ms = 10000;

enum class Verdict { pass, fail, timeout, crash };

struct Outcome {
    std::wstring_view suite;
    Verdict verdict{Verdict::crash};
    std::string detail;
    double seconds{};
};

std::wstring own_path()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            throw_last_error("GetModuleFileNameW");
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

#ifdef __SANITIZE_ADDRESS__
// A sanitizer report exits with 1 by default, which would read as an ordinary
// failure. Children inherit this environment; this process parsed its own
// options at startup. A later option overrides an earlier one.
void give_sanitizer_reports_their_own_exit_code()
{
    std::wstring options;
    if (const DWORD size = GetEnvironmentVariableW(L"ASAN_OPTIONS", nullptr, 0); size > 0) {
        options.resize(size);
        options.resize(GetEnvironmentVariableW(L"ASAN_OPTIONS", options.data(), size));
        options += L':';
    }
    options += L"exitcode=" + std::to_wstring(exit_code::asan_report);
    if (!SetEnvironmentVariableW(L"ASAN_OPTIONS", options.c_str())) {
        throw_last_error("SetEnvironmentVariableW");
    }
}
#endif

// A temporary file that receives one child's stdout and stderr, deleted when
// the handle closes. Only the child it is listed for inherits it.
UniqueHandle create_output_file()
{
    std::array<wchar_t, MAX_PATH + 1> directory{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    if (length == 0 || length >= directory.size()) {
        throw_last_error("GetTempPathW");
    }
    std::array<wchar_t, MAX_PATH + 1> path{};
    if (!GetTempFileNameW(directory.data(), L"wpt", 0, path.data())) {
        throw_last_error("GetTempFileNameW");
    }
    const HANDLE opened = CreateFileW(path.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        (void)DeleteFileW(path.data());
        throw_win32_error(error, "CreateFileW(suite output)");
    }
    UniqueHandle file(opened);
    if (!SetHandleInformation(file.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        throw_last_error("SetHandleInformation");
    }
    return file;
}

// Limits a child's inherited handles to its output file.
class InheritedHandle {
public:
    explicit InheritedHandle(HANDLE handle) : handle_(handle)
    {
        SIZE_T size{};
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &size); // Reports the required size.
        storage_.resize(size);
        if (!InitializeProcThreadAttributeList(list(), 1, 0, &size)) {
            throw_last_error("InitializeProcThreadAttributeList");
        }
        if (!UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &handle_, sizeof handle_,
                nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list());
            throw_win32_error(error, "UpdateProcThreadAttribute");
        }
    }
    ~InheritedHandle() { DeleteProcThreadAttributeList(list()); }
    InheritedHandle(const InheritedHandle&) = delete;
    InheritedHandle& operator=(const InheritedHandle&) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data()); }

private:
    HANDLE handle_;
    std::vector<std::byte> storage_;
};

// A running test child: its process inside a job that kills it, and anything it
// started, when the job handle closes.
struct Child {
    std::size_t index{}; // Into the outcomes, in the caller's order.
    UniqueHandle job, process, output;
    Clock::time_point started;
};

Child launch(const std::wstring& executable, std::wstring_view arguments, std::size_t index, DWORD flags = 0)
{
    Child child{index};
    child.output = create_output_file();
    child.job = UniqueHandle(CreateJobObjectW(nullptr, nullptr));
    if (!child.job) {
        throw_last_error("CreateJobObjectW");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        throw_last_error("SetInformationJobObject");
    }

    InheritedHandle inherited(child.output.get());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = child.output.get();
    startup.StartupInfo.hStdError = child.output.get();
    startup.lpAttributeList = inherited.list();
    std::wstring command = L'"' + executable + L"\" " + std::wstring(arguments);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            flags | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &created)) {
        throw_last_error("CreateProcessW");
    }
    child.process = UniqueHandle(created.hProcess);
    const UniqueHandle thread(created.hThread);
    // Started suspended, so no child code runs outside the owning job.
    if (!AssignProcessToJobObject(child.job.get(), child.process.get()) || ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(child.process.get(), exit_code::crashed);
        throw_win32_error(error, "Starting the test child process");
    }
    child.started = Clock::now();
    return child;
}

void classify(DWORD code, Outcome& outcome)
{
    switch (code) {
    case exit_code::passed:
        outcome.verdict = Verdict::pass;
        return;
    case exit_code::failed:
        outcome.verdict = Verdict::fail;
        outcome.detail = "failed cases (exit 1)";
        return;
    case exit_code::hung:
        outcome.verdict = Verdict::timeout;
        outcome.detail = "a watchdog abandoned a stuck case (exit 4)";
        return;
    case exit_code::asan_report:
        outcome.verdict = Verdict::crash;
        outcome.detail = "AddressSanitizer report (exit 86)";
        return;
    case exit_code::crashed:
        outcome.verdict = Verdict::crash;
        outcome.detail = "fatal error handler (exit 2)";
        return;
    case STATUS_STACK_BUFFER_OVERRUN: // Also the status of every __fastfail.
        outcome.verdict = Verdict::crash;
        outcome.detail = "fail-fast 0xC0000409, such as a /GS stack-cookie failure or another __fastfail";
        return;
    default: {
        outcome.verdict = Verdict::crash;
        char text[64];
        if (code >= 0xC0000000) { // An NTSTATUS error, such as an access violation.
            std::snprintf(text, sizeof text, "unhandled exception 0x%08lX", static_cast<unsigned long>(code));
        } else {
            std::snprintf(text, sizeof text, "exit %lu", static_cast<unsigned long>(code));
        }
        outcome.detail = text;
        return;
    }
    }
}

// Copies everything the child wrote to this process's stdout under its name,
// so the output of suites running at the same time never interleaves.
void print_output(std::wstring_view suite, HANDLE output)
{
    std::printf("== %.*ls ==\n", static_cast<int>(suite.size()), suite.data());
    LARGE_INTEGER start{};
    if (SetFilePointerEx(output, start, nullptr, FILE_BEGIN)) {
        std::array<char, 4096> buffer; // Small enough for the stack (C6262); suite output is short.
        DWORD read{};
        while (ReadFile(output, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            std::fwrite(buffer.data(), 1, read, stdout);
        }
    } else {
        std::printf("(the suite's output could not be read: Win32 error %lu)\n", GetLastError());
    }
    std::fflush(stdout);
}

// Everything a finished child wrote to its output file.
std::string read_output(HANDLE output)
{
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(output, start, nullptr, FILE_BEGIN)) {
        throw_last_error("SetFilePointerEx(child output)");
    }
    std::string text;
    std::array<char, 4096> buffer;
    DWORD read{};
    while (ReadFile(output, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
        text.append(buffer.data(), read);
    }
    return text;
}

const char* label(Verdict verdict) noexcept
{
    switch (verdict) {
    case Verdict::pass: return "PASS";
    case Verdict::fail: return "FAIL";
    case Verdict::timeout: return "TIMEOUT";
    case Verdict::crash: break;
    }
    return "CRASH";
}

} // namespace

unsigned default_supervised_jobs() noexcept
{
    return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
}

ChildRun run_own_child(std::wstring_view arguments, std::chrono::milliseconds timeout)
{
    const auto child = launch(own_path(), arguments, 0, CREATE_NO_WINDOW);
    ChildRun run;
    const auto wait_ms = static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(timeout.count(), 0, INFINITE - 1));
    const auto wake = WaitForSingleObject(child.process.get(), wait_ms);
    if (wake == WAIT_FAILED) {
        throw_last_error("WaitForSingleObject(child)");
    }
    if (wake == WAIT_TIMEOUT) {
        (void)TerminateJobObject(child.job.get(), exit_code::hung);
        (void)WaitForSingleObject(child.process.get(), termination_wait_ms);
        run.timed_out = true;
        run.exit_code = exit_code::hung;
    } else {
        DWORD code{};
        if (!GetExitCodeProcess(child.process.get(), &code)) {
            throw_last_error("GetExitCodeProcess(child)");
        }
        run.exit_code = code;
    }
    run.output = read_output(child.output.get());
    return run;
}

int supervise_suites(std::span<const SupervisedSuite> suites, unsigned jobs)
{
    jobs = std::clamp(jobs, 1u, static_cast<unsigned>(MAXIMUM_WAIT_OBJECTS));
#ifdef __SANITIZE_ADDRESS__
    give_sanitizer_reports_their_own_exit_code();
#endif
    const auto executable = own_path();
    const auto started = Clock::now();
    constexpr auto budget = hang_budget(suite_budget);

    std::vector<Outcome> outcomes(suites.size());
    std::vector<std::size_t> order(suites.size());
    for (std::size_t i = 0; i < suites.size(); ++i) {
        outcomes[i].suite = suites[i].name;
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return suites[a].priority > suites[b].priority;
    });
    std::printf("Running %zu suites, up to %u at a time.\n", suites.size(), jobs);
    std::fflush(stdout);

    std::vector<Child> running;
    const auto finish = [&](std::size_t slot, std::optional<DWORD> exit_code) {
        auto& child = running[slot];
        auto& outcome = outcomes[child.index];
        if (exit_code) {
            classify(*exit_code, outcome);
        } else {
            outcome.verdict = Verdict::timeout;
            outcome.detail = "terminated after the supervisor's " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(budget).count()) + " s budget";
        }
        outcome.seconds = std::chrono::duration<double>(Clock::now() - child.started).count();
        print_output(outcome.suite, child.output.get());
        running.erase(running.begin() + static_cast<std::ptrdiff_t>(slot));
    };

    std::size_t next = 0;
    while (next < order.size() || !running.empty()) {
        while (running.size() < jobs && next < order.size()) {
            const auto index = order[next++];
            try {
                running.push_back(launch(executable, L"--suite " + std::wstring(suites[index].name), index));
            } catch (const std::exception& error) {
                outcomes[index].detail = std::string("could not run the suite: ") + error.what();
                std::printf("== %.*ls ==\n%s\n", static_cast<int>(suites[index].name.size()), suites[index].name.data(),
                    outcomes[index].detail.c_str());
            }
        }
        if (running.empty()) {
            continue;
        }
        std::vector<HANDLE> processes;
        auto earliest = Clock::time_point::max();
        for (const auto& child : running) {
            processes.push_back(child.process.get());
            earliest = std::min(earliest, child.started + budget);
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(earliest - Clock::now());
        const auto wait_ms = static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(remaining.count(), 0, INFINITE - 1));
        const DWORD woke = WaitForMultipleObjects(static_cast<DWORD>(processes.size()), processes.data(), FALSE, wait_ms);
        if (woke < WAIT_OBJECT_0 + processes.size()) {
            const std::size_t slot = woke - WAIT_OBJECT_0;
            DWORD code{};
            if (!GetExitCodeProcess(running[slot].process.get(), &code)) {
                code = exit_code::crashed; // The verdict cannot be read: never report a pass.
            }
            finish(slot, code);
        } else if (woke == WAIT_TIMEOUT) {
            const auto now = Clock::now();
            for (std::size_t slot = running.size(); slot-- > 0;) {
                if (running[slot].started + budget <= now) {
                    (void)TerminateJobObject(running[slot].job.get(), exit_code::hung);
                    (void)WaitForSingleObject(running[slot].process.get(), termination_wait_ms);
                    finish(slot, std::nullopt);
                }
            }
        } else {
            throw_last_error("WaitForMultipleObjects(suites)");
        }
    }

    std::printf("== Summary ==\n");
    std::size_t counts[4]{};
    for (const auto& outcome : outcomes) {
        ++counts[static_cast<std::size_t>(outcome.verdict)];
        std::printf("%-7s %-20.*ls %7.1f s%s%s\n", label(outcome.verdict), static_cast<int>(outcome.suite.size()),
            outcome.suite.data(), outcome.seconds, outcome.detail.empty() ? "" : "  ", outcome.detail.c_str());
    }
    std::printf("%zu suites in %.1f s: %zu passed, %zu failed, %zu timed out, %zu crashed.\n", outcomes.size(),
        std::chrono::duration<double>(Clock::now() - started).count(), counts[0], counts[1], counts[2], counts[3]);
    std::fflush(stdout);
    return counts[0] == outcomes.size() ? exit_code::passed : exit_code::failed;
}

} // namespace wavplay_test
