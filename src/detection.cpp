// Localized saturation detection.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/detection.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hgm/checked.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

// Compact per-resource state. Kept small so that fabric-scale topologies stay
// within a bounded memory envelope.
struct Cell {
  std::uint32_t utilization_ppm = 0;
  std::uint32_t queue_ppm = 0;
  std::uint32_t buffer_ppm = 0;
  std::uint32_t pressure_ppm = 0;
  std::uint8_t flags = 0;
};

enum CellFlag : std::uint8_t {
  kCovered = 1u << 0,
  kAdmissible = 1u << 1,
  kSaturated = 1u << 2,
  kContradictory = 1u << 3,
  kStale = 1u << 4,
};

std::size_t index_of_resource(const TopologySnapshot& topology, ResourceId id) noexcept {
  const std::vector<Resource>& resources = topology.resources();
  const auto it = std::lower_bound(resources.begin(), resources.end(), id,
                                   [](const Resource& entry, ResourceId key) { return entry.id < key; });
  if (it == resources.end() || it->id != id) return static_cast<std::size_t>(-1);
  return static_cast<std::size_t>(it - resources.begin());
}

std::string generation_binding(const DecisionInput& input) {
  std::string out;
  out.reserve(64);
  out.append("t");
  out.append(std::to_string(input.topology != nullptr ? input.topology->generation().value() : 0));
  out.append("/c");
  out.append(std::to_string(input.capacity != nullptr ? input.capacity->generation().value() : 0));
  out.append("/s");
  out.append(std::to_string(input.signals != nullptr ? input.signals->generation().value() : 0));
  out.append("/p");
  out.append(std::to_string(input.paths != nullptr ? input.paths->generation().value() : 0));
  out.append("/r");
  out.append(std::to_string(input.traffic != nullptr ? input.traffic->generation().value() : 0));
  out.append("/y");
  out.append(std::to_string(input.policy != nullptr ? input.policy->generation().value() : 0));
  return out;
}

struct Accumulator {
  FlowId flow{};
  PathId path{};  // the path carrying the most fresh demand for this flow
  std::uint64_t fresh_bps = 0;
  std::uint64_t stale_bps = 0;
  std::uint64_t best_path_bps = 0;
  Millis observed_at = kNoTime;
};

}  // namespace

std::string_view to_string(RemediationState state) noexcept {
  switch (state) {
    case RemediationState::Unknown: return "unknown";
    case RemediationState::Eligible: return "eligible";
    case RemediationState::Suppressed: return "suppressed";
    case RemediationState::NotApplicable: return "not-applicable";
  }
  return "unknown";
}

// --- SaturationTracker ------------------------------------------------------

SaturationTracker::Entry SaturationTracker::carry(ResourceId id, Millis now) const {
  static_cast<void>(now);
  const Entry* entry = find(id);
  if (entry == nullptr) return Entry{};
  if (entry->last_saturated == kNoTime) return Entry{};
  return *entry;
}

void SaturationTracker::replace(std::vector<std::pair<ResourceId, Entry>> next) {
  std::sort(next.begin(), next.end(),
            [](const std::pair<ResourceId, Entry>& a, const std::pair<ResourceId, Entry>& b) {
              return a.first < b.first;
            });
  next.erase(std::unique(next.begin(), next.end(),
                         [](const std::pair<ResourceId, Entry>& a,
                            const std::pair<ResourceId, Entry>& b) { return a.first == b.first; }),
             next.end());
  entries_.swap(next);
}

const SaturationTracker::Entry* SaturationTracker::find(ResourceId id) const {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                                   [](const std::pair<ResourceId, Entry>& entry, ResourceId key) {
                                     return entry.first < key;
                                   });
  if (it == entries_.end() || it->first != id) return nullptr;
  return &it->second;
}

void SaturationTracker::clear() { entries_.clear(); }

Millis SaturationTracker::persistence_ms(ResourceId id, Millis now) const {
  const Entry* entry = find(id);
  if (entry == nullptr) return 0;
  return elapsed_ms(entry->first_saturated, now);
}

const Hotspot* FabricAssessment::find_hotspot(HotspotId id) const {
  for (const Hotspot& hotspot : hotspots) {
    if (hotspot.id == id) return &hotspot;
  }
  return nullptr;
}

bool references_resolve(const TopologySnapshot& topology, const CapacitySnapshot& capacity) {
  const std::vector<Resource>& resources = topology.resources();
  std::size_t cursor = 0;
  for (const ResourceCapacity& entry : capacity.entries()) {
    while (cursor < resources.size() && resources[cursor].id < entry.resource) ++cursor;
    if (cursor == resources.size() || resources[cursor].id != entry.resource) return false;
  }
  return true;
}

bool references_resolve(const TopologySnapshot& topology, const SignalSnapshot& signals) {
  const std::vector<Resource>& resources = topology.resources();
  std::size_t cursor = 0;
  for (const ResourceSignals& record : signals.records()) {
    while (cursor < resources.size() && resources[cursor].id < record.resource) ++cursor;
    if (cursor == resources.size() || resources[cursor].id != record.resource) return false;
  }
  return true;
}

bool references_resolve(const TopologySnapshot& topology, const PathSnapshot& paths) {
  for (const PathRecord& path : paths.paths()) {
    for (const ResourceId hop : path.hops) {
      if (!topology.has_resource(hop)) return false;
    }
  }
  return true;
}

// --- detection --------------------------------------------------------------

