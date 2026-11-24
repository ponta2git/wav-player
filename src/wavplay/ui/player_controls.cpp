#include "wavplay/ui/player_controls.h"

#include "wavplay/core/win32.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace wavplay::ui {
namespace {

bool same_font(const LOGFONTW& left, const LOGFONTW& right) noexcept
{
    // Compare the logical description, not structure padding or unused bytes
    // after the face name. Settings broadcasts often leave this unchanged.
    return std::tie(left.lfHeight, left.lfWidth, left.lfEscapement, left.lfOrientation,
        left.lfWeight, left.lfItalic, left.lfUnderline, left.lfStrikeOut, left.lfCharSet,
        left.lfOutPrecision, left.lfClipPrecision, left.lfQuality, left.lfPitchAndFamily) ==
        std::tie(right.lfHeight, right.lfWidth, right.lfEscapement, right.lfOrientation,
            right.lfWeight, right.lfItalic, right.lfUnderline, right.lfStrikeOut, right.lfCharSet,
            right.lfOutPrecision, right.lfClipPrecision, right.lfQuality, right.lfPitchAndFamily) &&
        std::wstring_view(left.lfFaceName) == std::wstring_view(right.lfFaceName);
}

LRESULT CALLBACK read_only_edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR) noexcept
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, read_only_edit_proc, subclass);
    }
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_GETDLGCODE && wparam == VK_TAB) {
        // Multiline EDIT claims all Tab input even when readonly. Let the native
        // dialog navigation handle Tab, preserving editing/copying for every other key.
        return result & ~(DLGC_WANTALLKEYS | DLGC_WANTTAB);
    }
    return result;
}

// A window's client-area DC from GetDC(window), not the GetDC(nullptr) screen
// DC, with a font selected for measurement. The previous font is restored and
// the DC released on every path, including exceptions. Each caller names its
// own GetDC operation and font-selection failure text.
class ScreenDc {
public:
    ScreenDc(HWND window, HFONT font, const char* operation, const char* select_failure) :
        window_(window), dc_(GetDC(window))
    {
        if (!dc_) {
            core::throw_last_error(operation);
        }
        previous_ = SelectObject(dc_, font);
        if (!previous_ || previous_ == HGDI_ERROR) {
            ReleaseDC(window_, dc_);
            throw std::runtime_error(select_failure);
        }
    }
    ~ScreenDc()
    {
        SelectObject(dc_, previous_);
        ReleaseDC(window_, dc_);
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;
    [[nodiscard]] HDC get() const noexcept { return dc_; }

private:
    HWND window_;
    HDC dc_;
    HGDIOBJ previous_{};
};

// Draws with the font already selected into `dc`, in the colors DefWindowProc
// gives a standard label, and leaves the colors of `dc` as they were.
bool draw_label_text(HDC dc, const RECT& bounds, const RECT& clip, std::wstring_view text) noexcept
{
    const auto background = SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
    const auto foreground = SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    const bool drawn = ExtTextOutW(dc, bounds.left, bounds.top, ETO_OPAQUE | ETO_CLIPPED, &clip,
        text.data(), static_cast<UINT>(text.size()), nullptr) != FALSE;
    SetTextColor(dc, foreground);
    SetBkColor(dc, background);
    return drawn;
}

} // namespace

bool draw_label(HDC dc, HFONT font, const RECT& bounds, const RECT& clip, std::wstring_view text) noexcept
{
    const auto selected = SelectObject(dc, font);
    if (!selected || selected == HGDI_ERROR) {
        return false;
    }
    const bool drawn = draw_label_text(dc, bounds, clip, text);
    SelectObject(dc, selected);
    return drawn;
}

