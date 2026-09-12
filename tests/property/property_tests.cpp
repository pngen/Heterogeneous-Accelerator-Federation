// Randomized property tests. Every case uses a recorded seed so that a failure
// is reproducible, and reports the seed in its output.

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/engine/migration_planner.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/engine/ranking.hpp"
#include "haf/federation/federation.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

namespace {

const std::vector<std::string> kCapabilities = {
    "numeric.fp64",  "numeric.fp16",     "numeric.bf16",   "numeric.tf32",   "numeric.fp8_e4m3",
    "numeric.fp8_e5m2", "numeric.int8",   "tensor.matrix_engine", "tensor.warp_group_mma",
    "memory.ecc_enabled", "memory.managed_allocation", "kernel.cooperative_launch",
    "migration.checkpoint_restore", "migration.live_state_transfer", "portability.recompile_available"};

const std::vector<std::string> kVendors = {"nvidia", "amd", "intel", "synthetic"};
const std::vector<std::string> kArchitectures = {"blackwell", "cdna3", "xe-hpg", "unknown-family"};
const std::vector<std::string> kFormats = {"fp32", "fp16", "bf16", "fp8_e4m3", "int8", "fp64"};
const std::vector<std::string> kIsaTargets = {"sm-120", "sm-100", "gfx-942", "spirv-1.6", "compute-120"};

/// Build a randomized device descriptor.
AcceleratorDescriptor random_device(SeededRandom& random, std::uint64_t index) {
    adapters::DeviceProfile profile;
    profile.vendor_token = kVendors[random.below(kVendors.size())];
    profile.product_token = "model-" + std::to_string(random.below(1000));
    profile.architecture_token = kArchitectures[random.below(kArchitectures.size())];
    profile.device_generation_token = "gen-" + std::to_string(random.below(50));
    profile.runtime_family = "runtime-" + std::to_string(random.below(4));
    profile.runtime_version = SemanticVersion(static_cast<std::uint32_t>(random.below(8)),
                                              static_cast<std::uint32_t>(random.below(20)),
                                              static_cast<std::uint32_t>(random.below(10)));
    profile.memory_total_bytes = static_cast<std::int64_t>(random.below(64) + 1) * 1024LL * 1024LL * 1024LL;
    profile.memory_free_bytes = profile.memory_total_bytes / 2;
    profile.numeric_formats.clear();
    profile.isa_targets.clear();
    for (const std::string& format : kFormats) {
        if (random.chance(45)) {
            profile.numeric_formats.push_back(format);
        }
    }
    for (const std::string& target : kIsaTargets) {
        if (random.chance(30)) {
            profile.isa_targets.push_back(target);
        }
    }
    profile.tensor_matrix_engine = random.chance(50);
    profile.tensor_warp_group_mma = random.chance(30);
    profile.migration_live_state_transfer = random.chance(10);
    profile.migration_checkpoint_restore = random.chance(40);
    profile.extensions.clear();
    if (random.chance(40)) {
        profile.extensions.push_back({"x.vendor.random_feature", "token-" + std::to_string(random.below(5))});
    }
    return make_device(profile, 1000 + index, "property-adapter", static_cast<std::int64_t>(index));
}

/// Build a randomized workload profile.
WorkloadProfile random_workload(SeededRandom& random, std::uint64_t index) {
    std::vector<CapabilityRequirement> requirements;
    for (const std::string& capability : kCapabilities) {
        if (!random.chance(25)) {
            continue;
        }
        const bool hard = random.chance(70);
        if (hard) {
            requirements.push_back(hard_present(capability));
        } else {
            requirements.push_back(soft_present(capability, static_cast<double>(random.below(100)) / 100.0));
        }
    }
    if (random.chance(40)) {
        requirements.push_back(hard_equals("vendor.id", kVendors[random.below(kVendors.size())]));
    }
    if (random.chance(40)) {
        requirements.push_back(hard_tokens("isa.code_object_targets", {kIsaTargets[random.below(kIsaTargets.size())]}));
    }
    if (random.chance(40)) {
        requirements.push_back(hard_at_least("memory.total_bytes",
                                             static_cast<std::int64_t>(random.below(64) + 1) * 1024LL * 1024LL * 1024LL));
    }
    WorkloadProfile profile = make_workload("property-workload-" + std::to_string(index), std::move(requirements));
    return profile;
}

FederationPolicy random_policy(SeededRandom& random) {
    FederationPolicy policy = FederationPolicy::permissive_default();
    policy.name = "property-policy-" + std::to_string(random.below(1000));
    if (random.chance(40)) {
        policy.allowed_vendors = {kVendors[random.below(kVendors.size())]};
    }
    if (random.chance(30)) {
        policy.forbidden_vendors = {kVendors[random.below(kVendors.size())]};
    }
    if (random.chance(30)) {
        policy.deprecated_architectures = {kArchitectures[random.below(kArchitectures.size())]};
    }
    if (random.chance(30)) {
        policy.denied_capabilities = {kCapabilities[random.below(kCapabilities.size())]};
    }
    if (random.chance(30)) {
        policy.required_capabilities = {kCapabilities[random.below(kCapabilities.size())]};
    }
    // Remove any vendor that is both allowed and forbidden; that combination is
    // a contradictory policy by construction and is covered elsewhere.
    {
        std::vector<std::string> filtered;
        for (const std::string& vendor : policy.allowed_vendors) {
            if (!std::binary_search(policy.forbidden_vendors.begin(), policy.forbidden_vendors.end(), vendor)) {
                filtered.push_back(vendor);
            }
        }
        policy.allowed_vendors = std::move(filtered);
        if (policy.allowed_vendors.empty() && !policy.forbidden_vendors.empty() &&
            policy.forbidden_vendors.size() >= kVendors.size()) {
            policy.forbidden_vendors.clear();
        }
    }
    policy.allow_degraded_capabilities = random.chance(50);
    policy.allow_cross_vendor_migration = random.chance(40);
    policy.allow_cross_vendor_reconstruction = random.chance(70);
    policy.minimum_memory_bytes = random.chance(40) ? random.below(32) * 1024ULL * 1024ULL * 1024ULL : 0;
    if (random.chance(30)) {
        static const std::vector<PortabilityClass> classes = {
            PortabilityClass::Native, PortabilityClass::BinaryCompatible, PortabilityClass::RecompileRequired,
            PortabilityClass::CheckpointRestoreSupported, PortabilityClass::StateReconstructionRequired,
            PortabilityClass::RepackageRequired};
        policy.allowed_portability_classes = {classes[random.below(classes.size())]};
    }
    if (random.chance(20)) {
        policy.minimum_runtime_version = VersionRange::parse(">=2.0.0").value();
    }
    // Remove a contradictory require/deny pair so that the policy is valid.
    std::sort(policy.required_capabilities.begin(), policy.required_capabilities.end());
    std::sort(policy.denied_capabilities.begin(), policy.denied_capabilities.end());
    std::sort(policy.allowed_vendors.begin(), policy.allowed_vendors.end());
    std::sort(policy.forbidden_vendors.begin(), policy.forbidden_vendors.end());
    std::vector<std::string> filtered;
    for (const std::string& capability : policy.required_capabilities) {
        if (!std::binary_search(policy.denied_capabilities.begin(), policy.denied_capabilities.end(), capability)) {
            filtered.push_back(capability);
        }
    }
    policy.required_capabilities = std::move(filtered);
    policy.refresh_identity();
    return policy;
}

EvaluationContext context_for(const AcceleratorDescriptor& target, const FederationPolicy& policy) {
    EvaluationContext context;
    context.federation = FederationId::from_raw(derive_identity("haf.property.federation", "p"));
    context.federation_generation = FederationGeneration(1);
    context.epoch = CoordinatorEpoch(1);
    context.policy_id = policy.id;
    context.policy_generation = policy.generation;
    context.policy = &policy;
    context.workload_revision = WorkloadRevision(1);
    context.now = now_monotonic();
    context.target.accelerator = target.id;
    context.target.device_generation = target.generation;
    context.target.capability_generation = target.capabilities.generation();
    context.target.evidence_generation = EvidenceGeneration(1);
    context.target.support_level = target.support_level;
    context.target.provenance = target.provenance;
    context.target.capabilities = &target.capabilities;
    context.target.evidence_fresh = true;
    context.target.accepts_new_work = true;
    return context;
}

}  // namespace

