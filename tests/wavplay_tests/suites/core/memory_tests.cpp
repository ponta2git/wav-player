#include "suites/suites.h"
#include "fixtures/image_residency.h"
#include "framework/supervisor.h"
#include "framework/test_suite.h"
#include "wavplay/core/block_copy.h"
#include "wavplay/core/memory.h"

#include <psapi.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The test executable's own import slot, used only by the isolated refusal
// child. No product interface or process outside that child is modified.
extern "C" decltype(&SetProcessWorkingSetSizeEx) __imp_SetProcessWorkingSetSizeEx;

namespace {

using namespace wavplay_test;
using wavplay::core::copy_block;
using wavplay::core::PageAllocation;
using wavplay::core::ResidentObject;
using wavplay::core::UniqueHandle;
using wavplay::core::zero_block;

static_assert(!std::is_copy_constructible_v<ResidentObject<std::uint64_t>>);
static_assert(!std::is_move_constructible_v<ResidentObject<std::uint64_t>>);
// A non-const lvalue reaches the deleted copy constructor, not the forwarding one.
static_assert(!std::is_constructible_v<ResidentObject<std::uint64_t>, ResidentObject<std::uint64_t>&>);
// The forwarding constructor's constraint also rejects a T whose destructor may throw.
struct ThrowingDestructor {
    ~ThrowingDestructor() noexcept(false);
};
static_assert(!std::is_constructible_v<ResidentObject<ThrowingDestructor>>);

struct WorkingSet {
    SIZE_T minimum{}, maximum{};
    DWORD flags{};

    WorkingSet()
    {
        if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &flags)) {
            wavplay::core::throw_last_error("GetProcessWorkingSetSizeEx");
        }
    }
};

void check_page_rounding_and_moves()
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t page = system.dwPageSize;
    expect("shared page size", wavplay::core::page_size(), page);
    const WorkingSet before;
    {
        PageAllocation first(page + 1);
        expect("requested size stays exact", first.size(), page + 1);
        expect("committed size includes the partial page", first.committed_size(), 2 * page);
        expect("partial final page is locked", static_cast<int>(first.locked()), 1);
        PSAPI_WORKING_SET_EX_INFORMATION query{};
        query.VirtualAddress = first.data() + page;
        if (!QueryWorkingSetEx(GetCurrentProcess(), &query, sizeof(query))) {
            wavplay::core::throw_last_error("QueryWorkingSetEx");
        }
        expect("final page is resident", static_cast<int>(query.VirtualAttributes.Valid), 1);
        expect("final page is fixed", static_cast<int>(query.VirtualAttributes.Locked), 1);
        expect("quota covers both committed pages", WorkingSet{}.minimum, before.minimum + 2 * page);
        first.data()[page] = std::byte{0x5a};

        PageAllocation second(1);
        expect("two allocations hold three pages", WorkingSet{}.minimum, before.minimum + 3 * page);
        second = std::move(first);
        expect("move assignment returns the old quota", WorkingSet{}.minimum, before.minimum + 2 * page);
        expect("move preserves requested size", second.size(), page + 1);
        expect("move preserves committed size", second.committed_size(), 2 * page);
        expect("move preserves data", static_cast<int>(second.data()[page]), 0x5a);
        // These observers explicitly support an empty, moved-from allocation.
#pragma warning(suppress : 26800)
        expect("moved allocation is empty", first.size(), 0);
#pragma warning(suppress : 26800)
        expect("moved allocation commits no pages", first.committed_size(), 0);
#pragma warning(suppress : 26800)
        expect("moved allocation does not report a lock", static_cast<int>(first.locked()), 0);

        const PageAllocation third(std::move(second));
        expect("move construction keeps the quota", WorkingSet{}.minimum, before.minimum + 2 * page);
        expect("move construction keeps the lock", static_cast<int>(third.locked()), 1);
#pragma warning(suppress : 26800)
        expect("move construction empties the source", second.size(), 0);
    }
    const WorkingSet after;
    expect("minimum quota is restored", after.minimum, before.minimum);
    expect("maximum quota is restored", after.maximum, before.maximum);
    expect("working-set flags are preserved", after.flags, before.flags);
}

