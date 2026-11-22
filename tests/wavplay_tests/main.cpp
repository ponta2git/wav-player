#include "diagnostics/audition.h"
#include "diagnostics/capability_probe.h"
#include "diagnostics/lifecycle_probe.h"
#include "diagnostics/prefetch_measurement.h"
#include "diagnostics/product_startup.h"
#include "framework/failure_reporting.h"
#include "framework/supervisor.h"
#include "suites/suites.h"

#include "wavplay/asio/driver_registry.h"
#include "wavplay/core/memory.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

// Test helpers poll in 1 ms sleeps. At the default 15.6 ms timer resolution
// each such sleep lasts a whole tick, which made polling, not playback, the
// cost of the real-time suites. Since Windows 10 2004 the request applies to
// this process only, and Windows 11 ignores it for a process it considers
// invisible (a console test without windows, a few seconds after start)
// unless the process opts out of that power throttling. Product deadlines are
// monotonic and do not depend on it; the real-device diagnostic commands and
// the --default-timer-child process keep the default.
class TimerResolution {
public:
    TimerResolution() noexcept
    {
        PROCESS_POWER_THROTTLING_STATE keep_resolution{};
        keep_resolution.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        keep_resolution.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        keep_resolution.StateMask = 0; // Always honor this process's timer resolution request.
        (void)SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &keep_resolution,
            sizeof keep_resolution);
        raised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
    ~TimerResolution()
    {
        if (raised_) {
            (void)timeEndPeriod(1);
        }
    }
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;

private:
    bool raised_{};
};

using wavplay::asio::activate_registered_driver;
using wavplay_test::Suite;
using Operands = std::span<wchar_t* const>;

std::size_t parse_wakes_per_ring(std::wstring_view value)
{
    if (value == L"2") { return 2; }
    if (value == L"4") { return 4; }
    if (value == L"8") { return 8; }
    throw std::invalid_argument("Expected 2, 4 or 8 refill wakes per ring");
}

std::chrono::milliseconds parse_control_delay(const wchar_t* text)
{
    const std::wstring_view delay(text);
    if (delay.empty() || delay.find_first_not_of(L"0123456789") != std::wstring_view::npos) {
        throw std::invalid_argument("Playback control delay must be an integer in milliseconds");
    }
    const auto milliseconds = std::stoull(std::wstring(delay));
    if (milliseconds > 60000) {
        throw std::invalid_argument("Playback control delay must not exceed 60000 ms");
    }
    return std::chrono::milliseconds(milliseconds);
}

// A diagnostic command for a real registered driver or the local disk. It
// returns the exit code; an exception prints "<operation> failed: <reason>".
struct Command {
    std::wstring_view flag;
    const char* synopsis; // Operands after the flag, as shown in the usage text.
    const char* operation;
    std::size_t min_operands, max_operands;
    int (*run)(Operands operands);
};

