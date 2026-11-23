#include "wavplay/ui/single_instance.h"
#include "wavplay/core/win32.h"

#include <sddl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace wavplay::ui {
namespace {

using core::UniqueHandle;
using Clock = std::chrono::steady_clock;

std::wstring object_name(std::wstring_view scope)
{
    if (scope.size() > 100 || scope.find_first_of(L"\\/") != std::wstring_view::npos) {
        throw std::invalid_argument("Invalid single-instance test scope");
    }
    // A pseudo-handle with TOKEN_QUERY access that needs no closing.
    const auto token = GetCurrentProcessToken();
    DWORD size{};
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) { core::throw_last_error("GetTokenInformation(single instance size)"); }
    std::vector<std::byte> storage(size);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        core::throw_last_error("GetTokenInformation(single instance user)");
    }
    PWSTR raw_sid{};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) {
        core::throw_last_error("ConvertSidToStringSidW(single instance)");
    }
    const core::LocalPtr<wchar_t> sid(raw_sid);
    std::wstring name = L"Local\\WAVPlayer.SingleInstance.v1.";
    name += sid.get();
    if (!scope.empty()) {
        name += L".test.";
        name += scope;
    }
    return name;
}

// All accesses cross process boundaries through Win32 interlocked operations.
// The mapping is page aligned. No CRT objects or process-local pointers live
// here; HWND is only a numeric handle and is checked against its published PID.
struct SharedWindow {
    LONG64 window{};
    LONG process{};
    LONG64 generation{};
};
// Every installed copy of the product maps this layout under the same object
// names. A change to it needs a new version in the names (".v1." above), or
// two versions would read each other's fields.
static_assert(sizeof(SharedWindow) == 24 && offsetof(SharedWindow, window) == 0 &&
    offsetof(SharedWindow, process) == 8 && offsetof(SharedWindow, generation) == 16);

struct UnmapView {
    void operator()(SharedWindow* view) const noexcept { (void)UnmapViewOfFile(view); }
};

DWORD remaining_ms(Clock::time_point deadline)
{
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) { throw std::runtime_error("The existing WAV Player did not become ready in time"); }
    return static_cast<DWORD>(remaining);
}

} // namespace

struct SingleInstance::Impl {
    UniqueHandle mutex, ready, mapping;
    std::unique_ptr<SharedWindow, UnmapView> shared;
    HWND window{};
    UINT message{};
    DWORD owner_thread{};
    bool owns{};

    explicit Impl(std::wstring_view scope)
    {
        const auto name = object_name(scope);
        mutex.reset(CreateMutexExW(nullptr, (name + L".owner").c_str(), 0,
            SYNCHRONIZE | MUTEX_MODIFY_STATE));
        if (!mutex) { core::throw_last_error("CreateMutexExW(single instance)"); }
        ready.reset(CreateEventExW(nullptr, (name + L".ready").c_str(), CREATE_EVENT_MANUAL_RESET,
            SYNCHRONIZE | EVENT_MODIFY_STATE));
        if (!ready) { core::throw_last_error("CreateEventExW(single instance)"); }
        mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(SharedWindow), (name + L".window").c_str()));
        if (!mapping) { core::throw_last_error("CreateFileMappingW(single instance)"); }
        shared.reset(static_cast<SharedWindow*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            0, 0, sizeof(SharedWindow))));
        if (!shared) { core::throw_last_error("MapViewOfFile(single instance)"); }
        message = RegisterWindowMessageW((name + L".activate").c_str());
        if (!message) { core::throw_last_error("RegisterWindowMessageW(single instance)"); }
    }

    ~Impl()
    {
        if (owns) {
            detach();
            (void)ReleaseMutex(mutex.get());
        }
    }

    void claim()
    {
        owns = true;
        owner_thread = GetCurrentThreadId();
        if (!ResetEvent(ready.get())) { core::throw_last_error("ResetEvent(single instance)"); }
        InterlockedIncrement64(&shared->generation);
        InterlockedExchange64(&shared->window, 0);
        InterlockedExchange(&shared->process, static_cast<LONG>(GetCurrentProcessId()));
    }

    bool publication_changed(LONG64 generation) const noexcept
    {
        return InterlockedCompareExchange64(&shared->generation, 0, 0) != generation ||
            WaitForSingleObject(ready.get(), 0) == WAIT_TIMEOUT;
    }

    bool try_claim()
    {
        const auto result = WaitForSingleObject(mutex.get(), 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            claim();
            return true;
        }
        if (result != WAIT_TIMEOUT) { core::throw_last_error("WaitForSingleObject(single instance)"); }
        return false;
    }

    void detach() noexcept
    {
        if (!owns) { return; }
        (void)ResetEvent(ready.get());
        InterlockedExchange64(&shared->window, 0);
        window = nullptr;
    }
};

