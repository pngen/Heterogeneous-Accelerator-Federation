// Heterogeneous Accelerator Federation - test harness.
//
// Design goals:
//   * every case has a stable "suite::case" name and can be executed exactly;
//   * long-running cases emit flushed progress markers so that a stalled run
//     identifies the exact case and phase instead of hanging opaquely;
//   * no timeouts are used as a substitute for correctness.

#ifndef HAF_TEST_HARNESS_HPP
#define HAF_TEST_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/status.hpp"

namespace haf::test {

[[nodiscard]] std::string describe_value(std::int64_t value);
[[nodiscard]] std::string describe_value(std::uint64_t value);
[[nodiscard]] std::string describe_value(int value);
[[nodiscard]] std::string describe_value(unsigned int value);
[[nodiscard]] std::string describe_value(bool value);
[[nodiscard]] std::string describe_value(const std::string& value);
[[nodiscard]] std::string describe_value(std::string_view value);
[[nodiscard]] std::string describe_value(const char* value);
[[nodiscard]] std::string describe_value(ErrorCode value);
[[nodiscard]] std::string describe_value(const Status& value);
[[nodiscard]] std::string describe_value(double value);

/// Fallback for types that expose to_string(). Constrained so that the
/// explicit overloads above always win.
template <class T>
[[nodiscard]] auto describe_value(const T& value) -> decltype(value.to_string()) {
    return value.to_string();
}

class CaseContext {
public:
    CaseContext(std::string suite, std::string name);

    [[nodiscard]] const std::string& full_name() const noexcept { return full_name_; }
    [[nodiscard]] const std::string& phase() const noexcept { return phase_; }

    /// Emit a flushed progress marker. Used to identify where a stalled case is.
    void phase_marker(std::string_view marker);
    void note(std::string_view text);

    void check(bool condition, std::string_view expression, const char* file, int line,
               std::string_view detail = {});
    void check_failed(std::string_view expression, const char* file, int line, std::string_view detail = {});

    template <class A, class B>
    void equal(const A& actual, const B& expected, std::string_view expression, const char* file, int line) {
        ++assertions_;
        failures_this_check_ = false;
        if (!(actual == expected)) {
            ++failures_;
            failures_this_check_ = true;
            std::string detail = "expected ";
            detail += describe_value(expected);
            detail += " but observed ";
            detail += describe_value(actual);
            report(expression, file, line, detail);
        }
    }

    void require(bool condition, std::string_view expression, const char* file, int line);

    [[nodiscard]] std::size_t assertions() const noexcept { return assertions_; }
    [[nodiscard]] std::size_t failures() const noexcept { return failures_; }
    [[nodiscard]] bool failed_this_check() const noexcept { return failures_this_check_; }

private:
    void report(std::string_view expression, const char* file, int line, const std::string& detail);

    std::string suite_;
    std::string name_;
    std::string full_name_;
    std::string phase_;
    std::size_t assertions_{0};
    std::size_t failures_{0};
    bool failures_this_check_{false};
};

using CaseFunction = void (*)(CaseContext&);

struct CaseRegistration {
    std::string suite;
    std::string name;
    CaseFunction function;
};

/// Global registry. Deliberately leaked so that static registration order is
/// irrelevant.
[[nodiscard]] std::vector<CaseRegistration>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, CaseFunction function);
};

/// Entry point shared by every suite executable.
int run_all(int argc, char** argv);

/// Monotonic elapsed time since the suite started.
[[nodiscard]] std::uint64_t elapsed_millis() noexcept;

/// Accept either a Status or a Result so that HAF_REQUIRE_OK works for both.
/// The returned reference is only valid while the referenced object is; callers
/// copy the Status into a value before using it.
[[nodiscard]] inline const Status& status_of(const Status& value) noexcept { return value; }
[[nodiscard]] inline const Status& status_of(const VoidResult& value) noexcept { return value.status(); }
template <class T>
[[nodiscard]] inline const Status& status_of(const Result<T>& value) noexcept {
    return value.status();
}

}  // namespace haf::test

#define HAF_TEST(suite, name)                                                                          \
    static void haf_case_##suite##_##name(::haf::test::CaseContext& ctx);                              \
    static const ::haf::test::Registrar haf_registrar_##suite##_##name(#suite, #name,                  \
                                                                      &haf_case_##suite##_##name);     \
    static void haf_case_##suite##_##name(::haf::test::CaseContext& ctx)

#define HAF_PHASE(marker) ctx.phase_marker(marker)
#define HAF_NOTE(text) ctx.note(text)
#define HAF_CHECK(condition) ctx.check((condition), #condition, __FILE__, __LINE__)
#define HAF_CHECK_MSG(condition, detail) ctx.check((condition), #condition, __FILE__, __LINE__, (detail))
#define HAF_FAIL(detail) ctx.check_failed("HAF_FAIL", __FILE__, __LINE__, (detail))
#define HAF_EQ(actual, expected) ctx.equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define HAF_REQUIRE(condition)                                    \
    do {                                                          \
        ctx.require((condition), #condition, __FILE__, __LINE__);  \
        if (ctx.failed_this_check()) {                            \
            return;                                               \
        }                                                         \
    } while (false)

// The status is copied into a value. Holding a reference to the status of the
// temporary produced by the expression would dangle as soon as the
// initializing full-expression ended -- exactly the stack-use-after-scope
// defect AddressSanitizer reported against the earlier form of this macro.
// Using a scoped local also lets several HAF_REQUIRE_OK calls appear in one
// case without redeclaring a variable.
#define HAF_REQUIRE_OK(expr)                                                                  \
    do {                                                                                      \
        const ::haf::Status haf_required_status_ = ::haf::test::status_of(expr);               \
        if (!haf_required_status_.ok()) {                                                     \
            HAF_FAIL(std::string("unexpected failure: ") + haf_required_status_.describe());   \
            return;                                                                           \
        }                                                                                     \
    } while (false)

#define HAF_CHECK_CODE(expr, expected_code)                                                 \
    do {                                                                                    \
        const ::haf::Status haf_status_ = (expr);                                           \
        ctx.equal(static_cast<int>(haf_status_.code()), static_cast<int>(expected_code),     \
                  #expr " has code " #expected_code, __FILE__, __LINE__);                    \
    } while (false)

#endif  // HAF_TEST_HARNESS_HPP
