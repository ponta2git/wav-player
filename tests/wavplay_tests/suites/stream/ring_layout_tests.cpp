#include "suites/suites.h"
#include "framework/test_suite.h"
#include "wavplay/core/memory.h"
#include "wavplay/stream/ring_layout.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using namespace wavplay_test;
using wavplay::stream::plan_block_shape;
using wavplay::stream::plan_ring_layout;
using wavplay::stream::PrefetchPolicy;
using wavplay::stream::RingLayout;

// The expected strides below are written for 64-byte cache lines.
static_assert(wavplay::core::cache_line_size == 64);

constexpr std::size_t mib = 1024 * 1024;
// Numerical geometry checks describe this explicit policy, independently of
// the product default, to verify the planner's contract.
constexpr PrefetchPolicy twice_per_ring{.producer_wakes_per_ring = 2};

void expect_layout(const char* name, const RingLayout& layout, std::size_t capacity, std::uint64_t total_blocks,
    std::size_t final_frames, std::size_t wake_interval, bool shared_line = false)
{
    const std::string prefix(name);
    expect(prefix + " capacity", layout.capacity, capacity);
    expect(prefix + " plane bytes", layout.plane_bytes, capacity * layout.block.stride);
    expect(prefix + " channel offset", layout.channel_offset, shared_line ? 32 : capacity * layout.block.stride);
    expect(prefix + " allocation bytes", layout.allocation_bytes(), (shared_line ? 1 : 2) * capacity * layout.block.stride);
    expect(prefix + " total blocks", layout.total_blocks, total_blocks);
    expect(prefix + " final frames", layout.final_frames, final_frames);
    expect(prefix + " wake interval", layout.wake_interval, wake_interval);
}

void test_block_shape()
{
    // 94 bytes round up to two lines; a 544-frame 24-bit block (1632 bytes) to 26.
    const auto odd = plan_block_shape(47, 2);
    expect("47-frame block frames", odd.frames, 47);
    expect("47-frame block bytes", odd.bytes, 94);
    expect("47-frame block stride", odd.stride, 128);
    const auto packed = plan_block_shape(544, 3);
    expect("packed block bytes", packed.bytes, 1632);
    expect("packed block stride", packed.stride, 1664);
    const auto dense = plan_block_shape(64, 2);
    expect("dense block stride", dense.stride, 128);

    // Two stereo blocks must fit the 16 MiB budget.
    expect("widest 16-bit block", plan_block_shape(2 * mib, 2).bytes, 4 * mib);
    expect_throw<std::invalid_argument>("16-bit block over budget", [] { (void)plan_block_shape(2 * mib + 1, 2); });
    expect_throw<std::invalid_argument>("zero-frame block", [] { (void)plan_block_shape(0, 2); });
    expect_throw<std::invalid_argument>("zero-byte samples", [] { (void)plan_block_shape(4, 0); });
    expect_throw<std::invalid_argument>("sample width multiplication would overflow", [] {
        (void)plan_block_shape(1, std::numeric_limits<std::size_t>::max() / 4 + 1);
    });
    const PrefetchPolicy small{.ring_budget_bytes = 4096};
    expect("widest block for a custom budget", plan_block_shape(512, 2, small).frames, 512);
    expect_throw<std::invalid_argument>("block over a custom budget", [&] { (void)plan_block_shape(513, 2, small); });
    // 1000 sample bytes fit a quarter of 4000, but their 1024-byte aligned slot does not.
    const PrefetchPolicy unaligned{.ring_budget_bytes = 4000};
    expect_throw<std::invalid_argument>("aligned slot over a custom budget", [&] {
        (void)plan_block_shape(500, 2, unaligned);
    });
    expect_throw<std::invalid_argument>("policy without a target", [] {
        (void)plan_block_shape(4, 2, PrefetchPolicy{.target_seconds = 0});
    });
    expect_throw<std::invalid_argument>("policy without producer wakes", [] {
        (void)plan_block_shape(4, 2, PrefetchPolicy{.producer_wakes_per_ring = 0});
    });
    expect_throw<std::invalid_argument>("policy without source batch bytes", [] {
        (void)plan_block_shape(4, 2, PrefetchPolicy{.batch_source_bytes = 0});
    });
}

void test_ring_layout()
{
    // One second per block: the two-second target needs exactly two slots.
    const auto second = plan_block_shape(44100, 2); // 88200 bytes, stride 88256.
    expect_layout("one-second blocks", plan_ring_layout(second, 3 * 44100 + 3, 44100, twice_per_ring), 2, 4, 3, 1);

    // 96-frame blocks: 919 slots hold two seconds, regardless of the source's byte width.
    const auto small = plan_block_shape(96, 2);
    expect_layout("96-frame blocks", plan_ring_layout(small, 3 * 44100 + 3, 44100, twice_per_ring), 919, 1379, 15, 459);

    // Packed output needs alignment gaps but keeps the same two-second target.
    const auto packed = plan_block_shape(544, 3);
    expect_layout("24-bit output ring", plan_ring_layout(packed, 10 * 48000, 48000, twice_per_ring), 177, 883, 192, 88);

    const auto pcm32 = plan_block_shape(544, 4);
    expect_layout("32-bit output ring", plan_ring_layout(pcm32, 10 * 48000, 48000, twice_per_ring), 177, 883, 192, 88);

    // A run shorter than the target needs only its own blocks, even a single one.
    const auto tiny = plan_block_shape(4, 2);
    expect_layout("one-frame run", plan_ring_layout(tiny, 1, 48000, twice_per_ring), 1, 1, 1, 1, true);

    // The budget wins over the target: 4 MiB stride leaves room for two slots.
    const auto widest = plan_block_shape(2 * mib, 2);
    expect_layout("budget-limited ring", plan_ring_layout(widest, 20 * mib, 384000, twice_per_ring), 2, 10, 2 * mib, 1);

    // A custom policy changes only the producer-side geometry.
    const PrefetchPolicy policy{.ring_budget_bytes = 1 * mib, .target_seconds = 1, .batch_source_bytes = 4096,
        .producer_wakes_per_ring = 4};
    const auto block = plan_block_shape(256, 2, policy); // 512 bytes per channel.
    expect_layout("custom policy", plan_ring_layout(block, 1'000'000, 48000, policy), 188, 3907, 64, 47);
}

