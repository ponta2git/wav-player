#pragma once

#include "framework/failure_reporting.h"
#include "framework/test_thread.h"

#include <chrono>
#include <concepts>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace wavplay_test {

// A failed assertion. No product code throws it, so expect_throw cannot mistake
// an assertion that failed inside the operation for the expected error.
struct Failure final : std::exception {
    explicit Failure(std::string message) : message_(std::make_shared<const std::string>(std::move(message))) {}
    const char* what() const noexcept override { return message_->c_str(); }

private:
    std::shared_ptr<const std::string> message_; // Shared, so copying the exception cannot throw.
};

[[noreturn]] inline void fail(std::string message)
{
    throw Failure(std::move(message));
}

namespace detail {

// " (file.cpp:123)" for a failure message.
inline std::string location_suffix(const std::source_location& location)
{
    std::string_view file = location.file_name();
    if (const auto separator = file.find_last_of("\\/"); separator != std::string_view::npos) {
        file.remove_prefix(separator + 1);
    }
    return " (" + std::string(file) + ':' + std::to_string(location.line()) + ')';
}

// The integer types std::cmp_equal accepts: no bool and no character types.
template<class T>
concept StandardInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
    !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

template<class T>
concept Checked = StandardInteger<T> || std::is_enum_v<T>;

// A scoped enum compares only with its own type; integers and unscoped enums
// compare by value.
template<class Actual, class Expected>
concept Comparable = Checked<Actual> && Checked<Expected> &&
    (std::same_as<Actual, Expected> || (!std::is_scoped_enum_v<Actual> && !std::is_scoped_enum_v<Expected>));

// An enum's underlying value, promoted past bool and character types.
template<class T>
constexpr auto checked_value(T value) noexcept
{
    if constexpr (std::is_enum_v<T>) {
        return +std::to_underlying(value);
    } else {
        return value;
    }
}

} // namespace detail

template<class Actual, class Expected>
    requires detail::Comparable<Actual, Expected>
void expect(std::string_view field, Actual actual, Expected expected,
    std::source_location location = std::source_location::current())
{
    const auto got = detail::checked_value(actual);
    const auto wanted = detail::checked_value(expected);
    if (!std::cmp_equal(got, wanted)) {
        fail(std::string(field) + ": expected " + std::to_string(wanted) + ", got " + std::to_string(got) +
            detail::location_suffix(location));
    }
}

inline void expect_true(std::string_view field, bool condition,
    std::source_location location = std::source_location::current())
{
    if (!condition) {
        fail(std::string(field) + ": expected true, got false" + detail::location_suffix(location));
    }
}

template<class Exception>
void expect_throw(std::string_view field, auto operation,
    std::source_location location = std::source_location::current())
{
    static_assert(!std::is_base_of_v<Exception, Failure>, "An assertion failure must not satisfy expect_throw");
    try {
        operation();
    } catch (const Failure&) {
        throw; // An assertion inside the operation failed, whatever the operation was expected to throw.
    } catch (const Exception&) {
        return;
    }
    fail(std::string(field) + ": expected an exception, call succeeded" + detail::location_suffix(location));
}

class TestSuite {
public:
    // Reports an assertion failure, any other exception (with its type) and a
    // non-standard exception alike as a failed case, then continues the suite.
    void run(const char* name, auto test)
    {
        const CurrentCase current(name);
        try {
            test();
        } catch (const Failure& failure) {
            report(name, failure.what());
        } catch (const std::exception& error) {
            report(name, std::string(typeid(error).name()) + ": " + error.what());
        } catch (...) {
            report(name, "non-standard exception");
        }
    }

    // Runs the case on a thread of its own. ASIO callbacks and teardown can block
    // before a test reaches its own clock checks, so a case still running at its
    // limit ends the process as hung.
    void run_timed(const char* name, auto test)
    {
        run(name, [&] {
            constexpr auto limit = hang_budget(case_budget);
            TestThread<void> done(std::move(test));
            if (done.wait_for(limit) != std::future_status::ready) {
                abandon_hung("timed out after " + std::to_string(limit.count() / 1000) + " seconds");
            }
            done.get();
        });
    }

    int result(const char* name) const
    {
        if (failures_ == 0) {
            std::printf("%s tests passed.\n", name);
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    // A Release case's limit in run_timed, before hang_budget scales it.
    static constexpr std::chrono::milliseconds case_budget{std::chrono::seconds(30)};

    void report(const char* name, const std::string& message)
    {
        std::fprintf(stderr, "[FAIL] %s: %s\n", name, message.c_str());
        ++failures_;
    }

    int failures_{};
};

} // namespace wavplay_test
