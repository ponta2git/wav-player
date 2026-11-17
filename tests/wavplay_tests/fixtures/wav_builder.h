#pragma once

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wavplay_test {

using Bytes = std::vector<std::uint8_t>;

struct Chunk {
    std::string_view id;
    Bytes payload;
};

inline std::array<std::uint8_t, 4> le32(std::uint32_t value)
{
    std::array<std::uint8_t, 4> bytes;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
    return bytes;
}

inline std::array<std::uint8_t, 8> le64(std::uint64_t value)
{
    std::array<std::uint8_t, 8> bytes;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
    return bytes;
}

struct Ds64Entry {
    std::string_view id;
    std::uint64_t size;
};

inline Chunk ds64(std::uint64_t riff_size, std::uint64_t data_size, std::uint64_t sample_count,
    std::initializer_list<Ds64Entry> table = {})
{
    Bytes payload;
    payload.append_range(le64(riff_size));
    payload.append_range(le64(data_size));
    payload.append_range(le64(sample_count));
    payload.append_range(le32(static_cast<std::uint32_t>(table.size())));
    for (const auto& entry : table) {
        if (entry.id.size() != 4) { throw std::invalid_argument("ds64 table ID must contain four bytes"); }
        payload.append_range(entry.id);
        payload.append_range(le64(entry.size));
    }
    return {"ds64", std::move(payload)};
}

// Small, fully populated RF64 files for integration tests. Large/sparse files
// use ds64() and explicit chunk headers so no logical size becomes a Bytes size.
inline Bytes rf64(std::initializer_list<Chunk> chunks, std::uint64_t sample_count)
{
    Bytes file{'R', 'F', '6', '4', 0xff, 0xff, 0xff, 0xff, 'W', 'A', 'V', 'E'};
    const auto append = [&](const Chunk& chunk, bool sentinel) {
        if (chunk.id.size() != 4) { throw std::invalid_argument("RF64 chunk ID must contain four bytes"); }
        file.append_range(chunk.id);
        file.append_range(le32(sentinel ? 0xffffffffU : static_cast<std::uint32_t>(chunk.payload.size())));
        file.append_range(chunk.payload);
        if (chunk.payload.size() % 2 != 0) { file.push_back(0); }
    };
    const auto data = std::ranges::find(chunks, std::string_view("data"), &Chunk::id);
    append(ds64(0, data == chunks.end() ? 0 : data->payload.size(), sample_count), false);
    for (const auto& chunk : chunks) { append(chunk, chunk.id == "data"); }
    std::ranges::copy(le64(file.size() - 8), file.begin() + 20);
    return file;
}

inline Bytes wave(std::initializer_list<Chunk> chunks)
{
    Bytes file{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    for (const auto& [id, payload] : chunks) {
        file.append_range(id);
        file.append_range(le32(static_cast<std::uint32_t>(payload.size())));
        file.append_range(payload);
        if (payload.size() % 2 != 0) {
            file.push_back(0);
        }
    }
    std::ranges::copy(le32(static_cast<std::uint32_t>(file.size() - 8)), file.begin() + 4);
    return file;
}

inline Chunk pcm_integer(std::uint8_t channels, std::uint32_t rate, std::uint8_t bits)
{
    Bytes fmt{1, 0, channels, 0};
    fmt.append_range(le32(rate));
    const auto block_align = static_cast<std::uint8_t>(bits / 8 * channels);
    fmt.append_range(le32(rate * block_align));
    fmt.insert(fmt.end(), {block_align, 0, bits, 0});
    return {"fmt ", std::move(fmt)};
}

inline Chunk pcm16(std::uint8_t channels, std::uint32_t rate)
{
    return pcm_integer(channels, rate, 16);
}

inline Chunk pcm24(std::uint8_t channels, std::uint32_t rate)
{
    return pcm_integer(channels, rate, 24);
}

inline Chunk pcm32(std::uint8_t channels, std::uint32_t rate)
{
    return pcm_integer(channels, rate, 32);
}

inline Chunk extensible_pcm(std::uint8_t channels, std::uint32_t rate, std::uint8_t valid_bits,
    std::uint8_t container_bits, std::uint32_t channel_mask = 0)
{
    auto fmt = pcm_integer(channels, rate, container_bits);
    fmt.payload[0] = 0xfe;
    fmt.payload[1] = 0xff;
    fmt.payload.insert(fmt.payload.end(), {22, 0, valid_bits, 0});
    fmt.payload.append_range(le32(channel_mask));
    // KSDATAFORMAT_SUBTYPE_PCM, serialized in the GUID's Windows byte order.
    fmt.payload.insert(fmt.payload.end(), {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71});
    return fmt;
}

inline Chunk float32(std::uint8_t channels, std::uint32_t rate, bool waveformat_ex = true)
{
    auto fmt = pcm_integer(channels, rate, 32);
    fmt.payload[0] = 3;
    if (waveformat_ex) { fmt.payload.insert(fmt.payload.end(), {0, 0}); }
    return fmt;
}

inline Chunk extensible_float32(std::uint8_t channels, std::uint32_t rate, std::uint32_t channel_mask = 0)
{
    auto fmt = extensible_pcm(channels, rate, 32, 32, channel_mask);
    fmt.payload[24] = 3; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; the other GUID bytes are identical.
    return fmt;
}

inline Chunk fact(std::uint32_t frames)
{
    const auto bytes = le32(frames);
    return {"fact", Bytes(bytes.begin(), bytes.end())};
}

// IEEE binary32 words, never evaluated by the floating-point unit. After the
// special values every sample is finite and nonzero, including the final frame.
inline std::uint32_t float32_sample_bits(std::size_t frame, std::size_t channel = 0)
{
    constexpr std::array<std::uint32_t, 18> edges{
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
        0x00800000, 0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7fc12345, 0xffc23456, 0x7f812345, 0xff823456};
    const auto raw = frame < edges.size() ? edges[frame] :
        0x3e800001U | (static_cast<std::uint32_t>(frame * 0x12345U) & 0x007fffffU);
    return channel == 0 ? raw : raw ^ 0x80000000U;
}

inline Chunk float32_ramp(std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    Bytes samples(frames * channels * 4);
    std::size_t offset{};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto bytes = le32(float32_sample_bits(start + frame, channel));
            for (const auto byte : bytes) { samples[offset++] = byte; }
        }
    }
    return {"data", std::move(samples)};
}

