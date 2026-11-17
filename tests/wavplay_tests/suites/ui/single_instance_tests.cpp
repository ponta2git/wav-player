#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/ui_desktop_guard.h"
#include "wavplay/core/win32.h"
#include "wavplay/ui/single_instance.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
using wavplay::core::UniqueHandle;
using wavplay::ui::SingleInstance;

constexpr std::size_t child_count = 4;
struct ChildReport {
    LONG role{}; // 1: primary, 2: activated existing, -1: refused/timeout.
    LONG activations{};
    LONG ready{};
    LONG iconic{};
    LONG visible{};
    LONG64 window{};
};
struct Reports { ChildReport child[child_count]; };
struct UnmapReports { void operator()(Reports* value) const noexcept { (void)UnmapViewOfFile(value); } };

std::wstring report_name(std::wstring_view scope, std::wstring_view suffix)
{
    return L"Local\\WAVPlayer.InstanceTests." + std::wstring(scope) + std::wstring(suffix);
}

LONG read(LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
void write(LONG& value, LONG result) { (void)InterlockedExchange(&value, result); }

void await(auto predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(5s);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) { fail("Single-instance child did not reach its checkpoint"); }
        Sleep(1);
    }
}

std::wstring executable_path()
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { wavplay::core::throw_last_error("GetModuleFileNameW(instance test)"); }
    path.resize(length);
    return path;
}

struct Fixture {
    std::wstring scope;
    UniqueHandle mapping, start, attach, finish, job;
    std::unique_ptr<Reports, UnmapReports> reports;
    std::vector<UniqueHandle> children;

    Fixture()
    {
        static std::atomic<unsigned> sequence{};
        scope = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Reports),
            report_name(scope, L".reports").c_str()));
        if (!mapping) { wavplay::core::throw_last_error("CreateFileMappingW(instance tests)"); }
        reports.reset(static_cast<Reports*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Reports))));
        if (!reports) { wavplay::core::throw_last_error("MapViewOfFile(instance tests)"); }
        const auto event = [&](std::wstring_view suffix) {
            UniqueHandle value(CreateEventW(nullptr, TRUE, FALSE, report_name(scope, suffix).c_str()));
            if (!value) { wavplay::core::throw_last_error("CreateEventW(instance tests)"); }
            return value;
        };
        start = event(L".start");
        attach = event(L".attach");
        finish = event(L".finish");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job) { wavplay::core::throw_last_error("CreateJobObjectW(instance tests)"); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            wavplay::core::throw_last_error("SetInformationJobObject(instance tests)");
        }
    }

    HANDLE launch(unsigned index, std::wstring_view mode)
    {
        const auto path = executable_path();
        std::wstring command = L'"' + path + L"\" --single-instance-child " + scope + L' ' +
            std::to_wstring(index) + L' ' + std::wstring(mode);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &created)) {
            wavplay::core::throw_last_error("CreateProcessW(instance tests)");
        }
        UniqueHandle process(created.hProcess), thread(created.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            const auto error = GetLastError();
            (void)TerminateProcess(process.get(), 1);
            wavplay::core::throw_win32_error(error, "AssignProcessToJobObject(instance tests)");
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            wavplay::core::throw_last_error("ResumeThread(instance tests)");
        }
        const auto raw = process.get();
        children.push_back(std::move(process));
        return raw;
    }

    void done(HANDLE process, DWORD expected = 0)
    {
        expect("child exits within deadline", WaitForSingleObject(process, static_cast<DWORD>(hang_budget(5s).count())),
            DWORD{WAIT_OBJECT_0});
        DWORD code{};
        expect_true("read child exit code", GetExitCodeProcess(process, &code) != FALSE);
        expect("child exit code", code, expected);
    }

    ~Fixture() { (void)SetEvent(finish.get()); }
};

struct ChildWindow {
    SingleInstance& instance;
    ChildReport& report;
    HWND window{};
    HANDLE closing_gate{};

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<ChildWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ChildWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && self->instance.handle_activation(message)) {
            InterlockedIncrement(&self->report.activations);
            write(self->report.iconic, IsIconic(window) ? 1 : 0);
            write(self->report.visible, IsWindowVisible(window) ? 1 : 0);
            if (self->closing_gate) {
                // Deterministically close after the sender observed readiness
                // but before it receives an acknowledgement. Keep ownership
                // until the parent lets teardown finish.
                self->instance.detach();
                (void)DestroyWindow(window);
                self->window = nullptr;
                write(self->report.ready, 2);
                (void)WaitForSingleObject(self->closing_gate, static_cast<DWORD>(hang_budget(5s).count()));
                return 0;
            }
            return 1;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void create(bool minimized)
    {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WAVPlayer.SingleInstance.TestWindow";
        if (!RegisterClassW(&type)) { wavplay::core::throw_last_error("RegisterClassW(instance tests)"); }
        window = CreateWindowExW(0, type.lpszClassName, L"WAV Player instance test", WS_OVERLAPPEDWINDOW,
            0, 0, 240, 120, nullptr, nullptr, type.hInstance, this);
        if (!window) { wavplay::core::throw_last_error("CreateWindowExW(instance tests)"); }
        if (minimized) { (void)ShowWindow(window, SW_SHOWMINNOACTIVE); }
        instance.attach(window);
        InterlockedExchange64(&report.window, static_cast<LONG64>(reinterpret_cast<std::uintptr_t>(window)));
        write(report.iconic, IsIconic(window) ? 1 : 0);
        write(report.ready, 1);
    }

    ~ChildWindow()
    {
        instance.detach();
        if (window) { (void)DestroyWindow(window); }
    }
};

