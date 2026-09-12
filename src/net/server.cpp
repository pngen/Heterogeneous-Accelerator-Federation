#include "haf/net/server.hpp"

#include <chrono>
#include <memory>

#include "haf/model/taxonomy.hpp"
#include <string>
#include <utility>
#include <vector>

namespace haf::net {
namespace {

[[nodiscard]] std::uint64_t steady_nanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] Result<ByteBuffer> encode_error_payload(ErrorCode code, const std::string& message,
                                                      const std::string& detail) {
    ErrorResponse response;
    response.code = code;
    response.message = message;
    response.detail = detail;
    return encode_message(response);
}

}  // namespace

void ConnectionHandler::on_connected(ConnectionState& connection) { static_cast<void>(connection); }
void ConnectionHandler::on_disconnected(ConnectionState& connection) { static_cast<void>(connection); }

ControlServer::ControlServer(ServerConfig config, ConnectionHandler& handler)
    : config_(std::move(config)), handler_(&handler) {}

ControlServer::~ControlServer() { stop(); }

VoidResult ControlServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return Status(ErrorCode::AlreadyExists, "control server has already been started");
    }
    network_startup();
    std::uint16_t bound_port = 0;
    Result<TcpListener> listener = TcpListener::bind(config_.bind_address, config_.port, bound_port);
    if (!listener.ok()) {
        started_.store(false);
        return listener.status();
    }
    listener_ = std::move(*listener);
    port_ = bound_port;
    stopping_.store(false);
    accept_thread_ = std::thread([this]() { accept_loop(); });
    return VoidResult();
}

void ControlServer::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;  // never started, or already stopped
    }
    stopping_.store(true);
    listener_.close();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    {
        std::vector<std::shared_ptr<TcpSocket>> sockets;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto& entry : live_sockets_) {
                sockets.push_back(entry.second);
            }
        }
        for (const std::shared_ptr<TcpSocket>& socket : sockets) {
            socket->close();
        }
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait_for(lock, std::chrono::seconds(30), [this]() { return active_.load() == 0; });
        live_sockets_.clear();
    }
}

void ControlServer::accept_loop() {
    while (!stopping_.load()) {
        Result<TcpSocket> accepted = listener_.accept(config_.accept_timeout_millis);
        if (!accepted.ok()) {
            if (accepted.status().code() == ErrorCode::Timeout) {
                continue;
            }
            if (stopping_.load()) {
                break;
            }
            // A transient accept failure must not spin: the loop yields through
            // the accept timeout above on the next iteration.
            continue;
        }
        if (active_.load() >= config_.max_connections) {
            rejected_.fetch_add(1);
            // Tell the peer why before closing, then close immediately.
            const Result<ByteBuffer> payload =
                encode_error_payload(ErrorCode::ResourceExhausted, "connection limit reached", config_.bind_address);
            if (payload.ok()) {
                Frame frame;
                frame.header.type = static_cast<std::uint16_t>(MessageType::ErrorResponse);
                frame.header.flags = frame_flags::kResponse | frame_flags::kError;
                frame.header.sequence = 0;
                frame.payload = *payload;
                const Result<ByteBuffer> encoded = encode_frame(frame);
                if (encoded.ok()) {
                    static_cast<void>(accepted->write_all(encoded->data(), encoded->size()));
                }
            }
            accepted->close();
            continue;
        }
        static_cast<void>(accepted->set_no_delay(true));
        std::string peer = accepted->peer_text();
        SessionId session;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            session = SessionId::from_raw(derive_identity("haf.session", std::to_string(next_session_)));
            ++next_session_;
        }
        auto socket = std::make_shared<TcpSocket>(std::move(*accepted));
        {
            std::lock_guard<std::mutex> guard(mutex_);
            live_sockets_.emplace(peer + "#" + session.to_string(), socket);
        }
        active_.fetch_add(1);
        accepted_.fetch_add(1);
        std::thread worker([this, socket, session, peer]() {
            connection_loop(socket, session, peer);
            {
                std::lock_guard<std::mutex> guard(mutex_);
                live_sockets_.erase(peer + "#" + session.to_string());
            }
            active_.fetch_sub(1);
            idle_.notify_all();
        });
        worker.detach();
    }
}