void test_ring_layout_boundaries()
{
    const auto block = plan_block_shape(4, 2);
    expect_throw<std::invalid_argument>("zero run frames", [&] { (void)plan_ring_layout(block, 0, 48000); });
    expect_throw<std::invalid_argument>("zero sample rate", [&] { (void)plan_ring_layout(block, 8, 0); });
    expect_throw<std::invalid_argument>("zero block frames", [&] { (void)plan_ring_layout({}, 8, 48000); });
    expect_throw<std::invalid_argument>("fractional sample width", [&] {
        (void)plan_ring_layout({4, 9, 64}, 8, 48000);
    });
    expect_throw<std::invalid_argument>("unaligned block stride", [&] {
        (void)plan_ring_layout({4, 8, 8}, 8, 48000);
    });
    expect_throw<std::invalid_argument>("policy changed since block validation", [&] {
        (void)plan_ring_layout(block, 8, 48000, PrefetchPolicy{.producer_wakes_per_ring = 0});
    });
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    const PrefetchPolicy huge_duration{.ring_budget_bytes = 4096, .target_seconds = max_size,
        .batch_source_bytes = max_size, .producer_wakes_per_ring = 2};
    const auto bounded = plan_block_shape(4, 2, huge_duration);
    // Duration saturates to the budget rather than wrapping to a smaller ring.
    expect_layout("overflow-safe duration",
        plan_ring_layout(bounded, 1'000'000, 48000, huge_duration), 64, 250000, 4, 32, true);
}

void test_shared_line_layout()
{
    struct Small { std::size_t sample_bytes, payload; };
    for (const auto& test : {Small{2, 16}, Small{3, 24}, Small{4, 32}}) {
        const auto block = plan_block_shape(8, test.sample_bytes);
        expect("small-channel payload", block.bytes, test.payload);
        expect_layout("two-second shared-line ring",
            plan_ring_layout(block, 144003, 48000, twice_per_ring), 12000, 18001, 3, 6000, true);
    }
    // 33 bytes cannot share a 32-byte half, even though each separate channel
    // still has a 64-byte stride. The boundary changes allocation, not width.
    const auto above_half = plan_block_shape(11, 3);
    const auto separate = plan_ring_layout(above_half, 144003, 48000);
    expect("33-byte channels remain in separate planes", separate.channel_offset, separate.plane_bytes);
    expect("33-byte allocation has two planes", separate.allocation_bytes(), 2 * separate.plane_bytes);

    for (std::size_t bytes = 0; bytes < 128; ++bytes) {
        const PrefetchPolicy policy{.ring_budget_bytes = bytes};
        expect_throw<std::invalid_argument>("less than two physical cache-line slots", [&] {
            (void)plan_block_shape(1, 1, policy);
        });
    }
    const PrefetchPolicy exact{.ring_budget_bytes = 128};
    const auto small = plan_block_shape(8, 4, exact);
    expect_layout("two shared slots exactly fill a tiny budget",
        plan_ring_layout(small, 25, 48000, exact), 2, 4, 1, 1, true);
    expect_throw<std::invalid_argument>("separate channels need four aligned slots", [&] {
        (void)plan_block_shape(11, 3, PrefetchPolicy{.ring_budget_bytes = 255});
    });
    const PrefetchPolicy separate_exact{.ring_budget_bytes = 256};
    expect_layout("two separate blocks exactly fill the budget",
        plan_ring_layout(plan_block_shape(11, 3, separate_exact), 34, 48000, separate_exact), 2, 4, 1, 1);
}

void test_wake_policies()
{
    const auto block = plan_block_shape(8, 4);
    expect_layout("two wakes per ring", plan_ring_layout(block, 144003, 48000,
        PrefetchPolicy{.producer_wakes_per_ring = 2}), 12000, 18001, 3, 6000, true);
    expect_layout("four wakes per ring", plan_ring_layout(block, 144003, 48000,
        PrefetchPolicy{.producer_wakes_per_ring = 4}), 12000, 18001, 3, 3000, true);
    const auto active = plan_ring_layout(block, 144003, 48000);
    expect("default policy retains the two-second capacity", active.capacity, 12000);
    expect_true("default policy wakes before the ring is empty",
        active.wake_interval > 0 && active.wake_interval < active.capacity);
}

} // namespace

int wavplay_test::run_ring_layout_tests()
{
    TestSuite suite;
    suite.run("block width validation and cache-line strides", test_block_shape);
    suite.run("ring capacity, run blocks and wake interval", test_ring_layout);
    suite.run("ring dimensions reject invalid inputs and bound arithmetic", test_ring_layout_boundaries);
    suite.run("small channel payloads share one line without sharing different blocks", test_shared_line_layout);
    suite.run("explicit refill policies and a safe configurable default", test_wake_policies);
    return suite.result("Ring layout");
}
