#pragma once

#include "wavplay/asio/asio_sdk.h"

#include <string>
#include <string_view>

namespace wavplay::asio {

// The SDK enumerator name of an ASIOError value, or "Unknown".
[[nodiscard]] constexpr std::string_view error_name(ASIOError error) noexcept
{
    switch (error) {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "Unknown";
    }
}

// The operation text as a stable prefix, then the code's name and value, e.g.
// "ASIO start failed: ASE_HWMalfunction (-999)".
[[nodiscard]] std::string error_message(ASIOError error, const char* operation);

// ASIO reports failures as ASIOError codes. Driver API faults that the caller
// cannot recover from become std::runtime_error with error_message's text.
// Within an exception handler, a new failure nests the currently handled
// exception instead of discarding its code, text or type. ASE_OK never throws.
// Runs on the control thread only. COM and registry failures around the driver
// use core::throw_win32_error.
void throw_if_error(ASIOError error, const char* operation);

} // namespace wavplay::asio
