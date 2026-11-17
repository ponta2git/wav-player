#include "wavplay/asio/driver_session.h"

#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/clock.h"
#include "wavplay/core/win32.h"

#include <objbase.h>

#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace wavplay::asio {
namespace {

// A failed inquiry cannot confirm the clock. Keep the original set error and
// avoid floating-point comparisons on a driver's untrusted (possibly signaling NaN) result.
bool reports_rate(IASIO& driver, ASIOSampleRate expected) noexcept
{
    ASIOSampleRate actual{};
    try {
        return driver.getSampleRate(&actual) == ASE_OK && same_clock(actual, expected);
    } catch (...) {
        return false;
    }
}

// Teardown diagnostics without CRT stdio. Writes the bytes CRT text-mode stderr
// wrote (the text, then CR LF) when a standard error handle exists; a GUI process
// normally has none, so the debugger output receives the same line as well. Each
// destination gets two writes (text, then CR LF) to avoid copying an unbounded
// text, so another writer on a shared pipe can land between them.
void report(const char* text) noexcept
{
    const auto error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) {
        DWORD written{};
        (void)WriteFile(error, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        (void)WriteFile(error, "\r\n", 2, &written, nullptr);
    }
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
}

} // namespace

DriverSession::Apartment::Apartment()
{
    // No OLE 1.0 DDE on this STA. An existing STA, with or without the flag,
    // still returns S_FALSE; only an MTA caller is refused.
    const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(result)) {
        core::throw_win32_error(static_cast<DWORD>(result), "Cannot initialize ASIO control thread as COM STA");
    }
}

DriverSession::Apartment::~Apartment()
{
    CoUninitialize();
}

void DriverSession::ReleaseDriver::operator()(IASIO* driver) const noexcept
{
    driver->Release();
}

DriverSession::DriverSession(const CLSID& clsid, HWND owner, ActivateDriver activate, DeviceSettings settings)
    : clsid_(clsid), owner_(owner), activate_(activate), settings_(settings)
{
    if (!owner || !activate) {
        throw std::invalid_argument("ASIO init requires an owner window and driver factory");
    }
    settings_.validate();
    open();
}

void DriverSession::open()
{
    std::unique_ptr<IASIO, ReleaseDriver> driver(activate_(clsid_));
    if (!driver) {
        throw std::runtime_error("ASIO activation returned no driver");
    }
    if (!driver->init(owner_)) {
        char message[sizeof(ASIODriverInfo::errorMessage) + 1]{};
        driver->getErrorMessage(message);
        throw std::runtime_error(*message ? std::string("ASIO init failed: ") + message : "ASIO init failed");
    }
    driver_ = std::move(driver);
}

DriverSession::~DriverSession() noexcept
{
    if (playback_active_) {
        // Releasing the driver and the STA under live buffers would leave the lease
        // writing through a destroyed session. Fail fast instead.
        report("ASIO driver session destroyed during playback");
        std::terminate();
    }
    try {
        close();
    } catch (const std::exception& error) {
        report(error.what());
    } catch (...) {
        report("Failed to restore original ASIO sample rate");
    }
}

IASIO& DriverSession::driver() const
{
    if (!driver_) {
        throw std::logic_error("ASIO driver session is closed");
    }
    return *driver_;
}

void DriverSession::close()
{
    if (playback_active_) {
        throw std::logic_error("Cannot close ASIO driver session during playback");
    }
    restore_and_release();
}

IASIO& DriverSession::reopen()
{
    if (playback_active_) {
        throw std::logic_error("Cannot reopen ASIO driver session during playback");
    }
    return reinstantiate();
}

IASIO& DriverSession::reinstantiate()
{
    restore_and_release();
    open();
    return *driver_;
}

void DriverSession::restore_and_release()
{
    auto driver = std::move(driver_); // Release on every exit, while the STA is still alive.
    const auto original = std::exchange(original_rate_, std::nullopt); // This instance's restoration obligation.
    if (!driver || !original || reports_rate(*driver, *original)) {
        return;
    }
    throw_if_error(driver->setSampleRate(*original), "Failed to restore original ASIO sample rate: setSampleRate failed");
    if (!reports_rate(*driver, *original)) {
        throw std::runtime_error("Failed to restore original ASIO sample rate: clock not confirmed");
    }
}

DriverLease::DriverLease(DriverSession& session) : session_(session)
{
    (void)session.driver(); // A closed session has nothing to play on.
    if (session.playback_active_) {
        throw std::runtime_error("ASIO playback already active");
    }
    session.playback_active_ = true;
}

DriverLease::~DriverLease()
{
    session_.playback_active_ = false;
}

ASIOError DriverLease::set_sample_rate(ASIOSampleRate requested, ASIOSampleRate previous)
{
    auto& driver = this->driver();
    auto& original = session_.original_rate_;
    // Even a failed hardware clock change can leave the device at another rate.
    const bool first_attempt = !original;
    if (first_attempt) {
        original = previous;
    }
    const auto result = driver.setSampleRate(requested);
    if (first_attempt && result != ASE_OK && reports_rate(driver, previous)) {
        original.reset(); // A confirmed refusal does not own later external changes.
    }
    return result;
}

IASIO& DriverLease::reopen()
{
    return session_.reinstantiate(); // The lease owns playback, so it bypasses the idle check.
}

} // namespace wavplay::asio
