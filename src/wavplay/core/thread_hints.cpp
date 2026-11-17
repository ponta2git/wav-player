#include "wavplay/core/thread_hints.h"

#include <windows.h>

namespace wavplay::core {

bool set_current_thread_speed(ExecutionSpeed speed) noexcept
{
    // system clears the control bit, so Windows manages the thread again;
    // high controls it without the throttled state, eco with it.
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = speed == ExecutionSpeed::system ? 0 : THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = speed == ExecutionSpeed::eco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    return SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof state) != FALSE;
}

void name_current_thread(const wchar_t* name) noexcept
{
    (void)SetThreadDescription(GetCurrentThread(), name);
}

} // namespace wavplay::core
