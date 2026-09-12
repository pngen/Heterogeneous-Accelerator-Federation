#include "haf/net/socket.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace haf::net {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;
constexpr int kWouldBlock = WSAEWOULDBLOCK;
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;
constexpr int kWouldBlock = EWOULDBLOCK;
#endif

[[nodiscard]] native_socket to_native(std::intptr_t handle) { return static_cast<native_socket>(handle); }

[[nodiscard]] std::intptr_t from_native(native_socket handle) { return static_cast<std::intptr_t>(handle); }

[[nodiscard]] bool is_valid(std::intptr_t handle) {
    return handle != -1 && to_native(handle) != kInvalidSocket;
}

[[nodiscard]] std::string last_error_text() {
#if defined(_WIN32)
    const int code = WSAGetLastError();
    return "winsock error " + std::to_string(code);
#else
    return std::strerror(errno);
#endif
}

[[nodiscard]] int last_error_code() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

}  // namespace

void network_startup() {
#if defined(_WIN32)
    static bool started = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    static_cast<void>(started);
#endif
}

void network_shutdown() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_), peer_(std::move(other.peer_)) {
    other.handle_ = -1;
    other.peer_.clear();
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        peer_ = std::move(other.peer_);
        other.handle_ = -1;
        other.peer_.clear();
    }
    return *this;
}

TcpSocket::~TcpSocket() { close(); }

void TcpSocket::reset(std::intptr_t handle, std::string peer) {
    close();
    handle_ = handle;
    peer_ = std::move(peer);
}

void TcpSocket::close() noexcept {
    if (is_valid(handle_)) {
#if defined(_WIN32)
        closesocket(to_native(handle_));
#else
        ::close(to_native(handle_));
#endif
    }
    handle_ = -1;
    peer_.clear();
}

bool TcpSocket::valid() const noexcept { return is_valid(handle_); }

std::string TcpSocket::peer_text() const { return peer_; }

Result<TcpSocket> TcpSocket::connect(const std::string& host, std::uint16_t port, int timeout_millis) {
    network_startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (resolved != 0 || results == nullptr) {
        return Status(ErrorCode::TransportFailure, "cannot resolve host '" + host + "'");
    }
    native_socket handle = kInvalidSocket;
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (handle == kInvalidSocket) {
            continue;
        }
#if defined(_WIN32)
        u_long non_blocking = 1;
        ioctlsocket(handle, FIONBIO, &non_blocking);
#else
        const int flags = fcntl(handle, F_GETFL, 0);
        fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
        const int rc = ::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
        if (rc == 0) {
            break;
        }
        const int error = last_error_code();
#if defined(_WIN32)
        const bool in_progress = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
        const bool in_progress = error == EINPROGRESS || error == EWOULDBLOCK;
#endif
        if (!in_progress) {
#if defined(_WIN32)
            closesocket(handle);
#else
            ::close(handle);
#endif
            handle = kInvalidSocket;
            continue;
        }
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(handle, &write_set);
        timeval timeout{};
        timeout.tv_sec = timeout_millis / 1000;
        timeout.tv_usec = (timeout_millis % 1000) * 1000;
#if defined(_WIN32)
        const int selected = ::select(0, nullptr, &write_set, nullptr, &timeout);
#else
        const int selected = ::select(handle + 1, nullptr, &write_set, nullptr, &timeout);
#endif
        if (selected <= 0) {
#if defined(_WIN32)
            closesocket(handle);
#else
            ::close(handle);
#endif
            handle = kInvalidSocket;
            continue;
        }
        int socket_error = 0;
#if defined(_WIN32)
        int length = sizeof(socket_error);
        if (getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length) != 0) {
#else
        socklen_t length = sizeof(socket_error);
        if (getsockopt(handle, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
#endif
            socket_error = -1;
        }
        if (socket_error != 0) {
#if defined(_WIN32)
            closesocket(handle);
#else
            ::close(handle);
#endif
            handle = kInvalidSocket;
            continue;
        }
        break;
    }
    freeaddrinfo(results);
    if (handle == kInvalidSocket) {
        return Status(ErrorCode::TransportFailure, "cannot connect to " + host + ":" + service);
    }
    // Return the socket to blocking mode; timeouts are enforced with select.
#if defined(_WIN32)
    u_long blocking = 0;
    ioctlsocket(handle, FIONBIO, &blocking);
#else
    {
        const int flags = fcntl(handle, F_GETFL, 0);
        fcntl(handle, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
    TcpSocket socket;
    socket.handle_ = from_native(handle);
    socket.peer_ = host + ":" + service;
    return socket;
}

Result<std::size_t> TcpSocket::read_some(void* buffer, std::size_t size, int timeout_millis) {
    if (!valid()) {
        return Status(ErrorCode::ConnectionClosed, "socket is not open");
    }
    if (size == 0) {
        return static_cast<std::size_t>(0);
    }
    const native_socket handle = to_native(handle_);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(handle, &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_millis / 1000;
    timeout.tv_usec = (timeout_millis % 1000) * 1000;
#if defined(_WIN32)
    const int selected = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
    const int selected = ::select(handle + 1, &read_set, nullptr, nullptr, &timeout);
#endif
    if (selected == 0) {
        return Status(ErrorCode::Timeout, "read timed out");
    }
    if (selected < 0) {
        return Status(ErrorCode::TransportFailure, "select failed: " + last_error_text());
    }
    const int received = ::recv(handle, static_cast<char*>(buffer), static_cast<int>(size), 0);
    if (received == 0) {
        return static_cast<std::size_t>(0);
    }
    if (received < 0) {
        const int error = last_error_code();
        if (error == kWouldBlock) {
            return Status(ErrorCode::Timeout, "read would block");
        }
        return Status(ErrorCode::TransportFailure, "recv failed: " + last_error_text());
    }
    return static_cast<std::size_t>(received);
}

VoidResult TcpSocket::read_exact(void* buffer, std::size_t size, int timeout_millis) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const Result<std::size_t> read = read_some(bytes + offset, size - offset, timeout_millis);
        if (!read.ok()) {
            return read.status();
        }
        if (*read == 0) {
            return Status(ErrorCode::ConnectionClosed, "peer closed the connection mid-frame");
        }
        offset += *read;
    }
    return VoidResult();
}

VoidResult TcpSocket::write_all(const void* buffer, std::size_t size) {
    if (!valid()) {
        return Status(ErrorCode::ConnectionClosed, "socket is not open");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const int sent = ::send(to_native(handle_), reinterpret_cast<const char*>(bytes + offset),
                                static_cast<int>(size - offset), 0);
        if (sent <= 0) {
            return Status(ErrorCode::TransportFailure, "send failed: " + last_error_text());
        }
        offset += static_cast<std::size_t>(sent);
    }
    return VoidResult();
}

VoidResult TcpSocket::set_no_delay(bool enabled) {
    if (!valid()) {
        return Status(ErrorCode::ConnectionClosed, "socket is not open");
    }
    const int value = enabled ? 1 : 0;
    if (setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        return Status(ErrorCode::TransportFailure, "cannot set TCP_NODELAY");
    }
    return VoidResult();
}

VoidResult TcpSocket::set_keepalive(bool enabled) {
    if (!valid()) {
        return Status(ErrorCode::ConnectionClosed, "socket is not open");
    }
    const int value = enabled ? 1 : 0;
    if (setsockopt(to_native(handle_), SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        return Status(ErrorCode::TransportFailure, "cannot set SO_KEEPALIVE");
    }
    return VoidResult();
}

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = -1;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = -1;
        other.port_ = 0;
    }
    return *this;
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() noexcept {
    if (is_valid(handle_)) {
#if defined(_WIN32)
        closesocket(to_native(handle_));
#else
        ::close(to_native(handle_));
#endif
    }
    handle_ = -1;
}

bool TcpListener::valid() const noexcept { return is_valid(handle_); }

Result<TcpListener> TcpListener::bind(const std::string& host, std::uint16_t port, std::uint16_t& bound_port) {
    network_startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results);
    if (resolved != 0 || results == nullptr) {
        return Status(ErrorCode::TransportFailure, "cannot resolve bind address '" + host + "'");
    }
    native_socket handle = kInvalidSocket;
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (handle == kInvalidSocket) {
            continue;
        }
        const int reuse = 1;
        setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
            break;
        }
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        handle = kInvalidSocket;
    }
    freeaddrinfo(results);
    if (handle == kInvalidSocket) {
        return Status(ErrorCode::TransportFailure, "cannot bind " + host + ":" + service);
    }
    if (::listen(handle, static_cast<int>(16)) != 0) {
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        return Status(ErrorCode::TransportFailure, "listen failed: " + last_error_text());
    }
    sockaddr_storage address{};
