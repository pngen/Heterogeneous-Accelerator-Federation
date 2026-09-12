// Model contracts: capability semantics, evidence provenance, descriptors,
// workload requirements, policy, membership lifecycle, and portability.

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/capability.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/evidence.hpp"
#include "haf/model/membership.hpp"
#include "haf/model/migration.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/portability.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/model/workload.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

HAF_TEST(model, capability_registry_is_closed_and_resolves_core_keys) {
    HAF_CHECK(capability_key_from_name("memory.total_bytes").ok());
    HAF_CHECK(capability_key_from_name("numeric.fp8_e4m3").ok());
    const Result<CapabilityKey> core = capability_key_from_name("memory.total_bytes");
    HAF_EQ(static_cast<int>(core->kind()), static_cast<int>(CapabilityKind::Integer));
    // Unknown non-extension keys are rejected rather than interned.
    const Result<CapabilityKey> unknown = capability_key_from_name("totally.made.up");
    HAF_CHECK(!unknown.ok());
    HAF_EQ(static_cast<int>(unknown.status().code()), static_cast<int>(ErrorCode::UnknownCapability));
    // Extension namespaces are accepted on demand and are never core keys.
    const Result<CapabilityKey> extension = capability_key_from_name("x.vendor.special_feature");
    HAF_CHECK(extension.ok());
    HAF_CHECK(extension->is_extension());
    HAF_CHECK(!find_core_capability_key("x.vendor.special_feature").has_value());
    // Deterministic enumeration order.
    const std::vector<CapabilityKey>& keys = all_core_capability_keys();
    HAF_CHECK(!keys.empty());
    for (std::size_t i = 1; i < keys.size(); ++i) {
        HAF_CHECK(keys[i - 1].name() < keys[i].name());
    }
}

HAF_TEST(model, capability_normalization_is_canonical) {
    CapabilityValue left = CapabilityValue::token_set({"FP16", "bf16", "fp16", "  int8 "});
    HAF_CHECK(left.canonicalize().ok());
    HAF_EQ(left.render(), std::string("bf16,fp16,int8"));

    CapabilityValue right = CapabilityValue::token_set({"int8", "fp16", "bf16"});
    HAF_CHECK(right.canonicalize().ok());
    HAF_CHECK(left.equals(right));

    CapabilityValue negative_zero = CapabilityValue::scalar(-0.0);
    HAF_CHECK(negative_zero.canonicalize().ok());
    CapabilityValue positive_zero = CapabilityValue::scalar(0.0);
    HAF_CHECK(positive_zero.canonicalize().ok());
    HAF_CHECK(negative_zero.equals(positive_zero));

    CapabilityValue infinite = CapabilityValue::scalar(std::numeric_limits<double>::infinity());
    const Status infinite_status = infinite.canonicalize();
    HAF_CHECK(!infinite_status.ok());
    HAF_EQ(static_cast<int>(infinite_status.code()), static_cast<int>(ErrorCode::NonFiniteQuantity));
}

HAF_TEST(model, capability_set_digest_is_order_independent) {
    const Result<CapabilityRecord> first = make_presence("numeric.fp64", CapabilityState::Supported);
    const Result<CapabilityRecord> second = make_presence("numeric.int8", CapabilityState::Supported);
    HAF_REQUIRE_OK(first);
    HAF_REQUIRE_OK(second);

    CapabilitySet left;
    HAF_REQUIRE_OK(left.set_records({*first, *second}));
    CapabilitySet right;
    HAF_REQUIRE_OK(right.set_records({*second, *first}));
    HAF_CHECK(left.digest_hex() == right.digest_hex());
    HAF_CHECK(left.id() == right.id());
}

HAF_TEST(model, unknown_capability_fails_closed_by_default) {
    CapabilitySet set;
    HAF_REQUIRE_OK(set.set_records({make_presence("numeric.fp32", CapabilityState::Supported)}));
    const CapabilityLookup missing = set.lookup("numeric.fp8_e4m3");
    HAF_EQ(static_cast<int>(missing.state), static_cast<int>(CapabilityState::Unknown));
    HAF_CHECK(!missing.namespace_closed);
}

