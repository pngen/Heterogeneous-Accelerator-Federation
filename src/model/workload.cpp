#include "haf/model/workload.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/hash.hpp"
#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

[[nodiscard]] Status check_requirement_identity(const CapabilityRequirement& requirement) {
    if (requirement.id.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "capability requirement has no identity");
    }
    return Status::success();
}

[[nodiscard]] Result<CapabilityRequirement> base_requirement(std::string_view capability, RequirementStrength strength) {
    const Result<CapabilityKey> key = capability_key_from_name(capability);
    if (!key.ok()) {
        return key.status();
    }
    CapabilityRequirement requirement;
    requirement.capability = key->name();
    requirement.strength = strength;
    requirement.value = CapabilityValue(key->kind());
    return requirement;
}

}  // namespace

std::string_view to_string(RequirementStrength strength) noexcept {
    switch (strength) {
        case RequirementStrength::Hard: return "HARD";
        case RequirementStrength::Soft: return "SOFT";
    }
    return "HARD";
}

bool requirement_strength_from_wire(std::uint8_t raw, RequirementStrength& out) noexcept {
    switch (raw) {
        case 0: out = RequirementStrength::Hard; return true;
        case 1: out = RequirementStrength::Soft; return true;
        default: return false;
    }
}

std::string_view to_string(RequirementOperator value) noexcept {
    switch (value) {
        case RequirementOperator::Present: return "PRESENT";
        case RequirementOperator::Absent: return "ABSENT";
        case RequirementOperator::Equals: return "EQUALS";
        case RequirementOperator::NotEquals: return "NOT_EQUALS";
        case RequirementOperator::AtLeast: return "AT_LEAST";
        case RequirementOperator::AtMost: return "AT_MOST";
        case RequirementOperator::GreaterThan: return "GREATER_THAN";
        case RequirementOperator::LessThan: return "LESS_THAN";
        case RequirementOperator::InSet: return "IN_SET";
        case RequirementOperator::SupersetOf: return "SUPERSET_OF";
        case RequirementOperator::SubsetOf: return "SUBSET_OF";
        case RequirementOperator::IntersectsWith: return "INTERSECTS_WITH";
        case RequirementOperator::VersionSatisfies: return "VERSION_SATISFIES";
    }
    return "PRESENT";
}

bool requirement_operator_from_wire(std::uint8_t raw, RequirementOperator& out) noexcept {
    if (raw > static_cast<std::uint8_t>(RequirementOperator::VersionSatisfies)) {
        return false;
    }
    out = static_cast<RequirementOperator>(raw);
    return true;
}

std::string_view to_string(ExecutionMode mode) noexcept {
    switch (mode) {
        case ExecutionMode::ExactBinary: return "EXACT_BINARY";
        case ExecutionMode::EquivalentAllowed: return "EQUIVALENT_ALLOWED";
        case ExecutionMode::AnyPortability: return "ANY_PORTABILITY";
    }
    return "EQUIVALENT_ALLOWED";
}

bool execution_mode_from_wire(std::uint8_t raw, ExecutionMode& out) noexcept {
    switch (raw) {
        case 0: out = ExecutionMode::ExactBinary; return true;
        case 1: out = ExecutionMode::EquivalentAllowed; return true;
        case 2: out = ExecutionMode::AnyPortability; return true;
        default: return false;
    }
}

std::string_view to_string(MigrationNeed need) noexcept {
    switch (need) {
        case MigrationNeed::None: return "NONE";
        case MigrationNeed::RestartOnly: return "RESTART_ONLY";
        case MigrationNeed::CheckpointRestore: return "CHECKPOINT_RESTORE";
        case MigrationNeed::LiveStateTransfer: return "LIVE_STATE_TRANSFER";
    }
    return "NONE";
}

bool migration_need_from_wire(std::uint8_t raw, MigrationNeed& out) noexcept {
    switch (raw) {
        case 0: out = MigrationNeed::None; return true;
        case 1: out = MigrationNeed::RestartOnly; return true;
        case 2: out = MigrationNeed::CheckpointRestore; return true;
        case 3: out = MigrationNeed::LiveStateTransfer; return true;
        default: return false;
    }
}

