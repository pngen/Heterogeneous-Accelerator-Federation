// Shared helpers for the examples.

#ifndef HAF_EXAMPLE_SUPPORT_HPP
#define HAF_EXAMPLE_SUPPORT_HPP

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "haf/adapters/rocm_synthetic.hpp"
#include "haf/adapters/synthetic.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/federation/federation.hpp"

namespace haf::examples {

inline void line(const std::string& text) { std::cout << text << std::endl; }

inline adapters::AdapterContext context(std::uint64_t seed, const std::string& node) {
    adapters::AdapterContext result;
    IdGenerator generator = make_deterministic_id_generator(seed);
    result.agent = generator.next_id<AgentIdTag>();
    result.agent_boot = generator.next_id<AgentBootIdTag>();
    result.node_token = node;
    result.node = node_id_from_token(node);
    result.seed = seed;
    return result;
}

inline AcceleratorDescriptor device(const adapters::DeviceProfile& profile, std::uint64_t seed,
                                    const std::string& adapter, std::int64_t index) {
    const Result<AcceleratorDescriptor> built =
        adapters::build_descriptor(profile, context(seed, "example-node"), adapter, index);
    if (!built.ok()) {
        throw std::runtime_error("example device construction failed: " + built.status().describe());
    }
    return *built;
}

/// Build a workload profile with a content-derived identity and revision.
inline WorkloadProfile make_profile(const std::string& name, std::vector<CapabilityRequirement> requirements) {
    WorkloadProfile profile;
    profile.name = name;
    profile.class_id = workload_class_id_from_token(name);
    profile.requirements = std::move(requirements);
    profile.requirement_id = derive_requirement_id(profile);
    profile.revision = derive_workload_revision(profile);
    return profile;
}

/// Convenience wrappers that abort the example when a builder fails.
inline CapabilityRequirement require(std::string_view capability, RequirementStrength strength) {
    const Result<CapabilityRequirement> built = require_present(capability, strength);
    if (!built.ok()) {
        throw std::runtime_error(built.status().describe());
    }
    return *built;
}

inline CapabilityRequirement require_vendor(const std::string& vendor) {
    const Result<CapabilityRequirement> built =
        require_equals("vendor.id", vendor, RequirementStrength::Hard);
    if (!built.ok()) {
        throw std::runtime_error(built.status().describe());
    }
    return *built;
}

inline CapabilityRequirement require_isa(std::vector<std::string> tokens) {
    const Result<CapabilityRequirement> built =
        require_tokens_superset("isa.code_object_targets", std::move(tokens), RequirementStrength::Hard);
    if (!built.ok()) {
        throw std::runtime_error(built.status().describe());
    }
    return *built;
}

inline CapabilityRequirement require_minimum(std::string_view capability, std::int64_t value) {
    const Result<CapabilityRequirement> built =
        require_at_least(capability, value, RequirementStrength::Hard);
    if (!built.ok()) {
        throw std::runtime_error(built.status().describe());
    }
    return *built;
}

inline Result<MemberRecord> join(Federation& federation, const AcceleratorDescriptor& descriptor) {
    const AuthorityClaim claim = federation.epoch_claim("example");
    Result<MemberRecord> observed = federation.observe(descriptor, claim);
    if (!observed.ok()) {
        return observed.status();
    }
    if (observed->state == MemberState::Observed) {
        Result<MemberRecord> admitted = federation.admit(descriptor.id, claim);
        if (!admitted.ok()) {
            return admitted.status();
        }
        observed = admitted;
    }
    if (observed->state == MemberState::Admitted) {
        Result<MemberRecord> activated = federation.activate(descriptor.id, claim);
        if (!activated.ok()) {
            return activated.status();
        }
        observed = activated;
    }
    return *observed;
}

}  // namespace haf::examples

#endif  // HAF_EXAMPLE_SUPPORT_HPP
