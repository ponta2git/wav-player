#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/track_player_fixture.h"

#include <cstdint>

namespace {

using namespace wavplay_test;

void test_wide_dac_boundaries(std::uint8_t source_bits, bool extensible24_in32 = false)
{
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const auto type : {ASIOSTInt24LSB, ASIOSTInt32LSB}) {
            if (source_bits == 32 && type == ASIOSTInt24LSB) { continue; }
            for (const auto position : {DacQueueAsio::Position::advancing, DacQueueAsio::Position::zero, DacQueueAsio::Position::unsupported}) {
                for (const std::uint64_t boundary : {0ULL, 3ULL * DacQueueAsio::block, 7ULL * DacQueueAsio::block}) {
                    test_pause_boundary(position, type, boundary, true, 1, 73, source_bits, channels, extensible24_in32);
                }
            }
            test_pause_boundary(DacQueueAsio::Position::zero, type, 2 * DacQueueAsio::block, false, 0, 193, source_bits, channels, extensible24_in32);
            test_pause_boundary(DacQueueAsio::Position::zero, type, 6 * DacQueueAsio::block, false, 5, 73, source_bits, channels, extensible24_in32);
            test_pause_boundary(DacQueueAsio::Position::zero, type, 3 * DacQueueAsio::block, true, 0, DacQueueAsio::block - 1, source_bits, channels, extensible24_in32);
        }
    }
}

} // namespace

// The wider PCM sources (PCM24, Extensible PCM24 in 32-bit containers and
// PCM32) through the same Pause, short EOF and Stop boundaries as the PCM16
// baseline in track_player, to each LSB integer output wide enough to hold them.
int wavplay_test::run_track_source_formats_tests()
{
    TestSuite suite;
    suite.run_timed("PCM24 pause/resume preserves every actual DAC sample", [] { test_wide_dac_boundaries(24); });
    suite.run_timed("Extensible PCM24/32 pause/resume preserves every actual DAC sample", [] { test_wide_dac_boundaries(24, true); });
    suite.run_timed("PCM32 pause/resume preserves every actual DAC sample", [] { test_wide_dac_boundaries(32); });
    suite.run_timed("PCM24 short EOF drains every actual DAC sample", [] { test_short_endings(24); });
    suite.run_timed("Extensible PCM24/32 short EOF drains every actual DAC sample", [] { test_short_endings(24, true); });
    suite.run_timed("PCM32 short EOF drains every actual DAC sample", [] { test_short_endings(32); });
    suite.run_timed("PCM24 Stop and replay preserve the DAC prefix and entire restarted track", [] { test_stop_boundaries(24); });
    suite.run_timed("Extensible PCM24/32 Stop and replay preserve every actual DAC sample", [] { test_stop_boundaries(24, true); });
    suite.run_timed("PCM32 Stop and replay preserve the DAC prefix and entire restarted track", [] { test_stop_boundaries(32); });
    return suite.result("Single-track source formats through the DAC");
}
