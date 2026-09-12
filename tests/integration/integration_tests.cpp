// End-to-end library flows that a downstream consumer would perform.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/federation/federation.hpp"
#include "haf/persist/file_store.hpp"
#include "haf/persist/file_store.hpp"
#include "support/environment.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

HAF_TEST(integration, heterogeneous_federation_end_to_end) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(900, permissive_policy());
    HAF_REQUIRE_OK(federation);

    HAF_PHASE("ADMIT");
    const AcceleratorDescriptor cuda = make_cuda_like(901);
    const AcceleratorDescriptor rocm = make_rocm_like(902);
    const AcceleratorDescriptor intel = make_intel_like(903);
    HAF_REQUIRE_OK(join(**federation, cuda));
    HAF_REQUIRE_OK(join(**federation, rocm));
    HAF_REQUIRE_OK(join(**federation, intel));
    HAF_EQ((*federation)->member_count(), std::size_t{3});

    HAF_PHASE("EVALUATE");
    const WorkloadProfile fp8 = make_workload(
        "llm-decode-fp8",
        {hard_equals("vendor.id", "nvidia"), hard_present("numeric.fp8_e4m3"),
         hard_tokens("isa.code_object_targets", {"sm-120"}), hard_at_least("memory.total_bytes", 1LL << 30)});
    HAF_REQUIRE_OK((*federation)->register_workload(fp8, (*federation)->epoch_claim("integration")));

    const Result<CompatibilityDecision> cuda_decision = (*federation)->evaluate(fp8.class_id, cuda.id);
    HAF_REQUIRE_OK(cuda_decision);
    HAF_CHECK(cuda_decision->outcome == CompatibilityOutcome::Eligible);

    const Result<bool> verified = (*federation)->verify_decision(cuda_decision->decision_id);
    HAF_REQUIRE_OK(verified);
    HAF_CHECK(*verified);

    HAF_PHASE("PLAN");
    const Result<MigrationPlan> plan = (*federation)->plan_migration(
        fp8.class_id, cuda.id, rocm.id, (*federation)->epoch_claim("integration plan"));
    HAF_REQUIRE_OK(plan);
    HAF_CHECK(plan->outcome == MigrationOutcome::RepackageAndRestart ||
              plan->outcome == MigrationOutcome::Unsupported ||
              plan->outcome == MigrationOutcome::Reconstruct);
    HAF_CHECK(!plan->steps.empty() || plan->state == MigrationState::Refused);

    HAF_PHASE("VERIFY");
    HAF_CHECK((*federation)->audit().ok());
    const Result<std::string> rendering = (*federation)->render_members();
    HAF_REQUIRE_OK(rendering);
    HAF_CHECK(rendering->find("vendor=nvidia") != std::string::npos);
    HAF_CHECK(rendering->find("vendor=amd") != std::string::npos);
    HAF_CHECK(rendering->find("vendor=intel") != std::string::npos);
}

HAF_TEST(integration, workload_portability_threshold_is_enforced) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(910, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor cuda = make_cuda_like(911);
    const AcceleratorDescriptor rocm = make_rocm_like(912);
    HAF_REQUIRE_OK(join(**federation, cuda));
    HAF_REQUIRE_OK(join(**federation, rocm));

    WorkloadProfile workload = make_workload("needs-native", {hard_equals("vendor.id", "amd")});
    workload.minimum_portability = PortabilityClass::Native;
    workload.execution_mode = ExecutionMode::ExactBinary;
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("integration")));

    EvaluationOptions options;
    options.source = cuda.id;
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, rocm.id, options);
    HAF_REQUIRE_OK(decision);
    HAF_CHECK(decision->outcome == CompatibilityOutcome::Ineligible);
    bool saw_portability_rejection = false;
    for (const CompatibilityReason& reason : decision->reasons) {
        if (reason.code == ErrorCode::PortabilityConstraintViolated) {
            saw_portability_rejection = true;
        }
    }
    HAF_CHECK(saw_portability_rejection);
}

HAF_TEST(integration, policy_change_advances_generation_and_invalidates_decisions) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(920, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor cuda = make_cuda_like(921);
    HAF_REQUIRE_OK(join(**federation, cuda));
    const WorkloadProfile workload = make_workload("policy", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("integration")));

    const PolicyGeneration before = (*federation)->policy_generation();
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, cuda.id);
    HAF_REQUIRE_OK(decision);

    FederationPolicy restricted = permissive_policy();
    restricted.name = "restricted";
    restricted.forbidden_vendors = {"nvidia"};
    restricted.refresh_identity();
    const Result<PolicyGeneration> applied =
        (*federation)->set_policy(restricted, (*federation)->epoch_claim("integration policy"));
    HAF_REQUIRE_OK(applied);
    HAF_CHECK(applied->value() != before.value());
    HAF_CHECK((*federation)->decisions().empty());

    const Result<CompatibilityDecision> after = (*federation)->evaluate(workload.class_id, cuda.id);
    HAF_REQUIRE_OK(after);
    HAF_CHECK(after->outcome == CompatibilityOutcome::Ineligible);
    HAF_CHECK(after->fingerprint_hex() != decision->fingerprint_hex());
}

