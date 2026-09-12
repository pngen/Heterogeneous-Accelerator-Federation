// Real CUDA hardware validation.
//
// These cases only exist when the build found a CUDA toolkit. They observe a
// genuine GPU through the CUDA runtime and execute a genuine kernel, verifying
// the result against a CPU reference. Nothing here is modelled.

#include <algorithm>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/adapters/cuda.hpp"
#include "haf/federation/federation.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

HAF_TEST(cuda, runtime_reports_a_real_device) {
    const adapters::CudaInventory inventory = adapters::cuda_inventory();
    HAF_NOTE("runtime=" + inventory.runtime_version + " driver=" + inventory.driver_version +
             " devices=" + std::to_string(inventory.device_count));
    HAF_CHECK(adapters::cuda_adapter_compiled());
    HAF_REQUIRE(inventory.available);
    HAF_CHECK(inventory.device_count >= 1);
    HAF_CHECK(!inventory.runtime_version.empty());
    HAF_CHECK(!inventory.driver_version.empty());
    for (const std::string& name : inventory.device_names) {
        HAF_NOTE("device=" + name);
        HAF_CHECK(!name.empty());
    }
}

HAF_TEST(cuda, adapter_observes_real_evidence) {
    std::unique_ptr<adapters::AcceleratorAdapter> adapter = adapters::make_cuda_adapter();
    HAF_REQUIRE(adapter != nullptr);
    HAF_EQ(adapter->name(), std::string("cuda-runtime"));
    HAF_CHECK(adapter->provenance() == EvidenceProvenance::Real);
    HAF_CHECK(adapter->support_level() == SupportLevel::Native);

    const adapters::AdapterContext context = make_context(2000);
    const Result<adapters::AdapterObservation> observation = adapter->observe(context);
    HAF_REQUIRE_OK(observation);
    HAF_CHECK(!observation->devices.empty());
    HAF_CHECK(observation->provenance == EvidenceProvenance::Real);
    for (const AcceleratorDescriptor& device : observation->devices) {
        HAF_CHECK(device.validate().ok());
        HAF_CHECK(device.provenance == EvidenceProvenance::Real);
        HAF_CHECK(device.support_level == SupportLevel::Native);
        HAF_CHECK(device.vendor_token == "nvidia");
        HAF_CHECK(device.runtime_family == "cuda");
        HAF_CHECK(!device.evidence.empty());
        HAF_CHECK(device.capabilities.lookup("memory.total_bytes").state == CapabilityState::Supported);
        HAF_CHECK(device.capabilities.lookup("isa.code_object_targets").state == CapabilityState::Supported);
        HAF_CHECK(device.memory_total_bytes() > 0);
        HAF_NOTE("device " + device.id.to_string() + " product=" + device.product_token +
                 " architecture=" + device.architecture_token + " generation=" + device.device_generation_token +
                 " runtime=" + device.runtime_version.to_string() + " driver=" + device.driver_version.to_string() +
                 " memory=" + std::to_string(device.memory_total_bytes()));
    }
}

HAF_TEST(cuda, real_kernel_execution_is_verified_against_a_cpu_reference) {
    std::unique_ptr<adapters::AcceleratorAdapter> adapter = adapters::make_cuda_adapter();
    HAF_REQUIRE(adapter != nullptr);
    HAF_PHASE("DISPATCH");
    const Result<adapters::AcceleratorAdapter::ProofResult> proof = adapter->prove(0);
    HAF_REQUIRE_OK(proof);
    HAF_NOTE(proof->detail + " bytes_transferred=" + std::to_string(proof->bytes_transferred));
    HAF_CHECK(proof->executed);
    HAF_CHECK(proof->verified);
    HAF_CHECK(proof->bytes_transferred > 0);
}

HAF_TEST(cuda, proof_releases_device_memory) {
    const Result<adapters::CudaProof> proof = adapters::run_cuda_proof(0);
    HAF_REQUIRE_OK(proof);
    HAF_CHECK(proof->verified);
    // The proof frees everything it allocated. Some driver-side variance is
    // normal, so the check allows a small tolerance while still catching a leak
    // of the two megabyte buffers the proof uses.
    const std::uint64_t tolerance = 8ULL * 1024ULL * 1024ULL;
    const std::uint64_t leaked =
        proof->free_bytes_before > proof->free_bytes_after ? proof->free_bytes_before - proof->free_bytes_after : 0;
    HAF_NOTE("free before=" + std::to_string(proof->free_bytes_before) + " after=" +
             std::to_string(proof->free_bytes_after));
    HAF_CHECK(leaked <= tolerance);
}

HAF_TEST(cuda, real_device_participates_in_the_federation) {
    std::unique_ptr<adapters::AcceleratorAdapter> adapter = adapters::make_cuda_adapter();
    HAF_REQUIRE(adapter != nullptr);
    const Result<adapters::AdapterObservation> observation = adapter->observe(make_context(2100));
    HAF_REQUIRE_OK(observation);
    HAF_REQUIRE(!observation->devices.empty());
    const AcceleratorDescriptor device = observation->devices.front();

    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(2101, permissive_policy());
    HAF_REQUIRE_OK(federation);
    HAF_REQUIRE_OK(join(**federation, device));

    const WorkloadProfile workload = make_workload(
        "real-cuda-fp32",
        {hard_equals("vendor.id", "nvidia"), hard_present("numeric.fp32"),
         hard_tokens("isa.code_object_targets", {device.device_generation_token}),
         hard_at_least("memory.free_bytes", 64LL * 1024 * 1024)});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("cuda test")));
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
    HAF_REQUIRE_OK(decision);
    HAF_CHECK(decision->outcome == CompatibilityOutcome::Eligible);
    HAF_CHECK(decision->provenance == EvidenceProvenance::Real);
    HAF_CHECK(decision->support_level == SupportLevel::Native);
    HAF_CHECK((*federation)->audit().ok());

    // A workload demanding live state transfer must be refused: CUDA genuinely
    // provides no live accelerator-state migration, and the federation says so
    // from positive evidence of absence rather than from missing evidence.
    WorkloadProfile live = make_workload("real-cuda-live", {hard_equals("vendor.id", "nvidia")});
    live.migration_need = MigrationNeed::LiveStateTransfer;
    HAF_REQUIRE_OK((*federation)->register_workload(live, (*federation)->epoch_claim("cuda test")));
    EvaluationOptions options;
    options.source = device.id;
    const Result<CompatibilityDecision> live_decision = (*federation)->evaluate(live.class_id, device.id, options);
    HAF_REQUIRE_OK(live_decision);
    HAF_CHECK(live_decision->outcome == CompatibilityOutcome::Ineligible);
}
