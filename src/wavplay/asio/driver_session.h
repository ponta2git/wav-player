#pragma once

#include "wavplay/asio/asio_sdk.h"
#include "wavplay/asio/device_settings.h"
#include "wavplay/asio/driver_registry.h"

#include <chrono>
#include <memory>
#include <optional>

namespace wavplay::asio {

class DriverLease;

// One initialized driver and its COM STA. Construct, use and destroy on the same
// control thread; the owner window must stay valid until the session ends. Clock
// changes made through a lease are restored before the driver is released.
// Destroying the session while a lease plays on it (for example from a window
// procedure dispatched inside a playback wait) violates this contract: the driver
// still has live buffers and callbacks, so the destructor writes one line to
// stderr (through Win32, also to the debugger) and calls std::terminate()
// instead of releasing it.
class DriverSession {
public:
    DriverSession(const CLSID& clsid, HWND owner, ActivateDriver activate = activate_registered_driver,
        DeviceSettings settings = {});
    ~DriverSession() noexcept;
    DriverSession(const DriverSession&) = delete;
    DriverSession& operator=(const DriverSession&) = delete;

    // The instance changes across a lease's reopen(). Throws once closed.
    [[nodiscard]] IASIO& driver() const;
    // During playback, a reentrant close throws and retains the driver.
    // Once idle, restore the original clock and release even if restoration fails.
    void close();
    // Idle only. Restore/release the old instance if present, then initialize
    // the same device with the original owner and settings. Also opens a closed
    // session, allowing notification generations to retire between close/open.
    [[nodiscard]] IASIO& reopen();

private:
    friend class DriverLease;

    struct Apartment {
        Apartment();
        ~Apartment();
        Apartment(const Apartment&) = delete; // One CoUninitialize per successful CoInitializeEx.
        Apartment& operator=(const Apartment&) = delete;
    };
    struct ReleaseDriver {
        void operator()(IASIO* driver) const noexcept;
    };

    void open();
    void restore_and_release();
    // The one reopen sequence: restore the original clock before releasing the
    // old instance, then initialize the same CLSID with the same owner. A failed
    // initialization leaves the device at its original clock, without a driver.
    [[nodiscard]] IASIO& reinstantiate();

    Apartment apartment_;
    const CLSID clsid_;
    const HWND owner_;
    const ActivateDriver activate_;
    const DeviceSettings settings_;
    bool playback_active_{};
    std::unique_ptr<IASIO, ReleaseDriver> driver_;
    std::optional<ASIOSampleRate> original_rate_;
};

// Exclusive use of a session's driver for one playback request. The lease records
// clock changes for restoration and may reopen the driver. The STA may dispatch
// owner-window messages inside driver calls and playback waits.
class DriverLease {
public:
    explicit DriverLease(DriverSession& session);
    ~DriverLease();
    DriverLease(const DriverLease&) = delete;
    DriverLease& operator=(const DriverLease&) = delete;

    // The driver instance changes across reopen(). Fetch it again afterward.
    [[nodiscard]] IASIO& driver() const { return session_.driver(); }
    [[nodiscard]] ASIOError set_sample_rate(ASIOSampleRate requested, ASIOSampleRate previous);
    [[nodiscard]] std::chrono::milliseconds start_silence() const noexcept { return session_.settings_.start_silence; }
    [[nodiscard]] long buffer_frames() const noexcept { return session_.settings_.buffer_frames; }
    [[nodiscard]] long output_pair() const noexcept { return session_.settings_.output_pair; }
    // Restore the original clock, release the idle driver, then initialize the
    // same CLSID with the same owner window. Returns the new instance; references
    // to the old one are dangling. Throws, without a driver, on failure.
    [[nodiscard]] IASIO& reopen();

private:
    DriverSession& session_;
};

} // namespace wavplay::asio
