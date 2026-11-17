#include "suites/suites.h"
#include "doubles/registered_driver.h"
#include "framework/failure_reporting.h"
#include "framework/supervisor.h"
#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_registry.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/core/win32.h"

#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace wavplay_test;

constexpr CLSID missing{0x76543211, 0x89ab, 0xcdef, {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};

void test_buffer_selection()
{
    using wavplay::asio::accepts_buffer_frames;
    using wavplay::asio::BufferSizeRange;
    using wavplay::asio::select_buffer_frames;
    // The non-throwing predicate agrees with the selector on every case.
    const auto check = [](BufferSizeRange range, std::initializer_list<long> valid,
                           std::initializer_list<long> invalid) {
        expect("zero selects the reported preferred width", select_buffer_frames(range, 0), range.preferred);
        expect_true("zero is accepted", accepts_buffer_frames(range, 0));
        for (const auto frames : valid) {
            expect("explicit buffer width is unchanged", select_buffer_frames(range, frames), frames);
            expect_true("supported width is accepted", accepts_buffer_frames(range, frames));
        }
        for (const auto frames : invalid) {
            expect_throw<std::invalid_argument>("unsupported manual buffer size", [&] {
                (void)select_buffer_frames(range, frames);
            });
            expect_true("unsupported width is refused", !accepts_buffer_frames(range, frames));
        }
    };
    check({3, 3, 3, 0}, {3}, {-1, 2, 4});
    check({1, 544, 8, 0}, {1, 47, 544}, {-1, 545});
    check({3, 12, 7, 4}, {3, 7, 11}, {-1, 2, 4, 8, 12, 13});
    check({3, 1025, 64, -1}, {4, 8, 64, 1024}, {-1, 2, 3, 47, 1025});
    constexpr auto largest = std::numeric_limits<long>::max();
    check({1, largest, 1, -1}, {1, 2, 1L << 30}, {3, largest, std::numeric_limits<long>::min()});
    check({1, largest, 1, 0}, {1, largest}, {std::numeric_limits<long>::min()});
    check({1, largest, 1, largest - 1}, {1, largest}, {largest - 1});
    std::string reason;
    try {
        (void)select_buffer_frames({8, 512, 64, 8}, 47);
    } catch (const std::invalid_argument& error) {
        reason = error.what();
    }
    for (const auto* value : {"requested=47", "minimum=8", "preferred=64", "maximum=512", "granularity=8"}) {
        expect_true("unsupported width explains the values needed to edit settings", reason.find(value) != std::string::npos);
    }
    for (const BufferSizeRange range : {BufferSizeRange{0, 8, 4, 1}, {-1, 8, 4, 1}, {4, 3, 4, 0},
             {1, 8, 0, 0}, {1, 8, 9, 0}, {1, 9, 4, 2}, {2, 16, 6, -1}, {2, 8, 4, -2}}) {
        for (const auto requested : {0L, 4L}) {
            expect_throw<std::runtime_error>("manual selection rejects a contradictory driver range", [&] {
                (void)select_buffer_frames(range, requested);
            });
            expect_true("a contradictory driver range accepts nothing", !accepts_buffer_frames(range, requested));
        }
    }
}

void test_error_names()
{
    using wavplay::asio::error_name;
    static_assert(error_name(ASE_NoClock) == "ASE_NoClock", "names are constant expressions");
    const std::pair<ASIOError, std::string_view> named[]{{ASE_OK, "ASE_OK"}, {ASE_SUCCESS, "ASE_SUCCESS"},
        {ASE_NotPresent, "ASE_NotPresent"}, {ASE_HWMalfunction, "ASE_HWMalfunction"},
        {ASE_InvalidParameter, "ASE_InvalidParameter"}, {ASE_InvalidMode, "ASE_InvalidMode"},
        {ASE_SPNotAdvancing, "ASE_SPNotAdvancing"}, {ASE_NoClock, "ASE_NoClock"}, {ASE_NoMemory, "ASE_NoMemory"}};
    for (const auto& [code, name] : named) {
        if (error_name(code) != name) {
            throw std::runtime_error("Wrong name for ASIOError " + std::to_string(code));
        }
    }
    for (const long code : {ASE_NotPresent - 1L, ASE_NoMemory + 1L, 1L, ASE_SUCCESS + 1L,
             std::numeric_limits<long>::min(), std::numeric_limits<long>::max()}) {
        expect_true("unknown ASIOError value", error_name(code) == "Unknown");
    }
    // The operation stays a stable prefix; the name and the decimal value follow.
    const auto failure = [](ASIOError code, const char* operation) {
        try {
            wavplay::asio::throw_if_error(code, operation);
        } catch (const std::runtime_error& error) {
            return std::string(error.what());
        }
        return std::string("no exception");
    };
    expect_true("ASE_OK does not throw", failure(ASE_OK, "ASIO start failed") == "no exception");
    const std::pair<std::string, std::string_view> messages[]{
        {failure(ASE_HWMalfunction, "ASIO start failed"), "ASIO start failed: ASE_HWMalfunction (-999)"},
        {failure(ASE_NotPresent, "ASIO getChannels failed"), "ASIO getChannels failed: ASE_NotPresent (-1000)"},
        {failure(ASE_SUCCESS, "ASIO future failed"), "ASIO future failed: ASE_SUCCESS (1061701536)"},
        {failure(7, "ASIO stop failed"), "ASIO stop failed: Unknown (7)"},
        {failure(std::numeric_limits<long>::min(), "ASIO stop failed"), "ASIO stop failed: Unknown (-2147483648)"}};
    for (const auto& [actual, expected] : messages) {
        if (actual != expected) {
            throw std::runtime_error("Unexpected driver failure message: " + actual);
        }
    }
}

void test_error_cause()
{
    try {
        try {
            throw std::system_error(ERROR_READ_FAULT, std::system_category(), "reading PCM");
        } catch (...) {
            wavplay::asio::throw_if_error(ASE_OK, "successful cleanup");
            wavplay::asio::throw_if_error(ASE_NoClock, "ASIO cleanup failed");
        }
        fail("expected the ASIO cleanup failure");
    } catch (const std::runtime_error& error) {
        expect_true("cleanup names its own operation and code",
            std::string_view(error.what()) == "ASIO cleanup failed: ASE_NoClock (-995)");
        try {
            std::rethrow_if_nested(error);
            fail("the cleanup error lost the original typed cause");
        } catch (const std::system_error& cause) {
            expect("the original Win32 error survives ASIO cleanup", cause.code().value(), ERROR_READ_FAULT);
            expect_true("the original error category survives ASIO cleanup", cause.code().category() == std::system_category());
            expect_true("the original operation survives ASIO cleanup", std::string_view(cause.what()).starts_with("reading PCM"));
        }
    }
    try {
        wavplay::asio::throw_if_error(ASE_NoClock, "ASIO operation failed");
        fail("expected an ordinary ASIO error");
    } catch (const std::runtime_error& error) {
        std::rethrow_if_nested(error); // No nested_exception with an empty nested_ptr on a leaf.
    }
}

void test_buffer_settings_lifetime()
{
    RegisteredDriver driver;
    for (const auto frames : {-1L, std::numeric_limits<long>::min()}) {
        expect_throw<std::invalid_argument>("negative buffer settings rejected before activation", [&] {
            wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.buffer_frames = frames});
        });
    }
    expect("invalid buffer settings never activate the driver", driver.attempts, 0);
    {
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation(), {.buffer_frames = 47});
        {
            wavplay::asio::DriverLease device(session);
            expect("lease retains manual buffer setting", device.buffer_frames(), 47);
            (void)device.reopen();
            expect("reopen retains manual buffer setting", device.buffer_frames(), 47);
        }
        expect("settings alone never query or configure buffers", driver.calls.size(), 0);
        session.close();
    }
    expect("manual settings survive both activations", driver.activations, 2);
    driver.check_closed();
}