FabricAssessment detect(const DecisionInput& input) {
  FabricAssessment result;
  const Millis now = input.now;

  struct AuthoritySink {
    AuthorityVector* target = nullptr;
    void grant(AuthorityDomain domain, std::string reason, std::string binding = std::string()) {
      target->grant(domain, std::move(reason), std::move(binding));
    }
    void deny(AuthorityDomain domain, std::string reason, std::string binding = std::string()) {
      target->deny(domain, std::move(reason), std::move(binding));
    }
  };
  AuthoritySink auth{&result.authority};
  const auto diagnose = [&result](ErrorCode code, std::string detail) {
    if (result.diagnostics.size() < Limits::kMaxDiagnostics) {
      result.diagnostics.push_back(Diagnostic{code, std::move(detail)});
    }
  };
  const auto reject = [&result, &input](CongestionScope scope, std::string reason) {
    result.scope = scope;
    result.scope_reason = std::move(reason);
    result.localization_proven = false;
    // An evaluation that cannot admit the evidence cannot have observed
    // continuous saturation. The series is broken rather than carried forward,
    // so a later recovery has to re-earn the persistence window.
    if (input.tracker != nullptr) input.tracker->replace({});
    result.authority.seal();
    return result;
  };

  // --- generation binding recorded regardless of the outcome ----------------
  if (input.topology != nullptr) {
    result.binding.topology = input.topology->id();
    result.binding.topology_generation = input.topology->generation();
  }
  if (input.capacity != nullptr) {
    result.binding.capacity = input.capacity->id();
    result.binding.capacity_generation = input.capacity->generation();
  }
  if (input.signals != nullptr) {
    result.binding.signals = input.signals->id();
    result.binding.signals_generation = input.signals->generation();
  }
  if (input.paths != nullptr) {
    result.binding.paths = input.paths->id();
    result.binding.path_generation = input.paths->generation();
  }
  if (input.traffic != nullptr) {
    result.binding.traffic = input.traffic->id();
    result.binding.traffic_generation = input.traffic->generation();
  }
  if (input.policy != nullptr) {
    result.binding.policy = input.policy->id();
    result.binding.policy_generation = input.policy->generation();
  }

  const std::string bindings = generation_binding(input);

  // --- coordinator / boot authority -----------------------------------------
  if (input.epoch.valid()) {
    auth.grant(AuthorityDomain::CoordinatorEpoch, "epoch installed", "epoch=" + input.epoch.hex());
  } else {
    auth.deny(AuthorityDomain::CoordinatorEpoch, "no coordinator epoch installed");
  }
  if (input.boot.valid()) {
    auth.grant(AuthorityDomain::BootIncarnation, "boot incarnation installed", "boot=" + input.boot.hex());
  } else {
    auth.deny(AuthorityDomain::BootIncarnation, "no boot incarnation installed");
  }

  // --- policy ---------------------------------------------------------------
  Thresholds thresholds{};
  MitigationBudget budget{};
  SeverityBands bands{};
  bool policy_live = false;
  if (input.policy == nullptr) {
    auth.deny(AuthorityDomain::Policy, "policy evidence missing");
    diagnose(ErrorCode::PolicyMissing, "no policy supplied; thresholds defaulted, mitigation unauthorised");
  } else {
    const PolicySnapshot& policy = *input.policy;
    thresholds = policy.thresholds();
    budget = policy.budget();
    bands = policy.bands();
    if (policy.expired_at(now)) {
      auth.deny(AuthorityDomain::Policy, "policy expired",
           "gen=" + std::to_string(policy.generation().value()));
      diagnose(ErrorCode::PolicyExpired, "policy ttl elapsed; mitigation unauthorised");
    } else {
      policy_live = true;
      auth.grant(AuthorityDomain::Policy, "policy live", "gen=" + std::to_string(policy.generation().value()));
    }
  }

  // --- topology -------------------------------------------------------------
  if (input.topology == nullptr || !input.topology->built()) {
    auth.deny(AuthorityDomain::Topology, "topology evidence missing");
    diagnose(ErrorCode::MissingEvidence, "no built topology snapshot supplied");
    return reject(CongestionScope::Unknown, "topology evidence missing");
  }
  const TopologySnapshot& topology = *input.topology;
  if (!topology.fresh_at(now)) {
    auth.deny(AuthorityDomain::Topology, "topology validity window elapsed");
    diagnose(ErrorCode::StaleTopology, "topology snapshot is past its validity window");
    return reject(CongestionScope::Unknown, "stale topology invalidates localization");
  }
  auth.grant(AuthorityDomain::Topology, "topology fresh",
        "gen=" + std::to_string(topology.generation().value()) + " id=" + topology.id().hex());

  // --- capacity -------------------------------------------------------------
  bool capacity_ok = false;
  if (input.capacity == nullptr || !input.capacity->built()) {
    auth.deny(AuthorityDomain::Capacity, "capacity evidence missing");
    diagnose(ErrorCode::MissingEvidence, "no built capacity snapshot supplied");
  } else if (input.capacity->topology_generation() != topology.generation()) {
    auth.deny(AuthorityDomain::Capacity, "capacity does not bind the live topology generation");
    diagnose(ErrorCode::CapacityGenerationMismatch, "capacity snapshot names a different topology generation");
  } else if (age_ms(input.capacity->observed_at(), now) > static_cast<Millis>(thresholds.max_capacity_age_ms)) {
    auth.deny(AuthorityDomain::Capacity, "capacity snapshot is stale");
    diagnose(ErrorCode::StaleEvidence, "capacity snapshot is older than the policy permits");
  } else {
    capacity_ok = true;
    auth.grant(AuthorityDomain::Capacity, "capacity bound to topology generation",
          "gen=" + std::to_string(input.capacity->generation().value()));
  }

  // --- signals --------------------------------------------------------------
  bool signals_ok = false;
  if (!capacity_ok) {
    auth.deny(AuthorityDomain::Signals, "capacity authority unavailable");
  } else if (input.signals == nullptr || !input.signals->built()) {
    auth.deny(AuthorityDomain::Signals, "signal evidence missing");
    diagnose(ErrorCode::MissingEvidence, "no built signal snapshot supplied");
  } else if (input.signals->topology_generation() != topology.generation()) {
    auth.deny(AuthorityDomain::Signals, "signals do not bind the live topology generation");
    diagnose(ErrorCode::TopologyGenerationMismatch, "signal snapshot names a different topology generation");
  } else if (input.signals->capacity_generation() != input.capacity->generation()) {
    auth.deny(AuthorityDomain::Signals, "signals do not bind the live capacity generation");
    diagnose(ErrorCode::CapacityGenerationMismatch, "signal snapshot names a different capacity generation");
  } else {
    signals_ok = true;
    auth.grant(AuthorityDomain::Signals, "signals bound to topology and capacity generations",
          "gen=" + std::to_string(input.signals->generation().value()));
  }

  // --- paths and traffic (optional; absence only removes attribution) -------
  bool paths_ok = false;
  if (input.paths == nullptr || !input.paths->built()) {
    auth.deny(AuthorityDomain::Paths, "path evidence missing");
    diagnose(ErrorCode::MissingEvidence, "no path snapshot supplied; attribution unavailable");
  } else if (input.paths->topology_generation() != topology.generation()) {
    auth.deny(AuthorityDomain::Paths, "paths do not bind the live topology generation");
    diagnose(ErrorCode::PathGenerationMismatch, "path snapshot names a different topology generation");
  } else if (age_ms(input.paths->observed_at(), now) > static_cast<Millis>(thresholds.max_path_age_ms)) {
    auth.deny(AuthorityDomain::Paths, "path evidence is stale");
    diagnose(ErrorCode::StaleEvidence, "path snapshot is older than the policy permits");
  } else {
    paths_ok = true;
    auth.grant(AuthorityDomain::Paths, "paths bound to topology generation",
          "gen=" + std::to_string(input.paths->generation().value()));
  }

  bool traffic_ok = false;
  if (!paths_ok) {
    auth.deny(AuthorityDomain::Traffic, "path authority unavailable");
  } else if (input.traffic == nullptr || !input.traffic->built()) {
    auth.deny(AuthorityDomain::Traffic, "traffic evidence missing");
    diagnose(ErrorCode::MissingEvidence, "no traffic snapshot supplied; attribution unavailable");
  } else if (input.traffic->path_generation() != input.paths->generation()) {
    auth.deny(AuthorityDomain::Traffic, "traffic does not bind the live path generation");
    diagnose(ErrorCode::TrafficGenerationMismatch, "traffic snapshot names a different path generation");
  } else {
    traffic_ok = true;
    auth.grant(AuthorityDomain::Traffic, "traffic bound to path generation",
          "gen=" + std::to_string(input.traffic->generation().value()));
  }

  if (!capacity_ok || !signals_ok) {
    return reject(CongestionScope::Unknown, "capacity or signal authority unavailable: congestion is not known");
  }

  const CapacitySnapshot& capacity = *input.capacity;
  const SignalSnapshot& signals = *input.signals;
  const std::vector<Resource>& resources = topology.resources();
  const std::size_t n = resources.size();
  result.resource_count = n;

  if (n == 0) {
    return reject(CongestionScope::None, "topology declares no resources");
  }

  // Every piece of evidence declares the exact topology generation it was
  // produced against. Something that names a resource outside that generation
  // contradicts its own binding and cannot be used to localize anything. A
  // caller that already ran this at admission may skip the repeated walk.
  if (!input.references_preverified) {
    if (!references_resolve(topology, capacity)) {
      auth.deny(AuthorityDomain::Capacity, "capacity names a resource outside the bound topology");
      diagnose(ErrorCode::UnknownResource, "capacity names a resource the topology does not define");
      return reject(CongestionScope::Unknown,
                    "capacity evidence contradicts its topology generation binding");
    }
    if (!references_resolve(topology, signals)) {
      auth.deny(AuthorityDomain::Signals, "signals name a resource outside the bound topology");
      diagnose(ErrorCode::UnknownResource, "signals name a resource the topology does not define");
      return reject(CongestionScope::Unknown,
                    "signal evidence contradicts its topology generation binding");
    }
    if (paths_ok && !references_resolve(topology, *input.paths)) {
      auth.deny(AuthorityDomain::Paths, "a path traverses a resource outside the bound topology");
      diagnose(ErrorCode::UnknownResource,
               "a path traverses a resource the topology does not define");
      return reject(CongestionScope::Unknown,
                    "path evidence contradicts its topology generation binding");
    }
  }

  std::vector<Cell> cells(n);
  std::uint64_t fabric_capacity = 0;
  std::uint64_t covered_capacity = 0;
  std::uint64_t admissible_capacity = 0;
  std::uint64_t saturated_capacity = 0;
  std::size_t observed_count = 0;
  std::size_t admissible_count = 0;
  std::size_t saturated_count = 0;
  std::size_t contradictory_count = 0;
  std::size_t stale_count = 0;
  std::size_t unusable_count = 0;

  for (std::size_t i = 0; i < n; ++i) {
    const Resource& resource = resources[i];
    const ResourceCapacity* entry = capacity.find(resource.id);
    if (entry == nullptr || !entry->usable || entry->capacity_units == 0) {
      ++unusable_count;
      continue;
    }
    fabric_capacity = sat_add(fabric_capacity, entry->capacity_units);

    const ResourceSignals* sample = signals.find(resource.id);
    if (sample == nullptr) continue;

    Cell& cell = cells[i];
    if (age_ms(sample->observed_at, now) > static_cast<Millis>(thresholds.max_signal_age_ms)) {
      cell.flags |= kStale;
      ++stale_count;
      continue;
    }
    if (sample->quality == TelemetryQuality::Missing) {
      cell.flags |= kStale;
      ++stale_count;
      continue;
    }

    cell.flags |= kCovered;
    covered_capacity = sat_add(covered_capacity, entry->capacity_units);
    ++observed_count;

    cell.utilization_ppm = ratio_ppm(sample->utilized_units, entry->capacity_units).value_or(0u);
    cell.queue_ppm =
        entry->queue_limit_units == 0
            ? 0u
            : ratio_ppm(sample->queue_depth_units, entry->queue_limit_units).value_or(0u);
    cell.buffer_ppm =
        entry->buffer_limit_bytes == 0
            ? 0u
            : ratio_ppm(sample->buffer_used_bytes, entry->buffer_limit_bytes).value_or(0u);
    cell.pressure_ppm = std::max(cell.utilization_ppm, std::max(cell.queue_ppm, cell.buffer_ppm));

    const bool queue_pressured =
        entry->queue_limit_units != 0 && cell.queue_ppm >= thresholds.queue_pressure_ppm;
    const bool buffer_pressured =
        entry->buffer_limit_bytes != 0 && cell.buffer_ppm >= thresholds.buffer_pressure_ppm;

    const bool saturated = cell.utilization_ppm >= thresholds.saturation_utilization_ppm ||
                           queue_pressured || buffer_pressured;

    // Contradictory telemetry: occupancy pressure with no load behind it, or
    // offered load with nothing observed on the resource. Neither can support
    // localization, so the sample is excluded rather than believed.
    const bool contradiction =
        (cell.utilization_ppm < thresholds.contradiction_utilization_ppm &&
         (queue_pressured || buffer_pressured)) ||
        (sample->offered_bps > 0 && sample->utilized_units == 0 &&
         sample->queue_depth_units == 0 && sample->drop_units == 0);

    const bool quality_ok = sample->quality == TelemetryQuality::Healthy ||
                            sample->quality == TelemetryQuality::Degraded;
    const bool confidence_ok = sample->confidence_ppm >= thresholds.min_confidence_ppm;

    if (contradiction) {
      cell.flags |= kContradictory;
      ++contradictory_count;
    }

    const bool admissible = quality_ok && confidence_ok && !contradiction;

    if (admissible) {
      cell.flags |= kAdmissible;
      admissible_capacity = sat_add(admissible_capacity, entry->capacity_units);
      ++admissible_count;
      if (saturated) {
        cell.flags |= kSaturated;
        saturated_capacity = sat_add(saturated_capacity, entry->capacity_units);
        ++saturated_count;
      }
    }

    if ((saturated && admissible) || contradiction || !admissible) {
      if (result.pressures.size() < Limits::kMaxPressureEntries) {
        ResourcePressure pressure;
        pressure.resource = resource.id;
        pressure.region = resource.region;
        pressure.capacity_units = entry->capacity_units;
        pressure.utilized_units = sample->utilized_units;
        pressure.queue_depth_units = sample->queue_depth_units;
        pressure.queue_limit_units = entry->queue_limit_units;
        pressure.buffer_used_bytes = sample->buffer_used_bytes;
        pressure.buffer_limit_bytes = entry->buffer_limit_bytes;
        pressure.drop_units = sample->drop_units;
        pressure.utilization_ppm = cell.utilization_ppm;
        pressure.queue_ppm = cell.queue_ppm;
        pressure.buffer_ppm = cell.buffer_ppm;
        pressure.pressure_ppm = cell.pressure_ppm;
        pressure.confidence_ppm = sample->confidence_ppm;
        pressure.quality = sample->quality;
        pressure.observed_at = sample->observed_at;
        pressure.saturated = saturated && admissible;
        pressure.admissible = admissible;
        pressure.contradictory = contradiction;
        pressure.stale = false;
        result.pressures.push_back(std::move(pressure));
      }
    }
  }

  result.fabric_capacity_units = fabric_capacity;
  result.covered_capacity_units = covered_capacity;
  result.admissible_capacity_units = admissible_capacity;
  result.saturated_capacity_units = saturated_capacity;
  result.observed_count = observed_count;
  result.admissible_count = admissible_count;
  result.saturated_count = saturated_count;
  result.contradictory_count = contradictory_count;
  result.stale_signal_count = stale_count;
  result.unknown_capacity_count = unusable_count;
  result.coverage_ppm = ratio_ppm(covered_capacity, fabric_capacity).value_or(0u);
  result.admissible_coverage_ppm = ratio_ppm(admissible_capacity, fabric_capacity).value_or(0u);
  result.capacity_coverage_ppm =
      ratio_ppm(static_cast<std::uint64_t>(n - unusable_count), static_cast<std::uint64_t>(n))
          .value_or(0u);
  result.global_share_ppm = ratio_ppm(saturated_capacity, fabric_capacity).value_or(0u);

  if (fabric_capacity == 0) {
    auth.deny(AuthorityDomain::Signals, "no usable capacity evidence");
    diagnose(ErrorCode::InsufficientCoverage, "no resource carries usable capacity evidence");
    return reject(CongestionScope::Unknown, "no usable capacity evidence: saturation is unknown");
  }
  if (result.capacity_coverage_ppm < thresholds.min_coverage_ppm) {
    auth.deny(AuthorityDomain::Capacity, "capacity coverage below the policy floor");
    diagnose(ErrorCode::InsufficientCoverage,
             "capacity known for " + std::to_string(result.capacity_coverage_ppm) +
                 " ppm of resources, below floor " + std::to_string(thresholds.min_coverage_ppm));
    return reject(CongestionScope::Unknown,
                  "capacity is unknown for too much of the fabric: saturation cannot be bounded");
  }
  // Coverage is measured on *believable* evidence: a fabric full of suspect or
  // stale samples is UNKNOWN, never "no congestion".
  if (result.admissible_coverage_ppm < thresholds.min_coverage_ppm) {
    auth.deny(AuthorityDomain::Signals, "admissible telemetry coverage below the policy floor");
    diagnose(ErrorCode::InsufficientCoverage,
             "admissible capacity " + std::to_string(result.admissible_coverage_ppm) +
                 " ppm below floor " + std::to_string(thresholds.min_coverage_ppm) + " ppm");
    return reject(CongestionScope::Unknown,
                  "admissible telemetry covers too little of the fabric: an unobserved region could "
                  "be saturated");
  }

  // --- saturation history ----------------------------------------------------
  const bool persistence_tracked = input.tracker != nullptr;
  if (!persistence_tracked) {
    auth.deny(AuthorityDomain::Persistence, "no saturation tracker available");
    diagnose(ErrorCode::MissingEvidence,
             "detection ran without a saturation tracker; nothing can be confirmed as persistent");
  }

  std::vector<std::pair<ResourceId, SaturationTracker::Entry>> next_history;
  next_history.reserve(saturated_count);

  // --- connected components over the saturated set ---------------------------
  std::vector<std::uint8_t> visited(n, 0u);
  std::vector<std::size_t> stack;
  std::vector<std::size_t> component;
  std::size_t truncated_hotspots = 0;

  for (std::size_t seed = 0; seed < n; ++seed) {
    if ((cells[seed].flags & kSaturated) == 0 || visited[seed] != 0) continue;

    component.clear();
    stack.clear();
    stack.push_back(seed);
    visited[seed] = 1u;
    while (!stack.empty()) {
      const std::size_t current = stack.back();
      stack.pop_back();
      component.push_back(current);
      for (const ResourceId neighbour : topology.neighbors(resources[current].id)) {
        const std::size_t index = index_of_resource(topology, neighbour);
        if (index == static_cast<std::size_t>(-1)) continue;
        if ((cells[index].flags & kSaturated) == 0) continue;
        if (visited[index] != 0) continue;
        visited[index] = 1u;
        stack.push_back(index);
      }
    }
    std::sort(component.begin(), component.end());

    Hotspot hotspot;
    hotspot.saturated_count = component.size();
    hotspot.saturated_resources.reserve(std::min(component.size(), Limits::kMaxHotspotResources));
    for (const std::size_t index : component) {
      if (hotspot.saturated_resources.size() >= Limits::kMaxHotspotResources) {
        hotspot.saturated_truncated = true;
        break;
      }
      hotspot.saturated_resources.push_back(resources[index].id);
    }

    std::uint64_t component_capacity = 0;
    Millis latest_first = kNoTime;
    std::uint32_t min_samples = 0;
    bool have_history = false;

    for (const std::size_t index : component) {
      const ResourceCapacity* entry = capacity.find(resources[index].id);
      if (entry != nullptr) component_capacity = sat_add(component_capacity, entry->capacity_units);

      const Cell& cell = cells[index];
      hotspot.peak_pressure_ppm = std::max(hotspot.peak_pressure_ppm, cell.pressure_ppm);
      hotspot.peak_utilization_ppm = std::max(hotspot.peak_utilization_ppm, cell.utilization_ppm);
      hotspot.peak_queue_ppm = std::max(hotspot.peak_queue_ppm, cell.queue_ppm);
      hotspot.peak_buffer_ppm = std::max(hotspot.peak_buffer_ppm, cell.buffer_ppm);

      SaturationTracker::Entry entry_state;
      if (persistence_tracked) {
        const SaturationTracker::Entry previous = input.tracker->carry(resources[index].id, now);
        if (previous.samples == 0) {
          entry_state.first_saturated = now;
          entry_state.samples = 1;
        } else {
          entry_state.first_saturated = previous.first_saturated;
          entry_state.samples = previous.samples == UINT32_MAX ? UINT32_MAX : previous.samples + 1;
        }
        entry_state.last_saturated = now;
      } else {
        entry_state.first_saturated = now;
        entry_state.last_saturated = now;
        entry_state.samples = 1;
      }
      next_history.emplace_back(resources[index].id, entry_state);

      if (!have_history) {
        latest_first = entry_state.first_saturated;
        min_samples = entry_state.samples;
        have_history = true;
      } else {
        if (entry_state.first_saturated > latest_first) latest_first = entry_state.first_saturated;
        if (entry_state.samples < min_samples) min_samples = entry_state.samples;
      }
    }

    hotspot.saturated_capacity_units = component_capacity;
    hotspot.share_ppm = ratio_ppm(component_capacity, fabric_capacity).value_or(0u);
    hotspot.persistence_ms = elapsed_ms(latest_first, now);
    hotspot.sample_count = min_samples;
    hotspot.first_observed = latest_first;
    hotspot.last_observed = now;

    // --- healthy neighbours: compared only on admissible evidence -----------
    std::vector<ResourceId> healthy;
    for (const std::size_t index : component) {
      for (const ResourceId neighbour : topology.neighbors(resources[index].id)) {
        const std::size_t neighbour_index = index_of_resource(topology, neighbour);
        if (neighbour_index == static_cast<std::size_t>(-1)) continue;
        const std::uint8_t flags = cells[neighbour_index].flags;
        if ((flags & kAdmissible) == 0) continue;  // no evidence: a neighbour is never implicated
        if ((flags & kSaturated) != 0) continue;   // saturated neighbours are not "healthy"
        healthy.push_back(neighbour);
      }
    }
    std::sort(healthy.begin(), healthy.end());
    healthy.erase(std::unique(healthy.begin(), healthy.end()), healthy.end());
    hotspot.healthy_neighbor_count = healthy.size();
    if (healthy.size() > Limits::kMaxHotspotResources) {
      hotspot.healthy_neighbors_truncated = true;
      healthy.resize(Limits::kMaxHotspotResources);
    }
    hotspot.healthy_neighbors = std::move(healthy);
    const bool comparison_available = hotspot.healthy_neighbor_count > 0;

    // --- regions -------------------------------------------------------------
    std::vector<RegionId> regions;
    regions.reserve(std::min(component.size(), Limits::kMaxRegions));
    for (const std::size_t index : component) {
      if (regions.size() >= Limits::kMaxRegions) break;
      regions.push_back(resources[index].region);
    }
    std::sort(regions.begin(), regions.end());
    regions.erase(std::unique(regions.begin(), regions.end()), regions.end());
    hotspot.regions = std::move(regions);

    // --- classification ------------------------------------------------------
    const bool all_admissible_saturated = admissible_count != 0 && saturated_count == admissible_count;
    if (hotspot.share_ppm >= thresholds.global_share_ppm) {
      hotspot.scope = CongestionScope::Global;
    } else if (hotspot.regions.size() > thresholds.max_regional_regions) {
      hotspot.scope = CongestionScope::Global;
    } else if (all_admissible_saturated) {
      hotspot.scope = CongestionScope::Global;
    } else if (hotspot.regions.size() > thresholds.max_local_regions) {
      hotspot.scope = CongestionScope::Regional;
    } else if (!comparison_available) {
      hotspot.scope = CongestionScope::Unknown;
    } else {
      hotspot.scope = CongestionScope::Localized;
    }

    // --- cause ---------------------------------------------------------------
    if (hotspot.peak_utilization_ppm >= kPpmScale) {
      hotspot.cause = SaturationCause::CapacityExhaustion;
    } else if (hotspot.peak_queue_ppm >= thresholds.queue_pressure_ppm) {
      hotspot.cause = SaturationCause::QueueBuildup;
    } else if (hotspot.peak_buffer_ppm >= thresholds.buffer_pressure_ppm) {
      hotspot.cause = SaturationCause::BufferExhaustion;
    } else {
      hotspot.cause = SaturationCause::Unknown;
    }

    // --- attribution ---------------------------------------------------------
    // Accumulators are appended and only sorted at the end. Inserting into a
    // sorted vector here would make a fabric-wide hotspot quadratic in its
    // contributor count.
    std::vector<Accumulator> accumulators;
    std::unordered_map<std::uint64_t, std::size_t> accumulator_index;
    std::uint64_t fresh_demand = 0;
    std::uint64_t stale_demand = 0;
    std::uint32_t distinct_fresh_paths = 0;
    bool attribution_incomplete = false;

    if (paths_ok && traffic_ok) {
      const PathSnapshot& paths = *input.paths;
      const TrafficSnapshot& traffic = *input.traffic;
      const std::vector<PathRecord>& path_records = paths.paths();
      const std::vector<TrafficRecord>& traffic_records = traffic.records();

      for (const std::size_t index : component) {
        for (const std::uint32_t path_index : paths.paths_through(resources[index].id)) {
          const PathRecord& path = path_records[path_index];
          const auto begin = std::lower_bound(
              traffic_records.begin(), traffic_records.end(), path.id,
              [](const TrafficRecord& entry, PathId key) { return entry.path < key; });
          for (auto it = begin; it != traffic_records.end() && it->path == path.id; ++it) {
            const TrafficRecord& record = *it;
            const bool fresh = age_ms(record.observed_at, now) <=
                                   static_cast<Millis>(thresholds.max_traffic_age_ms) &&
                               (record.quality == TelemetryQuality::Healthy ||
                                record.quality == TelemetryQuality::Degraded);
            std::size_t slot = 0;
            const auto found = accumulator_index.find(record.flow.value());
            if (found == accumulator_index.end()) {
              if (accumulators.size() >= Limits::kMaxContributorAccumulators) {
                attribution_incomplete = true;
                continue;
              }
              Accumulator created;
              created.flow = record.flow;
              created.path = path.id;
              slot = accumulators.size();
              accumulator_index.emplace(record.flow.value(), slot);
              accumulators.push_back(created);
            } else {
              slot = found->second;
            }
            Accumulator& accumulator = accumulators[slot];
            if (fresh) {
              accumulator.fresh_bps = sat_add(accumulator.fresh_bps, record.demand_bps);
              accumulator.observed_at = std::max(accumulator.observed_at, record.observed_at);
              if (record.demand_bps > accumulator.best_path_bps) {
                accumulator.best_path_bps = record.demand_bps;
                accumulator.path = path.id;
              }
              fresh_demand = sat_add(fresh_demand, record.demand_bps);
              ++distinct_fresh_paths;
            } else {
              accumulator.stale_bps = sat_add(accumulator.stale_bps, record.demand_bps);
              stale_demand = sat_add(stale_demand, record.demand_bps);
            }
          }
        }
      }
    }

    const std::uint64_t total_demand = sat_add(fresh_demand, stale_demand);
    hotspot.attributed_demand_bps = fresh_demand;
    hotspot.unattributed_demand_bps = stale_demand;
    hotspot.attribution_confidence_ppm =
        (total_demand == 0) ? 0u : ratio_ppm(fresh_demand, total_demand).value_or(0u);
    if (attribution_incomplete) hotspot.attribution_confidence_ppm = 0u;

    std::sort(accumulators.begin(), accumulators.end(), [](const Accumulator& a, const Accumulator& b) {
      if (a.fresh_bps != b.fresh_bps) return a.fresh_bps > b.fresh_bps;
      if (a.flow != b.flow) return a.flow < b.flow;
      return a.path < b.path;
    });
    hotspot.contributor_count = 0;
    for (const Accumulator& accumulator : accumulators) {
      if (accumulator.fresh_bps == 0) continue;
      ++hotspot.contributor_count;
      if (hotspot.contributors.size() >= Limits::kMaxContributors) {
        hotspot.contributors_truncated = true;
        continue;
      }
      Contributor contributor;
      contributor.flow = accumulator.flow;
      contributor.path = accumulator.path;
      contributor.demand_bps = accumulator.fresh_bps;
      contributor.share_ppm = ratio_ppm(accumulator.fresh_bps, fresh_demand).value_or(0u);
      contributor.fresh = true;
      contributor.observed_at = accumulator.observed_at;
      hotspot.contributors.push_back(contributor);
    }
    std::sort(hotspot.contributors.begin(), hotspot.contributors.end(),
              [](const Contributor& a, const Contributor& b) {
                if (a.share_ppm != b.share_ppm) return a.share_ppm > b.share_ppm;
                if (a.flow != b.flow) return a.flow < b.flow;
                return a.path < b.path;
              });

    const bool attribution_usable =
        paths_ok && traffic_ok && !attribution_incomplete &&
        hotspot.attribution_confidence_ppm >= thresholds.min_attribution_confidence_ppm;

    if (attribution_usable && !hotspot.contributors.empty() &&
        hotspot.contributors.front().share_ppm >= thresholds.dominant_contributor_ppm) {
      hotspot.cause = SaturationCause::FanInContention;
    } else if (attribution_usable && distinct_fresh_paths >= 8 &&
               hotspot.cause == SaturationCause::CapacityExhaustion) {
      hotspot.cause = SaturationCause::StructuralConvergence;
    }

    if (attribution_usable) {
      auth.grant(AuthorityDomain::Attribution, "fresh contributor evidence",
            "share=" + std::to_string(hotspot.contributors.empty()
                                          ? 0u
                                          : hotspot.contributors.front().share_ppm));
    } else {
      auth.deny(AuthorityDomain::Attribution,
           attribution_incomplete ? "contributor population exceeded the bound"
                                  : "contributor evidence missing, stale or below the confidence floor");
    }

    // --- persistence ---------------------------------------------------------
    hotspot.persistent =
        persistence_tracked && hotspot.sample_count >= thresholds.min_persistence_samples &&
        hotspot.persistence_ms >= static_cast<Millis>(thresholds.persistence_window_ms);
    if (hotspot.persistent) {
      auth.grant(AuthorityDomain::Persistence, "persistence window met",
            "ms=" + std::to_string(hotspot.persistence_ms) + " samples=" +
                std::to_string(hotspot.sample_count));
    } else {
      auth.deny(AuthorityDomain::Persistence, "persistence window not met",
           "ms=" + std::to_string(hotspot.persistence_ms) + " samples=" +
               std::to_string(hotspot.sample_count));
    }

    hotspot.severity = bands.classify(hotspot.peak_pressure_ppm);
    if (hotspot.persistent) {
      const auto quadruple = static_cast<Millis>(thresholds.persistence_window_ms) *
                             static_cast<Millis>(4);
      if (hotspot.persistence_ms >= quadruple &&
          static_cast<std::uint8_t>(hotspot.severity) <
              static_cast<std::uint8_t>(Severity::Critical)) {
        hotspot.severity = static_cast<Severity>(static_cast<std::uint8_t>(hotspot.severity) + 1u);
      }
    }

    // --- remediation eligibility --------------------------------------------
    if (!policy_live) {
      hotspot.remediation = RemediationState::Suppressed;
      hotspot.suppression_reason = "no live policy: mitigation is not authorised";
    } else if (!hotspot.persistent) {
      hotspot.remediation = RemediationState::NotApplicable;
      hotspot.suppression_reason = "persistence window not met: reported as a candidate only";
    } else if (hotspot.scope == CongestionScope::Unknown) {
      hotspot.remediation = RemediationState::Suppressed;
      hotspot.suppression_reason = "scope is unknown: localized mitigation is not authorised";
    } else if (hotspot.scope == CongestionScope::Global) {
      hotspot.remediation = RemediationState::Suppressed;
      hotspot.suppression_reason = "locality has broken: escalation, not local mitigation, is required";
    } else if (hotspot.scope == CongestionScope::Regional &&
               hotspot.share_ppm > budget.max_scope_share_ppm) {
      hotspot.remediation = RemediationState::Suppressed;
      hotspot.suppression_reason = "regional scope exceeds the per-intent mitigation budget";
    } else {
      hotspot.remediation = RemediationState::Eligible;
      hotspot.suppression_reason.clear();
    }

    // --- identity ------------------------------------------------------------
    std::string canonical;
    canonical.reserve(512);
    canonical.append("hg1|");
    canonical.append(bindings);
    canonical.append("|s");
    canonical.append(std::to_string(static_cast<unsigned>(hotspot.scope)));
    canonical.append("|c");
    canonical.append(std::to_string(static_cast<unsigned>(hotspot.cause)));
    canonical.append("|n");
    canonical.append(std::to_string(hotspot.saturated_count));
    canonical.append("|");
    for (std::size_t k = 0; k < hotspot.saturated_resources.size() && k < 32; ++k) {
      canonical.append(hex64(hotspot.saturated_resources[k].value()));
      canonical.push_back(',');
    }
    canonical.append("|m");
    canonical.append(hex64(hotspot.saturated_resources.empty()
                               ? 0ull
                               : hotspot.saturated_resources.back().value()));
    hotspot.id = HotspotId::from(identity_from_bytes(canonical));
    hotspot.key = "hs-" + hotspot.id.hex();

    hotspot.binding = result.binding;

    if (hotspot.persistent) {
      if (result.hotspots.size() < Limits::kMaxHotspots) {
        result.hotspots.push_back(std::move(hotspot));
      } else {
        ++truncated_hotspots;
      }
    } else {
      if (result.candidates.size() < Limits::kMaxHotspots) {
        result.candidates.push_back(std::move(hotspot));
      } else {
        ++truncated_hotspots;
      }
    }
  }

  if (persistence_tracked) {
    if (next_history.size() > Limits::kMaxHistoryResources) {
      diagnose(ErrorCode::BoundExceeded, "saturation history exceeded its bound and was truncated");
      next_history.resize(Limits::kMaxHistoryResources);
    }
    input.tracker->replace(std::move(next_history));
  }

  if (truncated_hotspots > 0) {
    diagnose(ErrorCode::BoundExceeded,
             "hotspot population exceeded the reporting bound; " +
                 std::to_string(truncated_hotspots) + " entries not reported");
  }

  // --- fabric verdict --------------------------------------------------------
  if (result.hotspots.empty()) {
    result.scope = CongestionScope::None;
    result.scope_reason = result.candidates.empty()
                              ? "no resource is saturated in the admissible evidence"
                              : "saturation observed but not yet persistent: no confirmed hotspot";
  } else {
    CongestionScope widest = CongestionScope::None;
    for (const Hotspot& hotspot : result.hotspots) {
      if (static_cast<std::uint8_t>(hotspot.scope) > static_cast<std::uint8_t>(widest)) {
        widest = hotspot.scope;
      }
    }
    result.scope = widest;
    std::size_t localized = 0;
    std::size_t global = 0;
    for (const Hotspot& hotspot : result.hotspots) {
      if (hotspot.scope == CongestionScope::Localized) ++localized;
      if (hotspot.scope == CongestionScope::Global) ++global;
    }
    if (global > 0) {
      result.scope_reason = "locality has broken: " + std::to_string(global) +
                            " hotspot(s) span the fabric";
    } else if (localized > 1) {
      result.scope_reason = std::to_string(localized) +
                            " simultaneous localized hotspots, no fabric-wide saturation";
    } else {
      result.scope_reason = "congestion is confined to a bounded set of resources";
    }
    result.localization_proven = localized > 0;
  }

  // --- escalation semantics --------------------------------------------------
  {
    std::uint64_t hotspot_share = 0;
    for (const Hotspot& hotspot : result.hotspots) {
      hotspot_share = sat_add(hotspot_share, static_cast<std::uint64_t>(hotspot.share_ppm));
    }
    if (result.scope == CongestionScope::Global) {
      result.escalation_required = true;
      result.escalation_reason = "a hotspot spans the fabric: global congestion ownership applies";
    } else if (result.global_share_ppm >= thresholds.global_share_ppm && !result.hotspots.empty()) {
      result.escalation_required = true;
      result.escalation_reason = "aggregate saturated capacity share exceeds the global threshold";
    } else if (result.hotspots.size() > budget.max_intents_per_plan &&
               hotspot_share >= budget.max_plan_scope_share_ppm) {
      result.escalation_required = true;
      result.escalation_reason =
          "simultaneous hotspot scope exceeds the whole-plan mitigation budget";
    }
  }

  if (result.escalation_required) {
    auth.deny(AuthorityDomain::MitigationBudget, "local mitigation cannot cover the observed scope");
  } else {
    auth.grant(AuthorityDomain::MitigationBudget, "observed scope fits the mitigation budget");
  }

  result.authority.seal();
  for (Hotspot& hotspot : result.hotspots) hotspot.authority = result.authority;
  for (Hotspot& candidate : result.candidates) candidate.authority = result.authority;
  return result;
}

}  // namespace hgm
