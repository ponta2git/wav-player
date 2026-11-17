#include "wavplay/media/read_only_file.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace wavplay::media {
namespace {

constexpr DWORD share_mode = FILE_SHARE_READ;
constexpr DWORD access_flags = FILE_FLAG_SEQUENTIAL_SCAN;

// DOS absolute paths need extended syntax both for long names and to keep a
// trailing space/dot literal. Normalize only separators and explicit . / ..
// components; GetFullPathNameW would also strip trailing filename spaces.
// Relative and already extended paths retain their existing Windows semantics.
std::wstring absolute_disk_path(std::wstring_view path)
{
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/')) { return {}; }
    std::wstring result = L"\\\\?\\";
    result.reserve(path.size() + 4);
    result.append(path.substr(0, 2));
    result += L'\\';
    auto remaining = path.substr(3);
    while (!remaining.empty()) {
        const auto end = remaining.find_first_of(L"\\/");
        const auto component = remaining.substr(0, end);
        if (end == std::wstring_view::npos) { remaining = {}; }
        else { remaining.remove_prefix(end + 1); }
        if (component.empty() || component == L".") { continue; }
        if (component == L"..") {
            if (result.size() > 7) {
                const auto separator = result.find_last_of(L'\\');
                result.resize(separator < 7 ? 7 : separator);
            }
        } else {
            if (result.back() != L'\\') { result += L'\\'; }
            result.append(component);
        }
    }
    if ((path.back() == L'\\' || path.back() == L'/') && result.back() != L'\\') { result += L'\\'; }
    return result;
}

std::wstring final_file_path(HANDLE file, DWORD volume_format)
{
    const auto flags = volume_format | FILE_NAME_OPENED;
    const auto needed = GetFinalPathNameByHandleW(file, nullptr, 0, flags);
    if (!needed) { return {}; }
    std::wstring final_path(needed, L'\0');
    const auto length = GetFinalPathNameByHandleW(file, final_path.data(), needed, flags);
    if (!length) { return {}; }
    if (length >= needed) { throw std::runtime_error("Opened file path changed while resolving its volume"); }
    final_path.resize(length);
    return final_path;
}

void require_local_volume(HANDLE file)
{
    // Inspect the opened object, not just the input drive: a local symbolic link
    // can resolve to a remote file. Prefer DOS names; use the GUID form when the
    // local volume has no DOS mount. FILE_NAME_OPENED avoids a redundant path
    // normalization/access check after CreateFile already resolved the file.
    auto final_path = final_file_path(file, VOLUME_NAME_DOS);
    if (final_path.empty()) { final_path = final_file_path(file, VOLUME_NAME_GUID); }
    if (final_path.empty()) { core::throw_last_error("Resolve file on a local volume"); }
    std::size_t root_size{};
    if (final_path.size() >= 7 && final_path.starts_with(L"\\\\?\\") && final_path[5] == L':' &&
        final_path[6] == L'\\') {
        root_size = 7;
    } else if (final_path.starts_with(L"\\\\?\\Volume{")) {
        const auto root_end = final_path.find(L'\\', 4);
        if (root_end != std::wstring::npos) { root_size = root_end + 1; }
    }
    // UNC paths cannot become a local root; the GUID fallback is also unavailable
    // for network shares by the GetFinalPathNameByHandle contract.
    if (!root_size) { throw std::runtime_error("Expected a file on a local volume"); }
    final_path.resize(root_size);
    if (!is_local_drive_type(GetDriveTypeW(final_path.c_str()))) {
        throw std::runtime_error("Expected a file on a local volume");
    }
}

} // namespace

ReadOnlyFile::ReadOnlyFile(const wchar_t* path)
{
    if (!path) { throw std::invalid_argument("A file path is required"); }
    // A drive without media fails the open with an error instead of a dialog.
    const core::CriticalErrorScope critical_errors;
    const auto absolute = absolute_disk_path(path);
    const auto file = CreateFileW(absolute.empty() ? path : absolute.c_str(),
        GENERIC_READ, share_mode, nullptr, OPEN_EXISTING, access_flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        core::throw_last_error("CreateFileW");
    }
    handle_.reset(file);
    // Pipes, consoles and devices also open with a path; only disk files can be
    // validated once and then read in place.
    if (GetFileType(file) != FILE_TYPE_DISK) {
        throw std::runtime_error("Not a disk file");
    }
    require_local_volume(file);
}

ReadOnlyFile ReadOnlyFile::reopen() const
{
    const core::CriticalErrorScope critical_errors;
    const auto file = ReOpenFile(handle_.get(), GENERIC_READ, share_mode, access_flags);
    if (file == INVALID_HANDLE_VALUE) {
        core::throw_last_error("ReOpenFile");
    }
    return ReadOnlyFile(core::UniqueHandle(file));
}

std::uint64_t ReadOnlyFile::size() const
{
    const core::CriticalErrorScope critical_errors;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_.get(), &size)) {
        core::throw_last_error("GetFileSizeEx");
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

void ReadOnlyFile::read_exact_at(std::uint64_t offset, std::span<std::byte> buffer)
{
    if (buffer.size() > MAXDWORD) {
        throw std::invalid_argument("A single file read cannot exceed 4 GiB");
    }
    // Windows file positions are signed. Offset -2 is FILE_USE_FILE_POINTER_POSITION,
    // which would read at the handle's file pointer instead of failing.
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("A file offset cannot exceed the signed 64-bit range");
    }
    if (buffer.empty()) {
        return; // Nothing to fill, even at or beyond the end of the file.
    }
    // On a synchronous handle an OVERLAPPED offset selects the position and the
    // call still completes before returning; no event is needed. The cache
    // manager tracks read-ahead by request offset, so sequential scans keep it.
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD count{};
    const core::CriticalErrorScope critical_errors; // Media removed after the open.
    if (!ReadFile(handle_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, &position)) {
        const auto error = GetLastError();
        // An explicit offset at or past the end fails with ERROR_HANDLE_EOF instead
        // of reading zero bytes. ReadFile has set count to zero, so the short-read
        // check below reports it.
        if (error != ERROR_HANDLE_EOF) {
            core::throw_win32_error(error, "ReadFile");
        }
    }
    if (count != buffer.size()) {
        throw std::runtime_error("Unexpected end of file");
    }
}

} // namespace wavplay::media
