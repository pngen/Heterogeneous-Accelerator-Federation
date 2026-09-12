#include "haf/model/capability_key.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

struct CoreKeyDefinition {
    std::string_view name;
    CapabilityKind kind;
};

// The table below is maintained by hand; the registry sorts it before assigning
// indices, so a mistaken entry order cannot change the public vocabulary.

// Canonical, closed vocabulary of the federation core. Kept sorted by name so
// that enumeration order is deterministic without a runtime sort.
constexpr CoreKeyDefinition kCoreKeys[] = {
    {"api.family", CapabilityKind::TokenSet},
    {"architecture.device_generation", CapabilityKind::Integer},
    {"architecture.family", CapabilityKind::Enumeration},
    {"comm.collectives", CapabilityKind::TokenSet},
    {"comm.direct_peer_memory", CapabilityKind::Presence},
    {"comm.host_staging_required", CapabilityKind::Presence},
    {"comm.rdma", CapabilityKind::Presence},
    {"compute.capability", CapabilityKind::Version},
    {"driver.version", CapabilityKind::Version},
    {"evidence.provenance", CapabilityKind::Enumeration},
    {"execution.features", CapabilityKind::TokenSet},
    {"health.error_state", CapabilityKind::Enumeration},
    {"health.readiness", CapabilityKind::Enumeration},
    {"health.thermal_state", CapabilityKind::Enumeration},
    {"interconnect.links", CapabilityKind::TokenSet},
    {"isa.code_object_targets", CapabilityKind::TokenSet},
    {"kernel.cooperative_launch", CapabilityKind::Presence},
    {"kernel.dynamic_parallelism", CapabilityKind::Presence},
    {"kernel.max_shared_memory_per_block_bytes", CapabilityKind::Integer},
    {"kernel.max_threads_per_block", CapabilityKind::Integer},
    {"memory.address_bits", CapabilityKind::Integer},
    {"memory.allocation_alignment_bytes", CapabilityKind::Integer},
    {"memory.classes", CapabilityKind::TokenSet},
    {"memory.ecc_enabled", CapabilityKind::Presence},
    {"memory.free_bytes", CapabilityKind::Integer},
    {"memory.host_pinned_transfer", CapabilityKind::Presence},
    {"memory.managed_allocation", CapabilityKind::Presence},
    {"memory.page_granularity_bytes", CapabilityKind::Integer},
    {"memory.total_bytes", CapabilityKind::Integer},
    {"memory.unified_addressing", CapabilityKind::Presence},
    {"memory.virtual_management", CapabilityKind::Presence},
    {"migration.checkpoint_restore", CapabilityKind::Presence},
    {"migration.cross_vendor_state", CapabilityKind::Presence},
    {"migration.live_state_transfer", CapabilityKind::Presence},
    {"migration.state_export", CapabilityKind::Presence},
    {"migration.state_reconstruction", CapabilityKind::Presence},
    {"numeric.bf16", CapabilityKind::Presence},
    {"numeric.denormals_preserved", CapabilityKind::Presence},
    {"numeric.formats", CapabilityKind::TokenSet},
    {"numeric.fma_contraction", CapabilityKind::Presence},
    {"numeric.fp16", CapabilityKind::Presence},
    {"numeric.fp32", CapabilityKind::Presence},
    {"numeric.fp4_e2m1", CapabilityKind::Presence},
    {"numeric.fp64", CapabilityKind::Presence},
    {"numeric.fp6_e2m3", CapabilityKind::Presence},
    {"numeric.fp8_e4m3", CapabilityKind::Presence},
    {"numeric.fp8_e5m2", CapabilityKind::Presence},
    {"numeric.int8", CapabilityKind::Presence},
    {"numeric.tf32", CapabilityKind::Presence},
    {"partition.mode", CapabilityKind::Enumeration},
    {"partition.observed_profiles", CapabilityKind::TokenSet},
    {"peer.access", CapabilityKind::TokenSet},
    {"peer.unified_access", CapabilityKind::Presence},
    {"portability.recompile_available", CapabilityKind::Presence},
    {"portability.repackage_available", CapabilityKind::Presence},
    {"queue.concurrent_streams", CapabilityKind::Integer},
    {"runtime.family", CapabilityKind::Enumeration},
    {"runtime.version", CapabilityKind::Version},
    {"support.level", CapabilityKind::Enumeration},
    {"sync.events", CapabilityKind::Presence},
    {"sync.graph_capture", CapabilityKind::Presence},
    {"sync.stream_semantics", CapabilityKind::Enumeration},
    {"tensor.block_scaled_mma", CapabilityKind::Presence},
    {"tensor.matrix_engine", CapabilityKind::Presence},
    {"tensor.structured_sparsity", CapabilityKind::Presence},
    {"tensor.warp_group_mma", CapabilityKind::Presence},
    {"vendor.id", CapabilityKind::Enumeration},
    {"vendor.product", CapabilityKind::Enumeration},
};

constexpr std::size_t kCoreKeyCount = sizeof(kCoreKeys) / sizeof(kCoreKeys[0]);