#if defined(_WIN32)
    int address_length = sizeof(address);
#else
    socklen_t address_length = sizeof(address);
#endif
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        return Status(ErrorCode::TransportFailure, "getsockname failed");
    }
    std::uint16_t actual = port;
    if (address.ss_family == AF_INET) {
        actual = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    } else if (address.ss_family == AF_INET6) {
        actual = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    TcpListener listener;
    listener.handle_ = from_native(handle);
    listener.port_ = actual;
    bound_port = actual;
    return listener;
}

Result<TcpSocket> TcpListener::accept(int timeout_millis) {
    if (!valid()) {
        return Status(ErrorCode::ConnectionClosed, "listener is not open");
    }
    const native_socket handle = to_native(handle_);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(handle, &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_millis / 1000;
    timeout.tv_usec = (timeout_millis % 1000) * 1000;
#if defined(_WIN32)
    const int selected = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
    const int selected = ::select(handle + 1, &read_set, nullptr, nullptr, &timeout);
#endif
    if (selected == 0) {
        return Status(ErrorCode::Timeout, "accept timed out");
    }
    if (selected < 0) {
        return Status(ErrorCode::TransportFailure, "accept select failed: " + last_error_text());
    }
    sockaddr_storage address{};
#if defined(_WIN32)
    int address_length = sizeof(address);
#else
    socklen_t address_length = sizeof(address);
#endif
    const native_socket client = ::accept(handle, reinterpret_cast<sockaddr*>(&address), &address_length);
    if (client == kInvalidSocket) {
        return Status(ErrorCode::TransportFailure, "accept failed: " + last_error_text());
    }
    char host_text[NI_MAXHOST] = {};
    char service_text[NI_MAXSERV] = {};
    std::string peer;
    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), address_length, host_text, sizeof(host_text), service_text,
                    sizeof(service_text), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        peer = std::string(host_text) + ":" + std::string(service_text);
    } else {
        peer = "unknown:0";
    }
    TcpSocket socket;
    socket.reset(from_native(client), std::move(peer));
    return socket;
}

}  // namespace haf::net