void ControlServer::connection_loop(std::shared_ptr<TcpSocket> socket, SessionId id, std::string peer) {
    ConnectionState connection;
    connection.id = id;
    connection.peer = std::move(peer);
    connection.accepted_at_nanos = steady_nanos();
    try {
        handler_->on_connected(connection);
    } catch (...) {
        socket->close();
        return;
    }

    std::vector<std::uint8_t> header_bytes(kFrameHeaderBytes);
    while (!stopping_.load()) {
        const VoidResult header_read =
            socket->read_exact(header_bytes.data(), header_bytes.size(), config_.read_timeout_millis);
        if (!header_read.ok()) {
            break;
        }
        const Result<FrameHeader> decoded =
            decode_frame_header(header_bytes.data(), header_bytes.size(), config_.max_payload_bytes);
        if (!decoded.ok()) {
            ++connection.protocol_faults;
            // A framing fault is unrecoverable: the stream position is unknown.
            const Result<ByteBuffer> payload =
                encode_error_payload(decoded.status().code(), decoded.status().message(), connection.peer);
            if (payload.ok()) {
                Frame frame;
                frame.header.type = static_cast<std::uint16_t>(MessageType::ErrorResponse);
                frame.header.flags = frame_flags::kResponse | frame_flags::kError;
                frame.payload = *payload;
                const Result<ByteBuffer> encoded = encode_frame(frame);
                if (encoded.ok()) {
                    static_cast<void>(socket->write_all(encoded->data(), encoded->size()));
                }
            }
            break;
        }
        ByteBuffer payload(decoded->payload_length);
        if (decoded->payload_length != 0) {
            const VoidResult payload_read =
                socket->read_exact(payload.data(), payload.size(), config_.read_timeout_millis);
            if (!payload_read.ok()) {
                break;
            }
        }
        std::vector<std::uint8_t> trailer(kFrameTrailerBytes);
        const VoidResult trailer_read =
            socket->read_exact(trailer.data(), trailer.size(), config_.read_timeout_millis);
        if (!trailer_read.ok()) {
            break;
        }
        const VoidResult verified = verify_frame_trailer(header_bytes.data(), header_bytes.size(), payload,
                                                         trailer.data(), trailer.size());
        if (!verified.ok()) {
            const Result<ByteBuffer> error_payload =
                encode_error_payload(ErrorCode::CorruptPayload, "frame integrity check failed", connection.peer);
            if (error_payload.ok()) {
                Frame frame;
                frame.header.type = static_cast<std::uint16_t>(MessageType::ErrorResponse);
                frame.header.flags = frame_flags::kResponse | frame_flags::kError;
                frame.payload = *error_payload;
                const Result<ByteBuffer> encoded = encode_frame(frame);
                if (encoded.ok()) {
                    static_cast<void>(socket->write_all(encoded->data(), encoded->size()));
                }
            }
            break;
        }

        MessageType type = MessageType::ErrorResponse;
        ResponseMessage response;
        if (!message_type_from_wire(decoded->type, type)) {
            const Result<ByteBuffer> error_payload =
                encode_error_payload(ErrorCode::UnknownMessageType, "unknown control-plane message type", "");
            response.type = MessageType::ErrorResponse;
            response.error = true;
            if (error_payload.ok()) {
                response.payload = *error_payload;
            }
        } else if (!is_known_request(type)) {
            const Result<ByteBuffer> error_payload = encode_error_payload(
                ErrorCode::ProtocolViolation, "message type is not a client request", std::string(to_string(type)));
            response.type = MessageType::ErrorResponse;
            response.error = true;
            if (error_payload.ok()) {
                response.payload = *error_payload;
            }
        } else if (connection.saw_request && decoded->sequence <= connection.last_request_sequence) {
            const Result<ByteBuffer> error_payload =
                encode_error_payload(ErrorCode::DuplicateMessage,
                                     "request sequence is not greater than the previous request sequence", "");
            response.type = MessageType::ErrorResponse;
            response.error = true;
            if (error_payload.ok()) {
                response.payload = *error_payload;
            }
        } else {
            connection.saw_request = true;
            connection.last_request_sequence = decoded->sequence;
            Result<ResponseMessage> handled = handler_->handle(type, payload, connection);
            if (handled.ok()) {
                response = std::move(*handled);
            } else {
                response.type = MessageType::ErrorResponse;
                response.error = true;
                const Result<ByteBuffer> error_payload =
                    encode_error_payload(handled.status().code(), handled.status().message(), connection.peer);
                if (error_payload.ok()) {
                    response.payload = *error_payload;
                }
            }
        }

        Frame response_frame;
        response_frame.header.protocol_version = decoded->protocol_version;
        response_frame.header.type = static_cast<std::uint16_t>(response.type);
        response_frame.header.flags = frame_flags::kResponse;
        if (response.error) {
            response_frame.header.flags |= frame_flags::kError;
        }
        response_frame.header.sequence = decoded->sequence;
        response_frame.payload = std::move(response.payload);
        const Result<ByteBuffer> encoded = encode_frame(response_frame);
        if (!encoded.ok()) {
            break;
        }
        const VoidResult written = socket->write_all(encoded->data(), encoded->size());
        if (!written.ok()) {
            break;
        }
        if (type == MessageType::Goodbye) {
            break;
        }
    }

    try {
        handler_->on_disconnected(connection);
    } catch (...) {
        // A handler that throws during teardown must not take the process down.
    }
    socket->close();
}

}  // namespace haf::net
