#include "wavplay/core/system_sleep.h"

namespace wavplay::core {

SystemSleepGuard::SystemSleepGuard()
{
    wchar_t reason[] = L"WAV Player is playing audio";
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason;
    const auto request = PowerCreateRequest(&context);
    if (request == INVALID_HANDLE_VALUE) {
        throw_last_error("PowerCreateRequest");
    }
    request_.reset(request);
    if (!PowerSetRequest(request_.get(), PowerRequestSystemRequired)) {
        // Member destruction closes the request even if activation fails.
        throw_last_error("PowerSetRequest");
    }
}

SystemSleepGuard::~SystemSleepGuard() noexcept
{
    // The last handle is also closed if clearing fails. No shared thread state
    // or another caller's independently owned request can be cleared here.
    (void)PowerClearRequest(request_.get(), PowerRequestSystemRequired);
}

} // namespace wavplay::core