Status CapabilityRequirement::validate() const {
    const Status identity = check_requirement_identity(*this);
    if (!identity.ok()) {
        return identity;
    }
    if (capability.empty() || capability.size() > Limits::kMaxNameBytes) {
        return Status(ErrorCode::InvalidArgument, "capability requirement has an empty or oversized capability name");
    }
    const Result<CapabilityKey> key = capability_key_from_name(capability);
    if (!key.ok()) {
        return key.status();
    }
    if (value.kind() != key->kind()) {
        return Status(ErrorCode::CapabilityMismatch, "requirement for '" + capability + "' carries a payload of kind " +
                                                         std::string(to_string(value.kind())) + " but the capability is " +
                                                         std::string(to_string(key->kind())));
    }
    if (!std::isfinite(weight)) {
        return Status(ErrorCode::NonFiniteQuantity, "requirement weight is not finite");
    }
    if (weight < 0.0 || weight > 1.0e9) {
        return Status(ErrorCode::ImpossibleQuantity, "requirement weight is outside the permitted range");
    }
    if (strength == RequirementStrength::Hard && weight != 0.0) {
        return Status(ErrorCode::InvalidArgument, "hard requirement must not carry a soft preference weight");
    }
    if (rationale.size() > Limits::kMaxRationaleBytes) {
        return Status(ErrorCode::BoundsExceeded, "requirement rationale exceeds the permitted length");
    }
    switch (op) {
        case RequirementOperator::VersionSatisfies:
            if (value.kind() != CapabilityKind::Version) {
                return Status(ErrorCode::CapabilityMismatch,
                              "VERSION_SATISFIES requires a version capability: '" + capability + "'");
            }
            if (version_range.is_any()) {
                return Status(ErrorCode::MalformedVersionRange,
                              "VERSION_SATISFIES requires a bounded version range for '" + capability + "'");
            }
            break;
        case RequirementOperator::InSet:
        case RequirementOperator::SupersetOf:
        case RequirementOperator::SubsetOf:
        case RequirementOperator::IntersectsWith:
            if (value.kind() != CapabilityKind::TokenSet && value.kind() != CapabilityKind::Enumeration) {
                return Status(ErrorCode::CapabilityMismatch,
                              "set operator requires a token-set or enumeration capability: '" + capability + "'");
            }
            break;
        case RequirementOperator::AtLeast:
        case RequirementOperator::AtMost:
        case RequirementOperator::GreaterThan:
        case RequirementOperator::LessThan:
            if (value.kind() != CapabilityKind::Integer && value.kind() != CapabilityKind::Scalar &&
                value.kind() != CapabilityKind::Version) {
                return Status(ErrorCode::CapabilityMismatch,
                              "ordering operator requires a numeric or version capability: '" + capability + "'");
            }
            break;
        default:
            break;
    }
    return Status::success();
}

void CapabilityRequirement::serialize(ByteWriter& writer) const {
    writer.id128(id.raw());
    writer.string(capability);
    writer.u8(static_cast<std::uint8_t>(strength));
    writer.u8(static_cast<std::uint8_t>(op));
    value.serialize(writer);
    writer.u32(static_cast<std::uint32_t>(version_range.clauses().size()));
    for (const VersionClause& clause : version_range.clauses()) {
        writer.u8(static_cast<std::uint8_t>(clause.op));
        writer.u32(clause.version.major());
        writer.u32(clause.version.minor());
        writer.u32(clause.version.patch());
        writer.string(clause.version.prerelease());
    }
    writer.f64(weight);
    writer.string(rationale);
}

