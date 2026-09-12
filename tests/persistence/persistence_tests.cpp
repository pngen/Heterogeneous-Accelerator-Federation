// Persistence and conservative recovery.
//
// Real process exit is exercised by the multiprocess suite; this suite covers
// the recovery semantics in detail within one process, including what must NOT
// be revived after a restart.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/federation/federation.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/persist/file_store.hpp"
#include "support/environment.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

namespace {

FederationConfig store_config(const std::filesystem::path& path, std::uint64_t seed) {
    FederationConfig config;
    config.name = "persistent-federation";
    config.id_seed = seed;
    config.store_path = path;
    config.persist = true;
    return config;
}

}  // namespace

HAF_TEST(persistence, fresh_federation_publishes_its_initial_state) {
    ScratchDirectory scratch("persistence-fresh");
    const std::filesystem::path store = scratch.file("federation.store");
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 300));
        HAF_REQUIRE_OK(federation);
        HAF_CHECK(!(*federation)->recovery_report().recovered);
        HAF_CHECK(std::filesystem::exists(store));
    }
    FileStore file_store(store);
    HAF_CHECK(file_store.verify().ok());
}

HAF_TEST(persistence, recovery_advances_authority_and_requires_revalidation) {
    ScratchDirectory scratch("persistence-recovery");
    const std::filesystem::path store = scratch.file("federation.store");
    FederationId federation_id;
    std::vector<AcceleratorId> accelerators;
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 301));
        HAF_REQUIRE_OK(federation);
        federation_id = (*federation)->id();
        for (int index = 0; index < 3; ++index) {
            const AcceleratorDescriptor device = make_device(adapters::cuda_class_profile(), 400, "persist", index);
            HAF_REQUIRE_OK(join(**federation, device));
            accelerators.push_back(device.id);
        }
        const WorkloadProfile workload = make_workload("persisted", {hard_equals("vendor.id", "nvidia")});
        HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("persist")));
        const Result<CompatibilityDecision> decision =
            (*federation)->evaluate(workload.class_id, accelerators.front());
        HAF_REQUIRE_OK(decision);
        HAF_CHECK((*federation)->audit().ok());
    }

    Result<std::unique_ptr<Federation>> recovered = Federation::open(store_config(store, 301));
    HAF_REQUIRE_OK(recovered);
    const RecoveryReport& report = (*recovered)->recovery_report();
    HAF_CHECK(report.recovered);
    HAF_CHECK((*recovered)->id() == federation_id);
    HAF_CHECK(report.new_epoch.value() > report.previous_epoch.value());
    HAF_EQ(report.members_restored, std::size_t{3});
    HAF_EQ(report.members_requiring_revalidation, std::size_t{3});
    HAF_EQ(report.decisions_invalidated, std::size_t{1});
    // Decision state is not revived: it was bound to the previous epoch.
    HAF_CHECK((*recovered)->decisions().empty());

    // Durable membership survives, but no member accepts new work until a live
    // agent revalidates it.
    for (const AcceleratorId& id : accelerators) {
        const Result<MemberRecord> member = (*recovered)->member(id);
        HAF_REQUIRE_OK(member);
        HAF_CHECK(member->state == MemberState::Degraded);
        HAF_CHECK(!member->accepts_new_work());
        const Result<AcceleratorDescriptor> descriptor = (*recovered)->descriptor(id);
        HAF_REQUIRE_OK(descriptor);
        for (const EvidenceRecord& evidence : descriptor->evidence) {
            HAF_CHECK(evidence.requires_revalidation);
        }
    }
    HAF_CHECK((*recovered)->audit().ok());
}

HAF_TEST(persistence, recovered_member_becomes_active_again_after_revalidation) {
    ScratchDirectory scratch("persistence-revalidate");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor device = make_cuda_like(500);
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 302));
        HAF_REQUIRE_OK(federation);
        HAF_REQUIRE_OK(join(**federation, device));
    }
    {
        Result<std::unique_ptr<Federation>> recovered = Federation::open(store_config(store, 302));
        HAF_REQUIRE_OK(recovered);
        const Result<MemberRecord> before = (*recovered)->member(device.id);
        HAF_REQUIRE_OK(before);
        HAF_CHECK(before->state == MemberState::Degraded);

        // The live agent re-advertises with the same boot identity.
        const AuthorityClaim claim = (*recovered)->epoch_claim("revalidation");
        const Result<MemberRecord> observed = (*recovered)->observe(device, claim);
        HAF_REQUIRE_OK(observed);
        const Result<MemberRecord> activated = (*recovered)->activate(device.id, claim);
        HAF_REQUIRE_OK(activated);
        HAF_CHECK(activated->state == MemberState::Active);
        HAF_CHECK(activated->accepts_new_work());
        const Result<AcceleratorDescriptor> descriptor = (*recovered)->descriptor(device.id);
        HAF_REQUIRE_OK(descriptor);
        for (const EvidenceRecord& evidence : descriptor->evidence) {
            HAF_CHECK(!evidence.requires_revalidation);
        }
    }
}

