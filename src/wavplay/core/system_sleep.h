#pragma once

#include "wavplay/core/win32.h"

namespace wavplay::core {

// Prevents automatic system sleep for one active operation, without requesting
// that the display stay on or changing the caller's thread execution state.
// Owns an independent Windows power request; explicit user sleep remains allowed.
class SystemSleepGuard {
public:
    SystemSleepGuard();
    ~SystemSleepGuard() noexcept;
    SystemSleepGuard(const SystemSleepGuard&) = delete;
    SystemSleepGuard& operator=(const SystemSleepGuard&) = delete;

private:
    UniqueHandle request_;
};

} // namespace wavplay::core
