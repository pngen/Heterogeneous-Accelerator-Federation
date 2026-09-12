#include "haf/engine/compatibility_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

[[nodiscard]] CompatibilityReason reason(ErrorCode code, std::string_view subject, std::string detail,
                                         RequirementStrength strength, CapabilityState observed) {
    CompatibilityReason result;
    result.code = code;
    result.subject = std::string(subject);
    result.detail = std::move(detail);
    result.strength = strength;
    result.observed = observed;
    return result;
}

/// Deterministic processing order for requirements.
void canonicalize_requirements(const WorkloadProfile& workload, std::vector<const CapabilityRequirement*>& out) {
    out.clear();
    out.reserve(workload.requirements.size());
    for (const CapabilityRequirement& requirement : workload.requirements) {
        out.push_back(&requirement);
    }
    std::sort(out.begin(), out.end(), [](const CapabilityRequirement* a, const CapabilityRequirement* b) {
        if (a->capability != b->capability) {
            return a->capability < b->capability;
        }
        if (a->strength != b->strength) {
            return static_cast<std::uint8_t>(a->strength) < static_cast<std::uint8_t>(b->strength);
        }
        if (a->op != b->op) {
            return static_cast<std::uint8_t>(a->op) < static_cast<std::uint8_t>(b->op);
        }
        return a->value.render() < b->value.render();
    });
}

[[nodiscard]] bool numeric_at_least(const CapabilityValue& provided, const CapabilityValue& required) {
    if (provided.kind() == CapabilityKind::Integer && required.kind() == CapabilityKind::Integer) {
        return provided.integer_value() >= required.integer_value();
    }
    if (provided.kind() == CapabilityKind::Scalar && required.kind() == CapabilityKind::Scalar) {
        return provided.scalar_value() >= required.scalar_value();
    }
    if (provided.kind() == CapabilityKind::Integer && required.kind() == CapabilityKind::Scalar) {
        return static_cast<double>(provided.integer_value()) >= required.scalar_value();
    }
    if (provided.kind() == CapabilityKind::Scalar && required.kind() == CapabilityKind::Integer) {
        return provided.scalar_value() >= static_cast<double>(required.integer_value());
    }
    if (provided.kind() == CapabilityKind::Version && required.kind() == CapabilityKind::Version) {
        return provided.version_value() >= required.version_value();
    }
    return false;
}

[[nodiscard]] bool numeric_at_most(const CapabilityValue& provided, const CapabilityValue& required) {
    if (provided.kind() == CapabilityKind::Integer && required.kind() == CapabilityKind::Integer) {
        return provided.integer_value() <= required.integer_value();
    }
    if (provided.kind() == CapabilityKind::Scalar && required.kind() == CapabilityKind::Scalar) {
        return provided.scalar_value() <= required.scalar_value();
    }
    if (provided.kind() == CapabilityKind::Integer && required.kind() == CapabilityKind::Scalar) {
        return static_cast<double>(provided.integer_value()) <= required.scalar_value();
    }
    if (provided.kind() == CapabilityKind::Scalar && required.kind() == CapabilityKind::Integer) {
        return provided.scalar_value() <= static_cast<double>(required.integer_value());
    }
    if (provided.kind() == CapabilityKind::Version && required.kind() == CapabilityKind::Version) {
        return provided.version_value() <= required.version_value();
    }
    return false;
}

[[nodiscard]] std::vector<std::string> as_token_vector(const CapabilityValue& value) {
    if (value.kind() == CapabilityKind::TokenSet) {
        return value.tokens();
    }
    if (value.kind() == CapabilityKind::Enumeration) {
        return {value.token()};
    }
    return {};
}

enum class MatchOutcome { Satisfied, Violated, Indeterminate };

