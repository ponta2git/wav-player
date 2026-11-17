#pragma once

#include "wavplay/asio/driver_registry.h"

#include <span>
#include <string>
#include <string_view>

namespace wavplay::ui {

struct PlayerOptions {
    CLSID driver{}; // Internal test/diagnostic injection only; the product reads SelectedDevice from INI.
    std::wstring path;
    std::wstring settings_path; // Empty only for callers which inject settings directly.
    std::wstring playlist_path; // Used when path is empty; product resolves it beside the executable.
};

// Arguments exclude the executable. No arguments load playlist.txt; --wav keeps
// the single-file diagnostic entry point. The device is configured in settings.ini.
[[nodiscard]] PlayerOptions parse_player_options(std::span<const std::wstring_view> arguments);

// Resolve configuration beside the executable, independently of the working directory.
[[nodiscard]] std::wstring executable_settings_path();
[[nodiscard]] std::wstring executable_playlist_path();

} // namespace wavplay::ui
