#pragma once

#include "wavplay/core/alignment.h"
#include "wavplay/core/win32.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace wavplay::core {

// Storage for a process-lifetime object that is never destroyed. Threads that
// outlive static destruction (driver callbacks, a late controller) can keep using
// it; the OS reclaims its resources at exit. Alignment is that of T.
template<class T>
union NoDestroy {
    T value;
    constexpr NoDestroy() : value() {}
    ~NoDestroy() {}
    NoDestroy(const NoDestroy&) = delete;
    NoDestroy& operator=(const NoDestroy&) = delete;
};

// The system page size (GetSystemInfo), queried once. VirtualAlloc, VirtualLock
// and the working-set quota all count whole pages of this size.
[[nodiscard]] std::size_t page_size() noexcept;

// Locks the image of this executable into the working set for the rest of the
// process: [__ImageBase, __ImageBase + SizeOfImage), that is the headers and
// every section, among them code, read-only data (import address table, jump and
// CRT tables), data, unwind tables, resources and base relocations. As for
// PageAllocation, the working-set limits grow by the image size first; if
// Windows refuses either step, the growth is undone and the image stays
// pageable. Data pages first written later keep the lock in their private
// copy. Never unlocked. Other modules (the ASIO driver, system DLLs), the
// kernel, stacks and heaps are not covered. Only the first call acts, and it
// can read the image from disk; later calls return its result. Call it once at
// startup, before threads that rely on the result exist.
bool lock_process_image() noexcept;
// Whether lock_process_image locked the image of this process.
[[nodiscard]] bool process_image_locked() noexcept;

// Committed, zero-filled pages owned as one allocation, for data a real-time
// consumer reads. Page granularity keeps the start page- and cache-line-aligned.
// The pages are locked into the working set, so trimming under memory pressure
// cannot turn a read into a page fault: the process minimum working set (the lock
// quota) grows by the page-rounded allocation for its lifetime. If Windows refuses either step,
// the pages stay pageable and locked() is false. Placement policies such as large
// pages or NUMA affinity belong here too, not in the buffer's users.
class PageAllocation {
public:
    PageAllocation() = default;
    explicit PageAllocation(std::size_t bytes);

    // Moving transfers ownership; an empty or moved-from allocation reports 0/false.
    [[nodiscard]] std::byte* data() const noexcept { return pages_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return pages_ ? pages_.get_deleter().size : 0; }
    [[nodiscard]] std::size_t committed_size() const noexcept { return pages_ ? pages_.get_deleter().committed_size : 0; }
    [[nodiscard]] bool locked() const noexcept { return pages_ && pages_.get_deleter().locked; }

private:
    // The deleter carries what release must undo, so moves keep it with the pages.
    struct Release {
        std::size_t size{};
        std::size_t committed_size{};
        bool locked{};
        void operator()(std::byte* pages) const noexcept;
    };
    std::unique_ptr<std::byte, Release> pages_;
};

// A stable object in private pages, never pages shared with unrelated CRT heap
// allocations. Construction and destruction run outside real-time callbacks;
// a refused working-set lock preserves ordinary C++ lifetime and is reported by
// locked(). This locks the object itself, not allocations it owns or points to.
// The forwarding constructor requires std::constructible_from, which also
// requires a non-throwing destructor for the noexcept ~ResidentObject. It
// competes with the deleted copy constructor only for a T constructible from a
// ResidentObject<T>&, which no current T is. Instantiate it only where T is complete.
template<class T>
class ResidentObject {
public:
    template<class... Args>
        requires std::constructible_from<T, Args...>
    explicit ResidentObject(Args&&... args)
        : pages_(storage_size())
    {
        void* storage = pages_.data();
        auto available = pages_.size();
        auto* aligned = static_cast<T*>(std::align(alignof(T), sizeof(T), storage, available));
        if (!aligned) {
            throw std::bad_alloc(); // storage_size() reserves the full alignment slack.
        }
        object_ = std::construct_at(aligned, std::forward<Args>(args)...);
    }

    ~ResidentObject() noexcept { std::destroy_at(object_); }
    ResidentObject(const ResidentObject&) = delete;
    ResidentObject& operator=(const ResidentObject&) = delete;
    ResidentObject(ResidentObject&&) = delete;
    ResidentObject& operator=(ResidentObject&&) = delete;

    [[nodiscard]] T* get() const noexcept { return object_; }
    [[nodiscard]] T* operator->() const noexcept { return object_; }
    [[nodiscard]] T& operator*() const noexcept { return *object_; }
    [[nodiscard]] bool locked() const noexcept { return pages_.locked(); }
    [[nodiscard]] std::size_t committed_size() const noexcept { return pages_.committed_size(); }

private:
    [[nodiscard]] static constexpr std::size_t storage_size() noexcept
    {
        static_assert(sizeof(T) <= std::numeric_limits<std::size_t>::max() - (alignof(T) - 1));
        return sizeof(T) + alignof(T) - 1;
    }

    PageAllocation pages_;
    T* object_{};
};

} // namespace wavplay::core