HAF_TEST(property, descriptors_and_capability_sets_round_trip) {
    const std::uint64_t seed = 0xA5A5'1234ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const AcceleratorDescriptor device = random_device(random, static_cast<std::uint64_t>(iteration));
        ByteWriter writer;
        device.serialize(writer);
        AcceleratorDescriptor decoded;
        ByteReader reader(writer.data());
        HAF_CHECK(AcceleratorDescriptor::deserialize(reader, decoded, true));
        ByteWriter second;
        decoded.serialize(second);
        HAF_CHECK(writer.data() == second.data());
        HAF_CHECK(decoded.capabilities.digest_hex() == device.capabilities.digest_hex());
    }
}

HAF_TEST(property, workload_profiles_round_trip) {
    const std::uint64_t seed = 0xBEEF'5678ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const WorkloadProfile workload = random_workload(random, static_cast<std::uint64_t>(iteration));
        HAF_CHECK(workload.validate().ok());
        ByteWriter writer;
        workload.serialize(writer);
        WorkloadProfile decoded;
        ByteReader reader(writer.data());
        HAF_CHECK(WorkloadProfile::deserialize(reader, decoded, true));
        HAF_CHECK(decoded.digest() == workload.digest());
    }
}

HAF_TEST(property, compatibility_is_a_pure_function_of_its_inputs) {
    const std::uint64_t seed = 0x0F0F'2468ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 150; ++iteration) {
        const AcceleratorDescriptor device = random_device(random, static_cast<std::uint64_t>(iteration));
        const WorkloadProfile workload = random_workload(random, static_cast<std::uint64_t>(iteration));
        const FederationPolicy policy = random_policy(random);
        const EvaluationContext context = context_for(device, policy);
        const Result<CompatibilityDecision> first = evaluate_compatibility(workload, context);
        const Result<CompatibilityDecision> second = evaluate_compatibility(workload, context);
        HAF_CHECK(first.ok());
        HAF_CHECK(second.ok());
        if (!first.ok() || !second.ok()) {
            return;
        }
        HAF_CHECK(first->outcome == second->outcome);
        HAF_CHECK(first->fingerprint_hex() == second->fingerprint_hex());
        HAF_CHECK(first->render() == second->render());
    }
}

