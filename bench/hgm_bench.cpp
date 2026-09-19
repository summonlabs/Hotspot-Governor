// Hotspot Governor benchmark harness.
//
// EVERYTHING THIS HARNESS MEASURES IS SYNTHETIC. It drives the real detection,
// attribution and intervention code over generated fabric populations. No
// physical switch, NIC, link, queue or RDMA fabric is involved, and no result
// here may be presented as a physical-network measurement.
//
// The measured quantity is completed work: resources fully evaluated and
// decisions fully produced. Generation and submission time are excluded.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hgm/engine.hpp"
#include "hgm/frame.hpp"
#include "hgm/governor.hpp"
#include "hgm/synthetic.hpp"
#include "hgm/version.hpp"

namespace {

using namespace hgm;

struct BenchCase {
  std::string name;
  std::uint32_t regions;
  std::uint32_t resources_per_region;
  std::uint32_t hotspots;
  std::uint32_t fan_in;
  std::uint32_t evidence_density_ppm;
  bool global_congestion;
  std::uint32_t evaluations;
};

struct BenchResult {
  std::string name;
  std::uint64_t resources = 0;
  std::uint64_t paths = 0;
  std::uint64_t evaluations = 0;
  std::uint64_t hotspots = 0;
  std::uint64_t candidates = 0;
  std::uint64_t intents = 0;
  std::uint64_t rejected = 0;
  double total_ms = 0;
  double generation_ms = 0;
  CongestionScope scope = CongestionScope::Unknown;
  bool escalation = false;
};

BenchResult run_case(const BenchCase& bench_case) {
  BenchResult result;
  result.name = bench_case.name;

  SyntheticConfig config;
  config.region_count = bench_case.regions;
  config.resources_per_region = bench_case.resources_per_region;
  config.hotspot_count = bench_case.hotspots;
  config.path_fan_in = bench_case.fan_in;
  config.path_resource_density_ppm = 1000000u;
  config.evidence_density_ppm = bench_case.evidence_density_ppm;
  config.global_congestion = bench_case.global_congestion;

  SyntheticFabric fabric(config);

  // The measured path is the real one: framed admission through a governor,
  // then evaluation. Fabric generation is excluded and reported separately.
  GovernorConfig governor_config;
  governor_config.node_name = "bench-governor";
  governor_config.include_explanation = false;
  Governor governor(governor_config);

  const Millis base = config.base_time;
  if (!governor.recover(base).ok() || !governor.install_epoch(CoordinatorEpoch::from(1), BootId::from(1), base).ok()) {
    result.name += " (setup-failed)";
    return result;
  }
  HelloPayload hello;
  hello.publisher = PublisherId::from_name("bench-publisher");
  hello.incarnation = Incarnation::from(1);
  hello.boot = BootId::from(1);
  hello.protocol_version = kFrameProtocolVersion;
  hello.name = "bench";
  WelcomePayload welcome;
  if (!governor.register_publisher(hello, base, welcome).ok()) {
    result.name += " (setup-failed)";
    return result;
  }

  std::uint64_t sequence = 1;
  const auto publish = [&](MessageType type, std::vector<std::byte> payload, std::uint64_t generation,
                           Millis at) -> bool {
    Provenance provenance;
    provenance.publisher = hello.publisher;
    provenance.incarnation = hello.incarnation;
    provenance.boot = hello.boot;
    provenance.epoch = welcome.epoch;
    provenance.sequence = sequence++;
    provenance.generation = StreamGeneration{stream_of(type), generation};
    provenance.emitted_at = at;
    Frame frame = make_frame(type, provenance, std::move(payload));
    return governor.admit(frame, at).ok();
  };

  Millis now = base;
  std::uint64_t generation = 1;
  double generation_ms = 0;
  double work_ms = 0;
  for (std::uint32_t round = 0; round < bench_case.evaluations; ++round) {
    now += 1000;
    ++generation;
    const auto generation_start = std::chrono::steady_clock::now();
    fabric.rebuild(now, generation);
    const auto generation_end = std::chrono::steady_clock::now();
    generation_ms += std::chrono::duration<double, std::milli>(generation_end - generation_start).count();

    // Every stream is republished each round: the measurement covers the full
    // framed admission path, including structural verification against the live
    // topology generation.
    std::vector<std::byte> body;
    const auto work_start = std::chrono::steady_clock::now();
    static_cast<void>(encode_topology_payload(fabric.topology(), body));
    if (!publish(MessageType::Topology, body, generation, now)) ++result.rejected;
    static_cast<void>(encode_capacity_payload(fabric.capacity(), body));
    if (!publish(MessageType::Capacity, body, generation, now)) ++result.rejected;
    static_cast<void>(encode_paths_payload(fabric.paths(), body));
    if (!publish(MessageType::Paths, body, generation, now)) ++result.rejected;
    static_cast<void>(encode_policy_payload(fabric.policy(now), body));
    if (!publish(MessageType::Policy, body, generation, now)) ++result.rejected;
    static_cast<void>(encode_signals_payload(fabric.signals(), body));
    if (!publish(MessageType::Signals, body, generation, now)) ++result.rejected;
    if (!publish(MessageType::Traffic, [&] {
          std::vector<std::byte> traffic_body;
          static_cast<void>(encode_traffic_payload(fabric.traffic(), traffic_body));
          return traffic_body;
        }(), generation, now)) {
      ++result.rejected;
    }

    const Decision decision = governor.evaluate(now + 1);
    const auto work_end = std::chrono::steady_clock::now();
    work_ms += std::chrono::duration<double, std::milli>(work_end - work_start).count();

    result.scope = decision.assessment.scope;
    result.hotspots = decision.assessment.hotspots.size();
    result.candidates = decision.assessment.candidates.size();
    result.intents = decision.plan.intents.size();
    result.escalation = decision.assessment.escalation_required;
    result.paths = fabric.paths().size();
    result.resources = fabric.topology().resource_count();
    ++result.evaluations;
  }
  result.total_ms = work_ms;
  result.generation_ms = generation_ms;
  return result;
}

std::vector<BenchCase> full_suite() {
  std::vector<BenchCase> cases;
  // Graph size sweep with a fixed single hotspot and fan-in.
  cases.push_back(BenchCase{"graph-1k", 8, 128, 1, 4, 1000000, false, 4});
  cases.push_back(BenchCase{"graph-8k", 16, 512, 1, 4, 1000000, false, 4});
  cases.push_back(BenchCase{"graph-64k", 32, 2048, 1, 4, 1000000, false, 4});
  // Hotspot count sweep at a fixed graph size.
  cases.push_back(BenchCase{"hotspots-8", 16, 256, 8, 4, 1000000, false, 4});
  cases.push_back(BenchCase{"hotspots-64", 16, 256, 64, 4, 1000000, false, 4});
  cases.push_back(BenchCase{"hotspots-256", 16, 256, 256, 4, 1000000, false, 4});
  // Fan-in sweep.
  cases.push_back(BenchCase{"fanin-1", 8, 256, 4, 1, 1000000, false, 4});
  cases.push_back(BenchCase{"fanin-16", 8, 256, 4, 16, 1000000, false, 4});
  cases.push_back(BenchCase{"fanin-64", 8, 256, 4, 64, 1000000, false, 4});
  // Evidence density sweep.
  cases.push_back(BenchCase{"density-100", 8, 256, 4, 4, 1000000, false, 4});
  cases.push_back(BenchCase{"density-70", 8, 256, 4, 4, 700000, false, 4});
  cases.push_back(BenchCase{"density-40", 8, 256, 4, 4, 400000, false, 4});
  // Global congestion.
  cases.push_back(BenchCase{"global-8k", 16, 512, 0, 4, 1000000, true, 4});
  return cases;
}

std::vector<BenchCase> quick_suite() {
  std::vector<BenchCase> cases;
  cases.push_back(BenchCase{"graph-1k", 8, 128, 1, 4, 1000000, false, 3});
  cases.push_back(BenchCase{"hotspots-8", 8, 128, 8, 4, 1000000, false, 3});
  cases.push_back(BenchCase{"fanin-16", 4, 64, 2, 16, 1000000, false, 3});
  cases.push_back(BenchCase{"density-40", 4, 64, 2, 4, 400000, false, 3});
  cases.push_back(BenchCase{"global-1k", 8, 128, 0, 4, 1000000, true, 3});
  return cases;
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quick") quick = true;
  }

