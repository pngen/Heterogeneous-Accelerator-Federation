#include "haf/model/evidence.hpp"

#include <utility>

#include "haf/core/limits.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {

std::string_view to_string(EvidenceProvenance provenance) noexcept {
    switch (provenance) {
        case EvidenceProvenance::Real: return "REAL";
        case EvidenceProvenance::Synthetic: return "SYNTHETIC";
        case EvidenceProvenance::Unsupported: return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
}

bool evidence_provenance_from_wire(std::uint8_t raw, EvidenceProvenance& out) noexcept {
    switch (raw) {
        case 0: out = EvidenceProvenance::Real; return true;
        case 1: out = EvidenceProvenance::Synthetic; return true;
        case 2: out = EvidenceProvenance::Unsupported; return true;
        default: return false;
    }
}

std::string_view to_string(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::StaticDescriptor: return "static_descriptor";
        case EvidenceKind::DynamicObservation: return "dynamic_observation";
        case EvidenceKind::AdapterProbe: return "adapter_probe";
        case EvidenceKind::Declaration: return "declaration";
        case EvidenceKind::RecoveryRestore: return "recovery_restore";
    }
    return "unknown";
}

bool evidence_kind_from_wire(std::uint8_t raw, EvidenceKind& out) noexcept {
    switch (raw) {
        case 0: out = EvidenceKind::StaticDescriptor; return true;
        case 1: out = EvidenceKind::DynamicObservation; return true;
        case 2: out = EvidenceKind::AdapterProbe; return true;
        case 3: out = EvidenceKind::Declaration; return true;
        case 4: out = EvidenceKind::RecoveryRestore; return true;
        default: return false;
    }
}

void EvidenceRecord::serialize(ByteWriter& writer) const {
    writer.id128(id.raw());
    writer.generation(generation);
    writer.u8(static_cast<std::uint8_t>(provenance));
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.string(adapter);
    writer.string(source);
    writer.string(adapter_version);
    writer.id128(subject.raw());
    writer.id128(physical_device.raw());
    writer.generation(capability_generation);
    writer.raw(payload_digest.data(), payload_digest.size());
    writer.u32(runtime_version.major);
    writer.u32(runtime_version.minor);
    writer.u32(runtime_version.patch);
    writer.u32(driver_version.major);
    writer.u32(driver_version.minor);
    writer.u32(driver_version.patch);
    writer.i64(to_unix_nanos(observed_at));
    writer.u64(sequence);
    writer.u64(freshness_budget_nanos);
    writer.boolean(dynamic);
    writer.boolean(requires_revalidation);
}

bool EvidenceRecord::deserialize(ByteReader& reader, EvidenceRecord& out) {
    EvidenceRecord record;
    std::uint8_t raw_provenance = 0;
    std::uint8_t raw_kind = 0;
    Id128 raw_id;
    Id128 raw_subject;
    Id128 raw_physical;
    std::uint8_t raw_digest[Sha256::kDigestBytes] = {};
    std::int64_t observed_at = 0;
    if (!reader.id128(raw_id) || !reader.generation(record.generation) || !reader.u8(raw_provenance) ||
        !reader.u8(raw_kind) || !reader.string(record.adapter, Limits::kMaxNameBytes) ||
        !reader.string(record.source, Limits::kMaxDescriptionBytes) ||
        !reader.string(record.adapter_version, Limits::kMaxTokenBytes) || !reader.id128(raw_subject) ||
        !reader.id128(raw_physical) || !reader.generation(record.capability_generation) ||
        !reader.raw(raw_digest, sizeof(raw_digest)) || !reader.u32(record.runtime_version.major) ||
        !reader.u32(record.runtime_version.minor) || !reader.u32(record.runtime_version.patch) ||
        !reader.u32(record.driver_version.major) || !reader.u32(record.driver_version.minor) ||
        !reader.u32(record.driver_version.patch) || !reader.i64(observed_at) || !reader.u64(record.sequence) ||
        !reader.u64(record.freshness_budget_nanos) || !reader.boolean(record.dynamic) ||
        !reader.boolean(record.requires_revalidation)) {
        return false;
    }
    if (!evidence_provenance_from_wire(raw_provenance, record.provenance)) {
        reader.fail(ErrorCode::MalformedData, "evidence provenance is outside the declared domain");
        return false;
    }
    if (!evidence_kind_from_wire(raw_kind, record.kind)) {
        reader.fail(ErrorCode::MalformedData, "evidence kind is outside the declared domain");
        return false;
    }
    record.id = EvidenceId::from_raw(raw_id);
    record.subject = AcceleratorId::from_raw(raw_subject);
    record.physical_device = PhysicalDeviceId::from_raw(raw_physical);
    for (std::size_t i = 0; i < Sha256::kDigestBytes; ++i) {
        record.payload_digest[i] = raw_digest[i];
    }
    record.observed_at = from_unix_nanos(observed_at);
    const Status valid = record.validate();
    if (!valid.ok()) {
        reader.fail(valid.code(), valid.message());
        return false;
    }
    out = std::move(record);
    return true;
}