void pump_until(HANDLE event)
{
    const auto deadline = std::chrono::steady_clock::now() + hang_budget(10s);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
            std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { throw std::runtime_error("Instance test child timed out"); }
        const auto result = MsgWaitForMultipleObjectsEx(1, &event, static_cast<DWORD>(remaining),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) { return; }
        if (result != WAIT_OBJECT_0 + 1) { throw std::runtime_error("Instance test message wait failed"); }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void simultaneous_launches()
{
    Fixture fixture;
    for (unsigned index = 0; index < child_count; ++index) { (void)fixture.launch(index, L"window"); }
    (void)SetEvent(fixture.start.get());
    (void)SetEvent(fixture.attach.get());
    await([&] {
        unsigned finished{};
        for (auto& report : fixture.reports->child) {
            if (read(report.role) == 2) { ++finished; }
        }
        return finished == child_count - 1;
    });
    unsigned primary{}, activated{};
    for (auto& report : fixture.reports->child) {
        if (read(report.role) == 1) {
            ++primary;
            activated += static_cast<unsigned>(read(report.activations));
        }
    }
    expect("only one simultaneous launch becomes primary", primary, 1u);
    expect("every later launch reaches the existing window", activated, static_cast<unsigned>(child_count - 1));
    (void)SetEvent(fixture.finish.get());
    for (const auto& child : fixture.children) { fixture.done(child.get()); }
}

void waits_for_window_and_restores()
{
    Fixture fixture;
    const auto primary = fixture.launch(0, L"minimized");
    (void)SetEvent(fixture.start.get());
    await([&] { return read(fixture.reports->child[0].role) == 1; });
    const auto later = fixture.launch(1, L"window");
    expect("later launch waits while owner has no window", WaitForSingleObject(later, 60), DWORD{WAIT_TIMEOUT});
    (void)SetEvent(fixture.attach.get());
    fixture.done(later);
    expect("later launch does not initialize another window", read(fixture.reports->child[1].ready), 0L);
    expect("later launch activates primary", read(fixture.reports->child[1].role), 2L);
    expect("activation restores minimized primary", read(fixture.reports->child[0].iconic), 0L);
    expect("activation shows primary", read(fixture.reports->child[0].visible), 1L);
    (void)SetEvent(fixture.finish.get());
    fixture.done(primary);
}

void refuses_unready_and_hung_owner()
{
    for (const auto mode : {L"unready", L"hung"}) {
        Fixture fixture;
        const auto primary = fixture.launch(0, mode);
        (void)SetEvent(fixture.start.get());
        (void)SetEvent(fixture.attach.get());
        await([&] {
            return std::wstring_view(mode) == L"unready" ? read(fixture.reports->child[0].role) == 1 :
                read(fixture.reports->child[0].ready) == 1;
        });
        const auto later = fixture.launch(1, L"short");
        fixture.done(later, 3);
        expect("failed activation does not become primary", read(fixture.reports->child[1].role), -1L);
        expect("failed activation does not create a window", read(fixture.reports->child[1].ready), 0L);
        (void)SetEvent(fixture.finish.get());
        fixture.done(primary);
    }
}

void recovers_after_abandoned_owner()
{
    Fixture fixture;
    const auto primary = fixture.launch(0, L"unready");
    (void)SetEvent(fixture.start.get());
    await([&] { return read(fixture.reports->child[0].role) == 1; });
    const auto later = fixture.launch(1, L"window");
    expect("new launch cannot acquire live owner", WaitForSingleObject(later, 60), DWORD{WAIT_TIMEOUT});
    expect_true("terminate simulated crashed owner", TerminateProcess(primary, 9) != FALSE);
    fixture.done(primary, 9);
    (void)SetEvent(fixture.attach.get());
    await([&] { return read(fixture.reports->child[1].ready) == 1; });
    expect("abandoned mutex permits replacement", read(fixture.reports->child[1].role), 1L);
    (void)SetEvent(fixture.finish.get());
    fixture.done(later);
}

void waits_for_closing_owner()
{
    Fixture fixture;
    const auto primary = fixture.launch(0, L"closing");
    (void)SetEvent(fixture.start.get());
    (void)SetEvent(fixture.attach.get());
    await([&] { return read(fixture.reports->child[0].ready) == 2; });
    const auto later = fixture.launch(1, L"window");
    expect("closing owner retains exclusive ownership", WaitForSingleObject(later, 60), DWORD{WAIT_TIMEOUT});
    (void)SetEvent(fixture.finish.get());
    fixture.done(primary);
    fixture.done(later);
    expect("replacement initializes only after close releases ownership", read(fixture.reports->child[1].role), 1L);
}

void handles_close_during_activation()
{
    Fixture fixture;
    const auto primary = fixture.launch(0, L"close-during-activation");
    (void)SetEvent(fixture.start.get());
    (void)SetEvent(fixture.attach.get());
    await([&] { return read(fixture.reports->child[0].ready) == 1; });
    const auto later = fixture.launch(1, L"window");
    await([&] { return read(fixture.reports->child[0].ready) == 2; });
    expect("activation losing its target still waits for teardown", WaitForSingleObject(later, 60), DWORD{WAIT_TIMEOUT});
    expect("close race never initializes before ownership release", read(fixture.reports->child[1].role), 0L);
    (void)SetEvent(fixture.finish.get());
    fixture.done(primary);
    fixture.done(later);
    expect("close race acquires only after teardown", read(fixture.reports->child[1].role), 1L);
}

} // namespace

