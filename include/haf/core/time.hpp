// Heterogeneous Accelerator Federation - time and freshness.
//
// Evidence freshness is expressed as an age relative to a caller-supplied
// "now" so that tests and deterministic replays are reproducible. The runtime
// never consults the wall clock implicitly inside a decision.

#ifndef HAF_CORE_TIME_HPP
#define HAF_CORE_TIME_HPP

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace haf {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using Nanos = std::chrono::nanoseconds;

/// Wall-clock instant expressed as nanoseconds since the Unix epoch. Wall time
/// is used only for evidence timestamps and diagnostics, never for ordering.
using Timestamp = std::chrono::time_point<SystemClock, Nanos>;

/// Monotonic instant used for ages, timeouts, and freshness decisions.
using MonotonicTime = std::chrono::time_point<SteadyClock, Nanos>;

[[nodiscard]] Timestamp now_timestamp() noexcept;
[[nodiscard]] MonotonicTime now_monotonic() noexcept;

[[nodiscard]] std::int64_t to_unix_nanos(Timestamp value) noexcept;
[[nodiscard]] Timestamp from_unix_nanos(std::int64_t value) noexcept;

/// Non-negative age of \p earlier relative to \p later, in nanoseconds.
[[nodiscard]] std::uint64_t age_nanos(MonotonicTime earlier, MonotonicTime later) noexcept;
[[nodiscard]] std::uint64_t age_nanos(Timestamp earlier, Timestamp later) noexcept;

/// ISO-8601 UTC rendering with millisecond precision, deterministic.
[[nodiscard]] std::string format_timestamp(Timestamp value);

/// Duration rendering used by CLI output ("1.500s", "12ms", "800ns").
[[nodiscard]] std::string format_duration_nanos(std::uint64_t nanos);

/// Legacy-style version triple used by vendor runtime/driver reporting.
struct RuntimeVersion {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    friend bool operator==(const RuntimeVersion&, const RuntimeVersion&) noexcept = default;
    friend auto operator<=>(const RuntimeVersion&, const RuntimeVersion&) noexcept = default;

    [[nodiscard]] std::string to_string() const;
};

}  // namespace haf

#endif  // HAF_CORE_TIME_HPP
