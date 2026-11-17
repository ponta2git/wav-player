#pragma once

#include <windows.h>

#include <chrono>
#include <memory>
#include <string_view>

namespace wavplay::ui {

// One product window per user and interactive Windows session, including
// copies installed in different directories. Own on the UI thread until the
// controller and window have both closed. Tests use an isolated nonempty scope.
class SingleInstance {
public:
    explicit SingleInstance(std::wstring_view test_scope = {});
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // true: this caller owns the instance and may initialize the player.
    // false: the existing window acknowledged the activation; exit without
    // loading settings, opening files or initializing another driver.
    // A failed/hung existing instance raises an error instead of starting two.
    [[nodiscard]] bool acquire_or_activate(
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    void attach(HWND window);
    void detach() noexcept;
    // Call from the attached window's procedure. Return 1 to acknowledge a
    // handled request. No pointers or file/command-line data cross processes.
    [[nodiscard]] bool handle_activation(UINT message) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wavplay::ui