// Little-endian frame indices on the left and their bitwise complements on the right.
inline Chunk pcm16_ramp(std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    Bytes samples(frames * channels * 2);
    std::size_t offset = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            const auto bits = channel == 0 ? start + frame : ~(start + frame);
            samples[offset++] = static_cast<std::uint8_t>(bits);
            samples[offset++] = static_cast<std::uint8_t>(bits >> 8);
        }
    }
    return {"data", std::move(samples)};
}

// The same frame indices and complements as pcm16_ramp, retaining all 24 bits.
inline Chunk pcm24_ramp(std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    Bytes samples(frames * channels * 3);
    std::size_t offset = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            const auto bits = channel == 0 ? start + frame : ~(start + frame);
            for (unsigned byte = 0; byte < 3; ++byte) {
                samples[offset++] = static_cast<std::uint8_t>(bits >> (8 * byte));
            }
        }
    }
    return {"data", std::move(samples)};
}

// Full-width signed PCM32 values; byte generation never narrows through float.
inline Chunk pcm32_ramp(std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    Bytes samples(frames * channels * 4);
    std::size_t offset = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            const auto bits = channel == 0 ? start + frame : ~(start + frame);
            for (unsigned byte = 0; byte < 4; ++byte) {
                samples[offset++] = static_cast<std::uint8_t>(bits >> (8 * byte));
            }
        }
    }
    return {"data", std::move(samples)};
}

// Extensible PCM24 in a 32-bit container: valid bits are MSB-aligned.
inline Chunk pcm24_in32_ramp(std::uint8_t channels, std::size_t frames, std::size_t start = 0)
{
    Bytes samples(frames * channels * 4);
    std::size_t offset = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            const auto bits = channel == 0 ? start + frame : ~(start + frame);
            samples[offset++] = 0;
            for (unsigned byte = 0; byte < 3; ++byte) {
                samples[offset++] = static_cast<std::uint8_t>(bits >> (8 * byte));
            }
        }
    }
    return {"data", std::move(samples)};
}

struct SparseRegion {
    std::uint64_t offset;
    Bytes bytes;
};

