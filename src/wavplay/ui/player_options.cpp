#include "wavplay/ui/player_options.h"
#include "wavplay/core/win32.h"

#include <algorithm>
#include <stdexcept>

namespace wavplay::ui {
namespace {

std::wstring executable_file_path(std::wstring_view name)
{
    std::wstring path(260, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) { core::throw_last_error("GetModuleFileNameW"); }
        if (length < path.size()) {
            path.resize(length);
            const auto separator = path.find_last_of(L"\\/");
            if (separator == std::wstring::npos) { throw std::runtime_error("Executable path has no directory"); }
            path.resize(separator + 1);
            path += name;
            return path;
        }
        if (path.size() >= 32768) { throw std::runtime_error("Executable path exceeds the Windows path limit"); }
        path.resize(std::min(path.size() * 2, std::size_t{32768}));
    }
}

} // namespace

PlayerOptions parse_player_options(std::span<const std::wstring_view> arguments)
{
    if (arguments.empty()) { return {}; }
    if (arguments.size() != 2 || arguments[0] != L"--wav" || arguments[1].empty()) {
        throw std::invalid_argument("Expected no arguments or --wav \"path.wav\"; set General.SelectedDevice in settings.ini");
    }
    return {.path = std::wstring(arguments[1])};
}

std::wstring executable_settings_path() { return executable_file_path(L"settings.ini"); }
std::wstring executable_playlist_path() { return executable_file_path(L"playlist.txt"); }

} // namespace wavplay::ui