HAF_TEST(model, closed_namespace_turns_absence_into_positive_evidence) {
    CapabilitySet set;
    HAF_REQUIRE_OK(set.set_records({make_presence("numeric.fp32", CapabilityState::Supported)}));
    HAF_REQUIRE_OK(set.set_closed_namespaces({"numeric"}));
    const CapabilityLookup missing = set.lookup("numeric.fp8_e4m3");
    HAF_EQ(static_cast<int>(missing.state), static_cast<int>(CapabilityState::Unsupported));
    HAF_CHECK(missing.namespace_closed);
    // Other namespaces remain open, so absence there is still UNKNOWN.
    HAF_EQ(static_cast<int>(set.lookup("tensor.matrix_engine").state),
           static_cast<int>(CapabilityState::Unknown));
}

HAF_TEST(model, capability_set_rejects_duplicates_and_kind_mismatch) {
    CapabilitySet set;
    const Result<CapabilityRecord> record = make_presence("numeric.fp64", CapabilityState::Supported);
    HAF_REQUIRE_OK(record);
    HAF_CHECK(!set.set_records({*record, *record}).ok());

    CapabilityRecord mismatched;
    const Result<CapabilityKey> key = capability_key_from_name("memory.total_bytes");
    HAF_REQUIRE_OK(key);
    mismatched.key = *key;
    mismatched.state = CapabilityState::Supported;
    mismatched.value = CapabilityValue::presence();
    const Status mismatch_status = mismatched.validate();
    HAF_CHECK(!mismatch_status.ok());
    HAF_EQ(static_cast<int>(mismatch_status.code()), static_cast<int>(ErrorCode::CapabilityMismatch));
}

HAF_TEST(model, evidence_provenance_is_never_upgraded_implicitly) {
    const AcceleratorDescriptor cuda = make_cuda_like(1);
    HAF_EQ(static_cast<int>(cuda.provenance), static_cast<int>(EvidenceProvenance::Synthetic));
    HAF_EQ(static_cast<int>(cuda.support_level), static_cast<int>(SupportLevel::Synthetic));
    HAF_CHECK(!cuda.evidence.empty());
    for (const EvidenceRecord& record : cuda.evidence) {
        HAF_EQ(static_cast<int>(record.provenance), static_cast<int>(cuda.provenance));
    }
}

