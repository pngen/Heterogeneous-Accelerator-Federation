#include "haf/model/policy.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "haf/core/limits.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

template <class T>
[[nodiscard]] Status canonicalize_sorted(std::vector<T>& values, const char* what) {
    if (values.size() > Limits::kMaxPolicyDimensions) {
        return Status(ErrorCode::BoundsExceeded, std::string("policy declares too many ") + what);
    }
    std::sort(values.begin(), values.end());
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] == values[i - 1]) {
            return Status(ErrorCode::DuplicateIdentity, std::string("policy declares a duplicate ") + what);
        }
    }
    return Status::success();
}

[[nodiscard]] Status canonicalize_tokens(std::vector<std::string>& values, const char* what) {
    std::vector<std::string> canonical;
    canonical.reserve(values.size());
    for (std::string& value : values) {
        const Result<std::string> result = canonical_token(value, Limits::kMaxTokenBytes);
        if (!result.ok()) {
            return Status(result.status().code(), std::string(what) + ": " + result.status().message());
        }
        canonical.push_back(*result);
    }
    values = std::move(canonical);
    return canonicalize_sorted(values, what);
}

[[nodiscard]] Status canonicalize_capabilities(std::vector<std::string>& values, const char* what) {
    std::vector<std::string> canonical;
    canonical.reserve(values.size());
    for (std::string& value : values) {
        const Result<CapabilityKey> key = capability_key_from_name(value);
        if (!key.ok()) {
            return Status(key.status().code(), std::string(what) + ": " + key.status().message());
        }
        canonical.push_back(key->name());
    }
    values = std::move(canonical);
    return canonicalize_sorted(values, what);
}

}  // namespace

std::string_view to_string(SyntheticEvidencePolicy value) noexcept {
    switch (value) {
        case SyntheticEvidencePolicy::Reject: return "REJECT";
        case SyntheticEvidencePolicy::Allow: return "ALLOW";
        case SyntheticEvidencePolicy::Require: return "REQUIRE";
    }
    return "ALLOW";
}

bool synthetic_evidence_policy_from_token(std::string_view token, SyntheticEvidencePolicy& out) noexcept {
    if (token == "reject") { out = SyntheticEvidencePolicy::Reject; return true; }
    if (token == "allow") { out = SyntheticEvidencePolicy::Allow; return true; }
    if (token == "require") { out = SyntheticEvidencePolicy::Require; return true; }
    return false;
}

bool synthetic_evidence_policy_from_wire(std::uint8_t raw, SyntheticEvidencePolicy& out) noexcept {
    switch (raw) {
        case 0: out = SyntheticEvidencePolicy::Reject; return true;
        case 1: out = SyntheticEvidencePolicy::Allow; return true;
        case 2: out = SyntheticEvidencePolicy::Require; return true;
        default: return false;
    }
}

Status FederationPolicy::validate() const {
    if (id.is_nil()) {
        return Status(ErrorCode::InvalidArgument, "federation policy has no identity");
    }
    if (name.size() > Limits::kMaxNameBytes) {
        return Status(ErrorCode::BoundsExceeded, "policy name exceeds the permitted length");
    }
    if (allowed_vendors.size() > Limits::kMaxPolicyDimensions ||
        forbidden_vendors.size() > Limits::kMaxPolicyDimensions ||
        forbidden_accelerators.size() > Limits::kMaxPolicyDimensions ||
        deprecated_architectures.size() > Limits::kMaxPolicyDimensions ||
        required_capabilities.size() > Limits::kMaxPolicyDimensions ||
        denied_capabilities.size() > Limits::kMaxPolicyDimensions || tags.size() > Limits::kMaxPolicyDimensions) {
        return Status(ErrorCode::BoundsExceeded, "policy dimension exceeds the permitted size");
    }
    // Every capability reference must resolve. A policy that names a capability
    // the federation vocabulary does not contain can never be satisfied and
    // must be rejected rather than silently treated as unknown.
    for (const std::string& capability : required_capabilities) {
        const Result<CapabilityKey> key = capability_key_from_name(capability);
        if (!key.ok()) {
            return Status(key.status().code(), "policy required_capabilities: " + key.status().message());
        }
    }
    for (const std::string& capability : denied_capabilities) {
        const Result<CapabilityKey> key = capability_key_from_name(capability);
        if (!key.ok()) {
            return Status(key.status().code(), "policy denied_capabilities: " + key.status().message());
        }
    }
    // A capability that is both required and denied is an impossible policy.
    for (const std::string& required : required_capabilities) {
        if (std::binary_search(denied_capabilities.begin(), denied_capabilities.end(), required)) {
            return Status(ErrorCode::ContradictoryCapabilities,
                          "policy both requires and denies capability '" + required + "'");
        }
    }
    for (const std::string& allowed : allowed_vendors) {
        if (std::binary_search(forbidden_vendors.begin(), forbidden_vendors.end(), allowed)) {
            return Status(ErrorCode::InvalidArgument, "policy both allows and forbids vendor '" + allowed + "'");
        }
    }
    if (minimum_memory_bytes > (1ULL << 62)) {
        return Status(ErrorCode::ImpossibleQuantity, "policy minimum memory is outside the permitted range");
    }
    if (required_evidence_freshness_nanos > Limits::kMaxEvidenceAgeNanos) {
        return Status(ErrorCode::ImpossibleQuantity, "policy evidence freshness budget is outside the permitted range");
    }
    return Status::success();
}

