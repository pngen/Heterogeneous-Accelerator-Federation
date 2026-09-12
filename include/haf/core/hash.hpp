// Heterogeneous Accelerator Federation - canonical hashing.
//
// Two independent primitives are provided:
//   * FNV-1a 64 for cheap in-memory keys and bucket selection.
//   * SHA-256 for integrity protection of persisted state, evidence digests,
//     snapshots, and decision fingerprints.
//
// Hashes are always computed over canonicalized byte sequences so that
// equivalent logical state produces an identical digest.

#ifndef HAF_CORE_HASH_HPP
#define HAF_CORE_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace haf {

/// Incremental FNV-1a 64-bit hash.
class Fnv1a64 {
public:
    static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;

    Fnv1a64() noexcept = default;

    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }

    template <class T>
    void update_pod(const T& value) noexcept {
        static_assert(std::is_trivially_copyable<T>::value, "update_pod requires a trivially copyable type");
        update(&value, sizeof(T));
    }

    void update_u64(std::uint64_t value) noexcept;
    void update_u32(std::uint32_t value) noexcept;

    [[nodiscard]] std::uint64_t digest() const noexcept { return state_; }

private:
    std::uint64_t state_{kOffsetBasis};
};

/// Incremental SHA-256. Used for integrity protection and fingerprinting.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;
    using digest_type = std::array<std::uint8_t, kDigestBytes>;

    Sha256() noexcept;

    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    void update_u64(std::uint64_t value) noexcept;
    void update_u32(std::uint32_t value) noexcept;

    /// Finalize. The object must not be reused afterwards without reset().
    [[nodiscard]] digest_type finalize() noexcept;
    void reset() noexcept;

    [[nodiscard]] static digest_type hash(const void* data, std::size_t size) noexcept;
    [[nodiscard]] static digest_type hash(std::string_view text) noexcept {
        return hash(text.data(), text.size());
    }

private:
    void compress(const std::uint8_t block[64]) noexcept;

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffered_;
    std::uint64_t total_bits_;
};

[[nodiscard]] std::string to_hex(const Sha256::digest_type& digest);

}  // namespace haf

#endif  // HAF_CORE_HASH_HPP