struct RegistryStorage {
    RegistryStorage() {
        // Indices are assigned in canonical name order so that enumeration is
        // deterministic no matter how the definition table happens to be laid
        // out. Ordering is derived, never assumed.
        std::vector<CoreKeyDefinition> sorted(kCoreKeys, kCoreKeys + kCoreKeyCount);
        std::sort(sorted.begin(), sorted.end(),
                  [](const CoreKeyDefinition& a, const CoreKeyDefinition& b) { return a.name < b.name; });
        names.reserve(sorted.size());
        kinds.reserve(sorted.size());
        core_keys.reserve(sorted.size());
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            index_by_name.emplace(std::string(sorted[i].name), static_cast<std::uint32_t>(i));
            names.emplace_back(sorted[i].name);
            kinds.push_back(sorted[i].kind);
            core_keys.push_back(CapabilityKey::from_index(static_cast<std::uint32_t>(i)));
        }
    }

    RegistryStorage(const RegistryStorage&) = delete;
    RegistryStorage& operator=(const RegistryStorage&) = delete;
    RegistryStorage(RegistryStorage&&) = delete;
    RegistryStorage& operator=(RegistryStorage&&) = delete;

    std::vector<std::string> names;
    std::vector<CapabilityKind> kinds;
    /// Core keys only. Extension keys are interned into names/kinds but never
    /// here, so the core vocabulary stays fixed and canonically ordered.
    std::vector<CapabilityKey> core_keys;
    std::unordered_map<std::string, std::uint32_t> index_by_name;
    std::deque<std::string> extension_storage;
    std::mutex mutex;
};

/// Process-lifetime registry. Deliberately function-local so that static
/// initialization order cannot make key resolution observe a half-built table.
[[nodiscard]] RegistryStorage& storage() {
    static RegistryStorage* instance = new RegistryStorage();
    return *instance;
}

[[nodiscard]] bool is_extension_name(std::string_view name) noexcept {
    // Extension namespace: "x.<namespace>.<name>" with at least one further dot.
    if (name.size() < 5 || name[0] != 'x' || name[1] != '.') {
        return false;
    }
    const std::size_t second = name.find('.', 2);
    if (second == std::string_view::npos || second + 1 >= name.size()) {
        return false;
    }
    return true;
}

}  // namespace

std::string_view to_string(CapabilityKind kind) noexcept {
    switch (kind) {
        case CapabilityKind::Presence: return "presence";
        case CapabilityKind::Version: return "version";
        case CapabilityKind::Integer: return "integer";
        case CapabilityKind::Scalar: return "scalar";
        case CapabilityKind::Enumeration: return "enumeration";
        case CapabilityKind::TokenSet: return "token_set";
    }
    return "unknown";
}

bool capability_kind_from_wire(std::uint8_t raw, CapabilityKind& out) noexcept {
    switch (raw) {
        case 0: out = CapabilityKind::Presence; return true;
        case 1: out = CapabilityKind::Version; return true;
        case 2: out = CapabilityKind::Integer; return true;
        case 3: out = CapabilityKind::Scalar; return true;
        case 4: out = CapabilityKind::Enumeration; return true;
        case 5: out = CapabilityKind::TokenSet; return true;
        default: return false;
    }
}

const std::string& CapabilityKey::name() const {
    static const std::string kEmpty;
    RegistryStorage& store = storage();
    if (index_ >= store.names.size()) {
        return kEmpty;
    }
    return store.names[index_];
}

CapabilityKind CapabilityKey::kind() const noexcept {
    RegistryStorage& store = storage();
    if (index_ >= store.kinds.size()) {
        return CapabilityKind::Presence;
    }
    return store.kinds[index_];
}

std::string_view CapabilityKey::name_space() const noexcept {
    RegistryStorage& store = storage();
    if (index_ >= store.names.size()) {
        return {};
    }
    const std::string& text = store.names[index_];
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos) {
        return text;
    }
    return std::string_view(text).substr(0, dot);
}

bool CapabilityKey::is_extension() const noexcept { return is_extension_name(name()); }

std::optional<CapabilityKey> find_core_capability_key(std::string_view canonical_name) noexcept {
    RegistryStorage& store = storage();
    const auto found = store.index_by_name.find(std::string(canonical_name));
    if (found == store.index_by_name.end() || found->second >= kCoreKeyCount) {
        return std::nullopt;
    }
    return CapabilityKey::from_index(found->second);
}

Result<CapabilityKey> capability_key_from_name(std::string_view name) {
    RegistryStorage& store = storage();
    {
        const auto found = store.index_by_name.find(std::string(name));
        if (found != store.index_by_name.end()) {
            return CapabilityKey::from_index(found->second);
        }
    }
    if (!is_extension_name(name)) {
        return Status(ErrorCode::UnknownCapability,
                      "capability key is not part of the federation vocabulary and is not an extension key: '" +
                          std::string(name) + "'");
    }
    const Result<std::string> canonical = canonical_token(name, Limits::kMaxNameBytes);
    if (!canonical.ok()) {
        return canonical.status();
    }
    if (!is_extension_name(*canonical)) {
        return Status(ErrorCode::UnknownCapability, "extension capability key is malformed: '" + *canonical + "'");
    }
    std::lock_guard<std::mutex> guard(store.mutex);
    const auto found = store.index_by_name.find(*canonical);
    if (found != store.index_by_name.end()) {
        return CapabilityKey::from_index(found->second);
    }
    if (store.names.size() >= 4096U) {
        return Status(ErrorCode::ResourceExhausted, "capability extension namespace table is full");
    }
    const auto index = static_cast<std::uint32_t>(store.names.size());
    store.extension_storage.push_back(*canonical);
    store.names.push_back(store.extension_storage.back());
    // Extension keys are declared by whichever adapter owns the namespace. The
    // core cannot validate their value domain, so they are treated as token
    // sets: the most conservative shape that still carries evidence.
    store.kinds.push_back(CapabilityKind::TokenSet);
    store.index_by_name.emplace(store.extension_storage.back(), index);
    return CapabilityKey::from_index(index);
}

const std::vector<CapabilityKey>& all_core_capability_keys() { return storage().core_keys; }

}  // namespace haf
