#include "wavplay/ui/player_text.h"
#include "wavplay/ui/error_message.h"

#include <objbase.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace wavplay::ui {

using control::PlaybackCommand;
using control::PlaybackSnapshot;
using control::PlaybackState;

namespace {

// Rates reach the GUI only after playback verified the driver clock bit for bit
// against an integral WAV rate (BufferSetup), or as the integral rate it queried
// the range at (BufferCapabilities). Returns that positive integer, or 0 for any
// other value; the text then omits what depends on the rate.
std::uint32_t integral_rate(double rate) noexcept
{
    if (!(rate >= 1.0 && rate <= 4294967295.0)) { return 0; } // Also rejects NaN.
    const auto integral = static_cast<std::uint32_t>(rate);
    return static_cast<double>(integral) == rate ? integral : 0;
}

// m:ss below one hour, h:mm:ss from there on.
void append_duration(std::wstring& text, std::uint64_t seconds)
{
    if (seconds >= 3600) {
        append_uint(text, seconds / 3600);
        text += L':';
        append_uint(text, seconds / 60 % 60, 2);
    } else {
        append_uint(text, seconds / 60);
    }
    text += L':';
    append_uint(text, seconds % 60, 2);
}

} // namespace

void append_uint(std::wstring& text, std::uint64_t value, unsigned min_digits)
{
    wchar_t digits[20]; // 18446744073709551615 has 20 digits.
    std::size_t first = std::size(digits);
    do {
        digits[--first] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    } while (value != 0);
    const auto width = std::min<std::size_t>(min_digits, std::size(digits));
    while (std::size(digits) - first < width) {
        digits[--first] = L'0';
    }
    text.append(digits + first, std::size(digits) - first);
}

void append_signed(std::wstring& text, long value)
{
    if (value < 0) {
        text += L'-';
        // Widen before negating: LONG_MIN has no positive long.
        append_uint(text, static_cast<std::uint64_t>(-static_cast<std::int64_t>(value)));
    } else {
        append_uint(text, static_cast<std::uint64_t>(value));
    }
}

void append_millis(std::wstring& text, std::uint32_t frames, std::uint32_t rate)
{
    // Microseconds, exact in 64 bits for every 32-bit width and rate. Ties go
    // to even, as the fixed-point conversion of a double rounds. At the
    // supported rates a three-decimal tie is a dyadic value that a double holds
    // exactly, and every other quotient lies further from a rounding boundary
    // than the double's error, so the text equals the former {:.3f} of
    // 1000.0 * frames / rate (checked against std::format in the tests).
    if (rate == 0) { return; } // Dividing by it would fault (#DE), not throw.
    const auto scaled = std::uint64_t{frames} * 1'000'000;
    auto micro = scaled / rate;
    const auto remainder = scaled % rate;
    if (2 * remainder > rate || (2 * remainder == rate && micro % 2 != 0)) {
        ++micro;
    }
    append_uint(text, micro / 1000);
    text += L'.';
    append_uint(text, micro % 1000, 3);
}

const wchar_t* state_text(PlaybackState state) noexcept
{
    switch (state) {
    case PlaybackState::initializing: return L"Initializing";
    case PlaybackState::reinitializing: return L"Reinitializing";
    case PlaybackState::stopped: return L"Stopped";
    case PlaybackState::starting: return L"Starting";
    case PlaybackState::playing: return L"Playing";
    case PlaybackState::pausing: return L"Pausing";
    case PlaybackState::stopping: return L"Stopping";
    case PlaybackState::paused: return L"Paused";
    case PlaybackState::completed: return L"Completed";
    case PlaybackState::failed: return L"Error";
    case PlaybackState::closing: return L"Closing";
    case PlaybackState::closed: return L"Closed";
    }
    return L"Error";
}

