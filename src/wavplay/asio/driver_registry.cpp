#include "wavplay/asio/driver_registry.h"

#include "wavplay/asio/asio_sdk.h"
#include "wavplay/core/win32.h"

#include <objbase.h>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace wavplay::asio {
namespace {

struct CloseKey {
    void operator()(HKEY key) const noexcept { RegCloseKey(key); }
};

bool try_parse_clsid(const wchar_t* text, CLSID& clsid)
{
    const std::wstring_view value(text ? text : L"");
    return value.size() == 38 && value.front() == L'{' && value.back() == L'}' &&
        SUCCEEDED(IIDFromString(value.data(), &clsid));
}

void require_registration(const CLSID& clsid)
{
    HKEY key{};
    auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
        KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        throw std::runtime_error("Selected CLSID not found in x64 ASIO registration");
    }
    if (status != ERROR_SUCCESS) {
        core::throw_win32_error(status, "Cannot open x64 ASIO registration");
    }
    const std::unique_ptr<std::remove_pointer_t<HKEY>, CloseKey> registrations(key);
    LSTATUS read_error = ERROR_SUCCESS;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{}; // Registry subkey names are at most 255 characters.
        DWORD length = ARRAYSIZE(name);
        status = RegEnumKeyExW(registrations.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            core::throw_win32_error(status, "Cannot enumerate x64 ASIO registration");
        }
        wchar_t value[39]{}; // Braced GUID and terminator.
        DWORD bytes = sizeof(value);
        status = RegGetValueW(registrations.get(), name, L"CLSID", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            nullptr, value, &bytes);
        if (status == ERROR_SUCCESS) {
            CLSID registered{};
            if (bytes == sizeof(value) && try_parse_clsid(value, registered) && IsEqualCLSID(registered, clsid)) {
                return;
            }
        } else if (status != ERROR_FILE_NOT_FOUND && status != ERROR_MORE_DATA && status != ERROR_UNSUPPORTED_TYPE) {
            // An unreadable unrelated key must not hide a later exact match.
            read_error = status;
        }
    }
    if (read_error != ERROR_SUCCESS) {
        core::throw_win32_error(read_error, "Cannot read x64 ASIO registration");
    }
    throw std::runtime_error("Selected CLSID not found in x64 ASIO registration");
}

} // namespace

CLSID parse_driver_clsid(const wchar_t* text)
{
    CLSID clsid{};
    if (!try_parse_clsid(text, clsid)) {
        throw std::invalid_argument("Expected a braced ASIO driver CLSID");
    }
    return clsid;
}

IASIO* activate_registered_driver(const CLSID& clsid)
{
    require_registration(clsid);
    void* driver{};
    // The ASIO SDK uses the driver's own CLSID as its interface ID.
    const auto result = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &driver);
    if (FAILED(result)) {
        core::throw_win32_error(static_cast<DWORD>(result), "Cannot activate selected ASIO driver");
    }
    return static_cast<IASIO*>(driver);
}

} // namespace wavplay::asio