// In usage-text order.
constexpr std::array commands{
    Command{L"--measure-prefetch", " [pcm16|pcm24|pcm32|ext24in32|float32|extfloat32] [ASIOST...]", "Prefetch measurement", 0, 2, [](Operands operands) {
        if (operands.size() == 2 && std::wstring_view(operands[1]).empty()) {
            throw std::invalid_argument("The prefetch output format must not be empty");
        }
        measure_prefetch(operands.empty() ? L"pcm16" : std::wstring_view(operands[0]),
            operands.size() == 2 ? std::wstring_view(operands[1]) : std::wstring_view{});
        return 0;
    }},
    Command{L"--measure-prefetch-stream", " [pcm16|pcm24|pcm32|ext24in32|float32|extfloat32] [ASIOST...] [2|4|8]", "Prefetch stream measurement", 0, 3, [](Operands operands) {
        if (operands.size() >= 2 && std::wstring_view(operands[1]).empty()) {
            throw std::invalid_argument("The prefetch output format must not be empty");
        }
        measure_prefetch_stream(operands.empty() ? L"pcm16" : std::wstring_view(operands[0]),
            operands.size() >= 2 ? std::wstring_view(operands[1]) : std::wstring_view{},
            operands.size() == 3 ? std::optional{parse_wakes_per_ring(operands[2])} : std::nullopt);
        return 0;
    }},
    Command{L"--measure-refill-pattern", " uniform|alternating|paired", "Refill pattern measurement", 1, 1, [](Operands operands) {
        measure_refill_pattern(operands[0]);
        return 0;
    }},
    Command{L"--check-prefetch-pcm", " pcm16|pcm24 2|4|8 [uniform|alternating|paired]", "Prefetch stream PCM check", 2, 3, [](Operands operands) {
        check_prefetch_pcm(operands[0], parse_wakes_per_ring(operands[1]),
            operands.size() == 3 ? std::optional{std::wstring_view(operands[2])} : std::nullopt);
        return 0;
    }},
    Command{L"--probe-asio", " {CLSID}", "ASIO probe", 1, 1, [](Operands operands) {
        probe_asio_driver(operands[0]);
        return 0;
    }},
    Command{L"--probe-asio-buffer", " {CLSID} buffer-frames", "ASIO buffer probe", 2, 2, [](Operands operands) {
        const auto report = probe_asio_buffer(operands[0], parse_probe_buffer_frames(operands[1]), activate_registered_driver);
        if (report.run.timed_out) {
            std::fputs("ASIO buffer probe incomplete: timed out waiting for buffer switches.\n", stderr);
            return 1;
        }
        return 0;
    }},
    Command{L"--probe-asio-rate-lifecycle", " {CLSID} rate-hz", "ASIO rate lifecycle probe", 2, 2, [](Operands operands) {
        const auto reports = probe_asio_rate_lifecycle(operands[0], parse_probe_rate(operands[1]), activate_registered_driver);
        if (std::ranges::any_of(reports, &LifecycleReport::timed_out)) {
            std::fputs("ASIO rate lifecycle probe incomplete: timed out waiting for buffer switches.\n", stderr);
            return 1;
        }
        return 0;
    }},
    Command{L"--audition-asio", " {CLSID} first.wav [second.wav]", "ASIO audition", 2, 3, [](Operands operands) {
        audition_asio(operands[0], operands[1], operands.size() == 3 ? operands[2] : nullptr, false, activate_registered_driver);
        return 0;
    }},
    Command{L"--audition-asio-buffer", " {CLSID} buffer-frames first.wav [second.wav]", "ASIO buffer audition", 3, 4, [](Operands operands) {
        const wavplay::asio::DeviceSettings settings{.buffer_frames = parse_probe_buffer_frames(operands[1])};
        audition_asio(operands[0], operands[2], operands.size() == 4 ? operands[3] : nullptr, false,
            activate_registered_driver, settings);
        return 0;
    }},
    Command{L"--audition-asio-pause", " {CLSID} first.wav pause-ms", "ASIO pause audition", 3, 3, [](Operands operands) {
        audition_asio_pause(operands[0], operands[1], parse_control_delay(operands[2]), activate_registered_driver);
        return 0;
    }},
    Command{L"--audition-asio-stop", " {CLSID} first.wav stop-ms [paused]", "ASIO stop audition", 3, 4, [](Operands operands) {
        if (operands.size() == 4 && std::wstring_view(operands[3]) != L"paused") {
            throw std::invalid_argument("The optional Stop audition mode must be 'paused'");
        }
        audition_asio_stop(operands[0], operands[1], parse_control_delay(operands[2]), operands.size() == 4,
            activate_registered_driver);
        return 0;
    }},
    Command{L"--audition-asio-controller", " {CLSID} file.wav scenario", "ASIO controller audition", 3, 3, [](Operands operands) {
        audition_asio_controller(operands[0], operands[1], operands[2], activate_registered_driver);
        return 0;
    }},
    Command{L"--audition-asio-transition", " {CLSID} first.wav second.wav", "ASIO audition", 3, 3, [](Operands operands) {
        audition_asio(operands[0], operands[1], operands[2], true, activate_registered_driver);
        return 0;
    }},
};

int usage()
{
    std::fputs("Usage: wavplay_tests.exe [--jobs N [--suite name ...] | --list | --in-process | --suite name [--suite name ...] | --test-product-startup wavplay.exe", stderr);
    for (const auto& command : commands) {
        std::fprintf(stderr, " | %.*ls%s", static_cast<int>(command.flag.size()), command.flag.data(), command.synopsis);
    }
    std::fputs("]\n", stderr);
    return 1;
}

int run_command(std::wstring_view flag, Operands operands)
{
    const auto command = std::ranges::find(commands, flag, &Command::flag);
    if (command == commands.end() || operands.size() < command->min_operands || operands.size() > command->max_operands) {
        return usage();
    }
    // The commands run the product's playback in the product's process-wide
    // state: like wWinMain, lock this image once before the first driver open,
    // so the image flag the auditions print means what it means in wavplay.exe.
    // Suites never come here, so their processes stay unlocked.
    (void)wavplay::core::lock_process_image();
    try {
        return command->run(operands);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s failed: %s\n", command->operation, error.what());
        return 1;
    }
}

int list_suites()
{
    for (const auto& suite : wavplay_test::suites) {
        std::printf("%.*ls\n", static_cast<int>(suite.name.size()), suite.name.data());
    }
    return 0;
}