const wchar_t* result_text(playback::PlaybackResult result) noexcept
{
    switch (result) {
    case playback::PlaybackResult::underrun: return L"Audio underrun. Restart the application to retry.";
    case playback::PlaybackResult::read_error: return L"Cannot read WAV data. Restart the application to retry.";
    case playback::PlaybackResult::driver_error: return L"ASIO driver error. Restart the application to retry.";
    case playback::PlaybackResult::reinitialize_required: return L"ASIO device reinitialization was requested.";
    case playback::PlaybackResult::first_rate_rejected: return L"The driver rejected the WAV sample rate.";
    case playback::PlaybackResult::next_rate_rejected: return L"The driver rejected the next sample rate.";
    case playback::PlaybackResult::next_track_rejected: return L"The next track was rejected.";
    case playback::PlaybackResult::playing:
    case playback::PlaybackResult::completed:
    case playback::PlaybackResult::paused:
    case playback::PlaybackResult::stopped: return L"";
    }
    return L"";
}

std::wstring error_text(const PlaybackSnapshot& snapshot)
{
    if (!snapshot.error) {
        return snapshot.track_error ? exception_message(snapshot.track_error) : result_text(snapshot.result);
    }
    if (snapshot.result != playback::PlaybackResult::read_error) {
        return exception_message(snapshot.error);
    }
    std::wstring text(result_text(snapshot.result));
    text += cause_separator;
    text += exception_message(snapshot.error);
    return text;
}

std::wstring buffer_text(const PlaybackSnapshot& snapshot)
{
    std::wstring text;
    buffer_text(text, snapshot);
    return text;
}

void buffer_text(std::wstring& text, const PlaybackSnapshot& snapshot)
{
    text.clear();
    if (!snapshot.settings_loaded) {
        text += snapshot.faulted() ? L"Configuration unavailable" : L"Awaiting configuration";
        return;
    }
    text.reserve(256); // The longest text, with every value at 10 digits, in one allocation.
    if (snapshot.requested_buffer_frames == 0) {
        text += L"Requested: driver preferred (BufferSize=0)";
    } else {
        text += L"Requested: ";
        append_signed(text, snapshot.requested_buffer_frames);
        text += L" frames";
    }
    const auto segment = snapshot.diagnostics.current_segment;
    if (const auto& setup = snapshot.diagnostics.segments.at(segment)) {
        // Retain the verified history after disposal, without describing it as
        // a live allocation. Formatting stays on state changes, never a timer.
        const bool active = snapshot.state == PlaybackState::playing || snapshot.state == PlaybackState::pausing ||
            snapshot.state == PlaybackState::stopping;
        text += active ? L"\r\nAllocated: " : L"\r\nLast allocation: ";
        append_signed(text, setup->buffer_frames);
        text += L" frames";
        if (const auto rate = integral_rate(setup->sample_rate); rate != 0 && setup->buffer_frames >= 0) {
            text += L" / ";
            append_millis(text, static_cast<std::uint32_t>(setup->buffer_frames), rate);
            text += L" ms at ";
            append_uint(text, rate);
            text += L" Hz";
        }
    } else {
        text += L"\r\nAllocation: not allocated";
    }
    if (const auto& caps = snapshot.diagnostics.buffer_capabilities.at(segment)) {
        text += L"\r\nCapabilities";
        if (const auto rate = integral_rate(caps->sample_rate)) {
            text += L" at ";
            append_uint(text, rate);
            text += L" Hz";
        }
        text += L": min ";
        append_signed(text, caps->minimum);
        text += L"; preferred ";
        append_signed(text, caps->preferred);
        text += L"\r\nMax ";
        append_signed(text, caps->maximum);
        text += L"; granularity ";
        append_signed(text, caps->granularity);
        text += caps->granularity == -1 ? L" (powers of two)" :
            caps->granularity == 0 ? L" (no extra step)" : L" (step from min)";
    } else {
        text += L"\r\nCapabilities: unavailable";
    }
}

std::wstring output_text(const PlaybackSnapshot& snapshot)
{
    std::wstring text;
    output_text(text, snapshot);
    return text;
}

void output_text(std::wstring& text, const PlaybackSnapshot& snapshot)
{
    text.clear();
    if (!snapshot.settings_loaded) {
        text += snapshot.faulted() ? L"Output: configuration unavailable" : L"Output: awaiting configuration";
        return;
    }
    if (IsEqualCLSID(snapshot.selected_device, CLSID{})) { text += L"Output: not configured"; return; }
    // Settings bound OutputPair to LONG_MAX - 2, so the second channel fits.
    text += L"Output: ";
    append_signed(text, snapshot.output_pair);
    text += L" / ";
    append_signed(text, snapshot.output_pair + 1);
    text += L" (configured)";
}