HAF_TEST(integration, migration_commit_moves_authority_and_is_refused_when_stale) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(930, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor first = make_device(adapters::cuda_class_profile(), 931, "integration", 0);
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), 931, "integration", 1);
    HAF_REQUIRE_OK(join(**federation, first));
    HAF_REQUIRE_OK(join(**federation, second));
    WorkloadProfile workload = make_workload("move", {hard_equals("vendor.id", "nvidia")});
    workload.migration_need = MigrationNeed::RestartOnly;
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("integration")));

    const Result<MigrationPlan> plan = (*federation)->plan_migration(
        workload.class_id, first.id, second.id, (*federation)->epoch_claim("integration plan"));
    HAF_REQUIRE_OK(plan);
    HAF_CHECK(plan->state == MigrationState::Planned);

    HAF_REQUIRE_OK((*federation)->advance_migration(plan->id, MigrationState::Validated,
                                                    (*federation)->epoch_claim("validate")));
    HAF_REQUIRE_OK((*federation)->advance_migration(plan->id, MigrationState::Prepared,
                                                    (*federation)->epoch_claim("prepare")));
    HAF_REQUIRE_OK((*federation)->advance_migration(plan->id, MigrationState::Transferring,
                                                    (*federation)->epoch_claim("transfer")));
    HAF_REQUIRE_OK((*federation)->advance_migration(plan->id, MigrationState::Verified,
                                                    (*federation)->epoch_claim("verify")));
    const Result<MigrationPlan> committed =
        (*federation)->commit_migration(plan->id, (*federation)->epoch_claim("commit"));
    HAF_REQUIRE_OK(committed);
    HAF_CHECK(committed->state == MigrationState::Committed);

    // Authority moved: the source no longer accepts new work.
    const Result<MemberRecord> source = (*federation)->member(first.id);
    HAF_REQUIRE_OK(source);
    HAF_CHECK(source->state == MemberState::Draining);
    HAF_CHECK(!source->accepts_new_work());
    HAF_CHECK((*federation)->audit().ok());

    // Re-committing an already committed plan is an idempotent no-op, not a
    // second authority transfer.
    const Result<MigrationPlan> replayed =
        (*federation)->commit_migration(plan->id, (*federation)->epoch_claim("replay commit"));
    HAF_REQUIRE_OK(replayed);
    HAF_CHECK(replayed->state == MigrationState::Committed);

    // A plan whose endpoint generation changed after it was authored is stale
    // and must be refused rather than acted upon.
    const Result<MigrationPlan> second_plan = (*federation)->plan_migration(
        workload.class_id, first.id, second.id, (*federation)->epoch_claim("second plan"));
    HAF_REQUIRE_OK(second_plan);
    {
        adapters::DeviceProfile profile = adapters::cuda_class_profile();
        profile.memory_free_bytes = 4096;
        const Result<CapabilitySet> capabilities = adapters::build_capability_set(profile);
        HAF_REQUIRE_OK(capabilities);
        const Result<MemberRecord> current = (*federation)->member(first.id);
        HAF_REQUIRE_OK(current);
        EvidenceRecord evidence = make_evidence("integration", "endpoint change", "1.0.0", first.id,
                                                first.physical_device, EvidenceProvenance::Synthetic,
                                                EvidenceKind::Declaration, capabilities->digest(),
                                                RuntimeVersion{1, 0, 0}, RuntimeVersion{1, 0, 0}, 0, false);
        evidence.capability_generation = current->capability_generation;
        HAF_REQUIRE_OK((*federation)->update_capabilities(first.id, *capabilities, evidence,
                                                          (*federation)->epoch_claim("endpoint change")));
    }
    const Result<MigrationPlan> stale_advance = (*federation)->advance_migration(
        second_plan->id, MigrationState::Validated, (*federation)->epoch_claim("stale advance"));
    HAF_CHECK(!stale_advance.ok());
    HAF_EQ(static_cast<int>(stale_advance.status().code()), static_cast<int>(ErrorCode::StaleMigration));
}

