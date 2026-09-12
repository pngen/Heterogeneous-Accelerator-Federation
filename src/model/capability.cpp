#include "haf/model/capability.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "haf/core/limits.hpp"
#include "haf/model/taxonomy.hpp"

namespace haf {
namespace {

[[nodiscard]] bool canonical_tokens_in_place(std::vector<std::string>& tokens, std::size_t max_tokens) {
    if (tokens.size() > max_tokens) {
        return false;
    }
    std::vector<std::string> normalized;
    normalized.reserve(tokens.size());
    for (std::string& token : tokens) {
        const Result<std::string> result = canonical_token(token, Limits::kMaxTokenBytes);
        if (!result.ok()) {
            return false;
        }
        normalized.push_back(*result);
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    tokens = std::move(normalized);
    return true;
}

[[nodiscard]] std::string join_tokens(const std::vector<std::string>& tokens) {
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += tokens[i];
    }
    return out;
}

[[nodiscard]] std::vector<std::string> split_tokens(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        out.emplace_back(text.substr(start, end - start));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

}  // namespace

std::string_view to_string(CapabilityState state) noexcept {
    switch (state) {
        case CapabilityState::Unknown: return "UNKNOWN";
        case CapabilityState::Supported: return "SUPPORTED";
        case CapabilityState::Unsupported: return "UNSUPPORTED";
        case CapabilityState::Degraded: return "DEGRADED";
    }
    return "UNKNOWN";
}

bool capability_state_from_wire(std::uint8_t raw, CapabilityState& out) noexcept {
    switch (raw) {
        case 0: out = CapabilityState::Unknown; return true;
        case 1: out = CapabilityState::Supported; return true;
        case 2: out = CapabilityState::Unsupported; return true;
        case 3: out = CapabilityState::Degraded; return true;
        default: return false;
    }
}

CapabilityValue CapabilityValue::presence() noexcept {
    CapabilityValue value(CapabilityKind::Presence);
    value.flag_ = true;
    return value;
}

CapabilityValue CapabilityValue::version(SemanticVersion value) {
    CapabilityValue result(CapabilityKind::Version);
    result.version_ = std::move(value);
    return result;
}

CapabilityValue CapabilityValue::integer(std::int64_t value) noexcept {
    CapabilityValue result(CapabilityKind::Integer);
    result.integer_ = value;
    return result;
}

CapabilityValue CapabilityValue::scalar(double value) noexcept {
    CapabilityValue result(CapabilityKind::Scalar);
    result.scalar_ = value;
    return result;
}

CapabilityValue CapabilityValue::enumeration(std::string token) {
    CapabilityValue result(CapabilityKind::Enumeration);
    result.token_ = std::move(token);
    return result;
}

CapabilityValue CapabilityValue::token_set(std::vector<std::string> tokens) {
    CapabilityValue result(CapabilityKind::TokenSet);
    result.tokens_ = std::move(tokens);
    return result;
}

Result<CapabilityValue> CapabilityValue::parse(CapabilityKind kind, std::string_view text) {
    switch (kind) {
        case CapabilityKind::Presence:
            return CapabilityValue::presence();
        case CapabilityKind::Version: {
            const std::optional<SemanticVersion> parsed = SemanticVersion::parse(text);
            if (!parsed.has_value()) {
                return Status(ErrorCode::MalformedData, "capability version payload is not a semantic version: '" +
                                                           std::string(text) + "'");
            }
            return CapabilityValue::version(*parsed);
        }
        case CapabilityKind::Integer: {
            std::string_view body = text;
            std::int64_t sign = 1;
            if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
                sign = body.front() == '-' ? -1 : 1;
                body.remove_prefix(1);
            }
            if (body.empty() || body.size() > 18) {
                return Status(ErrorCode::MalformedData, "capability integer payload is malformed: '" + std::string(text) + "'");
            }
            std::int64_t value = 0;
            for (const char c : body) {
                if (c < '0' || c > '9') {
                    return Status(ErrorCode::MalformedData,
                                  "capability integer payload is malformed: '" + std::string(text) + "'");
                }
                value = value * 10 + (c - '0');
            }
            return CapabilityValue::integer(sign * value);
        }
        case CapabilityKind::Scalar: {
            std::string body(text);
            if (body.empty() || body.size() > 64) {
                return Status(ErrorCode::MalformedData, "capability scalar payload is malformed");
            }
            char* end = nullptr;
            const double value = std::strtod(body.c_str(), &end);
            if (end == nullptr || *end != '\0') {
                return Status(ErrorCode::MalformedData, "capability scalar payload is malformed: '" + body + "'");
            }
            if (!std::isfinite(value)) {
                return Status(ErrorCode::NonFiniteQuantity, "capability scalar payload is not finite");
            }
            return CapabilityValue::scalar(value);
        }
        case CapabilityKind::Enumeration: {
            const Result<std::string> token = canonical_token(text, Limits::kMaxTokenBytes);
            if (!token.ok()) {
                return token.status();
            }
            return CapabilityValue::enumeration(*token);
        }
        case CapabilityKind::TokenSet: {
            std::vector<std::string> tokens = split_tokens(text);
            if (tokens.size() > Limits::kMaxTokensPerCapability) {
                return Status(ErrorCode::BoundsExceeded, "capability token set exceeds the permitted size");
            }
            if (!canonical_tokens_in_place(tokens, Limits::kMaxTokensPerCapability)) {
                return Status(ErrorCode::MalformedData, "capability token set contains a malformed token");
            }
            return CapabilityValue::token_set(std::move(tokens));
        }
    }
    return Status(ErrorCode::MalformedData, "capability value kind is not a known domain");
}

Status CapabilityValue::canonicalize() {
    switch (kind_) {
        case CapabilityKind::Presence:
            return Status::success();
        case CapabilityKind::Version:
            if (version_.major() > 100000U || version_.minor() > 100000U || version_.patch() > 100000U) {
                return Status(ErrorCode::ImpossibleQuantity, "capability version component is out of range");
            }
            return Status::success();
        case CapabilityKind::Integer:
            return Status::success();
        case CapabilityKind::Scalar:
            if (!std::isfinite(scalar_)) {
                return Status(ErrorCode::NonFiniteQuantity, "capability scalar is not finite");
            }
            if (scalar_ == 0.0) {
                scalar_ = 0.0;  // normalize -0.0
            }
            return Status::success();
        case CapabilityKind::Enumeration: {
            const Result<std::string> token = canonical_token(token_, Limits::kMaxTokenBytes);
            if (!token.ok()) {
                return token.status();
            }
            token_ = *token;
            return Status::success();
        }
        case CapabilityKind::TokenSet:
            if (!canonical_tokens_in_place(tokens_, Limits::kMaxTokensPerCapability)) {
                return Status(ErrorCode::MalformedData, "capability token set contains a malformed or surplus token");
            }
            return Status::success();
    }
    return Status(ErrorCode::MalformedData, "capability value kind is not a known domain");
}

std::string CapabilityValue::render() const {
    switch (kind_) {
        case CapabilityKind::Presence:
            return flag_ ? "present" : "absent";
        case CapabilityKind::Version:
            return version_.to_string();
        case CapabilityKind::Integer:
            return std::to_string(integer_);
        case CapabilityKind::Scalar: {
            char buffer[64];
            const int written = std::snprintf(buffer, sizeof(buffer), "%.9g", scalar_);
            return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
        }
        case CapabilityKind::Enumeration:
            return token_;
        case CapabilityKind::TokenSet:
            return join_tokens(tokens_);
    }
    return {};
}

bool CapabilityValue::equals(const CapabilityValue& other) const noexcept {
    if (kind_ != other.kind_) {
        return false;
    }
    switch (kind_) {
        case CapabilityKind::Presence:
            return flag_ == other.flag_;
        case CapabilityKind::Version:
            return version_ == other.version_;
        case CapabilityKind::Integer:
            return integer_ == other.integer_;
        case CapabilityKind::Scalar:
            return scalar_ == other.scalar_;
        case CapabilityKind::Enumeration:
            return token_ == other.token_;
        case CapabilityKind::TokenSet:
            return tokens_ == other.tokens_;
    }
    return false;
}

void CapabilityValue::serialize(ByteWriter& writer) const {
    writer.u8(static_cast<std::uint8_t>(kind_));
    switch (kind_) {
        case CapabilityKind::Presence:
            writer.boolean(flag_);
            break;
        case CapabilityKind::Version:
            writer.u32(version_.major());
            writer.u32(version_.minor());
            writer.u32(version_.patch());
            writer.string(version_.prerelease());
            break;
        case CapabilityKind::Integer:
            writer.i64(integer_);
            break;
        case CapabilityKind::Scalar:
            writer.f64(scalar_);
            break;
        case CapabilityKind::Enumeration:
            writer.string(token_);
            break;
        case CapabilityKind::TokenSet:
            writer.u32(static_cast<std::uint32_t>(tokens_.size()));
            for (const std::string& token : tokens_) {
                writer.string(token);
            }
            break;
    }
}

bool CapabilityValue::deserialize(ByteReader& reader, CapabilityValue& out) {
    std::uint8_t raw_kind = 0;
    if (!reader.u8(raw_kind)) {
        return false;
    }
    CapabilityKind kind = CapabilityKind::Presence;
    if (!capability_kind_from_wire(raw_kind, kind)) {
        reader.fail(ErrorCode::MalformedData, "capability value kind is outside the declared domain");
        return false;
    }
    return deserialize_of_kind(reader, kind, out);
}

bool CapabilityValue::deserialize_of_kind(ByteReader& reader, CapabilityKind kind, CapabilityValue& out) {
    CapabilityValue value(kind);
    switch (kind) {
        case CapabilityKind::Presence: {
            bool flag = false;
            if (!reader.boolean(flag)) {
                return false;
            }
            value.flag_ = flag;
            break;
        }
        case CapabilityKind::Version: {
            std::uint32_t major = 0;
            std::uint32_t minor = 0;
            std::uint32_t patch = 0;
            std::string prerelease;
            if (!reader.u32(major) || !reader.u32(minor) || !reader.u32(patch) ||
                !reader.string(prerelease, Limits::kMaxTokenBytes)) {
                return false;
            }
            SemanticVersion version(major, minor, patch);
            version.set_prerelease(std::move(prerelease));
            value.version_ = std::move(version);
            break;
        }
        case CapabilityKind::Integer: {
            std::int64_t integer = 0;
            if (!reader.i64(integer)) {
                return false;
            }
            value.integer_ = integer;
            break;
        }
        case CapabilityKind::Scalar: {
            double scalar = 0.0;
            if (!reader.f64(scalar)) {
                return false;
            }
            value.scalar_ = scalar;
            break;
        }
        case CapabilityKind::Enumeration: {
            std::string token;
            if (!reader.string(token, Limits::kMaxTokenBytes)) {
                return false;
            }
            value.token_ = std::move(token);
            break;
        }
        case CapabilityKind::TokenSet: {
            std::uint32_t count = 0;
            if (!reader.count(count, static_cast<std::uint32_t>(Limits::kMaxTokensPerCapability), 4)) {
                return false;
            }
            value.tokens_.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                std::string token;
                if (!reader.string(token, Limits::kMaxTokenBytes)) {
                    return false;
                }
                value.tokens_.push_back(std::move(token));
            }
            break;
        }
    }
    out = std::move(value);
    return true;
}

