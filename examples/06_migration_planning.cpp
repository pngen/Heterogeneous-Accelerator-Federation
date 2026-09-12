// Migration planning states exactly what must happen before work can move.

#include "example_support.hpp"

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-migration";
    config.id_seed = 6;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }
    const AcceleratorDescriptor first = device(adapters::cuda_class_profile(), 61, "profile", 0);
    const AcceleratorDescriptor second = device(adapters::cuda_class_profile(), 61, "profile", 1);
    const AcceleratorDescriptor rocm = device(adapters::rocm_class_profile(), 62, "profile-rocm", 0);
    if (!join(**federation, first).ok() || !join(**federation, second).ok() || !join(**federation, rocm).ok()) {
        line("cannot admit devices");
        return 1;
    }
    const WorkloadProfile workload =
        make_profile("migration-demo", {require("numeric.fp32", RequirementStrength::Hard)});
    if (!(*federation)->register_workload(workload, (*federation)->epoch_claim("example")).ok()) {
        line("cannot register workload");
        return 1;
    }

    line("Same vendor, same code object target:");
    const Result<MigrationPlan> native_plan = (*federation)->plan_migration(
        workload.class_id, first.id, second.id, (*federation)->epoch_claim("example plan"));
    if (!native_plan.ok()) {
        line("plan failed: " + native_plan.status().describe());
        return 1;
    }
    line(native_plan->render());

    line("");
    line("Cross vendor, no shared code object target:");
    const Result<MigrationPlan> cross_plan = (*federation)->plan_migration(
        workload.class_id, first.id, rocm.id, (*federation)->epoch_claim("example plan"));
    if (!cross_plan.ok()) {
        line("plan failed: " + cross_plan.status().describe());
        return 1;
    }
    line(cross_plan->render());

    // Advancing the lifecycle changes the state without changing the identity.
    const Result<MigrationPlan> validated = (*federation)->advance_migration(
        native_plan->id, MigrationState::Validated, (*federation)->epoch_claim("example validate"));
    if (!validated.ok()) {
        line("advance failed: " + validated.status().describe());
        return 1;
    }
    line("");
    line("after validation: state=" + std::string(to_string(validated->state)) +
         " same identity=" + (validated->id == native_plan->id ? "yes" : "no") +
         " same fingerprint=" + (validated->fingerprint_hex() == native_plan->fingerprint_hex() ? "yes" : "no"));

    // A stale authority cannot advance a plan.
    AuthorityClaim stale = (*federation)->epoch_claim("example stale");
    stale.epoch = CoordinatorEpoch(stale.epoch.value() + 3);
    const Result<MigrationPlan> refused =
        (*federation)->advance_migration(native_plan->id, MigrationState::Prepared, stale);
    line("stale advance: " + refused.status().describe());

    // A live-transfer requirement on a platform without live transfer is refused.
    WorkloadProfile live = make_profile("live-demo", {require("numeric.fp32", RequirementStrength::Hard)});
    live.migration_need = MigrationNeed::LiveStateTransfer;
    live.reconstruction_allowed = false;
    if (!(*federation)->register_workload(live, (*federation)->epoch_claim("example")).ok()) {
        line("cannot register live workload");
        return 1;
    }
    const Result<MigrationPlan> live_plan = (*federation)->plan_migration(
        live.class_id, first.id, second.id, (*federation)->epoch_claim("example plan"));
    if (!live_plan.ok()) {
        line("plan failed: " + live_plan.status().describe());
        return 1;
    }
    line("");
    line("live transfer requested but unavailable:");
    line(live_plan->render());
    return 0;
}
