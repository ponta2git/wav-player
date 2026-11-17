#include "wavplay/playlist/playlist.h"

#include "wavplay/core/win32.h"
#include "wavplay/media/read_only_file.h"
#include "wavplay/media/wav_reader.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace wavplay::playlist {
namespace {

std::wstring decode(std::string_view utf8)
{
    if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("playlist.txt exceeds the UTF-8 decoder's size limit");
    }
    if (utf8.starts_with("\xef\xbb\xbf")) { utf8.remove_prefix(3); }
    auto text = core::utf8_to_wide(utf8);
    if (!text) { throw std::invalid_argument("playlist.txt must contain valid UTF-8 text"); }
    return std::move(*text);
}

bool drive_letter(wchar_t value) noexcept
{
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

// Accept DOS drive-absolute paths, their extended-length spelling, and local
// volume GUID paths. UNC, drive-relative, root-relative and device namespaces
// must be rejected before querying a volume or opening a file.
std::size_t absolute_root(std::wstring_view path) noexcept
{
    if (path.find(L'\0') != std::wstring_view::npos) { return 0; }
    if (path.size() >= 3 && drive_letter(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) { return 3; }
    if (!path.starts_with(L"\\\\?\\")) { return 0; }
    if (path.size() >= 7 && drive_letter(path[4]) && path[5] == L':' && path[6] == L'\\') { return 7; }
    constexpr std::size_t guid_begin = 11, guid_length = 36;
    constexpr auto root_length = guid_begin + guid_length + 2;
    if (path.size() < root_length ||
        CompareStringOrdinal(path.data() + 4, 7, L"Volume{", 7, TRUE) != CSTR_EQUAL ||
        path[guid_begin + guid_length] != L'}' || path[root_length - 1] != L'\\') { return 0; }
    for (std::size_t index = 0; index < guid_length; ++index) {
        const auto value = path[guid_begin + index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != L'-') { return 0; }
        } else if (!((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
            (value >= L'A' && value <= L'F'))) { return 0; }
    }
    return root_length;
}

// The same drive types that ReadOnlyFile later requires of the opened file.
bool local_volume(const wchar_t* root) noexcept
{
    return media::is_local_drive_type(GetDriveTypeW(root));
}

bool playable_file(std::wstring_view path)
{
    const auto root_length = absolute_root(path);
    if (!root_length || path.size() >= 32768) { return false; }
    // An empty card reader or optical drive excludes the entry instead of
    // raising the system's no-disk dialog on this thread.
    const core::CriticalErrorScope critical_errors;
    std::wstring root(path.substr(0, root_length));
    std::ranges::replace(root, L'/', L'\\');
    if (!local_volume(root.c_str())) { return false; }

    const std::wstring file_path(path);
    // Follow volume mount points/junctions as well as the drive's initial root.
    // Querying a remote mapped drive was already excluded above.
    std::wstring volume(path.size() + 1, L'\0');
    if (!GetVolumePathNameW(file_path.c_str(), volume.data(), static_cast<DWORD>(volume.size())) ||
        !local_volume(volume.c_str())) { return false; }
    try {
        const media::WavReader validated(file_path.c_str());
        return validated.info().frame_count != 0;
    } catch (const std::runtime_error&) {
        // Missing, inaccessible and malformed media count as excluded entries.
        // Allocation failures still abort loading instead of hiding exhaustion.
        return false;
    }
}

// The flag of the uncancellable overloads, which nothing sets.
constinit const std::atomic<bool> never_stopped{false};

} // namespace

std::optional<Playlist> parse(std::string_view utf8, const std::atomic<bool>& stop)
{
    const auto text = decode(utf8); // Never publish a valid prefix of invalid text.
    std::wstring_view remaining(text);
    Playlist result;
    // Repeated paths remain repeated rows, but a large PCM24/32 file only needs
    // one startup validation. Views refer to the stable decoded text above.
    std::unordered_map<std::wstring_view, bool> validated;
    while (!remaining.empty()) {
        const auto end = remaining.find_first_of(L"\r\n");
        const auto line = remaining.substr(0, end);
        if (end == std::wstring_view::npos) { remaining = {}; }
        else { remaining.remove_prefix(end + 1); }
        if (line.empty() || line.front() == L'#') { continue; }
        // Read before each entry only, so blank lines, comments and the LF of
        // a CRLF never decide whether a stop during the last entry returns the
        // list. Only a hint to return early, ordering no other data: relaxed.
        if (stop.load(std::memory_order_relaxed)) { return std::nullopt; }
        const auto [entry, inserted] = validated.try_emplace(line, false);
        if (inserted) { entry->second = playable_file(line); }
        if (entry->second) { result.paths.emplace_back(line); }
        else { ++result.excluded_count; }
    }
    return result;
}

std::optional<Playlist> load(const wchar_t* path, const std::atomic<bool>& stop)
{
    media::ReadOnlyFile file(path);
    const auto size = file.size();
    if (size > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("playlist.txt exceeds the UTF-8 decoder's size limit");
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    file.read_exact_at(0, std::span(utf8));
    return parse(utf8, stop);
}

Playlist parse(std::string_view utf8)
{
    return parse(utf8, never_stopped).value();
}

Playlist load(const wchar_t* path)
{
    return load(path, never_stopped).value();
}

} // namespace wavplay::playlist