void check_invalid_sizes()
{
    expect_throw<std::invalid_argument>("empty allocation", [] { PageAllocation allocation(0); });
    expect_throw<std::length_error>("page-rounding overflow", [] {
        PageAllocation allocation(std::numeric_limits<std::size_t>::max());
    });
    const PageAllocation empty;
    expect("default allocation size", empty.size(), 0);
    expect("default allocation committed size", empty.committed_size(), 0);
    expect("default allocation lock", static_cast<int>(empty.locked()), 0);
}

#pragma warning(push)
#pragma warning(disable : 4324) // Deliberate cache-line alignment, including trailing padding.
struct alignas(wavplay::core::cache_line_size) ResidentPayload {
    std::array<std::byte, 8193> bytes{};
    unsigned& destroyed;
    int value;

    ResidentPayload(unsigned& count, int initial) noexcept : destroyed(count), value(initial) {}
    ~ResidentPayload() noexcept { ++destroyed; }
};
#pragma warning(pop)

struct ThrowingPayload {
    unsigned& destroyed;
    explicit ThrowingPayload(unsigned& count) : destroyed(count) { throw std::runtime_error("construction refused"); }
    ~ThrowingPayload() noexcept { ++destroyed; }
};

void check_resident_object()
{
    const WorkingSet before;
    unsigned destroyed{};
    void* allocation{};
    {
        ResidentObject<ResidentPayload> object(destroyed, 37);
        expect("resident construction forwards arguments", object->value, 37);
        expect("resident object has cache-line alignment",
            reinterpret_cast<std::uintptr_t>(object.get()) % alignof(ResidentPayload), 0);
        expect("resident object lock succeeds", static_cast<int>(object.locked()), 1);
        expect("resident object owns its lock quota", WorkingSet{}.minimum, before.minimum + object.committed_size());
        (*object).bytes.back() = std::byte{0x5a};
        expect("resident object exposes initialized storage", static_cast<int>(object->bytes.front()), 0);
        expect("resident object final byte is writable", static_cast<int>(object->bytes.back()), 0x5a);
        expect("resident object is not prematurely destroyed", destroyed, 0);

        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(object.get(), &region, sizeof(region))) {
            wavplay::core::throw_last_error("VirtualQuery(resident object)");
        }
        allocation = region.AllocationBase;
        expect("resident object has a private committed region", region.Type, MEM_PRIVATE);
        expect("resident object region matches its committed accounting", region.RegionSize, object.committed_size());
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        for (std::size_t offset = 0; offset < object.committed_size(); offset += system.dwPageSize) {
            PSAPI_WORKING_SET_EX_INFORMATION query{};
            query.VirtualAddress = static_cast<std::byte*>(allocation) + offset;
            if (!QueryWorkingSetEx(GetCurrentProcess(), &query, sizeof(query))) {
                wavplay::core::throw_last_error("QueryWorkingSetEx(resident object)");
            }
            expect("resident object page is present", static_cast<int>(query.VirtualAttributes.Valid), 1);
            expect("resident object page is fixed", static_cast<int>(query.VirtualAttributes.Locked), 1);
        }
    }
    MEMORY_BASIC_INFORMATION released{};
    if (!VirtualQuery(allocation, &released, sizeof(released))) {
        wavplay::core::throw_last_error("VirtualQuery(released object)");
    }
    expect("resident object releases its private pages", released.State, MEM_FREE);
    expect("resident object destructor runs once", destroyed, 1);
    expect("resident object returns minimum quota", WorkingSet{}.minimum, before.minimum);
    expect("resident object returns maximum quota", WorkingSet{}.maximum, before.maximum);

    unsigned failed_destructors{};
    expect_throw<std::runtime_error>("resident constructor failure", [&] {
        ResidentObject<ThrowingPayload> object(failed_destructors);
    });
    expect("failed object is never destroyed as a complete object", failed_destructors, 0);
    expect("failed resident construction returns minimum quota", WorkingSet{}.minimum, before.minimum);
    expect("failed resident construction returns maximum quota", WorkingSet{}.maximum, before.maximum);
    expect("resident lifetime preserves working-set flags", WorkingSet{}.flags, before.flags);
}

// The image lock lasts for the life of the process, so only child processes of
// this executable take it (run_image_lock_child); the suite process never does.

