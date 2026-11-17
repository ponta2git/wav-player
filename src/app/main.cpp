#include "wavplay/core/memory.h"
#include "wavplay/core/thread_hints.h"
#include "wavplay/ui/error_message.h"
#include "wavplay/ui/player_window.h"
#include "wavplay/ui/single_instance.h"

#include <windows.h>

#include <cstdlib>
#include <exception>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t usage[] =
        L"wavplay.exe\n"
        L"Optional single file: --wav \"C:\\Music\\track.wav\"\n\n"
        L"Set [General] SelectedDevice={ASIO-driver-CLSID} in settings.ini beside wavplay.exe.\n"
        L"Put one local absolute WAV path per line in UTF-8 playlist.txt beside wavplay.exe.\n"
        L"Starts stopped. Double-click or Enter plays the selected track.\n"
        L"Space: Play / Pause    Escape: Stop\n"
        L"Close the window to stop and release the device.";

void show_failure(const std::exception_ptr& error, bool include_usage) noexcept
{
    try {
        auto message = wavplay::ui::exception_message(error);
        if (include_usage) {
            message += L"\n\n";
            message += usage;
        }
        MessageBoxW(nullptr, message.c_str(), L"WAV Player", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"Cannot start WAV Player.", L"WAV Player", MB_OK | MB_ICONERROR);
    }
}

} // namespace

int run_application(int show_command)
{
    // The process's main thread is the UI thread. Named here, not in
    // run_player_window, so that tests running it on their own threads keep their names.
    wavplay::core::name_current_thread(L"WAV Player UI");
    wavplay::ui::PlayerOptions options;
    try {
        // The CRT's startup has already split the command line into __wargv
        // with the documented Microsoft C++ rules, the same parser that gives
        // wmain its argv. Element 0 is the program name.
        const std::span<wchar_t* const> values(__wargv, static_cast<std::size_t>(__argc));
        const std::vector<std::wstring_view> arguments(values.begin() + (values.empty() ? 0 : 1), values.end());
        if (arguments.size() == 1 && (arguments[0] == L"--help" || arguments[0] == L"/?")) {
            MessageBoxW(nullptr, usage, L"WAV Player", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        options = wavplay::ui::parse_player_options(arguments);
    } catch (const std::invalid_argument&) {
        // Only an argument error explains the command line.
        show_failure(std::current_exception(), true);
        return 1;
    } catch (...) {
        show_failure(std::current_exception(), false);
        return 1;
    }
    try {
        wavplay::ui::SingleInstance instance;
        if (!instance.acquire_or_activate()) { return 0; }
        // Only the instance that plays locks its image, before the first driver
        // open. A refusal leaves the image pageable (core::process_image_locked).
        (void)wavplay::core::lock_process_image();
        options.settings_path = wavplay::ui::executable_settings_path();
        options.playlist_path = wavplay::ui::executable_playlist_path();
        return wavplay::ui::run_player_window(options, show_command,
            wavplay::asio::activate_registered_driver, {}, &instance);
    } catch (...) {
        show_failure(std::current_exception(), false);
    }
    return 1;
}