bool redraw_changed_label(HDC dc, HFONT font, const RECT& bounds, std::wstring_view previous,
    std::wstring_view next) noexcept
{
    if (previous.size() > label_capacity || next.size() > label_capacity) {
        return false;
    }
    const auto selected = SelectObject(dc, font);
    if (!selected || selected == HGDI_ERROR) {
        return false;
    }
    std::array<int, label_capacity> ends{}; // ends[i]: the extent of the first i + 1 characters of `next`.
    SIZE before{}, after{};
    bool drawn{};
    if ((previous.empty() || GetTextExtentPoint32W(dc, previous.data(), static_cast<int>(previous.size()), &before)) &&
        (next.empty() || GetTextExtentExPointW(dc, next.data(), static_cast<int>(next.size()), 0, nullptr,
            ends.data(), &after))) {
        // [first, tail) of `next` replaces the characters between the common
        // head and the common tail of the two texts.
        std::size_t first = 0;
        while (first < previous.size() && first < next.size() && previous[first] == next[first]) {
            ++first;
        }
        std::size_t tail = next.size();
        for (auto old = previous.size(); old > first && tail > first && previous[old - 1] == next[tail - 1]; --old) {
            --tail;
        }
        RECT clip = bounds;
        if (first > 1) {
            clip.left = bounds.left + ends[first - 2];
        }
        // With the same extent, the common tail kept its position.
        if (before.cx == after.cx && tail < next.size()) {
            clip.right = std::min(bounds.right, bounds.left + ends[tail]);
        }
        drawn = clip.left >= clip.right || draw_label_text(dc, bounds, clip, next);
    }
    SelectObject(dc, selected);
    return drawn;
}

void PlayerControls::redraw_time(HWND item, std::wstring_view previous, std::wstring_view next) const noexcept
{
    if (!IsWindowVisible(item)) {
        return; // Showing the window paints the stored text.
    }
    bool drawn{};
    if (const auto dc = GetDC(item)) {
        RECT bounds{};
        drawn = GetClientRect(item, &bounds) && redraw_changed_label(dc, font_.get(), bounds, previous, next);
        ReleaseDC(item, dc);
    }
    if (!drawn) {
        InvalidateRect(item, nullptr, FALSE);
    }
}

void PlayerControls::paint_time(HWND item, HDC dc, const RECT& clip) const noexcept
{
    // The stored window text, not the cache: show_text() replaces only the
    // former. Its first label_capacity characters; no text of this field is longer.
    std::array<wchar_t, label_capacity + 1> text{};
    const auto size = GetWindowTextW(item, text.data(), static_cast<int>(text.size()));
    RECT bounds{};
    if (GetClientRect(item, &bounds)) {
        (void)draw_label(dc, font_.get(), bounds, clip, {text.data(), static_cast<std::size_t>(size)});
    }
}

// The standard label repaints its whole row for every new text. This subclass
// keeps the label's class, text and messages, but stores a new text without
// that repaint and paints the field itself, so that text_reuse() can redraw
// only the characters that changed. Only PlayerControls sets this text.
LRESULT CALLBACK PlayerControls::time_field_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass, DWORD_PTR context) noexcept
{
    const auto& self = *reinterpret_cast<const PlayerControls*>(context);
    switch (message) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, time_field_proc, subclass);
        break;
    case WM_SETTEXT: {
        const auto stored = DefWindowProcW(window, message, wparam, lparam);
        // While the field shows the cached text, text_reuse() redraws what
        // changed. Otherwise the pixels are not known: repaint the field.
        if (self.text_unknown_[control_id::slot(control_id::time)]) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return stored;
    }
    case WM_ERASEBKGND:
        return 1; // Painting fills its own background.
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        if (const auto dc = BeginPaint(window, &paint)) {
            self.paint_time(window, dc, paint.rcPaint);
            EndPaint(window, &paint);
        }
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT bounds{};
        if (GetClientRect(window, &bounds)) {
            self.paint_time(window, reinterpret_cast<HDC>(wparam), bounds);
        }
        return 0;
    }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

HWND PlayerControls::child(int id) const noexcept { return GetDlgItem(window_, id); }

