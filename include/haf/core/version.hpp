// Heterogeneous Accelerator Federation - semantic versions and version ranges.
//
// Version comparison follows Semantic Versioning 2.0.0 precedence rules.
// Build metadata is excluded from precedence and from canonical hashing so
// that two builds differing only in metadata compare equal.

#ifndef HAF_CORE_VERSION_HPP
#define HAF_CORE_VERSION_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/status.hpp"

namespace haf {

class SemanticVersion {
public:
    constexpr SemanticVersion() noexcept = default;
    constexpr SemanticVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) noexcept
        : major_(major), minor_(minor), patch_(patch) {}

    [[nodiscard]] static std::optional<SemanticVersion> parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr std::uint32_t major() const noexcept { return major_; }
    [[nodiscard]] constexpr std::uint32_t minor() const noexcept { return minor_; }
    [[nodiscard]] constexpr std::uint32_t patch() const noexcept { return patch_; }
    [[nodiscard]] const std::string& prerelease() const noexcept { return prerelease_; }
    [[nodiscard]] const std::string& build_metadata() const noexcept { return build_metadata_; }

    void set_prerelease(std::string value) { prerelease_ = std::move(value); }
    void set_build_metadata(std::string value) { build_metadata_ = std::move(value); }

    /// Canonical "major.minor.patch[-prerelease]" form. Build metadata is
    /// intentionally omitted because it carries no precedence meaning.
    [[nodiscard]] std::string to_string() const;

    /// Canonical form including build metadata, for display only.
    [[nodiscard]] std::string to_string_full() const;

    friend bool operator==(const SemanticVersion& a, const SemanticVersion& b) noexcept;
    friend std::strong_ordering operator<=>(const SemanticVersion& a, const SemanticVersion& b) noexcept;

    /// True when a canonical semantic version can be parsed from the text.
    [[nodiscard]] static bool is_valid(std::string_view text) noexcept;

private:
    std::uint32_t major_{0};
    std::uint32_t minor_{0};
    std::uint32_t patch_{0};
    std::string prerelease_;
    std::string build_metadata_;
};

/// One comparator inside a version range expression.
enum class VersionOperator : std::uint8_t {
    Any = 0,      ///< "*" - any version satisfies this clause.
    Equal = 1,    ///< "=1.2.3"
    NotEqual = 2, ///< "!=1.2.3"
    Less = 3,     ///< "<1.2.3"
    LessEqual = 4,
    Greater = 5,
    GreaterEqual = 6,
    Compatible = 7, ///< "^1.2.3" - same major, >= 1.2.3
    Tilde = 8,      ///< "~1.2.3" - same major.minor, >= 1.2.3
};

struct VersionClause {
    VersionOperator op{VersionOperator::Any};
    SemanticVersion version{};

    friend bool operator==(const VersionClause&, const VersionClause&) noexcept = default;
};

/// A conjunction of clauses: ">=12.0.0 <13.0.0". Empty range means "any".
class VersionRange {
public:
    VersionRange() = default;

    /// Parse a whitespace/comma separated conjunction. Returns a typed error on
    /// malformed input rather than silently widening the range.
    [[nodiscard]] static Result<VersionRange> parse(std::string_view text);

    [[nodiscard]] bool matches(const SemanticVersion& version) const noexcept;
    [[nodiscard]] bool is_any() const noexcept { return clauses_.empty(); }

    [[nodiscard]] const std::vector<VersionClause>& clauses() const noexcept { return clauses_; }

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const VersionRange&, const VersionRange&) noexcept = default;

private:
    std::vector<VersionClause> clauses_;
};

}  // namespace haf

#endif  // HAF_CORE_VERSION_HPP
