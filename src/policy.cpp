// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/policy.hpp"

#include <vector>

#include "hgm/byteio.hpp"

namespace hgm {

Digest PolicySnapshot::content_digest() const noexcept {
  std::vector<std::byte> encoded;
  encoded.reserve(256);
  ByteWriter writer(encoded);
  writer.u64(id_.value());
  writer.u64(generation_.value());
  writer.i64(issued_at_);
  writer.u32(ttl_ms_);
  writer.u32(thresholds_.saturation_utilization_ppm);
  writer.u32(thresholds_.queue_pressure_ppm);
  writer.u32(thresholds_.buffer_pressure_ppm);
  writer.u32(thresholds_.min_confidence_ppm);
  writer.u32(thresholds_.max_signal_age_ms);
  writer.u32(thresholds_.max_capacity_age_ms);
  writer.u32(thresholds_.max_topology_age_ms);
  writer.u32(thresholds_.max_path_age_ms);
  writer.u32(thresholds_.max_traffic_age_ms);
  writer.u32(thresholds_.persistence_window_ms);
  writer.u32(thresholds_.min_persistence_samples);
  writer.u32(thresholds_.max_local_share_ppm);
  writer.u32(thresholds_.global_share_ppm);
  writer.u32(thresholds_.max_local_regions);
  writer.u32(thresholds_.max_regional_regions);
  writer.u32(thresholds_.min_coverage_ppm);
  writer.u32(thresholds_.min_attribution_confidence_ppm);
  writer.u32(thresholds_.dominant_contributor_ppm);
  writer.u32(thresholds_.contradiction_utilization_ppm);
  writer.u32(budget_.max_affected_resources);
  writer.u32(budget_.max_affected_paths);
  writer.u32(budget_.max_affected_flows);
  writer.u32(budget_.max_rate_delta_ppm);
  writer.u32(budget_.max_scope_share_ppm);
  writer.u32(budget_.max_plan_scope_share_ppm);
  writer.u32(budget_.max_intents_per_plan);
  writer.u8(budget_.allow_flow_relocation ? 1u : 0u);
  writer.u8(budget_.allow_path_rebalance ? 1u : 0u);
  writer.u8(budget_.allow_admission_reduction ? 1u : 0u);
  writer.u8(budget_.allow_rate_change ? 1u : 0u);
  writer.u8(budget_.allow_resource_isolation ? 1u : 0u);
  writer.u8(budget_.allow_global_escalation ? 1u : 0u);
  writer.u8(budget_.require_attribution_for_relocation ? 1u : 0u);
  writer.u32(bands_.low_ppm);
  writer.u32(bands_.moderate_ppm);
  writer.u32(bands_.high_ppm);
  writer.u32(bands_.critical_ppm);
  return digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
}

}  // namespace hgm
