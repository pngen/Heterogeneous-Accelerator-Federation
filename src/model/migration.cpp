#include "haf/model/migration.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include "haf/core/limits.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

constexpr MigrationState kLegalTransitions[][2] = {
    {MigrationState::Planned, MigrationState::Validated},
    {MigrationState::Planned, MigrationState::Aborted},
    {MigrationState::Planned, MigrationState::Refused},
    {MigrationState::Validated, MigrationState::Prepared},
    {MigrationState::Validated, MigrationState::Aborted},
    {MigrationState::Prepared, MigrationState::Transferring},
    {MigrationState::Prepared, MigrationState::Aborted},
    {MigrationState::Transferring, MigrationState::Verified},
    {MigrationState::Transferring, MigrationState::Aborted},
    {MigrationState::Verified, MigrationState::Committed},
    {MigrationState::Verified, MigrationState::Aborted},
};

}  // namespace

std::string_view to_string(MigrationOutcome outcome) noexcept {
    switch (outcome) {
        case MigrationOutcome::MoveNativeState: return "MOVE_NATIVE_STATE";
        case MigrationOutcome::RestoreCheckpoint: return "RESTORE_CHECKPOINT";
        case MigrationOutcome::RecompileThenRestore: return "RECOMPILE_THEN_RESTORE";
        case MigrationOutcome::RepackageAndRestart: return "REPACKAGE_AND_RESTART";
        case MigrationOutcome::Reconstruct: return "RECONSTRUCT";
        case MigrationOutcome::Restart: return "RESTART";
        case MigrationOutcome::Unsupported: return "UNSUPPORTED";
        case MigrationOutcome::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool migration_outcome_from_token(std::string_view token, MigrationOutcome& out) noexcept {
    if (token == "move_native_state") { out = MigrationOutcome::MoveNativeState; return true; }
    if (token == "restore_checkpoint") { out = MigrationOutcome::RestoreCheckpoint; return true; }
    if (token == "recompile_then_restore") { out = MigrationOutcome::RecompileThenRestore; return true; }
    if (token == "repackage_and_restart") { out = MigrationOutcome::RepackageAndRestart; return true; }
    if (token == "reconstruct") { out = MigrationOutcome::Reconstruct; return true; }
    if (token == "restart") { out = MigrationOutcome::Restart; return true; }
    if (token == "unsupported") { out = MigrationOutcome::Unsupported; return true; }
    if (token == "unknown") { out = MigrationOutcome::Unknown; return true; }
    return false;
}

bool migration_outcome_from_wire(std::uint8_t raw, MigrationOutcome& out) noexcept {
    if (raw > static_cast<std::uint8_t>(MigrationOutcome::Unknown)) {
        return false;
    }
    out = static_cast<MigrationOutcome>(raw);
    return true;
}

std::string_view to_string(MigrationState state) noexcept {
    switch (state) {
        case MigrationState::Planned: return "PLANNED";
        case MigrationState::Validated: return "VALIDATED";
        case MigrationState::Prepared: return "PREPARED";
        case MigrationState::Transferring: return "TRANSFERRING";
        case MigrationState::Verified: return "VERIFIED";
        case MigrationState::Committed: return "COMMITTED";
        case MigrationState::Aborted: return "ABORTED";
        case MigrationState::Refused: return "REFUSED";
    }
    return "UNKNOWN";
}

bool migration_state_from_wire(std::uint8_t raw, MigrationState& out) noexcept {
    if (raw > static_cast<std::uint8_t>(MigrationState::Refused)) {
        return false;
    }
    out = static_cast<MigrationState>(raw);
    return true;
}

bool is_legal_migration_transition(MigrationState from, MigrationState to) noexcept {
    if (from == to) {
        return false;
    }
    for (const auto& transition : kLegalTransitions) {
        if (transition[0] == from && transition[1] == to) {
            return true;
        }
    }
    return false;
}

bool is_terminal_migration_state(MigrationState state) noexcept {
    return state == MigrationState::Committed || state == MigrationState::Aborted || state == MigrationState::Refused;
}

void MigrationStep::serialize(ByteWriter& writer) const {
    writer.string(kind);
    writer.string(description);
    writer.string(required_capability);
}

bool MigrationStep::deserialize(ByteReader& reader, MigrationStep& out) {
    MigrationStep step;
    if (!reader.string(step.kind, Limits::kMaxTokenBytes) ||
        !reader.string(step.description, Limits::kMaxDescriptionBytes) ||
        !reader.string(step.required_capability, Limits::kMaxNameBytes)) {
        return false;
    }
    if (step.kind.empty()) {
        reader.fail(ErrorCode::MalformedData, "migration step has no kind");
        return false;
    }
    out = std::move(step);
    return true;
}

Sha256::digest_type MigrationPlan::fingerprint() const {
    ByteWriter writer;
    writer.string("haf.migration-plan.v1");
    writer.id128(federation.raw());
    writer.generation(federation_generation);
    writer.generation(epoch);
    writer.id128(source.raw());
    writer.generation(source_generation);
    writer.generation(source_capability_generation);
    writer.id128(destination.raw());
    writer.generation(destination_generation);
    writer.generation(destination_capability_generation);
    writer.id128(workload.raw());
    writer.generation(workload_revision);
    writer.id128(source_decision.raw());
    writer.id128(destination_decision.raw());
    writer.u8(static_cast<std::uint8_t>(portability));
    writer.u8(static_cast<std::uint8_t>(outcome));
    // The lifecycle position (state) is deliberately excluded: advancing a plan
    // through Validated/Prepared/Verified must not change its identity, so a
    // reference taken before a transition is still recognisable after it.
    writer.id128(policy.raw());
    writer.generation(policy_generation);
    writer.u8(static_cast<std::uint8_t>(destination_provenance));
    writer.u32(estimated_cost);
    writer.u32(static_cast<std::uint32_t>(steps.size()));
    for (const MigrationStep& step : steps) {
        step.serialize(writer);
    }
    // Refusal explanations are deliberately excluded for the same reason: a
    // plan can acquire a later explanation (for example, being aborted because
    // coordinator authority advanced) without becoming a different plan. A
    // fingerprint identifies what must happen and under whose authority, not
    // which explanations have been attached to it so far.
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

std::string MigrationPlan::fingerprint_hex() const { return to_hex(fingerprint()); }

bool MigrationPlan::is_current(CoordinatorEpoch current_epoch, PolicyGeneration current_policy_generation,
                               DeviceGeneration current_source_generation,
                               DeviceGeneration current_destination_generation,
                               CapabilityGeneration current_source_capability,
                               CapabilityGeneration current_destination_capability,
                               WorkloadRevision current_workload_revision) const noexcept {
    return epoch == current_epoch &&
           policy_generation == current_policy_generation && source_generation == current_source_generation &&
           destination_generation == current_destination_generation &&
           source_capability_generation == current_source_capability &&
           destination_capability_generation == current_destination_capability &&
           workload_revision == current_workload_revision;
}

std::string MigrationPlan::render() const {
    std::string out;
    out += "outcome=";
    out += to_string(outcome);
    out += " state=";
    out += to_string(state);
    out += " portability=";
    out += to_string(portability);
    out += " source=";
    out += source.to_string();
    out += " destination=";
    out += destination.to_string();
    out += " destination_evidence=";
    out += to_string(destination_provenance);
    out += " estimated_cost=";
    out += std::to_string(estimated_cost);
    out += " fingerprint=";
    out += fingerprint_hex();
    for (const MigrationStep& step : steps) {
        out += "\n  step ";
        out += step.kind;
        out += ": ";
        out += step.description;
        if (!step.required_capability.empty()) {
            out += " [requires ";
            out += step.required_capability;
            out += "]";
        }
    }
    for (const CompatibilityReason& reason : refusal_reasons) {
        out += "\n  refused [";
        out += to_string(reason.code);
        out += "] ";
        out += reason.subject.empty() ? std::string("-") : reason.subject;
        out += ": ";
        out += reason.detail;
    }
    return out;
}

void MigrationPlan::serialize(ByteWriter& writer) const {
    // Note: the fingerprint deliberately excludes the lifecycle position, but
    // the persisted form must include it so that a restart can see where the
    // plan actually was.
    writer.id128(id.raw());
    writer.generation(generation);
    writer.id128(federation.raw());
    writer.generation(federation_generation);
    writer.generation(epoch);
    writer.id128(source.raw());
    writer.generation(source_generation);
    writer.generation(source_capability_generation);
    writer.id128(destination.raw());
    writer.generation(destination_generation);
    writer.generation(destination_capability_generation);
    writer.id128(workload.raw());
    writer.generation(workload_revision);
    writer.id128(source_decision.raw());
    writer.id128(destination_decision.raw());
    writer.u8(static_cast<std::uint8_t>(portability));
    writer.u8(static_cast<std::uint8_t>(outcome));
    // The lifecycle position is persisted even though it is excluded from the
    // fingerprint: advancing a plan must not change its identity, but a restart
    // must still see where the plan was.
    writer.u8(static_cast<std::uint8_t>(state));
    // The lifecycle position (state) is deliberately excluded: advancing a plan
    // through Validated/Prepared/Verified must not change its identity or its
    // generation, so a stale reference is still recognisable after a transition.
    writer.id128(policy.raw());
    writer.generation(policy_generation);
    writer.u8(static_cast<std::uint8_t>(destination_provenance));
    writer.u32(estimated_cost);
    writer.u32(static_cast<std::uint32_t>(steps.size()));
    for (const MigrationStep& step : steps) {
        step.serialize(writer);
    }
    writer.u32(static_cast<std::uint32_t>(refusal_reasons.size()));
    for (const CompatibilityReason& reason : refusal_reasons) {
        reason.serialize(writer);
    }
    writer.i64(to_unix_nanos(created_at));
}

bool MigrationPlan::deserialize(ByteReader& reader, MigrationPlan& out, bool persisted_scale) {
    const std::uint32_t step_limit =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxRequirementsPerWorkload)
                        : static_cast<std::uint32_t>(Limits::kMaxRequirementsPerWorkload);
    MigrationPlan plan;
    Id128 raw_id;
    Id128 raw_federation;
    Id128 raw_source;
    Id128 raw_destination;
    Id128 raw_workload;
    Id128 raw_source_decision;
    Id128 raw_destination_decision;
    Id128 raw_policy;
    std::uint8_t raw_portability = 0;
    std::uint8_t raw_outcome = 0;
    std::uint8_t raw_state = 0;
    std::uint8_t raw_provenance = 0;
    std::int64_t created_at = 0;
    if (!reader.id128(raw_id) || !reader.generation(plan.generation) || !reader.id128(raw_federation) ||
        !reader.generation(plan.federation_generation) || !reader.generation(plan.epoch) ||
        !reader.id128(raw_source) || !reader.generation(plan.source_generation) ||
        !reader.generation(plan.source_capability_generation) || !reader.id128(raw_destination) ||
        !reader.generation(plan.destination_generation) || !reader.generation(plan.destination_capability_generation) ||
        !reader.id128(raw_workload) || !reader.generation(plan.workload_revision) ||
        !reader.id128(raw_source_decision) || !reader.id128(raw_destination_decision) ||
        !reader.u8(raw_portability) || !reader.u8(raw_outcome) || !reader.u8(raw_state) ||
        !reader.id128(raw_policy) || !reader.generation(plan.policy_generation) || !reader.u8(raw_provenance) ||
        !reader.u32(plan.estimated_cost)) {
        return false;
    }
    if (!portability_class_from_wire(raw_portability, plan.portability)) {
        reader.fail(ErrorCode::MalformedData, "portability class is outside the declared domain");
        return false;
    }
    if (!migration_outcome_from_wire(raw_outcome, plan.outcome)) {
        reader.fail(ErrorCode::MalformedData, "migration outcome is outside the declared domain");
        return false;
    }
    if (!migration_state_from_wire(raw_state, plan.state)) {
        reader.fail(ErrorCode::MalformedData, "migration state is outside the declared domain");
        return false;
    }
    if (!evidence_provenance_from_wire(raw_provenance, plan.destination_provenance)) {
        reader.fail(ErrorCode::MalformedData, "evidence provenance is outside the declared domain");
        return false;
    }
    std::uint32_t step_count = 0;
    if (!reader.count(step_count, step_limit, 8)) {
        return false;
    }
    plan.steps.reserve(step_count);
    for (std::uint32_t i = 0; i < step_count; ++i) {
        MigrationStep step;
        if (!MigrationStep::deserialize(reader, step)) {
            return false;
        }
        plan.steps.push_back(std::move(step));
    }
    std::uint32_t reason_count = 0;
    if (!reader.count(reason_count, step_limit, 6)) {
        return false;
    }
    plan.refusal_reasons.reserve(reason_count);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        CompatibilityReason reason;
        if (!CompatibilityReason::deserialize(reader, reason)) {
            return false;
        }
        plan.refusal_reasons.push_back(std::move(reason));
    }
    if (!reader.i64(created_at)) {
        return false;
    }
    plan.id = MigrationPlanId::from_raw(raw_id);
    plan.federation = FederationId::from_raw(raw_federation);
    plan.source = AcceleratorId::from_raw(raw_source);
    plan.destination = AcceleratorId::from_raw(raw_destination);
    plan.workload = WorkloadClassId::from_raw(raw_workload);
    plan.source_decision = CompatibilityDecisionId::from_raw(raw_source_decision);
    plan.destination_decision = CompatibilityDecisionId::from_raw(raw_destination_decision);
    plan.policy = PolicyId::from_raw(raw_policy);
    plan.created_at = from_unix_nanos(created_at);
    if (plan.id != MigrationPlanId::from_raw(derive_identity("haf.migration-plan", plan.fingerprint_hex()))) {
        reader.fail(ErrorCode::IntegrityFailure, "migration plan identity does not match its fingerprint");
        return false;
    }
    out = std::move(plan);
    return true;
}

void refresh_migration_identity(MigrationPlan& plan) {
    const std::string hex = plan.fingerprint_hex();
    plan.id = MigrationPlanId::from_raw(derive_identity("haf.migration-plan", hex));
    std::uint64_t folded = 0;
    for (const char c : hex) {
        folded = (folded * 131ULL) + static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        folded ^= folded >> 29;
    }
    plan.generation = MigrationGeneration(folded == 0 ? 1 : folded);
}

}  // namespace haf
