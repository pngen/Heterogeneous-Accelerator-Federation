#include "haf/engine/migration_planner.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "haf/engine/portability_engine.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

[[nodiscard]] CompatibilityReason refusal(ErrorCode code, std::string_view subject, std::string detail) {
    CompatibilityReason reason;
    reason.code = code;
    reason.subject = std::string(subject);
    reason.detail = std::move(detail);
    reason.strength = RequirementStrength::Hard;
    reason.observed = CapabilityState::Unknown;
    return reason;
}

[[nodiscard]] bool supports(const CapabilitySet* set, std::string_view key) {
    if (set == nullptr) {
        return false;
    }
    const CapabilityLookup lookup = set->lookup(key);
    return lookup.present && lookup.state == CapabilityState::Supported;
}

[[nodiscard]] MigrationStep step(std::string kind, std::string description, std::string required_capability) {
    MigrationStep result;
    result.kind = std::move(kind);
    result.description = std::move(description);
    result.required_capability = std::move(required_capability);
    return result;
}

}  // namespace

MigrationOutcome derive_migration_outcome(PortabilityClass portability, MigrationNeed need,
                                          bool reconstruction_allowed,
                                          bool destination_supports_reconstruction,
                                          bool checkpoint_supported_both_ways,
                                          bool live_transfer_supported_both_ways) {
    if (portability == PortabilityClass::Unsupported) {
        return MigrationOutcome::Unsupported;
    }
    if (portability == PortabilityClass::Unknown) {
        return MigrationOutcome::Unknown;
    }
    switch (need) {
        case MigrationNeed::LiveStateTransfer:
            if (portability == PortabilityClass::LiveMigrationSupported && live_transfer_supported_both_ways) {
                return MigrationOutcome::MoveNativeState;
            }
            return MigrationOutcome::Unsupported;
        case MigrationNeed::CheckpointRestore:
            if (portability == PortabilityClass::LiveMigrationSupported && live_transfer_supported_both_ways) {
                return MigrationOutcome::MoveNativeState;
            }
            if (checkpoint_supported_both_ways &&
                meets_portability_threshold(portability, PortabilityClass::CheckpointRestoreSupported)) {
                return MigrationOutcome::RestoreCheckpoint;
            }
            if (requires_binary_transformation(portability) && checkpoint_supported_both_ways) {
                return MigrationOutcome::RecompileThenRestore;
            }
            if (reconstruction_allowed && destination_supports_reconstruction) {
                return MigrationOutcome::Reconstruct;
            }
            return MigrationOutcome::Unsupported;
        case MigrationNeed::RestartOnly:
        case MigrationNeed::None:
            if (requires_state_reconstruction(portability)) {
                return reconstruction_allowed && destination_supports_reconstruction ? MigrationOutcome::Reconstruct
                                                                                    : MigrationOutcome::Unsupported;
            }
            if (requires_binary_transformation(portability)) {
                return MigrationOutcome::RepackageAndRestart;
            }
            return MigrationOutcome::Restart;
    }
    return MigrationOutcome::Unknown;
}

