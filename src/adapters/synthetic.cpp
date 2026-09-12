#include "haf/adapters/synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf::adapters {
namespace {

using namespace cap;

[[nodiscard]] std::string device_token(const DeviceProfile& profile) {
    return profile.vendor_token + "-" + profile.product_token + "-" + profile.device_generation_token + "-" +
           std::to_string(profile.device_index);
}

void add_presence(std::vector<CapabilityRecord>& records, std::string_view key, bool supported) {
    records.push_back(make_presence(key, supported ? CapabilityState::Supported : CapabilityState::Unsupported));
}

/// A token set with no members is not a claim a device can truthfully make, so
/// an empty list is advertised as UNSUPPORTED (positive evidence of absence)
/// rather than SUPPORTED with nothing behind it.
[[nodiscard]] Status normalize_token_record(CapabilityRecord& record) {
    if (record.value.kind() == CapabilityKind::TokenSet && record.value.tokens().empty()) {
        record.state = CapabilityState::Unsupported;
    }
    return Status::success();
}

/// Namespaces this profile speaks about authoritatively. A capability in a
/// closed namespace that is not advertised at all is positively UNSUPPORTED;
/// anything outside is UNKNOWN.
[[nodiscard]] std::vector<std::string> closed_namespaces_for(const DeviceProfile& profile) {
    std::vector<std::string> namespaces = {"api",   "architecture", "comm",  "compute", "driver",
                                           "health", "interconnect", "isa",   "kernel",  "memory",
                                           "migration", "numeric", "partition", "peer", "portability",
                                           "queue", "runtime", "support", "sync", "tensor", "vendor"};
    // A profile with extension keys closes the "x" namespace as well.
    if (!profile.extensions.empty()) {
        namespaces.emplace_back("x");
    }
    return namespaces;
}

}  // namespace

DeviceProfile cuda_class_profile() {
    DeviceProfile profile;
    profile.vendor_token = std::string(vendors::kNvidia);
    profile.product_token = "geforce-rtx-5090";
    profile.architecture_token = "blackwell";
    profile.device_generation_token = "sm-120";
    profile.runtime_family = "cuda";
    profile.runtime_version = SemanticVersion(12, 9, 0);
    profile.driver_version = SemanticVersion(616, 92, 0);
    profile.memory_total_bytes = 32LL * 1024 * 1024 * 1024;
    profile.memory_free_bytes = 30LL * 1024 * 1024 * 1024;
    profile.isa_targets = {"compute-120", "sm-100", "sm-120"};
    profile.numeric_formats = {"bf16", "fp16", "fp32", "fp4_e2m1", "fp6_e2m3", "fp64", "fp8_e4m3", "fp8_e5m2",
                               "int8", "tf32"};
    profile.execution_features = {"cluster-launch", "cooperative-launch", "dynamic-parallelism",
                                  "graph-capture", "unified-memory"};
    profile.api_families = {"cuda-driver", "cuda-runtime", "nvml"};
    profile.memory_classes = {"device", "host-pinned", "managed", "peer"};
    profile.peer_access = {"nvlink", "pcie"};
    profile.interconnect_links = {"nvlink-5", "pcie-gen5"};
    profile.collectives = {"all-gather", "all-reduce", "broadcast"};
    profile.tensor_matrix_engine = true;
    profile.tensor_warp_group_mma = true;
    profile.tensor_block_scaled_mma = true;
    profile.tensor_structured_sparsity = true;
    profile.memory_unified_addressing = true;
    profile.memory_managed_allocation = true;
    profile.memory_virtual_management = true;
    profile.memory_host_pinned_transfer = true;
    profile.comm_rdma = false;
    profile.comm_direct_peer_memory = true;
    profile.kernel_dynamic_parallelism = true;
    profile.kernel_cooperative_launch = true;
    profile.sync_graph_capture = true;
    profile.stream_semantics = "in-order-queue";
    // CUDA does not provide live state transfer or device checkpoint/restore.
    profile.migration_checkpoint_restore = false;
    profile.migration_live_state_transfer = false;
    profile.migration_state_export = false;
    profile.migration_state_reconstruction = true;
    profile.migration_cross_vendor_state = false;
    profile.portability_recompile_available = true;
    profile.portability_repackage_available = true;
    profile.max_threads_per_block = 1024;
    profile.max_shared_memory_per_block_bytes = 232448;
    profile.concurrent_streams = 128;
    profile.extensions = {{"x.nvidia.cluster-dimensions", "8,4,2"}};
    return profile;
}

