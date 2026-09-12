// Heterogeneous Accelerator Federation - real CUDA adapter.
//
// Present only when the build found a CUDA toolkit. Everything it reports is
// REAL: it is read from the CUDA runtime on genuine hardware. Fields CUDA
// cannot prove are advertised as UNSUPPORTED rather than guessed, so the
// federation never derives compatibility from an assumption.
//
// CUDA does not provide live accelerator-state migration or device
// checkpoint/restore. Those capabilities are advertised UNSUPPORTED, which is
// positive evidence of absence rather than missing evidence.

#ifndef HAF_ADAPTERS_CUDA_HPP
#define HAF_ADAPTERS_CUDA_HPP

#include <memory>
#include <string>
#include <vector>

#include "haf/adapters/adapter.hpp"

namespace haf::adapters {

/// Construct the real CUDA adapter. Returns nullptr when no CUDA runtime is
/// available at run time, or when the build did not include the CUDA adapter.
[[nodiscard]] std::unique_ptr<AcceleratorAdapter> make_cuda_adapter();

/// True when this build includes the CUDA adapter at all.
[[nodiscard]] bool cuda_adapter_compiled();

/// Human-readable summary of what was physically observed on this host.
struct CudaInventory {
    bool available{false};
    int device_count{0};
    std::string runtime_version;
    std::string driver_version;
    std::vector<std::string> device_names;
    std::string detail;
};

[[nodiscard]] CudaInventory cuda_inventory();

/// End-to-end execution proof on one device: allocate, transfer, run a real
/// kernel, copy back, compare with the CPU reference, and release.
struct CudaProof {
    bool executed{false};
    bool verified{false};
    std::string device_name;
    std::uint64_t bytes_transferred{0};
    std::uint64_t free_bytes_before{0};
    std::uint64_t free_bytes_after{0};
    std::string detail;
};

[[nodiscard]] Result<CudaProof> run_cuda_proof(int device_index);

}  // namespace haf::adapters

#endif  // HAF_ADAPTERS_CUDA_HPP
