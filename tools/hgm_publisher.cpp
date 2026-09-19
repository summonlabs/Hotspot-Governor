// Publisher process: generates SYNTHETIC fabric evidence and sends it to a
// coordinator over a real socket using the framed protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hgm/frame.hpp"
#include "hgm/synthetic.hpp"
#include "hgm/transport.hpp"
#include "hgm/version.hpp"

namespace {

using namespace hgm;

std::string arg_value(const std::vector<std::string>& args, const std::string& flag,
                      const std::string& fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return fallback;
}

std::uint64_t parse_u64(const std::string& text, std::uint64_t fallback) {
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  const std::string host = arg_value(args, "--host", "127.0.0.1");
  const std::uint16_t port = static_cast<std::uint16_t>(parse_u64(arg_value(args, "--port", "0"), 0));
  const std::string name = arg_value(args, "--name", "hgm-publisher");
  const std::uint64_t incarnation = parse_u64(arg_value(args, "--incarnation", "1"), 1);
  const std::uint64_t boot = parse_u64(arg_value(args, "--boot", "1"), 1);
  const std::uint64_t rounds = parse_u64(arg_value(args, "--rounds", "4"), 4);
  const std::uint64_t interval_ms = parse_u64(arg_value(args, "--interval-ms", "0"), 0);
  const std::uint64_t forced_epoch = parse_u64(arg_value(args, "--force-epoch", "0"), 0);
  const std::uint64_t start_generation = parse_u64(arg_value(args, "--start-generation", "1"), 1);

  SyntheticConfig synthetic;
  synthetic.region_count = static_cast<std::uint32_t>(parse_u64(arg_value(args, "--regions", "4"), 4));
  synthetic.resources_per_region =
      static_cast<std::uint32_t>(parse_u64(arg_value(args, "--resources", "6"), 6));
  synthetic.hotspot_count = static_cast<std::uint32_t>(parse_u64(arg_value(args, "--hotspots", "1"), 1));
  synthetic.path_fan_in = static_cast<std::uint32_t>(parse_u64(arg_value(args, "--fan-in", "3"), 3));
  synthetic.global_congestion = arg_value(args, "--mode", "localized") == "global";
  synthetic.stale_evidence = arg_value(args, "--evidence", "fresh") == "stale";

  SyntheticFabric fabric(synthetic);

  Result<Socket> connected = connect_to(Endpoint{host, port}, 5000);
  if (!connected.ok()) {
    std::cerr << "connect failed: " << connected.status().text() << "\n";
    return 1;
  }
  Socket socket = std::move(connected).value();
  const auto now = []() { return SystemClock{}.now_ms(); };

  Provenance provenance;
  provenance.publisher = PublisherId::from_name(name);
  provenance.incarnation = Incarnation::from(incarnation);
  provenance.boot = BootId::from(boot);
  provenance.emitted_at = now();

  // --- handshake ------------------------------------------------------------
  HelloPayload hello;
  hello.publisher = provenance.publisher;
  hello.incarnation = provenance.incarnation;
  hello.boot = provenance.boot;
  hello.protocol_version = kFrameProtocolVersion;
  hello.name = name;
  std::vector<std::byte> payload;
  if (!encode_hello(hello, payload).ok()) {
    std::cerr << "encode hello failed\n";
    return 1;
  }
  Status status = send_frame(socket, make_frame(MessageType::Hello, provenance, std::move(payload)));
  if (!status.ok()) {
    std::cerr << "hello send failed: " << status.text() << "\n";
    return 1;
  }
  static_cast<void>(socket.set_receive_timeout(5000));
  Result<Frame> reply = receive_frame(socket);
  if (!reply.ok() || reply.value().header.type != MessageType::Welcome) {
    std::cerr << "handshake rejected: "
              << (reply.ok() ? std::string("unexpected reply") : reply.status().text()) << "\n";
    if (reply.ok() && reply.value().header.type == MessageType::Rejected) {
      RejectedPayload rejected;
      if (decode_rejected(std::span<const std::byte>(reply.value().payload.data(),
                                                      reply.value().payload.size()),
                          rejected)
              .ok()) {
        std::cerr << "reason: " << rejected.reason << "\n";
      }
    }
    return 1;
  }
  WelcomePayload welcome;
  status = decode_welcome(std::span<const std::byte>(reply.value().payload.data(),
                                                    reply.value().payload.size()),
                          welcome);
  if (!status.ok()) {
    std::cerr << "welcome decode failed: " << status.text() << "\n";
    return 1;
  }

  provenance.epoch = forced_epoch != 0 ? CoordinatorEpoch::from(forced_epoch) : welcome.epoch;

  std::cout << "WELCOME epoch=" << provenance.epoch.hex() << " coordinator=" << welcome.name
            << std::endl;

  // --- publish --------------------------------------------------------------
  std::uint64_t sequence = 1;
  std::uint64_t published = 0;
  const auto publish = [&](MessageType type, std::vector<std::byte> body, std::uint64_t generation,
                           Millis at) -> bool {
    Provenance stamp = provenance;
    stamp.sequence = sequence++;
    stamp.generation = StreamGeneration{stream_of(type), generation};
    stamp.emitted_at = at;
    Status send_status = send_frame(socket, make_frame(type, stamp, std::move(body)));
    if (!send_status.ok()) {
      std::cerr << "send failed: " << send_status.text() << "\n";
      return false;
    }
    ++published;
    return true;
  };

  for (std::uint64_t round = 0; round < rounds; ++round) {
    const Millis at = now();
    const std::uint64_t generation = start_generation + round;
    fabric.rebuild(at, generation);
    std::vector<std::byte> body;

    if (round == 0) {
      if (!encode_topology_payload(fabric.topology(), body).ok()) return 1;
      if (!publish(MessageType::Topology, body, generation, at)) return 1;
      if (!encode_capacity_payload(fabric.capacity(), body).ok()) return 1;
      if (!publish(MessageType::Capacity, body, generation, at)) return 1;
      if (!encode_paths_payload(fabric.paths(), body).ok()) return 1;
      if (!publish(MessageType::Paths, body, generation, at)) return 1;
      if (!encode_policy_payload(fabric.policy(at), body).ok()) return 1;
      if (!publish(MessageType::Policy, body, generation, at)) return 1;
    }
    if (!encode_signals_payload(fabric.signals(), body).ok()) return 1;
    if (!publish(MessageType::Signals, body, generation, at)) return 1;
    if (!encode_traffic_payload(fabric.traffic(), body).ok()) return 1;
    if (!publish(MessageType::Traffic, body, generation, at)) return 1;

    if (interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
  }

  std::cout << "PUBLISHED " << published << std::endl;
  static_cast<void>(socket.shutdown_both());
  socket.close();
  return 0;
}
