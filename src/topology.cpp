// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/topology.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "hgm/byteio.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

std::string bounded_name(std::string name, std::size_t limit) {
  if (name.size() > limit) name.resize(limit);
  return name;
}

}  // namespace

Region& TopologySnapshot::add_region(RegionId id, std::string name, RegionId parent) {
  regions_.push_back(Region{id, bounded_name(std::move(name), Limits::kMaxRegionNameBytes), parent});
  built_ = false;
  return regions_.back();
}

Resource& TopologySnapshot::add_resource(ResourceId id, RegionId region, ResourceKind kind, std::string name) {
  resources_.push_back(
      Resource{id, region, kind, bounded_name(std::move(name), Limits::kMaxResourceNameBytes)});
  built_ = false;
  return resources_.back();
}

Link& TopologySnapshot::add_link(LinkId id, ResourceId a, ResourceId b) {
  links_.push_back(Link{id, a, b});
  built_ = false;
  return links_.back();
}

Status TopologySnapshot::build() {
  built_ = false;
  id_ = TopologyId{};
  digest_ = Digest{};

  if (regions_.size() > Limits::kMaxRegions) {
    return Status::failure(ErrorCode::BoundExceeded, "region count exceeds limit");
  }
  if (resources_.size() > Limits::kMaxResources) {
    return Status::failure(ErrorCode::BoundExceeded, "resource count exceeds limit");
  }
  if (links_.size() > Limits::kMaxLinks) {
    return Status::failure(ErrorCode::BoundExceeded, "link count exceeds limit");
  }

  Status status = validate_and_index();
  if (!status.ok()) return status;

  std::vector<std::byte> encoded;
  encoded.reserve(resources_.size() * 24 + links_.size() * 16 + regions_.size() * 16);
  {
    ByteWriter writer(encoded);
    writer.u32(static_cast<std::uint32_t>(regions_.size()));
    for (const Region& region : regions_) {
      writer.u64(region.id.value());
      writer.u64(region.parent.value());
      writer.text_field(region.name);
    }
    writer.u32(static_cast<std::uint32_t>(resources_.size()));
    for (const Resource& resource : resources_) {
      writer.u64(resource.id.value());
      writer.u64(resource.region.value());
      writer.u8(static_cast<std::uint8_t>(resource.kind));
      writer.text_field(resource.name);
    }
    writer.u32(static_cast<std::uint32_t>(links_.size()));
    for (const Link& link : links_) {
      writer.u64(link.id.value());
      writer.u64(link.endpoint_a.value());
      writer.u64(link.endpoint_b.value());
    }
  }

  digest_ = digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
  id_ = TopologyId::from(digest_.fnv);
  built_ = true;
  return Status::success();
}

