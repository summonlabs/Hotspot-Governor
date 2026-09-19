// Localized saturation detection, classification, attribution and severity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hgm/authority.hpp"
#include "hgm/capacity.hpp"
#include "hgm/enums.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/paths.hpp"
#include "hgm/policy.hpp"
#include "hgm/provenance.hpp"
#include "hgm/signals.hpp"
#include "hgm/time.hpp"
#include "hgm/topology.hpp"
#include "hgm/traffic.hpp"

namespace hgm {

// Tracks how long each resource has been *continuously* saturated. A resource
// that stops being saturated loses its history immediately: persistence can
// never be accumulated from a broken series, and stale contributors can never
// hold hotspot authority open indefinitely.
class SaturationTracker {
 public:
  struct Entry {
    Millis first_saturated = kNoTime;
    Millis last_saturated = kNoTime;
    std::uint32_t samples = 0;
  };

  // Carries history forward for a resource that is saturated in this cycle.
  Entry carry(ResourceId id, Millis now) const;

  // Replaces the whole history with the resources saturated in this cycle.
  void replace(std::vector<std::pair<ResourceId, Entry>> next);

  const Entry* find(ResourceId id) const;
  std::size_t size() const noexcept { return entries_.size(); }
  void clear();
  Millis persistence_ms(ResourceId id, Millis now) const;

  const std::vector<std::pair<ResourceId, Entry>>& entries() const noexcept { return entries_; }

 private:
  std::vector<std::pair<ResourceId, Entry>> entries_;  // sorted by ResourceId
};

struct Contributor {
  FlowId flow{};
  PathId path{};
  std::uint64_t demand_bps = 0;
  std::uint32_t share_ppm = 0;
  bool fresh = false;
  Millis observed_at = kNoTime;
};

enum class RemediationState : std::uint8_t {
  Unknown = 0,
  Eligible = 1,
  Suppressed = 2,
  NotApplicable = 3,
};

std::string_view to_string(RemediationState state) noexcept;

// Per-resource pressure derived strictly from capacity + signal evidence.
struct ResourcePressure {
  ResourceId resource{};
  RegionId region{};
  std::uint64_t capacity_units = 0;
  std::uint64_t utilized_units = 0;
  std::uint64_t queue_depth_units = 0;
  std::uint64_t queue_limit_units = 0;
  std::uint64_t buffer_used_bytes = 0;
  std::uint64_t buffer_limit_bytes = 0;
  std::uint64_t drop_units = 0;
  std::uint32_t utilization_ppm = 0;
  std::uint32_t queue_ppm = 0;
  std::uint32_t buffer_ppm = 0;
  std::uint32_t pressure_ppm = 0;
  std::uint32_t confidence_ppm = 0;
  TelemetryQuality quality = TelemetryQuality::Unknown;
  Millis observed_at = kNoTime;
  bool saturated = false;
  bool admissible = false;
  bool contradictory = false;
  bool stale = false;
  bool attribution_degraded = false;
};

struct Hotspot {
  HotspotId id{};
  std::string key;  // bounded stable key derived from the hotspot contents
  CongestionScope scope = CongestionScope::Unknown;
  SaturationCause cause = SaturationCause::Unknown;
  Severity severity = Severity::None;

  std::vector<ResourceId> saturated_resources;  // capped at kMaxHotspotResources
  std::size_t saturated_count = 0;
  bool saturated_truncated = false;

  std::vector<ResourceId> healthy_neighbors;
  std::size_t healthy_neighbor_count = 0;
  bool healthy_neighbors_truncated = false;

  std::vector<RegionId> regions;
  std::vector<Contributor> contributors;
  std::size_t contributor_count = 0;
  bool contributors_truncated = false;

  std::uint64_t saturated_capacity_units = 0;
  std::uint32_t share_ppm = 0;
  std::uint32_t peak_pressure_ppm = 0;
  std::uint32_t peak_utilization_ppm = 0;
  std::uint32_t peak_queue_ppm = 0;
  std::uint32_t peak_buffer_ppm = 0;
  std::uint32_t attribution_confidence_ppm = 0;
  std::uint64_t attributed_demand_bps = 0;
  std::uint64_t unattributed_demand_bps = 0;