DeviceProfile rocm_class_profile() {
    DeviceProfile profile;
    profile.vendor_token = std::string(vendors::kAmd);
    profile.product_token = "instinct-mi300x";
    profile.architecture_token = "cdna3";
    profile.device_generation_token = "gfx-942";
    profile.runtime_family = "rocm";
    profile.runtime_version = SemanticVersion(6, 2, 0);
    profile.driver_version = SemanticVersion(6, 8, 0);
    profile.memory_total_bytes = 192LL * 1024 * 1024 * 1024;
    profile.memory_free_bytes = 180LL * 1024 * 1024 * 1024;
    profile.isa_targets = {"gfx-940", "gfx-942", "amdgcn-gfx942"};
    profile.numeric_formats = {"bf16", "fp16", "fp32", "fp64", "fp8_e4m3", "fp8_e5m2", "int8"};
    profile.execution_features = {"cooperative-launch", "graph-capture", "unified-memory"};
    profile.api_families = {"hip", "rocm-smi"};
    profile.memory_classes = {"device", "host-pinned", "managed", "peer"};
    profile.peer_access = {"infinity-fabric", "pcie"};
    profile.interconnect_links = {"infinity-fabric", "pcie-gen5"};
    profile.collectives = {"all-gather", "all-reduce", "broadcast", "reduce-scatter"};
    profile.tensor_matrix_engine = true;
    profile.tensor_warp_group_mma = false;
    profile.tensor_block_scaled_mma = false;
    profile.tensor_structured_sparsity = false;
    profile.memory_unified_addressing = true;
    profile.memory_managed_allocation = true;
    profile.memory_virtual_management = true;
    profile.memory_host_pinned_transfer = true;
    profile.comm_rdma = true;
    profile.comm_direct_peer_memory = true;
    profile.kernel_dynamic_parallelism = false;
    profile.kernel_cooperative_launch = true;
    profile.sync_graph_capture = true;
    profile.stream_semantics = "in-order-queue";
    profile.migration_checkpoint_restore = false;
    profile.migration_live_state_transfer = false;
    profile.migration_state_export = false;
    profile.migration_state_reconstruction = true;
    profile.migration_cross_vendor_state = false;
    profile.portability_recompile_available = true;
    profile.portability_repackage_available = true;
    profile.max_threads_per_block = 1024;
    profile.max_shared_memory_per_block_bytes = 65536;
    profile.concurrent_streams = 64;
    profile.extensions = {{"x.amd.wavefront-size", "64"}};
    return profile;
}

DeviceProfile intel_class_profile() {
    DeviceProfile profile;
    profile.vendor_token = std::string(vendors::kIntel);
    profile.product_token = "data-center-gpu-max-1550";
    profile.architecture_token = "xe-hpg";
    profile.device_generation_token = "dg2-512";
    profile.runtime_family = "level-zero";
    profile.runtime_version = SemanticVersion(1, 9, 0);
    profile.driver_version = SemanticVersion(1, 6, 0);
    profile.memory_total_bytes = 48LL * 1024 * 1024 * 1024;
    profile.memory_free_bytes = 40LL * 1024 * 1024 * 1024;
    profile.isa_targets = {"xe-hpg-512", "spirv-1.6"};
    profile.numeric_formats = {"bf16", "fp16", "fp32", "int8"};
    profile.execution_features = {"graph-capture", "unified-memory"};
    profile.api_families = {"level-zero", "opencl", "sycl"};
    profile.memory_classes = {"device", "host-pinned", "shared"};
    profile.peer_access = {"pcie", "xe-link"};
    profile.interconnect_links = {"xe-link", "pcie-gen5"};
    profile.collectives = {"all-reduce", "broadcast"};
    profile.tensor_matrix_engine = true;
    profile.memory_unified_addressing = true;
    profile.memory_host_pinned_transfer = true;
    profile.memory_virtual_management = true;
    profile.kernel_cooperative_launch = false;
    profile.stream_semantics = "out-of-order-queue";
    profile.portability_recompile_available = true;
    profile.portability_repackage_available = true;
    profile.max_threads_per_block = 1024;
    profile.max_shared_memory_per_block_bytes = 65536;
    profile.concurrent_streams = 16;
    profile.extensions = {{"x.intel.subslice-count", "32"}};
    return profile;
}