bool CapabilityRequirement::deserialize(ByteReader& reader, CapabilityRequirement& out) {
    CapabilityRequirement requirement;
    std::uint8_t raw_strength = 0;
    std::uint8_t raw_op = 0;
    if (!reader.strong_id(requirement.id) || !reader.string(requirement.capability, Limits::kMaxNameBytes) ||
        !reader.u8(raw_strength) || !reader.u8(raw_op)) {
        return false;
    }
    if (!requirement_strength_from_wire(raw_strength, requirement.strength)) {
        reader.fail(ErrorCode::MalformedData, "requirement strength is outside the declared domain");
        return false;
    }
    if (!requirement_operator_from_wire(raw_op, requirement.op)) {
        reader.fail(ErrorCode::MalformedData, "requirement operator is outside the declared domain");
        return false;
    }
    const Result<CapabilityKey> key = capability_key_from_name(requirement.capability);
    if (!key.ok()) {
        reader.fail(key.status().code(), key.status().message());
        return false;
    }
    // The payload carries its own kind tag; it must agree with the kind the key
    // declares. Assuming the kind here previously made the codec asymmetric.
    if (!CapabilityValue::deserialize(reader, requirement.value)) {
        return false;
    }
    if (requirement.value.kind() != key->kind()) {
        reader.fail(ErrorCode::CapabilityMismatch, "requirement for '" + requirement.capability +
                                                       "' carries a payload whose kind disagrees with the capability");
        return false;
    }
    std::uint32_t clause_count = 0;
    if (!reader.count(clause_count, 32, 13)) {
        return false;
    }
    std::vector<VersionClause> clauses;
    clauses.reserve(clause_count);
    for (std::uint32_t i = 0; i < clause_count; ++i) {
        std::uint8_t clause_op = 0;
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        std::uint32_t patch = 0;
        std::string prerelease;
        if (!reader.u8(clause_op) || !reader.u32(major) || !reader.u32(minor) || !reader.u32(patch) ||
            !reader.string(prerelease, Limits::kMaxTokenBytes)) {
            return false;
        }
        if (clause_op > static_cast<std::uint8_t>(VersionOperator::Tilde)) {
            reader.fail(ErrorCode::MalformedVersionRange, "version clause operator is outside the declared domain");
            return false;
        }
        SemanticVersion version(major, minor, patch);
        version.set_prerelease(std::move(prerelease));
        clauses.push_back(VersionClause{static_cast<VersionOperator>(clause_op), std::move(version)});
    }
    if (!clauses.empty()) {
        // Reconstruct through the canonical parser path so that a decoded range
        // is exactly as strict as a parsed one.
        std::string text;
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            if (i != 0) {
                text += " ";
            }
            switch (clauses[i].op) {
                case VersionOperator::Any: text += "*"; break;
                case VersionOperator::Equal: text += "="; break;
                case VersionOperator::NotEqual: text += "!="; break;
                case VersionOperator::Less: text += "<"; break;
                case VersionOperator::LessEqual: text += "<="; break;
                case VersionOperator::Greater: text += ">"; break;
                case VersionOperator::GreaterEqual: text += ">="; break;
                case VersionOperator::Compatible: text += "^"; break;
                case VersionOperator::Tilde: text += "~"; break;
            }
            text += clauses[i].version.to_string();
        }
        const Result<VersionRange> reparsed = VersionRange::parse(text);
        if (!reparsed.ok()) {
            reader.fail(reparsed.status().code(), reparsed.status().message());
            return false;
        }
        requirement.version_range = *reparsed;
    }
    if (!reader.f64(requirement.weight) || !reader.string(requirement.rationale, Limits::kMaxRationaleBytes)) {
        return false;
    }
    const Status valid = requirement.validate();
    if (!valid.ok()) {
        reader.fail(valid.code(), valid.message());
        return false;
    }
    out = std::move(requirement);
    return true;
}

