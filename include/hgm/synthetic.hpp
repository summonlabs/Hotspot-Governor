// Deterministic SYNTHETIC fabric populations.
//
// Everything this module produces is generated data. It exists so that tests,
// tools and the benchmark harness can exercise the runtime without a physical
// fabric. Nothing here is, or may be presented as, physical-network evidence.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hgm/capacity.hpp"
#include "hgm/error.hpp"
#include "hgm/paths.hpp"
#include "hgm/policy.hpp"
#include "hgm/signals.hpp"
#include "hgm/time.hpp"
#include "hgm/topology.hpp"
#include "hgm/traffic.hpp"

namespace hgm {

// xorshift64* — deterministic, seedable, not cryptographic. Used only to shape
// synthetic populations.
class SyntheticRng {
 public:
  explicit SyntheticRng(std::uint64_t seed) noexcept
      : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() noexcept {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::uint32_t next_u32() noexcept { return static_cast<std::uint32_t>(next() >> 32); }
  // Uniform in [0, bound); 0 when bound == 0.
  std::uint32_t below(std::uint32_t bound) noexcept {
    return bound == 0 ? 0u : static_cast<std::uint32_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

struct SyntheticConfig {
  std::uint32_t region_count = 4;
  std::uint32_t resources_per_region = 8;
  std::uint32_t hotspot_count = 1;
  // Index inside each region of the first designated saturated resource.
  std::uint32_t hotspot_index_base = 1;
  std::uint32_t path_fan_in = 4;
  std::uint32_t path_resource_density_ppm = 250000;
  std::uint32_t evidence_density_ppm = 1000000;
  std::uint32_t saturated_utilization_ppm = 980000;
  std::uint32_t healthy_utilization_ppm = 200000;
  std::uint32_t saturated_queue_ppm = 900000;
  std::uint32_t healthy_queue_ppm = 50000;
  std::uint32_t confidence_ppm = 900000;
  TelemetryQuality quality = TelemetryQuality::Healthy;
  bool global_congestion = false;
  bool contradictory_sample = false;
  bool stale_evidence = false;
  std::uint32_t evidence_age_ms = 0;
  std::uint32_t traffic_age_ms = 0;
  std::uint64_t capacity_units = 1000;
  std::uint64_t queue_limit_units = 100;
  std::uint64_t buffer_limit_bytes = 1000000;
  std::uint64_t saturated_demand_bps = 900000;
  std::uint64_t healthy_demand_bps = 1000;
  std::uint32_t policy_ttl_ms = 60000;
  std::uint64_t seed = 0x5EED1234ull;
  Millis base_time = 1000;
};

class SyntheticFabric {
 public:
  explicit SyntheticFabric(SyntheticConfig config = SyntheticConfig{});

  // Rebuilds every snapshot for the given evaluation time and generation.
  void rebuild(Millis now, std::uint64_t generation);

  const TopologySnapshot& topology() const noexcept { return topology_; }
  const CapacitySnapshot& capacity() const noexcept { return capacity_; }
  const SignalSnapshot& signals() const noexcept { return signals_; }
  const PathSnapshot& paths() const noexcept { return paths_; }
  const TrafficSnapshot& traffic() const noexcept { return traffic_; }

  // A live policy stamped shortly before "now".
  PolicySnapshot policy(Millis now) const;

  const std::vector<ResourceId>& saturated_resources() const noexcept { return saturated_; }
  const std::vector<ResourceId>& healthy_resources() const noexcept { return healthy_; }
  const SyntheticConfig& config() const noexcept { return config_; }
  std::uint64_t generation() const noexcept { return generation_; }

  // Status of the last rebuild. A generated population that cannot be built is
  // reported here rather than silently ignored.
  const Status& build_status() const noexcept { return build_status_; }
  bool build_ok() const noexcept { return build_status_.ok(); }

  Provenance provenance(StreamKind stream) const;

 private:
  SyntheticConfig config_;
  TopologySnapshot topology_;
  CapacitySnapshot capacity_;
  SignalSnapshot signals_;
  PathSnapshot paths_;
  TrafficSnapshot traffic_;
  std::vector<ResourceId> saturated_;
  std::vector<ResourceId> healthy_;
  std::vector<ResourceId> all_resources_;
  std::uint64_t generation_ = 0;
  Millis now_ = 0;
  Status build_status_ = Status::success();
};

}  // namespace hgm