void test_probe()
{
    RegisteredDriver driver;
    driver.sample_rate = 88200;
    driver.outputs = 8;
    driver.left_type = ASIOSTInt32LSB;
    driver.right_type = ASIOSTFloat32MSB;
    driver.buffer_size = {32, 1024, 128, -1};

    const auto result = probe(driver);
    expect("host window passed to init", static_cast<int>(driver.host == GetDesktopWindow()), 1);
    expect("current clock", static_cast<int>(result.sample_rate == 88200), 1);
    expect("output channels", result.output_channels, 8);
    expect("output 1 format", result.output_types[0], static_cast<long>(ASIOSTInt32LSB));
    expect("output 2 format", result.output_types[1], static_cast<long>(ASIOSTFloat32MSB));
    expect("minimum frames", result.buffer_size.minimum, 32);
    expect("maximum frames", result.buffer_size.maximum, 1024);
    expect("preferred frames", result.buffer_size.preferred, 128);
    expect("granularity", result.buffer_size.granularity, -1);
    driver.check_probe_closed();

    driver.right_type = ASIOSTInt32LSB;
    driver.sample_rate = 44100;
    const auto reopened = probe(driver);
    expect("fresh clock on reopen", static_cast<int>(reopened.sample_rate == 44100), 1);
    expect("fresh output 2 format", reopened.output_types[1], static_cast<long>(ASIOSTInt32LSB));
    expect("same CLSID reopened", driver.inits, 2);
    driver.check_probe_closed();
}

