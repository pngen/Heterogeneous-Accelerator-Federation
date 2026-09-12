// Heterogeneous Accelerator Federation - compatibility decisions.
//
// A compatibility decision is reproducible and explainable. It binds the exact
// generations of everything it depended on, so any change to workload
// requirements, accelerator capability evidence, federation policy, device
// incarnation, or coordinator authority makes the decision stale by
// construction and it must be recomputed rather than reused.

#ifndef HAF_MODEL_COMPATIBILITY_HPP
#define HAF_MODEL_COMPATIBILITY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/evidence.hpp"
#include "haf/model/portability.hpp"
#include "haf/model/workload.hpp"

namespace haf {

enum class CompatibilityOutcome : std::uint8_t {
    Eligible = 0,
    Ineligible = 1,
    Unknown = 2,
};

[[nodiscard]] std::string_view to_string(CompatibilityOutcome outcome) noexcept;
[[nodiscard]] bool compatibility_outcome_from_wire(std::uint8_t raw, CompatibilityOutcome& out) noexcept;

/// One structured explanation entry. Reasons are ordered deterministically:
/// hard failures first in capability-name order, then soft notes, then policy.
struct CompatibilityReason {
    ErrorCode code{ErrorCode::CompatibilityRejection};
    /// Capability key or policy item the reason refers to. Empty for global reasons.
    std::string subject;
    std::string detail;
    RequirementStrength strength{RequirementStrength::Hard};
    CapabilityState observed{CapabilityState::Unknown};

    friend bool operator==(const CompatibilityReason&, const CompatibilityReason&) noexcept = default;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CompatibilityReason& out);
};

/// Complete, generation-bound result of evaluating one workload against one
/// accelerator.
struct CompatibilityDecision {
    CompatibilityDecisionId id{};
    DecisionId decision_id{};
    DecisionGeneration generation{};

    FederationId federation{};
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};

    AcceleratorId accelerator{};
    DeviceGeneration device_generation{};
    CapabilityGeneration capability_generation{};
    EvidenceGeneration evidence_generation{};

    PolicyGeneration policy_generation{};
    PolicyId policy{};

    WorkloadClassId workload{};
    WorkloadRequirementId workload_requirement{};
    WorkloadRevision workload_revision{};

    CompatibilityOutcome outcome{CompatibilityOutcome::Unknown};
    /// Portability from the accelerator the workload is native to. For an
    /// eligibility decision against a device this is the device-relative
    /// classification produced by the portability engine.
    PortabilityClass portability{PortabilityClass::Unknown};
    SupportLevel support_level{SupportLevel::Unknown};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};

    /// Deterministic soft-preference score. Only meaningful for eligible
    /// outcomes; an ineligible or unknown candidate is never ranked.
    std::int64_t score{0};

    std::vector<CompatibilityReason> reasons;

    Timestamp created_at{};

    /// Deterministic fingerprint over the bound generations, the outcome, and
    /// the ordered reasons. Two evaluators given identical inputs must produce
    /// identical fingerprints.
    [[nodiscard]] Sha256::digest_type fingerprint() const;
    [[nodiscard]] std::string fingerprint_hex() const;

    /// True when the decision is still bound to the supplied current state.
    [[nodiscard]] bool is_current(FederationGeneration current_federation_generation, CoordinatorEpoch current_epoch,
                                  PolicyGeneration current_policy_generation, DeviceGeneration current_device_generation,
                                  CapabilityGeneration current_capability_generation,
                                  EvidenceGeneration current_evidence_generation,
                                  WorkloadRevision current_workload_revision) const noexcept;

    /// Human-readable machine-stable rendering used by the CLI.
    [[nodiscard]] std::string render() const;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CompatibilityDecision& out, bool persisted_scale);
};

/// Refresh the content-addressed identity and fingerprint-derived id.
void refresh_decision_identity(CompatibilityDecision& decision);

}  // namespace haf

#endif  // HAF_MODEL_COMPATIBILITY_HPP