Status CapabilityRecord::validate() const {
    if (!key.valid()) {
        return Status(ErrorCode::UnknownCapability, "capability record holds an unresolved key");
    }
    if (value.kind() != key.kind()) {
        return Status(ErrorCode::CapabilityMismatch, "capability '" + key.name() + "' declares kind " +
                                                         std::string(to_string(key.kind())) +
                                                         " but carries a " + std::string(to_string(value.kind())) +
                                                         " payload");
    }
    if (state == CapabilityState::Supported || state == CapabilityState::Degraded) {
        if (value.kind() != CapabilityKind::Presence && value.render().empty()) {
            return Status(ErrorCode::MalformedData, "capability '" + key.name() + "' claims support with an empty payload");
        }
    }
    if (unit.size() > Limits::kMaxTokenBytes) {
        return Status(ErrorCode::BoundsExceeded, "capability unit exceeds the permitted length");
    }
    return Status::success();
}

void CapabilityRecord::serialize(ByteWriter& writer) const {
    writer.string(key.name());
    writer.u8(static_cast<std::uint8_t>(state));
    writer.string(unit);
    value.serialize(writer);
}

bool CapabilityRecord::deserialize(ByteReader& reader, CapabilityRecord& out) {
    std::string name;
    std::uint8_t raw_state = 0;
    std::string unit;
    if (!reader.string(name, Limits::kMaxNameBytes)) {
        return false;
    }
    if (!reader.u8(raw_state)) {
        return false;
    }
    CapabilityState state = CapabilityState::Unknown;
    if (!capability_state_from_wire(raw_state, state)) {
        reader.fail(ErrorCode::MalformedData, "capability state is outside the declared domain for '" + name + "'");
        return false;
    }
    if (!reader.string(unit, Limits::kMaxTokenBytes)) {
        return false;
    }
    const Result<CapabilityKey> key = capability_key_from_name(name);
    if (!key.ok()) {
        reader.fail(key.status().code(), key.status().message());
        return false;
    }
    // The value carries its own kind tag on the wire; the reader validates that
    // it agrees with the kind declared by the key rather than assuming it.
    CapabilityValue value;
    if (!CapabilityValue::deserialize(reader, value)) {
        return false;
    }
    if (value.kind() != key->kind()) {
        reader.fail(ErrorCode::CapabilityMismatch,
                    "capability '" + name + "' declares kind " + std::string(to_string(key->kind())) +
                        " but carries a " + std::string(to_string(value.kind())) + " payload");
        return false;
    }
    CapabilityRecord record;
    record.key = *key;
    record.state = state;
    record.value = std::move(value);
    record.unit = std::move(unit);
    const Status canonical = record.value.canonicalize();
    if (!canonical.ok()) {
        reader.fail(canonical.code(), canonical.message());
        return false;
    }
    out = std::move(record);
    return true;
}

