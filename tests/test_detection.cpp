// Detection: localization, classification, persistence, evidence refusal.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>

#include "harness.hpp"
#include "hgm/detection.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::Harness;

namespace {

SyntheticConfig localized_config() {
  SyntheticConfig config;
  config.region_count = 4;
  config.resources_per_region = 8;
  config.hotspot_count = 1;
  config.path_fan_in = 3;
  return config;
}

Decision evaluate_with(Harness& harness, PathSnapshot* paths_override, TrafficSnapshot* traffic_override,
                       CapacitySnapshot* capacity_override, SignalSnapshot* signals_override,
                       TopologySnapshot* topology_override, PolicySnapshot* policy_override) {
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = topology_override != nullptr ? topology_override : &harness.fabric.topology();
  input.capacity = capacity_override != nullptr ? capacity_override : &harness.fabric.capacity();
  input.signals = signals_override != nullptr ? signals_override : &harness.fabric.signals();
  input.paths = paths_override != nullptr ? paths_override : &harness.fabric.paths();
  input.traffic = traffic_override != nullptr ? traffic_override : &harness.fabric.traffic();
  input.policy = policy_override != nullptr ? policy_override : &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  return harness.engine.decide(input);
}

}  // namespace

HGM_TEST(detection, localized_hotspot_is_identified) {
  Harness harness(localized_config());
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Localized);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.candidates.size(), static_cast<std::size_t>(0));
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  HGM_CHECK_EQ(hotspot.scope, CongestionScope::Localized);
  HGM_CHECK_EQ(hotspot.saturated_count, static_cast<std::size_t>(1));
  HGM_CHECK_EQ(hotspot.regions.size(), static_cast<std::size_t>(1));
  HGM_CHECK(hotspot.persistence_ms >= 2000);
  HGM_CHECK(hotspot.sample_count >= 3);
  HGM_CHECK(hotspot.persistent);
  HGM_CHECK(hotspot.share_ppm < 100000);
  HGM_CHECK(hotspot.peak_utilization_ppm >= 900000);
  HGM_CHECK(hotspot.healthy_neighbor_count > 0);
  HGM_CHECK_EQ(hotspot.remediation, RemediationState::Eligible);
  HGM_CHECK(decision.assessment.localization_proven);
  HGM_CHECK(!decision.assessment.escalation_required);
}

HGM_TEST(detection, healthy_fabric_reports_no_hotspot) {
  SyntheticConfig config = localized_config();
  config.hotspot_count = 0;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::None);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.candidates.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.saturated_count, static_cast<std::size_t>(0));
  HGM_CHECK(!decision.assessment.escalation_required);
}

HGM_TEST(detection, global_congestion_breaks_locality_and_escalates) {
  SyntheticConfig config = localized_config();
  config.global_congestion = true;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Global);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.hotspots.front().scope, CongestionScope::Global);
  HGM_CHECK_EQ(decision.assessment.hotspots.front().remediation, RemediationState::Suppressed);
  HGM_CHECK(decision.assessment.escalation_required);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.plan.intents.front().kind, MitigationKind::EscalateGlobalCongestion);
}

HGM_TEST(detection, unproven_persistence_yields_a_candidate_not_a_hotspot) {
  Harness harness(localized_config());
  const Decision first = harness.step();
  HGM_CHECK_EQ(first.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(first.assessment.candidates.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(first.assessment.scope, CongestionScope::None);
  const Hotspot& candidate = first.assessment.candidates.front();
  HGM_CHECK(!candidate.persistent);
  HGM_CHECK_EQ(candidate.remediation, RemediationState::NotApplicable);
  HGM_CHECK(candidate.sample_count >= 1);
}

HGM_TEST(detection, saturation_that_stops_resets_persistence) {
  Harness harness(localized_config());
  harness.run(3);
  HGM_CHECK_EQ(harness.engine.tracker().size(), static_cast<std::size_t>(1));
  // Break the saturation: the history entry must disappear, not decay.
  SyntheticConfig healthy = localized_config();
  healthy.hotspot_count = 0;
  harness.config = healthy;
  harness.fabric = SyntheticFabric(healthy);
  const Decision decision = harness.step();
  HGM_CHECK_EQ(harness.engine.tracker().size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::None);
}

HGM_TEST(detection, stale_topology_refuses_localization) {
  Harness harness(localized_config());
  harness.run(3);
  TopologySnapshot topology = harness.fabric.topology();
  topology.set_validity(0, harness.now - 1);
  const Decision decision = evaluate_with(harness, nullptr, nullptr, nullptr, nullptr, &topology, nullptr);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Topology));
  HGM_CHECK(!decision.assessment.localization_proven);
}