void test_open_failures()
{
    RegisteredDriver driver;
    auto expect_reason = [&](const CLSID& clsid, std::string_view reason) {
        try {
            probe(driver, clsid);
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find(reason) == std::string_view::npos) {
                throw std::runtime_error("Missing failure reason: " + std::string(error.what()));
            }
            driver.check_probe_closed();
            return;
        }
        throw std::runtime_error("Opening the selected device unexpectedly succeeded");
    };
    expect_reason(missing, "Selected CLSID is absent");
    expect("missing CLSID attempts", driver.attempts, 1);
    expect("missing CLSID activations", driver.activations, 0);

    driver.fail_init = true;
    expect_reason(selected, "mock device is busy");
    expect("init failure attempts", driver.attempts, 2);
    expect("init failure queries", driver.calls.size(), 0);
    driver.fail_init = false;
    probe(driver);
    driver.check_probe_closed();
}

void test_query_failures()
{
    RegisteredDriver driver;
    auto reject_and_retry = [&](const char* label, auto& field, auto invalid) {
        const auto previous = std::exchange(field, invalid);
        const auto before = driver.attempts;
        expect_throw<std::runtime_error>(label, [&] { probe(driver); });
        expect("attempts on query failure", driver.attempts, before + 1);
        driver.check_probe_closed();
        field = previous;
        probe(driver);
        driver.check_probe_closed();
    };
    reject_and_retry("missing clock", driver.sample_rate_result, ASE_NoClock);
    reject_and_retry("invalid clock", driver.sample_rate, std::numeric_limits<double>::quiet_NaN());
    reject_and_retry("failed channel count", driver.outputs, 0);
    reject_and_retry("output 2 is absent", driver.outputs, 1);
    reject_and_retry("failed output 1 format", driver.left_info_result, ASE_HWMalfunction);
    reject_and_retry("failed output 2 format", driver.right_info_result, ASE_HWMalfunction);
    reject_and_retry("misidentified output 2", driver.wrong_channel, true);
    reject_and_retry("failed buffer size", driver.buffer_size_result, ASE_HWMalfunction);
    reject_and_retry("invalid buffer size", driver.buffer_size.minimum, driver.buffer_size.maximum + 1);
}

void test_explicit_clsid()
{
    const auto parsed = wavplay::asio::parse_driver_clsid(L"{76543210-89ab-CDEF-8123-456789abcdef}");
    expect("parsed selected CLSID", static_cast<int>(IsEqualCLSID(parsed, selected)), 1);
    for (const auto* input : {L"", L"WAV.Player", L"{76543210-89ab-CDEF-8123-456789abcdef}suffix"}) {
        expect_throw<std::invalid_argument>("invalid CLSID", [=] { (void)wavplay::asio::parse_driver_clsid(input); });
    }
}

