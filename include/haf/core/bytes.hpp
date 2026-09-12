// Heterogeneous Accelerator Federation - bounded, deterministic byte codec.
//
// Serialization is explicit little-endian, length-prefixed, and defensive:
// every read validates bounds before allocating, and every count is clamped by
// a caller-supplied limit. No length field from an untrusted source can drive
// an unbounded allocation.

#ifndef HAF_CORE_BYTES_HPP
#define HAF_CORE_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"

namespace haf {

using ByteBuffer = std::vector<std::uint8_t>;

class ByteWriter {
public:
    ByteWriter() = default;

    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void boolean(bool value) { u8(value ? 1U : 0U); }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f64(double value);

    /// Length-prefixed byte string. The caller is responsible for having
    /// validated the semantic limit beforehand.
    void bytes(const std::uint8_t* data, std::size_t size);
    void bytes(const ByteBuffer& buffer) { bytes(buffer.data(), buffer.size()); }
    void string(std::string_view text) { bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

    void raw(const std::uint8_t* data, std::size_t size);
    void id128(const Id128& value) { raw(value.bytes().data(), value.bytes().size()); }

    template <class Tag>
    void strong_id(const StrongId<Tag>& value) { id128(value.raw()); }

    template <class Tag>
    void generation(const Generation<Tag>& value) { u64(value.value()); }

    [[nodiscard]] const ByteBuffer& data() const noexcept { return bytes_; }
    [[nodiscard]] ByteBuffer take() noexcept { return std::move(bytes_); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

private:
    ByteBuffer bytes_;
};

/// Bounded reader. Every accessor first checks that the requested bytes exist.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    explicit ByteReader(const ByteBuffer& buffer) noexcept : data_(buffer.data()), size_(buffer.size()) {}

    [[nodiscard]] bool good() const noexcept { return !failed_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return failed_ ? 0 : size_ - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] const Status& error() const noexcept { return error_; }

    [[nodiscard]] bool u8(std::uint8_t& out);
    [[nodiscard]] bool boolean(bool& out);
    [[nodiscard]] bool u16(std::uint16_t& out);
    [[nodiscard]] bool u32(std::uint32_t& out);
    [[nodiscard]] bool u64(std::uint64_t& out);
    [[nodiscard]] bool i64(std::int64_t& out);
    [[nodiscard]] bool f64(double& out);

    /// Reads a length-prefixed byte string, rejecting a declared length above
    /// the caller supplied maximum or above the remaining buffer size.
    [[nodiscard]] bool bytes(ByteBuffer& out, std::size_t max_bytes);
    [[nodiscard]] bool string(std::string& out, std::size_t max_bytes);

    [[nodiscard]] bool raw(std::uint8_t* out, std::size_t size);
    [[nodiscard]] bool skip(std::size_t size);
    [[nodiscard]] bool id128(Id128& out);

    template <class Tag>
    [[nodiscard]] bool strong_id(StrongId<Tag>& out) {
        Id128 raw_value;
        if (!id128(raw_value)) {
            return false;
        }
        out = StrongId<Tag>::from_raw(raw_value);
        return true;
    }

    template <class Tag>
    [[nodiscard]] bool generation(Generation<Tag>& out) {
        std::uint64_t raw_value = 0;
        if (!u64(raw_value)) {
            return false;
        }
        out = Generation<Tag>(raw_value);
        return true;
    }

    /// Reads a count that will be used to size a loop. Rejects counts above
    /// the caller supplied maximum and counts that cannot possibly fit in the remainder.
    [[nodiscard]] bool count(std::uint32_t& out, std::uint32_t max_count, std::size_t min_bytes_per_item = 1);

    void fail(ErrorCode code, std::string message);

private:
    [[nodiscard]] bool require(std::size_t size);

    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
    bool failed_{false};
    Status error_;
};

/// Canonical escaped rendering used by deterministic text output.
[[nodiscard]] std::string escape_text(std::string_view text);

/// True when p text is valid UTF-8 (including the empty string).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace haf

#endif  // HAF_CORE_BYTES_HPP