HAF_TEST(integration, capability_change_makes_prior_decisions_stale) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(940, permissive_policy());
    HAF_REQUIRE_OK(federation);
    AcceleratorDescriptor device = make_cuda_like(941);
    HAF_REQUIRE_OK(join(**federation, device));
    const WorkloadProfile workload = make_workload("stale-decision", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("integration")));
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
    HAF_REQUIRE_OK(decision);
    HAF_CHECK((*federation)->decision_is_current(decision->decision_id).value_or(false));

    // A capability update advances the capability generation.
    adapters::DeviceProfile profile = adapters::cuda_class_profile();
    profile.memory_free_bytes = 512;
    const Result<CapabilitySet> capabilities = adapters::build_capability_set(profile);
    HAF_REQUIRE_OK(capabilities);
    const Result<MemberRecord> current = (*federation)->member(device.id);
    HAF_REQUIRE_OK(current);
    EvidenceRecord evidence = make_evidence("integration", "capability change", "1.0.0", device.id,
                                            device.physical_device, EvidenceProvenance::Synthetic,
                                            EvidenceKind::Declaration, capabilities->digest(), RuntimeVersion{1, 0, 0},
                                            RuntimeVersion{1, 0, 0}, 0, false);
    evidence.capability_generation = current->capability_generation;
    HAF_REQUIRE_OK((*federation)->update_capabilities(device.id, *capabilities, evidence,
                                                      (*federation)->epoch_claim("capability change")));

    HAF_CHECK(!(*federation)->decision_is_current(decision->decision_id).value_or(true));
    HAF_CHECK((*federation)->invalidate_stale_decisions() >= 1);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(integration, event_stream_reflects_every_lifecycle_step) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(950, permissive_policy());
    HAF_REQUIRE_OK(federation);
    std::vector<EventKind> kinds;
    (*federation)->set_event_sink([&kinds](const FederationEvent& event) { kinds.push_back(event.kind); });

    const AcceleratorDescriptor device = make_cuda_like(951);
    HAF_REQUIRE_OK(join(**federation, device));
    HAF_REQUIRE_OK((*federation)->fence(device.id, (*federation)->epoch_claim("integration")));
    HAF_REQUIRE_OK((*federation)->retire(device.id, (*federation)->epoch_claim("integration")));

    const auto contains = [&kinds](EventKind kind) {
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    };
    HAF_CHECK(contains(EventKind::MemberObserved));
    HAF_CHECK(contains(EventKind::MemberAdmitted));
    HAF_CHECK(contains(EventKind::MemberActivated));
    HAF_CHECK(contains(EventKind::MemberFenced));
    HAF_CHECK(contains(EventKind::MemberRetired));
    HAF_CHECK(contains(EventKind::StorePersisted));
}

HAF_TEST(integration, repeated_revalidation_cycles_keep_the_audit_clean) {
    ScratchDirectory scratch("integration-cycles");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor device = make_device(adapters::cuda_class_profile(), 970, "cycles", 0);
    FederationId federation_id;
    // Each cycle models a coordinator restart followed by agent revalidation.
    for (int cycle = 0; cycle < 6; ++cycle) {
        FederationConfig config;
        config.name = "cycles";
        config.id_seed = 971;
        config.store_path = store;
        Result<std::unique_ptr<Federation>> federation = Federation::open(config);
        HAF_REQUIRE_OK(federation);
        if (cycle == 0) {
            federation_id = (*federation)->id();
        } else {
            HAF_CHECK((*federation)->id() == federation_id);
        }
        // The first cycle walks the full lifecycle; later cycles revalidate a
        // member that recovery left in DEGRADED.
        const Result<MemberRecord> active = join(**federation, device);
        HAF_REQUIRE_OK(active);
        HAF_CHECK(active->state == MemberState::Active);
        const AuditReport report = (*federation)->audit();
        if (!report.ok()) {
            HAF_NOTE("cycle " + std::to_string(cycle) + " audit: " + report.render());
        }
        HAF_CHECK(report.ok());
    }
}

HAF_TEST(integration, audit_detects_a_tampered_snapshot) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(960, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(961);
    HAF_REQUIRE_OK(join(**federation, device));
    FederationSnapshot snapshot = (*federation)->snapshot();
    HAF_CHECK(audit_snapshot(snapshot).ok());

    // A member whose descriptor has been replaced with a different incarnation
    // must be reported rather than silently trusted.
    snapshot.descriptors.front().generation = DeviceGeneration(42);
    const AuditReport report = audit_snapshot(snapshot);
    HAF_CHECK(!report.ok());
    bool saw_generation_violation = false;
    for (const AuditViolation& violation : report.violations) {
        if (violation.code == "membership.device_generation") {
            saw_generation_violation = true;
        }
    }
    HAF_CHECK(saw_generation_violation);
}
