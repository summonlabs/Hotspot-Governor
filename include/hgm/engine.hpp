// The decision engine: one deterministic pass from generation-bound evidence
// to assessment, bounded mitigation intent and explanation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "hgm/detection.hpp"
#include "hgm/explanation.hpp"
#include "hgm/intervention.hpp"
#include "hgm/metrics.hpp"
#include "hgm/time.hpp"

namespace hgm {

struct EngineConfig {
  bool include_explanation = true;
  ExplanationOptions explanation{};
};

struct Decision {
  FabricAssessment assessment;
  InterventionPlan plan;
  std::string explanation;
  Millis evaluated_at = kNoTime;
  EvidenceBinding binding{};
};

class Engine {
 public:
  explicit Engine(EngineConfig config = EngineConfig{}) : config_(config) {}

  // Runs one evaluation. Deterministic for identical input, identical tracker
  // state and identical config.
  Decision decide(const DecisionInput& input);

  SaturationTracker& tracker() noexcept { return tracker_; }
  const SaturationTracker& tracker() const noexcept { return tracker_; }

  void reset() {
    tracker_.clear();
    evaluations_ = 0;
  }

  const Metrics& metrics() const noexcept { return metrics_; }
  Metrics& metrics() noexcept { return metrics_; }
  std::uint64_t evaluations() const noexcept { return evaluations_; }
  const EngineConfig& config() const noexcept { return config_; }

 private:
  EngineConfig config_{};
  SaturationTracker tracker_{};
  Metrics metrics_{};
  std::uint64_t evaluations_ = 0;
};

}  // namespace hgm
