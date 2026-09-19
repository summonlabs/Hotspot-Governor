// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/engine.hpp"

#include <utility>

namespace hgm {

Decision Engine::decide(const DecisionInput& input) {
  Decision decision;
  decision.evaluated_at = input.now;

  decision.assessment = detect(input);
  decision.plan = plan_interventions(decision.assessment, input.topology, input.capacity,
                                     input.policy, input.now);
  decision.binding = decision.assessment.binding;

  if (config_.include_explanation) {
    decision.explanation = explain(decision.assessment, decision.plan, config_.explanation);
  }

  ++evaluations_;
  metrics_.count(metrics_.evaluations);
  metrics_.count(metrics_.hotspots_confirmed, decision.assessment.hotspots.size());
  metrics_.count(metrics_.candidates_observed, decision.assessment.candidates.size());
  metrics_.count(metrics_.interventions_planned, decision.plan.intents.size());
  metrics_.count(metrics_.interventions_suppressed, decision.plan.suppressed.size());
  for (const MitigationIntent& intent : decision.plan.intents) {
    if (intent.escalation) metrics_.count(metrics_.escalations);
  }
  return decision;
}

}  // namespace hgm
