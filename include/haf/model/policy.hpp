// Heterogeneous Accelerator Federation - federation policy.
//
// Policy is explicit, versioned data. It is never implicit process-global
// state. Every policy change advances the policy generation, which invalidates
// every compatibility decision, migration plan, and membership validation that
// was derived from the previous generation.

#ifndef HAF_MODEL_POLICY_HPP
#define HAF_MODEL_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/version.hpp"
#include "haf/model/evidence.hpp"
#include "haf/model/portability.hpp"

namespace haf {

/// How the federation treats evidence whose provenance is not REAL.
enum class SyntheticEvidencePolicy : std::uint8_t {
    Reject = 0,   ///< SYNTHETIC evidence makes a member ineligible. Strictest.
    Allow = 1,    ///< SYNTHETIC evidence is accepted and reported.
    Require = 2,  ///< Only SYNTHETIC evidence is accepted (diagnostic federations).
};

[[nodiscard]] std::string_view to_string(SyntheticEvidencePolicy value) noexcept;
[[nodiscard]] bool synthetic_evidence_policy_from_token(std::string_view token, SyntheticEvidencePolicy& out) noexcept;
[[nodiscard]] bool synthetic_evidence_policy_from_wire(std::uint8_t raw, SyntheticEvidencePolicy& out) noexcept;

struct FederationPolicy {
    PolicyId id{};
    PolicyGeneration generation{};
    std::string name;

    /// Empty means "no vendor restriction".
    std::vector<std::string> allowed_vendors;
    std::vector<std::string> forbidden_vendors;
    std::vector<AcceleratorId> forbidden_accelerators;
    std::vector<std::string> deprecated_architectures;

    /// Applied to the "runtime.version" capability when a workload requires it.
    VersionRange minimum_runtime_version{};

    /// Maximum age of dynamic evidence, in nanoseconds. Zero disables the check.
    std::uint64_t required_evidence_freshness_nanos{0};

    SyntheticEvidencePolicy synthetic_evidence_policy{SyntheticEvidencePolicy::Allow};
    /// When true, a member whose evidence is not REAL may be admitted but is
    /// never reported as REAL and never satisfies a policy that demands REAL.
    bool require_real_evidence_for_active{false};

    /// Capabilities that every member must support to be admitted.
    std::vector<std::string> required_capabilities;
    /// Capabilities that no member may support. Also usable as a workload
    /// constraint via policy tags.
    std::vector<std::string> denied_capabilities;

    /// Portability classes that migration planning may use.
    std::vector<PortabilityClass> allowed_portability_classes;

    bool allow_cross_vendor_migration{false};
    bool allow_cross_vendor_reconstruction{true};
    bool allow_degraded_capabilities{false};

    std::uint64_t minimum_memory_bytes{0};

    /// Tags a member or workload may reference, e.g. "isolation.strong".
    std::vector<std::string> tags;

    [[nodiscard]] Status validate() const;

    /// Deterministic digest over the canonical policy definition.
    [[nodiscard]] Sha256::digest_type digest() const;

    /// Refresh the content-addressed identity and generation from the digest.
    void refresh_identity();

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, FederationPolicy& out, bool persisted_scale);

    /// Default policy used when a federation is created without an explicit one.
    [[nodiscard]] static FederationPolicy permissive_default();
};

}  // namespace haf

#endif  // HAF_MODEL_POLICY_HPP