Status TopologySnapshot::validate_and_index() {
  // --- regions ---
  std::sort(regions_.begin(), regions_.end(), [](const Region& a, const Region& b) { return a.id < b.id; });
  for (std::size_t i = 0; i < regions_.size(); ++i) {
    if (!regions_[i].id.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "region id 0 is reserved");
    }
    if (i > 0 && regions_[i].id == regions_[i - 1].id) {
      return Status::failure(ErrorCode::InvalidArgument, "duplicate region id " + regions_[i].id.hex());
    }
    if (regions_[i].name.size() > Limits::kMaxRegionNameBytes) {
      return Status::failure(ErrorCode::BoundExceeded, "region name exceeds limit");
    }
  }
  for (const Region& region : regions_) {
    if (region.parent.valid()) {
      const auto parent = std::lower_bound(regions_.begin(), regions_.end(), region.parent,
                                           [](const Region& entry, RegionId id) { return entry.id < id; });
      if (parent == regions_.end() || parent->id != region.parent) {
        return Status::failure(ErrorCode::UnknownRegion,
                               "region parent " + region.parent.hex() + " is unknown");
      }
    }
    if (region.parent == region.id) {
      return Status::failure(ErrorCode::InvalidArgument, "region is its own parent");
    }
  }
  // Region cycles are a structural defect: walk with a bounded step count.
  for (std::size_t i = 0; i < regions_.size(); ++i) {
    RegionId cursor = regions_[i].parent;
    std::size_t steps = 0;
    while (cursor.valid()) {
      if (++steps > regions_.size()) {
        return Status::failure(ErrorCode::InvalidArgument, "region parent cycle detected");
      }
      const Region* parent = find_region(cursor);
      if (parent == nullptr) break;
      cursor = parent->parent;
    }
  }

  // --- resources ---
  std::sort(resources_.begin(), resources_.end(),
            [](const Resource& a, const Resource& b) { return a.id < b.id; });
  for (std::size_t i = 0; i < resources_.size(); ++i) {
    const Resource& resource = resources_[i];
    if (!resource.id.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "resource id 0 is reserved");
    }
    if (i > 0 && resources_[i].id == resources_[i - 1].id) {
      return Status::failure(ErrorCode::InvalidArgument, "duplicate resource id " + resource.id.hex());
    }
    if (!resource.region.valid() || !has_region(resource.region)) {
      return Status::failure(ErrorCode::UnknownRegion,
                             "resource " + resource.id.hex() + " names unknown region");
    }
    if (resource.name.size() > Limits::kMaxResourceNameBytes) {
      return Status::failure(ErrorCode::BoundExceeded, "resource name exceeds limit");
    }
  }

  // --- links ---
  std::sort(links_.begin(), links_.end(), [](const Link& a, const Link& b) {
    const ResourceId a_lo = a.endpoint_a < a.endpoint_b ? a.endpoint_a : a.endpoint_b;
    const ResourceId b_lo = b.endpoint_a < b.endpoint_b ? b.endpoint_a : b.endpoint_b;
    if (a_lo != b_lo) return a_lo < b_lo;
    const ResourceId a_hi = a.endpoint_a < a.endpoint_b ? a.endpoint_b : a.endpoint_a;
    const ResourceId b_hi = b.endpoint_a < b.endpoint_b ? b.endpoint_b : b.endpoint_a;
    return a_hi < b_hi;
  });
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const Link& link = links_[i];
    if (!link.endpoint_a.valid() || !link.endpoint_b.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "link names resource id 0");
    }
    if (link.endpoint_a == link.endpoint_b) {
      return Status::failure(ErrorCode::InvalidArgument, "link is a self loop");
    }
    if (!has_resource(link.endpoint_a) || !has_resource(link.endpoint_b)) {
      return Status::failure(ErrorCode::UnknownResource, "link endpoint is not a known resource");
    }
    if (i > 0) {
      const Link& prev = links_[i - 1];
      const bool same_pair = (prev.endpoint_a == link.endpoint_a && prev.endpoint_b == link.endpoint_b) ||
                             (prev.endpoint_a == link.endpoint_b && prev.endpoint_b == link.endpoint_a);
      if (same_pair) {
        return Status::failure(ErrorCode::InvalidArgument, "duplicate adjacency between two resources");
      }
    }
  }

  // --- adjacency CSR ---
  const std::size_t n = resources_.size();
  std::vector<std::uint32_t> degree(n, 0u);
  for (const Link& link : links_) {
    const auto a = static_cast<std::size_t>(std::lower_bound(
                       resources_.begin(), resources_.end(), link.endpoint_a,
                       [](const Resource& entry, ResourceId id) { return entry.id < id; }) -
                   resources_.begin());
    const auto b = static_cast<std::size_t>(std::lower_bound(
                       resources_.begin(), resources_.end(), link.endpoint_b,
                       [](const Resource& entry, ResourceId id) { return entry.id < id; }) -
                   resources_.begin());
    if (++degree[a] > Limits::kMaxNeighborsPerResource ||
        ++degree[b] > Limits::kMaxNeighborsPerResource) {
      return Status::failure(ErrorCode::BoundExceeded, "resource degree exceeds limit");
    }
  }
  neighbor_offsets_.assign(n + 1, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    neighbor_offsets_[i + 1] = neighbor_offsets_[i] + degree[i];
  }
  neighbor_ids_.assign(neighbor_offsets_[n], ResourceId{});
  std::vector<std::uint32_t> cursor(neighbor_offsets_.begin(), neighbor_offsets_.end() - 1);
  for (const Link& link : links_) {
    const auto a = static_cast<std::size_t>(std::lower_bound(
                       resources_.begin(), resources_.end(), link.endpoint_a,
                       [](const Resource& entry, ResourceId id) { return entry.id < id; }) -
                   resources_.begin());
    const auto b = static_cast<std::size_t>(std::lower_bound(
                       resources_.begin(), resources_.end(), link.endpoint_b,
                       [](const Resource& entry, ResourceId id) { return entry.id < id; }) -
                   resources_.begin());
    neighbor_ids_[cursor[a]++] = link.endpoint_b;
    neighbor_ids_[cursor[b]++] = link.endpoint_a;
  }
  for (std::size_t i = 0; i < n; ++i) {
    auto begin = neighbor_ids_.begin() + neighbor_offsets_[i];
    auto end = neighbor_ids_.begin() + neighbor_offsets_[i + 1];
    std::sort(begin, end);
    end = std::unique(begin, end);
    neighbor_offsets_[i + 1] = static_cast<std::uint32_t>(static_cast<std::size_t>(end - neighbor_ids_.begin()));
  }
  // Compact after unique.
  {
    std::vector<ResourceId> compact;
    compact.reserve(neighbor_ids_.size());
    std::vector<std::uint32_t> new_offsets(n + 1, 0u);
    for (std::size_t i = 0; i < n; ++i) {
      for (std::uint32_t k = neighbor_offsets_[i]; k < neighbor_offsets_[i + 1]; ++k) {
        compact.push_back(neighbor_ids_[k]);
      }
      new_offsets[i + 1] = static_cast<std::uint32_t>(compact.size());
    }
    neighbor_ids_.swap(compact);
    neighbor_offsets_.swap(new_offsets);
  }

  // --- region chains CSR ---
  chain_offsets_.assign(n + 1, 0u);
  region_index_by_resource_.assign(n, 0u);
  std::vector<RegionId> chain_scratch;
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t steps = 0;
    RegionId cursor_id = resources_[i].region;
    while (cursor_id.valid() && steps <= regions_.size()) {
      chain_scratch.push_back(cursor_id);
      const Region* entry = find_region(cursor_id);
      if (entry == nullptr) break;
      cursor_id = entry->parent;
      ++steps;
    }
    chain_offsets_[i + 1] = static_cast<std::uint32_t>(chain_scratch.size());
  }
  chain_ids_.swap(chain_scratch);
  for (std::size_t i = 0; i < n; ++i) {
    region_index_by_resource_[i] = static_cast<std::uint32_t>(
        std::lower_bound(regions_.begin(), regions_.end(), resources_[i].region,
                         [](const Region& entry, RegionId id) { return entry.id < id; }) -
        regions_.begin());
  }

  return Status::success();
}

