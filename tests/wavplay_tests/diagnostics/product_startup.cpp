#include "diagnostics/product_startup.h"
#include "fixtures/ui_desktop_guard.h"
#include "fixtures/wav_builder.h"
#include "framework/test_suite.h"
#include "wavplay/core/win32.h"
#include "wavplay/settings/device_config.h"
#include "wavplay/ui/player_controls.h"
#include "wavplay/ui/player_window.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace wavplay_test;
using wavplay::core::throw_last_error;
using wavplay::core::throw_win32_error;
using wavplay::core::UniqueHandle;
using namespace wavplay::ui;

struct StartupDirectory {
    const fs::path path;

    explicit StartupDirectory(const wchar_t* reserved_name) : path(std::wstring(reserved_name) + L" startup")
    {
        if (!fs::create_directory(path)) {
            throw std::runtime_error("The product startup test needs a new scratch directory");
        }
    }
    ~StartupDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    StartupDirectory(const StartupDirectory&) = delete;
    StartupDirectory& operator=(const StartupDirectory&) = delete;
};

void write_text(const fs::path& path, std::string_view contents)
{
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << contents;
    file.close();
}

// The product runs only after it is in the job. Closing the job kills an
// unresponsive product, including when the harness itself exits unexpectedly.
class ProductProcess {
public:
    ProductProcess(const fs::path& executable, const fs::path& cwd)
        : job_(CreateJobObjectW(nullptr, nullptr))
    {
        if (!job_) { throw_last_error("CreateJobObjectW(product startup)"); }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            throw_last_error("SetInformationJobObject(product startup)");
        }
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION created{};
        std::wstring command = L'"' + executable.wstring() + L'"';
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                nullptr, cwd.c_str(), &startup, &created)) {
            throw_last_error("CreateProcessW(product startup)");
        }
        process_.reset(created.hProcess);
        const UniqueHandle thread(created.hThread);
        id_ = created.dwProcessId;
        if (!AssignProcessToJobObject(job_.get(), process_.get()) ||
            ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            const auto error = GetLastError();
            (void)TerminateProcess(process_.get(), 1);
            (void)WaitForSingleObject(process_.get(), 5000);
            throw_win32_error(error, "Starting the product test process");
        }
    }
    ~ProductProcess()
    {
        job_.reset();
        (void)WaitForSingleObject(process_.get(), 5000); // Release the staged image before removing the directory.
    }
    ProductProcess(const ProductProcess&) = delete;
    ProductProcess& operator=(const ProductProcess&) = delete;

    HANDLE handle() const noexcept { return process_.get(); }
    DWORD id() const noexcept { return id_; }

private:
    UniqueHandle job_, process_;
    DWORD id_{};
};

HWND product_window(DWORD pid)
{
    HWND window{};
    while ((window = FindWindowExW(nullptr, window, player_window_class, nullptr)) != nullptr) {
        DWORD owner{};
        if (GetWindowThreadProcessId(window, &owner) && owner == pid) { return window; }
    }
    return nullptr;
}

std::wstring control_text(HWND child)
{
    // WM_GETTEXT is marshalled by Windows for these standard controls across
    // processes. A hung GUI fails within the deadline instead of hanging CI.
    std::array<wchar_t, 512> text{};
    DWORD_PTR copied{};
    if (!SendMessageTimeoutW(child, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 500, &copied)) {
        throw_last_error("SendMessageTimeoutW(product text)");
    }
    if (copied >= text.size()) { throw std::runtime_error("Invalid product control text length"); }
    return std::wstring(text.data(), static_cast<std::size_t>(copied));
}

void check_window_and_close(const ProductProcess& product)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        if (const auto window = product_window(product.id())) {
            const auto device = GetDlgItem(window, control_id::device);
            const auto error = GetDlgItem(window, control_id::error);
            const auto play = GetDlgItem(window, control_id::play_pause);
            if (device && error && play && control_text(device) == L"Device: NO DEVICE" &&
                control_text(error).contains(L"SelectedDevice")) {
                expect("Play is disabled without a selected device", IsWindowEnabled(play), FALSE);
                expect_true("the launched product still owns its window", product_window(product.id()) == window);
                if (!PostMessageW(window, WM_CLOSE, 0, 0)) { throw_last_error("PostMessageW(product close)"); }
                break;
            }
        }
        expect("product stays alive until its device guidance appears",
            WaitForSingleObject(product.handle(), 20), DWORD{WAIT_TIMEOUT});
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("The product did not show its missing-device guidance within the startup deadline");
        }
    }
    expect("product exits after Close", WaitForSingleObject(product.handle(), 10000), DWORD{WAIT_OBJECT_0});
    DWORD code{};
    if (!GetExitCodeProcess(product.handle(), &code)) { throw_last_error("GetExitCodeProcess(product)"); }
    expect("unconfigured product exit code", code, 1);
}

void test_first_launch(const wchar_t* source)
{
    const UiDesktopGuard desktop;
    // The real single-instance owner is per user and session, including copies
    // installed elsewhere. An explicit local run belongs in a dedicated session.
    if (FindWindowExW(nullptr, nullptr, player_window_class, nullptr)) {
        fail("The product startup test requires a session without a running player");
    }
    TempFile reservation;
    const StartupDirectory directory(reservation.write({}));
    const auto app = directory.path / L"player";
    fs::create_directory(app);
    const auto executable = app / L"wavplay.exe";
    fs::copy_file(source, executable);
    write_text(app / L"playlist.txt", "");
    // The working directory's settings would fail parsing before device setup.
    // Leaving its playlist absent tests the playlist's executable-relative path.
    constexpr std::string_view sentinel = "[Window]\nWidth=0\n";
    const auto cwd_settings = directory.path / L"settings.ini";
    write_text(cwd_settings, sentinel);
    {
        const ProductProcess product(executable, directory.path);
        check_window_and_close(product);
    }
    const auto saved = wavplay::settings::load_application_config((app / L"settings.ini").c_str());
    expect_true("first close saves window placement with no selected device", saved.window && !saved.selected_device);
    {
        std::ifstream input(cwd_settings, std::ios::binary);
        const std::string contents(std::istreambuf_iterator<char>{input}, {});
        expect_true("working-directory settings stay untouched", contents == sentinel);
    }
    fs::remove_all(directory.path); // A successful run must also have released the staged files.
}

} // namespace

int wavplay_test::run_product_startup_test(const wchar_t* executable)
{
    TestSuite suite;
    suite.run("Release product starts without a selected device and saves settings on Close",
        [&] { test_first_launch(executable); });
    return suite.result("Release product startup");
}