Sha256::digest_type FederationPolicy::digest() const {
    // Token dimensions are canonicalized before hashing so that the identity of
    // a policy never depends on the order its lists were declared in. Without
    // this, a decoded policy (canonicalized on load) would not match the
    // identity of the policy it was encoded from.
    const auto sorted_copy = [](const std::vector<std::string>& values) {
        std::vector<std::string> copy = values;
        std::sort(copy.begin(), copy.end());
        return copy;
    };
    const std::vector<std::string> allowed = sorted_copy(allowed_vendors);
    const std::vector<std::string> forbidden = sorted_copy(forbidden_vendors);
    const std::vector<std::string> deprecated = sorted_copy(deprecated_architectures);
    const std::vector<std::string> required = sorted_copy(required_capabilities);
    const std::vector<std::string> denied = sorted_copy(denied_capabilities);
    const std::vector<std::string> sorted_tags = sorted_copy(tags);
    ByteWriter writer;
    writer.string(name);
    writer.u32(static_cast<std::uint32_t>(allowed.size()));
    for (const std::string& value : allowed) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(forbidden.size()));
    for (const std::string& value : forbidden) {
        writer.string(value);
    }
    // Forbidden accelerators are ordered by identity so that declaration order
    // cannot change the policy digest.
    std::vector<AcceleratorId> forbidden_devices = forbidden_accelerators;
    std::sort(forbidden_devices.begin(), forbidden_devices.end());
    writer.u32(static_cast<std::uint32_t>(forbidden_devices.size()));
    for (const AcceleratorId& value : forbidden_devices) {
        writer.id128(value.raw());
    }
    writer.u32(static_cast<std::uint32_t>(deprecated.size()));
    for (const std::string& value : deprecated) {
        writer.string(value);
    }
    writer.string(minimum_runtime_version.to_string());
    writer.u64(required_evidence_freshness_nanos);
    writer.u8(static_cast<std::uint8_t>(synthetic_evidence_policy));
    writer.boolean(require_real_evidence_for_active);
    writer.u32(static_cast<std::uint32_t>(required.size()));
    for (const std::string& value : required) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(denied.size()));
    for (const std::string& value : denied) {
        writer.string(value);
    }
    std::vector<PortabilityClass> portability = allowed_portability_classes;
    std::sort(portability.begin(), portability.end());
    portability.erase(std::unique(portability.begin(), portability.end()), portability.end());
    writer.u32(static_cast<std::uint32_t>(portability.size()));
    for (const PortabilityClass value : portability) {
        writer.u8(static_cast<std::uint8_t>(value));
    }
    writer.boolean(allow_cross_vendor_migration);
    writer.boolean(allow_cross_vendor_reconstruction);
    writer.boolean(allow_degraded_capabilities);
    writer.u64(minimum_memory_bytes);
    writer.u32(static_cast<std::uint32_t>(sorted_tags.size()));
    for (const std::string& value : sorted_tags) {
        writer.string(value);
    }
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

