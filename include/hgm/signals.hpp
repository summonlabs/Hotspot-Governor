// Utilisation, queue and buffer telemetry, plus the freshness rules that make
// a sample admissible.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "hgm/digest.hpp"
#include "hgm/enums.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"

namespace hgm {

struct ResourceSignals {
  ResourceId resource{};
  std::uint64_t utilized_units = 0;
  std::uint64_t queue_depth_units = 0;
  std::uint64_t buffer_used_bytes = 0;
  std::uint64_t offered_bps = 0;
  std::uint64_t admitted_bps = 0;
  std::uint64_t drop_units = 0;
  std::uint32_t confidence_ppm = 0;
  TelemetryQuality quality = TelemetryQuality::Unknown;
  Millis observed_at = kNoTime;
};

class SignalSnapshot {
 public:
  SignalSnapshot() = default;

  ResourceSignals& add(ResourceId resource);
  Status build();

  bool built() const noexcept { return built_; }
  EvidenceId id() const noexcept { return id_; }
  const Digest& content_digest() const noexcept { return digest_; }

  EvidenceGeneration generation() const noexcept { return generation_; }
  void set_generation(EvidenceGeneration generation) noexcept { generation_ = generation; }

  TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
  void set_topology_generation(TopologyGeneration generation) noexcept { topology_generation_ = generation; }

  CapacityGeneration capacity_generation() const noexcept { return capacity_generation_; }
  void set_capacity_generation(CapacityGeneration generation) noexcept { capacity_generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  Millis window_start() const noexcept { return window_start_; }
  Millis window_end() const noexcept { return window_end_; }
  void set_window(Millis start, Millis end) noexcept {
    window_start_ = start;
    window_end_ = end;
  }

  const std::vector<ResourceSignals>& records() const noexcept { return records_; }
  std::size_t size() const noexcept { return records_.size(); }
  const ResourceSignals* find(ResourceId id) const;

 private:
  std::vector<ResourceSignals> records_;
  EvidenceId id_{};
  Digest digest_{};
  EvidenceGeneration generation_{};
  TopologyGeneration topology_generation_{};
  CapacityGeneration capacity_generation_{};
  Provenance provenance_{};
  Millis window_start_ = kNoTime;
  Millis window_end_ = kNoTime;
  bool built_ = false;
};

}  // namespace hgm
