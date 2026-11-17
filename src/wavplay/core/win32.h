#pragma once

#include <windows.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace wavplay::core {

[[noreturn]] inline void throw_win32_error(DWORD code, const char* operation)
{
    throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

// Call immediately after the failed API, before anything can overwrite the thread's last error.
[[noreturn]] inline void throw_last_error(const char* operation)
{
    throw_win32_error(GetLastError(), operation);
}

struct CloseHandleDeleter {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

// Owns a kernel handle. Wrap only valid handles: APIs returning INVALID_HANDLE_VALUE
// on failure must be checked before construction.
using UniqueHandle = std::unique_ptr<void, CloseHandleDeleter>;

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

// Owns memory that a Win32 API returned from its local heap for the caller to
// LocalFree, such as FormatMessageW's allocated buffer or a string SID.
template <class T>
using LocalPtr = std::unique_ptr<T, LocalFreeDeleter>;

// Strict UTF-8 to UTF-16 (MB_ERR_INVALID_CHARS): no value for invalid UTF-8
// or text beyond INT_MAX bytes. A BOM decodes to U+FEFF and NUL to L'\0';
// callers strip or refuse them and report invalid text with their own error.
// Only an allocation failure throws.
[[nodiscard]] inline std::optional<std::wstring> utf8_to_wide(std::string_view utf8)
{
    if (utf8.empty()) {
        return std::wstring();
    }
    if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const auto bytes = static_cast<int>(utf8.size());
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, nullptr, 0);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, wide.data(), length) != length) {
        return std::nullopt;
    }
    return wide;
}

// An unnamed auto-reset event, initially nonsignaled.
[[nodiscard]] inline UniqueHandle create_event()
{
    UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) {
        throw_last_error("CreateEventW");
    }
    return event;
}

// For its lifetime, the calling thread fails critical errors such as a removable
// or optical drive without media (ERROR_NOT_READY) instead of blocking in the
// system's modal hard-error dialog. Wrap only the application's own file access:
// the process-wide SetErrorMode would also reach the in-process ASIO driver.
// Nested scopes restore each previous mode in reverse order.
class CriticalErrorScope {
public:
    CriticalErrorScope() noexcept : previous_(GetThreadErrorMode())
    {
        // Cannot fail: the thread's current mode plus one valid flag.
        (void)SetThreadErrorMode(previous_ | SEM_FAILCRITICALERRORS, nullptr);
    }
    ~CriticalErrorScope() { (void)SetThreadErrorMode(previous_, nullptr); }
    CriticalErrorScope(const CriticalErrorScope&) = delete;
    CriticalErrorScope& operator=(const CriticalErrorScope&) = delete;

private:
    DWORD previous_;
};

} // namespace wavplay::core
