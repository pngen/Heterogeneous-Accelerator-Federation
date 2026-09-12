// Engine contracts: compatibility, portability, ranking, migration planning,
// and the invariant audit.

#include <algorithm>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/engine/audit.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/engine/migration_planner.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/engine/ranking.hpp"
#include "haf/federation/federation.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

namespace {

EvaluationContext make_context(const AcceleratorDescriptor& target, const FederationPolicy& policy) {
    EvaluationContext context;
    context.federation = FederationId::from_raw(derive_identity("haf.test.federation", "engine"));
    context.federation_generation = FederationGeneration(3);
    context.epoch = CoordinatorEpoch(2);
    context.policy_id = policy.id;
    context.policy_generation = policy.generation;
    context.policy = &policy;
    context.workload_revision = WorkloadRevision(11);
    context.now = now_monotonic();
    context.target.accelerator = target.id;
    context.target.device_generation = target.generation;
    context.target.capability_generation = target.capabilities.generation();
    context.target.evidence_generation = EvidenceGeneration(4);
    context.target.support_level = target.support_level;
    context.target.provenance = target.provenance;
    context.target.capabilities = &target.capabilities;
    context.target.evidence_fresh = true;
    context.target.accepts_new_work = true;
    return context;
}

}  // namespace

HAF_TEST(engine, eligible_when_every_hard_requirement_is_met) {
    const AcceleratorDescriptor cuda = make_cuda_like(10);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload(
        "fp8", {hard_equals("vendor.id", "nvidia"), hard_present("numeric.fp8_e4m3"),
                hard_tokens("isa.code_object_targets", {"sm-120"}),
                hard_at_least("memory.total_bytes", 8LL * 1024 * 1024 * 1024)});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(cuda, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Eligible));
    HAF_EQ(static_cast<int>(decision->portability), static_cast<int>(PortabilityClass::Native));
    HAF_CHECK(decision->reasons.empty());
}

HAF_TEST(engine, hard_requirement_filtering_is_specific) {
    const AcceleratorDescriptor rocm = make_rocm_like(11);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload =
        make_workload("cuda-only", {hard_equals("vendor.id", "nvidia"),
                                    hard_tokens("isa.code_object_targets", {"sm-120"}),
                                    hard_present("tensor.warp_group_mma")});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(rocm, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Ineligible));
    std::vector<ErrorCode> codes;
    for (const CompatibilityReason& reason : decision->reasons) {
        codes.push_back(reason.code);
    }
    HAF_CHECK(std::find(codes.begin(), codes.end(), ErrorCode::VendorMismatch) != codes.end());
    HAF_CHECK(std::find(codes.begin(), codes.end(), ErrorCode::IsaIncompatible) != codes.end());
    // A tensor feature the device positively lacks is reported as an absent
    // required capability, not as a numeric-mode problem.
    HAF_CHECK(std::find(codes.begin(), codes.end(), ErrorCode::UnsupportedCapability) != codes.end());
}

HAF_TEST(engine, unknown_fails_closed) {
    const AcceleratorDescriptor device = make_cuda_like(12);
    // Drop every numeric record and stop claiming the numeric namespace. The
    // capability is then UNKNOWN rather than positively unsupported, and the
    // decision must be UNKNOWN rather than ELIGIBLE.
    AcceleratorDescriptor open = device;
    std::vector<CapabilityRecord> records = open.capabilities.records();
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const CapabilityRecord& record) {
                                     return record.key.name().rfind("numeric.", 0) == 0;
                                 }),
                  records.end());
    HAF_REQUIRE_OK(open.capabilities.set_records(records));
    HAF_REQUIRE_OK(open.capabilities.set_closed_namespaces({"vendor", "architecture", "isa", "memory"}));
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("needs-fp8", {hard_present("numeric.fp8_e4m3")});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(open, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Unknown));
    HAF_CHECK(!decision->reasons.empty());
    HAF_EQ(static_cast<int>(decision->reasons.front().code), static_cast<int>(ErrorCode::UnknownCapability));
}

