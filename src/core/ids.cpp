#include "haf/core/ids.hpp"

namespace haf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

Id128 Id128::from_bytes(const std::uint8_t* data, std::size_t size) noexcept {
    Id128::bytes_type bytes{};
    const std::size_t limit = size < bytes.size() ? size : bytes.size();
    for (std::size_t i = 0; i < limit; ++i) {
        bytes[i] = data[i];
    }
    return Id128(bytes);
}

std::optional<Id128> Id128::parse(std::string_view hex) noexcept {
    if (hex.size() != kHexLength) {
        return std::nullopt;
    }
    Id128::bytes_type bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int high = hex_value(hex[i * 2]);
        const int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return Id128(bytes);
}

std::string Id128::to_hex() const {
    std::string out;
    out.resize(kHexLength);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        out[i * 2] = kHexDigits[(bytes_[i] >> 4) & 0x0FU];
        out[i * 2 + 1] = kHexDigits[bytes_[i] & 0x0FU];
    }
    return out;
}

const char* federation_id_kind_name() noexcept { return "haf.federation"; }

}  // namespace haf