[[nodiscard]] MatchOutcome match_requirement(const CapabilityRequirement& requirement, const CapabilityLookup& lookup,
                                             bool allow_degraded) {
    const CapabilityState state = lookup.state;
    if (state == CapabilityState::Unknown) {
        return MatchOutcome::Indeterminate;
    }
    if (state == CapabilityState::Degraded && !allow_degraded) {
        return MatchOutcome::Violated;
    }
    if (requirement.op == RequirementOperator::Absent) {
        // Absence can only be positively proven by an explicit UNSUPPORTED.
        return state == CapabilityState::Unsupported ? MatchOutcome::Satisfied : MatchOutcome::Violated;
    }
    if (state == CapabilityState::Unsupported) {
        return MatchOutcome::Violated;
    }
    if (lookup.value == nullptr) {
        return MatchOutcome::Indeterminate;
    }
    const CapabilityValue& provided = *lookup.value;
    switch (requirement.op) {
        case RequirementOperator::Present:
            return MatchOutcome::Satisfied;
        case RequirementOperator::Absent:
            return MatchOutcome::Violated;
        case RequirementOperator::Equals:
            return provided.equals(requirement.value) ? MatchOutcome::Satisfied : MatchOutcome::Violated;
        case RequirementOperator::NotEquals:
            return provided.equals(requirement.value) ? MatchOutcome::Violated : MatchOutcome::Satisfied;
        case RequirementOperator::AtLeast:
            return numeric_at_least(provided, requirement.value) ? MatchOutcome::Satisfied : MatchOutcome::Violated;
        case RequirementOperator::AtMost:
            return numeric_at_most(provided, requirement.value) ? MatchOutcome::Satisfied : MatchOutcome::Violated;
        case RequirementOperator::GreaterThan:
            return (numeric_at_least(provided, requirement.value) && !provided.equals(requirement.value))
                       ? MatchOutcome::Satisfied
                       : MatchOutcome::Violated;
        case RequirementOperator::LessThan:
            return (numeric_at_most(provided, requirement.value) && !provided.equals(requirement.value))
                       ? MatchOutcome::Satisfied
                       : MatchOutcome::Violated;
        case RequirementOperator::InSet: {
            const std::vector<std::string> allowed = as_token_vector(requirement.value);
            const std::vector<std::string> actual = as_token_vector(provided);
            if (actual.empty()) {
                return MatchOutcome::Violated;
            }
            for (const std::string& token : actual) {
                if (std::find(allowed.begin(), allowed.end(), token) == allowed.end()) {
                    return MatchOutcome::Violated;
                }
            }
            return MatchOutcome::Satisfied;
        }
        case RequirementOperator::SupersetOf: {
            const std::vector<std::string> required_tokens = as_token_vector(requirement.value);
            const std::vector<std::string> actual = as_token_vector(provided);
            for (const std::string& token : required_tokens) {
                if (std::find(actual.begin(), actual.end(), token) == actual.end()) {
                    return MatchOutcome::Violated;
                }
            }
            return MatchOutcome::Satisfied;
        }
        case RequirementOperator::SubsetOf: {
            const std::vector<std::string> allowed = as_token_vector(requirement.value);
            const std::vector<std::string> actual = as_token_vector(provided);
            for (const std::string& token : actual) {
                if (std::find(allowed.begin(), allowed.end(), token) == allowed.end()) {
                    return MatchOutcome::Violated;
                }
            }
            return MatchOutcome::Satisfied;
        }
        case RequirementOperator::IntersectsWith: {
            const std::vector<std::string> required_tokens = as_token_vector(requirement.value);
            const std::vector<std::string> actual = as_token_vector(provided);
            for (const std::string& token : actual) {
                if (std::find(required_tokens.begin(), required_tokens.end(), token) != required_tokens.end()) {
                    return MatchOutcome::Satisfied;
                }
            }
            return MatchOutcome::Violated;
        }
        case RequirementOperator::VersionSatisfies:
            if (provided.kind() != CapabilityKind::Version) {
                return MatchOutcome::Indeterminate;
            }
            return requirement.version_range.matches(provided.version_value()) ? MatchOutcome::Satisfied
                                                                               : MatchOutcome::Violated;
    }
    return MatchOutcome::Indeterminate;
}

