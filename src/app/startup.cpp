#include "wavplay/core/cpu_features.h"

#include <windows.h>

#if defined(__AVX__) || defined(__AVX2__)
#error The application startup gate must be compiled for the baseline ISA.
#endif

// The ABI boundary is deliberately only an integer, with no shared inline
// definitions or STL types. /GL- keeps the AVX2 body out of this baseline TU.
int run_application(int show_command);

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show_command)
{
    if (!wavplay::core::supports_required_cpu(wavplay::core::query_cpu_features())) {
        MessageBoxW(nullptr,
            L"WAV Player requires AVX2, BMI1/BMI2, FMA, F16C, LZCNT and MOVBE, "
            L"with AVX enabled by Windows.\n\n"
            L"This computer does not provide the required instruction sets or processor state. "
            L"WAV Player cannot start.",
            L"WAV Player", MB_OK | MB_ICONERROR);
        return 1;
    }
    return run_application(show_command);
}
