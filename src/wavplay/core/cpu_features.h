#pragma once

#include <cstdint>

namespace wavplay::core {

// Raw capability evidence, not a CPU model or a performance preference. The
// startup gate is compiled for SSE4.2 without LTCG; no AVX instruction is needed
// to collect or check this state. Missing CPUID leaves remain zero.
struct CpuFeatures {
    std::uint32_t leaf1_ecx{};
    std::uint32_t leaf7_ebx{};
    std::uint32_t extended1_ecx{};
    std::uint64_t enabled_xstate{};
    bool windows_avx{};
    bool windows_avx2{};
};

[[nodiscard]] CpuFeatures query_cpu_features() noexcept;

// MSVC /arch:AVX2 also permits scalar extensions. Require the complete v3
// extension set (on the Windows 11 25H2 SSE4.2 baseline), including the enabled
// XMM/YMM context, rather than inferring BMI, LZCNT or FMA from the AVX2 bit.
// This pure predicate is separately tested with each required feature absent.
[[nodiscard]] bool supports_required_cpu(const CpuFeatures& features) noexcept;

} // namespace wavplay::core