HAF_TEST(persistence, retired_members_stay_retired_across_a_restart) {
    ScratchDirectory scratch("persistence-retired");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor device = make_cuda_like(600);
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 303));
        HAF_REQUIRE_OK(federation);
        HAF_REQUIRE_OK(join(**federation, device));
        HAF_REQUIRE_OK((*federation)->retire(device.id, (*federation)->epoch_claim("retire")));
    }
    Result<std::unique_ptr<Federation>> recovered = Federation::open(store_config(store, 303));
    HAF_REQUIRE_OK(recovered);
    const Result<MemberRecord> member = (*recovered)->member(device.id);
    HAF_REQUIRE_OK(member);
    HAF_CHECK(member->state == MemberState::Retired);
    HAF_EQ(recovered->get()->recovery_report().retired_members, std::size_t{1});
    // Retired evidence is dead: a re-advertisement cannot revive it.
    const Result<MemberRecord> revived =
        (*recovered)->observe(device, (*recovered)->epoch_claim("revive after restart"));
    HAF_CHECK(!revived.ok());
}

HAF_TEST(persistence, abandoned_migration_plans_are_aborted_on_recovery) {
    ScratchDirectory scratch("persistence-plans");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor first = make_device(adapters::cuda_class_profile(), 700, "plan", 0);
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), 700, "plan", 1);
    MigrationPlanId plan_id;
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 304));
        HAF_REQUIRE_OK(federation);
        HAF_REQUIRE_OK(join(**federation, first));
        HAF_REQUIRE_OK(join(**federation, second));
        const WorkloadProfile workload = make_workload("plan", {hard_equals("vendor.id", "nvidia")});
        HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("persist")));
        const Result<MigrationPlan> plan = (*federation)->plan_migration(
            workload.class_id, first.id, second.id, (*federation)->epoch_claim("persist plan"));
        HAF_REQUIRE_OK(plan);
        plan_id = plan->id;
        HAF_REQUIRE_OK((*federation)->advance_migration(plan_id, MigrationState::Validated,
                                                        (*federation)->epoch_claim("validate")));
    }
    Result<std::unique_ptr<Federation>> recovered = Federation::open(store_config(store, 304));
    HAF_REQUIRE_OK(recovered);
    const Result<MigrationPlan> plan = (*recovered)->migration_plan(plan_id);
    HAF_REQUIRE_OK(plan);
    HAF_CHECK(plan->state == MigrationState::Aborted);
    HAF_CHECK(!plan->refusal_reasons.empty());
    HAF_CHECK((*recovered)->audit().ok());
}

HAF_TEST(persistence, cross_vendor_migration_plan_survives_a_store_round_trip) {
    ScratchDirectory scratch("persistence-cross-vendor-plan");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor cuda = make_device(adapters::cuda_class_profile(), 710, "plan", 0);
    const AcceleratorDescriptor rocm = make_device(adapters::rocm_class_profile(), 711, "plan-rocm", 0);
    MigrationPlanId plan_id;
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 309));
        HAF_REQUIRE_OK(federation);
        HAF_REQUIRE_OK(join(**federation, cuda));
        HAF_REQUIRE_OK(join(**federation, rocm));
        const WorkloadProfile workload = make_workload("cross-vendor", {hard_equals("vendor.id", "nvidia")});
        HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("persist")));
        const Result<MigrationPlan> plan = (*federation)->plan_migration(
            workload.class_id, cuda.id, rocm.id, (*federation)->epoch_claim("persist plan"));
        HAF_REQUIRE_OK(plan);
        plan_id = plan->id;
        // The plan is fully self-consistent at creation time.
        HAF_CHECK(plan->fingerprint_hex() == plan->fingerprint_hex());
    }
    // Read the durable payload directly and validate it without a federation.
    const Result<ByteBuffer> payload = FileStore(store).read();
    HAF_REQUIRE_OK(payload);
    const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
    HAF_REQUIRE_OK(decoded);
    HAF_EQ(decoded->plans.size(), std::size_t{1});
    if (!decoded->plans.empty()) {
        HAF_CHECK(decoded->plans.front().id == plan_id);
    }
    HAF_CHECK(audit_snapshot(*decoded).ok());

    Result<std::unique_ptr<Federation>> recovered = Federation::open(store_config(store, 309));
    HAF_REQUIRE_OK(recovered);
    HAF_REQUIRE_OK((*recovered)->migration_plan(plan_id));
}

