#include "haf/model/accelerator.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/hash.hpp"
#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

constexpr MemberState kLegalTransitions[][2] = {
    {MemberState::Discovered, MemberState::Observed},
    {MemberState::Discovered, MemberState::Fenced},
    {MemberState::Discovered, MemberState::Retired},
    {MemberState::Observed, MemberState::Admitted},
    {MemberState::Observed, MemberState::Fenced},
    {MemberState::Observed, MemberState::Retired},
    {MemberState::Admitted, MemberState::Active},
    {MemberState::Admitted, MemberState::Degraded},
    {MemberState::Admitted, MemberState::Draining},
    {MemberState::Admitted, MemberState::Fenced},
    {MemberState::Admitted, MemberState::Retired},
    {MemberState::Active, MemberState::Degraded},
    {MemberState::Active, MemberState::Draining},
    {MemberState::Active, MemberState::Fenced},
    {MemberState::Active, MemberState::Retired},
    {MemberState::Degraded, MemberState::Active},
    {MemberState::Degraded, MemberState::Draining},
    {MemberState::Degraded, MemberState::Fenced},
    {MemberState::Degraded, MemberState::Retired},
    {MemberState::Draining, MemberState::Active},
    {MemberState::Draining, MemberState::Fenced},
    {MemberState::Draining, MemberState::Retired},
    {MemberState::Fenced, MemberState::Observed},
    {MemberState::Fenced, MemberState::Retired},
};

}  // namespace

