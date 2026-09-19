// Coordinator process: listens on a real socket, admits real framed publisher
// traffic and evaluates the governor.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "hgm/coordinator.hpp"
#include "hgm/governor.hpp"
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

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
  for (const std::string& value : args) {
    if (value == flag) return true;
  }
  return false;
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

  CoordinatorConfig config;
  config.name = arg_value(args, "--name", "hgm-coordinator");
  config.endpoint.host = arg_value(args, "--host", "127.0.0.1");
  config.endpoint.port = static_cast<std::uint16_t>(parse_u64(arg_value(args, "--port", "0"), 0));
  config.max_connections = static_cast<std::size_t>(parse_u64(arg_value(args, "--max-connections", "32"), 32));

  GovernorConfig governor_config;
  governor_config.node_name = config.name;
  const std::string state_dir = arg_value(args, "--state-dir", "");
  if (!state_dir.empty()) {
    StoreConfig store_config;
    store_config.directory = state_dir;
    governor_config.store = store_config;
  }
  if (has_flag(args, "--drop-on-death")) governor_config.drop_evidence_on_publisher_death = true;

  Governor governor(governor_config);
  const Millis now = SystemClock{}.now_ms();
  const bool has_state_dir = !state_dir.empty();
  Status status = governor.recover(now);
  if (!status.ok()) {
    std::cerr << "recover failed: " << status.text() << "\n";
    return 1;
  }
  if (governor.recovery().recovered) {
    std::cout << "RECOVERED epoch=" << governor.epoch().hex()
              << " previous_boot=" << governor.recovery().previous_boot.hex()
              << " discarded_tail=" << governor.recovery().discarded_tail_bytes
              << " journal_records=" << governor.recovery().applied_journal_records << std::endl;
  }

  if (has_state_dir) {
    // Commit the boot so a supervising process can observe the advanced epoch
    // even if this process is killed rather than stopped.
    static_cast<void>(governor.commit_durable(now));
  }

  Coordinator coordinator(config, governor);
  status = coordinator.start();
  if (!status.ok()) {
    std::cerr << "coordinator start failed: " << status.text() << "\n";
    return 1;
  }
  std::cout << "READY port=" << coordinator.port() << " epoch=" << coordinator.epoch().hex()
            << " boot=" << coordinator.boot().hex() << std::endl;

  const std::uint64_t exit_after = parse_u64(arg_value(args, "--exit-after-evals", "0"), 0);
  const std::uint64_t evaluate_every = parse_u64(arg_value(args, "--evaluate-every", "1"), 1);

  // A small reporter so that a supervising process can observe admission,
  // refusal and publisher-loss progress without polling the coordinator itself.
  std::atomic<bool> monitor_stop{false};
  std::thread monitor([&]() {
    std::uint64_t last_admitted = 0;
    std::uint64_t last_refused = 0;
    std::uint64_t last_lost = 0;
    for (;;) {
      const bool stopping = monitor_stop.load();
      const std::uint64_t admitted = coordinator.frames_admitted();
      const std::uint64_t refused = coordinator.frames_refused();
      const std::uint64_t lost = coordinator.publishers_lost();
      if (admitted != last_admitted || refused != last_refused || lost != last_lost) {
        last_admitted = admitted;
        last_refused = refused;
        last_lost = lost;
        std::cout << "STATUS admitted=" << admitted << " refused=" << refused << " lost=" << lost
                  << " connections=" << coordinator.connection_count()
                  << " hellos=" << coordinator.hellos() << std::endl;
      }
      if (stopping) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  std::uint64_t last_admitted = 0;
  std::uint64_t evaluations = 0;
  for (;;) {
    coordinator.wait_for_admissions(last_admitted + evaluate_every);
    const std::uint64_t admitted = coordinator.frames_admitted();
    if (admitted <= last_admitted && !has_flag(args, "--spin")) {
      if (admitted == 0 && coordinator.connections_accepted() == 0) {
        // Nothing yet; the wait above blocks until there is work.
        continue;
      }
    }
    if (admitted == last_admitted) {
      if (exit_after != 0 && evaluations >= exit_after) break;
      continue;
    }
    last_admitted = admitted;
    const Millis at = SystemClock{}.now_ms();
    Decision decision = governor.evaluate(at);
    ++evaluations;
    std::cout << "EVAL n=" << evaluations << " scope=" << to_string(decision.assessment.scope)
              << " hotspots=" << decision.assessment.hotspots.size()
              << " candidates=" << decision.assessment.candidates.size()
              << " intents=" << decision.plan.intents.size()
              << " escalation=" << (decision.assessment.escalation_required ? 1 : 0) << std::endl;
    if (exit_after != 0 && evaluations >= exit_after) break;
  }

  monitor_stop.store(true);
  monitor.join();

  // A configured state directory always commits on a clean exit, so a
  // supervising process can rely on the durable state being current.
  if (has_state_dir) {
    status = governor.commit_durable(SystemClock{}.now_ms());
    std::cout << "COMMIT " << (status.ok() ? "ok" : status.text()) << std::endl;
  }
  static_cast<void>(coordinator.stop());
  std::cout << "STOPPED " << coordinator.render() << std::endl;
  return 0;
}
