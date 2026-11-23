#include "wavplay/settings/device_config.h"

#include "wavplay/asio/device_settings.h"
#include "wavplay/core/win32.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace wavplay::settings {
namespace {

// The [Window] keys, in the order of their names below. The position keys
// come first and accept negative values; with the two size keys they form the
// geometry, which a file gives completely or not at all.
enum : int { key_x, key_y, key_width, key_height, key_maximized, window_key_count, geometry_key_count = key_maximized };
constexpr std::array<std::string_view, 5> window_keys{"X", "Y", "Width", "Height", "Maximized"};
static_assert(window_keys.size() == window_key_count);

// A blank result is still a view into `text` (at its end), never a null view,
// so the writer can compute every value's offset in the source.
std::string_view trim(std::string_view text) noexcept
{
    constexpr std::string_view space = " \t\r";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) { return text.substr(text.size()); }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

bool equal_ascii(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) { return false; }
    const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (lower(left[index]) != lower(right[index])) { return false; }
    }
    return true;
}

// Decodes the whole text once (at most 1 MiB, at startup and exit) to check it
// with the same rule that decodes each value.
void validate_text(std::string_view utf8)
{
    if (utf8.size() > max_config_bytes) { throw std::invalid_argument("settings.ini exceeds 1 MiB"); }
    if (utf8.find('\0') != std::string_view::npos || !core::utf8_to_wide(utf8)) {
        throw std::invalid_argument("settings.ini must contain valid UTF-8 text");
    }
}

// Values are cut from validated text at ASCII delimiters, so they decode too.
std::wstring unicode(std::string_view value)
{
    auto wide = core::utf8_to_wide(value);
    if (!wide) { throw std::invalid_argument("settings.ini must contain valid UTF-8 text"); }
    return std::move(*wide);
}

struct Entry {
    std::string_view section, key, value;
    bool assigned{};
};

// Views retain their offsets in the original UTF-8 bytes, empty values
// included: an unassigned key's value is the empty view at its line's end.
// The writer uses the same scanner as the reader, so comments and whitespace
// need no reserialization.
void scan(std::string_view utf8, auto visit)
{
    if (utf8.starts_with("\xef\xbb\xbf")) { utf8.remove_prefix(3); }
    std::string_view section;
    while (!utf8.empty()) {
        const auto end = utf8.find('\n');
        const auto line = trim(utf8.substr(0, end));
        if (end == std::string_view::npos) { utf8 = {}; }
        else { utf8.remove_prefix(end + 1); }
        if (line.empty() || line.front() == ';' || line.front() == '#') { continue; }
        if (line.front() == '[') {
            const auto closing = line.find(']');
            if (closing == std::string_view::npos) { throw std::invalid_argument("Unclosed settings.ini section"); }
            const auto suffix = trim(line.substr(closing + 1));
            if (!suffix.empty() && suffix.front() != ';' && suffix.front() != '#') {
                throw std::invalid_argument("Invalid settings.ini section suffix");
            }
            section = trim(line.substr(1, closing - 1));
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            visit(Entry{section, line.substr(0, line.find_first_of(" \t;#")), line.substr(line.size()), false});
        } else {
            const auto tail = line.substr(separator + 1);
            visit(Entry{section, trim(line.substr(0, separator)),
                trim(tail.substr(0, tail.find_first_of(";#"))), true});
        }
    }
}

void require_unique_assignment(const Entry& entry, bool& seen)
{
    if (!entry.assigned) {
        throw std::invalid_argument(std::string(entry.key) + " requires an equals sign and a value");
    }
    if (std::exchange(seen, true)) {
        throw std::invalid_argument("Duplicate " + std::string(entry.key) + " in settings.ini");
    }
}

std::string selected_section(const CLSID& selected)
{
    std::array<wchar_t, 39> guid{};
    if (!StringFromGUID2(selected, guid.data(), static_cast<int>(guid.size()))) {
        throw std::runtime_error("Cannot format the settings device CLSID");
    }
    std::string result = "Device.";
    for (std::size_t index = 0; index + 1 < guid.size(); ++index) {
        result += static_cast<char>(guid[index]); // GUID syntax is entirely ASCII.
    }
    return result;
}

long decimal_value(std::string_view text, long minimum, long maximum, std::string_view key)
{
    long value{};
    if (!text.empty() && ((text.front() >= '0' && text.front() <= '9') ||
        (minimum < 0 && text.front() == '-'))) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
            value >= minimum && value <= maximum) { return value; }
    }
    throw std::invalid_argument(std::string(key) + " must be a decimal integer from " +
        std::to_string(minimum) + " to " + std::to_string(maximum));
}

std::optional<std::size_t> window_key(std::string_view key) noexcept
{
    for (std::size_t index = 0; index < window_keys.size(); ++index) {
        if (equal_ascii(key, window_keys[index])) { return index; }
    }
    return {};
}

