#include "profiles.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "haf/model/taxonomy.hpp"

namespace haf::test {
namespace {

/// Fail fast: a builder that cannot construct its object is a test defect.
template <class T>
[[nodiscard]] T unwrap(Result<T> result) {
    if (!result.ok()) {
        throw std::runtime_error("test builder failed: " + result.status().describe());
    }
    return *result;
}

[[nodiscard]] CapabilityRequirement unwrap_requirement(Result<CapabilityRequirement> result) {
    return unwrap<CapabilityRequirement>(std::move(result));
}

}  // namespace

adapters::AdapterContext make_context(std::uint64_t seed, const std::string& node_token) {
    adapters::AdapterContext context;
    IdGenerator generator = make_deterministic_id_generator(seed);
    context.agent = generator.next_id<AgentIdTag>();
    context.agent_boot = generator.next_id<AgentBootIdTag>();
    context.node_token = node_token;
    context.node = node_id_from_token(node_token);
    context.seed = seed;
    return context;
}

AcceleratorDescriptor make_device(const adapters::DeviceProfile& profile, std::uint64_t seed,
                                  const std::string& adapter_name, std::int64_t device_index) {
    const adapters::AdapterContext context = make_context(seed);
    return unwrap(adapters::build_descriptor(profile, context, adapter_name, device_index));
}

AcceleratorDescriptor make_cuda_like(std::uint64_t seed) {
    return make_device(adapters::cuda_class_profile(), seed, "test-cuda-like", 0);
}

AcceleratorDescriptor make_rocm_like(std::uint64_t seed) {
    return make_device(adapters::rocm_class_profile(), seed, "test-rocm-like", 0);
}

AcceleratorDescriptor make_intel_like(std::uint64_t seed) {
    return make_device(adapters::intel_class_profile(), seed, "test-level-zero-like", 0);
}

WorkloadProfile make_workload(const std::string& name, std::vector<CapabilityRequirement> requirements) {
    WorkloadProfile profile;
    profile.name = name;
    profile.class_id = workload_class_id_from_token(name);
    profile.requirements = std::move(requirements);
    profile.requirement_id = derive_requirement_id(profile);
    profile.revision = derive_workload_revision(profile);
    return profile;
}

CapabilityRequirement hard_present(std::string_view capability) {
    return unwrap_requirement(require_present(capability, RequirementStrength::Hard));
}

CapabilityRequirement hard_absent(std::string_view capability) {
    return unwrap_requirement(require_absent(capability, RequirementStrength::Hard));
}

CapabilityRequirement hard_equals(std::string_view capability, std::string_view payload) {
    return unwrap_requirement(require_equals(capability, payload, RequirementStrength::Hard));
}

CapabilityRequirement hard_at_least(std::string_view capability, std::int64_t value) {
    return unwrap_requirement(require_at_least(capability, value, RequirementStrength::Hard));
}

CapabilityRequirement hard_tokens(std::string_view capability, std::vector<std::string> tokens) {
    return unwrap_requirement(require_tokens_superset(capability, std::move(tokens), RequirementStrength::Hard));
}

CapabilityRequirement hard_version(std::string_view capability, std::string_view range) {
    return unwrap_requirement(require_version_range(capability, range, RequirementStrength::Hard));
}

CapabilityRequirement soft_present(std::string_view capability, double weight) {
    CapabilityRequirement requirement = unwrap_requirement(require_present(capability, RequirementStrength::Soft));
    requirement.weight = weight;
    return requirement;
}

FederationPolicy permissive_policy() { return FederationPolicy::permissive_default(); }

FederationPolicy strict_real_evidence_policy() {
    FederationPolicy policy = FederationPolicy::permissive_default();
    policy.name = "strict-real-evidence";
    policy.synthetic_evidence_policy = SyntheticEvidencePolicy::Reject;
    policy.require_real_evidence_for_active = true;
    policy.refresh_identity();
    return policy;
}

Result<std::unique_ptr<Federation>> open_memory_federation(std::uint64_t seed, const FederationPolicy& policy) {
    FederationConfig config;
    config.name = "test-federation";
    config.policy = policy;
    config.id_seed = seed;
    config.persist = false;
    return Federation::open(config);
}

Result<MemberRecord> join(Federation& federation, const AcceleratorDescriptor& descriptor) {
    const AuthorityClaim observe_claim = federation.epoch_claim("test observe");
    Result<MemberRecord> observed = federation.observe(descriptor, observe_claim);
    if (!observed.ok()) {
        return observed.status();
    }
    const AuthorityClaim transition_claim = federation.epoch_claim("test transition");
    if (observed->state == MemberState::Observed) {
        Result<MemberRecord> admitted = federation.admit(descriptor.id, transition_claim);
        if (!admitted.ok()) {
            return admitted.status();
        }
        observed = admitted;
    }
    if (observed->state == MemberState::Admitted || observed->state == MemberState::Degraded) {
        Result<MemberRecord> activated = federation.activate(descriptor.id, transition_claim);
        if (!activated.ok()) {
            return activated.status();
        }
        observed = activated;
    }
    return *observed;
}

std::uint64_t SeededRandom::next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t SeededRandom::below(std::uint64_t bound) noexcept {
    if (bound == 0) {
        return 0;
    }
    return next() % bound;
}

bool SeededRandom::chance(unsigned percent) noexcept { return below(100) < percent; }

}  // namespace haf::test
