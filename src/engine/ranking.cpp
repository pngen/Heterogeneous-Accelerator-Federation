#include "haf/engine/ranking.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/limits.hpp"

namespace haf {
namespace {

[[nodiscard]] std::int64_t score_candidate(const RankingCandidate& candidate, const RankingWeights& weights,
                                           std::vector<std::string>& rationale) {
    std::int64_t score = 0;
    const PortabilityClass portability = candidate.decision.portability;
    if (portability == PortabilityClass::Native) {
        score += weights.native_portability_bonus;
        rationale.push_back("portability=native +" + std::to_string(weights.native_portability_bonus));
    } else if (portability == PortabilityClass::LiveMigrationSupported) {
        score += weights.native_portability_bonus / 2;
        rationale.push_back("portability=live_migration +" + std::to_string(weights.native_portability_bonus / 2));
    }
    const std::uint8_t steps = portability_transformation_steps(portability);
    if (steps != 0 && steps != 255) {
        const std::int64_t penalty = static_cast<std::int64_t>(steps) * weights.transformation_step_penalty;
        score -= penalty;
        rationale.push_back("transformation_steps=" + std::to_string(steps) + " -" + std::to_string(penalty));
    }
    if (candidate.migration_cost != 0) {
        const std::int64_t penalty =
            static_cast<std::int64_t>(candidate.migration_cost) * weights.migration_cost_penalty;
        score -= penalty;
        rationale.push_back("migration_cost=" + std::to_string(candidate.migration_cost) + " -" +
                            std::to_string(penalty));
    }
    if (candidate.decision.provenance == EvidenceProvenance::Synthetic) {
        score -= weights.synthetic_evidence_penalty;
        rationale.push_back("evidence=synthetic -" + std::to_string(weights.synthetic_evidence_penalty));
    } else if (candidate.decision.provenance == EvidenceProvenance::Unsupported) {
        score -= weights.synthetic_evidence_penalty * 2;
        rationale.push_back("evidence=unsupported -" + std::to_string(weights.synthetic_evidence_penalty * 2));
    }
    if (weights.use_topology_distance && candidate.topology_distance != 0) {
        const std::int64_t penalty = static_cast<std::int64_t>(candidate.topology_distance) *
                                     weights.topology_distance_penalty;
        score -= penalty;
        rationale.push_back("topology_distance=" + std::to_string(candidate.topology_distance) + " -" +
                            std::to_string(penalty));
    }
    if (weights.use_free_memory && candidate.free_memory_bytes != 0) {
        const std::int64_t gib = static_cast<std::int64_t>(candidate.free_memory_bytes / (1024ULL * 1024ULL * 1024ULL));
        const std::int64_t bonus = gib * weights.free_memory_gib_bonus;
        score += bonus;
        rationale.push_back("free_memory_gib=" + std::to_string(gib) + " +" + std::to_string(bonus));
    }
    const std::int64_t decision_score = candidate.decision.score;
    score += decision_score;
    if (decision_score != 0) {
        rationale.push_back("compatibility_score " + std::string(decision_score >= 0 ? "+" : "") +
                            std::to_string(decision_score));
    }
    return score;
}

}  // namespace

Result<RankingResult> rank_candidates(const std::vector<RankingCandidate>& candidates,
                                      const RankingWeights& weights) {
    if (candidates.size() > Limits::kMaxRankingCandidates) {
        return Status(ErrorCode::BoundsExceeded, "ranking exceeds the permitted number of candidates");
    }
    RankingResult result;
    for (const RankingCandidate& candidate : candidates) {
        RankedCandidate entry;
        entry.accelerator = candidate.accelerator;
        entry.outcome = candidate.decision.outcome;
        entry.portability = candidate.decision.portability;
        entry.provenance = candidate.decision.provenance;
        if (candidate.decision.outcome != CompatibilityOutcome::Eligible) {
            entry.rationale.push_back("excluded: outcome is " + std::string(to_string(candidate.decision.outcome)));
            result.excluded.push_back(std::move(entry));
            continue;
        }
        entry.rank_score = score_candidate(candidate, weights, entry.rationale);
        result.ranked.push_back(std::move(entry));
    }

    std::stable_sort(result.ranked.begin(), result.ranked.end(),
                     [](const RankedCandidate& a, const RankedCandidate& b) {
                         if (a.rank_score != b.rank_score) {
                             return a.rank_score > b.rank_score;
                         }
                         if (a.portability != b.portability) {
                             return portability_strength(a.portability) > portability_strength(b.portability);
                         }
                         // Stable identity tie-break. Never a pointer, never an
                         // iteration order, never a wall-clock value.
                         return a.accelerator < b.accelerator;
                     });
    for (std::size_t i = 0; i < result.ranked.size(); ++i) {
        result.ranked[i].rank = i + 1;
    }
    std::stable_sort(result.excluded.begin(), result.excluded.end(),
                     [](const RankedCandidate& a, const RankedCandidate& b) { return a.accelerator < b.accelerator; });
    return result;
}

}  // namespace haf