std::string_view to_string(SupportLevel level) noexcept {
    switch (level) {
        case SupportLevel::Native: return "NATIVE";
        case SupportLevel::Translated: return "TRANSLATED";
        case SupportLevel::Synthetic: return "SYNTHETIC";
        case SupportLevel::Unsupported: return "UNSUPPORTED";
        case SupportLevel::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool support_level_from_token(std::string_view token, SupportLevel& out) noexcept {
    if (token == "native") { out = SupportLevel::Native; return true; }
    if (token == "translated") { out = SupportLevel::Translated; return true; }
    if (token == "synthetic") { out = SupportLevel::Synthetic; return true; }
    if (token == "unsupported") { out = SupportLevel::Unsupported; return true; }
    if (token == "unknown") { out = SupportLevel::Unknown; return true; }
    return false;
}

bool support_level_from_wire(std::uint8_t raw, SupportLevel& out) noexcept {
    switch (raw) {
        case 0: out = SupportLevel::Native; return true;
        case 1: out = SupportLevel::Translated; return true;
        case 2: out = SupportLevel::Synthetic; return true;
        case 3: out = SupportLevel::Unsupported; return true;
        case 4: out = SupportLevel::Unknown; return true;
        default: return false;
    }
}

std::string_view to_string(MemberState state) noexcept {
    switch (state) {
        case MemberState::Discovered: return "DISCOVERED";
        case MemberState::Observed: return "OBSERVED";
        case MemberState::Admitted: return "ADMITTED";
        case MemberState::Active: return "ACTIVE";
        case MemberState::Degraded: return "DEGRADED";
        case MemberState::Draining: return "DRAINING";
        case MemberState::Fenced: return "FENCED";
        case MemberState::Retired: return "RETIRED";
    }
    return "UNKNOWN";
}

bool member_state_from_wire(std::uint8_t raw, MemberState& out) noexcept {
    switch (raw) {
        case 0: out = MemberState::Discovered; return true;
        case 1: out = MemberState::Observed; return true;
        case 2: out = MemberState::Admitted; return true;
        case 3: out = MemberState::Active; return true;
        case 4: out = MemberState::Degraded; return true;
        case 5: out = MemberState::Draining; return true;
        case 6: out = MemberState::Fenced; return true;
        case 7: out = MemberState::Retired; return true;
        default: return false;
    }
}

bool is_legal_transition(MemberState from, MemberState to) noexcept {
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

bool is_terminal_state(MemberState state) noexcept { return state == MemberState::Retired; }

Status AcceleratorDescriptor::validate() const {
    if (id.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "accelerator descriptor has no accelerator identity");
    }
    if (physical_device.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "accelerator descriptor has no physical device identity");
    }
    if (agent.is_nil() || agent_boot.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "accelerator descriptor has no owning agent boot identity");
    }
    if (node.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "accelerator descriptor has no node identity");
    }
    if (vendor_token.empty() || product_token.empty() || architecture_token.empty() ||
        device_generation_token.empty() || runtime_family.empty()) {
        return Status(ErrorCode::InvalidArgument, "accelerator descriptor is missing a canonical taxonomy token");
    }
    if (vendor_token.size() > Limits::kMaxTokenBytes || product_token.size() > Limits::kMaxTokenBytes ||
        architecture_token.size() > Limits::kMaxTokenBytes ||
        device_generation_token.size() > Limits::kMaxTokenBytes || runtime_family.size() > Limits::kMaxTokenBytes) {
        return Status(ErrorCode::BoundsExceeded, "accelerator descriptor taxonomy token exceeds the permitted length");
    }
    if (vendor != vendor_id_from_token(vendor_token) || architecture != architecture_id_from_token(architecture_token)) {
        return Status(ErrorCode::IntegrityFailure,
                      "accelerator descriptor taxonomy identity does not match its canonical token");
    }
    if (topology_references.size() > Limits::kMaxTopologyReferences) {
        return Status(ErrorCode::BoundsExceeded, "accelerator descriptor has too many topology references");
    }
    for (std::size_t i = 1; i < topology_references.size(); ++i) {
        if (topology_references[i] < topology_references[i - 1]) {
            return Status(ErrorCode::MalformedData, "topology references are not in canonical order");
        }
    }
    const Status capability_status = capabilities.validate();
    if (!capability_status.ok()) {
        return capability_status;
    }
    if (capabilities.records().empty()) {
        return Status(ErrorCode::MalformedData, "accelerator descriptor carries no capability advertisement");
    }
    // The capability set must describe the same vendor/runtime the descriptor
    // claims structurally. Mismatch means the advertisement is inconsistent.
    const CapabilityLookup vendor_lookup = capabilities.lookup(cap::kVendorId);
    if (vendor_lookup.present && vendor_lookup.state == CapabilityState::Supported && vendor_lookup.value != nullptr) {
        if (vendor_lookup.value->token() != vendor_token) {
            return Status(ErrorCode::CapabilityMismatch,
                          "capability vendor.id disagrees with the descriptor vendor token");
        }
    }
    if (evidence.size() > Limits::kMaxPersistedEvidence) {
        return Status(ErrorCode::BoundsExceeded, "accelerator descriptor carries too many evidence records");
    }
    for (const EvidenceRecord& record : evidence) {
        const Status evidence_status = record.validate();
        if (!evidence_status.ok()) {
            return evidence_status;
        }
        if (record.subject != id) {
            return Status(ErrorCode::IntegrityFailure, "evidence record is bound to a different accelerator");
        }
        if (record.provenance != provenance) {
            return Status(ErrorCode::IntegrityFailure,
                          "evidence record provenance disagrees with the descriptor provenance");
        }
    }
    if (provenance == EvidenceProvenance::Synthetic && support_level == SupportLevel::Native) {
        return Status(ErrorCode::IntegrityFailure,
                      "SYNTHETIC evidence cannot be reported with a NATIVE support level");
    }
    if (provenance == EvidenceProvenance::Unsupported && support_level == SupportLevel::Native) {
        return Status(ErrorCode::IntegrityFailure,
                      "UNSUPPORTED evidence cannot be reported with a NATIVE support level");
    }
    if (support_level == SupportLevel::Native && evidence.empty()) {
        return Status(ErrorCode::IntegrityFailure, "a NATIVE support level requires at least one evidence record");
    }
    return Status::success();
}

