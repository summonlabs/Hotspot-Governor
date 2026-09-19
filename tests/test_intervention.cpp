// Bounded mitigation intent, suppression and explanation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "harness.hpp"
#include "hgm/explanation.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::Harness;

namespace {

SyntheticConfig localized_config() {
  SyntheticConfig config;
  config.region_count = 4;
  config.resources_per_region = 8;
  config.hotspot_count = 1;
  config.path_fan_in = 4;
  return config;
}

Decision evaluate_with_policy(Harness& harness, const PolicySnapshot& policy);

Decision evaluate_signals(Harness& harness, const SignalSnapshot& signals,
                          const PolicySnapshot* policy_override);

Decision evaluate_with_policy(Harness& harness, const PolicySnapshot& policy) {
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  return harness.engine.decide(input);
}

Decision evaluate_signals(Harness& harness, const SignalSnapshot& signals,
                          const PolicySnapshot* policy_override) {
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &signals;
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  input.policy = policy_override != nullptr ? policy_override : &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  return harness.engine.decide(input);
}

}  // namespace

HGM_TEST(intervention, eligible_hotspot_yields_one_bounded_intent) {
  Harness harness(localized_config());
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
  const MitigationIntent& intent = decision.plan.intents.front();
  HGM_CHECK(intent.id.valid());
  HGM_CHECK(intent.hotspot.valid());
  HGM_CHECK(intent.affected_resources.size() <= Limits::kMaxIntentAffectedResources);
  HGM_CHECK(intent.affected_paths.size() <= Limits::kMaxIntentAffectedPaths);
  HGM_CHECK(intent.affected_flows.size() <= Limits::kMaxIntentAffectedFlows);
  HGM_CHECK(intent.scope_share_ppm <= 250000);
  HGM_CHECK(!intent.rationale.empty());
  HGM_CHECK_EQ(intent.binding.topology_generation, decision.assessment.binding.topology_generation);
  // Every affected resource belongs to the hotspot or to its healthy neighbours.
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  for (const ResourceId id : intent.affected_resources) {
    const bool in_hotspot = std::find(hotspot.saturated_resources.begin(),
                                      hotspot.saturated_resources.end(),
                                      id) != hotspot.saturated_resources.end();
    const bool in_neighbours = std::find(hotspot.healthy_neighbors.begin(),
                                         hotspot.healthy_neighbors.end(),
                                         id) != hotspot.healthy_neighbors.end();
    HGM_CHECK(in_hotspot || in_neighbours);
  }
  HGM_CHECK(decision.plan.plan_scope_share_ppm <= 400000);
}

HGM_TEST(intervention, policy_can_disable_every_kind) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().allow_flow_relocation = false;
  policy.budget().allow_path_rebalance = false;
  policy.budget().allow_admission_reduction = false;
  policy.budget().allow_rate_change = false;
  policy.budget().allow_resource_isolation = false;
  const Decision decision = evaluate_with_policy(harness, policy);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
  HGM_CHECK(!decision.plan.suppressed.empty());
  HGM_CHECK(decision.plan.rejected_intent_count > 0);
}

HGM_TEST(intervention, per_intent_budget_suppresses_oversized_scopes) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().max_affected_resources = 0;
  const Decision decision = evaluate_with_policy(harness, policy);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
  bool saw = false;
  for (const SuppressedAction& action : decision.plan.suppressed) {
    if (action.reason.find("per-intent budget") != std::string::npos) saw = true;
  }
  HGM_CHECK(saw);
}

HGM_TEST(intervention, rate_delta_is_bounded_by_policy) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().allow_flow_relocation = false;
  policy.budget().allow_path_rebalance = false;
  policy.budget().max_rate_delta_ppm = 1000;
  const Decision decision = evaluate_with_policy(harness, policy);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
  const MitigationIntent& intent = decision.plan.intents.front();
  HGM_CHECK_EQ(intent.kind, MitigationKind::RatePacingChange);
  HGM_CHECK(intent.rate_delta_ppm <= 0);
  HGM_CHECK(intent.rate_delta_bounded_ppm <= 1000);
}