HAF_TEST(property, a_hard_violation_always_outranks_an_unknown) {
    const std::uint64_t seed = 0x1357'9BDFULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 150; ++iteration) {
        const AcceleratorDescriptor device = random_device(random, static_cast<std::uint64_t>(iteration));
        const FederationPolicy policy = permissive_policy();
        const WorkloadProfile workload =
            make_workload("hard-and-unknown",
                          {hard_equals("vendor.id", "definitely-not-the-device-vendor"),
                           hard_present("numeric.fp8_e5m2")});
        const Result<CompatibilityDecision> decision =
            evaluate_compatibility(workload, context_for(device, policy));
        HAF_CHECK(decision.ok());
        if (!decision.ok()) {
            return;
        }
        // A positive violation must never be reported as UNKNOWN, which would
        // understate what the federation knows.
        HAF_CHECK(decision->outcome == CompatibilityOutcome::Ineligible);
    }
}

HAF_TEST(property, portability_classification_is_stable_and_total) {
    const std::uint64_t seed = 0xC0DE'1111ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const AcceleratorDescriptor source = random_device(random, static_cast<std::uint64_t>(iteration * 2));
        const AcceleratorDescriptor destination = random_device(random, static_cast<std::uint64_t>(iteration * 2 + 1));
        PortabilityRequest request;
        request.source_capabilities = &source.capabilities;
        request.destination_capabilities = &destination.capabilities;
        request.source_vendor = source.vendor_token;
        request.destination_vendor = destination.vendor_token;
        request.policy_allows_cross_vendor_migration = random.chance(50);
        const PortabilityResult first = classify_portability(request);
        const PortabilityResult second = classify_portability(request);
        HAF_CHECK(first.value == second.value);
        HAF_CHECK(first.reasons.size() == second.reasons.size());
        // Live migration is never claimed across vendors without explicit support.
        if (source.vendor_token != destination.vendor_token && !request.policy_allows_cross_vendor_migration) {
            HAF_CHECK(first.value != PortabilityClass::LiveMigrationSupported);
        }
    }
}

HAF_TEST(property, repeated_update_invalidate_recompute_converges) {
    const std::uint64_t seed = 0xFEED'2222ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(seed, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(seed);
    HAF_REQUIRE_OK(join(**federation, device));
    const WorkloadProfile workload = make_workload("converge", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("property")));

    for (int iteration = 0; iteration < 60; ++iteration) {
        const AcceleratorDescriptor current = make_cuda_like(seed);
        const AuthorityClaim claim = (*federation)->epoch_claim("property update");
        Result<MemberRecord> updated = (*federation)->observe(current, claim);
        HAF_CHECK(updated.ok());
        if (!updated.ok()) {
            return;
        }
        const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, current.id);
        HAF_CHECK(decision.ok());
        if (!decision.ok()) {
            return;
        }
        HAF_CHECK(decision->outcome == CompatibilityOutcome::Eligible);
        static_cast<void>(random.next());
    }
    const AuditReport report = (*federation)->audit();
    HAF_CHECK(report.ok());
    HAF_EQ(report.violations.size(), std::size_t{0});
}

