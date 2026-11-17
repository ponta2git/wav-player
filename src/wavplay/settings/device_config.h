#pragma once

#include "wavplay/asio/device_settings.h"
#include "wavplay/asio/driver_registry.h"

#include <cstddef>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace wavplay::settings {

// Configuration is small control data, never an unbounded playback allocation.
inline constexpr std::size_t max_config_bytes = 1024 * 1024;

// Physical screen coordinates of the normal (restored) window rectangle.
// The view clamps this rectangle to the current monitors and its minimum size.
struct WindowPlacement {
    int x{}, y{}, width{}, height{};
    bool maximized{};
};

// Empty optionals preserve the caller's defaults. Other devices and unknown
// keys are ignored; a duplicate, malformed or overflowing known value is refused.
struct DeviceConfig {
    std::optional<long> buffer_frames;
    std::optional<std::chrono::milliseconds> start_silence;
    std::optional<long> output_pair; // First channel, one-based and odd.
    std::wstring name; // Optional user-provided display label, not a driver identity.

    // `defaults` with each configured value in place of the default. The
    // parser has checked the keys' ranges; callers still validate() the result.
    [[nodiscard]] asio::DeviceSettings apply(asio::DeviceSettings defaults) const;
};

struct ApplicationConfig {
    std::optional<CLSID> selected_device;
    DeviceConfig device;
    std::optional<WindowPlacement> window;
};

// Strict UTF-8 (optional BOM), ASCII case-insensitive INI section/key names.
// An explicit override is for injected drivers; normal startup uses General.SelectedDevice.
[[nodiscard]] ApplicationConfig parse_application_config(std::string_view utf8,
    std::optional<CLSID> selected_override = {});
[[nodiscard]] ApplicationConfig load_application_config(const wchar_t* path,
    std::optional<CLSID> selected_override = {});

// Called on the controller worker at shutdown. Re-read the latest file, replace
// only Window values, and retain manual settings, comments, BOM and unknown keys.
// A flushed sibling temporary file is committed only after a complete write.
// Missing files start with an empty SelectedDevice. Errors leave the source intact.
void save_window_config(const wchar_t* path, const WindowPlacement& window);

} // namespace wavplay::settings