Status CapabilitySet::set_records(std::vector<CapabilityRecord> records) {
    if (records.size() > Limits::kMaxCapabilitiesPerSet) {
        return Status(ErrorCode::BoundsExceeded, "capability set exceeds the permitted number of records");
    }
    std::vector<CapabilityRecord> working;
    working.reserve(records.size());
    for (CapabilityRecord& record : records) {
        const Status canonical = record.value.canonicalize();
        if (!canonical.ok()) {
            return canonical;
        }
        const Status valid = record.validate();
        if (!valid.ok()) {
            return valid;
        }
        working.push_back(std::move(record));
    }
    std::sort(working.begin(), working.end(), [](const CapabilityRecord& a, const CapabilityRecord& b) {
        return a.key.name() < b.key.name();
    });
    for (std::size_t i = 1; i < working.size(); ++i) {
        if (working[i].key.name() == working[i - 1].key.name()) {
            return Status(ErrorCode::DuplicateIdentity,
                          "capability set contains duplicate key '" + working[i].key.name() + "'");
        }
    }
    records_ = std::move(working);
    refresh_identity();
    return Status::success();
}

Status CapabilitySet::set_closed_namespaces(std::vector<std::string> namespaces) {
    if (namespaces.size() > Limits::kMaxCapabilitiesPerSet) {
        return Status(ErrorCode::BoundsExceeded, "closed namespace list exceeds the permitted size");
    }
    std::vector<std::string> working;
    working.reserve(namespaces.size());
    for (std::string& name_space : namespaces) {
        const Result<std::string> canonical = canonical_token(name_space, Limits::kMaxNameBytes);
        if (!canonical.ok()) {
            return canonical.status();
        }
        if (canonical->find('.') != std::string::npos) {
            return Status(ErrorCode::InvalidArgument, "closed namespace must be a single token: '" + *canonical + "'");
        }
        working.push_back(*canonical);
    }
    std::sort(working.begin(), working.end());
    working.erase(std::unique(working.begin(), working.end()), working.end());
    closed_namespaces_ = std::move(working);
    refresh_identity();
    return Status::success();
}

