// Heterogeneous Accelerator Federation - deterministic ranking.
//
// Ranking exists only to demonstrate deterministic federation decisions and to
// order migration destinations. It is explicitly subordinate to compatibility:
//
//   * eligibility is evaluated first;
//   * ineligible and UNKNOWN candidates are never ranked alongside eligible
//     ones - ranking returns only eligible candidates and reports the rest;
//   * ordering uses integer comparisons only, with a stable identity
//     tie-break, so the result never depends on container iteration order,
//     pointer values, thread timing, or vendor enumeration order.
//
// The federation does not decide when work runs. It only answers "which of
// these eligible accelerators is the better federation-level destination, and
// why".

#ifndef HAF_ENGINE_RANKING_HPP
#define HAF_ENGINE_RANKING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/migration.hpp"

namespace haf {

/// Explicit, inspectable ranking weights. Every dimension is optional and
/// disabled by default so that a caller must opt in to each preference.
struct RankingWeights {
    /// Higher is better. Applied when portability is NATIVE.
    std::int32_t native_portability_bonus{1000};
    /// Applied per transformation step implied by the portability class.
    std::int32_t transformation_step_penalty{250};
    /// Applied per unit of migration plan estimated cost.
    std::int32_t migration_cost_penalty{1};
    /// Applied when the evidence provenance is SYNTHETIC.
    std::int32_t synthetic_evidence_penalty{500};
    /// Applied per unit of topology distance supplied by the caller.
    std::int32_t topology_distance_penalty{100};
    /// Applied per GiB of fresh free memory reported by the candidate.
    std::int32_t free_memory_gib_bonus{10};
    /// When false, the free-memory dimension is ignored entirely, which keeps
    /// ranking independent of decay-prone observations.
    bool use_free_memory{false};
    /// When false, the topology dimension is ignored.
    bool use_topology_distance{false};
};

struct RankingCandidate {
    AcceleratorId accelerator{};
    CompatibilityDecision decision{};
    /// Caller-supplied locality measure. 0 means "same place as the workload".
    std::uint32_t topology_distance{0};
    /// Fresh free memory, when the caller has it.
    std::uint64_t free_memory_bytes{0};
    /// Estimated cost from a migration plan, when one exists.
    std::uint32_t migration_cost{0};
};

struct RankedCandidate {
    std::size_t rank{0};
    AcceleratorId accelerator{};
    std::int64_t rank_score{0};
    CompatibilityOutcome outcome{CompatibilityOutcome::Unknown};
    PortabilityClass portability{PortabilityClass::Unknown};
    MigrationOutcome migration_outcome{MigrationOutcome::Unknown};
    EvidenceProvenance provenance{EvidenceProvenance::Unsupported};
    /// Deterministic explanation of the ordering, one line per dimension.
    std::vector<std::string> rationale;
};

struct RankingResult {
    /// Eligible candidates only, best first.
    std::vector<RankedCandidate> ranked;
    /// Candidates excluded before ranking, with the reason they were excluded.
    std::vector<RankedCandidate> excluded;
};

/// Rank candidates. Throws nothing, allocates nothing per comparison, and is
/// a pure function of its inputs.
[[nodiscard]] Result<RankingResult> rank_candidates(const std::vector<RankingCandidate>& candidates,
                                                    const RankingWeights& weights);

}  // namespace haf

#endif  // HAF_ENGINE_RANKING_HPP
