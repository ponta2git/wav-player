#include "wavplay/ui/error_message.h"
#include "wavplay/core/win32.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace wavplay::ui {
namespace {

// The application's own texts are UTF-8. Text that is not, such as a narrow
// message localized in the ANSI code page, is decoded with CP_ACP instead.
std::wstring decode_message(std::string_view text)
{
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return L"Application error message is too long.";
    }
    if (auto utf16 = core::utf8_to_wide(text)) {
        return std::move(*utf16);
    }
    const auto bytes = static_cast<int>(text.size());
    const auto length = MultiByteToWideChar(CP_ACP, 0, text.data(), bytes, nullptr, 0);
    if (!length) {
        return L"Cannot decode the application error message.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, text.data(), bytes, result.data(), length)) {
        return L"Cannot decode the application error message.";
    }
    return result;
}

std::wstring system_message(const std::system_error& error)
{
    wchar_t* buffer{};
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(error.code().value()), 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const core::LocalPtr<wchar_t> owner(buffer);
    if (!length) {
        return decode_message(error.what());
    }
    std::wstring message(buffer, length);
    const auto last = message.find_last_not_of(L"\r\n \t");
    message.erase(last == std::wstring::npos ? 0 : last + 1);

    // Keep the operation supplied to system_error, but replace its narrow
    // localized suffix instead of trying to recover Japanese from what().
    const std::string_view what(error.what());
    const auto narrow_description = error.code().message();
    if (!narrow_description.empty() && what.ends_with(narrow_description)) {
        return decode_message(what.substr(0, what.size() - narrow_description.size())) + message;
    }
    return message;
}

// One exception's own text, without its nested causes.
std::wstring own_message(const std::exception_ptr& error)
{
    try {
        std::rethrow_exception(error);
    } catch (const std::system_error& detail) {
        return detail.code().category() == std::system_category() ? system_message(detail) : decode_message(detail.what());
    } catch (const std::exception& detail) {
        return decode_message(detail.what());
    } catch (...) {
        return L"Unexpected application failure.";
    }
}

// The cause std::throw_with_nested attached to `error`, or null. Matching the
// handler's type finds it without dynamic_cast (std::rethrow_if_nested's test),
// so the RTTI cast runtime is not linked for this cold path.
std::exception_ptr nested_cause(const std::exception_ptr& error)
{
    try {
        std::rethrow_exception(error);
    } catch (const std::nested_exception& nested) {
        return nested.nested_ptr();
    } catch (...) {
        return nullptr;
    }
}

} // namespace

std::wstring exception_message(const std::exception_ptr& error)
{
    std::wstring message;
    auto current = error;
    for (std::size_t level = 0; current && level < max_exception_levels; ++level) {
        if (level != 0) {
            message += cause_separator;
        }
        message += own_message(current);
        current = nested_cause(current);
    }
    return message;
}

} // namespace wavplay::ui
