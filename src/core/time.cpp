#include "haf/core/time.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace haf {

Timestamp now_timestamp() noexcept {
    return Timestamp(std::chrono::duration_cast<Nanos>(SystemClock::now().time_since_epoch()));
}

MonotonicTime now_monotonic() noexcept {
    return MonotonicTime(std::chrono::duration_cast<Nanos>(SteadyClock::now().time_since_epoch()));
}

std::int64_t to_unix_nanos(Timestamp value) noexcept { return value.time_since_epoch().count(); }

Timestamp from_unix_nanos(std::int64_t value) noexcept { return Timestamp(Nanos(value)); }

std::uint64_t age_nanos(MonotonicTime earlier, MonotonicTime later) noexcept {
    if (later <= earlier) {
        return 0;
    }
    return static_cast<std::uint64_t>((later - earlier).count());
}

std::uint64_t age_nanos(Timestamp earlier, Timestamp later) noexcept {
    if (later <= earlier) {
        return 0;
    }
    return static_cast<std::uint64_t>((later - earlier).count());
}

std::string format_timestamp(Timestamp value) {
    const std::int64_t nanos = to_unix_nanos(value);
    const std::int64_t seconds = nanos / 1'000'000'000LL;
    const std::int64_t millis = (nanos % 1'000'000'000LL) / 1'000'000LL;
    const std::time_t as_time = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (gmtime_s(&parts, &as_time) != 0) {
        return "1970-01-01T00:00:00.000Z";
    }
#else
    if (gmtime_r(&as_time, &parts) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }
#endif
    char buffer[40];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                                      parts.tm_min, parts.tm_sec, static_cast<int>(millis));
    if (written <= 0) {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

std::string format_duration_nanos(std::uint64_t nanos) {
    char buffer[64];
    if (nanos < 1'000ULL) {
        const int written = std::snprintf(buffer, sizeof(buffer), "%lluns", static_cast<unsigned long long>(nanos));
        return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
    }
    if (nanos < 1'000'000ULL) {
        const double value = static_cast<double>(nanos) / 1'000.0;
        const int written = std::snprintf(buffer, sizeof(buffer), "%.3fus", value);
        return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
    }
    if (nanos < 1'000'000'000ULL) {
        const double value = static_cast<double>(nanos) / 1'000'000.0;
        const int written = std::snprintf(buffer, sizeof(buffer), "%.3fms", value);
        return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
    }
    const double value = static_cast<double>(nanos) / 1'000'000'000.0;
    const int written = std::snprintf(buffer, sizeof(buffer), "%.3fs", value);
    return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
}

std::string RuntimeVersion::to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

}  // namespace haf
