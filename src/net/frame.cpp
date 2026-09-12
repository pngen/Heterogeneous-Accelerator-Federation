#include "haf/net/frame.hpp"

#include <array>
#include <cstring>

#include "haf/core/hash.hpp"

namespace haf::net {
namespace {

constexpr std::uint8_t kMagic[4] = {'H', 'A', 'F', '1'};

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFULL);
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[i]) << (i * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
    }
    return value;
}

void write_header(std::uint8_t* out, const FrameHeader& header) {
    std::memcpy(out, kMagic, sizeof(kMagic));
    put_u16(out + 4, header.protocol_version);
    put_u16(out + 6, header.type);
    put_u16(out + 8, header.flags);
    put_u16(out + 10, 0);
    put_u64(out + 12, header.sequence);
    put_u32(out + 20, header.payload_length);
    put_u32(out + 24, crc32_ieee(out, 24));
}

}  // namespace

std::uint32_t crc32_ieee(const void* data, std::size_t size) noexcept {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> generated{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            generated[i] = value;
        }
        return generated;
    }();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint64_t frame_trailer_digest(const std::uint8_t* header_bytes, std::size_t header_size,
                                   const ByteBuffer& payload) noexcept {
    Fnv1a64 hasher;
    hasher.update(header_bytes, header_size);
    if (!payload.empty()) {
        hasher.update(payload.data(), payload.size());
    }
    return hasher.digest();
}

Result<ByteBuffer> encode_frame(const Frame& frame) {
    if (frame.payload.size() > kMaxFramePayloadBytes) {
        return Status(ErrorCode::FrameTooLarge, "payload exceeds the maximum frame payload size");
    }
    FrameHeader header = frame.header;
    header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
    ByteBuffer buffer;
    buffer.resize(kFrameHeaderBytes + frame.payload.size() + kFrameTrailerBytes);
    write_header(buffer.data(), header);
    if (!frame.payload.empty()) {
        std::memcpy(buffer.data() + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
    }
    put_u64(buffer.data() + kFrameHeaderBytes + frame.payload.size(),
            frame_trailer_digest(buffer.data(), kFrameHeaderBytes, frame.payload));
    return buffer;
}

Result<FrameHeader> decode_frame_header(const std::uint8_t* header, std::size_t size,
                                        std::size_t maximum_payload) {
    if (size < kFrameHeaderBytes) {
        return Status(ErrorCode::TruncatedFrame, "frame header is shorter than the fixed header size");
    }
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return Status(ErrorCode::ProtocolViolation, "frame magic is not the federation control-plane magic");
    }
    if (get_u16(header + 10) != 0) {
        return Status(ErrorCode::ProtocolViolation, "frame reserved word is not zero");
    }
    if (get_u32(header + 24) != crc32_ieee(header, 24)) {
        return Status(ErrorCode::CorruptPayload, "frame header checksum does not match");
    }
    FrameHeader decoded;
    decoded.protocol_version = get_u16(header + 4);
    decoded.type = get_u16(header + 6);
    decoded.flags = get_u16(header + 8);
    decoded.sequence = get_u64(header + 12);
    decoded.payload_length = get_u32(header + 20);
    if (decoded.protocol_version < kMinimumProtocolVersion || decoded.protocol_version > kProtocolVersion) {
        return Status(ErrorCode::UnsupportedProtocolVersion, "frame declares an unsupported protocol version");
    }
    if (decoded.payload_length > kMaxFramePayloadBytes) {
        return Status(ErrorCode::FrameTooLarge, "frame declares a payload larger than the protocol maximum");
    }
    if (decoded.payload_length > maximum_payload) {
        return Status(ErrorCode::FrameTooLarge, "frame declares a payload larger than this connection permits");
    }
    return decoded;
}

VoidResult verify_frame_trailer(const std::uint8_t* header_bytes, std::size_t header_size,
                                const ByteBuffer& payload, const std::uint8_t* trailer,
                                std::size_t trailer_size) noexcept {
    if (trailer_size < kFrameTrailerBytes) {
        return Status(ErrorCode::TruncatedFrame, "frame trailer is incomplete");
    }
    const std::uint64_t declared = get_u64(trailer);
    const std::uint64_t computed = frame_trailer_digest(header_bytes, header_size, payload);
    if (computed != declared) {
        return Status(ErrorCode::CorruptPayload, "frame trailer digest does not match");
    }
    return VoidResult();
}

}  // namespace haf::net