[[nodiscard]] std::string describe_expectation(const CapabilityRequirement& requirement) {
    std::string out(to_string(requirement.op));
    if (requirement.op == RequirementOperator::VersionSatisfies) {
        out += " ";
        out += requirement.version_range.to_string();
    } else if (requirement.op != RequirementOperator::Present && requirement.op != RequirementOperator::Absent) {
        out += " ";
        out += requirement.value.render();
    }
    return out;
}

[[nodiscard]] bool token_in(const std::vector<std::string>& values, std::string_view token) {
    return std::find(values.begin(), values.end(), token) != values.end();
}

/// Reads an enumeration capability as a canonical token.
[[nodiscard]] std::string enumeration_token(const CapabilitySet& set, std::string_view key) {
    const CapabilityLookup lookup = set.lookup(key);
    if (lookup.present && lookup.value != nullptr && lookup.state == CapabilityState::Supported) {
        return lookup.value->render();
    }
    return {};
}

}  // namespace

ErrorCode rejection_code_for(std::string_view capability_name) noexcept {
    if (capability_name == cap::kVendorId) {
        return ErrorCode::VendorMismatch;
    }
    if (capability_name == cap::kArchitectureFamily || capability_name == cap::kArchitectureDeviceGeneration) {
        return ErrorCode::ArchitectureMismatch;
    }
    if (capability_name == cap::kRuntimeFamily) {
        return ErrorCode::RuntimeMismatch;
    }
    if (capability_name == cap::kRuntimeVersion || capability_name == cap::kDriverVersion) {
        return ErrorCode::RuntimeVersionUnsupported;
    }
    if (capability_name == cap::kIsaCodeObjectTargets || capability_name == cap::kComputeCapability) {
        return ErrorCode::IsaIncompatible;
    }
    if (capability_name == cap::kMemoryTotalBytes || capability_name == cap::kMemoryFreeBytes) {
        return ErrorCode::InsufficientMemory;
    }
    if (capability_name.rfind("numeric.", 0) == 0) {
        return ErrorCode::NumericModeUnsupported;
    }
    if (capability_name.rfind("migration.", 0) == 0 || capability_name.rfind("portability.", 0) == 0) {
        return ErrorCode::MigrationUnsupported;
    }
    return ErrorCode::UnsupportedCapability;
}