void validate_window(const WindowPlacement& window)
{
    if (window.width <= 0 || window.height <= 0 ||
        static_cast<std::int64_t>(window.x) + window.width > (std::numeric_limits<int>::max)() ||
        static_cast<std::int64_t>(window.y) + window.height > (std::numeric_limits<int>::max)()) {
        throw std::invalid_argument("Invalid settings.ini window rectangle");
    }
}

struct SourceFile {
    std::string bytes;
    bool exists{};
};

SourceFile read_config(const wchar_t* path)
{
    if (!path || !*path) { throw std::invalid_argument("settings.ini path is empty"); }
    const auto raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { return {}; }
        core::throw_win32_error(error, "Cannot open settings.ini");
    }
    const core::UniqueHandle file(raw);
    if (GetFileType(file.get()) != FILE_TYPE_DISK) { throw std::invalid_argument("settings.ini must be a disk file"); }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length)) { core::throw_last_error("Cannot measure settings.ini"); }
    if (length.QuadPart < 0 || static_cast<unsigned long long>(length.QuadPart) > max_config_bytes) {
        throw std::invalid_argument("settings.ini exceeds 1 MiB");
    }
    SourceFile result{std::string(static_cast<std::size_t>(length.QuadPart), '\0'), true};
    DWORD read{};
    if (!ReadFile(file.get(), result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &read, nullptr)) {
        core::throw_last_error("Cannot read settings.ini");
    }
    if (read != result.bytes.size()) { throw std::runtime_error("Incomplete settings.ini read"); }
    return result;
}

std::string update_window(std::string_view source, const WindowPlacement& window)
{
    const std::array values{std::to_string(window.x), std::to_string(window.y),
        std::to_string(window.width), std::to_string(window.height), std::string(window.maximized ? "1" : "0")};
    std::string result;
    result.reserve(source.size() + 128);
    std::size_t offset{};
    std::array<bool, window_keys.size()> seen{};
    scan(source, [&](const Entry& entry) {
        if (!equal_ascii(entry.section, "Window")) { return; }
        if (const auto index = window_key(entry.key)) {
            // save_window_config parses first, which refuses empty and
            // unassigned Window values. scan() still anchors every value in
            // the source, so the offset would stay defined without that check.
            // Debug builds assert the anchoring.
            assert(std::less_equal<>{}(source.data(), entry.value.data()) &&
                std::less_equal<>{}(entry.value.data() + entry.value.size(), source.data() + source.size()));
            seen[*index] = true;
            const auto value_offset = static_cast<std::size_t>(entry.value.data() - source.data());
            result.append(source.substr(offset, value_offset - offset));
            result += values[*index];
            offset = value_offset + entry.value.size();
        }
    });
    result.append(source.substr(offset));
    if (std::ranges::find(seen, false) != seen.end()) {
        const auto newline = source.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
        if (!result.empty() && result.back() != '\n') { result += newline; }
        result += "[Window]";
        result += newline;
        for (std::size_t index = 0; index < seen.size(); ++index) {
            if (!seen[index]) {
                result += window_keys[index];
                result += '=';
                result += values[index];
                result += newline;
            }
        }
    }
    if (result.size() > max_config_bytes) { throw std::invalid_argument("Saved settings.ini exceeds 1 MiB"); }
    return result;
}

class TemporaryConfig {
public:
    explicit TemporaryConfig(const wchar_t* destination)
    {
        static std::atomic<std::uint64_t> next{};
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            path_ = std::wstring(destination) + L".wavplay-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(next.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
            const auto raw = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (raw != INVALID_HANDLE_VALUE) {
                file_.reset(raw);
                return;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                core::throw_win32_error(error, "Cannot create temporary settings.ini");
            }
        }
        core::throw_win32_error(ERROR_FILE_EXISTS, "Cannot reserve a temporary settings.ini filename");
    }
    ~TemporaryConfig()
    {
        file_.reset();
        if (!committed_) { DeleteFileW(path_.c_str()); }
    }
    TemporaryConfig(const TemporaryConfig&) = delete;
    TemporaryConfig& operator=(const TemporaryConfig&) = delete;

    void write(std::string_view bytes)
    {
        DWORD written{};
        if (!WriteFile(file_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            core::throw_last_error("Cannot write temporary settings.ini");
        }
        if (written != bytes.size()) { throw std::runtime_error("Incomplete settings.ini write"); }
        if (!FlushFileBuffers(file_.get())) { core::throw_last_error("Cannot flush temporary settings.ini"); }
        file_.reset();
    }
    void commit(const wchar_t* destination, bool exists)
    {
        // One same-directory rename, without a copy/delete fallback. ReplaceFile
        // without a backup can remove the old file before returning error 1176.
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (exists ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(path_.c_str(), destination, flags)) { core::throw_last_error("Cannot replace settings.ini"); }
        committed_ = true;
    }

private:
    std::wstring path_;
    core::UniqueHandle file_;
    bool committed_{};
};

} // namespace

