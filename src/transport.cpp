// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/transport.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
  #include <errno.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;
#endif

NativeSocket to_native(std::intptr_t handle) noexcept { return static_cast<NativeSocket>(handle); }
std::intptr_t from_native(NativeSocket handle) noexcept { return static_cast<std::intptr_t>(handle); }

void close_native(NativeSocket handle) noexcept {
  if (handle == kInvalidNative) return;
#if defined(_WIN32)
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

int last_socket_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

bool is_would_block(int error) noexcept {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

bool is_timeout(int error) noexcept {
#if defined(_WIN32)
  return error == WSAETIMEDOUT;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

std::once_flag g_network_once;

void initialize_network() {
#if defined(_WIN32)
  WSADATA data{};
  static_cast<void>(::WSAStartup(MAKEWORD(2, 2), &data));
#endif
}

}  // namespace

void ensure_network_initialized() { std::call_once(g_network_once, initialize_network); }

// --- Socket -----------------------------------------------------------------

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != -1; }

void Socket::close() noexcept {
  if (!valid()) return;
  close_native(to_native(handle_));
  handle_ = -1;
}

void Socket::shutdown_both() noexcept {
  if (!valid()) return;
#if defined(_WIN32)
  ::shutdown(to_native(handle_), SD_BOTH);
#else
  ::shutdown(to_native(handle_), SHUT_RDWR);
#endif
}

Status Socket::set_receive_timeout(std::uint32_t milliseconds) {
  if (!valid()) return Status::failure(ErrorCode::NotConnected, "socket is not open");
#if defined(_WIN32)
  DWORD value = static_cast<DWORD>(milliseconds);
  if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "SO_RCVTIMEO could not be set");
  }
#else
  timeval value{};
  value.tv_sec = static_cast<long>(milliseconds / 1000u);
  value.tv_usec = static_cast<long>((milliseconds % 1000u) * 1000u);
  if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "SO_RCVTIMEO could not be set");
  }
#endif
  return Status::success();
}

Status Socket::set_send_timeout(std::uint32_t milliseconds) {
  if (!valid()) return Status::failure(ErrorCode::NotConnected, "socket is not open");
#if defined(_WIN32)
  DWORD value = static_cast<DWORD>(milliseconds);
  if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "SO_SNDTIMEO could not be set");
  }
#else
  timeval value{};
  value.tv_sec = static_cast<long>(milliseconds / 1000u);
  value.tv_usec = static_cast<long>((milliseconds % 1000u) * 1000u);
  if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "SO_SNDTIMEO could not be set");
  }
#endif
  return Status::success();
}

Status Socket::set_no_delay(bool enabled) {
  if (!valid()) return Status::failure(ErrorCode::NotConnected, "socket is not open");
  const int value = enabled ? 1 : 0;
#if defined(_WIN32)
  if (::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "TCP_NODELAY could not be set");
  }
#else
  if (::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) {
    return Status::failure(ErrorCode::IoError, "TCP_NODELAY could not be set");
  }
#endif
  return Status::success();
}

std::string Socket::peer_text() const {
  if (!valid()) return std::string();
  sockaddr_storage storage{};
#if defined(_WIN32)
  int length = sizeof(storage);
#else
  socklen_t length = sizeof(storage);
#endif
  if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return std::string();
  }
  char host[64] = {0};
  std::uint16_t port = 0;
  if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    static_cast<void>(::inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host)));
    port = ntohs(address->sin_port);
  } else if (storage.ss_family == AF_INET6) {
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
    static_cast<void>(::inet_ntop(AF_INET6, &address->sin6_addr, host, sizeof(host)));
    port = ntohs(address->sin6_port);
  } else {
    return std::string();
  }
  std::string text = host;
  text.push_back(':');
  text.append(std::to_string(port));
  return text;
}

// --- Listener ---------------------------------------------------------------

Listener::~Listener() { close(); }

Listener::Listener(Listener&& other) noexcept
    : handle_(other.handle_), bound_port_(other.bound_port_) {
  other.handle_ = -1;
  other.bound_port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    bound_port_ = other.bound_port_;
    other.handle_ = -1;
    other.bound_port_ = 0;
  }
  return *this;
}

bool Listener::valid() const noexcept { return handle_ != -1; }

void Listener::close() noexcept {
  if (!valid()) return;
  close_native(to_native(handle_));
  handle_ = -1;
  bound_port_ = 0;
}

Status Listener::bind_and_listen(const Endpoint& endpoint, std::uint32_t backlog) {
  ensure_network_initialized();
  if (valid()) return Status::failure(ErrorCode::AlreadyStarted, "listener is already bound");

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(endpoint.port);
  const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  if (::getaddrinfo(host, service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Status::failure(ErrorCode::IoError, "listener address could not be resolved");
  }

  NativeSocket handle = kInvalidNative;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == kInvalidNative) continue;
    const int reuse = 1;
#if defined(_WIN32)
    static_cast<void>(::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
#else
    static_cast<void>(::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
#endif
    if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(handle, static_cast<int>(backlog)) == 0) {
      break;
    }
    close_native(handle);
    handle = kInvalidNative;
  }
  ::freeaddrinfo(results);

  if (handle == kInvalidNative) {
    return Status::failure(ErrorCode::IoError, "listener could not bind or listen");
  }

  sockaddr_storage storage{};
#if defined(_WIN32)
  int length = sizeof(storage);
#else
  socklen_t length = sizeof(storage);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) == 0 &&
      storage.ss_family == AF_INET) {
    bound_port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
  }
  handle_ = from_native(handle);
  return Status::success();
}

