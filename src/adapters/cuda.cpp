#include "haf/adapters/cuda.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf::adapters {
namespace {

extern "C" int haf_cuda_probe_run(int device_index, std::uint64_t* bytes_transferred, std::uint64_t* free_before,
                                  std::uint64_t* free_after, char* error_buffer, int error_buffer_size);

[[nodiscard]] std::string cuda_error_text(cudaError_t status) {
    const char* text = cudaGetErrorString(status);
    return text == nullptr ? std::string("unknown CUDA error") : std::string(text);
}

[[nodiscard]] std::string trim_device_name(const char* raw) {
    std::string name(raw == nullptr ? "" : raw);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    std::size_t begin = 0;
    while (begin < name.size() && (name[begin] == ' ' || name[begin] == '\t')) {
        ++begin;
    }
    name = name.substr(begin);
    return name;
}

/// Canonical product token derived from the reported device name.
[[nodiscard]] std::string product_token_from_name(const std::string& name) {
    // Vendor product names contain spaces and punctuation, so they are slugged
    // into canonical taxonomy tokens rather than rejected.
    const Result<std::string> token = slug_token(name, Limits::kMaxTokenBytes);
    if (token.ok()) {
        return *token;
    }
    return "unknown-device";
}

[[nodiscard]] std::string architecture_token_for(int major, int minor) {
    // Architecture family names are public NVIDIA marketing names; mapping them
    // here keeps vendor knowledge inside the adapter.
    if (major >= 12) {
        return "blackwell";
    }
    if (major == 11) {
        return "blackwell";
    }
    if (major == 10) {
        return "blackwell";
    }
    if (major == 9) {
        return "hopper";
    }
    if (major == 8) {
        return minor == 9 ? "ada-lovelace" : "ampere";
    }
    if (major == 7) {
        return minor == 5 ? "turing" : "volta";
    }
    if (major == 6) {
        return "pascal";
    }
    return "nvidia-unknown";
}

[[nodiscard]] std::string device_generation_token_for(int major, int minor) {
    return "sm-" + std::to_string(major) + std::to_string(minor);
}

[[nodiscard]] SemanticVersion semantic_from_cuda_version(int version) {
    const std::uint32_t major = static_cast<std::uint32_t>((version / 1000) % 1000);
    const std::uint32_t minor = static_cast<std::uint32_t>((version / 10) % 100);
    const std::uint32_t patch = static_cast<std::uint32_t>(version % 10);
    return SemanticVersion(major, minor, patch);
}

[[nodiscard]] std::vector<std::string> numeric_formats_for(const cudaDeviceProp& properties) {
    std::vector<std::string> formats;
    formats.emplace_back("fp32");
    if (properties.major >= 2) {
        formats.emplace_back("fp64");
    }
    if (properties.major >= 5) {
        formats.emplace_back("fp16");
    }
    if (properties.major >= 8) {
        formats.emplace_back("bf16");
        formats.emplace_back("tf32");
        formats.emplace_back("int8");
    }
    if (properties.major >= 9) {
        formats.emplace_back("fp8_e4m3");
        formats.emplace_back("fp8_e5m2");
    }
    if (properties.major >= 10) {
        formats.emplace_back("fp6_e2m3");
        formats.emplace_back("fp4_e2m1");
    }
    std::sort(formats.begin(), formats.end());
    formats.erase(std::unique(formats.begin(), formats.end()), formats.end());
    return formats;
}

}  // namespace

bool cuda_adapter_compiled() { return true; }

CudaInventory cuda_inventory() {
    CudaInventory inventory;
    int count = 0;
    cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess) {
        inventory.detail = "cudaGetDeviceCount failed: " + cuda_error_text(status);
        return inventory;
    }
    int runtime_version = 0;
    int driver_version = 0;
    status = cudaRuntimeGetVersion(&runtime_version);
    if (status != cudaSuccess) {
        inventory.detail = "cudaRuntimeGetVersion failed: " + cuda_error_text(status);
        return inventory;
    }
    status = cudaDriverGetVersion(&driver_version);
    if (status != cudaSuccess) {
        inventory.detail = "cudaDriverGetVersion failed: " + cuda_error_text(status);
        return inventory;
    }
    inventory.available = true;
    inventory.device_count = count;
    inventory.runtime_version = semantic_from_cuda_version(runtime_version).to_string();
    inventory.driver_version = semantic_from_cuda_version(driver_version).to_string();
    for (int index = 0; index < count; ++index) {
        cudaDeviceProp properties{};
        if (cudaGetDeviceProperties(&properties, index) != cudaSuccess) {
            continue;
        }
        inventory.device_names.push_back(trim_device_name(properties.name));
    }
    inventory.detail = "real CUDA runtime on physical hardware";
    return inventory;
}

