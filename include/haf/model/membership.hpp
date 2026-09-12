// Heterogeneous Accelerator Federation - membership records.
//
// A member record is the federation's authoritative statement about one
// accelerator. It binds every generation that the statement depends on. When
// any of them changes, the statement is no longer current and the member must
// be revalidated rather than silently trusted.

#ifndef HAF_MODEL_MEMBERSHIP_HPP
#define HAF_MODEL_MEMBERSHIP_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/compatibility.hpp"

namespace haf {

struct MemberRecord {
    FederationId federation{};
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};

    AgentId agent{};
    AgentBootId agent_boot{};
    AgentGeneration agent_generation{};

    AcceleratorId accelerator{};
    PhysicalDeviceId physical_device{};
    DeviceGeneration device_generation{};

    CapabilityGeneration capability_generation{};
    EvidenceGeneration evidence_generation{};
    PolicyGeneration policy_generation{};

    MemberState state{MemberState::Discovered};
    /// Advances on every accepted mutation of this member.
    DecisionGeneration state_generation{};

    SupportLevel support_level{SupportLevel::Unknown};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};

    Timestamp admitted_at{};
    Timestamp updated_at{};

    /// Deterministic audit trail of state changes.
    std::vector<std::string> history;

    [[nodiscard]] bool is_live() const noexcept;
    [[nodiscard]] bool accepts_new_work() const noexcept;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, MemberRecord& out, bool persisted_scale);
};

}  // namespace haf

#endif  // HAF_MODEL_MEMBERSHIP_HPP