Status EvidenceRecord::validate() const {
    if (adapter.empty()) {
        return Status(ErrorCode::MalformedData, "evidence record has no adapter identifier");
    }
    if (adapter.size() > Limits::kMaxNameBytes || source.size() > Limits::kMaxDescriptionBytes ||
        adapter_version.size() > Limits::kMaxTokenBytes) {
        return Status(ErrorCode::BoundsExceeded, "evidence record text exceeds the permitted length");
    }
    if (provenance == EvidenceProvenance::Unsupported && kind != EvidenceKind::RecoveryRestore) {
        // UNSUPPORTED provenance marks a path that produced no observation at
        // all. Such a record may exist, but it must never claim a payload.
        bool all_zero = true;
        for (const std::uint8_t byte : payload_digest) {
            if (byte != 0) {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            return Status(ErrorCode::MalformedData,
                          "UNSUPPORTED evidence provenance must not carry an observed payload digest");
        }
    }
    return Status::success();
}

Sha256::digest_type EvidenceRecord::digest() const {
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(provenance));
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.string(adapter);
    writer.string(source);
    writer.string(adapter_version);
    writer.id128(subject.raw());
    writer.id128(physical_device.raw());
    writer.generation(capability_generation);
    writer.raw(payload_digest.data(), payload_digest.size());
    writer.u32(runtime_version.major);
    writer.u32(runtime_version.minor);
    writer.u32(runtime_version.patch);
    writer.u32(driver_version.major);
    writer.u32(driver_version.minor);
    writer.u32(driver_version.patch);
    writer.u64(sequence);
    writer.boolean(dynamic);
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

bool EvidenceRecord::is_fresh(MonotonicTime now, std::uint64_t effective_budget_nanos) const noexcept {
    if (requires_revalidation) {
        return false;
    }
    if (effective_budget_nanos == 0) {
        return true;
    }
    const std::uint64_t age = age_nanos(observed_monotonic_marker, now);
    return age <= effective_budget_nanos;
}

EvidenceRecord make_evidence(std::string adapter, std::string source, std::string adapter_version,
                             AcceleratorId subject, PhysicalDeviceId physical_device,
                             EvidenceProvenance provenance, EvidenceKind kind, Sha256::digest_type payload_digest,
                             RuntimeVersion runtime_version, RuntimeVersion driver_version,
                             std::uint64_t freshness_budget_nanos, bool dynamic) {
    EvidenceRecord record;
    record.provenance = provenance;
    record.kind = kind;
    record.adapter = std::move(adapter);
    record.source = std::move(source);
    record.adapter_version = std::move(adapter_version);
    record.subject = subject;
    record.physical_device = physical_device;
    record.payload_digest = payload_digest;
    record.runtime_version = runtime_version;
    record.driver_version = driver_version;
    record.freshness_budget_nanos = freshness_budget_nanos;
    record.dynamic = dynamic;
    record.observed_at = now_timestamp();
    record.observed_monotonic_marker = now_monotonic();
    record.requires_revalidation = false;
    record.id = EvidenceId::from_raw(derive_identity("haf.evidence", to_hex(record.digest())));
    return record;
}

}  // namespace haf
