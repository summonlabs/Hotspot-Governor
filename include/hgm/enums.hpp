// Closed enumerations shared across the model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace hgm {

// What kind of fabric resource a topology entry describes. The governor does
// not interpret vendor semantics; it only needs to know grouping and adjacency.
enum class ResourceKind : std::uint8_t {
  Unknown = 0,
  Switch = 1,
  Port = 2,
  Link = 3,
  Queue = 4,
  Buffer = 5,
  Host = 6,
  Optical = 7,
  Other = 255,
};

// Quality of the telemetry that produced a signal record.
enum class TelemetryQuality : std::uint8_t {
  Unknown = 0,   // publisher did not state quality
  Missing = 1,   // collector known to have no data
  Suspect = 2,   // collector flagged the sample as untrustworthy
  Degraded = 3,  // partial/coarse sample, admissible with reduced weight
  Healthy = 4,   // full-fidelity sample
};

// Severity band of a confirmed hotspot.
enum class Severity : std::uint8_t {
  None = 0,
  Low = 1,
  Moderate = 2,
  High = 3,
  Critical = 4,
};

// How far congestion extends.
enum class CongestionScope : std::uint8_t {
  Unknown = 0,    // inadmissible evidence: localization cannot be asserted
  None = 1,       // evidence is sufficient and nothing is saturated
  Localized = 2,  // confined to one region and a bounded share of the fabric
  Regional = 3,   // spans more than one region but is not fabric-wide
  Global = 4,     // locality has broken: saturation is fabric-wide
};

// Why a resource set is saturated. Derived from the evidence, never assumed.
enum class SaturationCause : std::uint8_t {
  Unknown = 0,
  CapacityExhaustion = 1,   // offered load met or exceeded capacity
  QueueBuildup = 2,         // queue depth pressure with headroom on capacity
  BufferExhaustion = 3,     // buffer occupancy pressure with drops observed
  Microburst = 4,           // short, non-persistent excursion
  FanInContention = 5,      // one or few contributors dominate the resource
  StructuralConvergence = 6 // many independent paths converge on the region
};

// Bounded mitigation intent kinds. The runtime expresses intent; it never
// enforces placement, rates or queueing.
enum class MitigationKind : std::uint8_t {
  None = 0,
  FlowRelocation = 1,             // move a contributing flow to another path
  PathRebalance = 2,              // rebalance across equal-cost paths
  AdmissionReduction = 3,         // reduce admission within the hotspot scope
  RatePacingChange = 4,           // request a bounded rate/pacing delta
  ResourceIsolation = 5,          // drain or isolate a degraded resource
  EscalateGlobalCongestion = 6,   // locality broke: hand off to global owner
};

std::string_view to_string(ResourceKind kind) noexcept;
std::string_view to_string(TelemetryQuality quality) noexcept;
std::string_view to_string(Severity severity) noexcept;
std::string_view to_string(CongestionScope scope) noexcept;
std::string_view to_string(SaturationCause cause) noexcept;
std::string_view to_string(MitigationKind kind) noexcept;

}  // namespace hgm