SingleInstance::SingleInstance(std::wstring_view test_scope) : impl_(std::make_unique<Impl>(test_scope)) {}
SingleInstance::~SingleInstance() = default;

bool SingleInstance::acquire_or_activate(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0 || timeout.count() >= std::numeric_limits<DWORD>::max()) {
        throw std::invalid_argument("Invalid single-instance wait timeout");
    }
    if (impl_->owns || impl_->try_claim()) { return true; }
    const auto deadline = Clock::now() + timeout;
    const HANDLE handles[]{impl_->mutex.get(), impl_->ready.get()};
    for (;;) {
        const auto result = WaitForMultipleObjects(2, handles, FALSE, remaining_ms(deadline));
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED_0) {
            impl_->claim();
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            throw std::runtime_error("The existing WAV Player is still starting or closing");
        }
        if (result != WAIT_OBJECT_0 + 1) { core::throw_last_error("WaitForMultipleObjects(single instance)"); }

        const auto generation = InterlockedCompareExchange64(&impl_->shared->generation, 0, 0);
        const auto process = static_cast<DWORD>(InterlockedCompareExchange(&impl_->shared->process, 0, 0));
        const auto target = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(
            InterlockedCompareExchange64(&impl_->shared->window, 0, 0)));
        DWORD actual_process{};
        const auto thread = GetWindowThreadProcessId(target, &actual_process);
        // The previous owner may have exited after signalling readiness. Take
        // the mutex only after release/abandonment. If another launcher won it,
        // wait for that generation instead of mistaking stale HWND data for it.
        if (impl_->try_claim()) { return true; }
        if (impl_->publication_changed(generation)) { continue; }
        if (!target || !thread || !process || actual_process != process) {
            throw std::runtime_error("The existing WAV Player window is unavailable");
        }
        // Transfer only this launch's foreground permission, never ASFW_ANY or
        // synthetic input. Windows may refuse it during unrelated user input.
        (void)AllowSetForegroundWindow(process);
        DWORD_PTR reply{};
        SetLastError(ERROR_SUCCESS);
        const auto sent = SendMessageTimeoutW(target, impl_->message, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, remaining_ms(deadline), &reply);
        if (sent && reply == 1) { return false; }
        const auto error = GetLastError();
        if (impl_->try_claim()) { return true; }
        if (impl_->publication_changed(generation)) { continue; }
        if (error) { core::throw_win32_error(error, "Activate existing WAV Player"); }
        throw std::runtime_error("The existing WAV Player did not acknowledge activation");
    }
}

void SingleInstance::attach(HWND window)
{
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (!impl_->owns || impl_->owner_thread != GetCurrentThreadId() || !window ||
        thread != GetCurrentThreadId() || process != GetCurrentProcessId()) {
        throw std::logic_error("Single-instance window must belong to the owning UI thread");
    }
    impl_->window = window;
    InterlockedExchange64(&impl_->shared->window, static_cast<LONG64>(reinterpret_cast<std::uintptr_t>(window)));
    // A waiter may have awakened on the previous owner's ready signal before
    // claim reset it. Publish a new epoch after the HWND so an observation made
    // while this owner was still preparing cannot look like a ready snapshot.
    InterlockedIncrement64(&impl_->shared->generation);
    if (!SetEvent(impl_->ready.get())) { core::throw_last_error("SetEvent(single instance ready)"); }
}

void SingleInstance::detach() noexcept { impl_->detach(); }

bool SingleInstance::handle_activation(UINT message) noexcept
{
    if (message != impl_->message || !impl_->owns || !impl_->window) { return false; }
    if (IsIconic(impl_->window)) {
        (void)ShowWindow(impl_->window, SW_RESTORE);
    } else if (!IsWindowVisible(impl_->window)) {
        (void)ShowWindow(impl_->window, SW_SHOW);
    }
    if (!SetForegroundWindow(impl_->window)) {
        FLASHWINFO flash{sizeof(flash), impl_->window, FLASHW_TRAY, 3, 0};
        (void)FlashWindowEx(&flash);
    }
    return true;
}

} // namespace wavplay::ui
