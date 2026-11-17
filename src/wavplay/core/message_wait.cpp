#include "wavplay/core/message_wait.h"

#include <objbase.h>

#include <algorithm>

namespace wavplay::core {

bool pump_messages() noexcept
{
    MSG message{};
    for (unsigned handled = 0; handled < message_batch_limit && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
         ++handled) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

bool quit_pending() noexcept
{
    MSG message{};
    return PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) != FALSE;
}

namespace {

bool in_single_threaded_apartment() noexcept
{
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
        (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
}

} // namespace

EventWait wait_for_events_until(std::span<const HANDLE> events, std::chrono::steady_clock::time_point deadline) noexcept
{
    // MsgWaitForMultipleObjectsEx keeps one of the MAXIMUM_WAIT_OBJECTS slots for input.
    if (events.empty() || events.size() >= MAXIMUM_WAIT_OBJECTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return {WaitResult::failed};
    }
    const auto count = static_cast<DWORD>(events.size());
    const bool pump = in_single_threaded_apartment();
    // Recheck one monotonic deadline after every early wake.
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            // A WM_QUIT queued before the deadline is a shutdown request, even
            // when the batch limit postponed it behind other messages.
            if (pump && quit_pending()) {
                return {WaitResult::quit};
            }
            // A stream of unrelated signals must not extend an expired wait.
            // Callers inspect their own state separately after every wake.
            return {WaitResult::timed_out};
        }
        // INFINITE must never be requested: a far deadline is re-checked after each bounded wait.
        const auto ms = static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(remaining.count(), INFINITE - 1));
        // Both waits report the lowest signaled index and consume only that event.
        const auto wake = pump ? MsgWaitForMultipleObjectsEx(count, events.data(), ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                               : WaitForMultipleObjects(count, events.data(), FALSE, ms);
        if (wake == WAIT_TIMEOUT) {
            continue;
        }
        const auto index = static_cast<unsigned>(wake - WAIT_OBJECT_0);
        const bool signaled = index < count;
        if (!signaled && !(pump && index == count)) {
            // WAIT_FAILED, with its error still in GetLastError. (An event,
            // unlike a mutex, is never abandoned.)
            return {WaitResult::failed};
        }
        if (!pump) {
            return {WaitResult::signaled, index};
        }
        // Whichever object MsgWait reports, dispatch a bounded batch, so that
        // repeated event wakes cannot starve the STA.
        if (!pump_messages()) {
            if (signaled) {
                // This wake already reset that auto-reset event. Restore the signal
                // so that reporting the quit cannot lose it; every caller rechecks
                // its own state after a wake, so a repeated signal is harmless.
                (void)SetEvent(events[index]);
            }
            return {WaitResult::quit};
        }
        if (signaled) {
            return {WaitResult::signaled, index};
        }
    }
}

WaitResult wait_for_event_until(HANDLE event, std::chrono::steady_clock::time_point deadline) noexcept
{
    return wait_for_events_until({&event, 1}, deadline).result;
}

} // namespace wavplay::core