namespace {

[[nodiscard]] Result<AcceleratorDescriptor> build_cuda_descriptor(const AdapterContext& context, int index,
                                                                  int runtime_version, int driver_version) {
    cudaDeviceProp properties{};
    cudaError_t status = cudaGetDeviceProperties(&properties, index);
    if (status != cudaSuccess) {
        return Status(ErrorCode::Unsupported, "cudaGetDeviceProperties failed: " + cuda_error_text(status));
    }

    std::size_t free_memory = 0;
    std::size_t total_memory = 0;
    const cudaError_t memory_status = cudaMemGetInfo(&free_memory, &total_memory);
    const bool memory_known = memory_status == cudaSuccess;

    const std::string name = trim_device_name(properties.name);
    const std::string product = product_token_from_name(name);
    const std::string architecture = architecture_token_for(properties.major, properties.minor);
    const std::string device_generation = device_generation_token_for(properties.major, properties.minor);
    const std::string isa_target = device_generation;
    const std::string ptx_target = "compute-" + std::to_string(properties.major) + std::to_string(properties.minor);

    std::vector<CapabilityRecord> records;
    auto push = [&records](Result<CapabilityRecord> record) -> Status {
        if (!record.ok()) {
            return record.status();
        }
        records.push_back(*record);
        return Status::success();
    };
    auto presence = [&records](std::string_view key, bool supported) {
        records.push_back(make_presence(key, supported ? CapabilityState::Supported : CapabilityState::Unsupported));
    };

    Status record_status = push(make_enumeration(cap::kVendorId, "nvidia", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kVendorProduct, product, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kArchitectureFamily, architecture, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kArchitectureDeviceGeneration, properties.major * 10 + properties.minor,
                                      CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kRuntimeFamily, "cuda", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status =
        push(make_version(cap::kRuntimeVersion, semantic_from_cuda_version(runtime_version).to_string(),
                          CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status =
        push(make_version(cap::kDriverVersion, semantic_from_cuda_version(driver_version).to_string(),
                          CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_version(cap::kComputeCapability, std::to_string(properties.major) + "." +
                                                                     std::to_string(properties.minor) + ".0",
                                      CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_token_set(cap::kIsaCodeObjectTargets, {isa_target, ptx_target},
                                        CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_token_set(cap::kNumericFormats, numeric_formats_for(properties),
                                        CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }

    std::vector<std::string> features{"graph-capture", "stream-events", "unified-memory"};
    if (properties.concurrentKernels != 0) {
        features.emplace_back("concurrent-kernels");
    }
    if (properties.cooperativeLaunch != 0) {
        features.emplace_back("cooperative-launch");
    }
    // Dynamic parallelism is available from compute capability 3.5 onwards;
    // CUDA exposes no direct device property for it, so it is derived from the
    // compute capability rather than guessed.
    const bool dynamic_parallelism = properties.major > 3 || (properties.major == 3 && properties.minor >= 5);
    if (dynamic_parallelism) {
        features.emplace_back("dynamic-parallelism");
    }
    if (properties.streamPrioritiesSupported != 0) {
        features.emplace_back("stream-priorities");
    }
    if (properties.managedMemory != 0) {
        features.emplace_back("managed-memory");
    }
    record_status = push(make_token_set(cap::kExecutionFeatures, features, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_token_set(cap::kApiFamily, {"cuda-driver", "cuda-runtime"}, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }

    std::vector<std::string> memory_classes{"device", "host-pinned"};
    if (properties.managedMemory != 0) {
        memory_classes.emplace_back("managed");
    }
    if (properties.unifiedAddressing != 0) {
        memory_classes.emplace_back("unified");
    }
    record_status = push(make_token_set(cap::kMemoryClasses, memory_classes, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kMemoryTotalBytes,
                                      static_cast<std::int64_t>(properties.totalGlobalMem), CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    if (memory_known) {
        record_status = push(make_integer(cap::kMemoryFreeBytes, static_cast<std::int64_t>(free_memory),
                                          CapabilityState::Supported));
        if (!record_status.ok()) {
            return record_status;
        }
    } else {
        presence(cap::kMemoryFreeBytes, false);
    }
    record_status = push(make_integer(cap::kMemoryAddressBits, 64, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kMemoryAllocationAlignmentBytes, 256, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kMemoryPageGranularityBytes,
                                      static_cast<std::int64_t>(properties.memPitch == 0 ? 65536 : properties.memPitch),
                                      CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kKernelMaxThreadsPerBlock, properties.maxThreadsPerBlock,
                                      CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kKernelMaxSharedMemoryPerBlockBytes,
                                      static_cast<std::int64_t>(properties.sharedMemPerBlock),
                                      CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_integer(cap::kQueueConcurrentStreams, 32, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kSyncStreamSemantics, "in-order-queue", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kSupportLevel, "native", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kEvidenceProvenance, "real", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kHealthReadiness, "ready", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kHealthThermalState, "nominal", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_enumeration(cap::kHealthErrorState, "none", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }

    std::vector<std::string> formats = numeric_formats_for(properties);
    auto has_format = [&formats](const char* token) {
        return std::find(formats.begin(), formats.end(), token) != formats.end();
    };
    presence(cap::kNumericFp64, has_format("fp64"));
    presence(cap::kNumericFp32, has_format("fp32"));
    presence(cap::kNumericFp16, has_format("fp16"));
    presence(cap::kNumericBf16, has_format("bf16"));
    presence(cap::kNumericTf32, has_format("tf32"));
    presence(cap::kNumericFp8E4m3, has_format("fp8_e4m3"));
    presence(cap::kNumericFp8E5m2, has_format("fp8_e5m2"));
    presence(cap::kNumericFp6E2m3, has_format("fp6_e2m3"));
    presence(cap::kNumericFp4E2m1, has_format("fp4_e2m1"));
    presence(cap::kNumericInt8, has_format("int8"));
    presence(cap::kNumericDenormalsPreserved, true);
    presence(cap::kNumericFmaContraction, true);

    presence(cap::kTensorMatrixEngine, properties.major >= 7);
    presence(cap::kTensorWarpGroupMma, properties.major >= 9);
    presence(cap::kTensorBlockScaledMma, properties.major >= 10);
    presence(cap::kTensorStructuredSparsity, properties.major >= 8);

    presence(cap::kMemoryUnifiedAddressing, properties.unifiedAddressing != 0);
    presence(cap::kMemoryManagedAllocation, properties.managedMemory != 0);
    presence(cap::kMemoryVirtualManagement, true);
    presence(cap::kMemoryHostPinnedTransfer, true);
    presence(cap::kMemoryEccEnabled, properties.ECCEnabled != 0);

    std::vector<std::string> peer_links;
    if (properties.computePreemptionSupported != 0) {
        peer_links.emplace_back("preemption");
    }
    peer_links.emplace_back("pcie");
    peer_links.emplace_back("nvlink-or-pcie");
    std::sort(peer_links.begin(), peer_links.end());
    record_status = push(make_token_set(cap::kPeerAccess, peer_links, CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    presence(cap::kPeerUnifiedAccess, properties.unifiedAddressing != 0);
    record_status = push(make_token_set(cap::kInterconnectLinks, {"pcie-gen5", "nvlink-or-pcie"},
                                        CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    record_status = push(make_token_set(cap::kCommCollectives, {"all-reduce", "broadcast", "all-gather"},
                                        CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }
    presence(cap::kCommRdma, false);
    presence(cap::kCommDirectPeerMemory, properties.unifiedAddressing != 0);
    presence(cap::kCommHostStagingRequired, false);

    presence(cap::kSyncEvents, true);
    presence(cap::kSyncGraphCapture, true);
    presence(cap::kKernelDynamicParallelism,
             properties.major > 3 || (properties.major == 3 && properties.minor >= 5));
    presence(cap::kKernelCooperativeLaunch, properties.cooperativeLaunch != 0);

    // Honest absence: CUDA offers no live accelerator-state migration, no
    // device checkpoint/restore, and no cross-vendor state transfer.
    presence(cap::kMigrationLiveStateTransfer, false);
    presence(cap::kMigrationCheckpointRestore, false);
    presence(cap::kMigrationStateExport, false);
    presence(cap::kMigrationStateReconstruction, true);
    presence(cap::kMigrationCrossVendorState, false);
    presence(cap::kPortabilityRecompileAvailable, true);
    presence(cap::kPortabilityRepackageAvailable, true);

    record_status = push(make_enumeration(cap::kPartitionMode, "none", CapabilityState::Supported));
    if (!record_status.ok()) {
        return record_status;
    }

    CapabilitySet capability_set;
    record_status = capability_set.set_records(std::move(records));
    if (!record_status.ok()) {
        return record_status;
    }
    // CUDA advertises a complete, closed view of every namespace it speaks
    // about, so an absent capability is positive evidence of absence.
    record_status = capability_set.set_closed_namespaces(
        {"api", "architecture", "comm", "compute", "driver", "health", "interconnect", "isa", "kernel",
         "memory", "migration", "numeric", "partition", "peer", "portability", "queue", "runtime", "support",
         "sync", "tensor", "vendor"});
    if (!record_status.ok()) {
        return record_status;
    }
    capability_set.set_generation(CapabilityGeneration(1));

    // Physical identity: PCI bus/device/domain plus the UUID, which is stable
    // for a physical device across process restarts but not across re-cabling.
    char uuid_buffer[64] = {};
    std::string uuid_text;
    {
        cudaUUID_t uuid{};
        bool have_uuid = false;
        if (properties.uuid.bytes[0] != 0) {
            uuid = properties.uuid;
            have_uuid = true;
        }
        if (have_uuid) {
            std::snprintf(uuid_buffer, sizeof(uuid_buffer),
                          "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                          static_cast<unsigned char>(uuid.bytes[0]), static_cast<unsigned char>(uuid.bytes[1]),
                          static_cast<unsigned char>(uuid.bytes[2]), static_cast<unsigned char>(uuid.bytes[3]),
                          static_cast<unsigned char>(uuid.bytes[4]), static_cast<unsigned char>(uuid.bytes[5]),
                          static_cast<unsigned char>(uuid.bytes[6]), static_cast<unsigned char>(uuid.bytes[7]),
                          static_cast<unsigned char>(uuid.bytes[8]), static_cast<unsigned char>(uuid.bytes[9]),
                          static_cast<unsigned char>(uuid.bytes[10]), static_cast<unsigned char>(uuid.bytes[11]),
                          static_cast<unsigned char>(uuid.bytes[12]), static_cast<unsigned char>(uuid.bytes[13]),
                          static_cast<unsigned char>(uuid.bytes[14]), static_cast<unsigned char>(uuid.bytes[15]));
            uuid_text = uuid_buffer;
        }
    }
    char pci_buffer[64] = {};
    std::snprintf(pci_buffer, sizeof(pci_buffer), "pci-%04x:%02x:%02x.0", properties.pciDomainID,
                  properties.pciBusID, properties.pciDeviceID);
    const std::string physical_token = uuid_text.empty() ? std::string(pci_buffer) : uuid_text;

    AcceleratorDescriptor descriptor;
    descriptor.physical_device =
        PhysicalDeviceId::from_raw(derive_identity("haf.cuda.physical", physical_token));
    descriptor.generation = DeviceGeneration(1);
    descriptor.agent = context.agent;
    descriptor.agent_boot = context.agent_boot;
    descriptor.node = context.node;
    descriptor.vendor_token = "nvidia";
    descriptor.vendor = vendor_id_from_token("nvidia");
    descriptor.product_token = product;
    descriptor.architecture_token = architecture;
    descriptor.architecture = architecture_id_from_token(architecture);
    descriptor.device_generation_token = device_generation;
    descriptor.runtime_family = "cuda";
    descriptor.runtime_version = semantic_from_cuda_version(runtime_version);
    descriptor.driver_version = semantic_from_cuda_version(driver_version);
    descriptor.capabilities = capability_set;
    descriptor.support_level = SupportLevel::Native;
    descriptor.provenance = EvidenceProvenance::Real;
    descriptor.topology_references.emplace_back(pci_buffer);
    descriptor.observed_at = now_timestamp();
    descriptor.id = derive_accelerator_id(descriptor.physical_device, context.agent, context.agent_boot,
                                          descriptor.generation);

    EvidenceRecord evidence = make_evidence(
        "cuda-runtime", "cudaGetDeviceProperties/cudaMemGetInfo/cudaRuntimeGetVersion", "1.0.0", descriptor.id,
        descriptor.physical_device, EvidenceProvenance::Real, EvidenceKind::StaticDescriptor,
        descriptor.capabilities.digest(),
        RuntimeVersion{descriptor.runtime_version.major(), descriptor.runtime_version.minor(),
                       descriptor.runtime_version.patch()},
        RuntimeVersion{descriptor.driver_version.major(), descriptor.driver_version.minor(),
                       descriptor.driver_version.patch()},
        Limits::kDefaultEvidenceFreshnessNanos, true);
    evidence.capability_generation = descriptor.capabilities.generation();
    descriptor.evidence.push_back(std::move(evidence));

    const Status valid = descriptor.validate();
    if (!valid.ok()) {
        return valid;
    }
    return descriptor;
}

class CudaAdapter final : public AcceleratorAdapter {
public:
    [[nodiscard]] std::string name() const override { return "cuda-runtime"; }
    [[nodiscard]] SupportLevel support_level() const override { return SupportLevel::Native; }
    [[nodiscard]] EvidenceProvenance provenance() const override { return EvidenceProvenance::Real; }

    [[nodiscard]] Result<AdapterObservation> observe(const AdapterContext& context) const override {
        AdapterObservation observation;
        observation.adapter = "cuda-runtime";
        observation.provenance = EvidenceProvenance::Real;
        observation.support_level = SupportLevel::Native;

        int count = 0;
        const cudaError_t count_status = cudaGetDeviceCount(&count);
        if (count_status != cudaSuccess) {
            return Status(ErrorCode::Unsupported,
                          "CUDA runtime present but cudaGetDeviceCount failed: " + cuda_error_text(count_status));
        }
        if (count <= 0) {
            return Status(ErrorCode::Unsupported, "CUDA runtime present but no CUDA device is visible");
        }
        int runtime_version = 0;
        int driver_version = 0;
        if (cudaRuntimeGetVersion(&runtime_version) != cudaSuccess) {
            return Status(ErrorCode::Unsupported, "cudaRuntimeGetVersion failed");
        }
        if (cudaDriverGetVersion(&driver_version) != cudaSuccess) {
            return Status(ErrorCode::Unsupported, "cudaDriverGetVersion failed");
        }
        for (int index = 0; index < count; ++index) {
            const Result<AcceleratorDescriptor> descriptor =
                build_cuda_descriptor(context, index, runtime_version, driver_version);
            if (!descriptor.ok()) {
                return descriptor.status();
            }
            observation.devices.push_back(*descriptor);
        }
        observation.detail = "observed " + std::to_string(count) +
                             " CUDA device(s) through the CUDA runtime on physical hardware";
        return observation;
    }

    [[nodiscard]] Result<ProofResult> prove(int device_index) const override {
        const Result<CudaProof> proof = run_cuda_proof(device_index);
        if (!proof.ok()) {
            return proof.status();
        }
        ProofResult result;
        result.executed = proof->executed;
        result.verified = proof->verified;
        result.detail = proof->detail;
        result.bytes_transferred = proof->bytes_transferred;
        return result;
    }
};

}  // namespace

std::unique_ptr<AcceleratorAdapter> make_cuda_adapter() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
        return nullptr;
    }
    return std::make_unique<CudaAdapter>();
}

Result<CudaProof> run_cuda_proof(int device_index) {
    CudaProof proof;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || device_index < 0 || device_index >= count) {
        return Status(ErrorCode::InvalidArgument, "CUDA device index is out of range");
    }
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, device_index) != cudaSuccess) {
        return Status(ErrorCode::Unsupported, "cudaGetDeviceProperties failed for the proof device");
    }
    proof.device_name = trim_device_name(properties.name);

    std::uint64_t bytes_transferred = 0;
    std::uint64_t free_before = 0;
    std::uint64_t free_after = 0;
    char error_buffer[512] = {};
    const int rc = haf_cuda_probe_run(device_index, &bytes_transferred, &free_before, &free_after, error_buffer,
                                      static_cast<int>(sizeof(error_buffer)));
    proof.bytes_transferred = bytes_transferred;
    proof.free_bytes_before = free_before;
    proof.free_bytes_after = free_after;
    proof.executed = rc != 1 && rc != 2 && rc != 3 && rc != 4 && rc != 5;
    proof.verified = rc == 0;
    proof.detail = rc == 0 ? "kernel executed on device and matched the CPU reference"
                           : std::string("CUDA proof failed (stage ") + std::to_string(rc) + "): " +
                                 (error_buffer[0] == '\0' ? std::string("no diagnostics") : std::string(error_buffer));
    if (rc != 0 && !proof.executed) {
        return Status(ErrorCode::Unsupported, proof.detail);
    }
    return proof;
}

}  // namespace haf::adapters