Status WorkloadProfile::validate() const {
    if (class_id.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "workload profile has no class identity");
    }
    if (requirement_id.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "workload profile has no requirement identity");
    }
    if (name.empty() || name.size() > Limits::kMaxNameBytes) {
        return Status(ErrorCode::InvalidArgument, "workload profile has an empty or oversized name");
    }
    if (description.size() > Limits::kMaxDescriptionBytes) {
        return Status(ErrorCode::BoundsExceeded, "workload description exceeds the permitted length");
    }
    if (requirements.size() > Limits::kMaxRequirementsPerWorkload) {
        return Status(ErrorCode::BoundsExceeded, "workload profile exceeds the permitted number of requirements");
    }
    std::vector<std::string> seen;
    seen.reserve(requirements.size());
    for (const CapabilityRequirement& requirement : requirements) {
        const Status valid = requirement.validate();
        if (!valid.ok()) {
            return valid;
        }
        seen.push_back(requirement.capability + "#" + std::string(to_string(requirement.strength)) + "#" +
                       std::string(to_string(requirement.op)));
    }
    std::sort(seen.begin(), seen.end());
    for (std::size_t i = 1; i < seen.size(); ++i) {
        if (seen[i] == seen[i - 1]) {
            return Status(ErrorCode::DuplicateIdentity,
                          "workload profile declares the same capability requirement twice: " + seen[i]);
        }
    }
    if (required_policy_tags.size() > Limits::kMaxPolicyDimensions) {
        return Status(ErrorCode::BoundsExceeded, "workload profile declares too many policy tags");
    }
    std::vector<std::string> tags = required_policy_tags;
    std::sort(tags.begin(), tags.end());
    for (std::size_t i = 1; i < tags.size(); ++i) {
        if (tags[i] == tags[i - 1]) {
            return Status(ErrorCode::DuplicateIdentity, "workload profile declares a duplicate policy tag: " + tags[i]);
        }
    }
    for (const std::string& tag : required_policy_tags) {
        if (tag.empty() || tag.size() > Limits::kMaxTokenBytes) {
            return Status(ErrorCode::InvalidArgument, "policy tag is empty or oversized");
        }
    }
    if (minimum_portability == PortabilityClass::LiveMigrationSupported &&
        migration_need != MigrationNeed::LiveStateTransfer) {
        return Status(ErrorCode::InvalidArgument,
                      "a live-migration portability minimum requires a live state transfer migration need");
    }
    return Status::success();
}

