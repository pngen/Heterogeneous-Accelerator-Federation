// Workload compatibility: hard requirements, UNKNOWN behaviour, and explanation.

#include "example_support.hpp"

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-compatibility";
    config.id_seed = 2;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }

    const AcceleratorDescriptor cuda = device(adapters::cuda_class_profile(), 21, "synthetic-cuda-like", 0);
    const AcceleratorDescriptor rocm = device(adapters::rocm_class_profile(), 22, "synthetic-rocm-like", 0);
    if (!join(**federation, cuda).ok() || !join(**federation, rocm).ok()) {
        line("cannot admit example devices");
        return 1;
    }

    WorkloadProfile workload;
    workload.name = "fp8-decode";
    workload.description = "needs CUDA, fp8 and an sm-120 code object";
    workload.class_id = workload_class_id_from_token(workload.name);
    workload.requirements = {require_equals("vendor.id", "nvidia", RequirementStrength::Hard).value(),
                             require_present("numeric.fp8_e4m3", RequirementStrength::Hard).value(),
                             require_tokens_superset("isa.code_object_targets", {"sm-120"},
                                                     RequirementStrength::Hard)
                                 .value(),
                             require_present("tensor.block_scaled_mma", RequirementStrength::Soft).value()};
    workload.requirements[3].weight = 0.5;
    workload.requirement_id = derive_requirement_id(workload);
    workload.revision = derive_workload_revision(workload);
    const Result<WorkloadRevision> registered =
        (*federation)->register_workload(workload, (*federation)->epoch_claim("example"));
    if (!registered.ok()) {
        line("cannot register workload: " + registered.status().describe());
        return 1;
    }
    line("workload revision=" + std::to_string(registered->value()));

    for (const AcceleratorDescriptor* entry : {&cuda, &rocm}) {
        const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, entry->id);
        if (!decision.ok()) {
            line("evaluate failed: " + decision.status().describe());
            return 1;
        }
        line("");
        line(decision->render());
    }

    // A requirement the evidence does not cover at all is UNKNOWN, not eligible.
    WorkloadProfile exotic = workload;
    exotic.name = "exotic";
    exotic.class_id = workload_class_id_from_token(exotic.name);
    exotic.requirements = {require_present("x.unmodelled.feature", RequirementStrength::Hard).value()};
    exotic.requirement_id = derive_requirement_id(exotic);
    exotic.revision = derive_workload_revision(exotic);
    if (!(*federation)->register_workload(exotic, (*federation)->epoch_claim("example")).ok()) {
        line("cannot register exotic workload");
        return 1;
    }
    const Result<CompatibilityDecision> unknown = (*federation)->evaluate(exotic.class_id, cuda.id);
    if (!unknown.ok()) {
        line("evaluate failed: " + unknown.status().describe());
        return 1;
    }
    line("");
    line("unmodelled requirement -> " + std::string(to_string(unknown->outcome)));
    line(unknown->render());
    return 0;
}
