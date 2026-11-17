#include "diagnostics/capability_probe.h"

#include "wavplay/asio/driver_registry.h"
#include "wavplay/asio/driver_session.h"
#include "wavplay/asio/sample_types.h"

#include <cstdio>
#include <system_error>

AsioProbeWindow make_asio_probe_window()
{
    AsioProbeWindow window(CreateWindowExW(0, L"STATIC", L"WAV Player", WS_OVERLAPPEDWINDOW,
        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr), &DestroyWindow);
    if (!window) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateWindowExW");
    }
    return window;
}

void print_output_types(const std::array<ASIOSampleType, 2>& types)
{
    for (std::size_t index = 0; index < types.size(); ++index) {
        const auto type = types[index];
        std::printf("  Output %zu: %s (%ld)\n", index + 1, wavplay::asio::sample_type_name(type), type);
    }
}

void print_capabilities(const wavplay::asio::DriverCapabilities& caps)
{
    std::printf("  Current sample rate: %.15g Hz\n  Output channels: %ld\n",
        caps.sample_rate, caps.output_channels);
    print_output_types(caps.output_types);
    const auto& size = caps.buffer_size;
    std::printf("  Buffer size (frames): min=%ld max=%ld preferred=%ld granularity=%ld\n",
        size.minimum, size.maximum, size.preferred, size.granularity);
}

void probe_asio_driver(const wchar_t* clsid_text)
{
    const auto clsid = wavplay::asio::parse_driver_clsid(clsid_text);
    const auto window = make_asio_probe_window();

    for (int pass = 1; pass <= 2; ++pass) {
        std::printf("Probe %d/2: %ls\n", pass, clsid_text);
        std::fflush(stdout);
        wavplay::asio::DriverSession session(clsid, window.get());
        print_capabilities(wavplay::asio::query_capabilities(session.driver()));
    }
    std::puts("Same driver reopened and released.");
}