Result<Socket> Listener::accept_one(std::uint32_t timeout_ms) {
  if (!valid()) return Status::failure(ErrorCode::NotStarted, "listener is not bound");

  fd_set read_set;
  FD_ZERO(&read_set);
  const NativeSocket native = to_native(handle_);
  FD_SET(native, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
#if defined(_WIN32)
  const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(native + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  if (ready == 0) return Status::failure(ErrorCode::Timeout, "no inbound connection within the window");
  if (ready < 0) return Status::failure(ErrorCode::IoError, "select failed on the listener");

  sockaddr_storage storage{};
#if defined(_WIN32)
  int length = sizeof(storage);
#else
  socklen_t length = sizeof(storage);
#endif
  const NativeSocket accepted =
      ::accept(native, reinterpret_cast<sockaddr*>(&storage), &length);
  if (accepted == kInvalidNative) {
    const int error = last_socket_error();
    if (is_would_block(error) || is_timeout(error)) {
      return Status::failure(ErrorCode::Timeout, "accept timed out");
    }
    return Status::failure(ErrorCode::IoError, "accept failed");
  }
  Socket socket(from_native(accepted));
  static_cast<void>(socket.set_no_delay(true));
  return socket;
}

// --- connect ----------------------------------------------------------------

Result<Socket> connect_to(const Endpoint& endpoint, std::uint32_t timeout_ms) {
  ensure_network_initialized();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(endpoint.port);
  const char* host = endpoint.host.empty() ? "127.0.0.1" : endpoint.host.c_str();
  if (::getaddrinfo(host, service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Status::failure(ErrorCode::NotConnected, "coordinator address could not be resolved");
  }

  NativeSocket handle = kInvalidNative;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == kInvalidNative) continue;
    if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_native(handle);
    handle = kInvalidNative;
  }
  ::freeaddrinfo(results);

  if (handle == kInvalidNative) {
    return Status::failure(ErrorCode::NotConnected,
                           "could not connect to " + endpoint.host + ":" + service);
  }
  Socket socket(from_native(handle));
  static_cast<void>(socket.set_no_delay(true));
  static_cast<void>(timeout_ms);
  return socket;
}

// --- framing over sockets ---------------------------------------------------

Status send_bytes(Socket& socket, std::span<const std::byte> bytes) {
  if (!socket.valid()) return Status::failure(ErrorCode::NotConnected, "socket is not open");
  std::size_t sent = 0;
  const NativeSocket native = to_native(socket.native_handle());
  while (sent < bytes.size()) {
    const std::size_t chunk = bytes.size() - sent;
    const int amount = static_cast<int>(std::min<std::size_t>(chunk, 1u << 20));
#if defined(_WIN32)
    const int written = ::send(native, reinterpret_cast<const char*>(bytes.data() + sent), amount, 0);
#else
    const int written = static_cast<int>(
        ::send(native, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<std::size_t>(amount), 0));
#endif
    if (written <= 0) {
      const int error = last_socket_error();
      if (is_would_block(error) || is_timeout(error)) {
        return Status::failure(ErrorCode::Timeout, "send timed out");
      }
      return Status::failure(ErrorCode::ConnectionClosed, "send failed: peer is gone");
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Status send_frame(Socket& socket, const Frame& frame) {
  std::vector<std::byte> bytes;
  Status status = encode_frame(frame, bytes);
  if (!status.ok()) return status;
  return send_bytes(socket, std::span<const std::byte>(bytes.data(), bytes.size()));
}

namespace {

Status recv_exact(Socket& socket, std::span<std::byte> buffer, bool& closed) {
  closed = false;
  std::size_t received = 0;
  const NativeSocket native = to_native(socket.native_handle());
  while (received < buffer.size()) {
    const int amount = static_cast<int>(std::min<std::size_t>(buffer.size() - received, 1u << 20));
#if defined(_WIN32)
    const int read = ::recv(native, reinterpret_cast<char*>(buffer.data() + received), amount, 0);
#else
    const int read = static_cast<int>(
        ::recv(native, reinterpret_cast<char*>(buffer.data() + received), static_cast<std::size_t>(amount), 0));
#endif
    if (read == 0) {
      closed = true;
      return Status::failure(ErrorCode::ConnectionClosed, "peer closed the connection");
    }
    if (read < 0) {
      const int error = last_socket_error();
      if (is_would_block(error) || is_timeout(error)) {
        return Status::failure(ErrorCode::Timeout, "receive timed out");
      }
      return Status::failure(ErrorCode::IoError, "receive failed");
    }
    received += static_cast<std::size_t>(read);
  }
  return Status::success();
}

}  // namespace

Result<Frame> receive_frame(Socket& socket) {
  if (!socket.valid()) return Status::failure(ErrorCode::NotConnected, "socket is not open");

  std::vector<std::byte> header_bytes(kFrameHeaderBytes);
  bool closed = false;
  Status status = recv_exact(socket, std::span<std::byte>(header_bytes.data(), header_bytes.size()), closed);
  if (!status.ok()) return status;

  Result<FrameHeader> header = decode_header(std::span<const std::byte>(header_bytes.data(), header_bytes.size()));
  if (!header.ok()) return header.status();

  Frame frame;
  frame.header = header.value();
  frame.payload.resize(frame.header.payload_length);
  if (!frame.payload.empty()) {
    status = recv_exact(socket, std::span<std::byte>(frame.payload.data(), frame.payload.size()), closed);
    if (!status.ok()) return status;
  }
  status = verify_payload(frame.header, std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  if (!status.ok()) return status;
  return frame;
}

}  // namespace hgm
