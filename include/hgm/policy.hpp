// Policy evidence: every binding threshold and every mitigation budget the
// governor is allowed to apply. Without a live policy the governor detects but
// authorises nothing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "hgm/digest.hpp"
#include "hgm/enums.hpp"
#include "hgm/ids.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"

namespace hgm {

// Pressure thresholds in parts-per-million.
struct Thresholds {
  std::uint32_t saturation_utilization_ppm = 900000;  // 90% of capacity
  std::uint32_t queue_pressure_ppm = 850000;          // 85% of queue limit
  std::uint32_t buffer_pressure_ppm = 900000;         // 90% of buffer limit
  std::uint32_t min_confidence_ppm = 700000;          // sample trust floor

  std::uint32_t max_signal_age_ms = 5000;
  std::uint32_t max_capacity_age_ms = 30000;
  std::uint32_t max_topology_age_ms = 60000;
  std::uint32_t max_path_age_ms = 30000;
  std::uint32_t max_traffic_age_ms = 5000;

  // Persistence: how long saturation must hold before it becomes a hotspot
  // with authority.
  std::uint32_t persistence_window_ms = 2000;
  std::uint32_t min_persistence_samples = 3;

  // Locality: share of fabric capacity a hotspot may cover and still count as
  // localized, and the share at which locality is considered broken.
  std::uint32_t max_local_share_ppm = 250000;  // 25%
  std::uint32_t global_share_ppm = 600000;     // 60%

  // Maximum number of distinct regions a localized hotspot may span.
  std::uint32_t max_local_regions = 1;
  std::uint32_t max_regional_regions = 4;

  // Telemetry coverage floor: below this the answer is UNKNOWN, never "none".
  std::uint32_t min_coverage_ppm = 600000;

  // Attribution.
  std::uint32_t min_attribution_confidence_ppm = 500000;
  std::uint32_t dominant_contributor_ppm = 500000;

  // A utilisation sample below this while queue/buffer pressure is high is a
  // contradiction: the governor refuses to localize on it.
  std::uint32_t contradiction_utilization_ppm = 400000;
};

// Bounded mitigation budget. These caps are what stop a local intent from
// silently becoming global policy.
struct MitigationBudget {
  std::uint32_t max_affected_resources = 16;
  std::uint32_t max_affected_paths = 32;
  std::uint32_t max_affected_flows = 32;
  std::uint32_t max_rate_delta_ppm = 250000;
  std::uint32_t max_scope_share_ppm = 250000;       // per intent
  std::uint32_t max_plan_scope_share_ppm = 400000;  // across the whole plan
  std::uint32_t max_intents_per_plan = 8;

  bool allow_flow_relocation = true;
  bool allow_path_rebalance = true;
  bool allow_admission_reduction = true;
  bool allow_rate_change = true;
  bool allow_resource_isolation = true;
  bool allow_global_escalation = true;

  // Attribution-dependent intents require fresh contributor evidence.
  bool require_attribution_for_relocation = true;

  bool allows(MitigationKind kind) const noexcept {
    switch (kind) {
      case MitigationKind::FlowRelocation: return allow_flow_relocation;
      case MitigationKind::PathRebalance: return allow_path_rebalance;
      case MitigationKind::AdmissionReduction: return allow_admission_reduction;
      case MitigationKind::RatePacingChange: return allow_rate_change;
      case MitigationKind::ResourceIsolation: return allow_resource_isolation;
      case MitigationKind::EscalateGlobalCongestion: return allow_global_escalation;
      case MitigationKind::None: return false;
    }
    return false;
  }
};

// Severity bands evaluated against the peak pressure in ppm.
struct SeverityBands {
  std::uint32_t low_ppm = 900000;
  std::uint32_t moderate_ppm = 950000;
  std::uint32_t high_ppm = 990000;
  std::uint32_t critical_ppm = 1100000;  // offered load above capacity

  Severity classify(std::uint32_t peak_pressure_ppm) const noexcept {
    if (peak_pressure_ppm >= critical_ppm) return Severity::Critical;
    if (peak_pressure_ppm >= high_ppm) return Severity::High;
    if (peak_pressure_ppm >= moderate_ppm) return Severity::Moderate;
    if (peak_pressure_ppm >= low_ppm) return Severity::Low;
    return Severity::None;
  }
};

class PolicySnapshot {
 public:
  PolicySnapshot() = default;

  PolicyId id() const noexcept { return id_; }
  void set_id(PolicyId id) noexcept { id_ = id; }

  PolicyGeneration generation() const noexcept { return generation_; }
  void set_generation(PolicyGeneration generation) noexcept { generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  Millis issued_at() const noexcept { return issued_at_; }
  void set_issued_at(Millis value) noexcept { issued_at_ = value; }

  std::uint32_t ttl_ms() const noexcept { return ttl_ms_; }
  void set_ttl_ms(std::uint32_t value) noexcept { ttl_ms_ = value; }

  bool expired_at(Millis now) const noexcept {
    if (ttl_ms_ == 0) return true;  // a zero TTL policy is never live
    if (issued_at_ == kNoTime) return true;
    if (now < issued_at_) return true;  // clock moved backwards: refuse
    return elapsed_ms(issued_at_, now) > static_cast<Millis>(ttl_ms_);
  }

  Thresholds& thresholds() noexcept { return thresholds_; }
  const Thresholds& thresholds() const noexcept { return thresholds_; }

  MitigationBudget& budget() noexcept { return budget_; }
  const MitigationBudget& budget() const noexcept { return budget_; }

  SeverityBands& bands() noexcept { return bands_; }
  const SeverityBands& bands() const noexcept { return bands_; }

  // Deterministic digest of the decision-relevant contents.
  Digest content_digest() const noexcept;

 private:
  PolicyId id_{};
  PolicyGeneration generation_{};
  Provenance provenance_{};
  Millis issued_at_ = kNoTime;
  std::uint32_t ttl_ms_ = 0;
  Thresholds thresholds_{};
  MitigationBudget budget_{};
  SeverityBands bands_{};
};

}  // namespace hgm