CapabilityLookup CapabilitySet::lookup(std::string_view canonical_key_name) const {
    CapabilityLookup result;
    const auto found = std::lower_bound(
        records_.begin(), records_.end(), canonical_key_name,
        [](const CapabilityRecord& record, std::string_view name) { return record.key.name() < name; });
    if (found != records_.end() && found->key.name() == canonical_key_name) {
        result.state = found->state;
        result.value = &found->value;
        result.present = true;
        return result;
    }
    const std::size_t dot = canonical_key_name.find('.');
    const std::string_view name_space =
        dot == std::string_view::npos ? canonical_key_name : canonical_key_name.substr(0, dot);
    const auto closed =
        std::lower_bound(closed_namespaces_.begin(), closed_namespaces_.end(), name_space);
    if (closed != closed_namespaces_.end() && *closed == name_space) {
        result.state = CapabilityState::Unsupported;
        result.namespace_closed = true;
    } else {
        result.state = CapabilityState::Unknown;
    }
    return result;
}

CapabilityLookup CapabilitySet::lookup(const CapabilityKey& key) const { return lookup(key.name()); }

Status CapabilitySet::validate() const {
    if (records_.size() > Limits::kMaxCapabilitiesPerSet) {
        return Status(ErrorCode::BoundsExceeded, "capability set exceeds the permitted number of records");
    }
    std::string previous;
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const CapabilityRecord& record = records_[i];
        const Status valid = record.validate();
        if (!valid.ok()) {
            return valid;
        }
        if (i != 0 && !(previous < record.key.name())) {
            return Status(ErrorCode::MalformedData, "capability set is not in canonical key order");
        }
        previous = record.key.name();
    }
    for (std::size_t i = 1; i < closed_namespaces_.size(); ++i) {
        if (!(closed_namespaces_[i - 1] < closed_namespaces_[i])) {
            return Status(ErrorCode::MalformedData, "closed namespace list is not in canonical order");
        }
    }
    return Status::success();
}

