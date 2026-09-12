// Heterogeneous Accelerator Federation - migration planning.
//
// A plan is a statement about what the federation knows, not a promise that a
// transfer will succeed. The planner refuses rather than guesses:
//
//   * no portability path            -> UNSUPPORTED / REFUSED
//   * portability evidence missing   -> UNKNOWN / REFUSED
//   * live transfer demanded but the destination cannot do it -> refused
//   * cross-vendor state movement without explicit support and policy consent
//                                    -> refused
//
// A committed plan is the only place where authority moves. A failure before
// commit must leave exactly one authoritative execution.

#ifndef HAF_ENGINE_MIGRATION_PLANNER_HPP
#define HAF_ENGINE_MIGRATION_PLANNER_HPP

#include <string>
#include <vector>

#include "haf/core/time.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/migration.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/workload.hpp"

namespace haf {

struct MigrationEndpoint {
    AcceleratorId accelerator{};
    DeviceGeneration device_generation{};
    CapabilityGeneration capability_generation{};
    const CapabilitySet* capabilities{nullptr};
    std::string vendor;
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    SupportLevel support_level{SupportLevel::Unknown};
};

struct MigrationRequest {
    FederationId federation{};
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};
    PolicyId policy_id{};
    PolicyGeneration policy_generation{};
    const FederationPolicy* policy{nullptr};

    MigrationEndpoint source;
    MigrationEndpoint destination;
    const WorkloadProfile* workload{nullptr};
    WorkloadRevision workload_revision{};

    CompatibilityDecisionId source_decision{};
    CompatibilityDecisionId destination_decision{};
};

/// Build a fully generation-bound migration plan. Never returns a plan in a
/// state beyond Planned; advancing the plan through its lifecycle is the job of
/// the federation runtime, which owns the authority checks.
[[nodiscard]] Result<MigrationPlan> plan_migration(const MigrationRequest& request);

/// Map a portability class and migration need to a plan outcome without
/// building a full plan. Exposed so tests and the CLI can explain the mapping.
[[nodiscard]] MigrationOutcome derive_migration_outcome(PortabilityClass portability, MigrationNeed need,
                                                        bool reconstruction_allowed,
                                                        bool destination_supports_reconstruction,
                                                        bool checkpoint_supported_both_ways,
                                                        bool live_transfer_supported_both_ways);

}  // namespace haf

#endif  // HAF_ENGINE_MIGRATION_PLANNER_HPP
