// Real CUDA member discovery when a CUDA toolkit and device are present.
//
// When the build has no CUDA adapter this example reports UNSUPPORTED and uses
// a clearly labelled synthetic CUDA-class profile instead, so that the example
// is still runnable without ever implying that real hardware was used.

#include "example_support.hpp"

#if defined(HAF_HAVE_CUDA_ADAPTER)
#include "haf/adapters/cuda.hpp"
#endif

using namespace haf;
using namespace haf::examples;

int main() {
    FederationConfig config;
    config.name = "example-cuda";
    config.id_seed = 8;
    config.persist = false;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    if (!federation.ok()) {
        line("cannot open federation: " + federation.status().describe());
        return 1;
    }

    std::vector<AcceleratorDescriptor> descriptors;
    bool real = false;
#if defined(HAF_HAVE_CUDA_ADAPTER)
    const adapters::CudaInventory inventory = adapters::cuda_inventory();
    line("CUDA inventory: available=" + std::string(inventory.available ? "yes" : "no") +
         " runtime=" + inventory.runtime_version + " driver=" + inventory.driver_version +
         " devices=" + std::to_string(inventory.device_count));
    if (!inventory.available) {
        line("detail: " + inventory.detail);
    }
    std::unique_ptr<adapters::AcceleratorAdapter> adapter = adapters::make_cuda_adapter();
    if (adapter != nullptr) {
        const Result<adapters::AdapterObservation> observation = adapter->observe(context(81, "cuda-node"));
        if (!observation.ok()) {
            line("CUDA observation failed: " + observation.status().describe());
            return 1;
        }
        descriptors = observation->devices;
        real = true;
        line(std::string("evidence provenance: ") + std::string(to_string(observation->provenance)));
    }
#else
    line("CUDA adapter: UNSUPPORTED in this build (no CUDA toolkit was found at configure time)");
#endif

    if (!real) {
        line("falling back to a SYNTHETIC CUDA-class profile; no physical GPU is involved");
        descriptors.push_back(device(adapters::cuda_class_profile(), 82, "synthetic-cuda-like", 0));
    }

    for (const AcceleratorDescriptor& descriptor : descriptors) {
        const Result<MemberRecord> joined = join(**federation, descriptor);
        if (!joined.ok()) {
            line("cannot admit device: " + joined.status().describe());
            return 1;
        }
        line("member " + descriptor.id.to_string() + " product=" + descriptor.product_token +
             " architecture=" + descriptor.architecture_token + " generation=" +
             descriptor.device_generation_token + " memory_bytes=" + std::to_string(descriptor.memory_total_bytes()) +
             " evidence=" + std::string(to_string(descriptor.provenance)));
    }

    if (real) {
#if defined(HAF_HAVE_CUDA_ADAPTER)
        std::unique_ptr<adapters::AcceleratorAdapter> proof_adapter = adapters::make_cuda_adapter();
        if (proof_adapter != nullptr) {
            const Result<adapters::AcceleratorAdapter::ProofResult> proof = proof_adapter->prove(0);
            if (!proof.ok()) {
                line("execution proof failed: " + proof.status().describe());
                return 1;
            }
            line("execution proof: executed=" + std::string(proof->executed ? "yes" : "no") +
                 " verified=" + std::string(proof->verified ? "yes" : "no") +
                 " bytes_transferred=" + std::to_string(proof->bytes_transferred));
            line("detail: " + proof->detail);
        }
#endif
    }

    const WorkloadProfile workload =
        make_profile("cuda-native", {require("numeric.fp32", RequirementStrength::Hard),
                                      require_vendor("nvidia"),
                                      require_minimum("memory.total_bytes", 1LL << 28)});
    if (!(*federation)->register_workload(workload, (*federation)->epoch_claim("example")).ok()) {
        line("cannot register workload");
        return 1;
    }
    for (const AcceleratorDescriptor& descriptor : descriptors) {
        const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, descriptor.id);
        if (!decision.ok()) {
            line("evaluate failed: " + decision.status().describe());
            return 1;
        }
        line(decision->render());
    }
    const AuditReport report = (*federation)->audit();
    line(report.render());
    return report.ok() ? 0 : 1;
}