void test_existing_apartments()
{
    RegisteredDriver driver;
    // The session asks for an STA without OLE1 DDE; either caller STA flavor is the same apartment.
    constexpr DWORD caller_sta_flags[]{COINIT_APARTMENTTHREADED, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE};
    for (const auto flags : caller_sta_flags) {
        expect("create caller STA", CoInitializeEx(nullptr, flags), S_OK);
        probe(driver);
        const bool caller_sta_preserved = in_sta();
        CoUninitialize();
        expect("nested session preserved caller STA", static_cast<int>(caller_sta_preserved), 1);
        driver.check_probe_closed();
    }

    expect("create incompatible caller MTA", CoInitializeEx(nullptr, COINIT_MULTITHREADED), S_OK);
    const auto attempts = driver.attempts;
    expect_throw<std::runtime_error>("MTA rejected", [&] { probe(driver); });
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    const auto result = CoGetApartmentType(&apartment, &qualifier);
    CoUninitialize();
    expect("incompatible caller apartment still initialized", result, S_OK);
    expect("incompatible caller apartment preserved", static_cast<int>(apartment), static_cast<int>(APTTYPE_MTA));
    expect("no activation from MTA", driver.attempts, attempts);
}

// Points the process's standard error handle elsewhere for one scope.
class StandardError {
public:
    explicit StandardError(HANDLE replacement) : saved_(GetStdHandle(STD_ERROR_HANDLE))
    {
        if (!SetStdHandle(STD_ERROR_HANDLE, replacement)) {
            throw std::runtime_error("Cannot redirect standard error");
        }
    }
    ~StandardError() { (void)SetStdHandle(STD_ERROR_HANDLE, saved_); }
    StandardError(const StandardError&) = delete;
    StandardError& operator=(const StandardError&) = delete;

private:
    HANDLE saved_;
};

// Destroys an idle session whose clock change the driver refuses to undo, with
// `error` as the process's standard error handle.
void destroy_unrestorable(HANDLE error)
{
    RegisteredDriver driver;
    {
        const StandardError redirect(error); // Outlives the session.
        wavplay::asio::DriverSession session(selected, GetDesktopWindow(), driver.activation());
        {
            wavplay::asio::DriverLease lease(session);
            expect("clock changed for playback", lease.set_sample_rate(44100, 48000), ASE_OK);
        }
        driver.set_rate_result = ASE_InvalidMode;
    }
    expect("restoration attempted at teardown", driver.set_rates.size(), 2);
    driver.check_closed();
}

// ~DriverSession reports a refused clock restoration through the Win32
// standard error handle with the bytes CRT text-mode stderr wrote, and skips
// the write when the process has no usable handle (a GUI process).
void test_teardown_report()
{
    HANDLE read_end{}, write_end{};
    if (!CreatePipe(&read_end, &write_end, nullptr, 0)) {
        throw std::runtime_error("Cannot create a standard error pipe");
    }
    const wavplay::core::UniqueHandle reader(read_end);
    wavplay::core::UniqueHandle writer(write_end);
    destroy_unrestorable(writer.get());
    writer.reset(); // The reader reaches the end after the reported line.
    std::string reported;
    char chunk[256];
    DWORD count{};
    while (ReadFile(reader.get(), chunk, sizeof(chunk), &count, nullptr) && count != 0) {
        reported.append(chunk, count);
    }
    if (reported != "Failed to restore original ASIO sample rate: setSampleRate failed: ASE_InvalidMode (-997)\r\n") {
        throw std::runtime_error("Unexpected teardown report on standard error: " + reported);
    }
    for (const HANDLE unusable : {HANDLE{}, INVALID_HANDLE_VALUE}) {
        destroy_unrestorable(unusable);
    }
}

// The text that OutputDebugStringA hands to a debugger, on the thread that
// creates this object. Without a debugger attached each call raises
// DBG_PRINTEXCEPTION_C; a vectored handler sees it first and passes it on, so
// the output still reaches the system.
class DebugOutputCapture {
public:
    DebugOutputCapture() : thread_(GetCurrentThreadId())
    {
        active_ = this;
        handler_ = AddVectoredExceptionHandler(1, &DebugOutputCapture::handle);
        if (!handler_) {
            active_ = nullptr;
            throw std::runtime_error("Cannot add a vectored exception handler");
        }
    }
    ~DebugOutputCapture()
    {
        (void)RemoveVectoredExceptionHandler(handler_);
        active_ = nullptr;
    }
    DebugOutputCapture(const DebugOutputCapture&) = delete;
    DebugOutputCapture& operator=(const DebugOutputCapture&) = delete;

