// Shared command-line helpers for haf_coordinator, haf_agent, and haf_cli.

#ifndef HAF_APPS_APP_SUPPORT_HPP
#define HAF_APPS_APP_SUPPORT_HPP

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace haf::app {

class Arguments {
public:
    Arguments(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            values_.emplace_back(argv[i]);
        }
    }

    [[nodiscard]] bool has(std::string_view flag) const {
        for (const std::string& value : values_) {
            if (value == flag) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> value(std::string_view flag) const {
        for (std::size_t i = 0; i + 1 < values_.size(); ++i) {
            if (values_[i] == flag) {
                return values_[i + 1];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string string_value(std::string_view flag, std::string fallback) const {
        const std::optional<std::string> found = value(flag);
        return found.has_value() ? *found : std::move(fallback);
    }

    [[nodiscard]] std::int64_t int_value(std::string_view flag, std::int64_t fallback) const {
        const std::optional<std::string> found = value(flag);
        if (!found.has_value()) {
            return fallback;
        }
        try {
            return std::stoll(*found);
        } catch (...) {
            return fallback;
        }
    }

    [[nodiscard]] std::uint64_t uint_value(std::string_view flag, std::uint64_t fallback) const {
        const std::int64_t parsed = int_value(flag, static_cast<std::int64_t>(fallback));
        return parsed < 0 ? fallback : static_cast<std::uint64_t>(parsed);
    }

    [[nodiscard]] const std::vector<std::string>& values() const noexcept { return values_; }

    /// Positional arguments, excluding flags and the values that follow them.
    [[nodiscard]] std::vector<std::string> positional(const std::vector<std::string>& flags_with_values) const {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < values_.size(); ++i) {
            bool is_value_of_flag = false;
            for (const std::string& flag : flags_with_values) {
                if (i > 0 && values_[i - 1] == flag) {
                    is_value_of_flag = true;
                    break;
                }
            }
            if (is_value_of_flag) {
                continue;
            }
            if (!values_[i].empty() && values_[i][0] == '-') {
                continue;
            }
            out.push_back(values_[i]);
        }
        return out;
    }

private:
    std::vector<std::string> values_;
};

/// Parse "host:port". Returns false when the text is malformed.
[[nodiscard]] inline bool parse_endpoint(const std::string& text, std::string& host, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    try {
        const long parsed = std::stol(text.substr(colon + 1));
        if (parsed <= 0 || parsed > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(parsed);
    } catch (...) {
        return false;
    }
    return true;
}

/// True when the stop file exists. Used to drive graceful shutdown from tests
/// without relying on signals or timing.
[[nodiscard]] inline bool stop_requested(const std::filesystem::path& stop_file) {
    if (stop_file.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(stop_file, error);
}

inline void sleep_millis(std::uint64_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

/// Print a machine-readable status line and flush immediately so that a parent
/// process observing the child sees it without buffering delay.
inline void emit(const std::string& line) {
    std::cout << line << std::endl;
    std::cout.flush();
}

}  // namespace haf::app

#endif  // HAF_APPS_APP_SUPPORT_HPP
