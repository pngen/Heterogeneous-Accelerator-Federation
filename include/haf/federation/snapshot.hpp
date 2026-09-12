// Heterogeneous Accelerator Federation - authoritative state snapshot.
//
// A snapshot is a canonical, content-addressed, immutable view of federation
// state. Deterministic rendering, auditing, and persistence all operate on it,
// so inspection can never observe a partially mutated federation.

#ifndef HAF_FEDERATION_SNAPSHOT_HPP
#define HAF_FEDERATION_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/time.hpp"
#include "haf/model/accelerator.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/membership.hpp"
#include "haf/model/migration.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/workload.hpp"

namespace haf {

/// Persistence format identifier. Bumping it requires a migration path.
inline constexpr std::uint32_t kSnapshotFormatVersion = 1;

struct FederationSnapshot {
    FederationId federation{};
    FederationGeneration generation{};
    CoordinatorEpoch epoch{};
    std::string name;

    PolicyId policy{};
    PolicyGeneration policy_generation{};
    FederationPolicy policy_data{};

    /// Sorted by accelerator identity.
    std::vector<MemberRecord> members;
    /// Sorted by accelerator identity. One descriptor per member.
    std::vector<AcceleratorDescriptor> descriptors;
    /// Sorted by workload class identity.
    std::vector<WorkloadProfile> workloads;
    /// Sorted by decision identity.
    std::vector<CompatibilityDecision> decisions;
    /// Sorted by plan identity.
    std::vector<MigrationPlan> plans;

    Timestamp created_at{};

    /// Deterministic digest over the canonical snapshot encoding. Two
    /// federations with identical authoritative state produce identical
    /// digests, independent of insertion order or platform.
    [[nodiscard]] Sha256::digest_type digest() const;
    [[nodiscard]] std::string digest_hex() const;

    /// Canonicalize ordering so that the digest is stable.
    void canonicalize();

    [[nodiscard]] const MemberRecord* find_member(const AcceleratorId& id) const;
    [[nodiscard]] const AcceleratorDescriptor* find_descriptor(const AcceleratorId& id) const;
    [[nodiscard]] const WorkloadProfile* find_workload(const WorkloadClassId& id) const;
};

/// Encode a snapshot into its canonical persisted payload.
[[nodiscard]] ByteBuffer encode_snapshot(const FederationSnapshot& snapshot);

/// Decode and fully validate a snapshot payload. Every length, count, enum and
/// cross reference is checked before the value is returned.
[[nodiscard]] Result<FederationSnapshot> decode_snapshot(const ByteBuffer& payload);

}  // namespace haf

#endif  // HAF_FEDERATION_SNAPSHOT_HPP
