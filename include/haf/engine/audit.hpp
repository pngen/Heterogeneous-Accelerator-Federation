// Heterogeneous Accelerator Federation - invariant audit.
//
// The audit is a pure function of a snapshot. It checks the properties the
// federation claims to maintain and reports every violation with a stable
// machine-readable code. A final validation artifact must report zero.

#ifndef HAF_ENGINE_AUDIT_HPP
#define HAF_ENGINE_AUDIT_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "haf/federation/snapshot.hpp"

namespace haf {

struct AuditViolation {
    /// Stable code, e.g. "identity.uniqueness".
    std::string code;
    /// Entity the violation refers to.
    std::string subject;
    std::string detail;

    friend bool operator==(const AuditViolation&, const AuditViolation&) noexcept = default;
};

struct AuditReport {
    std::size_t checks_run{0};
    std::vector<AuditViolation> violations;

    [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
    [[nodiscard]] std::string render() const;
};

/// Audit every invariant the federation maintains over a snapshot.
[[nodiscard]] AuditReport audit_snapshot(const FederationSnapshot& snapshot);

}  // namespace haf

#endif  // HAF_ENGINE_AUDIT_HPP
