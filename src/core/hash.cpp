#include "haf/core/hash.hpp"

#include <cstring>

namespace haf {

void Fnv1a64::update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        state_ ^= static_cast<std::uint64_t>(bytes[i]);
        state_ *= kPrime;
    }
}

void Fnv1a64::update_u64(std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<std::uint8_t>((value >> shift) & 0xFFULL);
        state_ ^= static_cast<std::uint64_t>(byte);
        state_ *= kPrime;
    }
}

void Fnv1a64::update_u32(std::uint32_t value) noexcept { update_u64(static_cast<std::uint64_t>(value)); }

namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

}  // namespace

Sha256::Sha256() noexcept : state_{}, buffer_{}, buffered_(0), total_bits_(0) { reset(); }

void Sha256::reset() noexcept {
    state_[0] = 0x6a09e667U;
    state_[1] = 0xbb67ae85U;
    state_[2] = 0x3c6ef372U;
    state_[3] = 0xa54ff53aU;
    state_[4] = 0x510e527fU;
    state_[5] = 0x9b05688cU;
    state_[6] = 0x1f83d9abU;
    state_[7] = 0x5be0cd19U;
    buffered_ = 0;
    total_bits_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_bits_ += static_cast<std::uint64_t>(size) * 8ULL;
    std::size_t offset = 0;
    if (buffered_ != 0) {
        while (offset < size && buffered_ < 64) {
            buffer_[buffered_++] = bytes[offset++];
        }
        if (buffered_ == 64) {
            compress(buffer_);
            buffered_ = 0;
        }
    }
    while (size - offset >= 64) {
        compress(bytes + offset);
        offset += 64;
    }
    while (offset < size) {
        buffer_[buffered_++] = bytes[offset++];
    }
}

void Sha256::update_u64(std::uint64_t value) noexcept {
    std::uint8_t raw[8];
    for (int i = 0; i < 8; ++i) {
        raw[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFULL);
    }
    update(raw, sizeof(raw));
}

void Sha256::update_u32(std::uint32_t value) noexcept { update_u64(static_cast<std::uint64_t>(value)); }

Sha256::digest_type Sha256::finalize() noexcept {
    const std::uint64_t bits = total_bits_;
    const std::uint8_t pad = 0x80U;
    update(&pad, 1);
    const std::uint8_t zero = 0x00U;
    while (buffered_ != 56) {
        update(&zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFULL);
    }
    // Write the big-endian length without re-counting it in total_bits_.
    for (int i = 7; i >= 0; --i) {
        buffer_[buffered_++] = length_bytes[i];
    }
    compress(buffer_);
    buffered_ = 0;

    digest_type digest{};
    for (int i = 0; i < 8; ++i) {
        digest[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
    }
    return digest;
}

Sha256::digest_type Sha256::hash(const void* data, std::size_t size) noexcept {
    Sha256 hasher;
    hasher.update(data, size);
    return hasher.finalize();
}

std::string to_hex(const Sha256::digest_type& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = kDigits[(digest[i] >> 4) & 0x0FU];
        out[i * 2 + 1] = kDigits[digest[i] & 0x0FU];
    }
    return out;
}

}  // namespace haf