HAF_TEST(engine, soft_preferences_never_rescue_an_ineligible_device) {
    const AcceleratorDescriptor rocm = make_rocm_like(13);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload(
        "soft-only", {hard_equals("vendor.id", "nvidia"), soft_present("tensor.matrix_engine", 1.0),
                      soft_present("memory.ecc_enabled", 0.5)});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(rocm, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Ineligible));
}

HAF_TEST(engine, soft_preferences_influence_score_deterministically) {
    const AcceleratorDescriptor cuda = make_cuda_like(14);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile with_preference =
        make_workload("scored", {hard_equals("vendor.id", "nvidia"), soft_present("memory.ecc_enabled", 0.75)});
    const WorkloadProfile without_preference =
        make_workload("scored", {hard_equals("vendor.id", "nvidia")});
    const Result<CompatibilityDecision> first = evaluate_compatibility(with_preference, make_context(cuda, policy));
    const Result<CompatibilityDecision> second = evaluate_compatibility(with_preference, make_context(cuda, policy));
    const Result<CompatibilityDecision> third = evaluate_compatibility(without_preference, make_context(cuda, policy));
    HAF_REQUIRE_OK(first);
    HAF_REQUIRE_OK(second);
    HAF_REQUIRE_OK(third);
    HAF_EQ(first->score, second->score);
    HAF_CHECK(first->score > third->score);
    // Identical inputs produce identical fingerprints.
    HAF_CHECK(first->fingerprint_hex() == second->fingerprint_hex());
}

HAF_TEST(engine, decision_fingerprint_binds_every_generation) {
    const AcceleratorDescriptor cuda = make_cuda_like(15);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("bound", {hard_equals("vendor.id", "nvidia")});
    EvaluationContext context = make_context(cuda, policy);
    const Result<CompatibilityDecision> base = evaluate_compatibility(workload, context);
    HAF_REQUIRE_OK(base);

    context.policy_generation = PolicyGeneration(policy.generation.value() + 1);
    const Result<CompatibilityDecision> changed_policy = evaluate_compatibility(workload, context);
    HAF_REQUIRE_OK(changed_policy);
    HAF_CHECK(base->fingerprint_hex() != changed_policy->fingerprint_hex());

    context = make_context(cuda, policy);
    context.target.capability_generation = CapabilityGeneration(context.target.capability_generation.value() + 1);
    const Result<CompatibilityDecision> changed_capability = evaluate_compatibility(workload, context);
    HAF_REQUIRE_OK(changed_capability);
    HAF_CHECK(base->fingerprint_hex() != changed_capability->fingerprint_hex());

    context = make_context(cuda, policy);
    context.epoch = CoordinatorEpoch(context.epoch.value() + 1);
    const Result<CompatibilityDecision> changed_epoch = evaluate_compatibility(workload, context);
    HAF_REQUIRE_OK(changed_epoch);
    HAF_CHECK(base->fingerprint_hex() != changed_epoch->fingerprint_hex());
}

HAF_TEST(engine, policy_rejecting_synthetic_evidence_makes_members_ineligible) {
    const AcceleratorDescriptor synthetic = make_rocm_like(16);
    const FederationPolicy policy = strict_real_evidence_policy();
    const WorkloadProfile workload = make_workload("any", {});
    const Result<CompatibilityDecision> decision =
        evaluate_compatibility(workload, make_context(synthetic, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Ineligible));
}

HAF_TEST(engine, degraded_capability_fails_closed_by_default) {
    AcceleratorDescriptor device = make_cuda_like(17);
    std::vector<CapabilityRecord> records = device.capabilities.records();
    for (CapabilityRecord& record : records) {
        if (record.key.name() == "numeric.fp8_e4m3") {
            record.state = CapabilityState::Degraded;
        }
    }
    HAF_REQUIRE_OK(device.capabilities.set_records(records));
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("fp8", {hard_present("numeric.fp8_e4m3")});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(device, policy));
    HAF_REQUIRE_OK(decision);
    HAF_EQ(static_cast<int>(decision->outcome), static_cast<int>(CompatibilityOutcome::Ineligible));

    FederationPolicy tolerant = permissive_policy();
    tolerant.allow_degraded_capabilities = true;
    tolerant.refresh_identity();
    const Result<CompatibilityDecision> accepted = evaluate_compatibility(workload, make_context(device, tolerant));
    HAF_REQUIRE_OK(accepted);
    HAF_EQ(static_cast<int>(accepted->outcome), static_cast<int>(CompatibilityOutcome::Eligible));
}