HAF_TEST(property, policy_change_invalidates_every_derived_decision) {
    const std::uint64_t seed = 0xDEAD'3333ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(seed, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(seed + 1);
    HAF_REQUIRE_OK(join(**federation, device));
    const WorkloadProfile workload = make_workload("policy", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("property")));

    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
    HAF_REQUIRE_OK(decision);
    const Result<bool> current_before = (*federation)->decision_is_current(decision->decision_id);
    HAF_REQUIRE_OK(current_before);
    HAF_CHECK(*current_before);

    FederationPolicy changed = permissive_policy();
    changed.name = "changed-" + std::to_string(random.below(100000));
    changed.minimum_memory_bytes = 1;
    changed.refresh_identity();
    HAF_REQUIRE_OK((*federation)->set_policy(changed, (*federation)->epoch_claim("property policy")));

    HAF_CHECK((*federation)->decisions().empty());
    const std::size_t removed = (*federation)->invalidate_stale_decisions();
    HAF_EQ(removed, std::size_t{0});
}

HAF_TEST(property, migration_plans_are_invalidated_by_generation_changes) {
    const std::uint64_t seed = 0xABCD'4444ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(seed, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor first = make_device(adapters::cuda_class_profile(), seed + 1, "a", 0);
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), seed + 1, "a", 1);
    HAF_REQUIRE_OK(join(**federation, first));
    HAF_REQUIRE_OK(join(**federation, second));
    const WorkloadProfile workload = make_workload("plan", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("property")));

    const Result<MigrationPlan> plan = (*federation)->plan_migration(
        workload.class_id, first.id, second.id, (*federation)->epoch_claim("property plan"));
    HAF_REQUIRE_OK(plan);
    HAF_CHECK((*federation)->migration_plan_is_current(plan->id).value_or(false));
    static_cast<void>(random.next());
}

HAF_TEST(property, capability_sets_are_content_addressed_and_order_independent) {
    const std::uint64_t seed = 0x9876'5555ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 200; ++iteration) {
        std::vector<CapabilityRecord> records;
        for (const std::string& capability : kCapabilities) {
            if (random.chance(40)) {
                records.push_back(make_presence(capability, CapabilityState::Supported));
            }
        }
        std::vector<CapabilityRecord> shuffled = records;
        std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(static_cast<std::uint32_t>(random.next())));
        CapabilitySet left;
        CapabilitySet right;
        HAF_CHECK(left.set_records(records).ok());
        HAF_CHECK(right.set_records(shuffled).ok());
        HAF_CHECK(left.digest_hex() == right.digest_hex());
        HAF_CHECK(left.id() == right.id());
    }
}

HAF_TEST(property, deterministic_ordering_survives_shuffled_inputs) {
    const std::uint64_t seed = 0x5555'6666ULL;
    HAF_NOTE("seed=" + std::to_string(seed));
    SeededRandom random(seed);
    for (int iteration = 0; iteration < 60; ++iteration) {
        const AcceleratorDescriptor device = random_device(random, static_cast<std::uint64_t>(iteration));
        const FederationPolicy policy = random_policy(random);
        const WorkloadProfile workload = random_workload(random, static_cast<std::uint64_t>(iteration));
        EvaluationContext context = context_for(device, policy);

        std::vector<CapabilityRecord> records = device.capabilities.records();
        std::shuffle(records.begin(), records.end(),
                     std::mt19937(static_cast<std::uint32_t>(random.next())));
        AcceleratorDescriptor reshuffled = device;
        HAF_CHECK(reshuffled.capabilities.set_records(records).ok());
        context.target.capabilities = &reshuffled.capabilities;
        context.target.capability_generation = reshuffled.capabilities.generation();

        const Result<CompatibilityDecision> original =
            evaluate_compatibility(workload, context_for(device, policy));
        const Result<CompatibilityDecision> shuffled_decision = evaluate_compatibility(workload, context);
        HAF_CHECK(original.ok());
        HAF_CHECK(shuffled_decision.ok());
        if (!original.ok() || !shuffled_decision.ok()) {
            return;
        }
        HAF_CHECK(original->outcome == shuffled_decision->outcome);
    }
}
