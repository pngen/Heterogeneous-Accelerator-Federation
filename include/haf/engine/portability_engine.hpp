// Heterogeneous Accelerator Federation - portability classification.
//
// Portability answers exactly one question: what must happen before a workload
// that is valid on accelerator A is valid on accelerator B?
//
// The classifier is evidence-driven and fails closed. If any input it needs to
// prove a stronger class is UNKNOWN, it returns UNKNOWN rather than the weaker
// class it could otherwise justify, because "we do not know" must never be
// silently downgraded into a concrete claim.

#ifndef HAF_ENGINE_PORTABILITY_ENGINE_HPP
#define HAF_ENGINE_PORTABILITY_ENGINE_HPP

#include <string>
#include <string_view>
#include <vector>

#include "haf/model/capability.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/workload.hpp"

namespace haf {

/// Everything the classifier is allowed to look at. Pointers must stay valid
/// for the duration of the call; the classifier never stores them.
struct PortabilityRequest {
    const CapabilitySet* source_capabilities{nullptr};
    const CapabilitySet* destination_capabilities{nullptr};
    std::string source_vendor;
    std::string destination_vendor;
    const WorkloadProfile* workload{nullptr};
    /// When false, a cross-vendor live state transfer claim is refused outright.
    bool policy_allows_cross_vendor_migration{false};
};

struct PortabilityResult {
    PortabilityClass value{PortabilityClass::Unknown};
    std::vector<CompatibilityReason> reasons;
};

/// Classify portability from source to destination.
[[nodiscard]] PortabilityResult classify_portability(const PortabilityRequest& request);

/// True when the capability is a decay-prone observation. Dynamic capabilities
/// participate in freshness policy; static ones do not.
[[nodiscard]] bool is_dynamic_capability(std::string_view capability_name) noexcept;

/// Deterministic ordering for explanation lists.
void sort_reasons(std::vector<CompatibilityReason>& reasons);

}  // namespace haf

#endif  // HAF_ENGINE_PORTABILITY_ENGINE_HPP
