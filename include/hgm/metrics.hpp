// Bounded accounting counters. All counters are monotonically increasing and
// saturate rather than wrap.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "hgm/checked.hpp"

namespace hgm {

struct Metrics {
  std::uint64_t frames_received = 0;
  std::uint64_t frames_accepted = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t frames_duplicate = 0;
  std::uint64_t frames_stale = 0;
  std::uint64_t frames_malformed = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t publisher_deaths = 0;
  std::uint64_t epoch_advances = 0;
  std::uint64_t evaluations = 0;
  std::uint64_t hotspots_confirmed = 0;
  std::uint64_t candidates_observed = 0;
  std::uint64_t interventions_planned = 0;
  std::uint64_t interventions_suppressed = 0;
  std::uint64_t escalations = 0;
  std::uint64_t journal_appends = 0;
  std::uint64_t journal_replays = 0;
  std::uint64_t recoveries = 0;
  std::uint64_t rejected_stale_epoch = 0;
  std::uint64_t rejected_stale_generation = 0;
  std::uint64_t rejected_stale_incarnation = 0;

  void count(std::uint64_t& counter, std::uint64_t delta = 1) noexcept { counter = sat_add(counter, delta); }

  std::string render() const;
};

}  // namespace hgm