  Millis persistence_ms = 0;
  std::uint32_t sample_count = 0;
  Millis first_observed = kNoTime;
  Millis last_observed = kNoTime;
  bool persistent = false;

  RemediationState remediation = RemediationState::Unknown;
  std::string suppression_reason;

  EvidenceBinding binding{};
  AuthorityVector authority;

  friend bool operator==(const Hotspot& a, const Hotspot& b) noexcept { return a.id == b.id; }
  friend bool operator!=(const Hotspot& a, const Hotspot& b) noexcept { return a.id != b.id; }
};

struct SuppressedAction {
  MitigationKind kind = MitigationKind::None;
  HotspotId hotspot{};
  std::string reason;
};

struct Diagnostic {
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
};

// The complete, explainable answer to the core question for one evaluation.
struct FabricAssessment {
  CongestionScope scope = CongestionScope::Unknown;
  std::string scope_reason;

  std::vector<Hotspot> hotspots;   // confirmed: persistent and authoritative
  std::vector<Hotspot> candidates; // observed but not yet persistent
  std::vector<ResourcePressure> pressures;  // saturated / contradictory entries
  std::vector<SuppressedAction> suppressed;
  std::vector<Diagnostic> diagnostics;
  AuthorityVector authority;
  EvidenceBinding binding{};

  std::uint64_t fabric_capacity_units = 0;
  std::uint64_t covered_capacity_units = 0;
  std::uint64_t admissible_capacity_units = 0;
  std::uint64_t saturated_capacity_units = 0;
  std::uint32_t coverage_ppm = 0;            // resources with any fresh signal
  std::uint32_t admissible_coverage_ppm = 0; // resources whose evidence may be believed
  std::uint32_t capacity_coverage_ppm = 0;   // resources with a usable capacity entry
  std::uint32_t global_share_ppm = 0;

  std::size_t resource_count = 0;
  std::size_t observed_count = 0;
  std::size_t admissible_count = 0;
  std::size_t saturated_count = 0;
  std::size_t contradictory_count = 0;
  std::size_t stale_signal_count = 0;
  std::size_t unknown_capacity_count = 0;

  bool localization_proven = false;
  bool escalation_required = false;
  std::string escalation_reason;

  const Hotspot* find_hotspot(HotspotId id) const;
};

struct DecisionInput {
  const TopologySnapshot* topology = nullptr;
  const CapacitySnapshot* capacity = nullptr;
  const SignalSnapshot* signals = nullptr;
  const PathSnapshot* paths = nullptr;
  const TrafficSnapshot* traffic = nullptr;
  const PolicySnapshot* policy = nullptr;
  Millis now = kNoTime;
  CoordinatorEpoch epoch{};
  BootId boot{};
  SaturationTracker* tracker = nullptr;

  // When false (the default) detection re-verifies that every piece of
  // evidence names only resources the bound topology generation defines. The
  // governor may set this once it has performed exactly that check at
  // admission time against the same snapshots; detection still refuses any
  // evidence whose generation binding does not match the live topology.
  bool references_preverified = false;
};

// True when every reference in the snapshot exists in the topology. Both sides
// are sorted by resource id, so this is a linear merge rather than a lookup per
// entry.
bool references_resolve(const TopologySnapshot& topology, const CapacitySnapshot& capacity);
bool references_resolve(const TopologySnapshot& topology, const SignalSnapshot& signals);
bool references_resolve(const TopologySnapshot& topology, const PathSnapshot& paths);

// Runs detection over one consistent set of generation-bound snapshots.
// Returns an UNKNOWN assessment (never a positive one) whenever evidence is
// missing, stale, contradictory or too sparse to localize.
FabricAssessment detect(const DecisionInput& input);

}  // namespace hgm