Result<CompatibilityDecision> evaluate_compatibility(const WorkloadProfile& workload,
                                                     const EvaluationContext& context) {
    const Status workload_status = workload.validate();
    if (!workload_status.ok()) {
        return workload_status;
    }
    if (context.policy == nullptr) {
        return Status(ErrorCode::InvalidArgument, "compatibility evaluation requires a federation policy");
    }
    if (context.target.capabilities == nullptr) {
        return Status(ErrorCode::InvalidArgument, "compatibility evaluation requires target capability evidence");
    }
    if (context.target.accelerator.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "compatibility evaluation requires a target accelerator identity");
    }
    const Status policy_status = context.policy->validate();
    if (!policy_status.ok()) {
        return policy_status;
    }
    const CapabilitySet& capabilities = *context.target.capabilities;
    const FederationPolicy& policy = *context.policy;

    CompatibilityDecision decision;
    decision.federation = context.federation;
    decision.federation_generation = context.federation_generation;
    decision.epoch = context.epoch;
    decision.policy = context.policy->id;
    decision.policy_generation = context.policy_generation;
    decision.accelerator = context.target.accelerator;
    decision.device_generation = context.target.device_generation;
    decision.capability_generation = context.target.capability_generation;
    decision.evidence_generation = context.target.evidence_generation;
    decision.workload = workload.class_id;
    decision.workload_requirement = workload.requirement_id;
    decision.workload_revision = context.workload_revision;
    decision.support_level = context.target.support_level;
    decision.provenance = context.target.provenance;
    decision.created_at = now_timestamp();

    bool has_hard_failure = false;
    bool has_indeterminate = false;
    std::int64_t score = 0;

    // ---- Hard and soft capability requirements ---------------------------
    std::vector<const CapabilityRequirement*> ordered;
    canonicalize_requirements(workload, ordered);
    for (const CapabilityRequirement* requirement_ptr : ordered) {
        const CapabilityRequirement& requirement = *requirement_ptr;
        const CapabilityLookup lookup = capabilities.lookup(requirement.capability);
        const bool dynamic = is_dynamic_capability(requirement.capability);
        if (dynamic && !context.target.evidence_fresh) {
            if (requirement.strength == RequirementStrength::Hard) {
                has_indeterminate = true;
                decision.reasons.push_back(reason(ErrorCode::StaleEvidence, requirement.capability,
                                                  "required capability is decay-prone and its evidence exceeded the policy freshness budget",
                                                  requirement.strength, lookup.state));
            } else {
                decision.reasons.push_back(reason(ErrorCode::StaleEvidence, requirement.capability,
                                                  "soft preference skipped because its evidence is stale",
                                                  requirement.strength, lookup.state));
            }
            continue;
        }
        const MatchOutcome outcome = match_requirement(requirement, lookup, policy.allow_degraded_capabilities);
        if (outcome == MatchOutcome::Satisfied) {
            if (requirement.strength == RequirementStrength::Soft) {
                score += static_cast<std::int64_t>(std::llround(requirement.weight * 1000.0));
                decision.reasons.push_back(reason(ErrorCode::Ok, requirement.capability,
                                                  "soft preference satisfied: " + describe_expectation(requirement),
                                                  requirement.strength, lookup.state));
            }
            continue;
        }
        if (outcome == MatchOutcome::Violated) {
            const ErrorCode code = rejection_code_for(requirement.capability);
            const std::string detail = "observed " + std::string(to_string(lookup.state)) +
                                       (lookup.present && lookup.value != nullptr ? " (" + lookup.value->render() + ")" : std::string()) +
                                       "; required " + describe_expectation(requirement);
            decision.reasons.push_back(reason(code, requirement.capability, detail, requirement.strength, lookup.state));
            if (requirement.strength == RequirementStrength::Hard) {
                has_hard_failure = true;
            } else {
                score -= static_cast<std::int64_t>(std::llround(requirement.weight * 500.0));
            }
            continue;
        }
        // Indeterminate: evidence is missing. Hard requirements fail closed.
        if (requirement.strength == RequirementStrength::Hard) {
            has_indeterminate = true;
        }
        decision.reasons.push_back(reason(ErrorCode::UnknownCapability, requirement.capability,
                                          "required capability has no evidence: observed UNKNOWN; required " +
                                              describe_expectation(requirement),
                                          requirement.strength, lookup.state));
    }

    // ---- Policy constraints ---------------------------------------------
    const std::string vendor = enumeration_token(capabilities, cap::kVendorId);
    const std::string architecture = enumeration_token(capabilities, cap::kArchitectureFamily);

    if (std::find(policy.forbidden_accelerators.begin(), policy.forbidden_accelerators.end(),
                  context.target.accelerator) != policy.forbidden_accelerators.end()) {
        has_hard_failure = true;
        decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, "policy.forbidden_accelerators",
                                          "accelerator is explicitly forbidden by policy", RequirementStrength::Hard,
                                          CapabilityState::Unsupported));
    }
    if (!vendor.empty() && token_in(policy.forbidden_vendors, vendor)) {
        has_hard_failure = true;
        decision.reasons.push_back(reason(ErrorCode::VendorMismatch, cap::kVendorId,
                                          "vendor '" + vendor + "' is forbidden by policy", RequirementStrength::Hard,
                                          CapabilityState::Supported));
    }
    if (!vendor.empty() && !policy.allowed_vendors.empty() && !token_in(policy.allowed_vendors, vendor)) {
        has_hard_failure = true;
        decision.reasons.push_back(reason(ErrorCode::VendorMismatch, cap::kVendorId,
                                          "vendor '" + vendor + "' is not in the policy allow list",
                                          RequirementStrength::Hard, CapabilityState::Supported));
    }
    if (!architecture.empty() && token_in(policy.deprecated_architectures, architecture)) {
        has_hard_failure = true;
        decision.reasons.push_back(reason(ErrorCode::ArchitectureMismatch, cap::kArchitectureFamily,
                                          "architecture '" + architecture + "' is deprecated by policy",
                                          RequirementStrength::Hard, CapabilityState::Supported));
    }
    if (!policy.minimum_runtime_version.is_any()) {
        const CapabilityLookup runtime = capabilities.lookup(cap::kRuntimeVersion);
        if (runtime.state == CapabilityState::Unknown) {
            has_indeterminate = true;
            decision.reasons.push_back(reason(ErrorCode::UnknownCapability, cap::kRuntimeVersion,
                                              "policy sets a minimum runtime version but the runtime version is UNKNOWN",
                                              RequirementStrength::Hard, runtime.state));
        } else if (runtime.present && runtime.value != nullptr &&
                   !policy.minimum_runtime_version.matches(runtime.value->version_value())) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::RuntimeVersionUnsupported, cap::kRuntimeVersion,
                                              "runtime version " + runtime.value->render() +
                                                  " does not satisfy policy minimum " +
                                                  policy.minimum_runtime_version.to_string(),
                                              RequirementStrength::Hard, runtime.state));
        }
    }
    if (policy.minimum_memory_bytes > 0) {
        const CapabilityLookup memory = capabilities.lookup(cap::kMemoryTotalBytes);
        if (memory.state == CapabilityState::Unknown) {
            has_indeterminate = true;
            decision.reasons.push_back(reason(ErrorCode::UnknownCapability, cap::kMemoryTotalBytes,
                                              "policy sets a minimum memory but total memory is UNKNOWN",
                                              RequirementStrength::Hard, memory.state));
        } else if (memory.present && memory.value != nullptr &&
                   static_cast<std::uint64_t>(memory.value->integer_value()) < policy.minimum_memory_bytes) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::InsufficientMemory, cap::kMemoryTotalBytes,
                                              "total memory " + std::to_string(memory.value->integer_value()) +
                                                  " is below the policy minimum " +
                                                  std::to_string(policy.minimum_memory_bytes),
                                              RequirementStrength::Hard, memory.state));
        }
    }
    for (const std::string& required : policy.required_capabilities) {
        const CapabilityLookup lookup = capabilities.lookup(required);
        if (lookup.state == CapabilityState::Unknown) {
            has_indeterminate = true;
            decision.reasons.push_back(reason(ErrorCode::UnknownCapability, required,
                                              "policy requires this capability but there is no evidence",
                                              RequirementStrength::Hard, lookup.state));
        } else if (lookup.state != CapabilityState::Supported) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::MissingMandatoryCapability, required,
                                              "policy requires this capability and the accelerator reports " +
                                                  std::string(to_string(lookup.state)),
                                              RequirementStrength::Hard, lookup.state));
        }
    }
    for (const std::string& denied : policy.denied_capabilities) {
        const CapabilityLookup lookup = capabilities.lookup(denied);
        if (lookup.state == CapabilityState::Supported || lookup.state == CapabilityState::Degraded) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, denied,
                                              "policy denies this capability and the accelerator supports it",
                                              RequirementStrength::Hard, lookup.state));
        }
    }
    for (const std::string& tag : workload.required_policy_tags) {
        if (!token_in(policy.tags, tag)) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, "policy.tag." + tag,
                                              "workload requires policy tag '" + tag +
                                                  "' which the active policy does not provide",
                                              RequirementStrength::Hard, CapabilityState::Unsupported));
        }
    }
    switch (policy.synthetic_evidence_policy) {
        case SyntheticEvidencePolicy::Reject:
            if (context.target.provenance != EvidenceProvenance::Real) {
                has_hard_failure = true;
                decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, "policy.synthetic_evidence",
                                                  "policy rejects " + std::string(to_string(context.target.provenance)) +
                                                      " evidence",
                                                  RequirementStrength::Hard, CapabilityState::Unsupported));
            }
            break;
        case SyntheticEvidencePolicy::Require:
            if (context.target.provenance != EvidenceProvenance::Synthetic) {
                has_hard_failure = true;
                decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, "policy.synthetic_evidence",
                                                  "policy accepts only SYNTHETIC evidence but this member reports " +
                                                      std::string(to_string(context.target.provenance)),
                                                  RequirementStrength::Hard, CapabilityState::Unsupported));
            }
            break;
        case SyntheticEvidencePolicy::Allow:
            break;
    }
    if (!context.target.accepts_new_work) {
        has_hard_failure = true;
        decision.reasons.push_back(reason(ErrorCode::PolicyMismatch, "membership.state",
                                          "member is not in a state that accepts new work",
                                          RequirementStrength::Hard, CapabilityState::Unsupported));
    }

    // ---- Portability ------------------------------------------------------
    if (context.source != nullptr) {
        PortabilityRequest portability_request;
        portability_request.source_capabilities = context.source->capabilities;
        portability_request.destination_capabilities = context.target.capabilities;
        portability_request.source_vendor = context.source->vendor;
        portability_request.destination_vendor = vendor;
        portability_request.workload = &workload;
        portability_request.policy_allows_cross_vendor_migration = policy.allow_cross_vendor_migration;
        const PortabilityResult portability = classify_portability(portability_request);
        decision.portability = portability.value;
        for (const CompatibilityReason& entry : portability.reasons) {
            if (entry.code != ErrorCode::Ok) {
                decision.reasons.push_back(entry);
            }
        }
        if (portability.value == PortabilityClass::Unknown) {
            has_indeterminate = true;
            decision.reasons.push_back(reason(ErrorCode::UnknownCapability, "portability",
                                              "portability from the source accelerator cannot be established",
                                              RequirementStrength::Hard, CapabilityState::Unknown));
        } else if (portability.value == PortabilityClass::Unsupported) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PortabilityConstraintViolated, "portability",
                                              "no portability path exists from the source accelerator",
                                              RequirementStrength::Hard, CapabilityState::Unsupported));
        } else if (!meets_portability_threshold(portability.value, workload.minimum_portability)) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PortabilityConstraintViolated, "portability",
                                              "portability " + std::string(to_string(portability.value)) +
                                                  " does not meet the workload minimum " +
                                                  std::string(to_string(workload.minimum_portability)),
                                              RequirementStrength::Hard, CapabilityState::Supported));
        }
        if (workload.execution_mode == ExecutionMode::ExactBinary &&
            portability.value != PortabilityClass::Native &&
            portability.value != PortabilityClass::BinaryCompatible &&
            portability.value != PortabilityClass::LiveMigrationSupported) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PortabilityConstraintViolated, "execution_mode",
                                              "workload requires the exact binary and the destination is " +
                                                  std::string(to_string(portability.value)),
                                              RequirementStrength::Hard, CapabilityState::Supported));
        }
        if (!policy.allowed_portability_classes.empty() &&
            portability.value != PortabilityClass::Unknown &&
            std::find(policy.allowed_portability_classes.begin(), policy.allowed_portability_classes.end(),
                      portability.value) == policy.allowed_portability_classes.end()) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::PortabilityConstraintViolated, "policy.allowed_portability_classes",
                                              "portability " + std::string(to_string(portability.value)) +
                                                  " is not permitted by policy",
                                              RequirementStrength::Hard, CapabilityState::Supported));
        }
        if (workload.migration_need == MigrationNeed::LiveStateTransfer &&
            portability.value != PortabilityClass::LiveMigrationSupported) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::MigrationUnsupported, "workload.migration_need",
                                              "workload requires live state transfer but the destination is " +
                                                  std::string(to_string(portability.value)),
                                              RequirementStrength::Hard, CapabilityState::Supported));
        }
        if (workload.migration_need == MigrationNeed::CheckpointRestore &&
            !meets_portability_threshold(portability.value, PortabilityClass::CheckpointRestoreSupported) &&
            !(workload.reconstruction_allowed &&
              meets_portability_threshold(portability.value, PortabilityClass::RecompileRequired))) {
            has_hard_failure = true;
            decision.reasons.push_back(reason(ErrorCode::MigrationUnsupported, "workload.migration_need",
                                              "workload requires checkpoint/restore but the destination is " +
                                                  std::string(to_string(portability.value)),
                                              RequirementStrength::Hard, CapabilityState::Supported));
        }
    } else {
        // No origin supplied: the workload is being evaluated for the device it
        // is native to. Portability is trivially NATIVE when eligible.
        decision.portability = PortabilityClass::Native;
    }

    score -= static_cast<std::int64_t>(portability_transformation_steps(decision.portability)) * 100;
    if (context.target.provenance == EvidenceProvenance::Synthetic) {
        score -= 250;
    } else if (context.target.provenance == EvidenceProvenance::Unsupported) {
        score -= 500;
    }
    const CapabilityLookup free_memory = capabilities.lookup(cap::kMemoryFreeBytes);
    if (free_memory.present && free_memory.value != nullptr && free_memory.state == CapabilityState::Supported &&
        context.target.evidence_fresh) {
        const std::int64_t gib = free_memory.value->integer_value() / (1024LL * 1024LL * 1024LL);
        score += std::min<std::int64_t>(std::max<std::int64_t>(gib, 0), 1000);
    }
    decision.score = score;

    if (has_hard_failure) {
        decision.outcome = CompatibilityOutcome::Ineligible;
    } else if (has_indeterminate) {
        decision.outcome = CompatibilityOutcome::Unknown;
    } else {
        decision.outcome = CompatibilityOutcome::Eligible;
    }

    sort_reasons(decision.reasons);
    refresh_decision_identity(decision);
    return decision;
}

