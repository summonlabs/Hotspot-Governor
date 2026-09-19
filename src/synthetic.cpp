// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/synthetic.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "hgm/checked.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

ResourceId resource_id(std::uint32_t region, std::uint32_t index) {
  std::string name = "res-";
  name.append(std::to_string(region));
  name.push_back('-');
  name.append(std::to_string(index));
  return ResourceId::from_name(name);
}

RegionId region_id(std::uint32_t index) {
  std::string name = "region-";
  name.append(std::to_string(index));
  return RegionId::from_name(name);
}

LinkId link_id(ResourceId a, ResourceId b) {
  const ResourceId lo = a < b ? a : b;
  const ResourceId hi = a < b ? b : a;
  std::string name = "link-";
  name.append(hex64(lo.value()));
  name.push_back('-');
  name.append(hex64(hi.value()));
  return LinkId::from_name(name);
}

}  // namespace

SyntheticFabric::SyntheticFabric(SyntheticConfig config) : config_(config) {
  if (config_.region_count == 0) config_.region_count = 1;
  if (config_.resources_per_region == 0) config_.resources_per_region = 1;
  if (config_.path_fan_in == 0) config_.path_fan_in = 1;
  rebuild(config_.base_time, 1);
}

Provenance SyntheticFabric::provenance(StreamKind stream) const {
  Provenance provenance;
  provenance.publisher = PublisherId::from_name(std::string("synthetic-") + std::string(to_string(stream)));
  provenance.incarnation = Incarnation::from(1);
  provenance.boot = BootId::from(1);
  provenance.epoch = CoordinatorEpoch::from(1);
  provenance.sequence = 1;
  provenance.generation = StreamGeneration{stream, generation_};
  provenance.emitted_at = now_;
  provenance.source = "synthetic";
  return provenance;
}