Result<MigrationPlan> plan_migration(const MigrationRequest& request) {
    if (request.policy == nullptr) {
        return Status(ErrorCode::InvalidArgument, "migration planning requires a federation policy");
    }
    if (request.workload == nullptr) {
        return Status(ErrorCode::InvalidArgument, "migration planning requires a workload profile");
    }
    if (request.source.capabilities == nullptr || request.destination.capabilities == nullptr) {
        return Status(ErrorCode::InvalidArgument, "migration planning requires capability evidence for both endpoints");
    }
    if (request.source.accelerator.is_nil() || request.destination.accelerator.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "migration planning requires both endpoint identities");
    }
    if (request.source.accelerator == request.destination.accelerator) {
        return Status(ErrorCode::InvalidArgument, "migration planning requires two distinct accelerators");
    }

    MigrationPlan plan;
    plan.federation = request.federation;
    plan.federation_generation = request.federation_generation;
    plan.epoch = request.epoch;
    plan.source = request.source.accelerator;
    plan.source_generation = request.source.device_generation;
    plan.source_capability_generation = request.source.capability_generation;
    plan.destination = request.destination.accelerator;
    plan.destination_generation = request.destination.device_generation;
    plan.destination_capability_generation = request.destination.capability_generation;
    plan.workload = request.workload->class_id;
    plan.workload_revision = request.workload_revision;
    plan.source_decision = request.source_decision;
    plan.destination_decision = request.destination_decision;
    plan.policy = request.policy->id;
    plan.policy_generation = request.policy_generation;
    plan.destination_provenance = request.destination.provenance;
    plan.created_at = now_timestamp();

    PortabilityRequest portability_request;
    portability_request.source_capabilities = request.source.capabilities;
    portability_request.destination_capabilities = request.destination.capabilities;
    portability_request.source_vendor = request.source.vendor;
    portability_request.destination_vendor = request.destination.vendor;
    portability_request.workload = request.workload;
    portability_request.policy_allows_cross_vendor_migration = request.policy->allow_cross_vendor_migration;
    const PortabilityResult portability = classify_portability(portability_request);
    plan.portability = portability.value;

    const bool same_vendor = !request.source.vendor.empty() && request.source.vendor == request.destination.vendor;
    const bool cross_vendor = !same_vendor;
    const bool live_both_ways = supports(request.source.capabilities, cap::kMigrationLiveStateTransfer) &&
                                supports(request.destination.capabilities, cap::kMigrationLiveStateTransfer);
    const bool checkpoint_both_ways = supports(request.source.capabilities, cap::kMigrationCheckpointRestore) &&
                                      supports(request.destination.capabilities, cap::kMigrationCheckpointRestore);
    const bool destination_reconstruction =
        supports(request.destination.capabilities, cap::kMigrationStateReconstruction);

    // ---- Refusals evaluated before any outcome is produced ---------------
    if (cross_vendor && !same_vendor) {
        const bool cross_vendor_state_supported = supports(request.source.capabilities, cap::kMigrationCrossVendorState) &&
                                                  supports(request.destination.capabilities, cap::kMigrationCrossVendorState);
        if (request.workload->migration_need == MigrationNeed::LiveStateTransfer) {
            if (!cross_vendor_state_supported || !request.policy->allow_cross_vendor_migration) {
                plan.refusal_reasons.push_back(refusal(
                    ErrorCode::MigrationUnsupported, cap::kMigrationCrossVendorState,
                    "live cross-vendor state transfer is not supported by both accelerators and permitted by policy"));
            }
        }
        if (requires_state_reconstruction(portability.value) && !request.policy->allow_cross_vendor_reconstruction) {
            plan.refusal_reasons.push_back(refusal(
                ErrorCode::MigrationUnsupported, cap::kMigrationStateReconstruction,
                "cross-vendor reconstruction is not permitted by the active policy"));
        }
    }
    if (portability.value == PortabilityClass::Unsupported) {
        plan.refusal_reasons.push_back(
            refusal(ErrorCode::PortabilityConstraintViolated, "portability",
                    "no portability path from source to destination"));
    }
    if (portability.value == PortabilityClass::Unknown) {
        plan.refusal_reasons.push_back(refusal(ErrorCode::UnknownCapability, "portability",
                                               "portability from source to destination cannot be established"));
    }
    if (request.policy->synthetic_evidence_policy == SyntheticEvidencePolicy::Reject &&
        request.destination.provenance != EvidenceProvenance::Real) {
        plan.refusal_reasons.push_back(refusal(ErrorCode::PolicyMismatch, "policy.synthetic_evidence",
                                               "policy rejects non-REAL evidence at the migration destination"));
    }
    if (!request.policy->allowed_portability_classes.empty() &&
        portability.value != PortabilityClass::Unknown && portability.value != PortabilityClass::Unsupported &&
        std::find(request.policy->allowed_portability_classes.begin(),
                  request.policy->allowed_portability_classes.end(),
                  portability.value) == request.policy->allowed_portability_classes.end()) {
        plan.refusal_reasons.push_back(refusal(ErrorCode::PortabilityConstraintViolated,
                                               "policy.allowed_portability_classes",
                                               "portability " + std::string(to_string(portability.value)) +
                                                   " is not permitted by the active policy"));
    }

    if (!plan.refusal_reasons.empty()) {
        plan.outcome = (portability.value == PortabilityClass::Unknown) ? MigrationOutcome::Unknown
                                                                        : MigrationOutcome::Unsupported;
        plan.state = MigrationState::Refused;
        sort_reasons(plan.refusal_reasons);
        refresh_migration_identity(plan);
        return plan;
    }

    // ---- Outcome ---------------------------------------------------------
    plan.outcome = derive_migration_outcome(portability.value, request.workload->migration_need,
                                            request.workload->reconstruction_allowed, destination_reconstruction,
                                            checkpoint_both_ways, live_both_ways);
    if (plan.outcome == MigrationOutcome::Unsupported || plan.outcome == MigrationOutcome::Unknown) {
        plan.refusal_reasons.push_back(
            refusal(plan.outcome == MigrationOutcome::Unknown ? ErrorCode::UnknownCapability
                                                              : ErrorCode::MigrationUnsupported,
                    "workload.migration_need",
                    "the workload requires " + std::string(to_string(request.workload->migration_need)) +
                        " but the destination supports only " + std::string(to_string(portability.value))));
        plan.state = MigrationState::Refused;
        sort_reasons(plan.refusal_reasons);
        refresh_migration_identity(plan);
        return plan;
    }

    // ---- Deterministic transformation steps ------------------------------
    switch (plan.outcome) {
        case MigrationOutcome::MoveNativeState:
            plan.steps.push_back(step("validate-live-transfer",
                                      "confirm both endpoints still support live state transfer",
                                      std::string(cap::kMigrationLiveStateTransfer)));
            plan.steps.push_back(step("transfer-state", "transfer execution state while the source stays authoritative",
                                      std::string(cap::kMigrationLiveStateTransfer)));
            break;
        case MigrationOutcome::RestoreCheckpoint:
            plan.steps.push_back(step("export-state", "export execution state from the source",
                                      std::string(cap::kMigrationCheckpointRestore)));
            plan.steps.push_back(step("restore-state", "restore the exported state on the destination",
                                      std::string(cap::kMigrationCheckpointRestore)));
            break;
        case MigrationOutcome::RecompileThenRestore:
            plan.steps.push_back(step("recompile", "rebuild the workload for the destination code object target",
                                      std::string(cap::kPortabilityRecompileAvailable)));
            plan.steps.push_back(step("export-state", "export execution state from the source",
                                      std::string(cap::kMigrationCheckpointRestore)));
            plan.steps.push_back(step("restore-state", "restore the exported state on the recompiled workload",
                                      std::string(cap::kMigrationCheckpointRestore)));
            break;
        case MigrationOutcome::RepackageAndRestart:
            plan.steps.push_back(step("repackage", "rebuild the runtime packaging for the destination",
                                      std::string(cap::kPortabilityRepackageAvailable)));
            plan.steps.push_back(step("restart", "start the workload fresh on the destination", std::string()));
            break;
        case MigrationOutcome::Reconstruct:
            plan.steps.push_back(step("reconstruct", "rebuild workload state from inputs or durable artifacts",
                                      std::string(cap::kMigrationStateReconstruction)));
            break;
        case MigrationOutcome::Restart:
            plan.steps.push_back(step("restart", "start the workload fresh on the destination", std::string()));
            break;
        case MigrationOutcome::Unsupported:
        case MigrationOutcome::Unknown:
            break;
    }
    std::sort(plan.steps.begin(), plan.steps.end(), [](const MigrationStep& a, const MigrationStep& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.description < b.description;
    });

    const std::uint32_t portability_cost = portability_transformation_steps(plan.portability);
    plan.estimated_cost = (portability_cost == 255 ? 1000U : portability_cost * 100U) +
                          static_cast<std::uint32_t>(plan.steps.size());
    plan.state = MigrationState::Planned;
    refresh_migration_identity(plan);
    return plan;
}

}  // namespace haf
