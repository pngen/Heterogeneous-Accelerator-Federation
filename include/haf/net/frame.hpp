// Heterogeneous Accelerator Federation - control-plane framing.
//
// Wire layout of one frame (all integers little-endian):
//
//   offset  size  field
//   0       4     magic 'H','A','F','1'
//   4       2     protocol version
//   6       2     message type
//   8       2     flags
//   10      2     reserved, must be zero
//   12      8     sequence number
//   20      4     payload length
//   24      4     CRC-32 (IEEE) over bytes 0..23
//   28      N     payload
//   28+N    8     FNV-1a 64 over bytes 0..27+N
//
// Every field is validated before the payload is handed to a decoder: magic,
// protocol version, reserved word, header CRC, payload length against a hard
// bound and against the configured connection limit, and the trailer digest.
// Truncation, oversize, corruption, and unknown message types are all refused
// with typed errors, and a framing fault closes the connection.

#ifndef HAF_NET_FRAME_HPP
#define HAF_NET_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "haf/core/bytes.hpp"
#include "haf/core/status.hpp"

namespace haf::net {

/// Current control-plane protocol version.
inline constexpr std::uint16_t kProtocolVersion = 1;
/// Oldest protocol version this build still accepts.
inline constexpr std::uint16_t kMinimumProtocolVersion = 1;

inline constexpr std::size_t kFrameHeaderBytes = 28;
inline constexpr std::size_t kFrameTrailerBytes = 8;
inline constexpr std::size_t kMaxFramePayloadBytes = 4U * 1024U * 1024U;

namespace frame_flags {
inline constexpr std::uint16_t kNone = 0;
inline constexpr std::uint16_t kResponse = 1U << 0;
inline constexpr std::uint16_t kError = 1U << 1;
}  // namespace frame_flags

/// Decoded, validated frame header.
struct FrameHeader {
    std::uint16_t protocol_version{kProtocolVersion};
    std::uint16_t type{0};
    std::uint16_t flags{frame_flags::kNone};
    std::uint64_t sequence{0};
    std::uint32_t payload_length{0};

    [[nodiscard]] bool is_response() const noexcept { return (flags & frame_flags::kResponse) != 0; }
    [[nodiscard]] bool is_error() const noexcept { return (flags & frame_flags::kError) != 0; }
};

/// A complete message: header plus payload.
struct Frame {
    FrameHeader header;
    ByteBuffer payload;
};

/// Compute the CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor).
[[nodiscard]] std::uint32_t crc32_ieee(const void* data, std::size_t size) noexcept;

/// Serialize a complete frame into one contiguous buffer.
[[nodiscard]] Result<ByteBuffer> encode_frame(const Frame& frame);

/// Validate the fixed-size header and report the declared payload length.
[[nodiscard]] Result<FrameHeader> decode_frame_header(const std::uint8_t* header, std::size_t size,
                                                      std::size_t maximum_payload);

/// Verify the 8-byte trailer digest against the bytes actually received.
[[nodiscard]] VoidResult verify_frame_trailer(const std::uint8_t* header_bytes, std::size_t header_size,
                                              const ByteBuffer& payload, const std::uint8_t* trailer,
                                              std::size_t trailer_size) noexcept;

/// Trailer digest over the received header bytes plus payload.
[[nodiscard]] std::uint64_t frame_trailer_digest(const std::uint8_t* header_bytes, std::size_t header_size,
                                                 const ByteBuffer& payload) noexcept;

}  // namespace haf::net

#endif  // HAF_NET_FRAME_HPP
