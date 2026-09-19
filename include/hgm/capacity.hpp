// Capacity evidence: the authoritative denominb for every utilisation ratio.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "hgm/digest.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"

namespace hgm {

struct ResourceCapacity {
  ResourceId resource{};
  std::uint64_t capacity_units = 0;      // service capacity; 0 means unusable
  std::uint64_t queue_limit_units = 0;   // 0 means "not applicable"
  std::uint64_t buffer_limit_bytes = 0;  // 0 means "not applicable"
  bool usable = true;                    // publisher-declared admin state
};

class CapacitySnapshot {
 public:
  CapacitySnapshot() = default;

  ResourceCapacity& add(ResourceId resource);
  Status build();

  bool built() const noexcept { return built_; }
  CapacityId id() const noexcept { return id_; }
  const Digest& content_digest() const noexcept { return digest_; }

  CapacityGeneration generation() const noexcept { return generation_; }
  void set_generation(CapacityGeneration generation) noexcept { generation_ = generation; }

  TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
  void set_topology_generation(TopologyGeneration generation) noexcept { topology_generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  Millis observed_at() const noexcept { return observed_at_; }
  void set_observed_at(Millis value) noexcept { observed_at_ = value; }

  const std::vector<ResourceCapacity>& entries() const noexcept { return entries_; }
  std::size_t size() const noexcept { return entries_.size(); }
  const ResourceCapacity* find(ResourceId id) const;

 private:
  std::vector<ResourceCapacity> entries_;
  CapacityId id_{};
  Digest digest_{};
  CapacityGeneration generation_{};
  TopologyGeneration topology_generation_{};
  Provenance provenance_{};
  Millis observed_at_ = kNoTime;
  bool built_ = false;
};

}  // namespace hgm
