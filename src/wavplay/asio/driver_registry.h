#pragma once

#include <guiddef.h>

#include <functional>

// The SDK declares IASIO with `interface`, which is `struct`; callers that use the
// driver include asio_sdk.h. This header stays free of the SDK for GUI code.
struct IASIO;

namespace wavplay::asio {

// Parses a braced driver CLSID such as settings.ini's SelectedDevice value.
[[nodiscard]] CLSID parse_driver_clsid(const wchar_t* text);

// Called in the session's STA, again on every reopen. Returns one owned
// reference or throws. A session keeps its own copy, so injected activation
// bindings travel with the session instead of a global slot.
using ActivateDriver = std::function<IASIO*(const CLSID&)>;

// Instantiates a driver listed in the x64 ASIO registration (HKLM\SOFTWARE\ASIO),
// as the SDK's host/pc/asiolist.cpp does. Only the configured CLSID is opened;
// this does not choose a fallback or provide a GUI device list.
[[nodiscard]] IASIO* activate_registered_driver(const CLSID& clsid);

} // namespace wavplay::asio
