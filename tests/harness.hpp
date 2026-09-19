// Shared fixture for SYNTHETIC populations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <utility>

#include "hgm/engine.hpp"
#include "hgm/synthetic.hpp"

namespace hgmt {

// Drives the engine over a synthetic fabric with a monotonically advancing
// clock, so persistence and staleness behave as they do in a live governor.
struct Harness {
  hgm::SyntheticConfig config;
  hgm::SyntheticFabric fabric;
  hgm::Engine engine;
  hgm::Millis now = 10000;
  std::uint64_t generation = 1;

  explicit Harness(hgm::SyntheticConfig configuration = hgm::SyntheticConfig{})
      : config(std::move(configuration)), fabric(config) {}

  hgm::Decision step(hgm::Millis delta_ms = 1000) {
    now += delta_ms;
    ++generation;
    fabric.rebuild(now, generation);
    return evaluate();
  }

  hgm::Decision run(std::uint32_t evaluations, hgm::Millis delta_ms = 1000) {
    hgm::Decision decision;
    for (std::uint32_t i = 0; i < evaluations; ++i) decision = step(delta_ms);
    return decision;
  }

  hgm::Decision evaluate() {
    hgm::PolicySnapshot policy = fabric.policy(now);
    hgm::DecisionInput input;
    input.topology = &fabric.topology();
    input.capacity = &fabric.capacity();
    input.signals = &fabric.signals();
    input.paths = &fabric.paths();
    input.traffic = &fabric.traffic();
    input.policy = &policy;
    input.now = now;
    input.epoch = hgm::CoordinatorEpoch::from(1);
    input.boot = hgm::BootId::from(1);
    input.tracker = &engine.tracker();
    return engine.decide(input);
  }
};

}  // namespace hgmt
