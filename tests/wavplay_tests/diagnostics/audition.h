#pragma once

#include "wavplay/asio/driver_registry.h"
#include "wavplay/asio/device_settings.h"

#include <chrono>

// Real-driver listening commands (--audition-asio*). Each opens the driver
// through `activate`, plays the given WAVs through the product path and reports
// what was played; the diagnostics suite drives them with mock drivers.
void audition_asio_controller(const wchar_t* clsid, const wchar_t* path, const wchar_t* scenario,
    wavplay::asio::ActivateDriver activate);
void audition_asio(const wchar_t* clsid, const wchar_t* first_path, const wchar_t* second_path,
    bool transition, wavplay::asio::ActivateDriver activate, wavplay::asio::DeviceSettings settings = {});
void audition_asio_pause(const wchar_t* clsid, const wchar_t* path, std::chrono::milliseconds pause_after,
    wavplay::asio::ActivateDriver activate);
void audition_asio_stop(const wchar_t* clsid, const wchar_t* path, std::chrono::milliseconds stop_after,
    bool paused_first, wavplay::asio::ActivateDriver activate);
