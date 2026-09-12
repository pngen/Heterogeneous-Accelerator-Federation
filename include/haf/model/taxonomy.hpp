// Heterogeneous Accelerator Federation - vendor and architecture taxonomy.
//
// Vendors and architectures are represented by strongly typed identities whose
// value is a pure deterministic function of a canonical lowercase token. Two
// processes that independently intern the token "nvidia" always derive the
// same VendorId, so identities are comparable across the control plane without
// shipping a shared registry.
//
// The federation core does not privilege any vendor: taxonomy tokens are
// opaque beyond canonicalization. Vendor-specific meaning lives in adapters and
// in adapter-owned capability extension namespaces.

#ifndef HAF_MODEL_TAXONOMY_HPP
#define HAF_MODEL_TAXONOMY_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"

namespace haf {

/// Canonicalize a taxonomy token: trim ASCII whitespace, lowercase, and reject
/// characters that are not [a-z0-9._+-]. Returns a typed error otherwise.
[[nodiscard]] Result<std::string> canonical_token(std::string_view token, std::size_t max_bytes);

/// Canonicalize arbitrary human text into a taxonomy token: lowercase, every
/// character outside [a-z0-9._+-] becomes '-', runs of '-' collapse, leading and
/// trailing '-' are removed. Used by adapters that receive free-form vendor
/// product names. Returns a typed error when nothing usable remains.
[[nodiscard]] Result<std::string> slug_token(std::string_view text, std::size_t max_bytes);

/// Deterministic 128-bit identity derived from a domain separator and a
/// canonical token. Pure function: identical inputs always give identical
/// outputs, in any process, on any platform.
[[nodiscard]] Id128 derive_identity(std::string_view domain, std::string_view canonical_token) noexcept;

[[nodiscard]] VendorId vendor_id_from_token(std::string_view canonical_token) noexcept;
[[nodiscard]] ArchitectureId architecture_id_from_token(std::string_view canonical_token) noexcept;
[[nodiscard]] NodeId node_id_from_token(std::string_view canonical_token) noexcept;
[[nodiscard]] WorkloadClassId workload_class_id_from_token(std::string_view canonical_token) noexcept;

/// Well-known vendor tokens used by the shipped adapters. The core itself does
/// not treat these specially; they exist so that adapters and policy files
/// agree on spelling.
namespace vendors {
inline constexpr std::string_view kNvidia = "nvidia";
inline constexpr std::string_view kAmd = "amd";
inline constexpr std::string_view kIntel = "intel";
inline constexpr std::string_view kApple = "apple";
inline constexpr std::string_view kSynthetic = "synthetic";
}  // namespace vendors

}  // namespace haf

#endif  // HAF_MODEL_TAXONOMY_HPP
