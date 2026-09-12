// Heterogeneous Accelerator Federation - ROCm-class synthetic adapter.
//
// This adapter exists because no AMD accelerator is present on the validation
// host. It models a ROCm-class device so that heterogeneous federation logic
// (cross-vendor ineligibility, ISA incompatibility, portability downgrade) can
// be exercised for real, while every artifact it produces is labelled
// SYNTHETIC from the adapter all the way to the CLI rendering.
//
// It never claims real ROCm execution. Where ROCm hardware is present, a real
// adapter would replace it without touching the federation core.

#ifndef HAF_ADAPTERS_ROCM_SYNTHETIC_HPP
#define HAF_ADAPTERS_ROCM_SYNTHETIC_HPP

#include <memory>

#include "haf/adapters/adapter.hpp"
#include "haf/adapters/synthetic.hpp"

namespace haf::adapters {

/// True when this build can only model the ROCm path.
[[nodiscard]] bool rocm_is_synthetic();

/// Human-readable statement of what is and is not proven for ROCm.
[[nodiscard]] const char* rocm_support_statement();

/// Build the ROCm-class synthetic adapter.
[[nodiscard]] std::unique_ptr<AcceleratorAdapter> make_rocm_synthetic_adapter();

/// Build the Intel-class synthetic adapter.
[[nodiscard]] std::unique_ptr<AcceleratorAdapter> make_intel_synthetic_adapter();

/// Build a synthetic adapter for an arbitrary profile.
[[nodiscard]] std::unique_ptr<AcceleratorAdapter> make_synthetic_adapter(std::string name, DeviceProfile profile);

}  // namespace haf::adapters

#endif  // HAF_ADAPTERS_ROCM_SYNTHETIC_HPP