std::string FleetEvaluation::render() const {
    std::string out;
    out += "workload=";
    out += workload.to_string();
    out += " revision=";
    out += std::to_string(workload_revision.value());
    out += " eligible=";
    out += std::to_string(eligible_count);
    out += " ineligible=";
    out += std::to_string(ineligible_count);
    out += " unknown=";
    out += std::to_string(unknown_count);
    for (const MatrixCell& cell : cells) {
        out += "\n";
        out += cell.accelerator.to_string();
        out += " ";
        out += to_string(cell.decision.outcome);
        out += " portability=";
        out += to_string(cell.decision.portability);
        out += " evidence=";
        out += to_string(cell.decision.provenance);
        out += " fingerprint=";
        out += cell.decision.fingerprint_hex();
    }
    return out;
}

Result<FleetEvaluation> evaluate_fleet(const WorkloadProfile& workload, const EvaluationContext& context,
                                       std::vector<EvaluationTarget> targets) {
    if (targets.size() > Limits::kMaxFleetEvaluationTargets) {
        return Status(ErrorCode::BoundsExceeded, "fleet evaluation exceeds the permitted number of targets");
    }
    std::sort(targets.begin(), targets.end(), [](const EvaluationTarget& a, const EvaluationTarget& b) {
        if (a.accelerator != b.accelerator) {
            return a.accelerator < b.accelerator;
        }
        if (a.device_generation != b.device_generation) {
            return a.device_generation < b.device_generation;
        }
        return a.capability_generation < b.capability_generation;
    });
    for (std::size_t i = 1; i < targets.size(); ++i) {
        if (targets[i].accelerator == targets[i - 1].accelerator) {
            return Status(ErrorCode::DuplicateIdentity, "fleet evaluation received a duplicate accelerator target");
        }
    }
    FleetEvaluation evaluation;
    evaluation.workload = workload.class_id;
    evaluation.workload_revision = context.workload_revision;
    evaluation.cells.reserve(targets.size());
    for (const EvaluationTarget& target : targets) {
        EvaluationContext per_target = context;
        per_target.target = target;
        const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, per_target);
        if (!decision.ok()) {
            return decision.status();
        }
        switch (decision->outcome) {
            case CompatibilityOutcome::Eligible: ++evaluation.eligible_count; break;
            case CompatibilityOutcome::Ineligible: ++evaluation.ineligible_count; break;
            case CompatibilityOutcome::Unknown: ++evaluation.unknown_count; break;
        }
        MatrixCell cell;
        cell.accelerator = target.accelerator;
        cell.decision = *decision;
        evaluation.cells.push_back(std::move(cell));
    }
    return evaluation;
}

}  // namespace haf
