#pragma once

#include <windows.h>

#include <chrono>
#include <span>

namespace wavplay::core {

// Bound on messages dispatched per batch, so neither commands nor window
// traffic can starve the other on a control STA.
inline constexpr unsigned message_batch_limit = 32;

// Dispatch at most message_batch_limit queued messages of the calling thread.
// Returns false after reposting WM_QUIT for the outer owner to consume.
[[nodiscard]] bool pump_messages() noexcept;

// True when the calling thread has a WM_QUIT queued (for example reposted by
// pump_messages), without removing it. A posted WM_QUIT counts wherever it is
// queued; the quit flag PostQuitMessage sets counts only once no other posted
// message remains, the point where Windows reports it.
[[nodiscard]] bool quit_pending() noexcept;

// How a message wait ended.
enum class WaitResult {
    signaled,  // The wait consumed one event's signal before the deadline.
    timed_out, // The deadline has passed; pending signals stay set for the next wait.
    quit,      // An STA wait met a WM_QUIT; it stays queued for the outer loop.
    failed,    // The wait itself failed; GetLastError describes the failure.
};

// A wait's result and, for signaled, the index of the event whose signal it
// consumed. The index is 0 for every other result.
struct EventWait {
    WaitResult result;
    unsigned index{};
};

// Wait for any of up to MAXIMUM_WAIT_OBJECTS - 1 auto-reset events until a
// monotonic deadline that early wakes never extend. When several events are
// set, the lowest index wins and only its signal is consumed. On an STA, every
// wake also dispatches a bounded message batch, because driver windows and COM
// calls can depend on that thread. Returns timed_out once the deadline has
// passed, even if events are also pending. A WM_QUIT is reposted for the outer
// loop (quit_pending()) and reported as quit, and never costs a signal: if the
// same wake consumed an event, that event is set again before quit is
// returned. A quit queued before the deadline is a shutdown request: on an STA,
// an expired wait with quit_pending() returns quit, not timed_out, even when
// the batch limit kept the WM_QUIT behind other messages. No events, or too
// many, fail with ERROR_INVALID_PARAMETER.
[[nodiscard]] EventWait wait_for_events_until(std::span<const HANDLE> events,
    std::chrono::steady_clock::time_point deadline) noexcept;

// wait_for_events_until for one auto-reset event. Out of line, so a caller
// forwarding its own event stays a tail jump.
[[nodiscard]] WaitResult wait_for_event_until(HANDLE event, std::chrono::steady_clock::time_point deadline) noexcept;

// A caller's extra wake source for a wait that another layer owns: after each
// wake by `event` (auto-reset), the waiting thread calls poll(context). Lets a
// lower layer run a higher layer's control poll without knowing its types. A
// hook without an event or a poll adds nothing to the wait.
struct WakeHook {
    HANDLE event{};
    void (*poll)(void*) noexcept {};
    void* context{};
};

} // namespace wavplay::core