// Resolves "--suite name" pairs, in the order given; nullopt after printing usage.
std::optional<std::vector<const Suite*>> select_suites(Operands arguments)
{
    std::vector<const Suite*> selected;
    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        if (std::wstring_view(arguments[i]) != L"--suite" || i + 1 == arguments.size()) {
            (void)usage();
            return std::nullopt;
        }
        const auto suite = std::ranges::find(wavplay_test::suites, std::wstring_view(arguments[i + 1]), &Suite::name);
        if (suite == wavplay_test::suites.end()) {
            std::fprintf(stderr, "Unknown suite: %ls (see --list)\n", arguments[i + 1]);
            (void)usage();
            return std::nullopt;
        }
        selected.push_back(&*suite);
    }
    return selected;
}

// Runs each selected suite in a process of its own, `jobs` at a time; see supervise_suites.
int supervise(std::span<const Suite* const> selected, unsigned jobs)
{
    std::vector<wavplay_test::SupervisedSuite> supervised;
    for (const auto* suite : selected) {
        supervised.push_back({suite->name, suite->priority});
    }
    return wavplay_test::supervise_suites(supervised, jobs);
}

std::vector<const Suite*> all_suites()
{
    std::vector<const Suite*> all;
    for (const auto& suite : wavplay_test::suites) {
        all.push_back(&suite);
    }
    return all;
}

// "--jobs N [--suite name ...]": 1 to 64 suites at a time, all suites unless named.
int supervise_with_jobs(Operands arguments)
{
    const std::wstring_view count(arguments[1]);
    if (count.empty() || count.size() > 2 || count.find_first_not_of(L"0123456789") != std::wstring_view::npos) {
        return usage();
    }
    const auto jobs = static_cast<unsigned>(std::stoul(std::wstring(count)));
    if (jobs < 1 || jobs > 64) { // MAXIMUM_WAIT_OBJECTS: the supervisor waits on all children at once.
        return usage();
    }
    if (arguments.size() == 2) {
        return supervise(all_suites(), jobs);
    }
    const auto selected = select_suites(arguments.subspan(2));
    return selected ? supervise(*selected, jobs) : 1;
}

// Runs every selected suite in this process, even after a failure; non-zero
// if any failed. A hang or crash ends the remaining suites.
int run_suites(std::span<const Suite* const> selected)
{
    // Only for suites: the diagnostic commands keep the product's timer resolution.
    const TimerResolution timer_resolution;
    int failed = 0;
    for (const auto* suite : selected) {
        wavplay_test::set_current_suite(suite->name);
        failed |= suite->run();
        std::fflush(stdout); // A later suite can end the process without flushing.
    }
    wavplay_test::set_current_suite({});
    return failed;
}

// Accepts only "--suite name" pairs, in the order given.
int run_named_suites(Operands arguments)
{
    // Under the supervisor stdout and stderr share one file: keep their order.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto selected = select_suites(arguments);
    return selected ? run_suites(*selected) : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    wavplay_test::configure_unattended_failures();
    const Operands arguments(argv + 1, static_cast<std::size_t>(argc - 1));
    if (arguments.empty()) {
        return supervise(all_suites(), wavplay_test::default_supervised_jobs());
    }
    const std::wstring_view mode(arguments[0]);
    if (mode == L"--single-instance-child") {
        return wavplay_test::run_single_instance_child(arguments.subspan(1));
    }
    if (mode == L"--player-options-child") {
        return wavplay_test::run_player_options_child(arguments.subspan(1));
    }
    if (mode == L"--image-lock-child") {
        return wavplay_test::run_image_lock_child(arguments.subspan(1));
    }
    if (mode == L"--leased-session-child") {
        return wavplay_test::run_leased_session_child(arguments.subspan(1));
    }
    if (mode == L"--locked-segment-child") {
        return wavplay_test::run_locked_segment_child(arguments.subspan(1));
    }
    if (mode == L"--default-timer-child") {
        return wavplay_test::run_default_timer_child(arguments.subspan(1));
    }
    if (mode == L"--test-product-startup") {
        return arguments.size() == 2 ? wavplay_test::run_product_startup_test(arguments[1]) : usage();
    }
    if (mode == L"--jobs") {
        return arguments.size() >= 2 ? supervise_with_jobs(arguments) : usage();
    }
    if (mode == L"--list") {
        return arguments.size() == 1 ? list_suites() : usage();
    }
    if (mode == L"--in-process") {
        return arguments.size() == 1 ? run_suites(all_suites()) : usage();
    }
    if (mode == L"--suite") {
        return run_named_suites(arguments);
    }
    return run_command(mode, arguments.subspan(1));
}
