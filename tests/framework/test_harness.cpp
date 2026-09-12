#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace haf::test {
namespace {

const auto kStart = std::chrono::steady_clock::now();

[[nodiscard]] std::string hex_of(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "0x";
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFULL);
        if (nibble != 0 || started || shift == 0) {
            started = true;
            out.push_back(kDigits[nibble]);
        }
    }
    return out;
}

}  // namespace

std::string describe_value(std::int64_t value) { return std::to_string(value); }
std::string describe_value(std::uint64_t value) { return std::to_string(value); }
std::string describe_value(int value) { return std::to_string(value); }
std::string describe_value(unsigned int value) { return std::to_string(value); }
std::string describe_value(bool value) { return value ? "true" : "false"; }
std::string describe_value(const std::string& value) { return "\"" + value + "\""; }
std::string describe_value(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string describe_value(const char* value) { return value == nullptr ? "(null)" : describe_value(std::string_view(value)); }
std::string describe_value(ErrorCode value) { return std::string(to_string(value)); }
std::string describe_value(const Status& value) { return value.describe(); }
std::string describe_value(double value) {
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
}

CaseContext::CaseContext(std::string suite, std::string name)
    : suite_(std::move(suite)), name_(std::move(name)), full_name_(suite_ + "::" + name_) {}

void CaseContext::phase_marker(std::string_view marker) {
    phase_ = std::string(marker);
    std::cout << marker << " " << full_name_ << std::endl;
}

void CaseContext::note(std::string_view text) {
    std::cout << "NOTE " << full_name_ << " " << text << std::endl;
}

void CaseContext::report(std::string_view expression, const char* file, int line, const std::string& detail) {
    std::cout << "FAIL " << full_name_ << " [phase " << (phase_.empty() ? "RUN" : phase_) << "] " << file << ":"
              << line << ": " << expression;
    if (!detail.empty()) {
        std::cout << " -- " << detail;
    }
    std::cout << std::endl;
}

void CaseContext::check(bool condition, std::string_view expression, const char* file, int line,
                        std::string_view detail) {
    ++assertions_;
    failures_this_check_ = false;
    if (!condition) {
        ++failures_;
        failures_this_check_ = true;
        report(expression, file, line, std::string(detail));
    }
}

void CaseContext::check_failed(std::string_view expression, const char* file, int line, std::string_view detail) {
    ++assertions_;
    ++failures_;
    failures_this_check_ = true;
    report(expression, file, line, std::string(detail));
}

void CaseContext::require(bool condition, std::string_view expression, const char* file, int line) {
    ++assertions_;
    failures_this_check_ = false;
    if (!condition) {
        ++failures_;
        failures_this_check_ = true;
        report(expression, file, line, "requirement not met; case aborted at this point");
    }
}

std::vector<CaseRegistration>& registry() {
    static std::vector<CaseRegistration>* cases = new std::vector<CaseRegistration>();
    return *cases;
}

Registrar::Registrar(const char* suite, const char* name, CaseFunction function) {
    registry().push_back(CaseRegistration{suite, name, function});
}

std::uint64_t elapsed_millis() noexcept {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - kStart).count());
}

int run_all(int argc, char** argv) {
    std::vector<CaseRegistration>& cases = registry();
    std::sort(cases.begin(), cases.end(), [](const CaseRegistration& a, const CaseRegistration& b) {
        if (a.suite != b.suite) {
            return a.suite < b.suite;
        }
        return a.name < b.name;
    });

    std::string selected;
    bool list_only = false;
    bool verbose = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--list") {
            list_only = true;
        } else if (argument == "--quiet") {
            verbose = false;
        } else if (argument.rfind("--case=", 0) == 0) {
            selected = std::string(argument.substr(7));
        } else if (argument.rfind("--filter=", 0) == 0) {
            selected = std::string(argument.substr(9));
        } else {
            std::cout << "unknown argument: " << argument << std::endl;
            return 2;
        }
    }

    if (list_only) {
        for (const CaseRegistration& registration : cases) {
            std::cout << registration.suite << "::" << registration.name << std::endl;
        }
        std::cout << "total " << cases.size() << " cases" << std::endl;
        return 0;
    }

    std::size_t executed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t assertions = 0;
    std::string first_failure;

    for (const CaseRegistration& registration : cases) {
        const std::string full = registration.suite + "::" + registration.name;
        if (!selected.empty()) {
            const bool exact = (full == selected);
            const bool prefix = (full.rfind(selected + "::", 0) == 0) || (registration.suite == selected);
            const bool substring = full.find(selected) != std::string::npos;
            if (!exact && !prefix && !substring) {
                ++skipped;
                continue;
            }
        }
        ++executed;
        if (verbose) {
            std::cout << "BEGIN " << full << std::endl;
        }
        CaseContext context(registration.suite, registration.name);
        try {
            registration.function(context);
        } catch (const std::exception& error) {
            context.check_failed("no exception escapes the case", __FILE__, __LINE__, error.what());
        } catch (...) {
            context.check_failed("no exception escapes the case", __FILE__, __LINE__, "unknown exception");
        }
        assertions += context.assertions();
        if (context.failures() != 0) {
            ++failed;
            if (first_failure.empty()) {
                first_failure = full;
            }
            std::cout << "FAIL " << full << " (" << context.failures() << " failed assertions)" << std::endl;
        } else if (verbose) {
            std::cout << "PASS " << full << std::endl;
        }
    }

    std::cout << "SUMMARY executed=" << executed << " skipped=" << skipped << " failed=" << failed
              << " assertions=" << assertions << " elapsed_ms=" << elapsed_millis() << std::endl;
    if (!first_failure.empty()) {
        std::cout << "FIRST_FAILURE " << first_failure << std::endl;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace haf::test

int main(int argc, char** argv) { return haf::test::run_all(argc, argv); }
