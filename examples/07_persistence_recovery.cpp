// Durable state, conservative recovery, and what a restart must NOT revive.

#include "example_support.hpp"

#include <filesystem>
#include <string>

#include "haf/persist/file_store.hpp"

using namespace haf;
using namespace haf::examples;

int main(int argc, char** argv) {
    std::filesystem::path store = std::filesystem::temp_directory_path() / "haf-example-recovery.store";
    if (argc > 1) {
        store = argv[1];
    }
    std::error_code error;
    std::filesystem::remove(store, error);

    AcceleratorId accelerator;
    FederationId federation_id;
    {
        FederationConfig config;
        config.name = "example-recovery";
        config.id_seed = 7;
        config.store_path = store;
        Result<std::unique_ptr<Federation>> federation = Federation::open(config);
        if (!federation.ok()) {
            line("cannot open federation: " + federation.status().describe());
            return 1;
        }
        federation_id = (*federation)->id();
        const AcceleratorDescriptor device = haf::examples::device(adapters::cuda_class_profile(), 71, "profile", 0);
        accelerator = device.id;
        const Result<MemberRecord> joined = join(**federation, device);
        if (!joined.ok()) {
            line("cannot admit device: " + joined.status().describe());
            return 1;
        }
        const WorkloadProfile workload =
            make_profile("recovery-demo", {require("numeric.fp32", RequirementStrength::Hard)});
        if (!(*federation)->register_workload(workload, (*federation)->epoch_claim("example")).ok()) {
            line("cannot register workload");
            return 1;
        }
        const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
        if (!decision.ok()) {
            line("evaluate failed: " + decision.status().describe());
            return 1;
        }
        line("epoch=" + std::to_string((*federation)->epoch().value()) +
             " generation=" + std::to_string((*federation)->generation().value()) +
             " state=" + std::string(to_string(joined->state)) + " decisions=" +
             std::to_string((*federation)->decisions().size()));
    }
    line("-- process-lifetime state released; reopening the same store --");

    FederationConfig reopened;
    reopened.name = "example-recovery";
    reopened.id_seed = 7;
    reopened.store_path = store;
    Result<std::unique_ptr<Federation>> recovered = Federation::open(reopened);
    if (!recovered.ok()) {
        line("cannot recover federation: " + recovered.status().describe());
        return 1;
    }
    const RecoveryReport& report = (*recovered)->recovery_report();
    line("recovered=" + std::string(report.recovered ? "yes" : "no") +
         " epoch " + std::to_string(report.previous_epoch.value()) + " -> " +
         std::to_string(report.new_epoch.value()) + " members_restored=" +
         std::to_string(report.members_restored) + " requiring_revalidation=" +
         std::to_string(report.members_requiring_revalidation) + " decisions_invalidated=" +
         std::to_string(report.decisions_invalidated));
    line("federation identity preserved: " + std::string((*recovered)->id() == federation_id ? "yes" : "no"));

    const Result<MemberRecord> member = (*recovered)->member(accelerator);
    if (!member.ok()) {
        line("member lookup failed: " + member.status().describe());
        return 1;
    }
    line("durable membership restored as " + std::string(to_string(member->state)) +
         ", accepts new work: " + (member->accepts_new_work() ? "yes" : "no"));

    const Result<AcceleratorDescriptor> descriptor = (*recovered)->descriptor(accelerator);
    if (!descriptor.ok()) {
        line("descriptor lookup failed: " + descriptor.status().describe());
        return 1;
    }
    for (const EvidenceRecord& evidence : descriptor->evidence) {
        line("evidence " + evidence.adapter + " requires_revalidation=" +
             (evidence.requires_revalidation ? "yes" : "no"));
    }

    const AuditReport report_audit = (*recovered)->audit();
    line(report_audit.render());

    std::filesystem::remove(store, error);
    return report_audit.ok() ? 0 : 1;
}
