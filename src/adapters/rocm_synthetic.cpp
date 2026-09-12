#include "haf/adapters/rocm_synthetic.hpp"

#include <utility>

namespace haf::adapters {

bool rocm_is_synthetic() { return true; }

const char* rocm_support_statement() {
    return "ROCm path: SYNTHETIC. No AMD accelerator or ROCm runtime is present on the validation host. "
           "Heterogeneous compatibility, ISA incompatibility, and portability downgrade logic are exercised "
           "against a deterministic model. Real ROCm execution is UNSUPPORTED in this environment.";
}

std::unique_ptr<AcceleratorAdapter> make_rocm_synthetic_adapter() {
    return std::make_unique<SyntheticAdapter>("synthetic-rocm-like", rocm_class_profile());
}

std::unique_ptr<AcceleratorAdapter> make_intel_synthetic_adapter() {
    return std::make_unique<SyntheticAdapter>("synthetic-level-zero-like", intel_class_profile());
}

std::unique_ptr<AcceleratorAdapter> make_synthetic_adapter(std::string name, DeviceProfile profile) {
    return std::make_unique<SyntheticAdapter>(std::move(name), std::move(profile));
}

}  // namespace haf::adapters
