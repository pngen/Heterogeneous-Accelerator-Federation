#include "haf/federation/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"

namespace haf {
namespace {

void write_count(ByteWriter& writer, std::size_t count) {
    writer.u32(static_cast<std::uint32_t>(count));
}

template <class T, class Less>
void sort_unique(std::vector<T>& values, Less less) {
    std::sort(values.begin(), values.end(), less);
}

}  // namespace

void FederationSnapshot::canonicalize() {
    sort_unique(members, [](const MemberRecord& a, const MemberRecord& b) { return a.accelerator < b.accelerator; });
    sort_unique(descriptors,
                [](const AcceleratorDescriptor& a, const AcceleratorDescriptor& b) { return a.id < b.id; });
    sort_unique(workloads,
                [](const WorkloadProfile& a, const WorkloadProfile& b) { return a.class_id < b.class_id; });
    sort_unique(decisions, [](const CompatibilityDecision& a, const CompatibilityDecision& b) {
        return a.decision_id < b.decision_id;
    });
    sort_unique(plans, [](const MigrationPlan& a, const MigrationPlan& b) { return a.id < b.id; });
}

const MemberRecord* FederationSnapshot::find_member(const AcceleratorId& id) const {
    const auto found = std::lower_bound(members.begin(), members.end(), id,
                                        [](const MemberRecord& record, const AcceleratorId& key) {
                                            return record.accelerator < key;
                                        });
    if (found != members.end() && found->accelerator == id) {
        return &(*found);
    }
    return nullptr;
}

const AcceleratorDescriptor* FederationSnapshot::find_descriptor(const AcceleratorId& id) const {
    const auto found = std::lower_bound(descriptors.begin(), descriptors.end(), id,
                                        [](const AcceleratorDescriptor& record, const AcceleratorId& key) {
                                            return record.id < key;
                                        });
    if (found != descriptors.end() && found->id == id) {
        return &(*found);
    }
    return nullptr;
}

const WorkloadProfile* FederationSnapshot::find_workload(const WorkloadClassId& id) const {
    const auto found = std::lower_bound(workloads.begin(), workloads.end(), id,
                                        [](const WorkloadProfile& record, const WorkloadClassId& key) {
                                            return record.class_id < key;
                                        });
    if (found != workloads.end() && found->class_id == id) {
        return &(*found);
    }
    return nullptr;
}

Sha256::digest_type FederationSnapshot::digest() const {
    // The digest binds every generation that carries authority, so two states
    // that differ only in authority are never considered identical.
    ByteWriter writer;
    writer.string("haf.snapshot.v1");
    writer.id128(federation.raw());
    writer.generation(generation);
    writer.generation(epoch);
    writer.string(name);
    writer.id128(policy.raw());
    writer.generation(policy_generation);
    write_count(writer, members.size());
    for (const MemberRecord& record : members) {
        record.serialize(writer);
    }
    write_count(writer, descriptors.size());
    for (const AcceleratorDescriptor& descriptor : descriptors) {
        const Sha256::digest_type structural = descriptor.structural_digest();
        writer.raw(structural.data(), structural.size());
        descriptor.capabilities.serialize(writer);
    }
    write_count(writer, workloads.size());
    for (const WorkloadProfile& workload : workloads) {
        const Sha256::digest_type content = workload.digest();
        writer.raw(content.data(), content.size());
    }
    write_count(writer, decisions.size());
    for (const CompatibilityDecision& decision : decisions) {
        const Sha256::digest_type content = decision.fingerprint();
        writer.raw(content.data(), content.size());
    }
    write_count(writer, plans.size());
    for (const MigrationPlan& plan : plans) {
        const Sha256::digest_type content = plan.fingerprint();
        writer.raw(content.data(), content.size());
    }
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

std::string FederationSnapshot::digest_hex() const { return to_hex(digest()); }

ByteBuffer encode_snapshot(const FederationSnapshot& snapshot) {
    ByteWriter writer;
    writer.u32(kSnapshotFormatVersion);
    writer.u32(0);
    writer.string("haf-federation");
    writer.id128(snapshot.federation.raw());
    writer.generation(snapshot.generation);
    writer.generation(snapshot.epoch);
    writer.string(snapshot.name);
    writer.id128(snapshot.policy.raw());
    writer.generation(snapshot.policy_generation);
    snapshot.policy_data.serialize(writer);

    write_count(writer, snapshot.members.size());
    for (const MemberRecord& record : snapshot.members) {
        record.serialize(writer);
    }
    write_count(writer, snapshot.descriptors.size());
    for (const AcceleratorDescriptor& descriptor : snapshot.descriptors) {
        descriptor.serialize(writer);
    }
    write_count(writer, snapshot.workloads.size());
    for (const WorkloadProfile& workload : snapshot.workloads) {
        workload.serialize(writer);
    }
    write_count(writer, snapshot.decisions.size());
    for (const CompatibilityDecision& decision : snapshot.decisions) {
        decision.serialize(writer);
    }
    write_count(writer, snapshot.plans.size());
    for (const MigrationPlan& plan : snapshot.plans) {
        plan.serialize(writer);
    }
    writer.i64(to_unix_nanos(snapshot.created_at));
    const Sha256::digest_type digest = snapshot.digest();
    writer.raw(digest.data(), digest.size());
    return writer.take();
}

Result<FederationSnapshot> decode_snapshot(const ByteBuffer& payload) {
    if (payload.size() > Limits::kMaxStorePayloadBytes) {
        return Status(ErrorCode::BoundsExceeded, "snapshot payload exceeds the supported maximum");
    }
    ByteReader reader(payload);
    std::uint32_t format_version = 0;
    std::uint32_t reserved = 0;
    std::string domain;
    if (!reader.u32(format_version) || !reader.u32(reserved) || !reader.string(domain, Limits::kMaxNameBytes)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::CorruptStore, "snapshot header is truncated");
    }
    if (format_version != kSnapshotFormatVersion) {
        return Status(ErrorCode::UnsupportedStoreVersion, "snapshot format version is not supported");
    }
    if (reserved != 0) {
        return Status(ErrorCode::CorruptStore, "snapshot header reserved word is not zero");
    }
    if (domain != "haf-federation") {
        return Status(ErrorCode::CorruptStore, "snapshot payload is not a federation snapshot");
    }

    FederationSnapshot snapshot;
    Id128 raw_federation;
    Id128 raw_policy;
    std::int64_t created_at = 0;
    if (!reader.id128(raw_federation) || !reader.generation(snapshot.generation) ||
        !reader.generation(snapshot.epoch) || !reader.string(snapshot.name, Limits::kMaxNameBytes) ||
        !reader.id128(raw_policy) || !reader.generation(snapshot.policy_generation)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::CorruptStore, "snapshot identity section is truncated");
    }
    if (!FederationPolicy::deserialize(reader, snapshot.policy_data, true)) {
        return reader.failed() ? reader.error()
                               : Status(ErrorCode::CorruptStore, "snapshot policy section is malformed");
    }