// This process's own directory for TempFile names. Suites run as parallel
// processes, and GetTempFileNameW numbers names from the clock and takes the
// next free one. In a shared directory, a name that a test deletes to make its
// path missing could be claimed and written by another suite while the test
// still relies on its absence. Removed at exit when empty.
class FixtureDirectory {
public:
    FixtureDirectory()
    {
        std::array<wchar_t, MAX_PATH> temp;
        const auto length = GetTempPath2W(static_cast<DWORD>(temp.size()), temp.data());
        if (length == 0) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetTempPath2W");
        }
        if (length >= temp.size()) {
            throw std::runtime_error("Temporary directory path exceeds MAX_PATH");
        }
        path_.assign(temp.data(), length);
        path_ += L"wavplay_tests." + std::to_wstring(GetCurrentProcessId());
        // GetTempFileNameW accepts a directory of at most MAX_PATH - 14 characters.
        if (path_.size() > MAX_PATH - 14) {
            throw std::runtime_error("Fixture directory path exceeds the GetTempFileNameW limit");
        }
        // A live process ID is unique. A directory left by an ended process
        // with the same ID is reused; GetTempFileNameW skips its names.
        if (!CreateDirectoryW(path_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateDirectoryW");
        }
    }
    ~FixtureDirectory() { RemoveDirectoryW(path_.c_str()); }
    FixtureDirectory(const FixtureDirectory&) = delete;
    FixtureDirectory& operator=(const FixtureDirectory&) = delete;

    [[nodiscard]] const wchar_t* path() const noexcept { return path_.c_str(); }

private:
    std::wstring path_;
};

class TempFile {
public:
    TempFile()
    {
        static const FixtureDirectory directory;
        if (!GetTempFileNameW(directory.path(), L"wav", 0, path_.data())) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetTempFileNameW");
        }
    }

    ~TempFile() { DeleteFileW(path_.data()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const wchar_t* write(const Bytes& bytes) const
    {
        std::FILE* stream{};
        const auto error = _wfopen_s(&stream, path_.data(), L"wb");
        if (!stream) {
            throw std::system_error(error, std::generic_category(), "open WAV fixture");
        }
        const bool complete = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size();
        const int closed = std::fclose(stream);
        if (!complete || closed != 0) {
            throw std::runtime_error("Could not write the complete WAV fixture");
        }
        return path_.data();
    }

    const wchar_t* write_sparse(std::uint64_t logical_size, std::initializer_list<SparseRegion> regions) const
    {
        constexpr std::uint64_t allocation_limit = 1024 * 1024;
        if (logical_size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            throw std::invalid_argument("Sparse WAV size exceeds the Windows file offset range");
        }
        for (const auto& region : regions) {
            if (region.offset > logical_size || region.bytes.size() > logical_size - region.offset ||
                region.bytes.size() > allocation_limit) {
                throw std::invalid_argument("Sparse WAV write lies outside its bounded fixture");
            }
        }
        const auto handle = CreateFileW(path_.data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create sparse WAV");
        }
        const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
        DWORD returned{};
        if (!DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "mark WAV sparse");
        }
        const auto seek = [&](std::uint64_t offset) {
            LARGE_INTEGER position;
            position.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "seek sparse WAV");
            }
        };
        seek(logical_size);
        if (!SetEndOfFile(file.get())) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "size sparse WAV");
        }
        for (const auto& region : regions) {
            seek(region.offset);
            DWORD written{};
            if (!WriteFile(file.get(), region.bytes.data(), static_cast<DWORD>(region.bytes.size()), &written, nullptr) ||
                written != region.bytes.size()) {
                throw std::runtime_error("Could not write the sparse WAV region");
            }
        }
        FILE_STANDARD_INFO info{};
        if (!GetFileInformationByHandleEx(file.get(), FileStandardInfo, &info, sizeof(info))) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "query sparse WAV allocation");
        }
        if (info.AllocationSize.QuadPart < 0 || static_cast<std::uint64_t>(info.AllocationSize.QuadPart) > allocation_limit) {
            throw std::runtime_error("Sparse WAV unexpectedly occupies more than 1 MiB of disk space");
        }
        return path_.data();
    }

private:
    std::array<wchar_t, MAX_PATH> path_{};
};

// OPEN_EXISTING requests access without changing the file or marking it for deletion.
inline DWORD probe_file_access(const wchar_t* path, DWORD access)
{
    const auto handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (!CloseHandle(handle)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "close probe handle");
    }
    return ERROR_SUCCESS;
}

} // namespace wavplay_test