// One page of this executable's data that only the image-lock children touch.
// Its nonzero constant keeps it in .data, mapped copy-on-write from the file
// until a child first writes it. x64 pages are 4 KiB (checked in the children).
struct alignas(4096) ProbePage {
    std::uint64_t value{0x5a5a'5a5a'5a5a'5a5a};
    std::array<std::byte, 4096 - sizeof(std::uint64_t)> rest{};
};
static_assert(sizeof(ProbePage) == 4096);
constinit ProbePage probe_page;

DWORD protection(const void* address)
{
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(address, &region, sizeof(region))) {
        wavplay::core::throw_last_error("VirtualQuery(image page)");
    }
    return region.Protect;
}

void expect_limits(std::string_view what, const WorkingSet& actual, const WorkingSet& expected)
{
    expect(std::string(what) + ": minimum working set", actual.minimum, expected.minimum);
    expect(std::string(what) + ": maximum working set", actual.maximum, expected.maximum);
    expect(std::string(what) + ": working-set flags", actual.flags, expected.flags);
}

void child_locks_image()
{
    using namespace wavplay::core;
    const auto page = page_size();
    expect("the probe is one whole page", sizeof(ProbePage), page);
    expect("the probe starts a page", reinterpret_cast<std::uintptr_t>(&probe_page) % page, 0);
    expect("the probe page is still copy-on-write", protection(&probe_page), PAGE_WRITECOPY);
    expect_true("the image starts pageable", !process_image_locked());
    const WorkingSet before;
    expect_true("the image lock succeeds", lock_process_image());
    expect_true("the image lock is reported", process_image_locked());
    const WorkingSet locked;
    expect("minimum working set grows by the image", locked.minimum, before.minimum + image_bytes());
    expect("maximum working set grows by the image", locked.maximum, before.maximum + image_bytes());
    expect("working-set flags are preserved", locked.flags, before.flags);
    expect("every image page is resident and locked", locked_image_pages(), image_bytes() / page);
    expect_true("the page of a known function is resident and locked",
        resident_and_locked(reinterpret_cast<const void*>(&lock_process_image)));
    expect_true("an unwritten static's page is resident and locked", resident_and_locked(&probe_page));

    probe_page.value = ~probe_page.value; // The first write gives the page a private copy.
    expect("the written static's page is private", protection(&probe_page), PAGE_READWRITE);
    expect_true("the private copy keeps the lock", resident_and_locked(&probe_page));
    expect_true("a second call reports the lock", lock_process_image());
    expect_limits("a second call", WorkingSet{}, locked);

    // A trim of the whole working set, as memory optimizers apply to every process.
    if (!EmptyWorkingSet(GetCurrentProcess())) {
        throw_last_error("EmptyWorkingSet");
    }
    expect("a trimmed working set keeps every image page", locked_image_pages(), image_bytes() / page);
}

struct QuotaRequest {
    HANDLE process{};
    SIZE_T minimum{}, maximum{};
    DWORD flags{};
    unsigned calls{};
};
constinit QuotaRequest denied_quota;