HAF_TEST(engine, portability_native_within_one_vendor_and_architecture) {
    const AcceleratorDescriptor first = make_cuda_like(18);
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), 19, "other-adapter", 1);
    PortabilityRequest request;
    request.source_capabilities = &first.capabilities;
    request.destination_capabilities = &second.capabilities;
    request.source_vendor = "nvidia";
    request.destination_vendor = "nvidia";
    const PortabilityResult result = classify_portability(request);
    HAF_EQ(static_cast<int>(result.value), static_cast<int>(PortabilityClass::Native));
}

HAF_TEST(engine, portability_downgrades_across_vendors) {
    const AcceleratorDescriptor cuda = make_cuda_like(20);
    const AcceleratorDescriptor rocm = make_rocm_like(21);
    PortabilityRequest request;
    request.source_capabilities = &cuda.capabilities;
    request.destination_capabilities = &rocm.capabilities;
    request.source_vendor = "nvidia";
    request.destination_vendor = "amd";
    const PortabilityResult result = classify_portability(request);
    HAF_CHECK(result.value == PortabilityClass::RecompileRequired ||
              result.value == PortabilityClass::RepackageRequired);
    HAF_CHECK(!implies_live_state_transfer(result.value));
    HAF_CHECK(!requires_state_reconstruction(result.value) || result.value == PortabilityClass::RepackageRequired);
}

HAF_TEST(engine, portability_is_unknown_when_inputs_are_missing) {
    AcceleratorDescriptor device = make_cuda_like(22);
    std::vector<CapabilityRecord> records = device.capabilities.records();
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const CapabilityRecord& record) {
                                     return record.key.name() == "isa.code_object_targets";
                                 }),
                  records.end());
    HAF_REQUIRE_OK(device.capabilities.set_records(records));
    HAF_REQUIRE_OK(device.capabilities.set_closed_namespaces({"vendor"}));
    PortabilityRequest request;
    request.source_capabilities = &device.capabilities;
    request.destination_capabilities = &device.capabilities;
    request.source_vendor = "nvidia";
    request.destination_vendor = "nvidia";
    const PortabilityResult result = classify_portability(request);
    HAF_EQ(static_cast<int>(result.value), static_cast<int>(PortabilityClass::Unknown));
}

HAF_TEST(engine, ranking_places_eligible_candidates_first_and_excludes_the_rest) {
    const AcceleratorDescriptor cuda = make_cuda_like(23);
    const AcceleratorDescriptor rocm = make_rocm_like(24);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("ranked", {hard_equals("vendor.id", "nvidia")});
    const Result<CompatibilityDecision> cuda_decision =
        evaluate_compatibility(workload, make_context(cuda, policy));
    const Result<CompatibilityDecision> rocm_decision =
        evaluate_compatibility(workload, make_context(rocm, policy));
    HAF_REQUIRE_OK(cuda_decision);
    HAF_REQUIRE_OK(rocm_decision);

    std::vector<RankingCandidate> candidates;
    RankingCandidate first;
    first.accelerator = cuda.id;
    first.decision = *cuda_decision;
    candidates.push_back(first);
    RankingCandidate second;
    second.accelerator = rocm.id;
    second.decision = *rocm_decision;
    candidates.push_back(second);

    const Result<RankingResult> ranked = rank_candidates(candidates, RankingWeights{});
    HAF_REQUIRE_OK(ranked);
    HAF_EQ(ranked->ranked.size(), std::size_t{1});
    HAF_EQ(ranked->excluded.size(), std::size_t{1});
    HAF_CHECK(ranked->ranked.front().accelerator == cuda.id);
    HAF_EQ(ranked->ranked.front().rank, std::size_t{1});
}

