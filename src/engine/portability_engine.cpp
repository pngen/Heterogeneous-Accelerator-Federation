#include "haf/engine/portability_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "haf/model/capability_key.hpp"

namespace haf {
namespace {

[[nodiscard]] bool supports(const CapabilitySet* set, std::string_view key) {
    if (set == nullptr) {
        return false;
    }
    const CapabilityLookup lookup = set->lookup(key);
    return lookup.present && lookup.state == CapabilityState::Supported;
}

[[nodiscard]] bool known(const CapabilitySet* set, std::string_view key) {
    if (set == nullptr) {
        return false;
    }
    return set->lookup(key).state != CapabilityState::Unknown;
}

[[nodiscard]] bool token_present(const CapabilitySet* set, std::string_view key, std::string_view token) {
    if (set == nullptr) {
        return false;
    }
    const CapabilityLookup lookup = set->lookup(key);
    if (!lookup.present || lookup.value == nullptr) {
        return false;
    }
    const std::vector<std::string>& tokens = lookup.value->tokens();
    return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

[[nodiscard]] bool shares_token(const CapabilitySet* a, const CapabilitySet* b, std::string_view key) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    const CapabilityLookup left = a->lookup(key);
    const CapabilityLookup right = b->lookup(key);
    if (!left.present || !right.present || left.value == nullptr || right.value == nullptr) {
        return false;
    }
    for (const std::string& token : left.value->tokens()) {
        const std::vector<std::string>& other = right.value->tokens();
        if (std::find(other.begin(), other.end(), token) != other.end()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] CompatibilityReason make_reason(ErrorCode code, std::string_view subject, std::string detail) {
    CompatibilityReason reason;
    reason.code = code;
    reason.subject = std::string(subject);
    reason.detail = std::move(detail);
    reason.strength = RequirementStrength::Hard;
    reason.observed = CapabilityState::Unknown;
    return reason;
}

}  // namespace

bool is_dynamic_capability(std::string_view capability_name) noexcept {
    if (capability_name.rfind("health.", 0) == 0) {
        return true;
    }
    if (capability_name == cap::kMemoryFreeBytes) {
        return true;
    }
    if (capability_name == cap::kQueueConcurrentStreams) {
        return true;
    }
    return false;
}

void sort_reasons(std::vector<CompatibilityReason>& reasons) {
    std::stable_sort(reasons.begin(), reasons.end(), [](const CompatibilityReason& a, const CompatibilityReason& b) {
        if (a.strength != b.strength) {
            return static_cast<std::uint8_t>(a.strength) < static_cast<std::uint8_t>(b.strength);
        }
        if (a.subject != b.subject) {
            return a.subject < b.subject;
        }
        if (a.code != b.code) {
            return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
        }
        return a.detail < b.detail;
    });
}

PortabilityResult classify_portability(const PortabilityRequest& request) {
    PortabilityResult result;
    result.value = PortabilityClass::Unknown;

    const CapabilitySet* source = request.source_capabilities;
    const CapabilitySet* destination = request.destination_capabilities;
    if (source == nullptr || destination == nullptr) {
        result.reasons.push_back(
            make_reason(ErrorCode::InvalidArgument, std::string(), "portability classification requires both a source and a destination capability set"));
        return result;
    }

    const bool same_vendor = !request.source_vendor.empty() && request.source_vendor == request.destination_vendor;

    // ---- Evidence gate ----------------------------------------------------
    // Every input that could justify a strong class must be positively known.
    struct RequiredEvidence {
        std::string_view key;
        const char* description;
    };
    const RequiredEvidence required[] = {
        {cap::kVendorId, "vendor identity"},
        {cap::kArchitectureFamily, "architecture family"},
        {cap::kIsaCodeObjectTargets, "ISA code object targets"},
        {cap::kRuntimeFamily, "runtime family"},
    };
    std::vector<std::string> unknown_inputs;
    for (const RequiredEvidence& item : required) {
        if (!known(source, item.key) || !known(destination, item.key)) {
            unknown_inputs.emplace_back(item.description);
        }
    }
    if (!unknown_inputs.empty()) {
        std::string detail = "portability cannot be established because these inputs are UNKNOWN on at least one side:";
        for (const std::string& item : unknown_inputs) {
            detail += " ";
            detail += item;
            detail += ";";
        }
        result.reasons.push_back(make_reason(ErrorCode::UnknownCapability, std::string(), std::move(detail)));
        result.value = PortabilityClass::Unknown;
        sort_reasons(result.reasons);
        return result;
    }

    const bool shared_code_object = shares_token(source, destination, cap::kIsaCodeObjectTargets);
    const bool destination_known_targets = known(destination, cap::kIsaCodeObjectTargets);
    const bool live_transfer_source = supports(source, cap::kMigrationLiveStateTransfer);
    const bool live_transfer_destination = supports(destination, cap::kMigrationLiveStateTransfer);
    const bool checkpoint_source = supports(source, cap::kMigrationCheckpointRestore);
    const bool checkpoint_destination = supports(destination, cap::kMigrationCheckpointRestore);
    const bool reconstruction_destination = supports(destination, cap::kMigrationStateReconstruction);
    const bool recompile_destination = supports(destination, cap::kPortabilityRecompileAvailable);
    const bool repackage_destination = supports(destination, cap::kPortabilityRepackageAvailable);
    const bool same_architecture = known(source, cap::kArchitectureFamily) && known(destination, cap::kArchitectureFamily)
                                       ? source->lookup(cap::kArchitectureFamily).value != nullptr &&
                                             destination->lookup(cap::kArchitectureFamily).value != nullptr &&
                                             source->lookup(cap::kArchitectureFamily).value->render() ==
                                                 destination->lookup(cap::kArchitectureFamily).value->render()
                                       : false;

    // ---- Strongest justified class ---------------------------------------
    const bool cross_vendor_live_allowed =
        same_vendor || (request.policy_allows_cross_vendor_migration &&
                        supports(source, cap::kMigrationCrossVendorState) &&
                        supports(destination, cap::kMigrationCrossVendorState));

    if (shared_code_object && live_transfer_source && live_transfer_destination && same_vendor) {
        result.value = PortabilityClass::LiveMigrationSupported;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kMigrationLiveStateTransfer,
                                             "both accelerators support live state transfer and share a code object target"));
    } else if (shared_code_object && live_transfer_source && live_transfer_destination && cross_vendor_live_allowed) {
        result.value = PortabilityClass::LiveMigrationSupported;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kMigrationLiveStateTransfer,
                                             "cross-vendor live state transfer is explicitly supported by both sides and permitted by policy"));
    } else if (shared_code_object && same_vendor && same_architecture) {
        result.value = PortabilityClass::Native;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kIsaCodeObjectTargets,
                                             "both accelerators present the same code object target on the same architecture and vendor"));
    } else if (shared_code_object) {
        result.value = PortabilityClass::BinaryCompatible;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kIsaCodeObjectTargets,
                                             "the destination presents a code object target that the source already produces"));
    } else if (checkpoint_source && checkpoint_destination && same_vendor) {
        result.value = PortabilityClass::CheckpointRestoreSupported;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kMigrationCheckpointRestore,
                                             "no shared code object target, but both sides support checkpoint and restore"));
    } else if (destination_known_targets && recompile_destination) {
        result.value = PortabilityClass::RecompileRequired;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kPortabilityRecompileAvailable,
                                             "the destination accepts recompiled code objects but shares no code object target with the source"));
    } else if (repackage_destination) {
        result.value = PortabilityClass::RepackageRequired;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kPortabilityRepackageAvailable,
                                             "the destination requires repackaging rather than recompilation"));
    } else if (reconstruction_destination) {
        result.value = PortabilityClass::StateReconstructionRequired;
        result.reasons.push_back(make_reason(ErrorCode::Ok, cap::kMigrationStateReconstruction,
                                             "execution state cannot move; the destination can only host a reconstructed workload"));
    } else {
        result.value = PortabilityClass::Unsupported;
        result.reasons.push_back(make_reason(ErrorCode::MigrationUnsupported, std::string(),
                                             "no shared code object target, no checkpoint path, no recompilation path, and no reconstruction path"));
    }

    // ---- Restrictions that downgrade an optimistic classification --------
    if (request.workload != nullptr && result.value == PortabilityClass::LiveMigrationSupported &&
        request.workload->migration_need != MigrationNeed::LiveStateTransfer) {
        // A workload that does not need live transfer still gets the stronger
        // class: the class describes what is possible, not what is required.
        result.reasons.push_back(make_reason(ErrorCode::Ok, "workload.migration_need",
                                             "live state transfer is available but the workload does not require it"));
    }

    if (!same_vendor && result.value == PortabilityClass::LiveMigrationSupported && !cross_vendor_live_allowed) {
        result.value = PortabilityClass::Unsupported;
        result.reasons.push_back(make_reason(ErrorCode::MigrationUnsupported, cap::kMigrationCrossVendorState,
                                             "cross-vendor live state transfer is not supported by both accelerators and permitted by policy"));
    }

    sort_reasons(result.reasons);
    return result;
}

}  // namespace haf
