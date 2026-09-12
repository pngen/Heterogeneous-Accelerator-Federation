#include "haf/federation/authority.hpp"

#include <utility>

namespace haf {

AuthorityClaim epoch_only_claim(CoordinatorEpoch epoch, std::string purpose) {
    AuthorityClaim claim;
    claim.check_epoch = true;
    claim.epoch = epoch;
    claim.purpose = std::move(purpose);
    return claim;
}

AuthorityClaim agent_claim(FederationGeneration generation, CoordinatorEpoch epoch, const AgentId& agent,
                           const AgentBootId& boot, DeviceGeneration device_generation,
                           CapabilityGeneration capability_generation, PolicyGeneration policy_generation,
                           std::string purpose) {
    AuthorityClaim claim;
    claim.check_federation_generation = true;
    claim.federation_generation = generation;
    claim.check_epoch = true;
    claim.epoch = epoch;
    claim.check_agent = true;
    claim.agent = agent;
    claim.check_agent_boot = true;
    claim.agent_boot = boot;
    claim.check_device_generation = true;
    claim.device_generation = device_generation;
    claim.check_capability_generation = true;
    claim.capability_generation = capability_generation;
    claim.check_policy_generation = true;
    claim.policy_generation = policy_generation;
    claim.purpose = std::move(purpose);
    return claim;
}

}  // namespace haf
