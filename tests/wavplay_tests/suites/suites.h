#pragma once

#include <array>
#include <span>
#include <string_view>

namespace wavplay_test {

// Each entry point runs every case of one suites/<layer>/<name>_tests.cpp,
// reports each failure on stderr, prints "<title> tests passed." only when
// none failed, and returns 0 on success or 1 on any failure.
int run_planar_conversion_tests();
int run_memory_tests();
int run_cpu_features_tests();
// Internal child process entry point: the process-lifetime image lock
// (core::lock_process_image), which a suite process never takes itself.
int run_image_lock_child(std::span<wchar_t* const> operands);
int run_system_sleep_tests();
int run_message_wait_tests();
int run_wav_header_tests();
int run_wav_reader_tests();
int run_device_config_tests();
int run_playlist_tests();
int run_sample_types_tests();
int run_driver_session_tests();
// Internal child process entry point: a driver session destroyed while a lease
// plays through it, which ends the process.
int run_leased_session_child(std::span<wchar_t* const> operands);
int run_output_format_tests();
int run_ring_layout_tests();
int run_prefetch_ring_tests();
int run_playback_signal_tests();
int run_segment_tests();
// Internal child process entry point: a mock playback cycle after the
// process-lifetime image lock, which a suite process never takes itself.
int run_locked_segment_child(std::span<wchar_t* const> operands);
int run_track_player_tests();
int run_track_source_formats_tests();
int run_track_output_types_tests();
int run_rate_transition_tests();
int run_session_playback_tests();
int run_playback_controller_tests();
// Internal child process entry point: the controller's progress at the default
// timer resolution, which a suite process raises to 1 ms (main.cpp).
int run_default_timer_child(std::span<wchar_t* const> operands);
int run_player_window_tests();
// Internal child process entry point: the product's argument parsing applied to
// this process's CRT wide argv, the parser that also fills the product's __wargv.
int run_player_options_child(std::span<wchar_t* const> operands);
int run_single_instance_tests();
// Internal child process entry point for the isolated cross-process UI tests.
int run_single_instance_child(std::span<wchar_t* const> operands);
int run_diagnostics_tests();

struct Suite {
    std::wstring_view name; // The suite's file name without "_tests.cpp", for --suite.
    int (*run)();
    // Dominated by real-time playback (seconds or more); the supervisor starts
    // these first so that a parallel run is not left waiting on one of them.
    bool long_running{};
};

// The default run order. Lower layers run first only so that a failure is
// attributed to the lowest broken layer: session playback follows the
// single-segment and rate transition suites it builds on, the source format and
// output type suites follow the single-track transport they reuse
// (track_player), and the diagnostic CLI commands run last. sample_types (asio)
// runs with the audio vocabulary, because the media and stream suites name
// their outputs through its asio::to_sample_format mapping. No suite may rely
// on another having run; each must also pass alone with --suite.
// scripts/check-project-layout.ps1 requires exactly one entry per
// suites/<layer>/<name>_tests.cpp.
inline constexpr std::array suites{
    Suite{L"cpu_features", run_cpu_features_tests},
    Suite{L"memory", run_memory_tests},
    Suite{L"system_sleep", run_system_sleep_tests},
    Suite{L"message_wait", run_message_wait_tests},
    Suite{L"planar_conversion", run_planar_conversion_tests},
    Suite{L"sample_types", run_sample_types_tests},
    Suite{L"wav_header", run_wav_header_tests},
    Suite{L"wav_reader", run_wav_reader_tests},
    Suite{L"device_config", run_device_config_tests},
    Suite{L"playlist", run_playlist_tests},
    Suite{L"ring_layout", run_ring_layout_tests},
    Suite{L"prefetch_ring", run_prefetch_ring_tests, true},
    Suite{L"driver_session", run_driver_session_tests},
    Suite{L"output_format", run_output_format_tests},
    Suite{L"playback_signal", run_playback_signal_tests},
    Suite{L"segment", run_segment_tests, true},
    Suite{L"track_player", run_track_player_tests, true},
    Suite{L"track_source_formats", run_track_source_formats_tests, true},
    Suite{L"track_output_types", run_track_output_types_tests, true},
    Suite{L"rate_transition", run_rate_transition_tests},
    Suite{L"session_playback", run_session_playback_tests},
    Suite{L"playback_controller", run_playback_controller_tests, true},
    Suite{L"player_window", run_player_window_tests},
    Suite{L"single_instance", run_single_instance_tests},
    Suite{L"diagnostics", run_diagnostics_tests, true},
};

} // namespace wavplay_test
