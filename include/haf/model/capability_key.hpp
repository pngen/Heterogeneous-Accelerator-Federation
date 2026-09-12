// Heterogeneous Accelerator Federation - capability key registry.
//
// Capabilities are identified by canonical dotted names, never by unstructured
// free text. The registry below is the closed vocabulary of the federation
// core. Adapters may add vendor-owned keys inside an extension namespace of
// the form "x.<namespace>.<name>", but an extension key can never satisfy a
// core requirement and an unknown non-extension key is always rejected.
//
// Wire encoding carries the canonical name so that a peer that does not know a
// key reports UnknownCapability instead of silently misinterpreting it.

#ifndef HAF_MODEL_CAPABILITY_KEY_HPP
#define HAF_MODEL_CAPABILITY_KEY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/status.hpp"

namespace haf {

/// Value domain of a capability key.
enum class CapabilityKind : std::uint8_t {
    Presence = 0,     ///< Supported / Unsupported / Unknown with no payload.
    Version = 1,      ///< Semantic version payload.
    Integer = 2,      ///< Exact integral quantity (bytes, counts, bits).
    Scalar = 3,       ///< Finite floating-point quantity.
    Enumeration = 4,  ///< Exactly one token from a canonical set.
    TokenSet = 5,     ///< Zero or more canonical tokens, sorted and deduplicated.
};

[[nodiscard]] std::string_view to_string(CapabilityKind kind) noexcept;
[[nodiscard]] bool capability_kind_from_wire(std::uint8_t raw, CapabilityKind& out) noexcept;

/// Stable handle for a capability key. Handles are process-local; the canonical
/// name is the portable representation and is always what gets hashed.
class CapabilityKey {
public:
    CapabilityKey() = default;

    [[nodiscard]] static CapabilityKey from_index(std::uint32_t index) noexcept { return CapabilityKey(index); }

