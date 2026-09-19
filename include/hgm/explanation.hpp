// Bounded, deterministic rendering of the assessment and the plan.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

#include "hgm/detection.hpp"
#include "hgm/intervention.hpp"
#include "hgm/limits.hpp"

namespace hgm {

struct ExplanationOptions {
  std::size_t max_lines = Limits::kMaxExplanationLines;
  std::size_t max_bytes = 16384;
  std::size_t max_pressures = 16;
  std::size_t max_neighbors = 8;
  std::size_t max_contributors = 8;
  bool include_authority = true;
};

// Renders scope, evidence, neighbouring comparison, contributing traffic,
// binding thresholds, authorised and suppressed mitigation, and the authority
// vector. Output is truncated deterministically at the configured bounds.
std::string explain(const FabricAssessment& assessment, const ExplanationOptions& options = {});

std::string explain(const InterventionPlan& plan, const ExplanationOptions& options = {});

std::string explain(const FabricAssessment& assessment, const InterventionPlan& plan,
                    const ExplanationOptions& options = {});

}  // namespace hgm
