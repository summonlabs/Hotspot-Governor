// Seeded randomized properties over SYNTHETIC populations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"
#include "hgm/checked.hpp"
#include "hgm/explanation.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::Harness;

namespace {

struct Population {
  std::uint32_t regions;
  std::uint32_t resources_per_region;
  std::uint32_t hotspots;
  std::uint32_t fan_in;
  std::uint32_t density_ppm;
  bool global;
};

std::vector<Population> populations() {
  std::vector<Population> cases;
  SyntheticRng rng(0xC0FFEEull);
  for (int i = 0; i < 24; ++i) {
    Population population;
    population.regions = 1 + rng.below(6);
    population.resources_per_region = 2 + rng.below(12);
    population.hotspots = rng.below(4);
    population.fan_in = 1 + rng.below(6);
    population.density_ppm = 400000 + rng.below(600000);
    population.global = rng.below(8) == 0;
    cases.push_back(population);
  }
  return cases;
}

SyntheticConfig to_config(const Population& population) {
  SyntheticConfig config;
  config.region_count = population.regions;
  config.resources_per_region = population.resources_per_region;
  config.hotspot_count = population.hotspots;
  config.path_fan_in = population.fan_in;
  config.evidence_density_ppm = population.density_ppm;
  config.global_congestion = population.global;
  return config;
}

void check_assessment_invariants(const FabricAssessment& assessment) {
  if (assessment.scope == CongestionScope::Unknown) {
    HGM_CHECK_EQ(assessment.hotspots.size(), static_cast<std::size_t>(0));
  }
  if (assessment.scope == CongestionScope::Localized ||
      assessment.scope == CongestionScope::Regional ||
      assessment.scope == CongestionScope::Global) {
    HGM_CHECK(!assessment.hotspots.empty());
  }
  HGM_CHECK(assessment.global_share_ppm <= 1000000);
  HGM_CHECK(assessment.coverage_ppm <= 1000000);
  HGM_CHECK(assessment.admissible_coverage_ppm <= 1000000);
  HGM_CHECK(assessment.saturated_count <= assessment.admissible_count);
  HGM_CHECK(assessment.admissible_count <= assessment.observed_count);
  for (const Hotspot& hotspot : assessment.hotspots) {
    HGM_CHECK(hotspot.persistent);
    HGM_CHECK(hotspot.id.valid());
    HGM_CHECK(!hotspot.key.empty());
    HGM_CHECK(hotspot.saturated_count > 0);
    HGM_CHECK(hotspot.saturated_resources.size() <= Limits::kMaxHotspotResources);
    HGM_CHECK(hotspot.contributors.size() <= Limits::kMaxContributors);
    HGM_CHECK(hotspot.share_ppm <= 1000000);
    HGM_CHECK(hotspot.attribution_confidence_ppm <= 1000000);
    HGM_CHECK(hotspot.persistence_ms >= 0);
    HGM_CHECK(hotspot.sample_count >= 1);
    HGM_CHECK(hotspot.severity != Severity::None);
    if (hotspot.scope == CongestionScope::Localized) {
      HGM_CHECK(!hotspot.healthy_neighbors.empty());
      HGM_CHECK_EQ(hotspot.remediation, RemediationState::Eligible);
    }
    for (const ResourceId id : hotspot.saturated_resources) HGM_CHECK(id.valid());
    for (const ResourceId id : hotspot.healthy_neighbors) HGM_CHECK(id.valid());
    std::uint64_t shares = 0;
    for (const Contributor& contributor : hotspot.contributors) {
      HGM_CHECK(contributor.share_ppm <= 1000000);
      shares += contributor.share_ppm;
    }
    HGM_CHECK(shares <= 1000000 + hotspot.contributors.size());
  }
  for (const Hotspot& candidate : assessment.candidates) {
    HGM_CHECK(!candidate.persistent);
  }
}

void check_plan_invariants(const InterventionPlan& plan, const FabricAssessment& assessment) {
  HGM_CHECK(plan.intents.size() <= Limits::kMaxHotspots);
  HGM_CHECK(plan.plan_scope_share_ppm <= 1000000);
  std::vector<HotspotId> hotspots;
  for (const MitigationIntent& intent : plan.intents) {
    HGM_CHECK(intent.id.valid());
    HGM_CHECK(intent.kind != MitigationKind::None);
    HGM_CHECK(intent.affected_resources.size() <= Limits::kMaxIntentAffectedResources);
    HGM_CHECK(intent.affected_paths.size() <= Limits::kMaxIntentAffectedPaths);
    HGM_CHECK(intent.affected_flows.size() <= Limits::kMaxIntentAffectedFlows);
    HGM_CHECK(intent.scope_share_ppm <= 1000000);
    HGM_CHECK(intent.rate_delta_bounded_ppm <= 250000);
    for (const HotspotId id : hotspots) HGM_CHECK_NE(id, intent.hotspot);
    hotspots.push_back(intent.hotspot);
    if (!intent.escalation) {
      HGM_CHECK(assessment.find_hotspot(intent.hotspot) != nullptr);
    }
  }
  for (const SuppressedAction& action : plan.suppressed) {
    HGM_CHECK(!action.reason.empty());
    HGM_CHECK(action.reason.size() <= Limits::kMaxReasonBytes);
  }
}

}  // namespace

