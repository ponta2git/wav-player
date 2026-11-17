#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace wavplay::ui {

// The levels exception_message shows: the exception and up to three nested causes.
inline constexpr std::size_t max_exception_levels = 4;
// Starts each cause's line below the text it explains.
inline constexpr std::wstring_view cause_separator = L"\r\nCause: ";

// Localized Win32 errors use FormatMessageW. Other exception messages are UTF-8,
// with an active-code-page fallback for existing driver-provided ANSI text.
// Causes attached by std::throw_with_nested follow on their own lines
// ("Cause: "), outermost first and formatted the same way, up to
// max_exception_levels in all. An empty exception pointer has no message; a
// non-standard exception gets a generic message. Formatting may allocate, so
// callers keep a literal fallback.
[[nodiscard]] std::wstring exception_message(const std::exception_ptr& error);

} // namespace wavplay::ui
