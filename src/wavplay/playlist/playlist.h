#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wavplay::playlist {

// An immutable startup snapshot. Paths keep their original spelling, order and
// duplicates; playback opens and validates them again when a request starts.
struct Playlist {
    std::vector<std::wstring> paths;
    std::size_t excluded_count{};
};

// Validate the complete UTF-8 text before opening any entries. Empty lines and
// lines starting with '#' do not count as excluded. All other lines must name
// an existing, parseable, nonempty WAV on a local volume through an absolute path.
// Only CR/LF delimiters are removed: spaces and tabs are never trimmed.
[[nodiscard]] Playlist parse(std::string_view utf8);

// Read once. File errors (including a missing playlist) and invalid UTF-8 throw;
// an empty file is a valid empty playlist. No file handles remain in the result.
[[nodiscard]] Playlist load(const wchar_t* path);

// The same parse and load, cancellable by another thread setting `stop`. It is
// read before each entry is validated (never on blank or comment lines); once
// it is seen set, the result has no value (std::nullopt), never a valid prefix,
// and nothing is thrown for it. An entry already being validated completes
// first, however long its WAV takes to scan, so a stop raised during the last
// entry still returns the complete list, whatever ends that entry's line.
// A lock-free flag rather than std::stop_token: std::stop_source::request_stop
// links the STL's WakeByAddressAll and WaitOnAddress imports.
[[nodiscard]] std::optional<Playlist> parse(std::string_view utf8, const std::atomic<bool>& stop);
[[nodiscard]] std::optional<Playlist> load(const wchar_t* path, const std::atomic<bool>& stop);

} // namespace wavplay::playlist