HAF_TEST(engine, ranking_tie_break_is_the_stable_identity) {
    const AcceleratorDescriptor cuda = make_cuda_like(25);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("tie", {hard_equals("vendor.id", "nvidia")});
    const Result<CompatibilityDecision> decision = evaluate_compatibility(workload, make_context(cuda, policy));
    HAF_REQUIRE_OK(decision);

    std::vector<RankingCandidate> forward;
    for (int index = 0; index < 4; ++index) {
        RankingCandidate candidate;
        candidate.accelerator = AcceleratorId::from_raw(derive_identity("haf.accelerator", "tie-" + std::to_string(index)));
        candidate.decision = *decision;
        candidate.decision.accelerator = candidate.accelerator;
        forward.push_back(candidate);
    }
    std::vector<RankingCandidate> reversed(forward.rbegin(), forward.rend());
    const Result<RankingResult> left = rank_candidates(forward, RankingWeights{});
    const Result<RankingResult> right = rank_candidates(reversed, RankingWeights{});
    HAF_REQUIRE_OK(left);
    HAF_REQUIRE_OK(right);
    HAF_EQ(left->ranked.size(), right->ranked.size());
    for (std::size_t i = 0; i < left->ranked.size(); ++i) {
        HAF_CHECK(left->ranked[i].accelerator == right->ranked[i].accelerator);
    }
}

HAF_TEST(engine, migration_outcome_mapping_is_explicit) {
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::Native, MigrationNeed::RestartOnly, true, true,
                                                     false, false)),
           static_cast<int>(MigrationOutcome::Restart));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::LiveMigrationSupported,
                                                     MigrationNeed::LiveStateTransfer, false, false, true, true)),
           static_cast<int>(MigrationOutcome::MoveNativeState));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::CheckpointRestoreSupported,
                                                     MigrationNeed::CheckpointRestore, false, false, true, false)),
           static_cast<int>(MigrationOutcome::RestoreCheckpoint));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::RecompileRequired,
                                                     MigrationNeed::CheckpointRestore, false, false, true, false)),
           static_cast<int>(MigrationOutcome::RecompileThenRestore));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::RecompileRequired, MigrationNeed::RestartOnly,
                                                     true, true, false, false)),
           static_cast<int>(MigrationOutcome::RepackageAndRestart));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::StateReconstructionRequired,
                                                     MigrationNeed::RestartOnly, true, true, false, false)),
           static_cast<int>(MigrationOutcome::Reconstruct));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::Unsupported, MigrationNeed::RestartOnly, true,
                                                     true, false, false)),
           static_cast<int>(MigrationOutcome::Unsupported));
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::Unknown, MigrationNeed::RestartOnly, true, true,
                                                     false, false)),
           static_cast<int>(MigrationOutcome::Unknown));
    // Live transfer demanded but not possible is refused rather than downgraded.
    HAF_EQ(static_cast<int>(derive_migration_outcome(PortabilityClass::CheckpointRestoreSupported,
                                                     MigrationNeed::LiveStateTransfer, true, true, true, false)),
           static_cast<int>(MigrationOutcome::Unsupported));
}

HAF_TEST(engine, migration_planner_refuses_cross_vendor_live_transfer) {
    const AcceleratorDescriptor cuda = make_cuda_like(26);
    const AcceleratorDescriptor rocm = make_rocm_like(27);
    const FederationPolicy policy = permissive_policy();
    WorkloadProfile workload = make_workload("live", {});
    workload.migration_need = MigrationNeed::LiveStateTransfer;
    workload.reconstruction_allowed = false;

    MigrationRequest request;
    request.federation = FederationId::from_raw(derive_identity("haf.test.federation", "migration"));
    request.federation_generation = FederationGeneration(1);
    request.epoch = CoordinatorEpoch(1);
    request.policy = &policy;
    request.policy_id = policy.id;
    request.policy_generation = policy.generation;
    request.source.accelerator = cuda.id;
    request.source.device_generation = cuda.generation;
    request.source.capability_generation = cuda.capabilities.generation();
    request.source.capabilities = &cuda.capabilities;
    request.source.vendor = "nvidia";
    request.source.provenance = cuda.provenance;
    request.destination.accelerator = rocm.id;
    request.destination.device_generation = rocm.generation;
    request.destination.capability_generation = rocm.capabilities.generation();
    request.destination.capabilities = &rocm.capabilities;
    request.destination.vendor = "amd";
    request.destination.provenance = rocm.provenance;
    request.workload = &workload;
    request.workload_revision = workload.revision;

    const Result<MigrationPlan> plan = plan_migration(request);
    HAF_REQUIRE_OK(plan);
    HAF_EQ(static_cast<int>(plan->outcome), static_cast<int>(MigrationOutcome::Unsupported));
    HAF_EQ(static_cast<int>(plan->state), static_cast<int>(MigrationState::Refused));
    HAF_CHECK(!plan->refusal_reasons.empty());
    HAF_EQ(static_cast<int>(plan->estimated_cost), 0);
}