void PlayerControls::add(const wchar_t* type, std::wstring_view value, int id, DWORD style, DWORD extended)
{
    const std::wstring terminated(value);
    const auto item = CreateWindowExW(extended, type, terminated.c_str(), WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (!item) {
        core::throw_last_error("CreateWindowExW(control)");
    }
    if (id == control_id::time) {
        // Without the subclass the label repaints itself, as the others do.
        (void)SetWindowSubclass(item, time_field_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    text_.at(control_id::slot(id)) = value;
}

void PlayerControls::create(HWND window, const PlayerOptions& options)
{
    window_ = window;
    add(L"STATIC", L"Device: awaiting configuration",
        control_id::device, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    add(L"STATIC", L"Output: awaiting configuration", control_id::output, SS_LEFT | SS_NOPREFIX);
    // Adjacent labels give the standard edit controls an accessible name and mnemonic.
    add(L"STATIC", L"&Buffer:", control_id::buffer_label, SS_LEFT);
    add(L"EDIT", L"Awaiting configuration", control_id::buffer,
        WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"&List: loading...", control_id::playlist_label, SS_LEFT);
    add(L"LISTBOX", L"", control_id::playlist,
        WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"&File:", control_id::file_label, SS_LEFT);
    add(L"EDIT", options.path, control_id::file,
        WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"&Play", control_id::play_pause, WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED);
    add(L"BUTTON", L"&Stop", control_id::stop, WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED);
    add(L"STATIC", L"Initializing", control_id::status, SS_LEFT | SS_NOPREFIX);
    add(L"STATIC", L"Time: --:-- / --:--", control_id::time, SS_LEFT | SS_NOPREFIX);
    add(L"STATIC", L"Space: Play / Pause    Escape: Stop", control_id::pending, SS_LEFT | SS_NOPREFIX);
    add(L"STATIC", L"&Error:", control_id::error_label, SS_LEFT);
    add(L"EDIT", L"", control_id::error,
        WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE);
    for (const auto id : {control_id::buffer, control_id::file, control_id::error}) {
        if (!SetWindowSubclass(child(id), read_only_edit_proc, 1, 0)) {
            throw std::runtime_error("Cannot enable readonly text keyboard navigation");
        }
    }
    update_metrics(GetDpiForWindow(window_));
    ready_ = true;
}

void PlayerControls::update_metrics(UINT dpi)
{
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        core::throw_last_error("SystemParametersInfoForDpi");
    }
    if (font_ && dpi_ == dpi && same_font(font_description_, metrics.lfMessageFont)) {
        // A setting can change how the same font is drawn. The time field
        // would otherwise keep its unchanged characters in the old rendering.
        InvalidateRect(child(control_id::time), nullptr, FALSE);
        return;
    }
    Font next(CreateFontIndirectW(&metrics.lfMessageFont));
    if (!next) {
        core::throw_last_error("CreateFontIndirectW");
    }
    TEXTMETRICW dimensions{};
    {
        const ScreenDc dc(window_, next.get(), "GetDC", "Cannot select the system UI font");
        if (!GetTextMetricsW(dc.get(), &dimensions)) {
            throw std::runtime_error("Cannot measure the system UI font");
        }
    }
    dpi_ = dpi;
    base_x_ = std::max(1L, dimensions.tmAveCharWidth);
    base_y_ = std::max(1L, dimensions.tmHeight + dimensions.tmExternalLeading);
    for (auto item = GetWindow(window_, GW_CHILD); item; item = GetWindow(item, GW_HWNDNEXT)) {
        SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(next.get()), FALSE);
    }
    font_ = std::move(next); // Every control has stopped using the old font.
    font_description_ = metrics.lfMessageFont;
    layout_dirty_ = true;
    update_playlist_extent();
}

SIZE PlayerControls::window_size(bool initial) const
{
    // Client bounds are derived from the same font units as the actual layout.
    RECT rectangle{0, 0, x(initial ? 440 : 320), y(initial ? 320 : 176)};
    if (!AdjustWindowRectExForDpi(&rectangle, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)), dpi_)) {
        core::throw_last_error("AdjustWindowRectExForDpi");
    }
    return {rectangle.right - rectangle.left, rectangle.bottom - rectangle.top};
}

