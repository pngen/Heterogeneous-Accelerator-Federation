// Heterogeneous Accelerator Federation - authority claims.
//
// No mutation is accepted because an object identity exists. Every mutation
// must present a claim naming the exact generations the caller believes are
// current. The federation compares the claim against its own authoritative
// state and refuses stale authority explicitly, with a typed error.
//
// The claim is deliberately explicit about which fields it asserts. A CLI read
// path asserts almost nothing; an agent-scoped capability update asserts
// everything.

#ifndef HAF_FEDERATION_AUTHORITY_HPP
#define HAF_FEDERATION_AUTHORITY_HPP

#include <string>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"

namespace haf {

struct AuthorityClaim {
    FederationGeneration federation_generation{};
    CoordinatorEpoch epoch{};

    bool check_federation_generation{false};
    bool check_epoch{true};

    bool check_agent{false};
    AgentId agent{};

    bool check_agent_boot{false};
    AgentBootId agent_boot{};

    bool check_device_generation{false};
    DeviceGeneration device_generation{};

    bool check_capability_generation{false};
    CapabilityGeneration capability_generation{};

    bool check_policy_generation{false};
    PolicyGeneration policy_generation{};

    bool check_workload_revision{false};
    WorkloadRevision workload_revision{};

    /// What the claim is for. Used in error messages only; never in decisions.
    std::string purpose;
};

/// Claim asserting only that the caller knows the current coordinator epoch.
[[nodiscard]] AuthorityClaim epoch_only_claim(CoordinatorEpoch epoch, std::string purpose);

/// Full claim for an agent-scoped mutation of one accelerator.
[[nodiscard]] AuthorityClaim agent_claim(FederationGeneration generation, CoordinatorEpoch epoch,
                                         const AgentId& agent, const AgentBootId& boot,
                                         DeviceGeneration device_generation,
                                         CapabilityGeneration capability_generation,
                                         PolicyGeneration policy_generation, std::string purpose);

}  // namespace haf

#endif  // HAF_FEDERATION_AUTHORITY_HPP