Sha256::digest_type CapabilitySet::digest() const {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(records_.size()));
    for (const CapabilityRecord& record : records_) {
        record.serialize(writer);
    }
    writer.u32(static_cast<std::uint32_t>(closed_namespaces_.size()));
    for (const std::string& name_space : closed_namespaces_) {
        writer.string(name_space);
    }
    const ByteBuffer& bytes = writer.data();
    return Sha256::hash(bytes.data(), bytes.size());
}

std::string CapabilitySet::digest_hex() const { return to_hex(digest()); }

void CapabilitySet::refresh_identity() { id_ = CapabilitySetId::from_raw(derive_identity("haf.capability-set", digest_hex())); }

void CapabilitySet::serialize(ByteWriter& writer) const {
    writer.id128(id_.raw());
    writer.generation(generation_);
    writer.u32(static_cast<std::uint32_t>(records_.size()));
    for (const CapabilityRecord& record : records_) {
        record.serialize(writer);
    }
    writer.u32(static_cast<std::uint32_t>(closed_namespaces_.size()));
    for (const std::string& name_space : closed_namespaces_) {
        writer.string(name_space);
    }
}

bool CapabilitySet::deserialize(ByteReader& reader, CapabilitySet& out, bool persisted_scale) {
    const std::uint32_t max_records =
        persisted_scale ? static_cast<std::uint32_t>(Limits::kMaxCapabilitiesPerSet)
                        : static_cast<std::uint32_t>(Limits::kMaxCapabilitiesPerSet);
    Id128 raw_id;
    CapabilityGeneration generation;
    if (!reader.id128(raw_id) || !reader.generation(generation)) {
        return false;
    }
    std::uint32_t count = 0;
    if (!reader.count(count, max_records, 8)) {
        return false;
    }
    std::vector<CapabilityRecord> records;
    records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        CapabilityRecord record;
        if (!CapabilityRecord::deserialize(reader, record)) {
            return false;
        }
        records.push_back(std::move(record));
    }
    std::uint32_t namespace_count = 0;
    if (!reader.count(namespace_count, max_records, 4)) {
        return false;
    }
    std::vector<std::string> namespaces;
    namespaces.reserve(namespace_count);
    for (std::uint32_t i = 0; i < namespace_count; ++i) {
        std::string name_space;
        if (!reader.string(name_space, Limits::kMaxNameBytes)) {
            return false;
        }
        namespaces.push_back(std::move(name_space));
    }
    CapabilitySet set;
    const Status records_status = set.set_records(std::move(records));
    if (!records_status.ok()) {
        reader.fail(records_status.code(), records_status.message());
        return false;
    }
    const Status namespaces_status = set.set_closed_namespaces(std::move(namespaces));
    if (!namespaces_status.ok()) {
        reader.fail(namespaces_status.code(), namespaces_status.message());
        return false;
    }
    set.set_generation(generation);
    // A decoders' recomputed identity may differ from the wire value only when
    // the payload is inconsistent; surface that as corruption.
    if (set.id() != CapabilitySetId::from_raw(raw_id)) {
        reader.fail(ErrorCode::IntegrityFailure, "capability set identity does not match its content digest");
        return false;
    }
    out = std::move(set);
    return true;
}

