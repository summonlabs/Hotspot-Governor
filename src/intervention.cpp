// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/intervention.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "hgm/checked.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

struct IntentDraft {
  MitigationKind kind = MitigationKind::None;
  std::vector<ResourceId> resources;
  std::vector<PathId> paths;
  std::vector<FlowId> flows;
  std::int64_t rate_delta_ppm = 0;
  std::string rationale;
};

void note_suppression(InterventionPlan& plan, MitigationKind kind, HotspotId hotspot,
                      const std::string& reason) {
  if (plan.suppressed.size() >= Limits::kMaxSuppressedEntries) return;
  for (const SuppressedAction& existing : plan.suppressed) {
    if (existing.kind == kind && existing.hotspot == hotspot) return;
  }
  SuppressedAction action;
  action.kind = kind;
  action.hotspot = hotspot;
  action.reason = reason.substr(0, Limits::kMaxReasonBytes);
  plan.suppressed.push_back(std::move(action));
}

std::uint32_t share_of(const CapacitySnapshot* capacity, const std::vector<ResourceId>& resources,
                       std::uint64_t fabric_capacity) {
  if (capacity == nullptr || fabric_capacity == 0) return 0;
  std::uint64_t total = 0;
  for (const ResourceId id : resources) {
    const ResourceCapacity* entry = capacity->find(id);
    if (entry != nullptr) total = sat_add(total, entry->capacity_units);
  }
  return ratio_ppm(total, fabric_capacity).value_or(0u);
}

std::vector<ResourceId> sorted_unique(std::vector<ResourceId> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

}  // namespace

const MitigationIntent* InterventionPlan::find(InterventionId id) const {
  for (const MitigationIntent& intent : intents) {
    if (intent.id == id) return &intent;
  }
  return nullptr;
}