HAF_TEST(model, evidence_freshness_requires_a_live_observation) {
    const AcceleratorDescriptor device = make_cuda_like(2);
    HAF_REQUIRE(!device.evidence.empty());
    EvidenceRecord record = device.evidence.front();
    record.requires_revalidation = false;
    record.observed_monotonic_marker = now_monotonic();
    HAF_CHECK(record.is_fresh(now_monotonic(), 1'000'000'000ULL));
    // A reloaded record has no live monotonic reference and can never be fresh.
    record.requires_revalidation = true;
    HAF_CHECK(!record.is_fresh(now_monotonic(), 1'000'000'000ULL));
}

HAF_TEST(model, evidence_round_trip_preserves_provenance) {
    const AcceleratorDescriptor device = make_rocm_like(3);
    EvidenceRecord original = device.evidence.front();
    original.capability_generation = device.capabilities.generation();
    ByteWriter writer;
    original.serialize(writer);
    ByteReader reader(writer.data());
    EvidenceRecord decoded;
    HAF_CHECK(EvidenceRecord::deserialize(reader, decoded));
    HAF_CHECK(decoded.provenance == original.provenance);
    HAF_CHECK(decoded.adapter == original.adapter);
    HAF_CHECK(decoded.payload_digest == original.payload_digest);
    HAF_CHECK(decoded.digest() == original.digest());
}

HAF_TEST(model, descriptor_identity_binds_the_incarnation) {
    const AcceleratorDescriptor device = make_cuda_like(4);
    HAF_CHECK(device.id == derive_accelerator_id(device.physical_device, device.agent, device.agent_boot,
                                                 device.generation));
    // A different boot produces a different incarnation identity for the same
    // physical device: authority is never inherited across a restart.
    const AgentBootId other_boot = make_context(99).agent_boot;
    const AcceleratorId other = derive_accelerator_id(device.physical_device, device.agent, other_boot,
                                                      device.generation);
    HAF_CHECK(other != device.id);
}

HAF_TEST(model, descriptor_validation_rejects_inconsistency) {
    AcceleratorDescriptor device = make_cuda_like(5);
    HAF_CHECK(device.validate().ok());

    AcceleratorDescriptor wrong_identity = device;
    wrong_identity.id = AcceleratorId::from_raw(derive_identity("haf.accelerator", "other"));
    HAF_CHECK(!wrong_identity.validate().ok());

    AcceleratorDescriptor native_synthetic = device;
    native_synthetic.support_level = SupportLevel::Native;
    const Status synthetic_status = native_synthetic.validate();
    HAF_CHECK(!synthetic_status.ok());
    HAF_EQ(static_cast<int>(synthetic_status.code()), static_cast<int>(ErrorCode::IntegrityFailure));

    AcceleratorDescriptor no_capabilities = device;
    HAF_REQUIRE_OK(no_capabilities.capabilities.set_records({}));
    HAF_CHECK(!no_capabilities.validate().ok());
}

HAF_TEST(model, descriptor_round_trip_is_byte_stable) {
    const AcceleratorDescriptor device = make_intel_like(6);
    ByteWriter first;
    device.serialize(first);
    AcceleratorDescriptor decoded;
    ByteReader reader(first.data());
    HAF_CHECK(AcceleratorDescriptor::deserialize(reader, decoded, false));
    ByteWriter second;
    decoded.serialize(second);
    HAF_CHECK(first.data() == second.data());
    HAF_CHECK(decoded.id == device.id);
    HAF_CHECK(decoded.capabilities.digest() == device.capabilities.digest());
}

HAF_TEST(model, membership_lifecycle_table_is_explicit) {
    HAF_CHECK(is_legal_transition(MemberState::Discovered, MemberState::Observed));
    HAF_CHECK(is_legal_transition(MemberState::Observed, MemberState::Admitted));
    HAF_CHECK(is_legal_transition(MemberState::Admitted, MemberState::Active));
    HAF_CHECK(is_legal_transition(MemberState::Active, MemberState::Fenced));
    HAF_CHECK(is_legal_transition(MemberState::Fenced, MemberState::Observed));
    HAF_CHECK(!is_legal_transition(MemberState::Discovered, MemberState::Active));
    HAF_CHECK(!is_legal_transition(MemberState::Retired, MemberState::Active));
    HAF_CHECK(!is_legal_transition(MemberState::Active, MemberState::Active));
    HAF_CHECK(is_terminal_state(MemberState::Retired));
    HAF_CHECK(!is_terminal_state(MemberState::Fenced));
}

HAF_TEST(model, member_record_liveness) {
    MemberRecord record;
    record.state = MemberState::Discovered;
    HAF_CHECK(!record.is_live());
    HAF_CHECK(!record.accepts_new_work());
    record.state = MemberState::Degraded;
    HAF_CHECK(record.is_live());
    HAF_CHECK(!record.accepts_new_work());
    record.state = MemberState::Active;
    HAF_CHECK(record.accepts_new_work());
    record.state = MemberState::Retired;
    HAF_CHECK(!record.is_live());
}

HAF_TEST(model, portability_taxonomy_ordering) {
    HAF_CHECK(portability_strength(PortabilityClass::Native) > portability_strength(PortabilityClass::BinaryCompatible));
    HAF_CHECK(portability_strength(PortabilityClass::BinaryCompatible) >
              portability_strength(PortabilityClass::RecompileRequired));
    HAF_CHECK(portability_strength(PortabilityClass::LiveMigrationSupported) >
              portability_strength(PortabilityClass::Native));
    // UNKNOWN never satisfies a concrete threshold.
    HAF_CHECK(!meets_portability_threshold(PortabilityClass::Unknown, PortabilityClass::Unsupported));
    HAF_CHECK(!meets_portability_threshold(PortabilityClass::Native, PortabilityClass::Unknown));
    HAF_CHECK(meets_portability_threshold(PortabilityClass::Native, PortabilityClass::Native));
    HAF_CHECK(!meets_portability_threshold(PortabilityClass::RecompileRequired, PortabilityClass::Native));
    // Semantics of the individual predicates.
    HAF_CHECK(implies_live_state_transfer(PortabilityClass::LiveMigrationSupported));
    HAF_CHECK(!implies_live_state_transfer(PortabilityClass::CheckpointRestoreSupported));
    HAF_CHECK(requires_binary_transformation(PortabilityClass::RecompileRequired));
    HAF_CHECK(!requires_binary_transformation(PortabilityClass::CheckpointRestoreSupported));
    HAF_CHECK(requires_state_reconstruction(PortabilityClass::StateReconstructionRequired));
    HAF_CHECK(!requires_state_reconstruction(PortabilityClass::CheckpointRestoreSupported));
}

HAF_TEST(model, workload_validation_rejects_duplicate_and_inconsistent) {
    WorkloadProfile profile = make_workload("dup", {hard_present("numeric.fp32"), hard_present("numeric.fp32")});
    const Status duplicate_status = profile.validate();
    HAF_CHECK(!duplicate_status.ok());
    HAF_EQ(static_cast<int>(duplicate_status.code()), static_cast<int>(ErrorCode::DuplicateIdentity));

    WorkloadProfile hard_with_weight = make_workload("weighted", {hard_present("numeric.fp32")});
    hard_with_weight.requirements[0].weight = 0.5;
    HAF_CHECK(!hard_with_weight.validate().ok());

    WorkloadProfile live_without_need = make_workload("live", {hard_present("numeric.fp32")});
    live_without_need.minimum_portability = PortabilityClass::LiveMigrationSupported;
    live_without_need.migration_need = MigrationNeed::RestartOnly;
    HAF_CHECK(!live_without_need.validate().ok());
}

HAF_TEST(model, workload_digest_ignores_declaration_order) {
    WorkloadProfile left = make_workload("order", {hard_present("numeric.fp32"), hard_present("memory.ecc_enabled")});
    WorkloadProfile right = make_workload("order", {hard_present("memory.ecc_enabled"), hard_present("numeric.fp32")});
    HAF_CHECK(left.digest() == right.digest());
    HAF_CHECK(derive_workload_revision(left) == derive_workload_revision(right));

    WorkloadProfile changed = make_workload("order", {hard_present("numeric.fp32")});
    HAF_CHECK(changed.digest() != left.digest());
    HAF_CHECK(derive_workload_revision(changed) != derive_workload_revision(left));
}

HAF_TEST(model, workload_requirement_round_trip) {
    WorkloadProfile profile = make_workload(
        "roundtrip", {hard_present("numeric.fp8_e4m3"), hard_version("runtime.version", ">=12.0.0 <13.0.0"),
                      hard_at_least("memory.total_bytes", 8LL * 1024 * 1024 * 1024),
                      hard_tokens("isa.code_object_targets", {"sm-120"}), soft_present("memory.ecc_enabled", 0.25)});
    HAF_CHECK(profile.validate().ok());
    ByteWriter writer;
    profile.serialize(writer);
    WorkloadProfile decoded;
    ByteReader reader(writer.data());
    HAF_CHECK(WorkloadProfile::deserialize(reader, decoded, false));
    HAF_CHECK(decoded.digest() == profile.digest());
    HAF_CHECK(decoded.requirements.size() == profile.requirements.size());
    HAF_CHECK(decoded.revision == profile.revision);
}

HAF_TEST(model, workload_requirement_identity_is_content_derived) {
    const CapabilityRequirement present = hard_present("numeric.fp32");
    const CapabilityRequirement present_again = hard_present("numeric.fp32");
    const CapabilityRequirement absent = hard_absent("numeric.fp32");
    HAF_CHECK(present.id == present_again.id);
    HAF_CHECK(present.id != absent.id);
}

HAF_TEST(model, policy_identity_is_content_addressed) {
    FederationPolicy first = permissive_policy();
    FederationPolicy second = permissive_policy();
    HAF_CHECK(first.id == second.id);
    HAF_CHECK(first.generation == second.generation);

    second.minimum_memory_bytes = 1024;
    second.refresh_identity();
    HAF_CHECK(first.id != second.id);
    HAF_CHECK(first.generation != second.generation);
}

HAF_TEST(model, policy_validation_rejects_contradiction) {
    FederationPolicy policy = permissive_policy();
    policy.required_capabilities = {"numeric.fp64"};
    policy.denied_capabilities = {"numeric.fp64"};
    std::sort(policy.required_capabilities.begin(), policy.required_capabilities.end());
    std::sort(policy.denied_capabilities.begin(), policy.denied_capabilities.end());
    policy.refresh_identity();
    const Status policy_status = policy.validate();
    HAF_CHECK(!policy_status.ok());
    HAF_EQ(static_cast<int>(policy_status.code()),
           static_cast<int>(ErrorCode::ContradictoryCapabilities));

    FederationPolicy vendor_conflict = permissive_policy();
    vendor_conflict.allowed_vendors = {"nvidia"};
    vendor_conflict.forbidden_vendors = {"nvidia"};
    HAF_CHECK(!vendor_conflict.validate().ok());
}

HAF_TEST(model, policy_round_trip_preserves_identity) {
    FederationPolicy policy = permissive_policy();
    policy.name = "roundtrip";
    policy.allowed_vendors = {"nvidia", "amd"};
    policy.deprecated_architectures = {"kepler"};
    policy.required_capabilities = {"numeric.fp32"};
    policy.denied_capabilities = {"migration.live_state_transfer"};
    policy.tags = {"isolation.strong"};
    policy.allowed_portability_classes = {PortabilityClass::Native, PortabilityClass::BinaryCompatible};
    policy.required_evidence_freshness_nanos = 5'000'000'000ULL;
    policy.synthetic_evidence_policy = SyntheticEvidencePolicy::Allow;
    policy.minimum_runtime_version = VersionRange::parse(">=6.0.0").value();
    policy.refresh_identity();
    HAF_CHECK(policy.validate().ok());

    ByteWriter writer;
    policy.serialize(writer);
    FederationPolicy decoded;
    ByteReader reader(writer.data());
    HAF_CHECK(FederationPolicy::deserialize(reader, decoded, true));
    HAF_CHECK(decoded.id == policy.id);
    HAF_CHECK(decoded.generation == policy.generation);
    HAF_CHECK(decoded.minimum_runtime_version.to_string() == policy.minimum_runtime_version.to_string());
}

HAF_TEST(model, migration_lifecycle_table_is_explicit) {
    HAF_CHECK(is_legal_migration_transition(MigrationState::Planned, MigrationState::Validated));
    HAF_CHECK(is_legal_migration_transition(MigrationState::Validated, MigrationState::Prepared));
    HAF_CHECK(is_legal_migration_transition(MigrationState::Prepared, MigrationState::Transferring));
    HAF_CHECK(is_legal_migration_transition(MigrationState::Transferring, MigrationState::Verified));
    HAF_CHECK(is_legal_migration_transition(MigrationState::Verified, MigrationState::Committed));
    HAF_CHECK(is_legal_migration_transition(MigrationState::Planned, MigrationState::Aborted));
    HAF_CHECK(!is_legal_migration_transition(MigrationState::Planned, MigrationState::Committed));
    HAF_CHECK(!is_legal_migration_transition(MigrationState::Committed, MigrationState::Aborted));
    HAF_CHECK(is_terminal_migration_state(MigrationState::Committed));
    HAF_CHECK(is_terminal_migration_state(MigrationState::Refused));
    HAF_CHECK(!is_terminal_migration_state(MigrationState::Verified));
}

HAF_TEST(model, taxonomy_slug_is_canonical) {
    const Result<std::string> slug = slug_token("NVIDIA GeForce RTX 5090", 128);
    HAF_REQUIRE_OK(slug);
    HAF_EQ(*slug, std::string("nvidia-geforce-rtx-5090"));
    HAF_EQ(*slug_token("  multi   space  ", 128), std::string("multi-space"));
    HAF_EQ(*slug_token("a/b\\c", 128), std::string("a-b-c"));
    HAF_CHECK(!slug_token("///", 128).ok());
}
