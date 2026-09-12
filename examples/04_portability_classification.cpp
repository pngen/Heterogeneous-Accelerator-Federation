// What must happen before work valid on one accelerator is valid on another.

#include "example_support.hpp"

using namespace haf;
using namespace haf::examples;

namespace {

void report(const AcceleratorDescriptor& source, const AcceleratorDescriptor& destination, bool allow_cross_vendor) {
    PortabilityRequest request;
    request.source_capabilities = &source.capabilities;
    request.destination_capabilities = &destination.capabilities;
    request.source_vendor = source.vendor_token;
    request.destination_vendor = destination.vendor_token;
    request.policy_allows_cross_vendor_migration = allow_cross_vendor;
    const PortabilityResult result = classify_portability(request);
    line(source.vendor_token + "/" + source.device_generation_token + " -> " + destination.vendor_token + "/" +
         destination.device_generation_token + " = " + std::string(to_string(result.value)));
    line("  transformation steps: " + std::to_string(portability_transformation_steps(result.value)));
    line("  requires binary transformation: " +
         std::string(requires_binary_transformation(result.value) ? "yes" : "no"));
    line("  requires state reconstruction: " +
         std::string(requires_state_reconstruction(result.value) ? "yes" : "no"));
    line("  implies live state transfer: " +
         std::string(implies_live_state_transfer(result.value) ? "yes" : "no"));
    for (const CompatibilityReason& reason : result.reasons) {
        line("  reason: " + std::string(to_string(reason.code)) + " " + reason.detail);
    }
    line("");
}

}  // namespace

int main() {
    const AcceleratorDescriptor cuda_a = device(adapters::cuda_class_profile(), 41, "profile-a", 0);
    const AcceleratorDescriptor cuda_b = device(adapters::cuda_class_profile(), 41, "profile-a", 1);
    const AcceleratorDescriptor rocm = device(adapters::rocm_class_profile(), 42, "profile-b", 0);
    const AcceleratorDescriptor intel = device(adapters::intel_class_profile(), 43, "profile-c", 0);

    line("Portability classes distinguish what the federation actually knows.");
    line("");
    report(cuda_a, cuda_b, false);
    report(cuda_a, rocm, false);
    report(cuda_a, intel, false);
    report(rocm, cuda_a, false);

    line("Cross-vendor live migration is refused even when a policy would permit it,");
    line("because neither accelerator positively supports it.");
    report(cuda_a, rocm, true);

    // A device with missing evidence yields UNKNOWN rather than a weak claim.
    AcceleratorDescriptor partial = device(adapters::partially_evidenced_profile(), 44, "profile-partial", 0);
    std::vector<CapabilityRecord> records = partial.capabilities.records();
    const Status status = partial.capabilities.set_closed_namespaces({"vendor"});
    if (!status.ok()) {
        line("cannot adjust namespaces: " + status.describe());
        return 1;
    }
    static_cast<void>(records);
    line("A device that does not speak for the ISA namespace:");
    report(cuda_a, partial, false);
    return 0;
}
