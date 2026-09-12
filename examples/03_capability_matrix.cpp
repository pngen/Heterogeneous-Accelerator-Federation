// Deterministic heterogeneous capability and compatibility matrix.

#include "example_support.hpp"

#include <vector>

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-matrix";
    config.id_seed = 3;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }

    const std::vector<adapters::DeviceProfile> profiles = {adapters::cuda_class_profile(),
                                                           adapters::rocm_class_profile(),
                                                           adapters::intel_class_profile()};
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const AcceleratorDescriptor entry =
            device(profiles[index], 30 + index, "synthetic-profile-" + std::to_string(index), 0);
        if (!join(**federation, entry).ok()) {
            line("cannot admit device " + std::to_string(index));
            return 1;
        }
    }

    std::vector<WorkloadProfile> workloads;
    workloads.push_back(make_profile("nvidia-fp8", {require_vendor("nvidia"),
                                                    require("numeric.fp8_e4m3", RequirementStrength::Hard),
                                                    require_isa({"sm-120"})}));
    workloads.push_back(make_profile("portable-fp32",
                                     {require("numeric.fp32", RequirementStrength::Hard),
                                      require("portability.recompile_available", RequirementStrength::Soft)}));
    workloads.push_back(make_profile("amd-instinct", {require_vendor("amd"),
                                                      require("numeric.fp64", RequirementStrength::Hard),
                                                      require_minimum("memory.total_bytes", 1LL << 30)}));

    for (const WorkloadProfile& workload : workloads) {
        if (!(*federation)->register_workload(workload, (*federation)->epoch_claim("example")).ok()) {
            line("cannot register workload " + workload.name);
            return 1;
        }
    }

    line("workload | accelerator | outcome | portability | evidence");
    for (const WorkloadProfile& workload : workloads) {
        const Result<FleetEvaluation> evaluation = (*federation)->evaluate_fleet(workload.class_id);
        if (!evaluation.ok()) {
            line("fleet evaluation failed: " + evaluation.status().describe());
            return 1;
        }
        const std::vector<AcceleratorId> accelerators = (*federation)->member_ids();
        for (std::size_t index = 0; index < evaluation->cells.size(); ++index) {
            const CompatibilityDecision& decision = evaluation->cells[index].decision;
            const Result<AcceleratorDescriptor> descriptor = (*federation)->descriptor(decision.accelerator);
            const std::string vendor = descriptor.ok() ? descriptor->vendor_token : std::string("?");
            line(workload.name + " | " + vendor + " | " + std::string(to_string(decision.outcome)) + " | " +
                 std::string(to_string(decision.portability)) + " | " + std::string(to_string(decision.provenance)));
            for (const CompatibilityReason& reason : decision.reasons) {
                line("    blocked by " + std::string(to_string(reason.code)) + " " + reason.subject);
            }
        }
        static_cast<void>(accelerators);
    }

    const AuditReport report = (*federation)->audit();
    line(report.render());
    return report.ok() ? 0 : 1;
}
