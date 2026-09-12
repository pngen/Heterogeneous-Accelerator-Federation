#include "haf/net/client.hpp"

#include <memory>
#include <utility>

namespace haf::net {

ControlClient::~ControlClient() { socket_.close(); }

Result<ControlClient> ControlClient::connect(const ClientConfig& config) {
    Result<TcpSocket> socket = TcpSocket::connect(config.host, config.port, config.connect_timeout_millis);
    if (!socket.ok()) {
        return socket.status();
    }
    static_cast<void>(socket->set_no_delay(true));
    ControlClient client;
    client.config_ = config;
    client.socket_ = std::move(*socket);
    return client;
}

Result<ClientResponse> ControlClient::request(MessageType type, const ByteBuffer& payload) {
    if (!socket_.valid()) {
        return Status(ErrorCode::ConnectionClosed, "control client is not connected");
    }
    Frame request;
    request.header.protocol_version = kProtocolVersion;
    request.header.type = static_cast<std::uint16_t>(type);
    request.header.flags = frame_flags::kNone;
    request.header.sequence = next_sequence();
    request.payload = payload;
    const Result<ByteBuffer> encoded = encode_frame(request);
    if (!encoded.ok()) {
        return encoded.status();
    }
    const VoidResult written = socket_.write_all(encoded->data(), encoded->size());
    if (!written.ok()) {
        return written.status();
    }

    std::vector<std::uint8_t> header_bytes(kFrameHeaderBytes);
    const VoidResult header_read =
        socket_.read_exact(header_bytes.data(), header_bytes.size(), config_.read_timeout_millis);
    if (!header_read.ok()) {
        return header_read.status();
    }
    const Result<FrameHeader> decoded =
        decode_frame_header(header_bytes.data(), header_bytes.size(), kMaxFramePayloadBytes);
    if (!decoded.ok()) {
        return decoded.status();
    }
    if (decoded->sequence != request.header.sequence) {
        return Status(ErrorCode::ProtocolViolation, "response sequence does not match the request sequence");
    }
    if (!decoded->is_response()) {
        return Status(ErrorCode::ProtocolViolation, "control-plane response is not flagged as a response");
    }
    ByteBuffer response_payload(decoded->payload_length);
    if (decoded->payload_length != 0) {
        const VoidResult payload_read =
            socket_.read_exact(response_payload.data(), response_payload.size(), config_.read_timeout_millis);
        if (!payload_read.ok()) {
            return payload_read.status();
        }
    }
    std::vector<std::uint8_t> trailer(kFrameTrailerBytes);
    const VoidResult trailer_read =
        socket_.read_exact(trailer.data(), trailer.size(), config_.read_timeout_millis);
    if (!trailer_read.ok()) {
        return trailer_read.status();
    }
    const VoidResult verified = verify_frame_trailer(header_bytes.data(), header_bytes.size(), response_payload,
                                                     trailer.data(), trailer.size());
    if (!verified.ok()) {
        return verified.status();
    }
    MessageType response_type = MessageType::ErrorResponse;
    if (!message_type_from_wire(decoded->type, response_type)) {
        return Status(ErrorCode::UnknownMessageType, "response declares an unknown message type");
    }
    ClientResponse response;
    response.type = response_type;
    response.error = decoded->is_error();
    response.payload = std::move(response_payload);
    return response;
}

Result<ErrorResponse> ControlClient::decode_error(const ClientResponse& response) {
    if (response.type != MessageType::ErrorResponse) {
        return Status(ErrorCode::InvalidArgument, "response is not an error response");
    }
    return decode_error_response(response.payload);
}

}  // namespace haf::net
