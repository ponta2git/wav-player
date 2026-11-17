#include "wavplay/core/memory.h"

#include <atomic>
#include <limits>
#include <stdexcept>

// The linker's symbol at the base address of the module that contains this code.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace wavplay::core {
namespace {

// Not a function-local static: its thread-safe initialization would give the
// image a TLS directory, and every thread of the process (the driver's too) a
// TLS block. Racing first calls store the same value.
constinit std::atomic<std::size_t> cached_page_size{};

// Working-set limits are process-wide, so concurrent allocations serialize their
// read-modify-write. An SRW lock needs no destruction, unlike a static std::mutex.
constinit SRWLOCK working_set_guard = SRWLOCK_INIT;

// Moves both limits by `bytes`, so the minimum stays at or below the maximum,
// and keeps the hard/soft flags as they are. Shrinking never underflows.
bool adjust_working_set(std::size_t bytes, bool grow) noexcept
{
    const auto process = GetCurrentProcess();
    AcquireSRWLockExclusive(&working_set_guard);
    SIZE_T minimum{}, maximum{};
    DWORD flags{};
    bool adjusted = GetProcessWorkingSetSizeEx(process, &minimum, &maximum, &flags) != FALSE;
    constexpr auto limit = std::numeric_limits<SIZE_T>::max();
    adjusted = adjusted && (grow ? minimum <= limit - bytes && maximum <= limit - bytes :
        minimum >= bytes && maximum >= bytes);
    if (adjusted) {
        minimum = grow ? minimum + bytes : minimum - bytes;
        maximum = grow ? maximum + bytes : maximum - bytes; // maximum >= minimum >= bytes before shrinking.
        adjusted = SetProcessWorkingSetSizeEx(process, minimum, maximum, flags) != FALSE;
    }
    ReleaseSRWLockExclusive(&working_set_guard);
    return adjusted;
}

// Locks [pages, pages + bytes), whole pages, into the working set. VirtualLock
// counts against the minimum working set, so grow it first. A refusal (quota,
// privilege, memory or an inaccessible page) leaves the limits as they were and
// the pages correct but pageable.
bool lock_pages(void* pages, std::size_t bytes) noexcept
{
    if (!adjust_working_set(bytes, true)) {
        return false;
    }
    if (VirtualLock(pages, bytes)) {
        return true;
    }
    (void)adjust_working_set(bytes, false);
    return false;
}

// The process's one image lock attempt and its result.
constinit std::atomic<bool> image_lock_attempted{};
constinit std::atomic<bool> image_locked{};

} // namespace

std::size_t page_size() noexcept
{
    auto size = cached_page_size.load(std::memory_order_relaxed);
    if (size == 0) {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        size = system.dwPageSize;
        cached_page_size.store(size, std::memory_order_relaxed);
    }
    return size;
}

PageAllocation::PageAllocation(std::size_t bytes)
{
    if (bytes == 0) {
        throw std::invalid_argument("Page allocation must not be empty");
    }
    const auto page = page_size();
    if (bytes > std::numeric_limits<std::size_t>::max() - (page - 1)) {
        throw std::length_error("Page allocation size overflows page rounding");
    }
    const auto committed_size = align_up(bytes, page);
    pages_ = decltype(pages_)(static_cast<std::byte*>(
        VirtualAlloc(nullptr, committed_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)),
        Release{bytes, committed_size});
    if (!pages_) {
        throw_last_error("VirtualAlloc");
    }
    // Both Windows APIs operate on complete pages. Using the requested byte
    // count for quota changes can leak or consume a page on every release.
    pages_.get_deleter().locked = lock_pages(pages_.get(), committed_size);
}

void PageAllocation::Release::operator()(std::byte* pages) const noexcept
{
    if (locked) {
        VirtualUnlock(pages, committed_size);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    if (locked) {
        (void)adjust_working_set(committed_size, false);
    }
}

bool lock_process_image() noexcept
{
    if (image_lock_attempted.exchange(true)) {
        return image_locked.load();
    }
    // The loader maps the whole image, the headers and every section, committed
    // and readable. SizeOfImage already is a multiple of the section alignment,
    // which is the page size here.
    auto* const base = reinterpret_cast<std::byte*>(&__ImageBase);
    const auto& headers = *reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    const bool locked = lock_pages(base, align_up(headers.OptionalHeader.SizeOfImage, page_size()));
    image_locked.store(locked);
    return locked;
}

bool process_image_locked() noexcept
{
    return image_locked.load();
}

} // namespace wavplay::core
