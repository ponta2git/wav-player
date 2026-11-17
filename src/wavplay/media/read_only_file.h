#pragma once

#include "wavplay/core/win32.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace wavplay::media {

// Whether a GetDriveTypeW result names a local volume: fixed, removable,
// optical or RAM disk. Remote, unknown and missing roots are not local. The
// playlist's pre-check and ReadOnlyFile's check of the opened file share it.
[[nodiscard]] constexpr bool is_local_drive_type(UINT type) noexcept
{
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK;
}

// A local file opened for synchronous, cached, sequential reads at explicit
// offsets; the handle's file pointer is never a read position. Other readers may
// share it; writers and deletion are denied while any handle stays open. Opening,
// reopening, size queries and reads run in a core::CriticalErrorScope, so a drive
// without media fails the call instead of blocking the thread in a system dialog.
// Storage tuning (unbuffered or overlapped I/O, request sizes, read-ahead hints)
// starts here.
class ReadOnlyFile {
public:
    explicit ReadOnlyFile(const wchar_t* path);

    // Another handle to the same protected file with the same sharing and access
    // flags. Positional reads share no file position between handles.
    [[nodiscard]] ReadOnlyFile reopen() const;

    [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(handle_); }
    void close() noexcept { handle_.reset(); }

    [[nodiscard]] std::uint64_t size() const;

    // Fill the whole buffer from an absolute file offset or throw. The read starts
    // at the caller's offset; it leaves the handle's file pointer after the data
    // read, and nothing relies on that pointer. Reaching the end of the file
    // before the buffer is full is an unexpected end of data. The offset must be
    // a signed 64-bit file position and each request must fit in a DWORD.
    void read_exact_at(std::uint64_t offset, std::span<std::byte> buffer);
    template<class T, std::size_t N>
    void read_exact_at(std::uint64_t offset, std::span<T, N> buffer)
    {
        read_exact_at(offset, std::as_writable_bytes(std::span<T>(buffer)));
    }

private:
    explicit ReadOnlyFile(core::UniqueHandle handle) noexcept : handle_(std::move(handle)) {}
    core::UniqueHandle handle_;
};

} // namespace wavplay::media
