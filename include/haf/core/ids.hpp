// Heterogeneous Accelerator Federation - strongly typed identities.
//
// Every identity in the federation is a distinct C++ type wrapping an opaque
// 128-bit value. Two identities of different semantic kinds can never be
// compared, assigned, or accidentally interchanged.
//
// Identity values are never reused: a fresh incarnation always receives a
// freshly generated value, so a stale authority token that names an old
// incarnation can never be satisfied by a new one.

#ifndef HAF_CORE_IDS_HPP
#define HAF_CORE_IDS_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace haf {

/// Opaque 128-bit identity value.
class Id128 {
public:
    using bytes_type = std::array<std::uint8_t, 16>;
    static constexpr std::size_t kHexLength = 32;

    constexpr Id128() noexcept = default;
    constexpr explicit Id128(bytes_type bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] static constexpr Id128 nil() noexcept { return Id128{}; }

    /// Build an identity from at most 16 bytes, zero padded on the right.
    [[nodiscard]] static Id128 from_bytes(const std::uint8_t* data, std::size_t size) noexcept;

    /// Parse exactly 32 lowercase or uppercase hex digits. Returns nullopt on
    /// any deviation (wrong length, non-hex character, empty input).
    [[nodiscard]] static std::optional<Id128> parse(std::string_view hex) noexcept;

    [[nodiscard]] std::string to_hex() const;

    [[nodiscard]] constexpr const bytes_type& bytes() const noexcept { return bytes_; }

    [[nodiscard]] constexpr bool is_nil() const noexcept {
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            if (bytes_[i] != 0) {
                return false;
            }
        }
        return true;
    }

    friend constexpr bool operator==(const Id128&, const Id128&) noexcept = default;
    friend constexpr auto operator<=>(const Id128&, const Id128&) noexcept = default;

private:
    bytes_type bytes_{};
};

/// Compile-time tag used to give each identity kind its own C++ type.
template <class Tag>
class StrongId {
public:
    using tag_type = Tag;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Id128 value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr StrongId from_raw(Id128 value) noexcept { return StrongId(value); }

    [[nodiscard]] static std::optional<StrongId> parse(std::string_view hex) noexcept {
        const std::optional<Id128> parsed = Id128::parse(hex);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return StrongId(*parsed);
    }

    [[nodiscard]] constexpr const Id128& raw() const noexcept { return value_; }
    [[nodiscard]] std::string to_string() const { return value_.to_hex(); }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return value_.is_nil(); }

    friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
    friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

private:
    Id128 value_{};
};

/// Monotonic generation counter bound to a semantic domain.
template <class Tag>
class Generation {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr Generation() noexcept = default;
    constexpr explicit Generation(value_type value) noexcept : value_(value) {}

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_initial() const noexcept { return value_ == 0; }

    /// Next generation in the same domain. Saturating; never wraps to zero,
    /// because a wrapped generation would resurrect stale authority.
    [[nodiscard]] constexpr Generation next() const noexcept {
        if (value_ == kMaxValue) {
            return *this;
        }
        return Generation(value_ + 1);
    }

    static constexpr value_type kMaxValue = 0xFFFF'FFFF'FFFF'FFFFULL;

    friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;
    friend constexpr auto operator<=>(const Generation&, const Generation&) noexcept = default;

private:
    value_type value_{0};
};

// --- Identity tags ---------------------------------------------------------

struct FederationIdTag {};
struct AgentIdTag {};
struct AgentBootIdTag {};
struct NodeIdTag {};
struct AcceleratorIdTag {};
struct PhysicalDeviceIdTag {};
struct VendorIdTag {};
struct ArchitectureIdTag {};
struct CapabilitySetIdTag {};
struct EvidenceIdTag {};
struct WorkloadClassIdTag {};
struct WorkloadRequirementIdTag {};
struct CompatibilityProfileIdTag {};
struct CompatibilityDecisionIdTag {};
struct PortabilityClassIdTag {};
struct MigrationPlanIdTag {};
struct ReservationIdTag {};
struct PolicyIdTag {};
struct DecisionIdTag {};
struct SnapshotIdTag {};
struct EventIdTag {};
struct SessionIdTag {};

// --- Generation tags -------------------------------------------------------

struct FederationGenerationTag {};
struct CoordinatorEpochTag {};
struct DeviceGenerationTag {};
struct CapabilityGenerationTag {};
struct EvidenceGenerationTag {};
struct PolicyGenerationTag {};
struct DecisionGenerationTag {};
struct MigrationGenerationTag {};
struct WorkloadRevisionTag {};
struct AgentGenerationTag {};
struct MessageSequenceTag {};

// --- Strongly typed identities --------------------------------------------

using FederationId = StrongId<FederationIdTag>;
using AgentId = StrongId<AgentIdTag>;
using AgentBootId = StrongId<AgentBootIdTag>;
using NodeId = StrongId<NodeIdTag>;
using AcceleratorId = StrongId<AcceleratorIdTag>;
using PhysicalDeviceId = StrongId<PhysicalDeviceIdTag>;
using VendorId = StrongId<VendorIdTag>;
using ArchitectureId = StrongId<ArchitectureIdTag>;
using CapabilitySetId = StrongId<CapabilitySetIdTag>;
using EvidenceId = StrongId<EvidenceIdTag>;
using WorkloadClassId = StrongId<WorkloadClassIdTag>;
using WorkloadRequirementId = StrongId<WorkloadRequirementIdTag>;
using CompatibilityProfileId = StrongId<CompatibilityProfileIdTag>;
using CompatibilityDecisionId = StrongId<CompatibilityDecisionIdTag>;
using PortabilityClassId = StrongId<PortabilityClassIdTag>;
using MigrationPlanId = StrongId<MigrationPlanIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using DecisionId = StrongId<DecisionIdTag>;
using SnapshotId = StrongId<SnapshotIdTag>;
using EventId = StrongId<EventIdTag>;
using SessionId = StrongId<SessionIdTag>;

// --- Strongly typed generations -------------------------------------------

using FederationGeneration = Generation<FederationGenerationTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using DeviceGeneration = Generation<DeviceGenerationTag>;
using CapabilityGeneration = Generation<CapabilityGenerationTag>;
using EvidenceGeneration = Generation<EvidenceGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using DecisionGeneration = Generation<DecisionGenerationTag>;
using MigrationGeneration = Generation<MigrationGenerationTag>;
using WorkloadRevision = Generation<WorkloadRevisionTag>;
using AgentGeneration = Generation<AgentGenerationTag>;
using MessageSequence = Generation<MessageSequenceTag>;

/// Human-readable kind name for diagnostics and hashing domains.
[[nodiscard]] const char* federation_id_kind_name() noexcept;

}  // namespace haf

namespace std {

template <class Tag>
struct hash<haf::StrongId<Tag>> {
    std::size_t operator()(const haf::StrongId<Tag>& id) const noexcept {
        const haf::Id128::bytes_type& b = id.raw().bytes();
        std::uint64_t folded = 1469598103934665603ULL;
        for (std::size_t i = 0; i < b.size(); ++i) {
            folded ^= static_cast<std::uint64_t>(b[i]);
            folded *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(folded);
    }
};

}  // namespace std

#endif  // HAF_CORE_IDS_HPP
