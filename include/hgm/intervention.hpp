// Bounded mitigation intent. The runtime expresses what is authorised; it
// never enforces placement, rates, queueing or recovery sequencing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hgm/authority.hpp"
#include "hgm/capacity.hpp"
#include "hgm/detection.hpp"
#include "hgm/ids.hpp"
#include "hgm/policy.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"
#include "hgm/topology.hpp"

namespace hgm {

struct MitigationIntent {
  InterventionId id{};
  MitigationKind kind = MitigationKind::None;
  HotspotId hotspot{};
  HotspotId escalation_hotspot{};  // set for escalation intents when known

  std::vector<ResourceId> affected_resources;
  std::vector<PathId> affected_paths;
  std::vector<FlowId> affected_flows;
  bool affected_truncated = false;

  std::int64_t rate_delta_ppm = 0;
  std::uint32_t rate_delta_bounded_ppm = 0;
  std::uint32_t scope_share_ppm = 0;
  Severity severity = Severity::None;
  CongestionScope hotspot_scope = CongestionScope::Unknown;
  SaturationCause cause = SaturationCause::Unknown;
  bool escalation = false;

  EvidenceBinding binding{};
  Provenance policy_provenance{};
  AuthorityVector authority;
  std::string rationale;

  friend bool operator==(const MitigationIntent& a, const MitigationIntent& b) noexcept {
    return a.id == b.id;
  }
};

struct InterventionPlan {
  std::vector<MitigationIntent> intents;
  std::vector<SuppressedAction> suppressed;
  AuthorityVector authority;
  EvidenceBinding binding{};

  bool escalation_required = false;
  std::string escalation_reason;
  std::uint32_t plan_scope_share_ppm = 0;
  std::size_t rejected_intent_count = 0;

  const MitigationIntent* find(InterventionId id) const;
};

// Plans bounded mitigation intent for a fabric assessment. Every intent is
// validated against the policy budget, the hotspot scope and the live
// generations before it is emitted; refusals are recorded, never silent.
InterventionPlan plan_interventions(const FabricAssessment& assessment,
                                    const TopologySnapshot* topology,
                                    const CapacitySnapshot* capacity,
                                    const PolicySnapshot* policy,
                                    Millis now);

}  // namespace hgm