HGM_TEST(property, synthetic_populations_hold_every_invariant) {
  for (const Population& population : populations()) {
    Harness harness(to_config(population));
    Decision decision;
    for (int round = 0; round < 4; ++round) decision = harness.step();
    HGM_CHECK(harness.fabric.build_ok());
    check_assessment_invariants(decision.assessment);
    check_plan_invariants(decision.plan, decision.assessment);
    HGM_CHECK(decision.explanation.size() <= 16384);
    HGM_CHECK(harness.engine.tracker().size() <= harness.fabric.topology().resource_count());
  }
}

HGM_TEST(property, identical_inputs_produce_identical_decisions) {
  for (const Population& population : populations()) {
    Harness first(to_config(population));
    Harness second(to_config(population));
    Decision a;
    Decision b;
    for (int round = 0; round < 3; ++round) {
      a = first.step();
      b = second.step();
    }
    HGM_CHECK_EQ(a.assessment.scope, b.assessment.scope);
    HGM_CHECK_EQ(a.assessment.hotspots.size(), b.assessment.hotspots.size());
    HGM_CHECK_EQ(a.plan.intents.size(), b.plan.intents.size());
    for (std::size_t i = 0; i < a.assessment.hotspots.size(); ++i) {
      HGM_CHECK_EQ(a.assessment.hotspots[i].id, b.assessment.hotspots[i].id);
      HGM_CHECK_EQ(a.assessment.hotspots[i].severity, b.assessment.hotspots[i].severity);
      HGM_CHECK_EQ(a.assessment.hotspots[i].cause, b.assessment.hotspots[i].cause);
    }
    for (std::size_t i = 0; i < a.plan.intents.size(); ++i) {
      HGM_CHECK_EQ(a.plan.intents[i].id, b.plan.intents[i].id);
      HGM_CHECK_EQ(a.plan.intents[i].kind, b.plan.intents[i].kind);
    }
    HGM_CHECK_EQ(a.explanation, b.explanation);
  }
}

HGM_TEST(property, detection_is_monotone_in_evidence_density) {
  SyntheticConfig config;
  config.region_count = 4;
  config.resources_per_region = 16;
  config.hotspot_count = 2;
  config.path_fan_in = 3;
  std::uint32_t previous_coverage = 1000001;
  for (const std::uint32_t density : {1000000u, 900000u, 800000u, 700000u, 600000u}) {
    config.evidence_density_ppm = density;
    Harness harness(config);
    const Decision decision = harness.run(3);
    HGM_CHECK(decision.assessment.admissible_coverage_ppm <= previous_coverage ||
              decision.assessment.scope == CongestionScope::Unknown);
    previous_coverage = decision.assessment.admissible_coverage_ppm;
  }
}

HGM_TEST(property, ratio_ppm_is_bounded_over_random_inputs) {
  SyntheticRng rng(0x1234);
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t whole = rng.next() % 1000000ull + 1;
    const std::uint64_t part = rng.next() % (whole * 2 + 1);
    const auto ppm = ratio_ppm(part, whole);
    HGM_CHECK(ppm.has_value());
    HGM_CHECK(*ppm <= 2000000u + 1u);
    if (part == 0) HGM_CHECK_EQ(*ppm, 0u);
    if (part == whole) HGM_CHECK_EQ(*ppm, 1000000u);
    if (part > whole) HGM_CHECK(*ppm > 1000000u);
  }
  HGM_CHECK(!ratio_ppm(1, 0).has_value());
}

HGM_TEST(property, checked_arithmetic_never_wraps) {
  SyntheticRng rng(0x9999);
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t a = rng.next();
    const std::uint64_t b = rng.next();
    std::uint64_t out = 0;
    if (add_ok<std::uint64_t>(a, b, out)) {
      HGM_CHECK(out >= a);
      HGM_CHECK(out >= b);
    } else {
      HGM_CHECK(a > UINT64_MAX - b);
    }
    if (mul_ok<std::uint64_t>(a, b, out)) {
      if (a != 0) HGM_CHECK_EQ(out / a, b);
    } else {
      HGM_CHECK(a != 0 && b > UINT64_MAX / a);
    }
    if (sub_ok<std::uint64_t>(a, b, out)) {
      HGM_CHECK(out <= a);
    } else {
      HGM_CHECK(b > a);
    }
  }
}

HGM_TEST(property, tracker_never_grows_past_the_saturated_set) {
  SyntheticRng rng(0xBEEF);
  for (int i = 0; i < 8; ++i) {
    SyntheticConfig config;
    config.region_count = 1 + rng.below(4);
    config.resources_per_region = 2 + rng.below(8);
    config.hotspot_count = rng.below(3);
    config.path_fan_in = 1 + rng.below(4);
    Harness harness(config);
    Decision decision;
    for (int round = 0; round < 3; ++round) {
      decision = harness.step();
      HGM_CHECK(harness.engine.tracker().size() <= decision.assessment.saturated_count);
    }
  }
}

HGM_TEST(property, explanation_stays_within_every_bound) {
  SyntheticRng rng(0x515151);
  for (int i = 0; i < 8; ++i) {
    SyntheticConfig config;
    config.region_count = 2 + rng.below(8);
    config.resources_per_region = 8 + rng.below(32);
    config.hotspot_count = rng.below(6);
    config.path_fan_in = 1 + rng.below(8);
    Harness harness(config);
    const Decision decision = harness.run(3);
    ExplanationOptions options;
    options.max_lines = 12;
    options.max_bytes = 512;
    const std::string text = explain(decision.assessment, decision.plan, options);
    HGM_CHECK(text.size() <= options.max_bytes);
    const std::size_t lines =
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + (text.empty() ? 0 : 1);
    HGM_CHECK(lines <= options.max_lines);
  }
}