const Resource* TopologySnapshot::find_resource(ResourceId id) const {
  if (!id.valid()) return nullptr;
  const auto it = std::lower_bound(resources_.begin(), resources_.end(), id,
                                   [](const Resource& entry, ResourceId key) { return entry.id < key; });
  if (it == resources_.end() || it->id != id) return nullptr;
  return &(*it);
}

const Region* TopologySnapshot::find_region(RegionId id) const {
  if (!id.valid()) return nullptr;
  const auto it = std::lower_bound(regions_.begin(), regions_.end(), id,
                                   [](const Region& entry, RegionId key) { return entry.id < key; });
  if (it == regions_.end() || it->id != id) return nullptr;
  return &(*it);
}

bool TopologySnapshot::has_region(RegionId id) const { return find_region(id) != nullptr; }

std::span<const ResourceId> TopologySnapshot::neighbors(ResourceId id) const {
  if (!built_ || !id.valid()) return {};
  const auto it = std::lower_bound(resources_.begin(), resources_.end(), id,
                                   [](const Resource& entry, ResourceId key) { return entry.id < key; });
  if (it == resources_.end() || it->id != id) return {};
  const auto index = static_cast<std::size_t>(it - resources_.begin());
  const std::uint32_t begin = neighbor_offsets_[index];
  const std::uint32_t end = neighbor_offsets_[index + 1];
  return std::span<const ResourceId>(neighbor_ids_.data() + begin, static_cast<std::size_t>(end - begin));
}

std::span<const RegionId> TopologySnapshot::region_chain(ResourceId id) const {
  if (!built_ || !id.valid()) return {};
  const auto it = std::lower_bound(resources_.begin(), resources_.end(), id,
                                   [](const Resource& entry, ResourceId key) { return entry.id < key; });
  if (it == resources_.end() || it->id != id) return {};
  const auto index = static_cast<std::size_t>(it - resources_.begin());
  const std::uint32_t begin = chain_offsets_[index];
  const std::uint32_t end = chain_offsets_[index + 1];
  return std::span<const RegionId>(chain_ids_.data() + begin, static_cast<std::size_t>(end - begin));
}

bool TopologySnapshot::same_region(ResourceId a, ResourceId b) const {
  const Resource* ra = find_resource(a);
  const Resource* rb = find_resource(b);
  if (ra == nullptr || rb == nullptr) return false;
  return ra->region == rb->region;
}

}  // namespace hgm
