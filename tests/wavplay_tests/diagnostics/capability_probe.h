#pragma once

#include "wavplay/asio/driver_queries.h"

#include <array>
#include <memory>
#include <type_traits>

// --probe-asio: open the registered driver twice in one process and print what
// the stopped driver reports, without setting a rate or creating buffers.

// A hidden owner window for driver sessions opened by the diagnostic commands.
using AsioProbeWindow = std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>;
AsioProbeWindow make_asio_probe_window();

void print_output_types(const std::array<ASIOSampleType, 2>& types);
void print_capabilities(const wavplay::asio::DriverCapabilities& caps);
void probe_asio_driver(const wchar_t* clsid);