DeviceProfile partially_evidenced_profile() {
    DeviceProfile profile;
    profile.vendor_token = "synthetic";
    profile.product_token = "partial-evidence-device";
    profile.architecture_token = "opaque";
    profile.device_generation_token = "rev-1";
    profile.runtime_family = "opaque-runtime";
    profile.runtime_version = SemanticVersion(0, 9, 0);
    profile.driver_version = SemanticVersion(0, 1, 0);
    profile.memory_total_bytes = 8LL * 1024 * 1024 * 1024;
    profile.memory_free_bytes = 4LL * 1024 * 1024 * 1024;
    profile.isa_targets = {"opaque-v1"};
    profile.numeric_formats = {"fp32"};
    profile.api_families = {"opaque"};
    profile.memory_classes = {"device"};
    profile.peer_access = {};
    profile.interconnect_links = {"pcie-gen4"};
    profile.collectives = {};
    profile.tensor_matrix_engine = false;
    return profile;
}

Result<CapabilitySet> build_capability_set(const DeviceProfile& profile) {
    std::vector<CapabilityRecord> records;
    records.reserve(96);

    const Result<CapabilityKey> vendor_key = capability_key_from_name(kVendorId);
    if (!vendor_key.ok()) {
        return vendor_key.status();
    }
    CapabilityRecord vendor_record;
    vendor_record.key = *vendor_key;
    vendor_record.state = CapabilityState::Supported;
    vendor_record.value = CapabilityValue::enumeration(profile.vendor_token);
    records.push_back(std::move(vendor_record));

    const Result<CapabilityKey> product_key = capability_key_from_name(kVendorProduct);
    if (!product_key.ok()) {
        return product_key.status();
    }
    CapabilityRecord product_record;
    product_record.key = *product_key;
    product_record.state = CapabilityState::Supported;
    product_record.value = CapabilityValue::enumeration(profile.product_token);
    records.push_back(std::move(product_record));

    const Result<CapabilityKey> architecture_key = capability_key_from_name(kArchitectureFamily);
    if (!architecture_key.ok()) {
        return architecture_key.status();
    }
    CapabilityRecord architecture_record;
    architecture_record.key = *architecture_key;
    architecture_record.state = CapabilityState::Supported;
    architecture_record.value = CapabilityValue::enumeration(profile.architecture_token);
    records.push_back(std::move(architecture_record));

    const Result<CapabilityKey> generation_key = capability_key_from_name(kArchitectureDeviceGeneration);
    if (!generation_key.ok()) {
        return generation_key.status();
    }
    CapabilityRecord generation_record;
    generation_record.key = *generation_key;
    generation_record.state = CapabilityState::Supported;
    generation_record.value = CapabilityValue::integer(profile.device_index + 1);
    records.push_back(std::move(generation_record));

    const Result<CapabilityKey> runtime_key = capability_key_from_name(kRuntimeFamily);
    if (!runtime_key.ok()) {
        return runtime_key.status();
    }
    CapabilityRecord runtime_record;
    runtime_record.key = *runtime_key;
    runtime_record.state = CapabilityState::Supported;
    runtime_record.value = CapabilityValue::enumeration(profile.runtime_family);
    records.push_back(std::move(runtime_record));

    auto push = [&records](Result<CapabilityRecord> record) -> Status {
        if (!record.ok()) {
            return record.status();
        }
        const Status normalized = normalize_token_record(*record);
        if (!normalized.ok()) {
            return normalized;
        }
        records.push_back(*record);
        return Status::success();
    };

    Status status = push(make_version(kRuntimeVersion, profile.runtime_version.to_string(), CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_version(kDriverVersion, profile.driver_version.to_string(), CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kIsaCodeObjectTargets, profile.isa_targets, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kNumericFormats, profile.numeric_formats, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kExecutionFeatures, profile.execution_features, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kApiFamily, profile.api_families, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kMemoryClasses, profile.memory_classes, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kPeerAccess, profile.peer_access, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kInterconnectLinks, profile.interconnect_links, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_token_set(kCommCollectives, profile.collectives, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kSyncStreamSemantics, profile.stream_semantics, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kMemoryTotalBytes, profile.memory_total_bytes, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kMemoryFreeBytes, profile.memory_free_bytes, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kMemoryAddressBits, profile.address_bits, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kMemoryAllocationAlignmentBytes, profile.allocation_alignment_bytes,
                               CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kMemoryPageGranularityBytes, profile.page_granularity_bytes, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kKernelMaxThreadsPerBlock, profile.max_threads_per_block, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kKernelMaxSharedMemoryPerBlockBytes, profile.max_shared_memory_per_block_bytes,
                               CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_integer(kQueueConcurrentStreams, profile.concurrent_streams, CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kSupportLevel, "synthetic", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kEvidenceProvenance, "synthetic", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kHealthReadiness, "ready", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kHealthThermalState, "nominal", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kHealthErrorState, "none", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }
    status = push(make_enumeration(kPartitionMode, "none", CapabilityState::Supported));
    if (!status.ok()) {
        return status;
    }

    add_presence(records, kNumericFp64, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "fp64") != profile.numeric_formats.end());
    add_presence(records, kNumericFp32, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "fp32") != profile.numeric_formats.end());
    add_presence(records, kNumericFp16, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "fp16") != profile.numeric_formats.end());
    add_presence(records, kNumericBf16, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "bf16") != profile.numeric_formats.end());
    add_presence(records, kNumericTf32, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "tf32") != profile.numeric_formats.end());
    add_presence(records, kNumericFp8E4m3, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                     "fp8_e4m3") != profile.numeric_formats.end());
    add_presence(records, kNumericFp8E5m2, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                     "fp8_e5m2") != profile.numeric_formats.end());
    add_presence(records, kNumericFp6E2m3, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                     "fp6_e2m3") != profile.numeric_formats.end());
    add_presence(records, kNumericFp4E2m1, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                     "fp4_e2m1") != profile.numeric_formats.end());
    add_presence(records, kNumericInt8, std::find(profile.numeric_formats.begin(), profile.numeric_formats.end(),
                                                  "int8") != profile.numeric_formats.end());
    add_presence(records, kNumericDenormalsPreserved, true);
    add_presence(records, kNumericFmaContraction, true);

    add_presence(records, kTensorMatrixEngine, profile.tensor_matrix_engine);
    add_presence(records, kTensorWarpGroupMma, profile.tensor_warp_group_mma);
    add_presence(records, kTensorBlockScaledMma, profile.tensor_block_scaled_mma);
    add_presence(records, kTensorStructuredSparsity, profile.tensor_structured_sparsity);

    add_presence(records, kMemoryUnifiedAddressing, profile.memory_unified_addressing);
    add_presence(records, kMemoryManagedAllocation, profile.memory_managed_allocation);
    add_presence(records, kMemoryVirtualManagement, profile.memory_virtual_management);
    add_presence(records, kMemoryHostPinnedTransfer, profile.memory_host_pinned_transfer);
    add_presence(records, kMemoryEccEnabled, profile.memory_ecc_enabled);

    add_presence(records, kPeerUnifiedAccess, false);
    add_presence(records, kSyncEvents, true);
    add_presence(records, kSyncGraphCapture, profile.sync_graph_capture);
    add_presence(records, kKernelDynamicParallelism, profile.kernel_dynamic_parallelism);
    add_presence(records, kKernelCooperativeLaunch, profile.kernel_cooperative_launch);
    add_presence(records, kCommRdma, profile.comm_rdma);
    add_presence(records, kCommDirectPeerMemory, profile.comm_direct_peer_memory);
    add_presence(records, kCommHostStagingRequired, profile.comm_host_staging_required);

    add_presence(records, kMigrationLiveStateTransfer, profile.migration_live_state_transfer);
    add_presence(records, kMigrationCheckpointRestore, profile.migration_checkpoint_restore);
    add_presence(records, kMigrationStateExport, profile.migration_state_export);
    add_presence(records, kMigrationStateReconstruction, profile.migration_state_reconstruction);
    add_presence(records, kMigrationCrossVendorState, profile.migration_cross_vendor_state);

    add_presence(records, kPortabilityRecompileAvailable, profile.portability_recompile_available);
    add_presence(records, kPortabilityRepackageAvailable, profile.portability_repackage_available);

    for (const auto& extension : profile.extensions) {
        std::vector<std::string> tokens;
        std::size_t start = 0;
        while (start <= extension.second.size()) {
            const std::size_t comma = extension.second.find(',', start);
            const std::size_t end = comma == std::string::npos ? extension.second.size() : comma;
            tokens.emplace_back(extension.second.substr(start, end - start));
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        status = push(make_token_set(extension.first, std::move(tokens), CapabilityState::Supported));
        if (!status.ok()) {
            return status;
        }
    }

    CapabilitySet set;
    status = set.set_records(std::move(records));
    if (!status.ok()) {
        return status;
    }
    status = set.set_closed_namespaces(closed_namespaces_for(profile));
    if (!status.ok()) {
        return status;
    }
    return set;
}

