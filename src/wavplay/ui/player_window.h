#pragma once

#include "wavplay/ui/player_options.h"
#include "wavplay/asio/device_settings.h"
#include "wavplay/core/thread_hints.h"

#include <windows.h>

namespace wavplay::ui {

class SingleInstance;

inline constexpr wchar_t player_window_class[] = L"WavplayPlayerWindow";

// One compile-time policy shared with the scope tests. This hint does not change
// priority, affinity or the control/driver threads.
inline constexpr core::ExecutionSpeed ui_thread_speed = core::ExecutionSpeed::system;

// Runs the UI message loop. Only the controller's STA calls the injected driver.
// A new cleanup failure remains visible until Close is requested again.
// WM_ENDSESSION closes like an external WM_QUIT and returns only after the
// release and the window save, or after about 4 s.
// Call on a dedicated UI thread before creating its first top-level window;
// this read-only view disables that thread's IME for the thread's lifetime.
// From window creation until it returns, the thread requests ui_thread_speed
// (a best-effort hint with priority unchanged), then returns to the system default.
int run_player_window(const PlayerOptions& options, int show_command = SW_SHOWNORMAL,
    asio::ActivateDriver activate = asio::activate_registered_driver, asio::DeviceSettings settings = {},
    SingleInstance* instance = nullptr);

} // namespace wavplay::ui
