#include "wavplay/core/cpu_features.h"

#include <windows.h>
#include <intrin.h>

#if defined(__AVX__) || defined(__AVX2__)
#error The CPU startup gate must be compiled for the baseline ISA.
#endif

namespace wavplay::core {

CpuFeatures query_cpu_features() noexcept
{
    CpuFeatures features;
    int registers[4]{};
    __cpuid(registers, 0);
    const auto maximum = static_cast<unsigned>(registers[0]);
    if (maximum >= 1) {
        __cpuidex(registers, 1, 0);
        features.leaf1_ecx = static_cast<std::uint32_t>(registers[2]);
    }
    if (maximum >= 7) {
        __cpuidex(registers, 7, 0);
        features.leaf7_ebx = static_cast<std::uint32_t>(registers[1]);
    }
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(registers[0]) >= 0x80000001u) {
        __cpuid(registers, static_cast<int>(0x80000001u));
        features.extended1_ecx = static_cast<std::uint32_t>(registers[2]);
    }
    features.enabled_xstate = GetEnabledXStateFeatures();
    features.windows_avx = IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) != FALSE;
    features.windows_avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
    return features;
}

bool supports_required_cpu(const CpuFeatures& features) noexcept
{
    // SSSE3, FMA, SSE4.1, SSE4.2, MOVBE, POPCNT, XSAVE, OSXSAVE, AVX, F16C.
    constexpr std::uint32_t leaf1 = (1u << 9) | (1u << 12) | (1u << 19) | (1u << 20) |
        (1u << 22) | (1u << 23) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29);
    constexpr std::uint32_t leaf7 = (1u << 3) | (1u << 5) | (1u << 8); // BMI1, AVX2, BMI2.
    constexpr std::uint32_t extended1 = 1u << 5; // LZCNT.
    constexpr std::uint64_t xmm_ymm = (1ull << 1) | (1ull << 2);
    return (features.leaf1_ecx & leaf1) == leaf1 && (features.leaf7_ebx & leaf7) == leaf7 &&
        (features.extended1_ecx & extended1) == extended1 &&
        (features.enabled_xstate & xmm_ymm) == xmm_ymm && features.windows_avx && features.windows_avx2;
}

} // namespace wavplay::core
