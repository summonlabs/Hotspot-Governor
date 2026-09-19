// Path evidence: which resources a path traverses, bound to an exact topology
// generation. Paths are reported by the fabric; the governor never computes,
// repairs or legalises them.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "hgm/digest.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"

namespace hgm {

struct PathRecord {
  PathId id{};
  FlowId flow{};  // 0 when the publisher does not attribute a flow
  std::vector<ResourceId> hops;
};

class PathSnapshot {
 public:
  PathSnapshot() = default;

  PathRecord& add(PathId id, FlowId flow);
  Status build();

  bool built() const noexcept { return built_; }
  PathSetId id() const noexcept { return id_; }
  const Digest& content_digest() const noexcept { return digest_; }

  PathGeneration generation() const noexcept { return generation_; }
  void set_generation(PathGeneration generation) noexcept { generation_ = generation; }

  TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
  void set_topology_generation(TopologyGeneration generation) noexcept { topology_generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  Millis observed_at() const noexcept { return observed_at_; }
  void set_observed_at(Millis value) noexcept { observed_at_ = value; }

  const std::vector<PathRecord>& paths() const noexcept { return paths_; }
  std::size_t size() const noexcept { return paths_.size(); }
  const PathRecord* find(PathId id) const;

  // Indices of paths that traverse the given resource, in ascending path order.
  std::span<const std::uint32_t> paths_through(ResourceId resource) const;

 private:
  std::vector<PathRecord> paths_;
  std::vector<std::uint32_t> through_offsets_;
  std::vector<ResourceId> through_resources_;
  std::vector<std::uint32_t> through_paths_;
  PathSetId id_{};
  Digest digest_{};
  PathGeneration generation_{};
  TopologyGeneration topology_generation_{};
  Provenance provenance_{};
  Millis observed_at_ = kNoTime;
  bool built_ = false;
};

}  // namespace hgm