void FederationPolicy::refresh_identity() {
    const std::string hex = to_hex(digest());
    id = PolicyId::from_raw(derive_identity("haf.policy", hex));
    // The generation is derived from content: an unchanged policy keeps its
    // generation, any change advances it.
    std::uint64_t folded = 0;
    const Sha256::digest_type content = digest();
    for (std::size_t i = 0; i < content.size(); ++i) {
        folded = (folded << 8) | static_cast<std::uint64_t>(content[i]);
        folded ^= folded >> 31;
    }
    generation = PolicyGeneration(folded == 0 ? 1 : folded);
}

void FederationPolicy::serialize(ByteWriter& writer) const {
    writer.id128(id.raw());
    writer.generation(generation);
    writer.string(name);
    writer.u32(static_cast<std::uint32_t>(allowed_vendors.size()));
    for (const std::string& value : allowed_vendors) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(forbidden_vendors.size()));
    for (const std::string& value : forbidden_vendors) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(forbidden_accelerators.size()));
    for (const AcceleratorId& value : forbidden_accelerators) {
        writer.id128(value.raw());
    }
    writer.u32(static_cast<std::uint32_t>(deprecated_architectures.size()));
    for (const std::string& value : deprecated_architectures) {
        writer.string(value);
    }
    writer.string(minimum_runtime_version.to_string());
    writer.u64(required_evidence_freshness_nanos);
    writer.u8(static_cast<std::uint8_t>(synthetic_evidence_policy));
    writer.boolean(require_real_evidence_for_active);
    writer.u32(static_cast<std::uint32_t>(required_capabilities.size()));
    for (const std::string& value : required_capabilities) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(denied_capabilities.size()));
    for (const std::string& value : denied_capabilities) {
        writer.string(value);
    }
    writer.u32(static_cast<std::uint32_t>(allowed_portability_classes.size()));
    for (const PortabilityClass value : allowed_portability_classes) {
        writer.u8(static_cast<std::uint8_t>(value));
    }
    writer.boolean(allow_cross_vendor_migration);
    writer.boolean(allow_cross_vendor_reconstruction);
    writer.boolean(allow_degraded_capabilities);
    writer.u64(minimum_memory_bytes);
    writer.u32(static_cast<std::uint32_t>(tags.size()));
    for (const std::string& value : tags) {
        writer.string(value);
    }
}

