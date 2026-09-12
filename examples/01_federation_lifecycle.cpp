// Federation discovery and admission through the public API.

#include "example_support.hpp"

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-federation";
    config.id_seed = 1;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }
    line("federation " + (*federation)->id().to_string() + " epoch=" +
         std::to_string((*federation)->epoch().value()));

    // An advertisement only becomes authoritative after the federation accepts
    // it, and only inside the authority the caller currently holds.
    const AcceleratorDescriptor cuda = device(adapters::cuda_class_profile(), 11, "synthetic-cuda-like", 0);
    const AcceleratorDescriptor rocm = device(adapters::rocm_class_profile(), 12, "synthetic-rocm-like", 0);

    const AuthorityClaim claim = (*federation)->epoch_claim("example advertisement");
    const Result<MemberRecord> observed = (*federation)->observe(cuda, claim);
    if (!observed.ok()) {
        line("observe failed: " + observed.status().describe());
        return 1;
    }
    line("after observe: state=" + std::string(to_string(observed->state)) +
         " capability_generation=" + std::to_string(observed->capability_generation.value()));

    const Result<MemberRecord> admitted = (*federation)->admit(cuda.id, claim);
    if (!admitted.ok()) {
        line("admit failed: " + admitted.status().describe());
        return 1;
    }
    line("after admit: state=" + std::string(to_string(admitted->state)));

    const Result<MemberRecord> active = (*federation)->activate(cuda.id, claim);
    if (!active.ok()) {
        line("activate failed: " + active.status().describe());
        return 1;
    }
    line("after activate: state=" + std::string(to_string(active->state)) +
         " accepts_new_work=" + (active->accepts_new_work() ? "yes" : "no"));

    const Result<MemberRecord> second = join(**federation, rocm);
    if (!second.ok()) {
        line("second member failed: " + second.status().describe());
        return 1;
    }
    line("members=" + std::to_string((*federation)->member_count()));

    // A stale claim is refused even though the object identity still exists.
    AuthorityClaim stale = claim;
    stale.epoch = CoordinatorEpoch(stale.epoch.value() + 7);
    const Result<MemberRecord> rejected = (*federation)->fence(cuda.id, stale);
    line("stale authority: " + rejected.status().describe());

    const AuditReport report = (*federation)->audit();
    line(report.render());
    return report.ok() ? 0 : 1;
}
