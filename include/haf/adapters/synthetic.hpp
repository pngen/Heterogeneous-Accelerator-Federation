// Heterogeneous Accelerator Federation - deterministic synthetic profiles.
//
// Synthetic profiles exist so that heterogeneous compatibility, portability,
// and migration logic can be exercised without the corresponding hardware.
// They are always labelled SYNTHETIC end to end: the adapter provenance, the
// support level, the evidence record, the decision, and the CLI output all say
// so. Synthetic evidence never becomes REAL.

#ifndef HAF_ADAPTERS_SYNTHETIC_HPP
#define HAF_ADAPTERS_SYNTHETIC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/adapters/adapter.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/capability.hpp"

namespace haf::adapters {

/// Complete description of a modelled accelerator. Differences between
/// profiles are meaningful: they are what makes heterogeneity testable.
struct DeviceProfile {
    std::string vendor_token;
    std::string product_token;
    std::string architecture_token;
    std::string device_generation_token;
    std::string runtime_family;
    SemanticVersion runtime_version{12, 9, 0};
    SemanticVersion driver_version{600, 0, 0};

    std::int64_t memory_total_bytes{16LL * 1024 * 1024 * 1024};
    std::int64_t memory_free_bytes{12LL * 1024 * 1024 * 1024};

    std::vector<std::string> isa_targets;
    std::vector<std::string> numeric_formats;
    std::vector<std::string> execution_features;
    std::vector<std::string> api_families;
    std::vector<std::string> memory_classes;
    std::vector<std::string> peer_access;
    std::vector<std::string> interconnect_links;
    std::vector<std::string> collectives;
    std::vector<std::string> topology_references;

    bool tensor_matrix_engine{false};
    bool tensor_warp_group_mma{false};
    bool tensor_block_scaled_mma{false};
    bool tensor_structured_sparsity{false};

    bool memory_unified_addressing{false};
    bool memory_managed_allocation{false};
    bool memory_virtual_management{false};
    bool memory_host_pinned_transfer{true};
    bool memory_ecc_enabled{true};

    bool comm_rdma{false};
    bool comm_direct_peer_memory{false};
    bool comm_host_staging_required{false};

    bool kernel_dynamic_parallelism{false};
    bool kernel_cooperative_launch{false};
    bool sync_graph_capture{false};
    std::string stream_semantics{"in-order-queue"};

    bool migration_checkpoint_restore{false};
    bool migration_live_state_transfer{false};
    bool migration_state_export{false};
    bool migration_state_reconstruction{true};
    bool migration_cross_vendor_state{false};

    bool portability_recompile_available{true};
    bool portability_repackage_available{true};

    std::int64_t address_bits{64};
    std::int64_t allocation_alignment_bytes{256};
    std::int64_t page_granularity_bytes{65536};
    std::int64_t max_threads_per_block{1024};
    std::int64_t max_shared_memory_per_block_bytes{49152};
    std::int64_t concurrent_streams{32};

    /// Extra adapter-owned capability keys of the form "x.<ns>.<name>" mapped to
    /// canonical token payloads. Exercises the extension namespace path.
    std::vector<std::pair<std::string, std::string>> extensions;

    std::int64_t device_index{0};
};

/// Modelled CUDA-class device (used only when no real CUDA device is present).
[[nodiscard]] DeviceProfile cuda_class_profile();
/// Modelled ROCm/AMD-class device. Differ from CUDA in vendor, architecture,
/// runtime, ISA targets, numeric support, and portability requirements.
[[nodiscard]] DeviceProfile rocm_class_profile();
/// Modelled Intel-class device with a different runtime and feature surface.
[[nodiscard]] DeviceProfile intel_class_profile();
/// Modelled accelerator with deliberately incomplete evidence: several
/// capability namespaces are not closed, so hard requirements against them are
/// UNKNOWN and fail closed.
[[nodiscard]] DeviceProfile partially_evidenced_profile();

/// Build a capability set from a profile. Every namespace the profile speaks
/// about authoritatively is declared closed.
[[nodiscard]] Result<CapabilitySet> build_capability_set(const DeviceProfile& profile);

/// Build a descriptor for one modelled device.
[[nodiscard]] Result<AcceleratorDescriptor> build_descriptor(const DeviceProfile& profile,
                                                             const AdapterContext& context,
                                                             const std::string& adapter_name,
                                                             std::int64_t device_index);

/// Adapter that reports one modelled device per configured profile.
class SyntheticAdapter final : public AcceleratorAdapter {
public:
    SyntheticAdapter(std::string adapter_name, DeviceProfile profile);

    [[nodiscard]] std::string name() const override { return adapter_name_; }
    [[nodiscard]] SupportLevel support_level() const override { return SupportLevel::Synthetic; }
    [[nodiscard]] EvidenceProvenance provenance() const override { return EvidenceProvenance::Synthetic; }
    [[nodiscard]] Result<AdapterObservation> observe(const AdapterContext& context) const override;

private:
    std::string adapter_name_;
    DeviceProfile profile_;
};

}  // namespace haf::adapters

#endif  // HAF_ADAPTERS_SYNTHETIC_HPP
