#pragma once

#include "wavplay/core/memory.h"
#include "wavplay/core/win32.h"

#include <psapi.h>

#include <cstddef>
#include <vector>

// The linker's symbol at the base address of this executable.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace wavplay_test {

// The working-set state of this executable's image, which core::lock_process_image
// locks for the life of a process. Only children of the test executable take that
// lock; a suite process never does.

// SizeOfImage: the headers and every section, the range the lock covers.
inline std::size_t image_bytes() noexcept
{
    const auto* base = reinterpret_cast<const std::byte*>(&__ImageBase);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew)->OptionalHeader.SizeOfImage;
}

// Only a valid working-set entry defines its Locked bit.
inline bool resident_and_locked(const void* address)
{
    PSAPI_WORKING_SET_EX_INFORMATION query{};
    query.VirtualAddress = const_cast<void*>(address);
    if (!QueryWorkingSetEx(GetCurrentProcess(), &query, sizeof(query))) {
        wavplay::core::throw_last_error("QueryWorkingSetEx(image page)");
    }
    return query.VirtualAttributes.Valid && query.VirtualAttributes.Locked;
}

// The pages of the image that are resident and locked, out of image_bytes() / page_size().
inline std::size_t locked_image_pages()
{
    const auto page = wavplay::core::page_size();
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(image_bytes() / page);
    for (std::size_t index = 0; index < pages.size(); ++index) {
        pages[index].VirtualAddress = reinterpret_cast<std::byte*>(&__ImageBase) + index * page;
    }
    if (!QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(pages.size() * sizeof(pages[0])))) {
        wavplay::core::throw_last_error("QueryWorkingSetEx(image)");
    }
    std::size_t locked = 0;
    for (const auto& entry : pages) {
        locked += entry.VirtualAttributes.Valid && entry.VirtualAttributes.Locked;
    }
    return locked;
}

} // namespace wavplay_test
