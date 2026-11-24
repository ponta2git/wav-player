#pragma once

#include "wavplay/ui/player_options.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace wavplay::ui {

// Child IDs belong to the view that creates the controls. They are contiguous
// so the view keeps exactly one cached text per control.
namespace control_id {
inline constexpr int play_pause = 101, stop = 102, status = 103, error = 104,
    file = 105, device = 106, pending = 107, file_label = 108, error_label = 109, buffer = 110,
    output = 111, buffer_label = 112, playlist = 113, playlist_label = 114, time = 115;
inline constexpr int first = play_pause, last = time, count = last - first + 1;

[[nodiscard]] constexpr std::size_t slot(int id) noexcept { return static_cast<std::size_t>(id - first); }

static_assert([] {
    std::array ids{play_pause, stop, status, error, file, device, pending, file_label, error_label, buffer,
        output, buffer_label, playlist, playlist_label, time};
    std::ranges::sort(ids);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (slot(ids[index]) != index) {
            return false;
        }
    }
    return ids.back() == last;
}(), "control IDs must cover first..last without gaps or duplicates");
static_assert(count == 15);
} // namespace control_id

// How the time field draws itself. Both functions draw like a left-aligned,
// single-line standard label: `text` starts at the top-left corner of
// `bounds`, in `font` and the system's label colors, and never wraps.
//
// Fills `clip` and draws the part of the text inside it; other pixels stay.
// False if GDI refuses.
[[nodiscard]] bool draw_label(HDC dc, HFONT font, const RECT& bounds, const RECT& clip, std::wstring_view text) noexcept;
// The label shows `previous` and its text is now `next`. Redraws the
// characters that differ, with one neighbor on each side for the overhang of a
// glyph, and everything after them unless the text kept its extent. False
// without drawing if a text has more than label_capacity characters or GDI
// refuses; the caller then repaints the label.
inline constexpr std::size_t label_capacity = 127;
[[nodiscard]] bool redraw_changed_label(HDC dc, HFONT font, const RECT& bounds, std::wstring_view previous,
    std::wstring_view next) noexcept;

// The parent HWND owns the controls; this object owns their shared GDI font.
// No playback state or command policy belongs in this view.
class PlayerControls {
public:
    PlayerControls() = default;
    PlayerControls(const PlayerControls&) = delete; // The time field's subclass keeps this address.
    PlayerControls& operator=(const PlayerControls&) = delete;
    void create(HWND window, const PlayerOptions& options);
    void update_metrics(UINT dpi);
    void layout();
    [[nodiscard]] SIZE window_size(bool initial = false) const;
    [[nodiscard]] HWND child(int id) const noexcept;
    // Sets a control's text only when it differs from the cached copy, which
    // preserves selection and scrolling in read-only text. The cache takes the
    // new text only after SetWindowTextW succeeds, so it equals the control.
    void text(int id, std::wstring value);
    void text(int id, const wchar_t* value); // Allocates only when the text changes.
    // Same update, with a caller-owned prepared string. On success it holds the
    // previous cached text, whose storage the caller can reuse. An unchanged
    // value or a failed SetWindowTextW leaves it intact. No allocation follows
    // the window update: the cache commits by a noexcept string swap.
    // The time field then redraws only the characters that changed; every
    // other control repaints itself.
    void text_reuse(int id, std::wstring& prepared);
    // For failure paths: sets the control without throwing and marks the cached
    // copy unknown, so the next text() call sets the control again.
    void show_text(int id, const wchar_t* value) noexcept;
    void playlist(const std::shared_ptr<const std::vector<std::wstring>>& paths);
    // Both hand the keyboard to the fallback target: the enabled playlist,
    // otherwise the frame. enable() does so when it disables the focused
    // control; ensure_focus() when the frame is active and either has the
    // focus itself or no window has it.
    void enable(int id, bool enabled) const noexcept;
    void ensure_focus() const noexcept;
    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    struct DeleteFont { void operator()(HFONT font) const noexcept { DeleteObject(font); } };
    using Font = std::unique_ptr<std::remove_pointer_t<HFONT>, DeleteFont>;
    HWND window_{};
    Font font_;
    LOGFONTW font_description_{};
    UINT dpi_{96};
    int base_x_{7}, base_y_{16};
    SIZE layout_size_{};
    bool ready_{}, layout_dirty_{true};
    std::array<std::wstring, control_id::count> text_;
    std::array<bool, control_id::count> text_unknown_{};
    std::shared_ptr<const std::vector<std::wstring>> paths_;

    [[nodiscard]] int x(int units) const noexcept { return MulDiv(units, base_x_, 4); }
    [[nodiscard]] int y(int units) const noexcept { return MulDiv(units, base_y_, 8); }
    void add(const wchar_t* type, std::wstring_view value, int id, DWORD style, DWORD extended = 0);
    // The time field is a standard label that this object paints: all of it
    // for WM_PAINT, and only what changed when text_reuse() sets a new text.
    void redraw_time(HWND item, std::wstring_view previous, std::wstring_view next) const noexcept;
    void paint_time(HWND item, HDC dc, const RECT& clip) const noexcept;
    static LRESULT CALLBACK time_field_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass, DWORD_PTR context) noexcept;
    void update_playlist_extent();
    [[nodiscard]] HWND focus_fallback() const noexcept;
};

} // namespace wavplay::ui
