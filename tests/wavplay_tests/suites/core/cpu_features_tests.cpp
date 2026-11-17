#include "suites/suites.h"
#include "framework/test_suite.h"
#include "wavplay/core/cpu_features.h"

#include <array>
#include <cstdint>

namespace {

using namespace wavplay_test;
using wavplay::core::CpuFeatures;
using wavplay::core::supports_required_cpu;

CpuFeatures supported()
{
    // Independent CPUID bit positions, kept separate from the product's masks.
    return {0x3CD81200u, 0x128u, 0x20u, 0x6u, true, true};
}

void check_required_features()
{
    expect_true("complete required ISA and enabled state", supports_required_cpu(supported()));
    expect_true("unknown CPU is rejected", !supports_required_cpu({}));
    for (const auto bit : std::array{9, 12, 19, 20, 22, 23, 26, 27, 28, 29}) {
        auto features = supported();
        features.leaf1_ecx &= ~(1u << bit);
        expect_true("every required basic feature is checked", !supports_required_cpu(features));
    }
    for (const auto bit : std::array{3, 5, 8}) {
        auto features = supported();
        features.leaf7_ebx &= ~(1u << bit);
        expect_true("AVX2 and both BMI extensions are independent", !supports_required_cpu(features));
    }
    auto features = supported();
    features.extended1_ecx = 0;
    expect_true("LZCNT is required independently of AVX2", !supports_required_cpu(features));
}

void check_operating_system_state()
{
    for (const std::uint64_t state : {0ull, 1ull, 2ull, 4ull, 0x18ull}) {
        auto features = supported();
        features.enabled_xstate = state;
        expect_true("CPU support alone cannot enable AVX", !supports_required_cpu(features));
    }
    for (unsigned missing = 0; missing != 2; ++missing) {
        auto features = supported();
        (missing == 0 ? features.windows_avx : features.windows_avx2) = false;
        expect_true("Windows availability is required", !supports_required_cpu(features));
    }
    auto features = supported();
    features.leaf1_ecx = features.leaf7_ebx = features.extended1_ecx = 0xffffffffu;
    features.enabled_xstate = 0xffffffffffffffffull;
    expect_true("unrelated future features do not prevent startup", supports_required_cpu(features));
}

void check_current_computer()
{
    expect_true("the test host supports the product ISA", supports_required_cpu(wavplay::core::query_cpu_features()));
}

} // namespace

int wavplay_test::run_cpu_features_tests()
{
    TestSuite suite;
    suite.run("required CPU extensions", check_required_features);
    suite.run("OS state and additional features", check_operating_system_state);
    suite.run("current CPU and Windows", check_current_computer);
    return suite.result("CPU features");
}