    std::uint32_t member_count = 0;
    if (!reader.count(member_count, static_cast<std::uint32_t>(Limits::kMaxPersistedMembers), 8)) {
        return reader.error();
    }
    snapshot.members.reserve(member_count);
    for (std::uint32_t i = 0; i < member_count; ++i) {
        MemberRecord record;
        if (!MemberRecord::deserialize(reader, record, true)) {
            return reader.error();
        }
        snapshot.members.push_back(std::move(record));
    }
    std::uint32_t descriptor_count = 0;
    if (!reader.count(descriptor_count, static_cast<std::uint32_t>(Limits::kMaxPersistedMembers), 8)) {
        return reader.error();
    }
    snapshot.descriptors.reserve(descriptor_count);
    for (std::uint32_t i = 0; i < descriptor_count; ++i) {
        AcceleratorDescriptor descriptor;
        if (!AcceleratorDescriptor::deserialize(reader, descriptor, true)) {
            return reader.error();
        }
        snapshot.descriptors.push_back(std::move(descriptor));
    }
    std::uint32_t workload_count = 0;
    if (!reader.count(workload_count, static_cast<std::uint32_t>(Limits::kMaxPersistedWorkloads), 8)) {
        return reader.error();
    }
    snapshot.workloads.reserve(workload_count);
    for (std::uint32_t i = 0; i < workload_count; ++i) {
        WorkloadProfile profile;
        if (!WorkloadProfile::deserialize(reader, profile, true)) {
            return reader.error();
        }
        snapshot.workloads.push_back(std::move(profile));
    }
    std::uint32_t decision_count = 0;
    if (!reader.count(decision_count, static_cast<std::uint32_t>(Limits::kMaxPersistedDecisions), 8)) {
        return reader.error();
    }
    snapshot.decisions.reserve(decision_count);
    for (std::uint32_t i = 0; i < decision_count; ++i) {
        CompatibilityDecision decision;
        if (!CompatibilityDecision::deserialize(reader, decision, true)) {
            return reader.error();
        }
        snapshot.decisions.push_back(std::move(decision));
    }
    std::uint32_t plan_count = 0;
    if (!reader.count(plan_count, static_cast<std::uint32_t>(Limits::kMaxPersistedPlans), 8)) {
        return reader.error();
    }
    snapshot.plans.reserve(plan_count);
    for (std::uint32_t i = 0; i < plan_count; ++i) {
        MigrationPlan plan;
        if (!MigrationPlan::deserialize(reader, plan, true)) {
            return reader.error();
        }
        snapshot.plans.push_back(std::move(plan));
    }
    if (!reader.i64(created_at)) {
        return reader.error();
    }
    std::uint8_t stored_digest[Sha256::kDigestBytes] = {};
    if (!reader.raw(stored_digest, sizeof(stored_digest))) {
        return reader.error();
    }
    if (reader.remaining() != 0) {
        return Status(ErrorCode::CorruptStore, "snapshot payload has trailing bytes");
    }
    snapshot.federation = FederationId::from_raw(raw_federation);
    snapshot.policy = PolicyId::from_raw(raw_policy);
    snapshot.created_at = from_unix_nanos(created_at);
    snapshot.canonicalize();

    const Sha256::digest_type computed = snapshot.digest();
    if (std::memcmp(computed.data(), stored_digest, computed.size()) != 0) {
        return Status(ErrorCode::IntegrityFailure, "snapshot digest does not match its content");
    }
    if (snapshot.policy != snapshot.policy_data.id) {
        return Status(ErrorCode::IntegrityFailure, "snapshot policy identity disagrees with the embedded policy");
    }
    if (snapshot.federation.is_nil()) {
        return Status(ErrorCode::CorruptStore, "snapshot has no federation identity");
    }
    return snapshot;
}

}  // namespace haf