HAF_TEST(engine, migration_planner_produces_deterministic_plans) {
    const AcceleratorDescriptor first = make_cuda_like(28);
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), 29, "other-adapter", 1);
    const FederationPolicy policy = permissive_policy();
    const WorkloadProfile workload = make_workload("restart", {});

    MigrationRequest request;
    request.federation = FederationId::from_raw(derive_identity("haf.test.federation", "determinism"));
    request.federation_generation = FederationGeneration(1);
    request.epoch = CoordinatorEpoch(1);
    request.policy = &policy;
    request.policy_id = policy.id;
    request.policy_generation = policy.generation;
    request.source.accelerator = first.id;
    request.source.device_generation = first.generation;
    request.source.capability_generation = first.capabilities.generation();
    request.source.capabilities = &first.capabilities;
    request.source.vendor = "nvidia";
    request.destination.accelerator = second.id;
    request.destination.device_generation = second.generation;
    request.destination.capability_generation = second.capabilities.generation();
    request.destination.capabilities = &second.capabilities;
    request.destination.vendor = "nvidia";
    request.workload = &workload;
    request.workload_revision = workload.revision;

    const Result<MigrationPlan> left = plan_migration(request);
    const Result<MigrationPlan> right = plan_migration(request);
    HAF_REQUIRE_OK(left);
    HAF_REQUIRE_OK(right);
    HAF_CHECK(left->fingerprint_hex() == right->fingerprint_hex());
    HAF_CHECK(left->id == right->id);
    HAF_CHECK(left->render() == right->render());
}

HAF_TEST(engine, audit_reports_zero_violations_for_a_healthy_federation) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(30, permissive_policy());
    HAF_REQUIRE_OK(federation);
    HAF_REQUIRE_OK(join(**federation, make_cuda_like(31)));
    HAF_REQUIRE_OK(join(**federation, make_rocm_like(32)));
    const AuditReport report = (*federation)->audit();
    HAF_CHECK(report.ok());
    HAF_EQ(report.violations.size(), std::size_t{0});
    HAF_CHECK(report.checks_run > 0);
    HAF_CHECK(report.render().find("violations=0") != std::string::npos);
}

HAF_TEST(engine, fleet_evaluation_is_deterministic_and_ordered) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(33, permissive_policy());
    HAF_REQUIRE_OK(federation);
    HAF_REQUIRE_OK(join(**federation, make_cuda_like(34)));
    HAF_REQUIRE_OK(join(**federation, make_rocm_like(35)));
    HAF_REQUIRE_OK(join(**federation, make_intel_like(36)));
    const WorkloadProfile workload = make_workload("fleet", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("test")));
    const Result<FleetEvaluation> first = (*federation)->evaluate_fleet(workload.class_id);
    const Result<FleetEvaluation> second = (*federation)->evaluate_fleet(workload.class_id);
    HAF_REQUIRE_OK(first);
    HAF_REQUIRE_OK(second);
    HAF_EQ(first->cells.size(), std::size_t{3});
    HAF_EQ(first->eligible_count, std::size_t{1});
    HAF_EQ(first->ineligible_count, std::size_t{2});
    HAF_CHECK(first->render() == second->render());
}
