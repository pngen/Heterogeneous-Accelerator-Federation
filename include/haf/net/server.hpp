// Heterogeneous Accelerator Federation - control-plane server.
//
// Threading model
// ---------------
//   * one accept thread owned by the server;
//   * one detached worker per connection;
//   * a connection-count condition variable so that stop() waits until every
//     worker has finished without ever joining a thread that needs a lock the
//     stopping thread holds;
//   * a registry of live sockets so that stop() can force-close half-open
//     peers instead of waiting for a read timeout.
//
// The server owns no federation state. All semantics live in the handler.

#ifndef HAF_NET_SERVER_HPP
#define HAF_NET_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "haf/core/limits.hpp"
#include "haf/net/frame.hpp"
#include "haf/net/messages.hpp"
#include "haf/net/socket.hpp"

namespace haf::net {

struct ServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{0};
    std::size_t max_connections{Limits::kMaxConnections};
    int accept_timeout_millis{100};
    int read_timeout_millis{30000};
    /// Per-connection payload bound. Deliberately below the protocol maximum so
    /// that a connection cannot force a large allocation.
    std::size_t max_payload_bytes{kMaxFramePayloadBytes};
    /// Maximum number of consecutive protocol faults tolerated before the
    /// connection is closed.
    std::size_t max_protocol_faults{1};
};

/// Per-connection state owned by the server.
struct ConnectionState {
    SessionId id{};
    std::string peer;
    std::uint64_t last_request_sequence{0};
    bool saw_request{false};
    std::size_t protocol_faults{0};
    std::uint64_t accepted_at_nanos{0};
};

struct ResponseMessage {
    MessageType type{MessageType::ErrorResponse};
    ByteBuffer payload;
    bool error{false};
};

class ConnectionHandler {
public:
    ConnectionHandler() = default;
    virtual ~ConnectionHandler() = default;

    ConnectionHandler(const ConnectionHandler&) = delete;
    ConnectionHandler& operator=(const ConnectionHandler&) = delete;

    /// Handle one request. Runs on the connection's worker thread. Returning a
    /// failure status causes the server to emit a typed error response.
    [[nodiscard]] virtual Result<ResponseMessage> handle(MessageType type, const ByteBuffer& payload,
                                                         ConnectionState& connection) = 0;

    virtual void on_connected(ConnectionState& connection);
    virtual void on_disconnected(ConnectionState& connection);
};

class ControlServer {
public:
    ControlServer(ServerConfig config, ConnectionHandler& handler);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    [[nodiscard]] VoidResult start();
    /// Stop accepting, force-close live connections, and wait for workers.
    /// Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::size_t active_connections() const noexcept { return active_.load(); }
    [[nodiscard]] std::uint64_t accepted_connections() const noexcept { return accepted_.load(); }
    [[nodiscard]] std::uint64_t rejected_connections() const noexcept { return rejected_.load(); }

private:
    void accept_loop();
    void connection_loop(std::shared_ptr<TcpSocket> socket, SessionId id, std::string peer);

    ServerConfig config_;
    ConnectionHandler* handler_;
    TcpListener listener_;
    std::uint16_t port_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::map<std::string, std::shared_ptr<TcpSocket>> live_sockets_;
    std::uint64_t next_session_{1};
};

}  // namespace haf::net

#endif  // HAF_NET_SERVER_HPP
