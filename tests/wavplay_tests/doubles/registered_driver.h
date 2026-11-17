#pragma once

#include "doubles/mock_asio.h"
#include "framework/test_suite.h"
#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/playback/segment.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Registered-driver doubles that also check registration, init, references and
// the STA of asio::DriverSession or a diagnostic CLI command.
namespace wavplay_test {

// Models a driver listed in the x64 ASIO registration: only the selected CLSID
// activates, the factory grants one reference, and a leaked reference prevents
// the next activation. Every recorded driver call requires an initialized driver
// with a live reference (check_entry), whatever on_call a test installs.
// Sessions open it through activation().
class RegisteredDriver : public MockAsio {
public:
    unsigned attempts{}, activations{}, inits{}, releases{}, references{};
    bool fail_init{}, initialized{}, bad_lifetime{}, wrong_channel{};
    void* host{};

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        bad_lifetime |= !in_sta() || !references;
        ++releases;
        initialized = false;
        return --references;
    }
    ASIOBool init(void* window) override
    {
        ++inits;
        bad_lifetime |= !in_sta() || references != 1;
        host = window;
        initialized = !fail_init;
        return initialized ? ASIOTrue : ASIOFalse;
    }
    void getErrorMessage(char* destination) override
    {
        constexpr char message[] = "mock device is busy";
        std::copy_n(message, sizeof(message), destination);
    }

    ASIOError getChannelInfo(ASIOChannelInfo* info) override
    {
        const auto result = MockAsio::getChannelInfo(info);
        if (result == ASE_OK && (info->channel < first_output || info->channel > first_output + 1)) {
            info->type = ASIOSTInt24MSB; // Remaining outputs must not masquerade as output 2.
        }
        if (wrong_channel) {
            info->channel = 0; // The driver returned output 1 again for output 2.
        }
        return result;
    }

    void check_probe_closed() const
    {
        check_closed();
        for (const auto call : calls) {
            using enum Call;
            if (call != rate && call != channels && call != channel && call != size) {
                throw std::runtime_error("Unexpected ASIO operation during probe: " +
                    std::to_string(static_cast<int>(call)));
            }
        }
    }

    void check_closed() const
    {
        expect("remaining driver references", references, 0);
        expect("Release calls", releases, activations);
        expect("driver used outside its lifetime/STA", static_cast<int>(bad_lifetime), 0);
        // A leaked STA prevents opening an MTA after the session.
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
        expect("COM initialization balance after release", result, S_OK);
    }

protected:
    void check_entry(Call) override { bad_lifetime |= !initialized || !references; }

    IASIO* activated(const CLSID& clsid) override
    {
        ++attempts;
        if (!IsEqualCLSID(clsid, selected)) {
            throw std::runtime_error("Selected CLSID is absent");
        }
        if (references) {
            throw std::runtime_error("Selected driver is still open");
        }
        ++activations;
        AddRef();
        return this;
    }
};

inline wavplay::asio::DriverCapabilities probe(RegisteredDriver& driver, const CLSID& clsid = selected)
{
    wavplay::asio::DriverSession session(clsid, GetDesktopWindow(), driver.activation());
    return wavplay::asio::query_capabilities(session.driver());
}

// Models a driver that, after any buffer allocation in one init, handles a
// rate change by keeping the previous rate's width until the driver is reopened.
class LifecycleDriver : public RegisteredDriver {
public:
    unsigned ticks_per_start = 32;
    std::vector<std::size_t> opened_at; // Index into calls at each init.

    LifecycleDriver()
    {
        left_type = right_type = next_left_type = next_right_type = ASIOSTInt24LSB;
        fit_width();
    }
    ASIOBool init(void* window) override
    {
        opened_at.push_back(calls.size());
        buffers_since_init_ = false;
        fit_width();
        return RegisteredDriver::init(window);
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
    {
        const auto result = MockAsio::createBuffers(infos, count, size, callbacks);
        buffers_since_init_ |= result == ASE_OK;
        return result;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override
    {
        const auto stale = buffer_size;
        const auto result = MockAsio::setSampleRate(rate);
        if (result == ASE_OK) {
            if (buffers_since_init_) {
                buffer_size = stale;
                latency = stale.preferred;
            } else {
                fit_width();
            }
        }
        return result;
    }
    ASIOError start() override
    {
        started = {}; // MockAsio::start resolves one promise per start.
        const auto result = MockAsio::start();
        if (result == ASE_OK) {
            ticker_ = std::jthread([this](std::stop_token stop) {
                // Playback publishes its return from start(). The probe does not, so allow it 30 ms.
                for (unsigned waited = 0; !wavplay::playback::is_streaming() && waited < 30; ++waited) {
                    Sleep(1);
                }
                for (unsigned i = 0; i < ticks_per_start && !stop.stop_requested(); ++i) {
                    tick();
                    Sleep(1);
                }
            });
        }
        return result;
    }
    ASIOError stop() override
    {
        if (ticker_.joinable()) {
            ticker_.request_stop();
            ticker_.join();
        }
        return MockAsio::stop();
    }

    // Allocation, streaming and clock calls made while the indexed init was open.
    std::vector<Call> lifecycle_calls(std::size_t open) const
    {
        using enum Call;
        const auto end = open + 1 < opened_at.size() ? calls.begin() + opened_at[open + 1] : calls.end();
        std::vector<Call> kept;
        std::copy_if(calls.begin() + opened_at.at(open), end, std::back_inserter(kept), [](Call call) {
            return call == create || call == start || call == stop || call == dispose || call == can_rate || call == set_rate;
        });
        return kept;
    }

private:
    bool buffers_since_init_{};
    std::jthread ticker_;

    void fit_width()
    {
        const long frames = sample_rate == 44100 ? 512 : 544;
        buffer_size = {frames, frames, frames, 0};
        latency = frames;
    }
};

// Thread-timer callback that posts WM_QUIT (exit code 37) to interrupt an owner-thread wait.
inline void CALLBACK request_quit(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    PostQuitMessage(37);
}

} // namespace wavplay_test