Sha256::digest_type AcceleratorDescriptor::structural_digest() const {
    ByteWriter writer;
    writer.id128(id.raw());
    writer.id128(physical_device.raw());
    writer.generation(generation);
    writer.id128(agent.raw());
    writer.id128(agent_boot.raw());
    writer.id128(node.raw());
    writer.string(vendor_token);
    writer.string(product_token);
    writer.string(architecture_token);
    writer.string(device_generation_token);
    writer.string(runtime_family);
    writer.u32(runtime_version.major());
    writer.u32(runtime_version.minor());
    writer.u32(runtime_version.patch());
    writer.u32(driver_version.major());
    writer.u32(driver_version.minor());
    writer.u32(driver_version.patch());
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

void AcceleratorDescriptor::serialize(ByteWriter& writer) const {
    writer.id128(id.raw());
    writer.id128(physical_device.raw());
    writer.generation(generation);
    writer.id128(agent.raw());
    writer.id128(agent_boot.raw());
    writer.id128(node.raw());
    writer.string(vendor_token);
    writer.string(product_token);
    writer.string(architecture_token);
    writer.string(device_generation_token);
    writer.string(runtime_family);
    writer.u32(runtime_version.major());
    writer.u32(runtime_version.minor());
    writer.u32(runtime_version.patch());
    writer.u32(driver_version.major());
    writer.u32(driver_version.minor());
    writer.u32(driver_version.patch());
    capabilities.serialize(writer);
    writer.u8(static_cast<std::uint8_t>(support_level));
    writer.u8(static_cast<std::uint8_t>(provenance));
    writer.u32(static_cast<std::uint32_t>(evidence.size()));
    for (const EvidenceRecord& record : evidence) {
        record.serialize(writer);
    }
    writer.u32(static_cast<std::uint32_t>(topology_references.size()));
    for (const std::string& reference : topology_references) {
        writer.string(reference);
    }
    writer.i64(to_unix_nanos(observed_at));
}

bool AcceleratorDescriptor::deserialize(ByteReader& reader, AcceleratorDescriptor& out, bool persisted_scale) {
    const std::uint32_t evidence_limit =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxPersistedEvidence)
                        : static_cast<std::uint32_t>(Limits::kMaxAdvertisementsPerMessage);
    AcceleratorDescriptor descriptor;
    std::uint8_t raw_support = 0;
    std::uint8_t raw_provenance = 0;
    Id128 raw_id;
    Id128 raw_physical;
    Id128 raw_agent;
    Id128 raw_boot;
    Id128 raw_node;
    std::int64_t observed_at = 0;
    RuntimeVersion runtime_version;
    RuntimeVersion driver_version;
    if (!reader.id128(raw_id) || !reader.id128(raw_physical) || !reader.generation(descriptor.generation) ||
        !reader.id128(raw_agent) || !reader.id128(raw_boot) || !reader.id128(raw_node) ||
        !reader.string(descriptor.vendor_token, Limits::kMaxTokenBytes) ||
        !reader.string(descriptor.product_token, Limits::kMaxTokenBytes) ||
        !reader.string(descriptor.architecture_token, Limits::kMaxTokenBytes) ||
        !reader.string(descriptor.device_generation_token, Limits::kMaxTokenBytes) ||
        !reader.string(descriptor.runtime_family, Limits::kMaxTokenBytes) ||
        !reader.u32(runtime_version.major) || !reader.u32(runtime_version.minor) ||
        !reader.u32(runtime_version.patch) || !reader.u32(driver_version.major) ||
        !reader.u32(driver_version.minor) || !reader.u32(driver_version.patch)) {
        return false;
    }
    if (!CapabilitySet::deserialize(reader, descriptor.capabilities, persisted_scale)) {
        return false;
    }
    if (!reader.u8(raw_support) || !reader.u8(raw_provenance)) {
        return false;
    }
    if (!support_level_from_wire(raw_support, descriptor.support_level)) {
        reader.fail(ErrorCode::MalformedData, "support level is outside the declared domain");
        return false;
    }
    if (!evidence_provenance_from_wire(raw_provenance, descriptor.provenance)) {
        reader.fail(ErrorCode::MalformedData, "evidence provenance is outside the declared domain");
        return false;
    }
    std::uint32_t evidence_count = 0;
    if (!reader.count(evidence_count, evidence_limit, 8)) {
        return false;
    }
    descriptor.evidence.reserve(evidence_count);
    for (std::uint32_t i = 0; i < evidence_count; ++i) {
        EvidenceRecord record;
        if (!EvidenceRecord::deserialize(reader, record)) {
            return false;
        }
        descriptor.evidence.push_back(std::move(record));
    }
    std::uint32_t topology_count = 0;
    if (!reader.count(topology_count, static_cast<std::uint32_t>(Limits::kMaxTopologyReferences), 4)) {
        return false;
    }
    descriptor.topology_references.reserve(topology_count);
    std::string previous;
    for (std::uint32_t i = 0; i < topology_count; ++i) {
        std::string reference;
        if (!reader.string(reference, Limits::kMaxNameBytes)) {
            return false;
        }
        if (i != 0 && !(previous < reference)) {
            reader.fail(ErrorCode::MalformedData, "topology references are not in canonical order");
            return false;
        }
        previous = reference;
        descriptor.topology_references.push_back(std::move(reference));
    }
    if (!reader.i64(observed_at)) {
        return false;
    }
    descriptor.id = AcceleratorId::from_raw(raw_id);
    descriptor.physical_device = PhysicalDeviceId::from_raw(raw_physical);
    descriptor.agent = AgentId::from_raw(raw_agent);
    descriptor.agent_boot = AgentBootId::from_raw(raw_boot);
    descriptor.node = NodeId::from_raw(raw_node);
    descriptor.runtime_version = SemanticVersion(runtime_version.major, runtime_version.minor, runtime_version.patch);
    descriptor.driver_version = SemanticVersion(driver_version.major, driver_version.minor, driver_version.patch);
    descriptor.observed_at = from_unix_nanos(observed_at);
    descriptor.vendor = vendor_id_from_token(descriptor.vendor_token);
    descriptor.architecture = architecture_id_from_token(descriptor.architecture_token);
    const Status valid = descriptor.validate();
    if (!valid.ok()) {
        reader.fail(valid.code(), valid.message());
        return false;
    }
    out = std::move(descriptor);
    return true;
}

