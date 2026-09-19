// Real loopback sockets with the framed protocol on top.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "hgm/limits.hpp"
#include "hgm/synthetic.hpp"
#include "hgm/transport.hpp"
#include "testing.hpp"

using namespace hgm;

namespace {

Provenance sample_provenance() {
  Provenance provenance;
  provenance.publisher = PublisherId::from(5);
  provenance.incarnation = Incarnation::from(6);
  provenance.boot = BootId::from(7);
  provenance.epoch = CoordinatorEpoch::from(8);
  provenance.sequence = 9;
  provenance.generation = StreamGeneration{StreamKind::Topology, 10};
  provenance.emitted_at = 11;
  return provenance;
}

}  // namespace

HGM_TEST(transport, listener_binds_an_ephemeral_port) {
  ensure_network_initialized();
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  HGM_CHECK(listener.valid());
  HGM_CHECK(listener.port() != 0);
  HGM_CHECK_EQ(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).code(), ErrorCode::AlreadyStarted);
  listener.close();
}

HGM_TEST(transport, a_frame_round_trips_over_loopback) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  std::vector<std::byte> payload(4096, std::byte{0x5A});
  Frame sent = make_frame(MessageType::Heartbeat, sample_provenance(), std::move(payload));
  Status client_status = Status::failure(ErrorCode::Internal, "not run");
  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (!socket.ok()) {
      client_status = socket.status();
      return;
    }
    client_status = send_frame(socket.value(), sent);
  });

  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(5000));
  Result<Frame> received = receive_frame(accepted.value());
  client.join();
  HGM_CHECK(client_status.ok());
  HGM_CHECK(received.ok());
  HGM_CHECK_EQ(received.value().header.type, MessageType::Heartbeat);
  HGM_CHECK_EQ(received.value().header.publisher, PublisherId::from(5));
  HGM_CHECK_EQ(received.value().payload.size(), static_cast<std::size_t>(4096));
  HGM_CHECK_EQ(received.value().payload.front(), std::byte{0x5A});
  HGM_CHECK(!accepted.value().peer_text().empty());
  listener.close();
}

HGM_TEST(transport, a_full_topology_frame_round_trips) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 3);
  std::vector<std::byte> payload;
  HGM_CHECK(encode_topology_payload(fabric.topology(), payload).ok());
  Frame sent = make_frame(MessageType::Topology, sample_provenance(), std::move(payload));

  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (socket.ok()) static_cast<void>(send_frame(socket.value(), sent));
  });
  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(5000));
  Result<Frame> received = receive_frame(accepted.value());
  client.join();
  HGM_CHECK(received.ok());
  TopologySnapshot decoded;
  HGM_CHECK(decode_topology_payload(
                std::span<const std::byte>(received.value().payload.data(), received.value().payload.size()),
                decoded)
                .ok());
  HGM_CHECK_EQ(decoded.id(), fabric.topology().id());
  listener.close();
}

HGM_TEST(transport, receive_timeout_is_reported_not_fatal) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (socket.ok()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      socket.value().close();
    }
  });
  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(50));
  const Result<Frame> timed_out = receive_frame(accepted.value());
  HGM_CHECK(!timed_out.ok());
  HGM_CHECK_EQ(timed_out.status().code(), ErrorCode::Timeout);
  client.join();
  listener.close();
}

HGM_TEST(transport, peer_close_is_detected) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (socket.ok()) socket.value().close();
  });
  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(5000));
  const Result<Frame> closed = receive_frame(accepted.value());
  HGM_CHECK(!closed.ok());
  HGM_CHECK_EQ(closed.status().code(), ErrorCode::ConnectionClosed);
  client.join();
  listener.close();
}

HGM_TEST(transport, an_oversized_declared_payload_is_refused_before_allocation) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  header.type = MessageType::Topology;
  header.payload_length = static_cast<std::uint32_t>(Limits::kMaxFrameBytes) + 1u;
  std::vector<std::byte> header_bytes;
  encode_header(header, header_bytes);
  // The encoded CRC is valid for the claimed (unencodable) length, so the
  // receiver must refuse on the bound alone.
  std::vector<std::byte> forged = header_bytes;
  {
    const std::uint64_t crc = crc64(std::span<const std::byte>(forged.data(), kFrameHeaderBytes - 8));
    for (int i = 0; i < 8; ++i) {
      forged[static_cast<std::size_t>(kFrameHeaderBytes - 8 + i)] =
          static_cast<std::byte>((crc >> (8 * i)) & 0xFFull);
    }
  }

  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (socket.ok()) static_cast<void>(send_bytes(socket.value(), std::span<const std::byte>(forged.data(), forged.size())));
  });
  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(5000));
  const Result<Frame> refused = receive_frame(accepted.value());
  client.join();
  HGM_CHECK(!refused.ok());
  HGM_CHECK_EQ(refused.status().code(), ErrorCode::FrameTooLarge);
  listener.close();
}

HGM_TEST(transport, corrupted_wire_bytes_are_refused) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();

  std::vector<std::byte> payload(128, std::byte{0x11});
  Frame frame = make_frame(MessageType::Heartbeat, sample_provenance(), std::move(payload));
  std::vector<std::byte> wire;
  HGM_CHECK(encode_frame(frame, wire).ok());
  wire[kFrameHeaderBytes + 3] = static_cast<std::byte>(0xEE);

  std::thread client([&]() {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
    if (socket.ok()) static_cast<void>(send_bytes(socket.value(), std::span<const std::byte>(wire.data(), wire.size())));
  });
  Result<Socket> accepted = listener.accept_one(5000);
  HGM_CHECK(accepted.ok());
  static_cast<void>(accepted.value().set_receive_timeout(5000));
  const Result<Frame> refused = receive_frame(accepted.value());
  client.join();
  HGM_CHECK(!refused.ok());
  HGM_CHECK_EQ(refused.status().code(), ErrorCode::IntegrityMismatch);
  listener.close();
}

HGM_TEST(transport, connect_to_a_closed_port_fails_cleanly) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();
  listener.close();
  const Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 2000);
  HGM_CHECK(!socket.ok());
  HGM_CHECK_EQ(socket.status().code(), ErrorCode::NotConnected);
}

HGM_TEST(transport, accept_times_out_without_failing) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const Result<Socket> accepted = listener.accept_one(50);
  HGM_CHECK(!accepted.ok());
  HGM_CHECK_EQ(accepted.status().code(), ErrorCode::Timeout);
  listener.close();
  HGM_CHECK_EQ(listener.accept_one(10).status().code(), ErrorCode::NotStarted);
}

HGM_TEST(transport, sequential_connections_are_handled) {
  Listener listener;
  HGM_CHECK(listener.bind_and_listen(Endpoint{"127.0.0.1", 0}).ok());
  const std::uint16_t port = listener.port();
  for (int i = 0; i < 4; ++i) {
    std::thread client([&]() {
      Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port});
      if (!socket.ok()) return;
      std::vector<std::byte> payload(64, std::byte{static_cast<unsigned char>(i)});
      Frame frame = make_frame(MessageType::Heartbeat, sample_provenance(), std::move(payload));
      static_cast<void>(send_frame(socket.value(), frame));
    });
    Result<Socket> accepted = listener.accept_one(5000);
    HGM_CHECK(accepted.ok());
    static_cast<void>(accepted.value().set_receive_timeout(5000));
    Result<Frame> received = receive_frame(accepted.value());
    HGM_CHECK(received.ok());
    HGM_CHECK_EQ(received.value().payload.front(), std::byte{static_cast<unsigned char>(i)});
    client.join();
  }
  listener.close();
}
