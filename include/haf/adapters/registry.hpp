// Heterogeneous Accelerator Federation - adapter registry.
//
// The registry describes what this build can actually observe. It is explicit
// about which paths are REAL, which are SYNTHETIC, and which are UNSUPPORTED,
// because that distinction is part of the product's honesty contract.

#ifndef HAF_ADAPTERS_REGISTRY_HPP
#define HAF_ADAPTERS_REGISTRY_HPP

#include <memory>
#include <string>
#include <vector>

#include "haf/adapters/adapter.hpp"
#include "haf/model/evidence.hpp"

namespace haf::adapters {

struct AdapterAvailability {
    std::string name;
    SupportLevel support_level{SupportLevel::Unsupported};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    bool available{false};
    std::string detail;
};

/// Adapters that are guaranteed to be constructible: always the synthetic
/// profiles, which do not require any vendor runtime.
[[nodiscard]] std::vector<AdapterAvailability> synthetic_adapter_availability();

/// Build every adapter that does not require a vendor runtime.
[[nodiscard]] std::vector<std::unique_ptr<AcceleratorAdapter>> make_synthetic_adapters(bool include_cuda_class_synthetic);

/// Build the CUDA-class synthetic adapter explicitly, for hosts without CUDA.
[[nodiscard]] std::unique_ptr<AcceleratorAdapter> make_cuda_class_synthetic_adapter();

}  // namespace haf::adapters

#endif  // HAF_ADAPTERS_REGISTRY_HPP