HGM_TEST(detection, capacity_generation_mismatch_refuses_localization) {
  Harness harness(localized_config());
  harness.run(3);
  CapacitySnapshot capacity = harness.fabric.capacity();
  capacity.set_topology_generation(TopologyGeneration::from(9999));
  capacity.build();
  const Decision decision = evaluate_with(harness, nullptr, nullptr, &capacity, nullptr, nullptr, nullptr);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Capacity));
}

HGM_TEST(detection, signal_topology_mismatch_refuses_localization) {
  Harness harness(localized_config());
  harness.run(3);
  SignalSnapshot signals = harness.fabric.signals();
  signals.set_topology_generation(TopologyGeneration::from(4242));
  signals.build();
  const Decision decision = evaluate_with(harness, nullptr, nullptr, nullptr, &signals, nullptr, nullptr);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Signals));
}

HGM_TEST(detection, sparse_telemetry_yields_unknown_not_none) {
  SyntheticConfig config = localized_config();
  config.evidence_density_ppm = 300000;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK(decision.assessment.admissible_coverage_ppm < 600000);
  bool saw_coverage = false;
  for (const Diagnostic& diagnostic : decision.assessment.diagnostics) {
    if (diagnostic.code == ErrorCode::InsufficientCoverage) saw_coverage = true;
  }
  HGM_CHECK(saw_coverage);
}

HGM_TEST(detection, suspect_telemetry_yields_unknown_not_none) {
  SyntheticConfig config = localized_config();
  config.quality = TelemetryQuality::Suspect;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(decision.assessment.admissible_count, static_cast<std::size_t>(0));
}

HGM_TEST(detection, stale_signals_yield_unknown) {
  SyntheticConfig config = localized_config();
  config.stale_evidence = true;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
}

HGM_TEST(detection, contradictory_telemetry_cannot_support_a_hotspot) {
  SyntheticConfig config = localized_config();
  config.contradictory_sample = true;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK(decision.assessment.contradictory_count > 0);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::None);
  bool saw = false;
  for (const ResourcePressure& pressure : decision.assessment.pressures) {
    if (pressure.contradictory) saw = true;
  }
  HGM_CHECK(saw);
}

HGM_TEST(detection, missing_policy_still_detects_but_authorises_nothing) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot expired = harness.fabric.policy(harness.now);
  expired.set_issued_at(harness.now - 100000);
  expired.set_ttl_ms(1000);
  const Decision decision =
      evaluate_with(harness, nullptr, nullptr, nullptr, nullptr, nullptr, &expired);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.hotspots.front().remediation, RemediationState::Suppressed);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Policy));
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
}

HGM_TEST(detection, path_generation_mismatch_removes_attribution_only) {
  Harness harness(localized_config());
  harness.run(3);
  PathSnapshot paths = harness.fabric.paths();
  paths.set_topology_generation(TopologyGeneration::from(7777));
  paths.build();
  const Decision decision = evaluate_with(harness, &paths, nullptr, nullptr, nullptr, nullptr, nullptr);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Localized);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Paths));
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Attribution));
  bool saw = false;
  for (const Diagnostic& diagnostic : decision.assessment.diagnostics) {
    if (diagnostic.code == ErrorCode::PathGenerationMismatch) saw = true;
  }
  HGM_CHECK(saw);
}

HGM_TEST(detection, simultaneous_hotspots_stay_separate) {
  SyntheticConfig config = localized_config();
  config.hotspot_count = 2;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Localized);
  HGM_CHECK_NE(decision.assessment.hotspots[0].id, decision.assessment.hotspots[1].id);
  HGM_CHECK(!decision.assessment.escalation_required);
  // One bounded intent per confirmed hotspot.
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(2));
  HGM_CHECK_NE(decision.plan.intents[0].hotspot, decision.plan.intents[1].hotspot);
}

