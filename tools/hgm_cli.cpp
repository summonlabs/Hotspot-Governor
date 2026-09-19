// Hotspot Governor command line tool.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "hgm/engine.hpp"
#include "hgm/error.hpp"
#include "hgm/governor.hpp"
#include "hgm/synthetic.hpp"
#include "hgm/version.hpp"

namespace {

using namespace hgm;

void print_usage() {
  std::cout << "hgm_cli " << kVersionString << " — Hotspot Governor\n"
            << "usage:\n"
            << "  hgm_cli version\n"
            << "  hgm_cli selfcheck\n"
            << "  hgm_cli demo [--regions N] [--resources N] [--hotspots N] [--fan-in N]"
               " [--global] [--stale] [--seed N]\n";
}

std::uint32_t parse_u32(const std::string& text, std::uint32_t fallback) {
  try {
    const unsigned long value = std::stoul(text);
    if (value > 0xFFFFFFFFul) return fallback;
    return static_cast<std::uint32_t>(value);
  } catch (...) {
    return fallback;
  }
}

struct DemoOptions {
  SyntheticConfig config{};
  bool global = false;
  bool stale = false;
  std::uint32_t evaluations = 4;
};

// Runs the full pipeline over a SYNTHETIC population and prints the resulting
// explanation. Nothing here is physical-network evidence.
int run_demo(const DemoOptions& options) {
  SyntheticFabric fabric(options.config);
  GovernorConfig governor_config;
  governor_config.node_name = "hgm-cli";
  Governor governor(governor_config);

  WelcomePayload welcome;
  HelloPayload hello;
  hello.publisher = PublisherId::from_name("cli-publisher");
  hello.incarnation = Incarnation::from(1);
  hello.boot = BootId::from(2);
  hello.protocol_version = kFrameProtocolVersion;
  hello.name = "cli";
  const Millis base = options.config.base_time;
  Status status = governor.install_epoch(CoordinatorEpoch::from(1), BootId::from(1), base);
  if (!status.ok()) {
    std::cerr << "install_epoch failed: " << status.text() << "\n";
    return 1;
  }
  status = governor.register_publisher(hello, base, welcome);
  if (!status.ok()) {
    std::cerr << "register_publisher failed: " << status.text() << "\n";
    return 1;
  }

  const auto publish = [&](MessageType type, std::vector<std::byte> payload, std::uint64_t sequence,
                           std::uint64_t generation, Millis at) -> Status {
    Provenance provenance;
    provenance.publisher = hello.publisher;
    provenance.incarnation = hello.incarnation;
    provenance.boot = hello.boot;
    provenance.epoch = welcome.epoch;
    provenance.sequence = sequence;
    provenance.generation = StreamGeneration{stream_of(type), generation};
    provenance.emitted_at = at;
    Frame frame = make_frame(type, provenance, std::move(payload));
    Result<AdmissionReport> admitted = governor.admit(frame, at);
    if (!admitted.ok()) return admitted.status();
    return Status::success();
  };

  std::uint64_t sequence = 1;
  for (std::uint32_t round = 0; round < options.evaluations; ++round) {
    const Millis now = base + static_cast<Millis>(round) * 1000;
    const std::uint64_t generation = 10 + round;
    fabric.rebuild(now, generation);

    // Every stream is republished each round with a consistent generation, so
    // the topology the signals bind to is always the topology that is live.
    std::vector<std::byte> payload;
    if (!encode_topology_payload(fabric.topology(), payload).ok()) return 1;
    status = publish(MessageType::Topology, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "topology rejected: " << status.text() << "\n";
      return 1;
    }
    if (!encode_capacity_payload(fabric.capacity(), payload).ok()) return 1;
    status = publish(MessageType::Capacity, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "capacity rejected: " << status.text() << "\n";
      return 1;
    }
    if (!encode_paths_payload(fabric.paths(), payload).ok()) return 1;
    status = publish(MessageType::Paths, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "paths rejected: " << status.text() << "\n";
      return 1;
    }
    if (!encode_policy_payload(fabric.policy(now), payload).ok()) return 1;
    status = publish(MessageType::Policy, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "policy rejected: " << status.text() << "\n";
      return 1;
    }
    if (!encode_signals_payload(fabric.signals(), payload).ok()) return 1;
    status = publish(MessageType::Signals, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "signals rejected: " << status.text() << "\n";
      return 1;
    }
    if (!encode_traffic_payload(fabric.traffic(), payload).ok()) return 1;
    status = publish(MessageType::Traffic, payload, sequence++, generation, now);
    if (!status.ok()) {
      std::cerr << "traffic rejected: " << status.text() << "\n";
      return 1;
    }

    Decision decision = governor.evaluate(now + 1);
    if (round + 1 == options.evaluations) {
      std::cout << "== SYNTHETIC population: regions=" << options.config.region_count
                << " resources/region=" << options.config.resources_per_region
                << " hotspots=" << options.config.hotspot_count
                << " fan-in=" << options.config.path_fan_in
                << " global=" << (options.global ? "yes" : "no")
                << " stale=" << (options.stale ? "yes" : "no") << "\n";
      std::cout << decision.explanation << "\n";
      std::cout << "metrics: " << governor.metrics().render() << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  if (args.empty()) {
    print_usage();
    return 0;
  }

  const std::string command = args[0];
  if (command == "version") {
    std::cout << hgm::kProductName << " " << hgm::version_string() << "\n";
    std::cout << "durable-format=" << hgm::kDurableFormatVersion
              << " frame-protocol=" << hgm::kFrameProtocolVersion << "\n";
    return 0;
  }
  if (command == "selfcheck") {
    DemoOptions options;
    options.config.region_count = 4;
    options.config.resources_per_region = 6;
    options.config.hotspot_count = 1;
    options.config.path_fan_in = 3;
    options.evaluations = 4;
    return run_demo(options);
  }
  if (command == "demo") {
    DemoOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string& flag = args[i];
      const auto next = [&](std::uint32_t fallback) {
        if (i + 1 < args.size()) return parse_u32(args[++i], fallback);
        return fallback;
      };
      if (flag == "--regions") {
        options.config.region_count = next(options.config.region_count);
      } else if (flag == "--resources") {
        options.config.resources_per_region = next(options.config.resources_per_region);
      } else if (flag == "--hotspots") {
        options.config.hotspot_count = next(options.config.hotspot_count);
      } else if (flag == "--fan-in") {
        options.config.path_fan_in = next(options.config.path_fan_in);
      } else if (flag == "--seed") {
        options.config.seed = next(static_cast<std::uint32_t>(options.config.seed));
      } else if (flag == "--global") {
        options.global = true;
        options.config.global_congestion = true;
      } else if (flag == "--stale") {
        options.stale = true;
        options.config.stale_evidence = true;
      } else {
        std::cerr << "unknown option: " << flag << "\n";
        print_usage();
        return 2;
      }
    }
    return run_demo(options);
  }

  std::cerr << "unknown command: " << command << "\n";
  print_usage();
  return 2;
}