std::int64_t AcceleratorDescriptor::memory_total_bytes() const noexcept {
    const CapabilityLookup lookup = capabilities.lookup(cap::kMemoryTotalBytes);
    if (lookup.present && lookup.value != nullptr && lookup.state == CapabilityState::Supported) {
        return lookup.value->integer_value();
    }
    return 0;
}

std::vector<std::string> AcceleratorDescriptor::isa_code_object_targets() const {
    const CapabilityLookup lookup = capabilities.lookup(cap::kIsaCodeObjectTargets);
    if (lookup.present && lookup.value != nullptr && lookup.state == CapabilityState::Supported) {
        return lookup.value->tokens();
    }
    return {};
}

std::vector<std::string> AcceleratorDescriptor::numeric_formats() const {
    const CapabilityLookup lookup = capabilities.lookup(cap::kNumericFormats);
    if (lookup.present && lookup.value != nullptr && lookup.state == CapabilityState::Supported) {
        return lookup.value->tokens();
    }
    return {};
}

AcceleratorId derive_accelerator_id(const PhysicalDeviceId& physical, const AgentId& agent, const AgentBootId& boot,
                                    DeviceGeneration generation) noexcept {
    ByteWriter writer;
    writer.id128(physical.raw());
    writer.id128(agent.raw());
    writer.id128(boot.raw());
    writer.u64(generation.value());
    const ByteBuffer& bytes = writer.data();
    const Sha256::digest_type digest = Sha256::hash(bytes.data(), bytes.size());
    return AcceleratorId::from_raw(derive_identity("haf.accelerator", to_hex(digest)));
}

}  // namespace haf