bool FederationPolicy::deserialize(ByteReader& reader, FederationPolicy& out, bool persisted_scale) {
    const std::uint32_t dimension_limit =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxPolicyDimensions)
                        : static_cast<std::uint32_t>(Limits::kMaxPolicyDimensions);
    FederationPolicy policy;
    Id128 raw_id;
    std::uint8_t raw_synthetic = 0;
    std::string minimum_version;
    if (!reader.id128(raw_id) || !reader.generation(policy.generation) ||
        !reader.string(policy.name, Limits::kMaxNameBytes)) {
        return false;
    }
    auto read_tokens = [&](std::vector<std::string>& target, std::size_t max_bytes) -> bool {
        std::uint32_t count = 0;
        if (!reader.count(count, dimension_limit, 4)) {
            return false;
        }
        target.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string value;
            if (!reader.string(value, max_bytes)) {
                return false;
            }
            target.push_back(std::move(value));
        }
        return true;
    };
    if (!read_tokens(policy.allowed_vendors, Limits::kMaxTokenBytes) ||
        !read_tokens(policy.forbidden_vendors, Limits::kMaxTokenBytes)) {
        return false;
    }
    std::uint32_t forbidden_count = 0;
    if (!reader.count(forbidden_count, dimension_limit, 16)) {
        return false;
    }
    policy.forbidden_accelerators.reserve(forbidden_count);
    for (std::uint32_t i = 0; i < forbidden_count; ++i) {
        AcceleratorId value;
        if (!reader.strong_id(value)) {
            return false;
        }
        policy.forbidden_accelerators.push_back(value);
    }
    if (!read_tokens(policy.deprecated_architectures, Limits::kMaxTokenBytes) ||
        !reader.string(minimum_version, Limits::kMaxStringBytes) ||
        !reader.u64(policy.required_evidence_freshness_nanos) || !reader.u8(raw_synthetic) ||
        !reader.boolean(policy.require_real_evidence_for_active)) {
        return false;
    }
    if (!synthetic_evidence_policy_from_wire(raw_synthetic, policy.synthetic_evidence_policy)) {
        reader.fail(ErrorCode::MalformedData, "synthetic evidence policy is outside the declared domain");
        return false;
    }
    if (!read_tokens(policy.required_capabilities, Limits::kMaxNameBytes) ||
        !read_tokens(policy.denied_capabilities, Limits::kMaxNameBytes)) {
        return false;
    }
    std::uint32_t portability_count = 0;
    if (!reader.count(portability_count, dimension_limit, 1)) {
        return false;
    }
    policy.allowed_portability_classes.reserve(portability_count);
    for (std::uint32_t i = 0; i < portability_count; ++i) {
        std::uint8_t raw_value = 0;
        if (!reader.u8(raw_value)) {
            return false;
        }
        PortabilityClass value = PortabilityClass::Unknown;
        if (!portability_class_from_wire(raw_value, value)) {
            reader.fail(ErrorCode::MalformedData, "portability class is outside the declared domain");
            return false;
        }
        policy.allowed_portability_classes.push_back(value);
    }
    if (!reader.boolean(policy.allow_cross_vendor_migration) ||
        !reader.boolean(policy.allow_cross_vendor_reconstruction) ||
        !reader.boolean(policy.allow_degraded_capabilities) || !reader.u64(policy.minimum_memory_bytes) ||
        !read_tokens(policy.tags, Limits::kMaxTokenBytes)) {
        return false;
    }
    const Result<VersionRange> range = VersionRange::parse(minimum_version);
    if (!range.ok()) {
        reader.fail(range.status().code(), range.status().message());
        return false;
    }
    policy.minimum_runtime_version = *range;
    policy.id = PolicyId::from_raw(raw_id);

    // Canonicalize the decoded vectors exactly as a natively constructed policy
    // would be, then require that the identity matches its content digest.
    std::vector<std::string> allowed = policy.allowed_vendors;
    std::vector<std::string> forbidden_vendors = policy.forbidden_vendors;
    std::vector<std::string> deprecated = policy.deprecated_architectures;
    std::vector<std::string> required = policy.required_capabilities;
    std::vector<std::string> denied = policy.denied_capabilities;
    std::vector<std::string> tags = policy.tags;
    const Status ok = [&]() -> Status {
        Status status = canonicalize_tokens(allowed, "allowed vendor");
        if (!status.ok()) return status;
        status = canonicalize_tokens(forbidden_vendors, "forbidden vendor");
        if (!status.ok()) return status;
        status = canonicalize_tokens(deprecated, "deprecated architecture");
        if (!status.ok()) return status;
        status = canonicalize_capabilities(required, "required capability");
        if (!status.ok()) return status;
        status = canonicalize_capabilities(denied, "denied capability");
        if (!status.ok()) return status;
        return canonicalize_tokens(tags, "policy tag");
    }();
    if (!ok.ok()) {
        reader.fail(ok.code(), ok.message());
        return false;
    }
    policy.allowed_vendors = std::move(allowed);
    policy.forbidden_vendors = std::move(forbidden_vendors);
    policy.deprecated_architectures = std::move(deprecated);
    policy.required_capabilities = std::move(required);
    policy.denied_capabilities = std::move(denied);
    policy.tags = std::move(tags);

    const Status valid = policy.validate();
    if (!valid.ok()) {
        reader.fail(valid.code(), valid.message());
        return false;
    }
    const PolicyId recomputed = policy.id;
    policy.refresh_identity();
    if (policy.id != recomputed) {
        reader.fail(ErrorCode::IntegrityFailure, "policy identity does not match its content digest");
        return false;
    }
    out = std::move(policy);
    return true;
}

FederationPolicy FederationPolicy::permissive_default() {
    FederationPolicy policy;
    policy.name = "default";
    policy.synthetic_evidence_policy = SyntheticEvidencePolicy::Allow;
    policy.allow_cross_vendor_reconstruction = true;
    policy.allow_cross_vendor_migration = false;
    policy.refresh_identity();
    return policy;
}

}  // namespace haf