void SyntheticFabric::rebuild(Millis now, std::uint64_t generation) {
  now_ = now;
  generation_ = generation;

  // --- topology -------------------------------------------------------------
  topology_ = TopologySnapshot{};
  for (std::uint32_t r = 0; r < config_.region_count; ++r) {
    topology_.add_region(region_id(r), "region-" + std::to_string(r));
  }
  all_resources_.clear();
  all_resources_.reserve(static_cast<std::size_t>(config_.region_count) * config_.resources_per_region);
  for (std::uint32_t r = 0; r < config_.region_count; ++r) {
    for (std::uint32_t i = 0; i < config_.resources_per_region; ++i) {
      const ResourceId id = resource_id(r, i);
      topology_.add_resource(id, region_id(r), i == 0 ? ResourceKind::Switch : ResourceKind::Port,
                             "res-" + std::to_string(r) + "-" + std::to_string(i));
      all_resources_.push_back(id);
    }
  }
  for (std::uint32_t r = 0; r < config_.region_count; ++r) {
    for (std::uint32_t i = 0; i < config_.resources_per_region; ++i) {
      const std::uint32_t next = (i + 1) % config_.resources_per_region;
      // Add each ring edge once; a two-resource region would otherwise emit the
      // same adjacency twice.
      if (i >= next) continue;
      const ResourceId a = resource_id(r, i);
      const ResourceId b = resource_id(r, next);
      topology_.add_link(link_id(a, b), a, b);
    }
  }
  for (std::uint32_t r = 0; r + 1 < config_.region_count; ++r) {
    const ResourceId a = resource_id(r, 0);
    const ResourceId b = resource_id(r + 1, 0);
    topology_.add_link(link_id(a, b), a, b);
  }
  topology_.set_generation(TopologyGeneration::from(generation));
  topology_.set_provenance(provenance(StreamKind::Topology));
  topology_.set_validity(now - 1000, now + 60000);
  build_status_ = topology_.build();

  // --- designated saturated / healthy resources -----------------------------
  saturated_.clear();
  if (config_.global_congestion) {
    saturated_ = all_resources_;
  } else {
    const std::uint32_t per_region =
        config_.resources_per_region > 1 ? config_.resources_per_region - 1 : 1;
    for (std::uint32_t h = 0; h < config_.hotspot_count; ++h) {
      const std::uint32_t region = h % config_.region_count;
      const std::uint32_t slot = h / config_.region_count;
      const std::uint32_t index =
          config_.hotspot_index_base + (slot * 2) % per_region;
      if (index >= config_.resources_per_region) continue;
      const ResourceId id = resource_id(region, index);
      if (std::find(saturated_.begin(), saturated_.end(), id) == saturated_.end()) {
        saturated_.push_back(id);
      }
    }
  }
  std::sort(saturated_.begin(), saturated_.end());

  healthy_.clear();
  for (const ResourceId id : all_resources_) {
    if (!std::binary_search(saturated_.begin(), saturated_.end(), id)) healthy_.push_back(id);
  }

  // --- capacity -------------------------------------------------------------
  capacity_ = CapacitySnapshot{};
  for (const ResourceId id : all_resources_) {
    ResourceCapacity& entry = capacity_.add(id);
    entry.capacity_units = config_.capacity_units;
    entry.queue_limit_units = config_.queue_limit_units;
    entry.buffer_limit_bytes = config_.buffer_limit_bytes;
    entry.usable = true;
  }
  capacity_.set_generation(CapacityGeneration::from(generation));
  capacity_.set_topology_generation(TopologyGeneration::from(generation));
  capacity_.set_provenance(provenance(StreamKind::Capacity));
  capacity_.set_observed_at(now);
  if (build_status_.ok()) build_status_ = capacity_.build();

  // --- signals --------------------------------------------------------------
  signals_ = SignalSnapshot{};
  const Millis observed_at = config_.stale_evidence
                                 ? now - static_cast<Millis>(config_.evidence_age_ms) - 600000
                                 : now - static_cast<Millis>(config_.evidence_age_ms);
  for (const ResourceId id : all_resources_) {
    const std::uint64_t bucket = identity_from_bytes(hex64(id.value())) % kPpmScale;
    if (bucket >= config_.evidence_density_ppm) continue;
    const bool is_saturated = std::binary_search(saturated_.begin(), saturated_.end(), id);
    ResourceSignals& record = signals_.add(id);
    record.utilized_units =
        (static_cast<std::uint64_t>(is_saturated ? config_.saturated_utilization_ppm
                                                 : config_.healthy_utilization_ppm) *
         config_.capacity_units) /
        kPpmScale;
    record.queue_depth_units =
        (static_cast<std::uint64_t>(is_saturated ? config_.saturated_queue_ppm
                                                 : config_.healthy_queue_ppm) *
         config_.queue_limit_units) /
        kPpmScale;
    record.buffer_used_bytes = 0;
    record.offered_bps =
        is_saturated ? config_.saturated_demand_bps : config_.healthy_demand_bps;
    record.admitted_bps = record.offered_bps;
    record.drop_units = is_saturated ? 1 : 0;
    record.confidence_ppm = config_.confidence_ppm;
    record.quality = config_.quality;
    record.observed_at = observed_at;
    if (config_.contradictory_sample && is_saturated) {
      // Occupancy pressure with no utilisation behind it.
      record.utilized_units = config_.capacity_units / 10;
      record.offered_bps = 0;
      record.admitted_bps = 0;
    }
  }
  signals_.set_generation(EvidenceGeneration::from(generation));
  signals_.set_topology_generation(TopologyGeneration::from(generation));
  signals_.set_capacity_generation(CapacityGeneration::from(generation));
  signals_.set_provenance(provenance(StreamKind::Signals));
  signals_.set_window(now - 1000, observed_at);
  if (build_status_.ok()) build_status_ = signals_.build();

  // --- paths ----------------------------------------------------------------
  paths_ = PathSnapshot{};
  traffic_ = TrafficSnapshot{};
  std::vector<std::pair<PathId, std::uint64_t>> demands;
  for (const ResourceId id : all_resources_) {
    if (paths_.size() + config_.path_fan_in > Limits::kMaxPaths) break;
    const std::uint64_t bucket = identity_from_bytes(std::string("p") + hex64(id.value())) % kPpmScale;
    if (bucket >= config_.path_resource_density_ppm) continue;
    const bool is_saturated = std::binary_search(saturated_.begin(), saturated_.end(), id);
    for (std::uint32_t k = 0; k < config_.path_fan_in; ++k) {
      const std::string name = "path-" + hex64(id.value()) + "-" + std::to_string(k);
      const PathId path_id = PathId::from_name(name);
      const FlowId flow_id = FlowId::from_name("flow-" + hex64(id.value()) + "-" + std::to_string(k));
      PathRecord& path = paths_.add(path_id, flow_id);
      path.hops.push_back(id);
      const auto neighbours = topology_.neighbors(id);
      if (!neighbours.empty()) {
        path.hops.push_back(neighbours[static_cast<std::size_t>(k) % neighbours.size()]);
      }
      const std::uint64_t demand = is_saturated ? config_.saturated_demand_bps / config_.path_fan_in
                                                : config_.healthy_demand_bps / config_.path_fan_in;
      demands.emplace_back(path_id, demand == 0 ? 1ull : demand);
    }
  }
  paths_.set_generation(PathGeneration::from(generation));
  paths_.set_topology_generation(TopologyGeneration::from(generation));
  paths_.set_provenance(provenance(StreamKind::Paths));
  paths_.set_observed_at(now);
  if (build_status_.ok()) build_status_ = paths_.build();

  for (const std::pair<PathId, std::uint64_t>& entry : demands) {
    const PathRecord* record = paths_.find(entry.first);
    const FlowId flow = record != nullptr ? record->flow : FlowId{};
    TrafficRecord& traffic_record = traffic_.add(flow, entry.first);
    traffic_record.demand_bps = entry.second;
    traffic_record.quality = TelemetryQuality::Healthy;
    traffic_record.observed_at = now - static_cast<Millis>(config_.traffic_age_ms);
  }
  traffic_.set_generation(TrafficGeneration::from(generation));
  traffic_.set_path_generation(PathGeneration::from(generation));
  traffic_.set_provenance(provenance(StreamKind::Traffic));
  if (build_status_.ok()) build_status_ = traffic_.build();

}

PolicySnapshot SyntheticFabric::policy(Millis now) const {
  PolicySnapshot policy;
  policy.set_id(PolicyId::from_name("synthetic-policy"));
  policy.set_generation(PolicyGeneration::from(generation_));
  policy.set_issued_at(now - 100);
  policy.set_ttl_ms(config_.policy_ttl_ms);
  policy.set_provenance(provenance(StreamKind::Policy));
  return policy;
}

}  // namespace hgm
