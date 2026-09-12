// Heterogeneous Accelerator Federation - resource discipline.
//
// Every length, count, and depth that can be influenced by a peer, a file, or
// an untrusted caller is bounded here. Network limits and persistence limits
// are deliberately separate: a durable federation may legitimately exceed the
// number of records that fit in a single control-plane message.

#ifndef HAF_CORE_LIMITS_HPP
#define HAF_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace haf {

struct Limits {
    // --- Text -------------------------------------------------------------
    static constexpr std::size_t kMaxNameBytes = 256;
    static constexpr std::size_t kMaxTokenBytes = 128;
    static constexpr std::size_t kMaxStringBytes = 4096;
    static constexpr std::size_t kMaxDescriptionBytes = 8192;
    static constexpr std::size_t kMaxRationaleBytes = 1024;

    // --- Capability model -------------------------------------------------
    static constexpr std::size_t kMaxCapabilitiesPerSet = 4096;
    static constexpr std::size_t kMaxTokensPerCapability = 512;

    // --- Workload requirements -------------------------------------------
    static constexpr std::size_t kMaxRequirementsPerWorkload = 4096;
    static constexpr std::size_t kMaxPolicyDimensions = 1024;
    static constexpr std::size_t kMaxTopologyReferences = 64;

    // --- Control plane (per message) --------------------------------------
    static constexpr std::size_t kMaxFrameBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxAdvertisementsPerMessage = 64;
    static constexpr std::size_t kMaxMembersPerResponse = 4096;
    /// Library-scale bounds. These are deliberately separate from the message
    /// limits above: an in-process fleet evaluation or ranking is not a control
    /// plane response and must not inherit a wire limit.
    static constexpr std::size_t kMaxFleetEvaluationTargets = 1'000'000;
    static constexpr std::size_t kMaxRankingCandidates = 1'000'000;
    static constexpr std::size_t kMaxConnections = 1024;
    static constexpr std::size_t kMaxQueuedMessages = 4096;
    static constexpr std::size_t kMaxMatrixCells = 1'000'000;
    static constexpr std::size_t kMaxMatrixAccelerators = 100'000;
    static constexpr std::size_t kMaxMatrixWorkloads = 100'000;

    // --- Persistence (durable scale, not message scale) -------------------
    static constexpr std::size_t kMaxPersistedMembers = 1'000'000;
    static constexpr std::size_t kMaxPersistedDecisions = 1'000'000;
    static constexpr std::size_t kMaxPersistedPlans = 1'000'000;
    static constexpr std::size_t kMaxPersistedEvidence = 4'000'000;
    static constexpr std::size_t kMaxPersistedWorkloads = 1'000'000;
    static constexpr std::size_t kMaxPersistedPolicies = 4096;
    static constexpr std::uint64_t kMaxStorePayloadBytes = 512ULL * 1024ULL * 1024ULL;

    // --- Runtime ----------------------------------------------------------
    static constexpr std::size_t kMaxAgents = 65536;
    static constexpr std::size_t kMaxThreads = 256;

    // --- Time -------------------------------------------------------------
    static constexpr std::uint64_t kMaxEvidenceAgeNanos = 3'600'000'000'000ULL;
    static constexpr std::uint64_t kDefaultEvidenceFreshnessNanos = 300'000'000'000ULL;
};

}  // namespace haf

#endif  // HAF_CORE_LIMITS_HPP
