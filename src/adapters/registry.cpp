#include "haf/adapters/registry.hpp"

#include <utility>

#include "haf/adapters/rocm_synthetic.hpp"

namespace haf::adapters {

std::vector<AdapterAvailability> synthetic_adapter_availability() {
    std::vector<AdapterAvailability> availability;
    availability.push_back(AdapterAvailability{"synthetic-cuda-like", SupportLevel::Synthetic,
                                               EvidenceProvenance::Synthetic, true,
                                               "deterministic CUDA-class model; used only when no real CUDA device is present"});
    availability.push_back(AdapterAvailability{"synthetic-rocm-like", SupportLevel::Synthetic,
                                               EvidenceProvenance::Synthetic, true, rocm_support_statement()});
    availability.push_back(AdapterAvailability{"synthetic-level-zero-like", SupportLevel::Synthetic,
                                               EvidenceProvenance::Synthetic, true,
                                               "deterministic Intel/Level-Zero-class model; no Intel accelerator present"});
    return availability;
}

std::unique_ptr<AcceleratorAdapter> make_cuda_class_synthetic_adapter() {
    return std::make_unique<SyntheticAdapter>("synthetic-cuda-like", cuda_class_profile());
}

std::vector<std::unique_ptr<AcceleratorAdapter>> make_synthetic_adapters(bool include_cuda_class_synthetic) {
    std::vector<std::unique_ptr<AcceleratorAdapter>> adapters;
    if (include_cuda_class_synthetic) {
        adapters.push_back(make_cuda_class_synthetic_adapter());
    }
    adapters.push_back(make_rocm_synthetic_adapter());
    adapters.push_back(make_intel_synthetic_adapter());
    return adapters;
}

}  // namespace haf::adapters