Sha256::digest_type WorkloadProfile::digest() const {
    ByteWriter writer;
    writer.id128(class_id.raw());
    writer.string(name);
    writer.string(description);
    writer.u8(static_cast<std::uint8_t>(execution_mode));
    writer.u8(static_cast<std::uint8_t>(minimum_portability));
    writer.u8(static_cast<std::uint8_t>(migration_need));
    writer.boolean(reconstruction_allowed);
    // Requirements are canonicalized by capability name before hashing so that
    // declaration order never influences the digest.
    std::vector<const CapabilityRequirement*> ordered;
    ordered.reserve(requirements.size());
    for (const CapabilityRequirement& requirement : requirements) {
        ordered.push_back(&requirement);
    }
    std::sort(ordered.begin(), ordered.end(), [](const CapabilityRequirement* a, const CapabilityRequirement* b) {
        if (a->capability != b->capability) {
            return a->capability < b->capability;
        }
        if (a->strength != b->strength) {
            return static_cast<std::uint8_t>(a->strength) < static_cast<std::uint8_t>(b->strength);
        }
        return static_cast<std::uint8_t>(a->op) < static_cast<std::uint8_t>(b->op);
    });
    writer.u32(static_cast<std::uint32_t>(ordered.size()));
    for (const CapabilityRequirement* requirement : ordered) {
        writer.string(requirement->capability);
        writer.u8(static_cast<std::uint8_t>(requirement->strength));
        writer.u8(static_cast<std::uint8_t>(requirement->op));
        writer.string(requirement->value.render());
        writer.string(requirement->version_range.to_string());
        writer.f64(requirement->weight);
        writer.string(requirement->rationale);
    }
    std::vector<std::string> tags = required_policy_tags;
    std::sort(tags.begin(), tags.end());
    writer.u32(static_cast<std::uint32_t>(tags.size()));
    for (const std::string& tag : tags) {
        writer.string(tag);
    }
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

void WorkloadProfile::serialize(ByteWriter& writer) const {
    writer.id128(class_id.raw());
    writer.id128(requirement_id.raw());
    writer.generation(revision);
    writer.string(name);
    writer.string(description);
    writer.u8(static_cast<std::uint8_t>(execution_mode));
    writer.u8(static_cast<std::uint8_t>(minimum_portability));
    writer.u8(static_cast<std::uint8_t>(migration_need));
    writer.boolean(reconstruction_allowed);
    writer.u32(static_cast<std::uint32_t>(requirements.size()));
    for (const CapabilityRequirement& requirement : requirements) {
        requirement.serialize(writer);
    }
    writer.u32(static_cast<std::uint32_t>(required_policy_tags.size()));
    for (const std::string& tag : required_policy_tags) {
        writer.string(tag);
    }
}

bool WorkloadProfile::deserialize(ByteReader& reader, WorkloadProfile& out, bool persisted_scale) {
    const std::uint32_t requirement_limit =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxRequirementsPerWorkload)
                        : static_cast<std::uint32_t>(Limits::kMaxRequirementsPerWorkload);
    WorkloadProfile profile;
    std::uint8_t raw_mode = 0;
    std::uint8_t raw_portability = 0;
    std::uint8_t raw_need = 0;
    Id128 raw_class;
    Id128 raw_requirement;
    if (!reader.id128(raw_class) || !reader.id128(raw_requirement) || !reader.generation(profile.revision) ||
        !reader.string(profile.name, Limits::kMaxNameBytes) ||
        !reader.string(profile.description, Limits::kMaxDescriptionBytes) || !reader.u8(raw_mode) ||
        !reader.u8(raw_portability) || !reader.u8(raw_need) || !reader.boolean(profile.reconstruction_allowed)) {
        return false;
    }
    if (!execution_mode_from_wire(raw_mode, profile.execution_mode)) {
        reader.fail(ErrorCode::MalformedData, "execution mode is outside the declared domain");
        return false;
    }
    if (!portability_class_from_wire(raw_portability, profile.minimum_portability)) {
        reader.fail(ErrorCode::MalformedData, "portability class is outside the declared domain");
        return false;
    }
    if (!migration_need_from_wire(raw_need, profile.migration_need)) {
        reader.fail(ErrorCode::MalformedData, "migration need is outside the declared domain");
        return false;
    }
    std::uint32_t requirement_count = 0;
    if (!reader.count(requirement_count, requirement_limit, 8)) {
        return false;
    }
    profile.requirements.reserve(requirement_count);
    for (std::uint32_t i = 0; i < requirement_count; ++i) {
        CapabilityRequirement requirement;
        if (!CapabilityRequirement::deserialize(reader, requirement)) {
            return false;
        }
        profile.requirements.push_back(std::move(requirement));
    }
    std::uint32_t tag_count = 0;
    if (!reader.count(tag_count, static_cast<std::uint32_t>(Limits::kMaxPolicyDimensions), 4)) {
        return false;
    }
    profile.required_policy_tags.reserve(tag_count);
    for (std::uint32_t i = 0; i < tag_count; ++i) {
        std::string tag;
        if (!reader.string(tag, Limits::kMaxTokenBytes)) {
            return false;
        }
        profile.required_policy_tags.push_back(std::move(tag));
    }
    profile.class_id = WorkloadClassId::from_raw(raw_class);
    profile.requirement_id = WorkloadRequirementId::from_raw(raw_requirement);
    const Status valid = profile.validate();
    if (!valid.ok()) {
        reader.fail(valid.code(), valid.message());
        return false;
    }
    out = std::move(profile);
    return true;
}

std::size_t WorkloadProfile::hard_requirement_count() const noexcept {
    std::size_t count = 0;
    for (const CapabilityRequirement& requirement : requirements) {
        if (requirement.strength == RequirementStrength::Hard) {
            ++count;
        }
    }
    return count;
}

std::size_t WorkloadProfile::soft_requirement_count() const noexcept {
    return requirements.size() - hard_requirement_count();
}

WorkloadRequirementId derive_requirement_id(const WorkloadProfile& profile) {
    ByteWriter writer;
    writer.id128(profile.class_id.raw());
    writer.string(profile.name);
    const ByteBuffer& bytes = writer.data();
    return WorkloadRequirementId::from_raw(
        derive_identity("haf.workload-requirement", to_hex(Sha256::hash(bytes.data(), bytes.size()))));
}

