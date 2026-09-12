// Heterogeneous Accelerator Federation - vendor adapter surface.
//
// Adapters are the only place vendor knowledge lives. The federation core never
// sees a vendor SDK type, a vendor header, or a vendor-specific assumption.
//
// Each adapter reports observations plus the provenance of those observations.
// An adapter that cannot observe real hardware must say so: it may return
// SYNTHETIC observations, or it may return UNSUPPORTED and no devices. It must
// never present a modelled device as real.

#ifndef HAF_ADAPTERS_ADAPTER_HPP
#define HAF_ADAPTERS_ADAPTER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/model/accelerator.hpp"

namespace haf::adapters {

/// Everything an adapter needs in order to produce a descriptor.
struct AdapterContext {
    AgentId agent{};
    AgentBootId agent_boot{};
    NodeId node{};
    std::string node_token;
    /// Seed for deterministic synthetic models. Zero means "use the adapter's
    /// own default", never OS entropy inside an adapter.
    std::uint64_t seed{0};
};

struct AdapterObservation {
    /// One descriptor per observed accelerator, in deterministic order.
    std::vector<AcceleratorDescriptor> devices;
    /// Adapter identifier, e.g. "cuda-runtime".
    std::string adapter;
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    SupportLevel support_level{SupportLevel::Unsupported};
    /// Human-readable explanation of what the adapter was able to observe.
    std::string detail;
};

class AcceleratorAdapter {
public:
    AcceleratorAdapter() = default;
    virtual ~AcceleratorAdapter() = default;

    AcceleratorAdapter(const AcceleratorAdapter&) = delete;
    AcceleratorAdapter& operator=(const AcceleratorAdapter&) = delete;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual SupportLevel support_level() const = 0;
    [[nodiscard]] virtual EvidenceProvenance provenance() const = 0;

    /// Observe accelerators. Must be deterministic for a fixed context and a
    /// fixed machine state, and must never throw.
    [[nodiscard]] virtual Result<AdapterObservation> observe(const AdapterContext& context) const = 0;

    /// Optional: run an end-to-end proof on one observed device. Returns a typed
    /// error when the adapter cannot prove execution.
    struct ProofResult {
        bool executed{false};
        bool verified{false};
        std::string detail;
        std::uint64_t bytes_transferred{0};
    };
    [[nodiscard]] virtual Result<ProofResult> prove(int device_index) const;
};

}  // namespace haf::adapters

#endif  // HAF_ADAPTERS_ADAPTER_HPP