    [[nodiscard]] bool valid() const noexcept { return index_ != kInvalidIndex; }
    [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

    /// Canonical dotted name, e.g. "memory.total_bytes".
    [[nodiscard]] const std::string& name() const;
    [[nodiscard]] CapabilityKind kind() const noexcept;

    /// Namespace is the portion of the name before the first dot.
    [[nodiscard]] std::string_view name_space() const noexcept;

    /// True when the key lives in an adapter-owned extension namespace.
    [[nodiscard]] bool is_extension() const noexcept;

    friend bool operator==(const CapabilityKey& a, const CapabilityKey& b) noexcept { return a.index_ == b.index_; }
    friend bool operator!=(const CapabilityKey& a, const CapabilityKey& b) noexcept { return a.index_ != b.index_; }
    friend bool operator<(const CapabilityKey& a, const CapabilityKey& b) noexcept {
        return a.name() < b.name();
    }

    static constexpr std::uint32_t kInvalidIndex = 0xFFFF'FFFFU;

private:
    explicit CapabilityKey(std::uint32_t index) noexcept : index_(index) {}

    std::uint32_t index_{kInvalidIndex};
};

/// Resolve a canonical name. Core keys resolve always. Extension keys of the
/// form "x.<namespace>.<name>" are interned on first use. Anything else yields
/// UnknownCapability, so an unknown mandatory capability can never degrade
/// silently into compatibility.
[[nodiscard]] Result<CapabilityKey> capability_key_from_name(std::string_view name);

/// Look up a core key without interning an extension.
[[nodiscard]] std::optional<CapabilityKey> find_core_capability_key(std::string_view canonical_name) noexcept;

/// Every core key, in canonical name order. Deterministic.
[[nodiscard]] const std::vector<CapabilityKey>& all_core_capability_keys();

/// Canonical key names referenced by the federation engines. These constants
/// exist so that adapters, policy, tests, and documentation agree on spelling.
namespace cap {
inline constexpr std::string_view kVendorId = "vendor.id";
inline constexpr std::string_view kVendorProduct = "vendor.product";
inline constexpr std::string_view kArchitectureFamily = "architecture.family";
inline constexpr std::string_view kArchitectureDeviceGeneration = "architecture.device_generation";
inline constexpr std::string_view kRuntimeFamily = "runtime.family";
inline constexpr std::string_view kRuntimeVersion = "runtime.version";
inline constexpr std::string_view kDriverVersion = "driver.version";
inline constexpr std::string_view kApiFamily = "api.family";
inline constexpr std::string_view kIsaCodeObjectTargets = "isa.code_object_targets";
inline constexpr std::string_view kComputeCapability = "compute.capability";
inline constexpr std::string_view kExecutionFeatures = "execution.features";

inline constexpr std::string_view kNumericFormats = "numeric.formats";
inline constexpr std::string_view kNumericFp64 = "numeric.fp64";
inline constexpr std::string_view kNumericFp32 = "numeric.fp32";
inline constexpr std::string_view kNumericFp16 = "numeric.fp16";
inline constexpr std::string_view kNumericBf16 = "numeric.bf16";
inline constexpr std::string_view kNumericTf32 = "numeric.tf32";
inline constexpr std::string_view kNumericFp8E4m3 = "numeric.fp8_e4m3";
inline constexpr std::string_view kNumericFp8E5m2 = "numeric.fp8_e5m2";
inline constexpr std::string_view kNumericFp6E2m3 = "numeric.fp6_e2m3";
inline constexpr std::string_view kNumericFp4E2m1 = "numeric.fp4_e2m1";
inline constexpr std::string_view kNumericInt8 = "numeric.int8";
inline constexpr std::string_view kNumericDenormalsPreserved = "numeric.denormals_preserved";
inline constexpr std::string_view kNumericFmaContraction = "numeric.fma_contraction";

inline constexpr std::string_view kTensorMatrixEngine = "tensor.matrix_engine";
inline constexpr std::string_view kTensorWarpGroupMma = "tensor.warp_group_mma";
inline constexpr std::string_view kTensorBlockScaledMma = "tensor.block_scaled_mma";
inline constexpr std::string_view kTensorStructuredSparsity = "tensor.structured_sparsity";

inline constexpr std::string_view kMemoryTotalBytes = "memory.total_bytes";
inline constexpr std::string_view kMemoryFreeBytes = "memory.free_bytes";
inline constexpr std::string_view kMemoryClasses = "memory.classes";
inline constexpr std::string_view kMemoryUnifiedAddressing = "memory.unified_addressing";
inline constexpr std::string_view kMemoryManagedAllocation = "memory.managed_allocation";
inline constexpr std::string_view kMemoryVirtualManagement = "memory.virtual_management";
inline constexpr std::string_view kMemoryAddressBits = "memory.address_bits";
inline constexpr std::string_view kMemoryAllocationAlignmentBytes = "memory.allocation_alignment_bytes";
inline constexpr std::string_view kMemoryPageGranularityBytes = "memory.page_granularity_bytes";
inline constexpr std::string_view kMemoryEccEnabled = "memory.ecc_enabled";
inline constexpr std::string_view kMemoryHostPinnedTransfer = "memory.host_pinned_transfer";

inline constexpr std::string_view kPeerAccess = "peer.access";
inline constexpr std::string_view kPeerUnifiedAccess = "peer.unified_access";
inline constexpr std::string_view kInterconnectLinks = "interconnect.links";

inline constexpr std::string_view kSyncStreamSemantics = "sync.stream_semantics";
inline constexpr std::string_view kSyncEvents = "sync.events";
inline constexpr std::string_view kSyncGraphCapture = "sync.graph_capture";
inline constexpr std::string_view kQueueConcurrentStreams = "queue.concurrent_streams";

inline constexpr std::string_view kKernelMaxThreadsPerBlock = "kernel.max_threads_per_block";
inline constexpr std::string_view kKernelMaxSharedMemoryPerBlockBytes = "kernel.max_shared_memory_per_block_bytes";
inline constexpr std::string_view kKernelDynamicParallelism = "kernel.dynamic_parallelism";
inline constexpr std::string_view kKernelCooperativeLaunch = "kernel.cooperative_launch";

inline constexpr std::string_view kCommCollectives = "comm.collectives";
inline constexpr std::string_view kCommRdma = "comm.rdma";
inline constexpr std::string_view kCommDirectPeerMemory = "comm.direct_peer_memory";
inline constexpr std::string_view kCommHostStagingRequired = "comm.host_staging_required";

inline constexpr std::string_view kPartitionMode = "partition.mode";
inline constexpr std::string_view kPartitionObservedProfiles = "partition.observed_profiles";

inline constexpr std::string_view kHealthReadiness = "health.readiness";
inline constexpr std::string_view kHealthThermalState = "health.thermal_state";
inline constexpr std::string_view kHealthErrorState = "health.error_state";

inline constexpr std::string_view kMigrationLiveStateTransfer = "migration.live_state_transfer";
inline constexpr std::string_view kMigrationCheckpointRestore = "migration.checkpoint_restore";
inline constexpr std::string_view kMigrationStateExport = "migration.state_export";
inline constexpr std::string_view kMigrationStateReconstruction = "migration.state_reconstruction";
inline constexpr std::string_view kMigrationCrossVendorState = "migration.cross_vendor_state";

inline constexpr std::string_view kPortabilityRecompileAvailable = "portability.recompile_available";
inline constexpr std::string_view kPortabilityRepackageAvailable = "portability.repackage_available";

inline constexpr std::string_view kSupportLevel = "support.level";
inline constexpr std::string_view kEvidenceProvenance = "evidence.provenance";
}  // namespace cap

}  // namespace haf

#endif  // HAF_MODEL_CAPABILITY_KEY_HPP
