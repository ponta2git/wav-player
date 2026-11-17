#pragma once

#include "wavplay/asio/driver_queries.h"
#include "wavplay/asio/driver_registry.h"

#include <array>
#include <optional>
#include <vector>

// Silent diagnosis of how a driver's capabilities depend on the buffer
// lifecycle that preceded a rate change within one init. Every scenario
// opens the device afresh and writes only zero samples.
enum class LifecycleScenario { fresh, created, started, started_then_wait };
inline constexpr std::array lifecycle_scenarios{LifecycleScenario::fresh, LifecycleScenario::created,
    LifecycleScenario::started, LifecycleScenario::started_then_wait};

struct LifecycleProbeOptions {
    unsigned switches = 128; // bufferSwitch calls per silent run.
    DWORD boundary_wait_ms = 1000; // started_then_wait: dispose to setSampleRate, pumping messages.
    DWORD switch_timeout_ms = 5000;
};

// One createBuffers/disposeBuffers cycle, optionally started with silence.
struct SilentRun {
    double sample_rate; // getSampleRate after createBuffers.
    long width; // createBuffers size.
    long output_latency; // getLatencies after createBuffers.
    std::array<ASIOSampleType, 2> output_types;
    bool started;
    bool timed_out;
    unsigned switches; // Total bufferSwitch calls before stop returned.
    unsigned during_start; // Calls that arrived before start() returned.
    unsigned steady_intervals; // Intervals averaged below. Zero if too few switches.
    double mean_interval_ms, min_interval_ms, max_interval_ms;
    ASIOError position_error; // First getSamplePosition failure, or ASE_OK.
    std::optional<double> position_frames_per_switch; // All steady positions must succeed and strictly advance.
};

struct DriverEvent {
    double ms; // Since the scenario's driver was opened.
    bool rate_change; // sampleRateDidChange instead of asioMessage.
    long selector, value;
    double rate;
    long reply;
};

struct LifecycleReport {
    LifecycleScenario scenario;
    wavplay::asio::DriverCapabilities before;
    std::optional<SilentRun> original; // Buffers at the original rate, for all but fresh.
    wavplay::asio::DriverCapabilities after_set; // Immediately after setSampleRate, as playback would query it.
    SilentRun target; // Started at the width reported in after_set.
    wavplay::asio::DriverCapabilities after_target; // After the target-rate buffers were disposed.
    std::vector<DriverEvent> events;

    bool timed_out() const noexcept { return target.timed_out || (original && original->timed_out); }
};

const char* lifecycle_scenario_name(LifecycleScenario scenario);

// Parses the command-line target rate in Hz.
unsigned parse_probe_rate(const wchar_t* text);
// A positive explicit width; unlike a saved setting, this diagnostic has no
// implicit/default width argument.
long parse_probe_buffer_frames(const wchar_t* text);

struct BufferProbeReport {
    long requested_frames;
    wavplay::asio::DriverCapabilities before;
    SilentRun run;
    wavplay::asio::DriverCapabilities after;
    std::vector<DriverEvent> events;
};

// Creates the exact requested width at the current sample rate, streams only
// silence, then stops/disposes/releases. QPC/getSamplePosition instrumentation
// measures acceptance and cadence; it is not a product CPU-load comparison.
BufferProbeReport probe_asio_buffer(const wchar_t* clsid, long buffer_frames,
    wavplay::asio::ActivateDriver activate, const LifecycleProbeOptions& options = {});

// Prints each scenario as it completes and returns all of them. Every
// scenario releases the driver after restoring its original rate.
std::vector<LifecycleReport> probe_asio_rate_lifecycle(const wchar_t* clsid, unsigned target_rate,
    wavplay::asio::ActivateDriver activate, const LifecycleProbeOptions& options = {});