void PlayerControls::layout()
{
    if (!ready_) {
        return;
    }
    RECT area{};
    if (!GetClientRect(window_, &area)) {
        core::throw_last_error("GetClientRect");
    }
    if (!layout_dirty_ && layout_size_.cx == area.right && layout_size_.cy == area.bottom) {
        return;
    }
    const int width = std::max(0L, area.right - x(16));
    struct Placement { int id, left, top, width, height; };
    // Reserve readable diagnostics and transport first. All remaining vertical
    // space belongs to the list and its copyable, wrapped selected-path view.
    // Sharing that row also keeps the full UI usable on a small, scaled monitor.
    const int fixed_height = y(30) + y(4) + y(10) + y(2) + y(6) + y(18) +
        y(6) + y(10) + y(2) + y(10) + y(4) + y(4);
    const int edit_space = area.bottom - fixed_height;
    const int minimum_edit = y(22);
    const int buffer_height = std::clamp(edit_space / 3, minimum_edit, y(56));
    const int error_height = std::clamp(edit_space / 5, minimum_edit, y(36));
    const int list_height = std::max(minimum_edit, edit_space - buffer_height - error_height);
    const int list_label_top = y(30) + buffer_height + y(4);
    const int list_top = list_label_top + y(10) + y(2);
    const int button_top = list_top + list_height + y(6);
    const int status_top = button_top + y(18) + y(6);
    const int pending_top = status_top + y(10) + y(2);
    const int error_top = pending_top + y(10) + y(4);
    const int edit_left = x(48), label_width = x(36);
    const int edit_width = std::max(1L, area.right - edit_left - x(8));
    const int list_width = (width - x(8)) * 2 / 3;
    const int file_left = x(8) + list_width + x(8);
    const int file_width = std::max(1L, area.right - file_left - x(8));
    const std::array placements{
        Placement{control_id::device, x(8), y(4), width, y(10)},
        Placement{control_id::output, x(8), y(16), width, y(10)},
        Placement{control_id::buffer_label, x(8), y(30), label_width, y(10)},
        Placement{control_id::buffer, edit_left, y(30), edit_width, buffer_height},
        Placement{control_id::playlist_label, x(8), list_label_top, list_width, y(10)},
        Placement{control_id::playlist, x(8), list_top, list_width, list_height},
        Placement{control_id::file_label, file_left, list_label_top, file_width, y(10)},
        Placement{control_id::file, file_left, list_top, file_width, list_height},
        Placement{control_id::play_pause, x(8), button_top, x(60), y(18)},
        Placement{control_id::stop, x(76), button_top, x(50), y(18)},
        Placement{control_id::status, x(136), button_top + y(4), std::max(1, width - x(128)), y(10)},
        Placement{control_id::time, x(8), status_top, width, y(10)},
        Placement{control_id::pending, x(8), pending_top, width, y(10)},
        Placement{control_id::error_label, x(8), error_top, label_width, y(10)},
        Placement{control_id::error, edit_left, error_top, edit_width, error_height}};
    auto positions = BeginDeferWindowPos(static_cast<int>(placements.size()));
    if (!positions) {
        core::throw_last_error("BeginDeferWindowPos");
    }
    for (const auto& item : placements) {
        positions = DeferWindowPos(positions, child(item.id), nullptr, item.left, item.top, item.width, item.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (!positions) {
            core::throw_last_error("DeferWindowPos");
        }
    }
    if (!EndDeferWindowPos(positions)) {
        core::throw_last_error("EndDeferWindowPos");
    }
    layout_size_ = {area.right, area.bottom};
    layout_dirty_ = false;
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void PlayerControls::playlist(const std::shared_ptr<const std::vector<std::wstring>>& paths)
{
    if (paths_ == paths) { return; }
    const auto list = child(control_id::playlist);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    struct RedrawScope {
        HWND list;
        ~RedrawScope() { SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE); }
    } redraw{list};
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (paths) {
        if (paths->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::length_error("Playlist exceeds the Windows list limit");
        }
        // Reserve the item array and the string storage (bytes) once, instead of
        // growing them row by row. A refusal surfaces from LB_ADDSTRING below.
        std::size_t bytes{};
        for (const auto& path : *paths) {
            bytes += (path.size() + 1) * sizeof(wchar_t);
        }
        (void)SendMessageW(list, LB_INITSTORAGE, static_cast<WPARAM>(paths->size()), static_cast<LPARAM>(bytes));
        for (const auto& path : *paths) {
            const auto result = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
            if (result == LB_ERR || result == LB_ERRSPACE) { throw std::runtime_error("Cannot display the playlist"); }
        }
    }
    paths_ = paths;
    update_playlist_extent();
}

void PlayerControls::update_playlist_extent()
{
    if (!paths_ || !font_) { return; }
    const auto list = child(control_id::playlist);
    LONG width{};
    {
        const ScreenDc dc(list, font_.get(), "GetDC(playlist)", "Cannot measure playlist paths");
        for (const auto& path : *paths_) {
            SIZE size{};
            if (path.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                !GetTextExtentPoint32W(dc.get(), path.c_str(), static_cast<int>(path.size()), &size)) {
                throw std::runtime_error("Cannot measure playlist paths");
            }
            width = std::max(width, size.cx);
        }
    }
    SendMessageW(list, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(width + x(4)), 0);
}

void PlayerControls::text(int id, std::wstring value)
{
    text_reuse(id, value);
}

void PlayerControls::text_reuse(int id, std::wstring& value)
{
    const auto slot = control_id::slot(id);
    auto& cached = text_.at(slot);
    if (!text_unknown_[slot] && cached == value) {
        return;
    }
    const auto item = child(id);
    if (!SetWindowTextW(item, value.c_str())) {
        core::throw_last_error("SetWindowTextW");
    }
    // The time field does not repaint itself for a text that follows the
    // cached one (time_field_proc). Nothing after the window update throws.
    if (id == control_id::time && !text_unknown_[slot]) {
        redraw_time(item, cached, value);
    }
    cached.swap(value); // Commit only after success; return the previous storage.
    text_unknown_[slot] = false;
}

void PlayerControls::text(int id, const wchar_t* value)
{
    const auto slot = control_id::slot(id);
    if (!text_unknown_.at(slot) && text_[slot] == value) {
        return;
    }
    text(id, std::wstring(value));
}

void PlayerControls::show_text(int id, const wchar_t* value) noexcept
{
    // Marked first: the time field repaints for a text set while it is unknown.
    text_unknown_[control_id::slot(id)] = true;
    (void)SetWindowTextW(child(id), value);
}

void PlayerControls::enable(int id, bool enabled) const noexcept
{
    const auto item = child(id);
    if ((IsWindowEnabled(item) != FALSE) == enabled) {
        return;
    }
    const bool focused = !enabled && GetFocus() == item;
    EnableWindow(item, enabled);
    if (focused) {
        // A disabled control cannot keep the keyboard. Disabling first means
        // the fallback never picks this control, even when it is the list.
        SetFocus(focus_fallback());
    }
}

void PlayerControls::ensure_focus() const noexcept
{
    const auto focus = GetFocus();
    if (GetActiveWindow() == window_ && (!focus || focus == window_)) {
        const auto target = focus_fallback();
        if (target != focus) {
            SetFocus(target);
        }
    }
}

HWND PlayerControls::focus_fallback() const noexcept
{
    // Not the first tab stop: the read-only EDITs would run the caret-blink
    // timer and swallow Enter. The frame itself still takes Space, Escape and
    // the access keys through the message loop.
    const auto list = child(control_id::playlist);
    return IsWindowEnabled(list) ? list : window_;
}

} // namespace wavplay::ui