CapabilityRecord make_presence(std::string_view key, CapabilityState state) {
    CapabilityRecord record;
    const Result<CapabilityKey> resolved = capability_key_from_name(key);
    if (resolved.ok()) {
        record.key = *resolved;
    }
    record.state = state;
    record.value = CapabilityValue::presence();
    return record;
}

Result<CapabilityRecord> make_integer(std::string_view key, std::int64_t value, CapabilityState state) {
    const Result<CapabilityKey> resolved = capability_key_from_name(key);
    if (!resolved.ok()) {
        return resolved.status();
    }
    if (resolved->kind() != CapabilityKind::Integer) {
        return Status(ErrorCode::CapabilityMismatch, "capability '" + std::string(key) + "' is not an integer capability");
    }
    CapabilityRecord record;
    record.key = *resolved;
    record.state = state;
    record.value = CapabilityValue::integer(value);
    return record;
}

Result<CapabilityRecord> make_version(std::string_view key, std::string_view version, CapabilityState state) {
    const Result<CapabilityKey> resolved = capability_key_from_name(key);
    if (!resolved.ok()) {
        return resolved.status();
    }
    if (resolved->kind() != CapabilityKind::Version) {
        return Status(ErrorCode::CapabilityMismatch, "capability '" + std::string(key) + "' is not a version capability");
    }
    const std::optional<SemanticVersion> parsed = SemanticVersion::parse(version);
    if (!parsed.has_value()) {
        return Status(ErrorCode::MalformedData, "capability '" + std::string(key) + "' received a non-semantic version");
    }
    CapabilityRecord record;
    record.key = *resolved;
    record.state = state;
    record.value = CapabilityValue::version(*parsed);
    return record;
}

Result<CapabilityRecord> make_enumeration(std::string_view key, std::string_view token, CapabilityState state) {
    const Result<CapabilityKey> resolved = capability_key_from_name(key);
    if (!resolved.ok()) {
        return resolved.status();
    }
    if (resolved->kind() != CapabilityKind::Enumeration) {
        return Status(ErrorCode::CapabilityMismatch, "capability '" + std::string(key) + "' is not an enumeration");
    }
    const Result<std::string> canonical = canonical_token(token, Limits::kMaxTokenBytes);
    if (!canonical.ok()) {
        return canonical.status();
    }
    CapabilityRecord record;
    record.key = *resolved;
    record.state = state;
    record.value = CapabilityValue::enumeration(*canonical);
    return record;
}

Result<CapabilityRecord> make_token_set(std::string_view key, std::vector<std::string> tokens, CapabilityState state) {
    const Result<CapabilityKey> resolved = capability_key_from_name(key);
    if (!resolved.ok()) {
        return resolved.status();
    }
    if (resolved->kind() != CapabilityKind::TokenSet) {
        return Status(ErrorCode::CapabilityMismatch, "capability '" + std::string(key) + "' is not a token set");
    }
    CapabilityValue value = CapabilityValue::token_set(std::move(tokens));
    const Status canonical = value.canonicalize();
    if (!canonical.ok()) {
        return canonical;
    }
    CapabilityRecord record;
    record.key = *resolved;
    record.state = state;
    record.value = std::move(value);
    return record;
}

}  // namespace haf
