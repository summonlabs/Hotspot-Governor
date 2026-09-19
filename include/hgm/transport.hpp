// Real OS socket transport with the Hotspot Governor framing on top.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "hgm/error.hpp"
#include "hgm/frame.hpp"

namespace hgm {

struct Endpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;  // 0 asks the OS for an ephemeral port
};

// One TCP connection. Move-only; closing is idempotent.
class Socket {
 public:
  Socket() = default;
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  bool valid() const noexcept;
  void close() noexcept;
  void shutdown_both() noexcept;
  std::intptr_t native_handle() const noexcept { return handle_; }

  // Receive timeout in milliseconds. 0 disables the timeout.
  Status set_receive_timeout(std::uint32_t milliseconds);
  Status set_send_timeout(std::uint32_t milliseconds);
  Status set_no_delay(bool enabled);

  // Peer address as "host:port"; empty when unavailable.
  std::string peer_text() const;

 private:
  friend class Listener;
  friend Result<Socket> connect_to(const Endpoint&, std::uint32_t);
  explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}

  std::intptr_t handle_ = -1;
};

class Listener {
 public:
  Listener() = default;
  ~Listener();

  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Binds and listens. Port 0 selects an ephemeral port, readable through
  // port().
  Status bind_and_listen(const Endpoint& endpoint, std::uint32_t backlog = 32);
  void close() noexcept;
  bool valid() const noexcept;

  std::uint16_t port() const noexcept { return bound_port_; }

  // Waits up to timeout_ms for one inbound connection. A timeout is reported
  // as ErrorCode::Timeout, not as a failure.
  Result<Socket> accept_one(std::uint32_t timeout_ms);

 private:
  std::intptr_t handle_ = -1;
  std::uint16_t bound_port_ = 0;
};

Result<Socket> connect_to(const Endpoint& endpoint, std::uint32_t timeout_ms = 5000);

// Reads a frame from a blocking socket: exactly the header, then exactly the
// declared payload, verifying both CRCs. Returns ErrorCode::Timeout when the
// receive timeout elapses before a full header arrives, and
// ErrorCode::ConnectionClosed when the peer closes cleanly.
Result<Frame> receive_frame(Socket& socket);

Status send_frame(Socket& socket, const Frame& frame);

// Writes raw bytes to the socket. Used by framing and by adversarial tests that
// need to put arbitrary bytes on the wire.
Status send_bytes(Socket& socket, std::span<const std::byte> bytes);

// Initializes the platform network stack once per process.
void ensure_network_initialized();

}  // namespace hgm