  std::printf("Hotspot Governor %s benchmark — SYNTHETIC populations only\n",
              std::string(kVersionString).c_str());
  std::printf("measured quantity: completed work through the real runtime path\n");
  std::printf("(framed admission into a governor, then one evaluation per round)\n");
  std::printf("excluded: synthetic fabric generation, reported separately as generation_ms\n\n");

  const std::vector<BenchCase> cases = quick ? quick_suite() : full_suite();
  std::printf("%-14s %10s %10s %6s %9s %9s %10s %12s %10s\n", "case", "resources", "paths", "evals",
              "hotspots", "intents", "total ms", "ms/eval", "ns/res-eval");
  std::printf("%-14s %10s %10s %6s %9s %9s %10s %12s %10s\n", "----", "---------", "-----", "-----",
              "--------", "-------", "--------", "-------", "-----------");

  double worst = 0;
  std::string worst_case;
  for (const BenchCase& bench_case : cases) {
    const BenchResult result = run_case(bench_case);
    const double per_evaluation = result.evaluations == 0 ? 0 : result.total_ms / static_cast<double>(result.evaluations);
    const double per_resource_ns =
        (result.evaluations == 0 || result.resources == 0)
            ? 0
            : (result.total_ms * 1e6) / (static_cast<double>(result.resources) * static_cast<double>(result.evaluations));
    std::printf("%-14s %10llu %10llu %6llu %9llu %9llu %10.2f %12.3f %10.1f\n",
                result.name.c_str(), static_cast<unsigned long long>(result.resources),
                static_cast<unsigned long long>(result.paths),
                static_cast<unsigned long long>(result.evaluations),
                static_cast<unsigned long long>(result.hotspots),
                static_cast<unsigned long long>(result.intents), result.total_ms, per_evaluation,
                per_resource_ns);
    std::printf("               scope=%s escalation=%s rejected=%llu generation_ms=%.2f\n",
                std::string(to_string(result.scope)).c_str(), result.escalation ? "yes" : "no",
                static_cast<unsigned long long>(result.rejected), result.generation_ms);
    if (per_evaluation > worst) {
      worst = per_evaluation;
      worst_case = result.name;
    }
  }
  std::printf("\nworst case: %s at %.3f ms per evaluation (SYNTHETIC)\n", worst_case.c_str(), worst);
  std::printf("no physical-network measurement is made or implied by this harness\n");
  return 0;
}
