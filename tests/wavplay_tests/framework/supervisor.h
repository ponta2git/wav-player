#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace wavplay_test {

struct SupervisedSuite {
    std::wstring_view name;
    bool long_running{}; // Started before the others, so the parallel run ends sooner.
};

// Runs each suite as "<this executable> --suite <name>" in a process of its
// own, at most `jobs` at a time, so a hang, crash or sanitizer stop in one
// suite cannot end the others or share their process-wide state. Each child
// runs in a job that is killed when its wall-clock budget runs out or the
// supervisor exits, and writes to a file of its own that is printed as one
// block when the suite ends. Prints a PASS/FAIL/TIMEOUT/CRASH line per suite in
// the given order and a total, and returns 0 only if every suite passed.
int supervise_suites(std::span<const SupervisedSuite> suites, unsigned jobs);

// The default parallelism: half the logical processors, at least 1 and at most
// 4. Most suites wait on real-time playback rather than compute, but timing
// assertions still need idle processors.
[[nodiscard]] unsigned default_supervised_jobs() noexcept;

// How a child of this executable ended and what it wrote.
struct ChildRun {
    unsigned long exit_code{};
    bool timed_out{};   // Still running after the timeout, then terminated.
    std::string output; // Its stdout and stderr together.
};

// Runs "<this executable> <arguments>" without a window and waits up to
// `timeout`; a child still running then, and its descendants, are terminated
// with exit_code::hung. The child starts only after joining its owning job;
// closing that job also releases descendants after normal exit or exceptions.
// As for a suite, stdout and stderr go to a temporary file that is the child's
// only inherited handle, so the child never waits for this reader.
[[nodiscard]] ChildRun run_own_child(std::wstring_view arguments, std::chrono::milliseconds timeout);

} // namespace wavplay_test
