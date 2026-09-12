// Builders shared by every suite: deterministic descriptors, workloads, and
// policies. Keeping them in one place keeps the suites comparable.

#ifndef HAF_TEST_PROFILES_HPP
#define HAF_TEST_PROFILES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/adapters/synthetic.hpp"
#include "haf/core/idgen.hpp"
#include "haf/federation/federation.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/workload.hpp"

namespace haf::test {

/// Deterministic adapter context used by descriptor builders.
[[nodiscard]] adapters::AdapterContext make_context(std::uint64_t seed, const std::string& node_token = "test-node");

/// Build a descriptor from a profile with a fixed, reproducible identity.
[[nodiscard]] AcceleratorDescriptor make_device(const adapters::DeviceProfile& profile, std::uint64_t seed,
                                                const std::string& adapter_name = "test-adapter",
                                                std::int64_t device_index = 0);

/// Acknowledged CUDA-class device (deterministic).
[[nodiscard]] AcceleratorDescriptor make_cuda_like(std::uint64_t seed);
/// Modelled ROCm-class device (deterministic).
[[nodiscard]] AcceleratorDescriptor make_rocm_like(std::uint64_t seed);
/// Modelled Intel-class device with a different runtime and feature surface.
[[nodiscard]] AcceleratorDescriptor make_intel_like(std::uint64_t seed);

/// Workload builders.
[[nodiscard]] WorkloadProfile make_workload(const std::string& name,
                                            std::vector<CapabilityRequirement> requirements);

/// Requirement builders that fail loudly in tests when construction fails.
[[nodiscard]] CapabilityRequirement hard_present(std::string_view capability);
[[nodiscard]] CapabilityRequirement hard_absent(std::string_view capability);
[[nodiscard]] CapabilityRequirement hard_equals(std::string_view capability, std::string_view payload);
[[nodiscard]] CapabilityRequirement hard_at_least(std::string_view capability, std::int64_t value);
[[nodiscard]] CapabilityRequirement hard_tokens(std::string_view capability, std::vector<std::string> tokens);
[[nodiscard]] CapabilityRequirement hard_version(std::string_view capability, std::string_view range);
[[nodiscard]] CapabilityRequirement soft_present(std::string_view capability, double weight);

/// Policy builders.
[[nodiscard]] FederationPolicy permissive_policy();
[[nodiscard]] FederationPolicy strict_real_evidence_policy();

/// A federation opened in memory with a deterministic identity.
[[nodiscard]] Result<std::unique_ptr<Federation>> open_memory_federation(std::uint64_t seed,
                                                                        const FederationPolicy& policy);

/// Register and activate a device in one step, returning the member record.
[[nodiscard]] Result<MemberRecord> join(Federation& federation, const AcceleratorDescriptor& descriptor);

/// Pseudo-random generator with a recorded seed, used by property tests.
class SeededRandom {
public:
    explicit SeededRandom(std::uint64_t seed) noexcept : seed_(seed), state_(seed ^ 0x9E3779B97F4A7C15ULL) {}

    [[nodiscard]] std::uint64_t next() noexcept;
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept;
    [[nodiscard]] bool chance(unsigned percent) noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t seed_;
    std::uint64_t state_;
};

}  // namespace haf::test

#endif  // HAF_TEST_PROFILES_HPP