int wavplay_test::run_single_instance_child(std::span<wchar_t* const> operands)
{
    if (operands.size() != 3) { return 2; }
    const std::wstring_view scope(operands[0]), mode(operands[2]);
    const auto index = std::stoul(operands[1]);
    if (index >= child_count) { return 2; }
    UniqueHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, report_name(scope, L".reports").c_str()));
    if (!mapping) { return 2; }
    std::unique_ptr<Reports, UnmapReports> reports(static_cast<Reports*>(
        MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Reports))));
    if (!reports) { return 2; }
    auto& report = reports->child[index];
    try {
        const auto event = [&](std::wstring_view suffix) {
            UniqueHandle value(OpenEventW(SYNCHRONIZE, FALSE, report_name(scope, suffix).c_str()));
            if (!value) { wavplay::core::throw_last_error("OpenEventW(instance child)"); }
            return value;
        };
        const auto start = event(L".start"), attach = event(L".attach"), finish = event(L".finish");
        if (WaitForSingleObject(start.get(), static_cast<DWORD>(hang_budget(10s).count())) != WAIT_OBJECT_0) { return 2; }
        SingleInstance instance(scope);
        if (!instance.acquire_or_activate(mode == L"short" ? 150ms : hang_budget(5s))) {
            write(report.role, 2);
            return 0;
        }
        write(report.role, 1);
        if (mode == L"unready") {
            return WaitForSingleObject(finish.get(), static_cast<DWORD>(hang_budget(10s).count())) == WAIT_OBJECT_0 ? 0 : 2;
        }
        if (WaitForSingleObject(attach.get(), static_cast<DWORD>(hang_budget(10s).count())) != WAIT_OBJECT_0) { return 2; }
        ChildWindow window{instance, report};
        if (mode == L"close-during-activation") { window.closing_gate = finish.get(); }
        window.create(mode == L"minimized");
        if (mode == L"closing") {
            instance.detach();
            write(report.ready, 2);
        }
        if (mode == L"hung") {
            return WaitForSingleObject(finish.get(), static_cast<DWORD>(hang_budget(10s).count())) == WAIT_OBJECT_0 ? 0 : 2;
        }
        pump_until(finish.get());
        return 0;
    } catch (const std::exception&) {
        write(report.role, -1);
        return 3;
    }
}

int wavplay_test::run_single_instance_tests()
{
    const UiDesktopGuard desktop;
    TestSuite suite;
    suite.run_timed("simultaneous launches initialize one window", simultaneous_launches);
    suite.run_timed("later launch waits for readiness and restores the existing window", waits_for_window_and_restores);
    suite.run_timed("unready and hung owners never permit duplicate initialization", refuses_unready_and_hung_owner);
    suite.run_timed("a crashed owner can be replaced without stale readiness", recovers_after_abandoned_owner);
    suite.run_timed("closing owner remains exclusive until teardown finishes", waits_for_closing_owner);
    suite.run_timed("window closes during an activation handshake", handles_close_during_activation);
    return suite.result("Single instance");
}