InterventionPlan plan_interventions(const FabricAssessment& assessment,
                                    const TopologySnapshot* topology,
                                    const CapacitySnapshot* capacity,
                                    const PolicySnapshot* policy, Millis now) {
  InterventionPlan plan;
  plan.binding = assessment.binding;
  plan.escalation_required = assessment.escalation_required;
  plan.escalation_reason = assessment.escalation_reason;
  plan.authority = assessment.authority;

  // The affected scope is validated against the exact topology generation that
  // the assessment was bound to; a topology the planner cannot see is not a
  // topology it may act on.
  if (topology == nullptr || !topology->built()) {
    plan.authority.deny(AuthorityDomain::Topology, "topology unavailable: the affected scope cannot be validated");
    plan.authority.seal();
    return plan;
  }
  if (assessment.binding.topology.valid() && assessment.binding.topology != topology->id()) {
    plan.authority.deny(AuthorityDomain::Topology,
                        "assessment was bound to a different topology generation");
    plan.authority.seal();
    return plan;
  }

  const bool policy_live = policy != nullptr && !policy->expired_at(now);
  if (!policy_live) {
    plan.authority.deny(AuthorityDomain::Policy, "no live policy: no mitigation may be authorised");
    plan.authority.seal();
    return plan;
  }
  const MitigationBudget& budget = policy->budget();
  const Thresholds& thresholds = policy->thresholds();

  // --- global escalation short-circuits local mitigation ---------------------
  if (assessment.escalation_required) {
    for (const Hotspot& hotspot : assessment.hotspots) {
      if (hotspot.remediation == RemediationState::Eligible) {
        note_suppression(plan, MitigationKind::AdmissionReduction, hotspot.id,
                         "locality broke: local mitigation is withheld in favour of escalation");
      }
    }
    if (budget.allow_global_escalation) {
      MitigationIntent intent;
      intent.kind = MitigationKind::EscalateGlobalCongestion;
      intent.escalation = true;
      intent.hotspot = assessment.hotspots.empty() ? HotspotId{} : assessment.hotspots.front().id;
      intent.escalation_hotspot = intent.hotspot;
      intent.hotspot_scope = assessment.scope;
      intent.severity = Severity::Critical;
      intent.cause = SaturationCause::Unknown;
      intent.binding = assessment.binding;
      intent.policy_provenance = policy->provenance();
      intent.rationale = assessment.escalation_reason.empty()
                             ? std::string("locality has broken; escalating to global congestion ownership")
                             : assessment.escalation_reason;
      const std::string canonical = "iv1|esc|" + assessment.binding.topology.hex() + "|" +
                                    std::to_string(assessment.binding.topology_generation.value()) +
                                    "|" + std::to_string(assessment.binding.signals_generation.value());
      intent.id = InterventionId::from(identity_from_bytes(canonical));
      intent.authority = plan.authority;
      intent.authority.grant(AuthorityDomain::MitigationBudget, "escalation is not a local action");
      intent.authority.seal();
      plan.intents.push_back(std::move(intent));
    } else {
      note_suppression(plan, MitigationKind::EscalateGlobalCongestion, HotspotId{},
                       "global escalation is disabled by policy");
    }
    plan.authority.seal();
    return plan;
  }

  const std::uint64_t fabric_capacity = assessment.fabric_capacity_units;

  for (const Hotspot& hotspot : assessment.hotspots) {
    if (hotspot.remediation != RemediationState::Eligible) {
      note_suppression(plan, MitigationKind::AdmissionReduction, hotspot.id,
                       hotspot.suppression_reason.empty() ? "remediation not eligible"
                                                          : hotspot.suppression_reason);
      continue;
    }
    if (plan.intents.size() >= budget.max_intents_per_plan) {
      note_suppression(plan, MitigationKind::AdmissionReduction, hotspot.id,
                       "whole-plan intent budget exhausted");
      ++plan.rejected_intent_count;
      continue;
    }

    const bool attribution_usable =
        hotspot.attribution_confidence_ppm >= thresholds.min_attribution_confidence_ppm &&
        !hotspot.contributors.empty();

    // Candidate kinds in priority order for the observed cause.
    std::vector<MitigationKind> candidates;
    switch (hotspot.cause) {
      case SaturationCause::FanInContention:
        candidates = {MitigationKind::FlowRelocation, MitigationKind::PathRebalance,
                      MitigationKind::RatePacingChange, MitigationKind::AdmissionReduction};
        break;
      case SaturationCause::StructuralConvergence:
        candidates = {MitigationKind::PathRebalance, MitigationKind::AdmissionReduction,
                      MitigationKind::RatePacingChange};
        break;
      case SaturationCause::CapacityExhaustion:
        candidates = {MitigationKind::AdmissionReduction, MitigationKind::RatePacingChange,
                      MitigationKind::FlowRelocation, MitigationKind::ResourceIsolation};
        break;
      case SaturationCause::QueueBuildup:
        candidates = {MitigationKind::RatePacingChange, MitigationKind::AdmissionReduction,
                      MitigationKind::PathRebalance};
        break;
      case SaturationCause::BufferExhaustion:
        candidates = {MitigationKind::AdmissionReduction, MitigationKind::ResourceIsolation,
                      MitigationKind::RatePacingChange};
        break;
      case SaturationCause::Microburst:
      case SaturationCause::Unknown:
      default:
        candidates = {MitigationKind::AdmissionReduction};
        break;
    }

    bool emitted_for_hotspot = false;
    std::size_t emitted_here = 0;
    for (const MitigationKind kind : candidates) {
      // Exactly one bounded intent per confirmed hotspot: the highest-priority
      // kind that survives every validation. Alternatives are recorded as
      // suppressed rather than stacked.
      if (emitted_here >= 1) {
        note_suppression(plan, kind, hotspot.id,
                         "a higher-priority intent was already emitted for this hotspot");
        continue;
      }
      if (!budget.allows(kind)) {
        note_suppression(plan, kind, hotspot.id, "kind disabled by policy budget");
        continue;
      }

      IntentDraft draft;
      draft.kind = kind;
      draft.resources = hotspot.saturated_resources;
      if (kind == MitigationKind::FlowRelocation) {
        if (budget.require_attribution_for_relocation && !attribution_usable) {
          note_suppression(plan, kind, hotspot.id,
                           "attribution confidence below the floor: relocation is not authorised");
          continue;
        }
        // Relocation targets are the evidence-backed healthy neighbours.
        for (const ResourceId neighbour : hotspot.healthy_neighbors) {
          if (draft.resources.size() >= Limits::kMaxIntentAffectedResources) break;
          draft.resources.push_back(neighbour);
        }
        if (attribution_usable) {
          for (const Contributor& contributor : hotspot.contributors) {
            if (draft.flows.size() >= Limits::kMaxIntentAffectedFlows) break;
            draft.flows.push_back(contributor.flow);
            if (draft.paths.size() < Limits::kMaxIntentAffectedPaths) {
              draft.paths.push_back(contributor.path);
            }
          }
          draft.rationale = "relocate the dominant contributing flows off the saturated resource set";
        } else {
          draft.rationale =
              "relocate the saturated scope wholesale (policy does not require attribution)";
        }
      } else if (kind == MitigationKind::PathRebalance) {
        if (!attribution_usable) {
          note_suppression(plan, kind, hotspot.id,
                           "attribution confidence below the floor: rebalance is not authorised");
          continue;
        }
        for (const Contributor& contributor : hotspot.contributors) {
          if (draft.paths.size() >= Limits::kMaxIntentAffectedPaths) break;
          draft.paths.push_back(contributor.path);
          if (draft.flows.size() < Limits::kMaxIntentAffectedFlows) draft.flows.push_back(contributor.flow);
        }
        draft.rationale = "rebalance the contributing paths across their equal-cost alternatives";
      } else if (kind == MitigationKind::RatePacingChange) {
        const std::uint32_t target = thresholds.saturation_utilization_ppm;
        std::uint32_t excess = hotspot.peak_utilization_ppm > target
                                   ? hotspot.peak_utilization_ppm - target
                                   : 0u;
        if (excess == 0) {
          excess = hotspot.peak_pressure_ppm > target ? hotspot.peak_pressure_ppm - target : 0u;
        }
        if (excess == 0) {
          note_suppression(plan, kind, hotspot.id, "no measurable rate excess to reduce");
          continue;
        }
        const std::uint32_t bounded =
            std::min<std::uint32_t>(excess, budget.max_rate_delta_ppm);
        draft.rate_delta_ppm = -static_cast<std::int64_t>(bounded);
        draft.rationale = "request a bounded rate/pacing reduction at the saturated scope";
      } else if (kind == MitigationKind::ResourceIsolation) {
        bool has_drops = false;
        for (const ResourcePressure& pressure : assessment.pressures) {
          if (pressure.saturated && pressure.drop_units > 0) {
            has_drops = true;
            break;
          }
        }
        if (!has_drops) {
          note_suppression(plan, kind, hotspot.id,
                           "no drop evidence on the saturated resources: isolation is not authorised");
          continue;
        }
        draft.rationale = "isolate the degraded resource so the fabric can drain it";
      } else if (kind == MitigationKind::AdmissionReduction) {
        draft.rationale = "reduce admission within the hotspot scope only";
      }

      draft.resources = sorted_unique(std::move(draft.resources));
      std::sort(draft.paths.begin(), draft.paths.end());
      draft.paths.erase(std::unique(draft.paths.begin(), draft.paths.end()), draft.paths.end());
      std::sort(draft.flows.begin(), draft.flows.end());
      draft.flows.erase(std::unique(draft.flows.begin(), draft.flows.end()), draft.flows.end());

      if (draft.resources.size() > budget.max_affected_resources) {
        note_suppression(plan, kind, hotspot.id, "affected resource count exceeds the per-intent budget");
        continue;
      }
      if (draft.paths.size() > budget.max_affected_paths) {
        note_suppression(plan, kind, hotspot.id, "affected path count exceeds the per-intent budget");
        continue;
      }
      if (draft.flows.size() > budget.max_affected_flows) {
        note_suppression(plan, kind, hotspot.id, "affected flow count exceeds the per-intent budget");
        continue;
      }
      if (draft.resources.size() > Limits::kMaxIntentAffectedResources ||
          draft.paths.size() > Limits::kMaxIntentAffectedPaths ||
          draft.flows.size() > Limits::kMaxIntentAffectedFlows) {
        note_suppression(plan, kind, hotspot.id, "affected set exceeds the hard bound");
        continue;
      }

      bool scope_known = true;
      for (const ResourceId id : draft.resources) {
        if (!topology->has_resource(id)) {
          scope_known = false;
          break;
        }
      }
      if (!scope_known) {
        note_suppression(plan, kind, hotspot.id,
                         "affected resource is absent from the live topology generation");
        continue;
      }

      const std::uint32_t scope_share = share_of(capacity, draft.resources, fabric_capacity);
      if (scope_share > budget.max_scope_share_ppm) {
        note_suppression(plan, kind, hotspot.id,
                         "affected scope exceeds the per-intent share budget");
        continue;
      }
      std::uint64_t projected = static_cast<std::uint64_t>(plan.plan_scope_share_ppm) +
                                static_cast<std::uint64_t>(scope_share);
      if (projected > budget.max_plan_scope_share_ppm) {
        note_suppression(plan, kind, hotspot.id,
                         "affected scope would exceed the whole-plan share budget");
        ++plan.rejected_intent_count;
        continue;
      }

      MitigationIntent intent;
      intent.kind = kind;
      intent.hotspot = hotspot.id;
      intent.affected_resources = std::move(draft.resources);
      intent.affected_paths = std::move(draft.paths);
      intent.affected_flows = std::move(draft.flows);
      intent.rate_delta_ppm = draft.rate_delta_ppm;
      intent.rate_delta_bounded_ppm =
          static_cast<std::uint32_t>(intent.rate_delta_ppm < 0 ? -intent.rate_delta_ppm
                                                               : intent.rate_delta_ppm);
      intent.scope_share_ppm = scope_share;
      intent.severity = hotspot.severity;
      intent.hotspot_scope = hotspot.scope;
      intent.cause = hotspot.cause;
      intent.binding = hotspot.binding;
      intent.policy_provenance = policy->provenance();
      intent.rationale = draft.rationale;

      std::string canonical;
      canonical.reserve(256);
      canonical.append("iv1|");
      canonical.append(std::to_string(static_cast<unsigned>(kind)));
      canonical.append("|h");
      canonical.append(hotspot.id.hex());
      canonical.append("|n");
      canonical.append(std::to_string(intent.affected_resources.size()));
      canonical.append("|r");
      for (const ResourceId id : intent.affected_resources) {
        canonical.append(hex64(id.value()));
        canonical.push_back(',');
      }
      canonical.append("|d");
      canonical.append(std::to_string(intent.rate_delta_ppm));
      intent.id = InterventionId::from(identity_from_bytes(canonical));

      intent.authority = plan.authority;
      intent.authority.grant(AuthorityDomain::MitigationBudget,
                             "intent within the per-intent and whole-plan budget");
      intent.authority.seal();

      plan.plan_scope_share_ppm = static_cast<std::uint32_t>(projected);
      plan.intents.push_back(std::move(intent));
      emitted_for_hotspot = true;
      ++emitted_here;
    }

    if (!emitted_for_hotspot) {
      ++plan.rejected_intent_count;
      note_suppression(plan, MitigationKind::None, hotspot.id,
                       "no mitigation kind satisfied the policy budget for this hotspot");
    }
  }

  plan.authority.grant(AuthorityDomain::MitigationBudget,
                       plan.intents.empty() ? "no intent emitted" : "all intents within budget");
  plan.authority.seal();
  return plan;
}

}  // namespace hgm
