// Heterogeneous Accelerator Federation - migration and reconstruction plans.
//
// The federation never claims that accelerator state can move merely because a
// workload can be restarted elsewhere. A plan states exactly which mechanism is
// available, binds every generation it depends on, and names the authority that
// would commit the move.
//
// The transactional lifecycle is:
//
//   Planned -> Validated -> Prepared -> Transferring -> Verified -> Committed
//
// with Aborted reachable from any pre-commit state and Refused as the terminal
// outcome of a rejected plan. Authority is committed only at Committed; a
// failure before that point must never leave two authoritative executions.

#ifndef HAF_MODEL_MIGRATION_HPP
#define HAF_MODEL_MIGRATION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/portability.hpp"

namespace haf {

enum class MigrationOutcome : std::uint8_t {
    MoveNativeState = 0,
    RestoreCheckpoint = 1,
    RecompileThenRestore = 2,
    RepackageAndRestart = 3,
    Reconstruct = 4,
    Restart = 5,
    Unsupported = 6,
    Unknown = 7,
};

[[nodiscard]] std::string_view to_string(MigrationOutcome outcome) noexcept;
[[nodiscard]] bool migration_outcome_from_token(std::string_view token, MigrationOutcome& out) noexcept;
[[nodiscard]] bool migration_outcome_from_wire(std::uint8_t raw, MigrationOutcome& out) noexcept;

enum class MigrationState : std::uint8_t {
    Planned = 0,
    Validated = 1,
    Prepared = 2,
    Transferring = 3,
    Verified = 4,
    Committed = 5,
    Aborted = 6,
    Refused = 7,
};

[[nodiscard]] std::string_view to_string(MigrationState state) noexcept;
[[nodiscard]] bool migration_state_from_wire(std::uint8_t raw, MigrationState& out) noexcept;
[[nodiscard]] bool is_legal_migration_transition(MigrationState from, MigrationState to) noexcept;
[[nodiscard]] bool is_terminal_migration_state(MigrationState state) noexcept;

/// One concrete action the workload/platform must take before the move is valid.
struct MigrationStep {
    std::string kind;        ///< e.g. "recompile", "export-state", "stage-artifact"
    std::string description;
    /// Capability that must be SUPPORTED on the destination for this step.
    std::string required_capability;

    friend bool operator==(const MigrationStep&, const MigrationStep&) noexcept = default;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, MigrationStep& out);
};

struct MigrationPlan {
    MigrationPlanId id{};
    MigrationGeneration generation{};

    FederationId federation{};
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};

    AcceleratorId source{};
    DeviceGeneration source_generation{};
    CapabilityGeneration source_capability_generation{};

    AcceleratorId destination{};
    DeviceGeneration destination_generation{};
    CapabilityGeneration destination_capability_generation{};

    WorkloadClassId workload{};
    WorkloadRevision workload_revision{};

    CompatibilityDecisionId source_decision{};
    CompatibilityDecisionId destination_decision{};

    PortabilityClass portability{PortabilityClass::Unknown};
    MigrationOutcome outcome{MigrationOutcome::Unknown};
    MigrationState state{MigrationState::Planned};

    PolicyGeneration policy_generation{};
    PolicyId policy{};

    /// Required transformations, in deterministic order.
    std::vector<MigrationStep> steps;
    /// Evidence provenance of the destination.
    EvidenceProvenance destination_provenance{EvidenceProvenance::Unsupported};

    /// Relative cost estimate used by deterministic ranking. Higher is more
    /// expensive. Derived only from the portability class and step count, so it
    /// is reproducible and cannot encode hidden policy.
    std::uint32_t estimated_cost{0};

    std::vector<CompatibilityReason> refusal_reasons;

    Timestamp created_at{};

    /// Deterministic digest of what this plan requires and the authority it
    /// was authored under: both endpoints and their generations, the workload
    /// revision, the referenced decisions, the portability class, the outcome,
    /// the policy generation, the destination evidence provenance, the cost
    /// estimate, and the required transformation steps.
    ///
    /// The lifecycle position and the refusal explanations are excluded, so a
    /// plan keeps one identity while it advances and while explanations are
    /// attached to it.
    [[nodiscard]] Sha256::digest_type fingerprint() const;
    [[nodiscard]] std::string fingerprint_hex() const;

    /// Currency is defined by the generations the plan actually depends on:
    /// coordinator epoch, active policy, both endpoint incarnations and
    /// capability sets, and the workload revision. The federation-wide
    /// generation counter is deliberately excluded because advancing a plan is
    /// itself a federation mutation; including it would make every plan stale
    /// the instant it was validated.
    [[nodiscard]] bool is_current(CoordinatorEpoch current_epoch, PolicyGeneration current_policy_generation,
                                  DeviceGeneration current_source_generation,
                                  DeviceGeneration current_destination_generation,
                                  CapabilityGeneration current_source_capability,
                                  CapabilityGeneration current_destination_capability,
                                  WorkloadRevision current_workload_revision) const noexcept;

    [[nodiscard]] std::string render() const;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, MigrationPlan& out, bool persisted_scale);
};

void refresh_migration_identity(MigrationPlan& plan);

}  // namespace haf

#endif  // HAF_MODEL_MIGRATION_HPP
