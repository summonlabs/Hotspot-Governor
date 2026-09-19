// Authoritative topology evidence: regions, resources and their adjacency.
// The governor consumes topology truth; it never computes or repairs it.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hgm/digest.hpp"
#include "hgm/enums.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/provenance.hpp"
#include "hgm/time.hpp"

namespace hgm {

// A grouping of resources. Regions nest through parent; the governor uses the
// transitive region set of a resource when bounding localization.
struct Region {
  RegionId id{};
  std::string name;
  RegionId parent{};
};

struct Resource {
  ResourceId id{};
  RegionId region{};
  ResourceKind kind = ResourceKind::Unknown;
  std::string name;
};

// An undirected adjacency between two resources.
struct Link {
  LinkId id{};
  ResourceId endpoint_a{};
  ResourceId endpoint_b{};
};

class TopologySnapshot {
 public:
  TopologySnapshot() = default;

  Region& add_region(RegionId id, std::string name, RegionId parent = RegionId{});
  Resource& add_resource(ResourceId id, RegionId region, ResourceKind kind, std::string name = std::string());
  Link& add_link(LinkId id, ResourceId a, ResourceId b);

  // Sorts, validates, builds the adjacency index and derives the content
  // identity. Must succeed before the snapshot is usable.
  Status build();

  bool built() const noexcept { return built_; }
  TopologyId id() const noexcept { return id_; }
  const Digest& content_digest() const noexcept { return digest_; }

  TopologyGeneration generation() const noexcept { return generation_; }
  void set_generation(TopologyGeneration generation) noexcept { generation_ = generation; }

  const Provenance& provenance() const noexcept { return provenance_; }
  void set_provenance(const Provenance& provenance) { provenance_ = provenance; }

  Millis valid_from() const noexcept { return valid_from_; }
  Millis valid_until() const noexcept { return valid_until_; }
  void set_validity(Millis from, Millis until) noexcept {
    valid_from_ = from;
    valid_until_ = until;
  }
  bool fresh_at(Millis now) const noexcept {
    if (valid_until_ == 0) return true;
    return now <= valid_until_;
  }

  std::size_t resource_count() const noexcept { return resources_.size(); }
  std::size_t region_count() const noexcept { return regions_.size(); }
  std::size_t link_count() const noexcept { return links_.size(); }
  bool empty() const noexcept { return resources_.empty(); }

  const std::vector<Resource>& resources() const noexcept { return resources_; }
  const std::vector<Region>& regions() const noexcept { return regions_; }
  const std::vector<Link>& links() const noexcept { return links_; }

  const Resource* find_resource(ResourceId id) const;
  const Region* find_region(RegionId id) const;
  bool has_region(RegionId id) const;
  bool has_resource(ResourceId id) const { return find_resource(id) != nullptr; }

  // Sorted, de-duplicated neighbours of a resource. Empty for unknown ids.
  std::span<const ResourceId> neighbors(ResourceId id) const;

  // The resource's own region followed by its ancestors, nearest first.
  std::span<const RegionId> region_chain(ResourceId id) const;

  // A resource that is neither id nor reachable through the region chain of a
  // different branch. Used only for explanation, not for admissibility.
  bool same_region(ResourceId a, ResourceId b) const;

 private:
  Status validate_and_index();

  std::vector<Region> regions_;
  std::vector<Resource> resources_;
  std::vector<Link> links_;

  // Adjacency, CSR form: offsets_[i] .. offsets_[i + 1) index into neighbor_ids_.
  std::vector<std::uint32_t> neighbor_offsets_;
  std::vector<ResourceId> neighbor_ids_;

  // Region chains, CSR form, parallel to resources_.
  std::vector<std::uint32_t> chain_offsets_;
  std::vector<RegionId> chain_ids_;

  std::vector<std::uint32_t> region_index_by_resource_;

  TopologyId id_{};
  Digest digest_{};
  TopologyGeneration generation_{};
  Provenance provenance_{};
  Millis valid_from_ = kNoTime;
  Millis valid_until_ = 0;
  bool built_ = false;
};

}  // namespace hgm
