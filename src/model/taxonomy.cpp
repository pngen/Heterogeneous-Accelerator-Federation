#include "haf/model/taxonomy.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>

#include "haf/core/hash.hpp"
#include "haf/core/limits.hpp"

namespace haf {
namespace {

[[nodiscard]] bool is_token_char(unsigned char c) noexcept {
    if (c >= static_cast<unsigned char>('a') && c <= static_cast<unsigned char>('z')) {
        return true;
    }
    if (c >= static_cast<unsigned char>('0') && c <= static_cast<unsigned char>('9')) {
        return true;
    }
    return c == static_cast<unsigned char>('.') || c == static_cast<unsigned char>('_') ||
           c == static_cast<unsigned char>('+') || c == static_cast<unsigned char>('-');
}

}  // namespace

Result<std::string> canonical_token(std::string_view token, std::size_t max_bytes) {
    std::size_t begin = 0;
    std::size_t end = token.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(token[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])) != 0) {
        --end;
    }
    if (begin == end) {
        return Status(ErrorCode::InvalidArgument, "taxonomy token is empty");
    }
    std::string out;
    out.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        const auto raw = static_cast<unsigned char>(token[i]);
        const auto lowered = static_cast<unsigned char>(std::tolower(raw));
        if (!is_token_char(lowered)) {
            return Status(ErrorCode::InvalidArgument,
                          "taxonomy token contains a character outside [a-z0-9._+-]: '" + std::string(token) + "'");
        }
        out.push_back(static_cast<char>(lowered));
    }
    if (out.size() > max_bytes) {
        return Status(ErrorCode::BoundsExceeded, "taxonomy token exceeds the permitted length");
    }
    return out;
}

Result<std::string> slug_token(std::string_view text, std::size_t max_bytes) {
    std::string out;
    out.reserve(text.size());
    bool pending_separator = false;
    for (const char raw : text) {
        const auto lowered = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(raw)));
        if (is_token_char(lowered)) {
            if (pending_separator && !out.empty()) {
                out.push_back('-');
            }
            pending_separator = false;
            out.push_back(static_cast<char>(lowered));
        } else {
            pending_separator = true;
        }
    }
    if (out.empty()) {
        return Status(ErrorCode::InvalidArgument, "text contains no character usable in a taxonomy token");
    }
    if (out.size() > max_bytes) {
        out.resize(max_bytes);
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
        if (out.empty()) {
            return Status(ErrorCode::BoundsExceeded, "taxonomy token is empty after truncation");
        }
    }
    return out;
}

Id128 derive_identity(std::string_view domain, std::string_view canonical_token_value) noexcept {
    // Two independent FNV-1a streams with distinct domain separation give a
    // 128-bit value that is stable, collision resistant enough for identity
    // derivation, and free of any process-local state.
    Fnv1a64 low;
    low.update(domain);
    low.update(std::string_view("\x1f"));
    low.update(canonical_token_value);

    Fnv1a64 high;
    high.update(std::string_view("haf.identity.v1"));
    high.update(canonical_token_value);
    high.update(domain);
    high.update(std::string_view("\x1e"));

    const std::uint64_t low_value = low.digest();
    const std::uint64_t high_value = high.digest();
    Id128::bytes_type bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((low_value >> (i * 8)) & 0xFFULL);
        bytes[static_cast<std::size_t>(i) + 8] = static_cast<std::uint8_t>((high_value >> (i * 8)) & 0xFFULL);
    }
    // Identity derivation must never produce the nil value, which is reserved
    // to mean "absent".
    if (low_value == 0 && high_value == 0) {
        bytes[0] = 1U;
    }
    return Id128(bytes);
}

VendorId vendor_id_from_token(std::string_view canonical_token_value) noexcept {
    return VendorId::from_raw(derive_identity("haf.vendor", canonical_token_value));
}

ArchitectureId architecture_id_from_token(std::string_view canonical_token_value) noexcept {
    return ArchitectureId::from_raw(derive_identity("haf.architecture", canonical_token_value));
}

NodeId node_id_from_token(std::string_view canonical_token_value) noexcept {
    return NodeId::from_raw(derive_identity("haf.node", canonical_token_value));
}

WorkloadClassId workload_class_id_from_token(std::string_view canonical_token_value) noexcept {
    return WorkloadClassId::from_raw(derive_identity("haf.workload-class", canonical_token_value));
}

}  // namespace haf