    // Whether every captured text could be stored.
    [[nodiscard]] bool complete() const noexcept { return !failed_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    static LONG CALLBACK handle(EXCEPTION_POINTERS* exception) noexcept
    {
        const auto* record = exception->ExceptionRecord;
        auto* const self = active_.load();
        if (self && GetCurrentThreadId() == self->thread_ && record->ExceptionCode == DBG_PRINTEXCEPTION_C &&
            record->NumberParameters >= 2) {
            // Parameter 0 is the length with the terminating null, 1 the text.
            const auto* text = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
            const auto length = static_cast<std::size_t>(record->ExceptionInformation[0]);
            try {
                self->text_.append(text ? text : "(null)", text ? strnlen(text, length) : 6);
            } catch (...) {
                self->failed_ = true;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static inline std::atomic<DebugOutputCapture*> active_{};
    DWORD thread_;
    PVOID handler_{};
    std::string text_;
    bool failed_{};
};

// The debugger output receives the same line, CR LF included: the only
// destination of a GUI process, which has no standard error handle.
void test_teardown_debug_output()
{
    if (IsDebuggerPresent()) {
        std::printf("[NOTE] teardown debug output: an attached debugger receives it first; not checked.\n");
        return;
    }
    const DebugOutputCapture captured;
    destroy_unrestorable(nullptr);
    if (!captured.complete() || captured.text() !=
        "Failed to restore original ASIO sample rate: setSampleRate failed: ASE_InvalidMode (-997)\r\n") {
        fail("Unexpected teardown report on the debugger output: " + captured.text());
    }
}

// Destroys a session while a lease plays through it. The lease is never
// destroyed, so a session that did not end the process leaves no dangling
// lease behind.
void child_destroys_leased_session()
{
    RegisteredDriver driver;
    auto session = std::make_unique<wavplay::asio::DriverSession>(selected, GetDesktopWindow(), driver.activation());
    alignas(wavplay::asio::DriverLease) std::byte lease[sizeof(wavplay::asio::DriverLease)];
    (void)std::construct_at(reinterpret_cast<wavplay::asio::DriverLease*>(lease), *session);
    session.reset();
}

// Releasing the driver and the STA under live buffers is never attempted: the
// destructor reports and ends the process through std::terminate (the test
// executable's handler reports it and exits with exit_code::crashed).
void test_destroyed_during_playback()
{
    const auto child = run_own_child(L"--leased-session-child", hang_budget(std::chrono::seconds(10)));
    if (child.timed_out) {
        fail("the leased-session child did not exit: " + child.output);
    }
    if (child.exit_code != exit_code::crashed) {
        fail("the leased-session child exited with " + std::to_string(child.exit_code) + ": " + child.output);
    }
    if (!child.output.starts_with("ASIO driver session destroyed during playback\r\n") ||
        child.output.find("std::terminate without an active exception") == std::string::npos) {
        fail("Unexpected report of a session destroyed during playback: " + child.output);
    }
}

} // namespace

int wavplay_test::run_leased_session_child(std::span<wchar_t* const> operands)
{
    if (!operands.empty()) {
        return 2;
    }
    try {
        child_destroys_leased_session();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::fputs("The leased session was destroyed without ending the process.\n", stderr);
    return 1;
}

int wavplay_test::run_driver_session_tests()
{
    TestSuite suite;
    suite.run("exact ASIO buffer selection and range boundaries", test_buffer_selection);
    suite.run("ASIOError names in driver failure messages", test_error_names);
    suite.run("ASIO cleanup errors preserve the typed original cause", test_error_cause);
    suite.run_timed("buffer settings validation and reopen lifetime", test_buffer_settings_lifetime);
    suite.run("explicit CLSID syntax", test_explicit_clsid);
    suite.run_timed("read-only ASIO capability query and reopen", test_probe);
    suite.run_timed("ASIO activation and initialization failures", test_open_failures);
    suite.run_timed("ASIO capability query failures and reopen", test_query_failures);
    suite.run_timed("existing caller COM apartment ownership", test_existing_apartments);
    suite.run_timed("teardown report of a refused clock restoration", test_teardown_report);
    suite.run_timed("teardown report reaches the debugger output as one CRLF line", test_teardown_debug_output);
    suite.run_timed("a session destroyed during playback reports it and terminates the process",
        test_destroyed_during_playback);
    return suite.result("ASIO driver session");
}
