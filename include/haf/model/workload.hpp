// Heterogeneous Accelerator Federation - workload requirements.
//
// A workload profile is a set of typed constraints over accelerator
// capabilities plus a small number of execution-level constraints that are not
// capability shaped (execution exactness, portability threshold, migration
// need, reconstruction allowance).
//
// HARD requirements decide eligibility. SOFT preferences never rescue an
// ineligible accelerator; they only order eligible ones deterministically.

#ifndef HAF_MODEL_WORKLOAD_HPP
#define HAF_MODEL_WORKLOAD_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/version.hpp"
#include "haf/model/capability.hpp"
#include "haf/model/portability.hpp"

namespace haf {

enum class RequirementStrength : std::uint8_t { Hard = 0, Soft = 1 };

[[nodiscard]] std::string_view to_string(RequirementStrength strength) noexcept;
[[nodiscard]] bool requirement_strength_from_wire(std::uint8_t raw, RequirementStrength& out) noexcept;

enum class RequirementOperator : std::uint8_t {
    Present = 0,          ///< capability must be positively supported
    Absent = 1,           ///< capability must be positively unsupported
    Equals = 2,           ///< payload equality
    NotEquals = 3,
    AtLeast = 4,          ///< numeric or version lower bound, inclusive
    AtMost = 5,
    GreaterThan = 6,      ///< numeric strict lower bound
    LessThan = 7,
    InSet = 8,            ///< provided enumeration/token is a member of the required set
    SupersetOf = 9,       ///< provided token set contains every required token
    SubsetOf = 10,        ///< provided token set is contained in the required tokens
    IntersectsWith = 11,  ///< provided token set shares at least one token
    VersionSatisfies = 12 ///< provided version satisfies the requirement version range
};

[[nodiscard]] std::string_view to_string(RequirementOperator value) noexcept;
[[nodiscard]] bool requirement_operator_from_wire(std::uint8_t raw, RequirementOperator& out) noexcept;

/// How exact the execution must be on the destination.
enum class ExecutionMode : std::uint8_t {
    ExactBinary = 0,      ///< the identical code object must execute
    EquivalentAllowed = 1,///< an equivalent code object produced by recompilation is acceptable
    AnyPortability = 2,
};

[[nodiscard]] std::string_view to_string(ExecutionMode mode) noexcept;
[[nodiscard]] bool execution_mode_from_wire(std::uint8_t raw, ExecutionMode& out) noexcept;

/// What the workload needs in order to change accelerators at all.
enum class MigrationNeed : std::uint8_t {
    None = 0,             ///< no movement required
    RestartOnly = 1,      ///< a clean restart on the destination is sufficient
    CheckpointRestore = 2,///< execution state must be exported and restored
    LiveStateTransfer = 3,///< execution must continue across the move
};

[[nodiscard]] std::string_view to_string(MigrationNeed need) noexcept;
[[nodiscard]] bool migration_need_from_wire(std::uint8_t raw, MigrationNeed& out) noexcept;

struct CapabilityRequirement {
    WorkloadRequirementId id{};
    /// Canonical capability key name. Resolved at validation time.
    std::string capability;
    RequirementStrength strength{RequirementStrength::Hard};
    RequirementOperator op{RequirementOperator::Present};
    CapabilityValue value{};
    VersionRange version_range{};
    /// Soft preference weight. Must be finite; ignored for hard requirements.
    double weight{0.0};
    std::string rationale;

    [[nodiscard]] Status validate() const;
    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CapabilityRequirement& out);
};

struct WorkloadProfile {
    WorkloadClassId class_id{};
    WorkloadRequirementId requirement_id{};
    WorkloadRevision revision{};

    /// Canonical workload name, e.g. "llm.decode.fp8".
    std::string name;
    std::string description;

    std::vector<CapabilityRequirement> requirements;

    ExecutionMode execution_mode{ExecutionMode::EquivalentAllowed};
    PortabilityClass minimum_portability{PortabilityClass::Unsupported};
    MigrationNeed migration_need{MigrationNeed::None};
    bool reconstruction_allowed{true};
    /// Policy tags the destination must satisfy, e.g. "isolation.strong".
    std::vector<std::string> required_policy_tags;

    [[nodiscard]] Status validate() const;

    /// Deterministic digest over the canonical workload definition. Bound into
    /// every decision so that a revised workload invalidates them.
    [[nodiscard]] Sha256::digest_type digest() const;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, WorkloadProfile& out, bool persisted_scale);

    [[nodiscard]] std::size_t hard_requirement_count() const noexcept;
    [[nodiscard]] std::size_t soft_requirement_count() const noexcept;
};

/// Deterministic identity for a workload requirement revision.
[[nodiscard]] WorkloadRequirementId derive_requirement_id(const WorkloadProfile& profile);
[[nodiscard]] WorkloadRevision derive_workload_revision(const WorkloadProfile& profile);

/// Builder helpers used by examples, tests, and the CLI.
[[nodiscard]] Result<CapabilityRequirement> require_present(std::string_view capability, RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_absent(std::string_view capability, RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_at_least(std::string_view capability, std::int64_t value,
                                                            RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_token_in(std::string_view capability, std::vector<std::string> tokens,
                                                             RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_tokens_superset(std::string_view capability,
                                                                    std::vector<std::string> tokens,
                                                                    RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_version_range(std::string_view capability, std::string_view range,
                                                                  RequirementStrength strength);
[[nodiscard]] Result<CapabilityRequirement> require_equals(std::string_view capability, std::string_view payload,
                                                           RequirementStrength strength);

}  // namespace haf

#endif  // HAF_MODEL_WORKLOAD_HPP
