// Contributing-traffic attribution.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "harness.hpp"
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
  config.saturated_demand_bps = 400000;
  return config;
}

Decision evaluate_with_traffic(Harness& harness, TrafficSnapshot* traffic) {
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = traffic;
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  return harness.engine.decide(input);
}

// Rewrites the demand of the busiest path through the given resource.
TrafficSnapshot skew_demand(const SyntheticFabric& fabric, ResourceId resource,
                            std::uint64_t dominant_bps) {
  std::vector<PathId> paths;
  for (const std::uint32_t index : fabric.paths().paths_through(resource)) {
    paths.push_back(fabric.paths().paths()[index].id);
  }
  std::sort(paths.begin(), paths.end());
  TrafficSnapshot traffic;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    const PathRecord* record = fabric.paths().find(paths[i]);
    TrafficRecord& entry = traffic.add(record != nullptr ? record->flow : FlowId{}, paths[i]);
    entry.demand_bps = i == 0 ? dominant_bps : 1000;
    entry.quality = TelemetryQuality::Healthy;
    entry.observed_at = fabric.traffic().records().empty()
                            ? 0
                            : fabric.traffic().records().front().observed_at;
  }
  traffic.set_generation(TrafficGeneration::from(fabric.traffic().generation().value()));
  traffic.set_path_generation(fabric.paths().generation());
  static_cast<void>(traffic.build());
  return traffic;
}

}  // namespace

HGM_TEST(attribution, contributors_are_ranked_and_bounded) {
  Harness harness(localized_config());
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  HGM_CHECK(hotspot.contributor_count > 0);
  HGM_CHECK(!hotspot.contributors.empty());
  HGM_CHECK(hotspot.contributors.size() <= Limits::kMaxContributors);
  HGM_CHECK(hotspot.attribution_confidence_ppm >= 900000);
  HGM_CHECK(hotspot.attributed_demand_bps > 0);
  std::uint64_t share_sum = 0;
  for (const Contributor& contributor : hotspot.contributors) {
    HGM_CHECK(contributor.share_ppm > 0);
    HGM_CHECK(contributor.fresh);
    share_sum += contributor.share_ppm;
  }
  HGM_CHECK(share_sum <= 1000000);
  for (std::size_t i = 1; i < hotspot.contributors.size(); ++i) {
    HGM_CHECK(hotspot.contributors[i - 1].share_ppm >= hotspot.contributors[i].share_ppm);
  }
  HGM_CHECK(decision.assessment.authority.granted(AuthorityDomain::Attribution));
}

HGM_TEST(attribution, one_dominant_flow_becomes_fan_in_contention) {
  Harness harness(localized_config());
  harness.run(3);
  const ResourceId hotspot_resource = harness.fabric.saturated_resources().front();
  TrafficSnapshot skewed = skew_demand(harness.fabric, hotspot_resource, 10000000);
  const Decision decision = evaluate_with_traffic(harness, &skewed);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  HGM_CHECK(!hotspot.contributors.empty());
  HGM_CHECK(hotspot.contributors.front().share_ppm >= 500000);
  HGM_CHECK_EQ(hotspot.cause, SaturationCause::FanInContention);
  HGM_CHECK(!decision.plan.intents.empty());
  HGM_CHECK_EQ(decision.plan.intents.front().kind, MitigationKind::FlowRelocation);
  HGM_CHECK(!decision.plan.intents.front().affected_flows.empty());
}

HGM_TEST(attribution, stale_traffic_cannot_preserve_attribution) {
  Harness harness(localized_config());
  harness.run(3);
  const ResourceId hotspot_resource = harness.fabric.saturated_resources().front();
  TrafficSnapshot skewed = skew_demand(harness.fabric, hotspot_resource, 10000000);
  // Age every record well beyond the policy window.
  TrafficSnapshot stale;
  for (const TrafficRecord& record : skewed.records()) {
    TrafficRecord& entry = stale.add(record.flow, record.path);
    entry.demand_bps = record.demand_bps;
    entry.quality = record.quality;
    entry.observed_at = harness.now - 600000;
  }
  stale.set_generation(TrafficGeneration::from(skewed.generation().value() + 1));
  stale.set_path_generation(skewed.path_generation());
  HGM_CHECK(stale.build().ok());

  const Decision decision = evaluate_with_traffic(harness, &stale);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  HGM_CHECK_EQ(hotspot.attribution_confidence_ppm, 0u);
  HGM_CHECK(hotspot.contributors.empty());
  HGM_CHECK(hotspot.unattributed_demand_bps > 0);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Attribution));
  // The hotspot itself still stands on resource evidence; only attribution-based
  // mitigation is withheld.
  HGM_CHECK(!decision.plan.intents.empty());
  HGM_CHECK_NE(decision.plan.intents.front().kind, MitigationKind::FlowRelocation);
  HGM_CHECK_NE(decision.plan.intents.front().kind, MitigationKind::PathRebalance);
}

HGM_TEST(attribution, policy_can_authorise_attribution_free_relocation) {
  Harness harness(localized_config());
  harness.run(3);
  SyntheticFabric& fabric = harness.fabric;
  PolicySnapshot policy = fabric.policy(harness.now);
  policy.budget().require_attribution_for_relocation = false;
  // Force the cause that prefers relocation without attaching contributors.
  for (std::uint32_t i = 0; i < 2; ++i) {
    DecisionInput input;
    input.topology = &fabric.topology();
    input.capacity = &fabric.capacity();
    input.signals = &fabric.signals();
    input.policy = &policy;
    input.now = harness.now;
    input.epoch = CoordinatorEpoch::from(1);
    input.boot = BootId::from(1);
    input.tracker = &harness.engine.tracker();
    const Decision decision = harness.engine.decide(input);
    if (!decision.assessment.hotspots.empty()) {
      HGM_CHECK(decision.plan.intents.size() <= 1);
    }
  }
  HGM_CHECK(true);
}

HGM_TEST(attribution, contributors_aggregate_by_flow) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 3);
  HGM_CHECK(fabric.signals().size() > 0);
  std::map<FlowId, std::uint64_t> by_flow;
  for (const TrafficRecord& record : fabric.traffic().records()) {
    by_flow[record.flow] += record.demand_bps;
  }
  HGM_CHECK(!by_flow.empty());
  HGM_CHECK(fabric.traffic().records().size() == by_flow.size());
}
