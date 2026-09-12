// Heterogeneous Accelerator Federation - control-plane client.
//
// A synchronous request/response client with a single outstanding request. The
// agent and the CLI both need determinism far more than pipelining, and a
// single-outstanding-request client cannot mis-associate a response.

#ifndef HAF_NET_CLIENT_HPP
#define HAF_NET_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "haf/net/frame.hpp"
#include "haf/net/messages.hpp"
#include "haf/net/socket.hpp"

namespace haf::net {

struct ClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    int connect_timeout_millis{5000};
    int read_timeout_millis{30000};
    std::string name{"haf-client"};
    std::string version{"1.0.0"};
};

/// Typed response: the message type is validated against the request.
struct ClientResponse {
    MessageType type{MessageType::ErrorResponse};
    bool error{false};
    ByteBuffer payload;
};

class ControlClient {
public:
    ControlClient() = default;
    ~ControlClient();

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;
    ControlClient(ControlClient&&) noexcept = default;
    ControlClient& operator=(ControlClient&&) noexcept = default;

    [[nodiscard]] static Result<ControlClient> connect(const ClientConfig& config);

    /// Send a request and read exactly one response.
    [[nodiscard]] Result<ClientResponse> request(MessageType type, const ByteBuffer& payload);

    [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }
    void close() noexcept { socket_.close(); }
    [[nodiscard]] std::uint64_t next_sequence() noexcept { return sequence_++; }

    /// Decode an error response carried by a failed call.
    [[nodiscard]] static Result<ErrorResponse> decode_error(const ClientResponse& response);

private:
    ClientConfig config_;
    TcpSocket socket_;
    std::uint64_t sequence_{1};
};

}  // namespace haf::net

#endif  // HAF_NET_CLIENT_HPP