ApplicationConfig parse_application_config(std::string_view utf8, std::optional<CLSID> selected_override)
{
    validate_text(utf8);
    ApplicationConfig result;
    bool selected_seen{};
    std::array<bool, window_keys.size()> window_seen{};
    std::array<int, window_keys.size()> window_values{};
    scan(utf8, [&](const Entry& entry) {
        if (equal_ascii(entry.section, "General") && equal_ascii(entry.key, "SelectedDevice")) {
            require_unique_assignment(entry, selected_seen);
            if (!entry.value.empty()) {
                result.selected_device = asio::parse_driver_clsid(unicode(entry.value).c_str());
            }
        } else if (equal_ascii(entry.section, "Window")) {
            if (const auto index = window_key(entry.key)) {
                require_unique_assignment(entry, window_seen[*index]);
                const auto minimum = *index < key_width ? (std::numeric_limits<int>::min)() : (*index == key_maximized ? 0 : 1);
                const auto maximum = *index == key_maximized ? 1 : (std::numeric_limits<int>::max)();
                window_values[*index] = static_cast<int>(decimal_value(entry.value, minimum, maximum, entry.key));
            }
        }
    });
    if (selected_override) { result.selected_device = selected_override; }
    if (result.selected_device && IsEqualGUID(*result.selected_device, GUID{})) { result.selected_device.reset(); }
    const auto geometry_count = std::count(window_seen.begin(), window_seen.begin() + geometry_key_count, true);
    if ((geometry_count != 0 || window_seen[key_maximized]) && geometry_count != geometry_key_count) {
        throw std::invalid_argument("settings.ini Window requires X, Y, Width and Height together");
    }
    if (geometry_count == geometry_key_count) {
        result.window = WindowPlacement{window_values[key_x], window_values[key_y], window_values[key_width],
            window_values[key_height], window_values[key_maximized] != 0};
        validate_window(*result.window);
    }
    if (!result.selected_device) { return result; }
    const auto wanted = selected_section(*result.selected_device);
    std::array<bool, 4> device_seen{};
    scan(utf8, [&](const Entry& entry) {
        if (!equal_ascii(entry.section, wanted)) { return; }
        if (equal_ascii(entry.key, "BufferSize")) {
            require_unique_assignment(entry, device_seen[0]);
            result.device.buffer_frames = decimal_value(entry.value, 0, (std::numeric_limits<long>::max)(), entry.key);
        } else if (equal_ascii(entry.key, "StartSilence")) {
            require_unique_assignment(entry, device_seen[1]);
            result.device.start_silence = std::chrono::milliseconds(decimal_value(entry.value, 0,
                static_cast<long>(asio::max_start_silence.count()), entry.key));
        } else if (equal_ascii(entry.key, "OutputPair")) {
            require_unique_assignment(entry, device_seen[2]);
            const auto pair = decimal_value(entry.value, 1, (std::numeric_limits<long>::max)() - 2, entry.key);
            if (pair % 2 == 0) { throw std::invalid_argument("OutputPair must be an odd first channel number (1, 3, 5, ...)"); }
            result.device.output_pair = pair;
        } else if (equal_ascii(entry.key, "Name")) {
            require_unique_assignment(entry, device_seen[3]);
            result.device.name = unicode(entry.value);
        }
    });
    return result;
}

ApplicationConfig load_application_config(const wchar_t* path, std::optional<CLSID> selected_override)
{
    // The executable and its settings may live on removable media. Apply the
    // same no-dialog policy as WAV and playlist I/O, only on this caller's
    // thread and only while our own operation runs.
    const core::CriticalErrorScope critical_errors;
    return parse_application_config(read_config(path).bytes, selected_override);
}

asio::DeviceSettings DeviceConfig::apply(asio::DeviceSettings defaults) const
{
    // Override in place: a DeviceSettings field this config does not set
    // keeps the caller's value.
    defaults.start_silence = start_silence.value_or(defaults.start_silence);
    defaults.buffer_frames = buffer_frames.value_or(defaults.buffer_frames);
    defaults.output_pair = output_pair.value_or(defaults.output_pair);
    return defaults;
}

void save_window_config(const wchar_t* path, const WindowPlacement& window)
{
    // Cover the whole transaction, including flush, rename and the temporary
    // file's cleanup during unwinding. Never change the ASIO driver's mode.
    const core::CriticalErrorScope critical_errors;
    validate_window(window);
    auto source = read_config(path);
    (void)parse_application_config(source.bytes); // Never silently repair malformed user settings on exit.
    if (!source.exists) { source.bytes = "[General]\nSelectedDevice=\n"; }
    const auto updated = update_window(source.bytes, window);
    if (source.exists && updated == source.bytes) { return; }
    TemporaryConfig temporary(path);
    temporary.write(updated);
    temporary.commit(path, source.exists);
}

} // namespace wavplay::settings