HGM_TEST(detection, a_region_spanning_component_is_regional_not_localized) {
  SyntheticConfig config;
  config.region_count = 2;
  config.resources_per_region = 4;
  config.hotspot_count = 2;
  config.hotspot_index_base = 0;
  config.path_fan_in = 3;
  // Both designated resources are the backbone resource of their region, so
  // they are adjacent and form one component spanning two regions.
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK(harness.fabric.build_ok());
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.hotspots.front().regions.size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Regional);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
}

HGM_TEST(detection, evidence_naming_an_unknown_resource_is_refused) {
  Harness harness(localized_config());
  harness.run(3);
  const ResourceId stranger = ResourceId::from_name("resource-that-does-not-exist");

  {
    SignalSnapshot signals = harness.fabric.signals();
    ResourceSignals& extra = signals.add(stranger);
    extra.utilized_units = 1;
    extra.observed_at = harness.now;
    extra.confidence_ppm = 900000;
    extra.quality = TelemetryQuality::Healthy;
    HGM_CHECK(signals.build().ok());
    const Decision decision =
        evaluate_with(harness, nullptr, nullptr, nullptr, &signals, nullptr, nullptr);
    HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
    HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Signals));
  }
  {
    CapacitySnapshot capacity = harness.fabric.capacity();
    ResourceCapacity& extra = capacity.add(stranger);
    extra.capacity_units = 1000;
    HGM_CHECK(capacity.build().ok());
    const Decision decision =
        evaluate_with(harness, nullptr, nullptr, &capacity, nullptr, nullptr, nullptr);
    HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
    HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Capacity));
  }
  {
    PathSnapshot paths = harness.fabric.paths();
    PathRecord& extra = paths.add(PathId::from_name("rogue-path"), FlowId::from_name("rogue-flow"));
    extra.hops = {stranger};
    HGM_CHECK(paths.build().ok());
    const Decision decision =
        evaluate_with(harness, &paths, nullptr, nullptr, nullptr, nullptr, nullptr);
    HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
    HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Paths));
  }
}

HGM_TEST(detection, a_fully_dropped_offer_is_not_a_contradiction) {
  Harness harness(localized_config());
  harness.run(3);
  SignalSnapshot signals = harness.fabric.signals();
  // Offered load that was entirely dropped is self-consistent telemetry.
  for (ResourceSignals& record : const_cast<std::vector<ResourceSignals>&>(signals.records())) {
    record.offered_bps = 1000;
    record.admitted_bps = 0;
    record.utilized_units = 0;
    record.queue_depth_units = 0;
    record.drop_units = 1000;
  }
  signals.set_generation(EvidenceGeneration::from(harness.fabric.signals().generation().value() + 1));
  HGM_CHECK(signals.build().ok());
  const Decision decision = evaluate_with(harness, nullptr, nullptr, nullptr, &signals, nullptr, nullptr);
  HGM_CHECK_EQ(decision.assessment.contradictory_count, static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
}

HGM_TEST(detection, detection_without_a_tracker_confirms_nothing) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
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
  input.tracker = nullptr;
  const FabricAssessment assessment = detect(input);
  HGM_CHECK_EQ(assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(assessment.candidates.size(), static_cast<std::size_t>(1));
}

HGM_TEST(detection, hotspot_identity_is_stable_for_identical_evidence) {
  Harness first(localized_config());
  Harness second(localized_config());
  const Decision a = first.run(3);
  const Decision b = second.run(3);
  HGM_CHECK_EQ(a.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(a.assessment.hotspots.front().id, b.assessment.hotspots.front().id);
  HGM_CHECK_EQ(a.assessment.hotspots.front().key, b.assessment.hotspots.front().key);
}

HGM_TEST(detection, authority_vector_denies_without_authority_installed) {
  Harness harness(localized_config());
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.policy = &policy;
  input.now = harness.now;
  input.tracker = &harness.engine.tracker();
  const FabricAssessment assessment = detect(input);
  HGM_CHECK(!assessment.authority.granted(AuthorityDomain::CoordinatorEpoch));
  HGM_CHECK(!assessment.authority.granted(AuthorityDomain::BootIncarnation));
  HGM_CHECK(assessment.authority.granted(AuthorityDomain::Topology));
}
