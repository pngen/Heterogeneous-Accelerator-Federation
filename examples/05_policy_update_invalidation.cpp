// Policy is explicit, versioned data; changing it invalidates derived decisions.

#include "example_support.hpp"

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-policy";
    config.id_seed = 5;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }
    const AcceleratorDescriptor cuda = device(adapters::cuda_class_profile(), 51, "profile", 0);
    if (!join(**federation, cuda).ok()) {
        line("cannot admit device");
        return 1;
    }
    const WorkloadProfile workload = make_profile("policy-demo", {require("numeric.fp32", RequirementStrength::Hard)});
    if (!(*federation)->register_workload(workload, (*federation)->epoch_claim("example")).ok()) {
        line("cannot register workload");
        return 1;
    }

    const Result<CompatibilityDecision> before = (*federation)->evaluate(workload.class_id, cuda.id);
    if (!before.ok()) {
        line("evaluate failed: " + before.status().describe());
        return 1;
    }
    line("policy generation " + std::to_string((*federation)->policy_generation().value()));
    line("outcome=" + std::string(to_string(before->outcome)) + " fingerprint=" + before->fingerprint_hex());
    line("decision is current: " +
         std::string((*federation)->decision_is_current(before->decision_id).value_or(false) ? "yes" : "no"));

    FederationPolicy restricted = FederationPolicy::permissive_default();
    restricted.name = "no-degraded-no-fp64";
    restricted.denied_capabilities = {"numeric.fp64"};
    restricted.require_real_evidence_for_active = false;
    restricted.minimum_memory_bytes = 1ULL << 40;
    restricted.refresh_identity();
    const Result<PolicyGeneration> applied =
        (*federation)->set_policy(restricted, (*federation)->epoch_claim("example policy"));
    if (!applied.ok()) {
        line("policy update failed: " + applied.status().describe());
        return 1;
    }
    line("");
    line("policy generation advanced to " + std::to_string(applied->value()));
    line("recorded decisions after the change: " + std::to_string((*federation)->decisions().size()));

    const Result<CompatibilityDecision> after = (*federation)->evaluate(workload.class_id, cuda.id);
    if (!after.ok()) {
        line("evaluate failed: " + after.status().describe());
        return 1;
    }
    line("outcome=" + std::string(to_string(after->outcome)) + " fingerprint=" + after->fingerprint_hex());
    line(after->render());

    // A policy that denies a capability the device genuinely supports is
    // enforced from positive evidence, not from missing evidence.
    FederationPolicy contradictory = FederationPolicy::permissive_default();
    contradictory.required_capabilities = {"numeric.fp64"};
    contradictory.denied_capabilities = {"numeric.fp64"};
    line("");
    line("contradictory policy: " + contradictory.validate().describe());
    return 0;
}