WorkloadRevision derive_workload_revision(const WorkloadProfile& profile) {
    // The revision is a content digest folded into a 64-bit generation so that
    // two structurally different revisions can never share a value in practice.
    const Sha256::digest_type digest = profile.digest();
    std::uint64_t folded = 0;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        folded = (folded << 8) | static_cast<std::uint64_t>(digest[i]);
        folded ^= folded >> 29;
    }
    if (folded == 0) {
        folded = 1;
    }
    return WorkloadRevision(folded);
}

Result<CapabilityRequirement> require_present(std::string_view capability, RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    base->op = RequirementOperator::Present;
    base->id = WorkloadRequirementId::from_raw(derive_identity("haf.requirement", base->capability + "|present"));
    return base;
}

Result<CapabilityRequirement> require_absent(std::string_view capability, RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    base->op = RequirementOperator::Absent;
    base->id = WorkloadRequirementId::from_raw(derive_identity("haf.requirement", base->capability + "|absent"));
    return base;
}

Result<CapabilityRequirement> require_at_least(std::string_view capability, std::int64_t value,
                                               RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    base->op = RequirementOperator::AtLeast;
    base->value = CapabilityValue::integer(value);
    base->id = WorkloadRequirementId::from_raw(
        derive_identity("haf.requirement", base->capability + "|at_least|" + std::to_string(value)));
    const Status valid = base->validate();
    if (!valid.ok()) {
        return valid;
    }
    return base;
}

Result<CapabilityRequirement> require_token_in(std::string_view capability, std::vector<std::string> tokens,
                                               RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    base->op = RequirementOperator::InSet;
    base->value = CapabilityValue::token_set(std::move(tokens));
    const Status canonical = base->value.canonicalize();
    if (!canonical.ok()) {
        return canonical;
    }
    std::string joined = base->value.render();
    base->id = WorkloadRequirementId::from_raw(
        derive_identity("haf.requirement", base->capability + "|in|" + joined));
    const Status valid = base->validate();
    if (!valid.ok()) {
        return valid;
    }
    return base;
}

Result<CapabilityRequirement> require_tokens_superset(std::string_view capability, std::vector<std::string> tokens,
                                                      RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    base->op = RequirementOperator::SupersetOf;
    base->value = CapabilityValue::token_set(std::move(tokens));
    const Status canonical = base->value.canonicalize();
    if (!canonical.ok()) {
        return canonical;
    }
    std::string joined = base->value.render();
    base->id = WorkloadRequirementId::from_raw(
        derive_identity("haf.requirement", base->capability + "|superset|" + joined));
    const Status valid = base->validate();
    if (!valid.ok()) {
        return valid;
    }
    return base;
}

Result<CapabilityRequirement> require_version_range(std::string_view capability, std::string_view range,
                                                    RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    const Result<VersionRange> parsed = VersionRange::parse(range);
    if (!parsed.ok()) {
        return parsed.status();
    }
    base->op = RequirementOperator::VersionSatisfies;
    base->version_range = *parsed;
    base->value = CapabilityValue::version(SemanticVersion{});
    base->id = WorkloadRequirementId::from_raw(
        derive_identity("haf.requirement", base->capability + "|version|" + parsed->to_string()));
    const Status valid = base->validate();
    if (!valid.ok()) {
        return valid;
    }
    return base;
}

Result<CapabilityRequirement> require_equals(std::string_view capability, std::string_view payload,
                                             RequirementStrength strength) {
    Result<CapabilityRequirement> base = base_requirement(capability, strength);
    if (!base.ok()) {
        return base.status();
    }
    const Result<CapabilityKey> key = capability_key_from_name(capability);
    if (!key.ok()) {
        return key.status();
    }
    const Result<CapabilityValue> parsed = CapabilityValue::parse(key->kind(), payload);
    if (!parsed.ok()) {
        return parsed.status();
    }
    base->op = RequirementOperator::Equals;
    base->value = *parsed;
    base->id = WorkloadRequirementId::from_raw(
        derive_identity("haf.requirement", base->capability + "|equals|" + base->value.render()));
    const Status valid = base->validate();
    if (!valid.ok()) {
        return valid;
    }
    return base;
}

}  // namespace haf
