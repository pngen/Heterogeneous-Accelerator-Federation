#include "haf/core/idgen.hpp"

#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#include "haf/core/hash.hpp"
#include "haf/core/time.hpp"

namespace haf {
namespace {

/// SplitMix64: full-period 64-bit generator with strong avalanche behaviour.
[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

}  // namespace

Id128 IdGenerator::next() noexcept {
    if (!started_) {
        started_ = true;
        state_ = seed_ ^ 0xD1B54A32D192ED03ULL;
    }
    Id128::bytes_type bytes{};
    // Two independent 64-bit draws plus a counter mix. The counter guarantees
    // that a repeated internal state can never re-emit an earlier identity.
    const std::uint64_t first = splitmix64(state_);
    const std::uint64_t second = splitmix64(state_);
    ++counter_;
    const std::uint64_t third = splitmix64(state_) ^ (counter_ * 0xA24BAED4963EE407ULL);
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((first >> (i * 8)) & 0xFFULL);
        bytes[static_cast<std::size_t>(i) + 8] = static_cast<std::uint8_t>((second >> (i * 8)) & 0xFFULL);
    }
    bytes[0] = static_cast<std::uint8_t>(bytes[0] ^ static_cast<std::uint8_t>(third & 0xFFULL));
    bytes[15] = static_cast<std::uint8_t>(bytes[15] ^ static_cast<std::uint8_t>((third >> 8) & 0xFFULL));
    if (Id128(bytes).is_nil()) {
        bytes[15] = 1U;
    }
    return Id128(bytes);
}

IdGenerator make_entropy_id_generator() {
    std::random_device device;
    const std::uint64_t entropy = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    const std::uint64_t clock = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::uint64_t thread_hash = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::uint64_t process_hash = Fnv1a64{}.digest();
    return IdGenerator(entropy ^ (clock * 0x9E3779B97F4A7C15ULL) ^ (thread_hash << 1) ^ process_hash);
}

IdGenerator make_deterministic_id_generator(std::uint64_t seed) { return IdGenerator(seed); }

}  // namespace haf
