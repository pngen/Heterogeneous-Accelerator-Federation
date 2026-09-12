// Heterogeneous Accelerator Federation - compatibility engine.
//
// For every (workload requirements, accelerator evidence) pair the engine
// produces a typed outcome and a structured, deterministic explanation:
//
//   ELIGIBLE    every hard requirement is positively satisfied.
//   INELIGIBLE  a hard requirement is positively violated.
//   UNKNOWN     evidence needed to decide a hard requirement is missing.
//
// UNKNOWN fails closed: an accelerator can never become eligible because it
// omitted evidence. The engine never consults process-global state, never reads
// the wall clock, and never iterates an unordered container, so identical
// inputs always produce identical decisions and identical fingerprints.

#ifndef HAF_ENGINE_COMPATIBILITY_ENGINE_HPP
#define HAF_ENGINE_COMPATIBILITY_ENGINE_HPP

#include <string>
#include <vector>

#include "haf/core/time.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/workload.hpp"

namespace haf {

/// The accelerator being evaluated.
struct EvaluationTarget {
    AcceleratorId accelerator{};
    DeviceGeneration device_generation{};
    CapabilityGeneration capability_generation{};
    EvidenceGeneration evidence_generation{};
    SupportLevel support_level{SupportLevel::Unknown};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    /// Capability evidence. Borrowed; must outlive the evaluation.
    const CapabilitySet* capabilities{nullptr};
    /// False when decay-prone evidence exceeded the policy freshness budget.
    bool evidence_fresh{true};
    /// False when the member is not in a state that accepts new work.
    bool accepts_new_work{true};
};

/// Optional origin accelerator. When supplied, the decision also classifies
/// portability from the origin to the target and enforces the workload's
/// portability and migration requirements.
struct EvaluationSource {
    AcceleratorId accelerator{};
    DeviceGeneration device_generation{};
    CapabilityGeneration capability_generation{};
    const CapabilitySet* capabilities{nullptr};
    std::string vendor;
};

struct EvaluationContext {
    FederationId federation{};
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};
    PolicyId policy_id{};
    PolicyGeneration policy_generation{};
    /// Borrowed; must outlive the evaluation.
    const FederationPolicy* policy{nullptr};
    WorkloadRevision workload_revision{};
    EvaluationTarget target;
    const EvaluationSource* source{nullptr};
    MonotonicTime now{};
};

/// Stable machine-readable rejection code for a capability whose hard
/// requirement was not met. Specific where the federation vocabulary allows it.
[[nodiscard]] ErrorCode rejection_code_for(std::string_view capability_name) noexcept;

/// Evaluate one workload against one accelerator.
[[nodiscard]] Result<CompatibilityDecision> evaluate_compatibility(const WorkloadProfile& workload,
                                                                   const EvaluationContext& context);

/// Evaluate a workload against every supplied target, in deterministic target
/// order, reusing the shared context fields. Targets are ordered by
/// AcceleratorId before evaluation so that the result is independent of the
/// caller's enumeration order.
struct MatrixCell {
    AcceleratorId accelerator{};
    CompatibilityDecision decision{};
};

struct FleetEvaluation {
    WorkloadClassId workload{};
    WorkloadRevision workload_revision{};
    std::vector<MatrixCell> cells;
    std::size_t eligible_count{0};
    std::size_t ineligible_count{0};
    std::size_t unknown_count{0};

    [[nodiscard]] std::string render() const;
};

[[nodiscard]] Result<FleetEvaluation> evaluate_fleet(const WorkloadProfile& workload, const EvaluationContext& context,
                                                     std::vector<EvaluationTarget> targets);

}  // namespace haf

#endif  // HAF_ENGINE_COMPATIBILITY_ENGINE_HPP
