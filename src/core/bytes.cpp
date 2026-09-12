#include "haf/core/bytes.hpp"

#include <cmath>
#include <cstring>

namespace haf {

void ByteWriter::u16(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ByteWriter::f64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
}

void ByteWriter::bytes(const std::uint8_t* data, std::size_t size) {
    u32(static_cast<std::uint32_t>(size));
    raw(data, size);
}

void ByteWriter::raw(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    bytes_.insert(bytes_.end(), data, data + size);
}

bool ByteReader::require(std::size_t size) {
    if (failed_) {
        return false;
    }
    if (size > size_ - offset_) {
        fail(ErrorCode::TruncatedFrame, "buffer underrun while decoding");
        return false;
    }
    return true;
}

void ByteReader::fail(ErrorCode code, std::string message) {
    if (!failed_) {
        failed_ = true;
        error_ = Status(code, std::move(message));
    }
}

bool ByteReader::u8(std::uint8_t& out) {
    if (!require(1)) {
        return false;
    }
    out = data_[offset_++];
    return true;
}

bool ByteReader::boolean(bool& out) {
    std::uint8_t raw = 0;
    if (!u8(raw)) {
        return false;
    }
    if (raw > 1U) {
        fail(ErrorCode::MalformedData, "boolean field holds a value other than 0 or 1");
        return false;
    }
    out = raw == 1U;
    return true;
}

bool ByteReader::u16(std::uint16_t& out) {
    if (!require(2)) {
        return false;
    }
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_]) |
                                     static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8));
    offset_ += 2;
    return true;
}

bool ByteReader::u32(std::uint32_t& out) {
    if (!require(4)) {
        return false;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (i * 8);
    }
    offset_ += 4;
    out = value;
    return true;
}

bool ByteReader::u64(std::uint64_t& out) {
    if (!require(8)) {
        return false;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (i * 8);
    }
    offset_ += 8;
    out = value;
    return true;
}

bool ByteReader::i64(std::int64_t& out) {
    std::uint64_t raw = 0;
    if (!u64(raw)) {
        return false;
    }
    out = static_cast<std::int64_t>(raw);
    return true;
}

bool ByteReader::f64(double& out) {
    std::uint64_t bits = 0;
    if (!u64(bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    if (!std::isfinite(out)) {
        fail(ErrorCode::NonFiniteQuantity, "decoded floating point quantity is not finite");
        return false;
    }
    return true;
}

bool ByteReader::bytes(ByteBuffer& out, std::size_t max_bytes) {
    std::uint32_t declared = 0;
    if (!u32(declared)) {
        return false;
    }
    if (declared > max_bytes) {
        fail(ErrorCode::BoundsExceeded, "declared byte length exceeds the permitted maximum");
        return false;
    }
    if (!require(declared)) {
        return false;
    }
    out.assign(data_ + offset_, data_ + offset_ + declared);
    offset_ += declared;
    return true;
}

bool ByteReader::string(std::string& out, std::size_t max_bytes) {
    std::uint32_t declared = 0;
    if (!u32(declared)) {
        return false;
    }
    if (declared > max_bytes) {
        fail(ErrorCode::BoundsExceeded, "declared string length exceeds the permitted maximum");
        return false;
    }
    if (!require(declared)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + offset_), declared);
    offset_ += declared;
    return true;
}

bool ByteReader::raw(std::uint8_t* out, std::size_t size) {
    if (!require(size)) {
        return false;
    }
    if (size != 0) {
        std::memcpy(out, data_ + offset_, size);
    }
    offset_ += size;
    return true;
}

bool ByteReader::skip(std::size_t size) {
    if (!require(size)) {
        return false;
    }
    offset_ += size;
    return true;
}

bool ByteReader::id128(Id128& out) {
    Id128::bytes_type raw_bytes{};
    if (!raw(raw_bytes.data(), raw_bytes.size())) {
        return false;
    }
    out = Id128(raw_bytes);
    return true;
}

bool ByteReader::count(std::uint32_t& out, std::uint32_t max_count, std::size_t min_bytes_per_item) {
    std::uint32_t declared = 0;
    if (!u32(declared)) {
        return false;
    }
    if (declared > max_count) {
        fail(ErrorCode::BoundsExceeded, "declared item count exceeds the permitted maximum");
        return false;
    }
    if (min_bytes_per_item != 0) {
        const std::uint64_t needed = static_cast<std::uint64_t>(declared) * static_cast<std::uint64_t>(min_bytes_per_item);
        if (needed > static_cast<std::uint64_t>(remaining())) {
            fail(ErrorCode::TruncatedFrame, "declared item count cannot fit in the remaining buffer");
            return false;
        }
    }
    out = declared;
    return true;
}

std::string escape_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == static_cast<unsigned char>('"') || c == static_cast<unsigned char>('\\')) {
            out.push_back('\\');
            out.push_back(raw);
        } else if (c == static_cast<unsigned char>('\n')) {
            out += "\\n";
        } else if (c == static_cast<unsigned char>('\r')) {
            out += "\\r";
        } else if (c == static_cast<unsigned char>('\t')) {
            out += "\\t";
        } else if (c < 0x20U || c == 0x7FU) {
            static constexpr char kDigits[] = "0123456789abcdef";
            out += "\\x";
            out.push_back(kDigits[(c >> 4) & 0x0FU]);
            out.push_back(kDigits[c & 0x0FU]);
        } else {
            out.push_back(raw);
        }
    }
    return out;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t size = text.size();
    while (i < size) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t codepoint = 0;
        if (c < 0x80U) {
            ++i;
            continue;
        }
        if ((c & 0xE0U) == 0xC0U) {
            extra = 1;
            codepoint = c & 0x1FU;
        } else if ((c & 0xF0U) == 0xE0U) {
            extra = 2;
            codepoint = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            extra = 3;
            codepoint = c & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= size + 1 && i + extra > size) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if (i + k >= size) {
                return false;
            }
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (cc & 0x3FU);
        }
        if (extra == 1 && codepoint < 0x80U) {
            return false;
        }
        if (extra == 2 && codepoint < 0x800U) {
            return false;
        }
        if (extra == 3 && codepoint < 0x10000U) {
            return false;
        }
        if (codepoint > 0x10FFFFU) {
            return false;
        }
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace haf
