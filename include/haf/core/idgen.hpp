// Heterogeneous Accelerator Federation - identity generation.
//
// Identities must never be reused. In production the generator is seeded from
// the operating system entropy source. In tests a fixed seed is supplied so
// that every generated identity is reproducible.

#ifndef HAF_CORE_IDGEN_HPP
#define HAF_CORE_IDGEN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"

namespace haf {

class IdGenerator {
public:
    explicit IdGenerator(std::uint64_t seed) noexcept : seed_(seed) {}

    /// Fresh identifier from the deterministic stream. The stream has a period
    /// far larger than any realistic process lifetime and never repeats a
    /// value within one generator instance.
    [[nodiscard]] Id128 next() noexcept;

    template <class Tag>
    [[nodiscard]] StrongId<Tag> next_id() noexcept {
        return StrongId<Tag>::from_raw(next());
    }

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

    /// Number of values produced so far.
    [[nodiscard]] std::uint64_t produced() const noexcept { return counter_; }

private:
    std::uint64_t seed_{0};
    std::uint64_t state_{0};
    std::uint64_t counter_{0};
    bool started_{false};
};

/// Generator seeded from OS entropy plus a process-unique salt. Used by the
/// runtime for real incarnations.
[[nodiscard]] IdGenerator make_entropy_id_generator();

/// Deterministic generator for reproducible tests and SYNTHETIC profiles.
[[nodiscard]] IdGenerator make_deterministic_id_generator(std::uint64_t seed);

/// Parse a 32-character hex identity string, or return a typed error.
template <class Tag>
[[nodiscard]] Result<StrongId<Tag>> parse_id(std::string_view hex) {
    const std::optional<StrongId<Tag>> parsed = StrongId<Tag>::parse(hex);
    if (!parsed.has_value()) {
        return Status(ErrorCode::InvalidArgument, "identity is not 32 lowercase/uppercase hex digits: " + std::string(hex));
    }
    return *parsed;
}

}  // namespace haf

#endif  // HAF_CORE_IDGEN_HPP
