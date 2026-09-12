// Heterogeneous Accelerator Federation - capability semantics.
//
// The semantic states are deliberately minimal and total:
//
//   SUPPORTED    - the advertiser positively claims the capability.
//   UNSUPPORTED  - the advertiser positively claims the absence.
//   DEGRADED     - the capability exists but is currently impaired.
//   UNKNOWN      - no evidence. This is the state of every capability that was
//                  not advertised inside a namespace the advertiser declared
//                  closed.
//
// UNKNOWN always fails a hard requirement. A device can never become eligible
// because it omitted evidence.

#ifndef HAF_MODEL_CAPABILITY_HPP
#define HAF_MODEL_CAPABILITY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/version.hpp"
#include "haf/model/capability_key.hpp"

namespace haf {

enum class CapabilityState : std::uint8_t {
    Unknown = 0,
    Supported = 1,
    Unsupported = 2,
    Degraded = 3,
};

[[nodiscard]] std::string_view to_string(CapabilityState state) noexcept;
[[nodiscard]] bool capability_state_from_wire(std::uint8_t raw, CapabilityState& out) noexcept;

/// Canonical capability payload. The active member is determined by kind().
class CapabilityValue {
public:
    CapabilityValue() = default;
    explicit CapabilityValue(CapabilityKind kind) noexcept : kind_(kind) {}

    [[nodiscard]] static CapabilityValue presence() noexcept;
    [[nodiscard]] static CapabilityValue version(SemanticVersion value);
    [[nodiscard]] static CapabilityValue integer(std::int64_t value) noexcept;
    [[nodiscard]] static CapabilityValue scalar(double value) noexcept;
    [[nodiscard]] static CapabilityValue enumeration(std::string token);
    [[nodiscard]] static CapabilityValue token_set(std::vector<std::string> tokens);

    /// Parse a textual payload of a declared kind. Tokens are canonicalized;
    /// numbers must be integral/finite; versions must be semantic versions.
    [[nodiscard]] static Result<CapabilityValue> parse(CapabilityKind kind, std::string_view text);

    /// Canonicalize in place. Tokens are lowercased, trimmed, sorted, and
    /// deduplicated; -0.0 becomes 0.0; non-finite scalars are rejected.
    [[nodiscard]] Status canonicalize();

    [[nodiscard]] CapabilityKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == CapabilityKind::Presence && !flag_; }

    [[nodiscard]] bool flag() const noexcept { return flag_; }
    [[nodiscard]] const SemanticVersion& version_value() const noexcept { return version_; }
    [[nodiscard]] std::int64_t integer_value() const noexcept { return integer_; }
    [[nodiscard]] double scalar_value() const noexcept { return scalar_; }
    [[nodiscard]] const std::string& token() const noexcept { return token_; }
    [[nodiscard]] const std::vector<std::string>& tokens() const noexcept { return tokens_; }

    /// Deterministic canonical rendering used by text output and by hashing.
    [[nodiscard]] std::string render() const;

    /// Structural equality over canonicalized payloads.
    [[nodiscard]] bool equals(const CapabilityValue& other) const noexcept;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CapabilityValue& out);
    [[nodiscard]] static bool deserialize_of_kind(ByteReader& reader, CapabilityKind kind, CapabilityValue& out);

private:
    CapabilityKind kind_{CapabilityKind::Presence};
    bool flag_{false};
    SemanticVersion version_{};
    std::int64_t integer_{0};
    double scalar_{0.0};
    std::string token_;
    std::vector<std::string> tokens_;
};

/// A single advertised capability.
struct CapabilityRecord {
    CapabilityKey key{};
    CapabilityState state{CapabilityState::Unknown};
    CapabilityValue value{};
    std::string unit;

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CapabilityRecord& out);
    [[nodiscard]] Status validate() const;
};

/// Result of looking a capability up in a set. Absence of a record inside a
/// namespace the advertiser declared closed is positive UNSUPPORTED evidence;
/// absence anywhere else is UNKNOWN.
struct CapabilityLookup {
    CapabilityState state{CapabilityState::Unknown};
    const CapabilityValue* value{nullptr};
    bool present{false};
    bool namespace_closed{false};
};

/// An accelerator capability set: a canonical, content-addressed collection of
/// advertised capabilities plus the namespaces the advertiser speaks for
/// authoritatively.
class CapabilitySet {
public:
    CapabilitySet() = default;

    [[nodiscard]] const CapabilitySetId& id() const noexcept { return id_; }
    [[nodiscard]] CapabilityGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] const std::vector<CapabilityRecord>& records() const noexcept { return records_; }
    [[nodiscard]] const std::vector<std::string>& closed_namespaces() const noexcept { return closed_namespaces_; }

    void set_generation(CapabilityGeneration generation) noexcept { generation_ = generation; }

    /// Replace all records. Records are canonicalized, validated, sorted by
    /// canonical key name, and de-duplicated.
    [[nodiscard]] Status set_records(std::vector<CapabilityRecord> records);
    [[nodiscard]] Status set_closed_namespaces(std::vector<std::string> namespaces);

    /// Look up by canonical key name. Unknown names yield UNKNOWN rather than
    /// an error so that callers can express "evidence missing".
    [[nodiscard]] CapabilityLookup lookup(std::string_view canonical_key_name) const;

    /// Look up by resolved key.
    [[nodiscard]] CapabilityLookup lookup(const CapabilityKey& key) const;

    /// Validate the whole set: bounds, key/value kind agreement, duplicate
    /// keys, canonical ordering, finite numbers, canonical tokens.
    [[nodiscard]] Status validate() const;

    /// Deterministic content digest over the canonical encoding.
    [[nodiscard]] Sha256::digest_type digest() const;
    [[nodiscard]] std::string digest_hex() const;

    /// Recompute the content-addressed identity from the digest.
    void refresh_identity();

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

    void serialize(ByteWriter& writer) const;
    [[nodiscard]] static bool deserialize(ByteReader& reader, CapabilitySet& out, bool persisted_scale);

private:
    CapabilitySetId id_{};
    CapabilityGeneration generation_{};
    std::vector<CapabilityRecord> records_;
    std::vector<std::string> closed_namespaces_;
};

/// Convenience builders used by adapters and tests.
[[nodiscard]] CapabilityRecord make_presence(std::string_view key, CapabilityState state);
[[nodiscard]] Result<CapabilityRecord> make_integer(std::string_view key, std::int64_t value, CapabilityState state);
[[nodiscard]] Result<CapabilityRecord> make_version(std::string_view key, std::string_view version, CapabilityState state);
[[nodiscard]] Result<CapabilityRecord> make_enumeration(std::string_view key, std::string_view token, CapabilityState state);
[[nodiscard]] Result<CapabilityRecord> make_token_set(std::string_view key, std::vector<std::string> tokens, CapabilityState state);

}  // namespace haf

#endif  // HAF_MODEL_CAPABILITY_HPP
