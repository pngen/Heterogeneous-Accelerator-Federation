#include "haf/model/compatibility.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "haf/core/limits.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {

std::string_view to_string(CompatibilityOutcome outcome) noexcept {
    switch (outcome) {
        case CompatibilityOutcome::Eligible: return "ELIGIBLE";
        case CompatibilityOutcome::Ineligible: return "INELIGIBLE";
        case CompatibilityOutcome::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool compatibility_outcome_from_wire(std::uint8_t raw, CompatibilityOutcome& out) noexcept {
    switch (raw) {
        case 0: out = CompatibilityOutcome::Eligible; return true;
        case 1: out = CompatibilityOutcome::Ineligible; return true;
        case 2: out = CompatibilityOutcome::Unknown; return true;
        default: return false;
    }
}

void CompatibilityReason::serialize(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.string(subject);
    writer.string(detail);
    writer.u8(static_cast<std::uint8_t>(strength));
    writer.u8(static_cast<std::uint8_t>(observed));
}

bool CompatibilityReason::deserialize(ByteReader& reader, CompatibilityReason& out) {
    CompatibilityReason reason;
    std::uint16_t raw_code = 0;
    std::uint8_t raw_strength = 0;
    std::uint8_t raw_observed = 0;
    if (!reader.u16(raw_code) || !reader.string(reason.subject, Limits::kMaxNameBytes) ||
        !reader.string(reason.detail, Limits::kMaxDescriptionBytes) || !reader.u8(raw_strength) ||
        !reader.u8(raw_observed)) {
        return false;
    }
    if (raw_code > static_cast<std::uint16_t>(ErrorCode::Unsupported)) {
        reader.fail(ErrorCode::MalformedData, "compatibility reason code is outside the declared domain");
        return false;
    }
    if (!requirement_strength_from_wire(raw_strength, reason.strength)) {
        reader.fail(ErrorCode::MalformedData, "compatibility reason strength is outside the declared domain");
        return false;
    }
    if (!capability_state_from_wire(raw_observed, reason.observed)) {
        reader.fail(ErrorCode::MalformedData, "compatibility reason observed state is outside the declared domain");
        return false;
    }
    reason.code = static_cast<ErrorCode>(raw_code);
    out = std::move(reason);
    return true;
}

Sha256::digest_type CompatibilityDecision::fingerprint() const {
    ByteWriter writer;
    writer.string("haf.decision.v1");
    writer.id128(federation.raw());
    writer.generation(federation_generation);
    writer.generation(epoch);
    writer.id128(accelerator.raw());
    writer.generation(device_generation);
    writer.generation(capability_generation);
    writer.generation(evidence_generation);
    writer.id128(policy.raw());
    writer.generation(policy_generation);
    writer.id128(workload.raw());
    writer.id128(workload_requirement.raw());
    writer.generation(workload_revision);
    writer.u8(static_cast<std::uint8_t>(outcome));
    writer.u8(static_cast<std::uint8_t>(portability));
    writer.u8(static_cast<std::uint8_t>(support_level));
    writer.u8(static_cast<std::uint8_t>(provenance));
    // The soft-preference score is part of the decision's content, so it is
    // covered by the fingerprint: altering a decision's ranking without
    // recomputing its identity is tampering.
    writer.i64(score);
    writer.u32(static_cast<std::uint32_t>(reasons.size()));
    for (const CompatibilityReason& reason : reasons) {
        reason.serialize(writer);
    }
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

std::string CompatibilityDecision::fingerprint_hex() const { return to_hex(fingerprint()); }

bool CompatibilityDecision::is_current(FederationGeneration current_federation_generation,
                                      CoordinatorEpoch current_epoch, PolicyGeneration current_policy_generation,
                                      DeviceGeneration current_device_generation,
                                      CapabilityGeneration current_capability_generation,
                                      EvidenceGeneration current_evidence_generation,
                                      WorkloadRevision current_workload_revision) const noexcept {
    return federation_generation == current_federation_generation && epoch == current_epoch &&
           policy_generation == current_policy_generation && device_generation == current_device_generation &&
           capability_generation == current_capability_generation &&
           evidence_generation == current_evidence_generation && workload_revision == current_workload_revision;
}

std::string CompatibilityDecision::render() const {
    std::string out;
    out += "outcome=";
    out += to_string(outcome);
    out += " accelerator=";
    out += accelerator.to_string();
    out += " workload=";
    out += workload.to_string();
    out += " portability=";
    out += to_string(portability);
    out += " support=";
    out += to_string(support_level);
    out += " evidence=";
    out += to_string(provenance);
    out += " score=";
    out += std::to_string(score);
    out += " fingerprint=";
    out += fingerprint_hex();
    for (const CompatibilityReason& reason : reasons) {
        out += "\n  [";
        out += to_string(reason.code);
        out += "] ";
        out += to_string(reason.strength);
        out += " ";
        out += reason.subject.empty() ? std::string("-") : reason.subject;
        out += ": ";
        out += reason.detail;
    }
    return out;
}

void CompatibilityDecision::serialize(ByteWriter& writer) const {
    writer.id128(id.raw());
    writer.id128(decision_id.raw());
    writer.generation(generation);
    writer.id128(federation.raw());
    writer.generation(federation_generation);
    writer.generation(epoch);
    writer.id128(accelerator.raw());
    writer.generation(device_generation);
    writer.generation(capability_generation);
    writer.generation(evidence_generation);
    writer.id128(policy.raw());
    writer.generation(policy_generation);
    writer.id128(workload.raw());
    writer.id128(workload_requirement.raw());
    writer.generation(workload_revision);
    writer.u8(static_cast<std::uint8_t>(outcome));
    writer.u8(static_cast<std::uint8_t>(portability));
    writer.u8(static_cast<std::uint8_t>(support_level));
    writer.u8(static_cast<std::uint8_t>(provenance));
    writer.i64(score);
    writer.u32(static_cast<std::uint32_t>(reasons.size()));
    for (const CompatibilityReason& reason : reasons) {
        reason.serialize(writer);
    }
    writer.i64(to_unix_nanos(created_at));
}

bool CompatibilityDecision::deserialize(ByteReader& reader, CompatibilityDecision& out, bool persisted_scale) {
    const std::uint32_t reason_limit =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxPersistedDecisions)
                        : static_cast<std::uint32_t>(Limits::kMaxRequirementsPerWorkload * 2U);
    CompatibilityDecision decision;
    Id128 raw_id;
    Id128 raw_decision;
    Id128 raw_federation;
    Id128 raw_accelerator;
    Id128 raw_policy;
    Id128 raw_workload;
    Id128 raw_requirement;
    std::uint8_t raw_outcome = 0;
    std::uint8_t raw_portability = 0;
    std::uint8_t raw_support = 0;
    std::uint8_t raw_provenance = 0;
    std::int64_t created_at = 0;
    if (!reader.id128(raw_id) || !reader.id128(raw_decision) || !reader.generation(decision.generation) ||
        !reader.id128(raw_federation) || !reader.generation(decision.federation_generation) ||
        !reader.generation(decision.epoch) || !reader.id128(raw_accelerator) ||
        !reader.generation(decision.device_generation) || !reader.generation(decision.capability_generation) ||
        !reader.generation(decision.evidence_generation) || !reader.id128(raw_policy) ||
        !reader.generation(decision.policy_generation) || !reader.id128(raw_workload) ||
        !reader.id128(raw_requirement) || !reader.generation(decision.workload_revision) ||
        !reader.u8(raw_outcome) || !reader.u8(raw_portability) || !reader.u8(raw_support) ||
        !reader.u8(raw_provenance) || !reader.i64(decision.score)) {
        return false;
    }
    if (!compatibility_outcome_from_wire(raw_outcome, decision.outcome)) {
        reader.fail(ErrorCode::MalformedData, "compatibility outcome is outside the declared domain");
        return false;
    }
    if (!portability_class_from_wire(raw_portability, decision.portability)) {
        reader.fail(ErrorCode::MalformedData, "portability class is outside the declared domain");
        return false;
    }
    if (!support_level_from_wire(raw_support, decision.support_level)) {
        reader.fail(ErrorCode::MalformedData, "support level is outside the declared domain");
        return false;
    }
    if (!evidence_provenance_from_wire(raw_provenance, decision.provenance)) {
        reader.fail(ErrorCode::MalformedData, "evidence provenance is outside the declared domain");
        return false;
    }
    std::uint32_t reason_count = 0;
    if (!reader.count(reason_count, reason_limit, 6)) {
        return false;
    }
    decision.reasons.reserve(reason_count);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        CompatibilityReason reason;
        if (!CompatibilityReason::deserialize(reader, reason)) {
            return false;
        }
        decision.reasons.push_back(std::move(reason));
    }
    if (!reader.i64(created_at)) {
        return false;
    }
    decision.id = CompatibilityDecisionId::from_raw(raw_id);
    decision.decision_id = DecisionId::from_raw(raw_decision);
    decision.federation = FederationId::from_raw(raw_federation);
    decision.accelerator = AcceleratorId::from_raw(raw_accelerator);
    decision.policy = PolicyId::from_raw(raw_policy);
    decision.workload = WorkloadClassId::from_raw(raw_workload);
    decision.workload_requirement = WorkloadRequirementId::from_raw(raw_requirement);
    decision.created_at = from_unix_nanos(created_at);
    const std::string expected_id = to_hex(decision.fingerprint());
    if (decision.decision_id != DecisionId::from_raw(derive_identity("haf.decision", expected_id))) {
        reader.fail(ErrorCode::IntegrityFailure, "compatibility decision identity does not match its fingerprint");
        return false;
    }
    out = std::move(decision);
    return true;
}

void refresh_decision_identity(CompatibilityDecision& decision) {
    const std::string hex = decision.fingerprint_hex();
    decision.decision_id = DecisionId::from_raw(derive_identity("haf.decision", hex));
    decision.id = CompatibilityDecisionId::from_raw(derive_identity("haf.compatibility-decision", hex));
    std::uint64_t folded = 0;
    for (const char c : hex) {
        folded = (folded * 131ULL) + static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        folded ^= folded >> 29;
    }
    decision.generation = DecisionGeneration(folded == 0 ? 1 : folded);
}

}  // namespace haf
