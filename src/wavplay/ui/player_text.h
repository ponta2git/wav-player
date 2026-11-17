#pragma once

#include "wavplay/control/playback_controller.h"

#include <cstdint>
#include <string>

// Presentation text and command availability of the player window. Nothing here
// touches a window, so tests reach every string without creating one. Numbers
// are written with integer arithmetic only: no std::format, locale or
// floating-point text conversion is linked into the GUI.
namespace wavplay::ui {

// Appends value in decimal, zero-padded to min_digits (at most 20, the digits
// of the largest 64-bit value).
void append_uint(std::wstring& text, std::uint64_t value, unsigned min_digits = 1);
void append_signed(std::wstring& text, long value);
// Appends frames / rate in milliseconds with three decimals, rounding the exact
// quotient half to even. A zero rate has no duration and appends nothing.
void append_millis(std::wstring& text, std::uint32_t frames, std::uint32_t rate);

[[nodiscard]] const wchar_t* state_text(control::PlaybackState state) noexcept;
[[nodiscard]] const wchar_t* result_text(playback::PlaybackResult result) noexcept;
// The error field: the controller's exception, else the track refusal's reason
// (both through exception_message), else the result's fixed text. A read error
// keeps its fixed text, which names the failure and the restart it needs, and
// shows the producer's exception below it as a cause.
[[nodiscard]] std::wstring error_text(const control::PlaybackSnapshot& snapshot);
[[nodiscard]] std::wstring buffer_text(const control::PlaybackSnapshot& snapshot);
[[nodiscard]] std::wstring output_text(const control::PlaybackSnapshot& snapshot);
[[nodiscard]] std::wstring device_text(const control::PlaybackSnapshot& snapshot);
[[nodiscard]] std::wstring playlist_label_text(const control::PlaybackSnapshot& snapshot);

// Replace text, retaining its capacity for repeated refreshes. The window uses
// these overloads with one scratch string; the value-returning forms above are
// convenient for independent callers. No snapshot state is cached here.
void buffer_text(std::wstring& text, const control::PlaybackSnapshot& snapshot);
void output_text(std::wstring& text, const control::PlaybackSnapshot& snapshot);
void device_text(std::wstring& text, const control::PlaybackSnapshot& snapshot);
void playlist_label_text(std::wstring& text, const control::PlaybackSnapshot& snapshot);

// Integer seconds from PCM frames. A zero rate means no track timing is known.
[[nodiscard]] std::wstring playback_time_text(std::uint64_t position_frames,
    std::uint64_t frame_count, std::uint32_t sample_rate);
void playback_time_text(std::wstring& text, std::uint64_t position_frames,
    std::uint64_t frame_count, std::uint32_t sample_rate);

// The hint below the transport buttons: the close progress, the submitted but
// unsettled command, or the keyboard shortcuts.
[[nodiscard]] const wchar_t* pending_text(const control::PlaybackSnapshot& snapshot, bool cleanup_error,
    bool close_requested, bool pending_selection, control::PlaybackCommand pending_command) noexcept;

// One policy is shared by button rendering and command acceptance. Controls are
// a view, never the authoritative record of what operations may be submitted.
struct TransportAvailability {
    bool toggle{}, stop{}, select{};
};

[[nodiscard]] TransportAvailability available(const control::PlaybackCommandState& snapshot,
    std::uint64_t stop_receipt) noexcept;

} // namespace wavplay::ui