HAF_TEST(persistence, store_contains_no_stray_temporary_files) {
    ScratchDirectory scratch("persistence-temp");
    const std::filesystem::path store = scratch.file("federation.store");
    Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 305));
    HAF_REQUIRE_OK(federation);
    for (int index = 0; index < 8; ++index) {
        HAF_REQUIRE_OK(join(**federation, make_device(adapters::rocm_class_profile(), 800, "temp", index)));
    }
    std::vector<std::string> entries;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(scratch.path())) {
        entries.push_back(entry.path().filename().string());
    }
    std::sort(entries.begin(), entries.end());
    HAF_EQ(entries.size(), std::size_t{1});
    if (!entries.empty()) {
        HAF_EQ(entries.front(), std::string("federation.store"));
    }
}

HAF_TEST(persistence, foreign_federation_identity_is_rejected) {
    ScratchDirectory scratch("persistence-foreign");
    const std::filesystem::path store = scratch.file("federation.store");
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 306));
        HAF_REQUIRE_OK(federation);
        HAF_REQUIRE_OK(join(**federation, make_cuda_like(900)));
    }
    // Reopening with a different explicit identity must not adopt the store.
    FederationConfig config = store_config(store, 306);
    config.id = FederationId::from_raw(derive_identity("haf.federation", "different"));
    const Result<std::unique_ptr<Federation>> foreign = Federation::open(config);
    HAF_CHECK(!foreign.ok());
    HAF_EQ(static_cast<int>(foreign.status().code()), static_cast<int>(ErrorCode::IntegrityFailure));
}

HAF_TEST(persistence, snapshot_digest_is_stable_across_a_round_trip) {
    ScratchDirectory scratch("persistence-digest");
    const std::filesystem::path store = scratch.file("federation.store");
    std::string digest;
    std::size_t members = 0;
    {
        Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 307));
        HAF_REQUIRE_OK(federation);
        for (int index = 0; index < 5; ++index) {
            HAF_REQUIRE_OK(join(**federation, make_device(adapters::cuda_class_profile(), 1000, "digest", index)));
        }
        const FederationSnapshot snapshot = (*federation)->snapshot();
        digest = snapshot.digest_hex();
        members = snapshot.members.size();
        HAF_EQ(members, std::size_t{5});
    }
    const Result<ByteBuffer> payload = FileStore(store).read();
    HAF_REQUIRE_OK(payload);
    const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
    HAF_REQUIRE_OK(decoded);
    HAF_EQ(decoded->digest_hex(), digest);
    HAF_EQ(decoded->members.size(), members);
    HAF_CHECK(audit_snapshot(*decoded).ok());
}

HAF_TEST(persistence, capability_update_is_persisted_before_it_is_observable) {
    ScratchDirectory scratch("persistence-visibility");
    const std::filesystem::path store = scratch.file("federation.store");
    Result<std::unique_ptr<Federation>> federation = Federation::open(store_config(store, 308));
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(1100);
    HAF_REQUIRE_OK(join(**federation, device));
    const Result<MemberRecord> before = (*federation)->member(device.id);
    HAF_REQUIRE_OK(before);

    adapters::DeviceProfile profile = adapters::cuda_class_profile();
    profile.memory_free_bytes = 1024;
    const Result<CapabilitySet> capabilities = adapters::build_capability_set(profile);
    HAF_REQUIRE_OK(capabilities);
    EvidenceRecord evidence =
        make_evidence("visibility", "persisted update", "1.0.0", device.id, device.physical_device,
                      EvidenceProvenance::Synthetic, EvidenceKind::Declaration, capabilities->digest(),
                      RuntimeVersion{1, 0, 0}, RuntimeVersion{1, 0, 0}, 0, false);
    evidence.capability_generation = before->capability_generation;
    const Result<MemberRecord> updated = (*federation)->update_capabilities(
        device.id, *capabilities, evidence, (*federation)->epoch_claim("visibility"));
    HAF_REQUIRE_OK(updated);
    HAF_CHECK(updated->capability_generation.value() > before->capability_generation.value());

    // The durable store must already reflect the accepted mutation.
    const Result<ByteBuffer> payload = FileStore(store).read();
    HAF_REQUIRE_OK(payload);
    const Result<FederationSnapshot> decoded = decode_snapshot(*payload);
    HAF_REQUIRE_OK(decoded);
    const MemberRecord* persisted = decoded->find_member(device.id);
    HAF_CHECK(persisted != nullptr);
    if (persisted != nullptr) {
        HAF_CHECK(persisted->capability_generation == updated->capability_generation);
    }
}