BOOL WINAPI refuse_quota_growth(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD flags) noexcept
{
    denied_quota = {process, minimum, maximum, flags, denied_quota.calls + 1};
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

// Fail exactly the OS quota request instead of depending on the account's
// privileges. Both product calls are noexcept; restore the import and its page
// protection before any assertion can throw. The other child exercises an
// actual VirtualLock failure after a successful working-set growth.
void child_refused_quota_growth()
{
    using namespace wavplay::core;
    const WorkingSet before;
    const auto original = __imp_SetProcessWorkingSetSizeEx;
    DWORD original_protection{};
    if (!VirtualProtect(&__imp_SetProcessWorkingSetSizeEx, sizeof original, PAGE_READWRITE, &original_protection)) {
        throw_last_error("VirtualProtect(quota import)");
    }
    __imp_SetProcessWorkingSetSizeEx = refuse_quota_growth;
    const bool locked = lock_process_image();
    const bool repeated = lock_process_image();
    __imp_SetProcessWorkingSetSizeEx = original;
    DWORD ignored{};
    if (!VirtualProtect(&__imp_SetProcessWorkingSetSizeEx, sizeof original, original_protection, &ignored)) {
        throw_last_error("VirtualProtect(restore quota import)");
    }
    expect_true("the quota import is restored", __imp_SetProcessWorkingSetSizeEx == original);
    expect("the import page protection is restored", protection(&__imp_SetProcessWorkingSetSizeEx), original_protection);
    expect("one quota request despite the repeated call", denied_quota.calls, 1);
    expect_true("the quota request targets this child", denied_quota.process == GetCurrentProcess());
    expect("the quota request covers the image minimum", denied_quota.minimum, before.minimum + image_bytes());
    expect("the quota request covers the image maximum", denied_quota.maximum, before.maximum + image_bytes());
    expect("the quota request preserves flags", denied_quota.flags, before.flags);
    expect_true("the image lock is refused", !locked);
    expect_true("a repeated call uses the cached refusal", !repeated);
    expect_true("the refusal is reported", !process_image_locked());
    expect_limits("after the refusal", WorkingSet{}, before);
    expect("no image page is locked", locked_image_pages(), 0);
    expect_true("a second call reports the refusal", !lock_process_image());
}

// An inaccessible page fails VirtualLock after the limits grew.
void child_lock_fails_after_growth()
{
    using namespace wavplay::core;
    DWORD previous{};
    if (!VirtualProtect(&probe_page, sizeof(probe_page), PAGE_NOACCESS, &previous)) {
        throw_last_error("VirtualProtect(probe page)");
    }
    const WorkingSet before;
    const bool locked = lock_process_image();
    if (!VirtualProtect(&probe_page, sizeof(probe_page), previous, &previous)) {
        throw_last_error("VirtualProtect(restore probe page)");
    }
    expect_true("the image lock fails", !locked);
    expect_true("the failure is reported", !process_image_locked());
    expect_limits("after the failed lock", WorkingSet{}, before);
    expect("no image page stays locked", locked_image_pages(), 0);
}

// Runs this executable with --image-lock-child <mode> and fails with what the
// child wrote unless it exits with 0.
void check_image_lock_child(std::wstring_view mode)
{
    const auto child = run_own_child(L"--image-lock-child " + std::wstring(mode), hang_budget(std::chrono::seconds(10)));
    if (child.timed_out) {
        fail("the image lock child did not exit: " + child.output);
    }
    if (child.exit_code != 0) {
        fail("the image lock child exited with " + std::to_string(child.exit_code) + ": " + child.output);
    }
}

// Every block length from 0 to 320 bytes (each tail after 0 to 19 whole 16-byte
// steps) and the 2048-, 8192- and 16384-byte blocks.
constexpr std::size_t largest_block = 16384;

std::vector<std::size_t> block_lengths()
{
    std::vector<std::size_t> lengths;
    for (std::size_t bytes = 0; bytes <= 320; ++bytes) {
        lengths.push_back(bytes);
    }
    for (const std::size_t bytes : {std::size_t{2048}, std::size_t{8192}, largest_block}) {
        lengths.push_back(bytes);
    }
    return lengths;
}

// Bytes checked on each side of a destination block, and their value.
constexpr std::size_t guard_bytes = 64;
constexpr unsigned char guard_value = 0xff;

// Never zero or the guard value, and periodic only over 251 bytes, so a missing,
// shifted or repeated 16-byte store changes a compared byte.
std::byte source_pattern(std::size_t index) noexcept
{
    return static_cast<std::byte>(index % 251 + 1);
}

// Readable pages between two inaccessible pages: a block that starts at begin()
// or ends at end() faults on any load or store before its first or past its
// last byte, even without AddressSanitizer.
class GuardedWindow {
public:
    explicit GuardedWindow(std::size_t bytes)
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const std::size_t page = system.dwPageSize;
        size_ = (bytes + page - 1) / page * page;
        allocation_ = static_cast<std::byte*>(
            VirtualAlloc(nullptr, page + size_ + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!allocation_) {
            wavplay::core::throw_last_error("VirtualAlloc(guarded window)");
        }
        base_ = allocation_ + page;
        DWORD previous{};
        if (!VirtualProtect(allocation_, page, PAGE_NOACCESS, &previous) ||
            !VirtualProtect(end(), page, PAGE_NOACCESS, &previous)) {
            const auto error = GetLastError();
            VirtualFree(allocation_, 0, MEM_RELEASE);
            wavplay::core::throw_win32_error(error, "VirtualProtect(guard page)");
        }
    }

    ~GuardedWindow() { VirtualFree(allocation_, 0, MEM_RELEASE); }
    GuardedWindow(const GuardedWindow&) = delete;
    GuardedWindow& operator=(const GuardedWindow&) = delete;

    [[nodiscard]] std::byte* begin() const noexcept { return base_; }
    [[nodiscard]] std::byte* end() const noexcept { return base_ + size_; }

    // A read-only source also exposes a store through the wrong pointer.
    void fill_read_only(std::size_t pattern_offset = 0)
    {
        for (std::size_t index = 0; index < size_; ++index) {
            base_[index] = source_pattern(index + pattern_offset);
        }
        DWORD previous{};
        if (!VirtualProtect(base_, size_, PAGE_READONLY, &previous)) {
            wavplay::core::throw_last_error("VirtualProtect(read-only source)");
        }
    }

private:
    std::byte* allocation_{};
    std::byte* base_{};
    std::size_t size_{};
};

struct BlockCase {
    const char* operation;
    std::size_t bytes;
    std::uintptr_t destination;
    std::uintptr_t source;
};

// Fails with the case when [actual, actual + count) differs from expected.
void expect_region(const BlockCase& block, const char* region, const std::byte* actual, const std::byte* expected,
    std::size_t count)
{
    if (count == 0 || std::memcmp(actual, expected, count) == 0) {
        return;
    }
    std::size_t offset = 0;
    while (actual[offset] == expected[offset]) {
        ++offset;
    }
    fail(std::string(block.operation) + " of " + std::to_string(block.bytes) + " bytes (destination address mod 32 = " +
        std::to_string(block.destination % 32) + ", source address mod 32 = " + std::to_string(block.source % 32) +
        "): " + region + " byte " + std::to_string(offset) + " is " +
        std::to_string(std::to_integer<int>(actual[offset])) + ", expected " +
        std::to_string(std::to_integer<int>(expected[offset])));
}

// Runs copy_block and zero_block into [to, to + bytes) with guard_bytes of
// guards before it and `after` guard bytes after it, and checks every byte.
void check_block(std::byte* to, const std::byte* from, std::size_t bytes, std::size_t after)
{
    static const std::vector<std::byte> zeros(largest_block);
    static const std::vector<std::byte> guards(guard_bytes, std::byte{guard_value});
    const auto destination = reinterpret_cast<std::uintptr_t>(to);
    const auto source = reinterpret_cast<std::uintptr_t>(from);

    std::memset(to - guard_bytes, guard_value, guard_bytes + bytes + after);
    copy_block(to, from, bytes);
    const BlockCase copy{"copy_block", bytes, destination, source};
    expect_region(copy, "guard before the block", to - guard_bytes, guards.data(), guard_bytes);
    expect_region(copy, "block", to, from, bytes);
    expect_region(copy, "guard after the block", to + bytes, guards.data(), after);

    std::memset(to - guard_bytes, guard_value, guard_bytes + bytes + after);
    zero_block(to, bytes);
    const BlockCase zero{"zero_block", bytes, destination, source};
    expect_region(zero, "guard before the block", to - guard_bytes, guards.data(), guard_bytes);
    expect_region(zero, "block", to, zeros.data(), bytes);
    expect_region(zero, "guard after the block", to + bytes, guards.data(), after);
}

void check_block_copy_at_inaccessible_pages()
{
    GuardedWindow source(largest_block), destination(guard_bytes + largest_block);
    source.fill_read_only();
    for (const auto bytes : block_lengths()) {
        // The last loaded and the last stored byte each precede an inaccessible page.
        check_block(destination.end() - bytes, source.end() - bytes, bytes, 0);
    }
}

void check_block_copy_misalignment()
{
    // Page-aligned windows: an offset of 0..31 is the address modulo 32. At
    // offset 0 the first loaded byte follows an inaccessible page.
    GuardedWindow source(31 + largest_block), destination(guard_bytes + 31 + largest_block + guard_bytes);
    source.fill_read_only();
    for (const auto bytes : block_lengths()) {
        for (std::size_t to_offset = 0; to_offset < 32; ++to_offset) {
            for (std::size_t from_offset = 0; from_offset < 32; ++from_offset) {
                check_block(destination.begin() + guard_bytes + to_offset, source.begin() + from_offset, bytes,
                    guard_bytes);
            }
        }
    }
}

void check_two_blocks(std::byte* left, std::byte* right, const std::byte* source_left,
    const std::byte* source_right, std::size_t bytes, std::size_t after)
{
    static const std::array<std::byte, guard_bytes> guards = [] {
        std::array<std::byte, guard_bytes> result;
        result.fill(std::byte{guard_value});
        return result;
    }();
    for (const bool duplicate : {false, true}) {
        std::memset(left - guard_bytes, guard_value, guard_bytes + bytes + after);
        std::memset(right - guard_bytes, guard_value, guard_bytes + bytes + after);
        if (duplicate) {
            wavplay::core::duplicate_block(left, right, source_left, bytes);
        } else {
            wavplay::core::copy_stereo_blocks(left, right, source_left, source_right, bytes);
        }
        for (const bool right_channel : {false, true}) {
            const auto* to = right_channel ? right : left;
            const auto* from = right_channel && !duplicate ? source_right : source_left;
            const BlockCase operation{duplicate ? "duplicate_block" : "copy_stereo_blocks", bytes,
                reinterpret_cast<std::uintptr_t>(to), reinterpret_cast<std::uintptr_t>(from)};
            expect_region(operation, right_channel ? "right leading guard" : "left leading guard",
                to - guard_bytes, guards.data(), guard_bytes);
            expect_region(operation, right_channel ? "right block" : "left block", to, from, bytes);
            expect_region(operation, right_channel ? "right trailing guard" : "left trailing guard",
                to + bytes, guards.data(), after);
        }
    }
}

void check_two_blocks_at_inaccessible_pages()
{
    GuardedWindow source_left(largest_block), source_right(largest_block);
    GuardedWindow left(guard_bytes + largest_block), right(guard_bytes + largest_block);
    source_left.fill_read_only();
    source_right.fill_read_only(113); // Different channels expose a swapped or duplicated source.
    for (const auto bytes : block_lengths()) {
        for (const bool left_at_end : {false, true}) {
            for (const bool right_at_end : {false, true}) {
                check_two_blocks(left.end() - bytes, right.end() - bytes,
                    left_at_end ? source_left.end() - bytes : source_left.begin(),
                    right_at_end ? source_right.end() - bytes : source_right.begin(), bytes, 0);
            }
        }
    }
}

void check_two_blocks_misalignment()
{
    GuardedWindow source_left(31 + largest_block), source_right(31 + largest_block);
    GuardedWindow left(guard_bytes + 31 + largest_block + guard_bytes);
    GuardedWindow right(guard_bytes + 31 + largest_block + guard_bytes);
    source_left.fill_read_only();
    source_right.fill_read_only(113);
    for (const auto bytes : block_lengths()) {
        for (std::size_t destination = 0; destination < 32; ++destination) {
            for (std::size_t source = 0; source < 32; ++source) {
                // Every left/right destination alignment pair occurs once. Each
                // source also visits all alignments against each destination,
                // without a redundant four-dimensional Cartesian product.
                check_two_blocks(left.begin() + guard_bytes + destination,
                    right.begin() + guard_bytes + (destination + source) % 32,
                    source_left.begin() + source,
                    source_right.begin() + (source + 2 * destination + 1) % 32, bytes, guard_bytes);
            }
        }
    }
}

} // namespace