std::wstring device_text(const PlaybackSnapshot& snapshot)
{
    std::wstring text;
    device_text(text, snapshot);
    return text;
}

void device_text(std::wstring& text, const PlaybackSnapshot& snapshot)
{
    text.clear();
    if (!snapshot.device_available && (snapshot.faulted() ||
        (snapshot.settings_loaded && IsEqualCLSID(snapshot.selected_device, CLSID{})))) {
        text += L"Device: NO DEVICE";
        return;
    }
    if (!snapshot.settings_loaded) { text += L"Device: awaiting configuration"; return; }
    wchar_t clsid[39]{};
    if (!StringFromGUID2(snapshot.selected_device, clsid, ARRAYSIZE(clsid))) {
        throw std::runtime_error("Cannot format the configured driver CLSID");
    }
    text += L"Device: ";
    if (!snapshot.device_name.empty()) {
        text += snapshot.device_name;
        text += L' ';
    }
    text += clsid;
}

std::wstring playlist_label_text(const PlaybackSnapshot& snapshot)
{
    std::wstring text;
    playlist_label_text(text, snapshot);
    return text;
}

void playlist_label_text(std::wstring& text, const PlaybackSnapshot& snapshot)
{
    text.clear();
    text += L"&List: ";
    append_uint(text, snapshot.playlist ? snapshot.playlist->size() : 0);
    text += L" tracks, ";
    append_uint(text, snapshot.excluded_count);
    text += L" excluded";
}

std::wstring playback_time_text(std::uint64_t position_frames,
    std::uint64_t frame_count, std::uint32_t sample_rate)
{
    std::wstring text;
    playback_time_text(text, position_frames, frame_count, sample_rate);
    return text;
}

void playback_time_text(std::wstring& text, std::uint64_t position_frames,
    std::uint64_t frame_count, std::uint32_t sample_rate)
{
    text.clear();
    if (sample_rate == 0) { text += L"Time: --:-- / --:--"; return; }
    text += L"Time: ";
    append_duration(text, std::min(position_frames, frame_count) / sample_rate);
    text += L" / ";
    append_duration(text, frame_count / sample_rate);
}

const wchar_t* pending_text(const PlaybackSnapshot& snapshot, bool cleanup_error,
    bool close_requested, bool pending_selection, PlaybackCommand pending_command) noexcept
{
    if (cleanup_error) { return L"Device released. Close again to exit."; }
    if (close_requested) { return L"Close requested. Waiting for the device to release..."; }
    if (snapshot.accepted != snapshot.settled) {
        return pending_selection ? L"Selection requested..." :
            pending_command == PlaybackCommand::play ? L"Play requested..." :
            pending_command == PlaybackCommand::pause ? L"Pause requested..." : L"Stop requested...";
    }
    return L"Space: Play / Pause    Escape: Stop";
}

TransportAvailability available(const control::PlaybackCommandState& snapshot, std::uint64_t stop_receipt) noexcept
{
    const bool settled = snapshot.accepted == snapshot.settled;
    const auto state = snapshot.state;
    return {
        .toggle = settled && snapshot.has_tracks &&
            (state == PlaybackState::stopped || state == PlaybackState::playing ||
            state == PlaybackState::paused || state == PlaybackState::completed),
        .stop = snapshot.settled >= stop_receipt && (state == PlaybackState::starting ||
            state == PlaybackState::playing || state == PlaybackState::pausing || state == PlaybackState::paused ||
            state == PlaybackState::completed ||
            (state == PlaybackState::stopped && (!settled || snapshot.position_frames != 0))),
        .select = snapshot.has_tracks && !snapshot.faulted &&
            state != PlaybackState::initializing && state != PlaybackState::reinitializing &&
            state != PlaybackState::closing && state != PlaybackState::closed};
}

} // namespace wavplay::ui