HGM_TEST(intervention, isolation_requires_drop_evidence) {
  SyntheticConfig config = localized_config();
  // Drive the cause to capacity exhaustion, which is the only cause whose
  // candidate list contains resource isolation.
  config.saturated_utilization_ppm = 1100000;
  Harness harness(config);
  harness.run(3);

  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().allow_flow_relocation = false;
  policy.budget().allow_path_rebalance = false;
  policy.budget().allow_rate_change = false;
  policy.budget().allow_admission_reduction = false;

  const Decision positive = evaluate_with_policy(harness, policy);
  HGM_CHECK_EQ(positive.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(positive.assessment.hotspots.front().cause, SaturationCause::CapacityExhaustion);
  HGM_CHECK_EQ(positive.plan.intents.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(positive.plan.intents.front().kind, MitigationKind::ResourceIsolation);

  // Now remove every drop from the evidence: isolation must be refused and the
  // refusal must be recorded.
  SignalSnapshot no_drops;
  for (const ResourceSignals& record : harness.fabric.signals().records()) {
    ResourceSignals& entry = no_drops.add(record.resource);
    entry = record;
    entry.drop_units = 0;
  }
  no_drops.set_generation(EvidenceGeneration::from(harness.fabric.signals().generation().value() + 1));
  no_drops.set_topology_generation(harness.fabric.signals().topology_generation());
  no_drops.set_capacity_generation(harness.fabric.signals().capacity_generation());
  no_drops.set_window(harness.fabric.signals().window_start(), harness.fabric.signals().window_end());
  HGM_CHECK(no_drops.build().ok());

  const Decision negative = evaluate_signals(harness, no_drops, &policy);
  HGM_CHECK_EQ(negative.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(negative.plan.intents.size(), static_cast<std::size_t>(0));
  bool saw = false;
  for (const SuppressedAction& action : negative.plan.suppressed) {
    if (action.reason.find("drop evidence") != std::string::npos) saw = true;
  }
  HGM_CHECK(saw);
}

HGM_TEST(intervention, global_scope_emits_only_escalation) {
  SyntheticConfig config = localized_config();
  config.global_congestion = true;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.plan.intents.front().kind, MitigationKind::EscalateGlobalCongestion);
  HGM_CHECK(decision.plan.intents.front().escalation);
  HGM_CHECK_EQ(decision.plan.intents.front().scope_share_ppm, 0u);
  HGM_CHECK(decision.plan.escalation_required);
}

HGM_TEST(intervention, escalation_can_be_disabled_by_policy) {
  SyntheticConfig config = localized_config();
  config.global_congestion = true;
  Harness harness(config);
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().allow_global_escalation = false;
  const Decision decision = evaluate_with_policy(harness, policy);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
}

HGM_TEST(intervention, plan_is_deterministic_for_identical_evidence) {
  Harness first(localized_config());
  Harness second(localized_config());
  const Decision a = first.run(3);
  const Decision b = second.run(3);
  HGM_CHECK_EQ(a.plan.intents.size(), b.plan.intents.size());
  if (!a.plan.intents.empty()) {
    HGM_CHECK_EQ(a.plan.intents.front().id, b.plan.intents.front().id);
    HGM_CHECK_EQ(a.plan.intents.front().kind, b.plan.intents.front().kind);
  }
}

HGM_TEST(intervention, explanation_reports_scope_evidence_and_authority) {
  Harness harness(localized_config());
  const Decision decision = harness.run(3);
  HGM_CHECK(!decision.explanation.empty());
  const std::string& text = decision.explanation;
  HGM_CHECK(text.find("verdict: localized") != std::string::npos);
  HGM_CHECK(text.find("binding: topology=") != std::string::npos);
  HGM_CHECK(text.find("healthy_neighbours(") != std::string::npos);
  HGM_CHECK(text.find("contributors(") != std::string::npos);
  HGM_CHECK(text.find("remediation=eligible") != std::string::npos);
  HGM_CHECK(text.find("authority:") != std::string::npos);
  HGM_CHECK(text.find("coverage:") != std::string::npos);
  HGM_CHECK(text.find("escalation:") != std::string::npos);
  HGM_CHECK(text.size() <= 16384);
}

HGM_TEST(intervention, explanation_is_bounded_for_large_populations) {
  SyntheticConfig config;
  config.region_count = 16;
  config.resources_per_region = 64;
  config.hotspot_count = 16;
  config.path_fan_in = 8;
  Harness harness(config);
  const Decision decision = harness.run(3);
  ExplanationOptions options;
  options.max_lines = 32;
  options.max_bytes = 1024;
  const std::string text = explain(decision.assessment, decision.plan, options);
  HGM_CHECK(text.size() <= 1024);
  const std::size_t lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
  HGM_CHECK(lines <= options.max_lines);
  HGM_CHECK(text.find("truncated") != std::string::npos);
}

HGM_TEST(intervention, suppression_is_recorded_not_silent) {
  Harness harness(localized_config());
  const Decision decision = harness.run(3);
  HGM_CHECK(!decision.plan.suppressed.empty());
  for (const SuppressedAction& action : decision.plan.suppressed) {
    HGM_CHECK(!action.reason.empty());
    HGM_CHECK(action.reason.size() <= Limits::kMaxReasonBytes);
  }
}
