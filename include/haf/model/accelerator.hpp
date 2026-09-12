// Heterogeneous Accelerator Federation - accelerator descriptor.
//
// The descriptor is the federation's view of one accelerator incarnation. It
// separates three things that must never be conflated:
//
//   * structural identity   - who the device is and which incarnation it is;
//   * observed capability   - what the device can do, with provenance;
//   * membership/lifecycle  - whether the federation currently trusts it.
//
// Structural fields are vendor-neutral. Everything vendor-specific is carried
// inside the capability set, either as a canonical token or inside an
// adapter-owned "x.<namespace>.*" extension key. The federation core never
// branches on a vendor name.

#ifndef HAF_MODEL_ACCELERATOR_HPP
#define HAF_MODEL_ACCELERATOR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/model/capability.hpp"
#include "haf/model/evidence.hpp"

namespace haf {

/// How faithfully the federation can represent this accelerator.
enum class SupportLevel : std::uint8_t {
    Native = 0,       ///< A real vendor runtime on real hardware backs every claim.
    Translated = 1,   ///< Real hardware observed through a translation layer.
    Synthetic = 2,    ///< Deterministic model; no physical device behind it.
    Unsupported = 3,  ///< The federation cannot represent this accelerator.
    Unknown = 4,
};

[[nodiscard]] std::string_view to_string(SupportLevel level) noexcept;
[[nodiscard]] bool support_level_from_token(std::string_view token, SupportLevel& out) noexcept;
[[nodiscard]] bool support_level_from_wire(std::uint8_t raw, SupportLevel& out) noexcept;

/// Lifecycle state of one accelerator within one federation.
enum class MemberState : std::uint8_t {
    Discovered = 0,  ///< Reported by an agent, nothing validated yet.
    Observed = 1,    ///< Capability advertisement validated and evidence bound.
    Admitted = 2,    ///< Federation accepted the member and assigned generations.
    Active = 3,      ///< Eligible to host work.
    Degraded = 4,    ///< Still admitted, but partially impaired.
    Draining = 5,    ///< No new work; existing work may finish.
    Fenced = 6,      ///< Authority revoked; no mutation accepted.
    Retired = 7,     ///< Terminal. Evidence is dead and cannot return.
};

[[nodiscard]] std::string_view to_string(MemberState state) noexcept;
[[nodiscard]] bool member_state_from_wire(std::uint8_t raw, MemberState& out) noexcept;

/// Explicit, auditable lifecycle transition table. Anything not listed here is
/// rejected with InvalidTransition.
[[nodiscard]] bool is_legal_transition(MemberState from, MemberState to) noexcept;
[[nodiscard]] bool is_terminal_state(MemberState state) noexcept;

/// Structural description of an accelerator incarnation.
struct AcceleratorDescriptor {
    AcceleratorId id{};
    PhysicalDeviceId physical_device{};
    DeviceGeneration generation{};

    AgentId agent{};
    AgentBootId agent_boot{};
    NodeId node{};

    /// Canonical vendor token plus its derived typed identity.
    std::string vendor_token;
    VendorId vendor{};
    /// Canonical product token, e.g. "geforce-rtx-5090".
    std::string product_token;
    /// Canonical architecture family token, e.g. "blackwell".
    std::string architecture_token;
    ArchitectureId architecture{};
    /// Canonical code-generation token, e.g. "sm-120" or "gfx-942".
    std::string device_generation_token;

    /// Canonical runtime family token, e.g. "cuda", "rocm", "level-zero".
    std::string runtime_family;
    SemanticVersion runtime_version{};
    SemanticVersion driver_version{};

    /// Observed capability evidence. Single source of truth for "what can it do".
    CapabilitySet capabilities{};

    SupportLevel support_level{SupportLevel::Unknown};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};

    /// Evidence records backing this descriptor, in deterministic order.
    std::vector<EvidenceRecord> evidence;

    /// References into an external topology description. The federation stores
    /// references only; it is not a topology database.
    std::vector<std::string> topology_references;

    Timestamp observed_at{};

    /// Validate the whole descriptor: identifiers, canonical ordering, ranges,
    /// capability/evidence consistency, and support-level/label agreement.
    [[nodiscard]] Status validate() const;

    /// Deterministic digest over the structural identity fields only. Used to
    /// detect that a re-enumerated device is not the previous incarnation.
    [[nodiscard]] Sha256::digest_type structural_digest() const;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, AcceleratorDescriptor& out, bool persisted_scale);

    /// Convenience typed accessors over the capability set.
    [[nodiscard]] std::int64_t memory_total_bytes() const noexcept;
    [[nodiscard]] std::vector<std::string> isa_code_object_targets() const;
    [[nodiscard]] std::vector<std::string> numeric_formats() const;
};

/// Derive the deterministic accelerator identity from physical device, agent,
/// boot, and incarnation. Two enumerations of the same physical device by the
/// same live agent boot describe the same incarnation; any change of boot or
/// incarnation yields a different AcceleratorId.
[[nodiscard]] AcceleratorId derive_accelerator_id(const PhysicalDeviceId& physical, const AgentId& agent,
                                                  const AgentBootId& boot, DeviceGeneration generation) noexcept;

}  // namespace haf

#endif  // HAF_MODEL_ACCELERATOR_HPP
