// Heterogeneous Accelerator Federation - TCP socket layer.
//
// A deliberately small, blocking, bounded wrapper. Every read and write has an
// explicit timeout so that a half-open peer can never wedge a thread forever,
// and every failure is reported as a typed status rather than an exception.

#ifndef HAF_NET_SOCKET_HPP
#define HAF_NET_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "haf/core/status.hpp"

namespace haf::net {

/// Process-wide socket subsystem lifetime. Idempotent.
void network_startup();
void network_shutdown();

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    [[nodiscard]] static Result<TcpSocket> connect(const std::string& host, std::uint16_t port,
                                                   int timeout_millis);

    /// Read up to \p size bytes. Returns the number of bytes read; zero means
    /// the peer closed the connection cleanly.
    [[nodiscard]] Result<std::size_t> read_some(void* buffer, std::size_t size, int timeout_millis);

    /// Read exactly \p size bytes or fail.
    [[nodiscard]] VoidResult read_exact(void* buffer, std::size_t size, int timeout_millis);

    [[nodiscard]] VoidResult write_all(const void* buffer, std::size_t size);

    [[nodiscard]] VoidResult set_no_delay(bool enabled);
    [[nodiscard]] VoidResult set_keepalive(bool enabled);

    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string peer_text() const;

    /// Native handle, for diagnostics and for the listener.
    [[nodiscard]] std::intptr_t native_handle() const noexcept { return handle_; }
    void reset(std::intptr_t handle, std::string peer);

private:
    std::intptr_t handle_{-1};
    std::string peer_;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    /// Bind to \p host : \p port. Port zero selects an ephemeral port, whose
    /// real value is reported through \p bound_port.
    [[nodiscard]] static Result<TcpListener> bind(const std::string& host, std::uint16_t port,
                                                  std::uint16_t& bound_port);

    /// Accept one connection, waiting at most \p timeout_millis. A timeout is
    /// reported as ErrorCode::Timeout.
    [[nodiscard]] Result<TcpSocket> accept(int timeout_millis);

    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    std::intptr_t handle_{-1};
    std::uint16_t port_{0};
};

}  // namespace haf::net

#endif  // HAF_NET_SOCKET_HPP
