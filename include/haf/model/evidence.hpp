// Heterogeneous Accelerator Federation - evidence provenance.
//
// Every federation claim is backed by an evidence record. Evidence is either
// REAL (observed from a genuine vendor runtime on genuine hardware), SYNTHETIC
// (produced by a deterministic model because the hardware path is unavailable),
// or UNSUPPORTED (the claim cannot be made at all).
//
// SYNTHETIC evidence never silently becomes REAL: the provenance class is part
// of the record, part of the capability-set digest, and part of every decision
// fingerprint. A decision taken on synthetic evidence reports itself as such.

#ifndef HAF_MODEL_EVIDENCE_HPP
#define HAF_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"

namespace haf {

enum class EvidenceProvenance : std::uint8_t {
    Real = 0,
    Synthetic = 1,
    Unsupported = 2,
};

[[nodiscard]] std::string_view to_string(EvidenceProvenance provenance) noexcept;
[[nodiscard]] bool evidence_provenance_from_wire(std::uint8_t raw, EvidenceProvenance& out) noexcept;

/// How the observation was produced.
enum class EvidenceKind : std::uint8_t {
    StaticDescriptor = 0,  ///< Immutable device properties read from the vendor runtime.
    DynamicObservation = 1,///< Capacity/health style observation that decays.
    AdapterProbe = 2,      ///< Active probe performed by an adapter.
    Declaration = 3,       ///< Explicit operator/lab declaration in a profile file.
    RecoveryRestore = 4,   ///< Reloaded from durable storage; always requires revalidation.
};

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] bool evidence_kind_from_wire(std::uint8_t raw, EvidenceKind& out) noexcept;

/// Provenance record for one observation of one accelerator.
struct EvidenceRecord {
    EvidenceId id{};
    EvidenceGeneration generation{};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    EvidenceKind kind{EvidenceKind::StaticDescriptor};

    /// Adapter identifier, e.g. "cuda-runtime", "synthetic-rocm-like".
    std::string adapter;
    /// Human-readable source description, e.g. "cudaGetDeviceProperties(cudaDevAttr*)".
    std::string source;
    /// Version of the adapter that produced the observation.
    std::string adapter_version;

    AcceleratorId subject{};
    PhysicalDeviceId physical_device{};
    CapabilityGeneration capability_generation{};

    /// Content digest of the capability payload this evidence describes.
    Sha256::digest_type payload_digest{};

    RuntimeVersion runtime_version{};
    RuntimeVersion driver_version{};

    /// Wall-clock instant of observation. Persisted, used for display.
    Timestamp observed_at{};
    /// Observation sequence within the producing process. Monotonic.
    std::uint64_t sequence{0};

    /// Freshness budget in nanoseconds. Zero means "no freshness requirement".
    std::uint64_t freshness_budget_nanos{0};

    /// True for observations that must be refreshed after a process restart.
    bool dynamic{false};

    /// Set when the record was loaded from durable storage. A record with this
    /// flag set has no live monotonic reference and therefore cannot be treated
    /// as current until it is revalidated by a live adapter observation.
    bool requires_revalidation{true};

    /// Process-local monotonic observation instant. Deliberately NOT persisted:
    /// after a restart the marker is reset to the clock epoch and freshness can
    /// only be re-established by a live observation.
    MonotonicTime observed_monotonic_marker{};

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, EvidenceRecord& out);
    [[nodiscard]] Status validate() const;

    /// Deterministic digest over the provenance-bearing fields.
    [[nodiscard]] Sha256::digest_type digest() const;

    /// True when the record carries usable live evidence at the given
    /// monotonic instant. Any record awaiting revalidation is never fresh.
    [[nodiscard]] bool is_fresh(MonotonicTime now, std::uint64_t effective_budget_nanos) const noexcept;
};

/// Build a fully populated evidence record, deriving its identity from content.
[[nodiscard]] EvidenceRecord make_evidence(std::string adapter, std::string source, std::string adapter_version,
                                           AcceleratorId subject, PhysicalDeviceId physical_device,
                                           EvidenceProvenance provenance, EvidenceKind kind,
                                           Sha256::digest_type payload_digest, RuntimeVersion runtime_version,
                                           RuntimeVersion driver_version, std::uint64_t freshness_budget_nanos,
                                           bool dynamic);

}  // namespace haf

#endif  // HAF_MODEL_EVIDENCE_HPP