Result<AcceleratorDescriptor> build_descriptor(const DeviceProfile& profile, const AdapterContext& context,
                                               const std::string& adapter_name, std::int64_t device_index) {
    DeviceProfile local = profile;
    local.device_index = device_index;

    const Result<CapabilitySet> capabilities = build_capability_set(local);
    if (!capabilities.ok()) {
        return capabilities.status();
    }
    const Result<std::string> vendor = canonical_token(local.vendor_token, Limits::kMaxTokenBytes);
    if (!vendor.ok()) {
        return vendor.status();
    }
    const Result<std::string> product = canonical_token(local.product_token, Limits::kMaxTokenBytes);
    if (!product.ok()) {
        return product.status();
    }
    const Result<std::string> architecture = canonical_token(local.architecture_token, Limits::kMaxTokenBytes);
    if (!architecture.ok()) {
        return architecture.status();
    }
    const Result<std::string> generation = canonical_token(local.device_generation_token, Limits::kMaxTokenBytes);
    if (!generation.ok()) {
        return generation.status();
    }
    const Result<std::string> runtime = canonical_token(local.runtime_family, Limits::kMaxTokenBytes);
    if (!runtime.ok()) {
        return runtime.status();
    }

    PhysicalDeviceId physical = PhysicalDeviceId::from_raw(derive_identity(
        "haf.synthetic.physical", context.node_token + "/" + adapter_name + "/" + device_token(local)));

    AcceleratorDescriptor descriptor;
    descriptor.physical_device = physical;
    descriptor.generation = DeviceGeneration(1);
    descriptor.agent = context.agent;
    descriptor.agent_boot = context.agent_boot;
    descriptor.node = context.node;
    descriptor.vendor_token = *vendor;
    descriptor.vendor = vendor_id_from_token(*vendor);
    descriptor.product_token = *product;
    descriptor.architecture_token = *architecture;
    descriptor.architecture = architecture_id_from_token(*architecture);
    descriptor.device_generation_token = *generation;
    descriptor.runtime_family = *runtime;
    descriptor.runtime_version = local.runtime_version;
    descriptor.driver_version = local.driver_version;
    descriptor.capabilities = *capabilities;
    descriptor.capabilities.set_generation(CapabilityGeneration(1));
    descriptor.support_level = SupportLevel::Synthetic;
    descriptor.provenance = EvidenceProvenance::Synthetic;
    descriptor.topology_references = local.topology_references;
    std::sort(descriptor.topology_references.begin(), descriptor.topology_references.end());
    descriptor.topology_references.erase(
        std::unique(descriptor.topology_references.begin(), descriptor.topology_references.end()),
        descriptor.topology_references.end());
    descriptor.observed_at = now_timestamp();
    descriptor.id = derive_accelerator_id(physical, context.agent, context.agent_boot, descriptor.generation);

    EvidenceRecord evidence = make_evidence(
        adapter_name, "deterministic synthetic device model", "1.0.0", descriptor.id, physical,
        EvidenceProvenance::Synthetic, EvidenceKind::Declaration, descriptor.capabilities.digest(),
        RuntimeVersion{local.runtime_version.major(), local.runtime_version.minor(), local.runtime_version.patch()},
        RuntimeVersion{local.driver_version.major(), local.driver_version.minor(), local.driver_version.patch()},
        Limits::kDefaultEvidenceFreshnessNanos, true);
    evidence.capability_generation = descriptor.capabilities.generation();
    descriptor.evidence.push_back(std::move(evidence));

    const Status valid = descriptor.validate();
    if (!valid.ok()) {
        return valid;
    }
    return descriptor;
}

SyntheticAdapter::SyntheticAdapter(std::string adapter_name, DeviceProfile profile)
    : adapter_name_(std::move(adapter_name)), profile_(std::move(profile)) {}

Result<AdapterObservation> SyntheticAdapter::observe(const AdapterContext& context) const {
    AdapterObservation observation;
    observation.adapter = adapter_name_;
    observation.provenance = EvidenceProvenance::Synthetic;
    observation.support_level = SupportLevel::Synthetic;
    observation.detail = "deterministic synthetic profile for '" + profile_.vendor_token +
                         "'; no physical device is involved";
    const Result<AcceleratorDescriptor> descriptor = build_descriptor(profile_, context, adapter_name_, 0);
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    observation.devices.push_back(*descriptor);
    return observation;
}

}  // namespace haf::adapters
