// Contributing-traffic evidence: how much offered demand each flow places on
// each path, bound to an exact path generation.
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

struct TrafficRecord {
  FlowId flow{};
  PathId path{};
  std::uint64_t demand_bps = 0;
  TelemetryQuality quality = TelemetryQuality::Unknown;
  Millis observed_at = kNoTime;
};

class TrafficSnapshot {
 public:
  TrafficSnapshot() = default;

  TrafficRecord& add(FlowId flow, PathId path);
  Status build();

  bool built() const noexcept { return built_; }
  TrafficId id() const noexcept { return id_; }
  const Digest& content_digest() const noexcept { return digest_; }

  TrafficGeneration generation() const noexcept { return generation_; }
  void set_generation(TrafficGeneration generation) noexcept { generation_ = generation; }

  PathGeneration path_generation() const noexcept { return path_generation_; }
  void set_path_generation(PathGeneration generation) noexcept { path_generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  const std::vector<TrafficRecord>& records() const noexcept { return records_; }
  std::size_t size() const noexcept { return records_.size(); }

 private:
  std::vector<TrafficRecord> records_;
  TrafficId id_{};
  Digest digest_{};
  TrafficGeneration generation_{};
  PathGeneration path_generation_{};
  Provenance provenance_{};
  bool built_ = false;
};

}  // namespace hgm