int wavplay_test::run_image_lock_child(std::span<wchar_t* const> operands)
{
    if (operands.size() != 1) {
        return 2;
    }
    const std::wstring_view mode(operands[0]);
    try {
        if (mode == L"lock") {
            child_locks_image();
        } else if (mode == L"refused") {
            child_refused_quota_growth();
        } else if (mode == L"lock-fails") {
            child_lock_fails_after_growth();
        } else {
            return 2;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

int wavplay_test::run_memory_tests()
{
    TestSuite suite;
    suite.run("page-rounded working-set ownership and moves", check_page_rounding_and_moves);
    suite.run("invalid page allocation sizes", check_invalid_sizes);
    suite.run("resident object alignment, complete lifetime and failed construction", check_resident_object);
    suite.run("image lock in a child: every page resident and locked, after copy-on-write and a trim too",
        [] { check_image_lock_child(L"lock"); });
    suite.run("image lock preserves limits and caches a refused quota request",
        [] { check_image_lock_child(L"refused"); });
    suite.run("image lock failing after the growth restores the limits", [] { check_image_lock_child(L"lock-fails"); });
    suite.run("block copy and zero end before inaccessible pages", check_block_copy_at_inaccessible_pages);
    suite.run("block copy and zero at every source and destination misalignment", check_block_copy_misalignment);
    suite.run("stereo copy and mono duplication stay inside read-only spans and inaccessible pages",
        check_two_blocks_at_inaccessible_pages);
    suite.run("stereo copy and mono duplication with independent channel alignments", check_two_blocks_misalignment);
    return suite.result("Core memory and block copy");
}
