// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/paths.hpp"

#include <algorithm>
#include <numeric>

#include "hgm/byteio.hpp"
#include "hgm/limits.hpp"

namespace hgm {

PathRecord& PathSnapshot::add(PathId id, FlowId flow) {
  paths_.push_back(PathRecord{id, flow, {}});
  built_ = false;
  return paths_.back();
}

Status PathSnapshot::build() {
  built_ = false;
  id_ = PathSetId{};
  digest_ = Digest{};

  if (paths_.size() > Limits::kMaxPaths) {
    return Status::failure(ErrorCode::BoundExceeded, "path count exceeds limit");
  }

  std::sort(paths_.begin(), paths_.end(),
            [](const PathRecord& a, const PathRecord& b) { return a.id < b.id; });
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    const PathRecord& path = paths_[i];
    if (!path.id.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "path id 0 is reserved");
    }
    if (i > 0 && path.id == paths_[i - 1].id) {
      return Status::failure(ErrorCode::InvalidArgument, "duplicate path id " + path.id.hex());
    }
    if (path.hops.empty()) {
      return Status::failure(ErrorCode::InvalidArgument, "path " + path.id.hex() + " has no hops");
    }
    if (path.hops.size() > Limits::kMaxHopsPerPath) {
      return Status::failure(ErrorCode::BoundExceeded, "path hop count exceeds limit");
    }
    for (const ResourceId hop : path.hops) {
      if (!hop.valid()) {
        return Status::failure(ErrorCode::InvalidArgument, "path " + path.id.hex() + " names resource 0");
      }
    }
  }

  // --- reverse index: resource -> path indices, CSR ---
  std::vector<ResourceId> listed;
  listed.reserve(paths_.size() * 4);
  for (const PathRecord& path : paths_) {
    listed.insert(listed.end(), path.hops.begin(), path.hops.end());
  }
  std::sort(listed.begin(), listed.end());
  listed.erase(std::unique(listed.begin(), listed.end()), listed.end());
  through_offsets_.assign(listed.size() + 1, 0u);
  through_resources_ = listed;

  std::vector<std::uint32_t> counts(listed.size(), 0u);
  for (const PathRecord& path : paths_) {
    std::vector<ResourceId> unique_hops = path.hops;
    std::sort(unique_hops.begin(), unique_hops.end());
    unique_hops.erase(std::unique(unique_hops.begin(), unique_hops.end()), unique_hops.end());
    for (const ResourceId hop : unique_hops) {
      const auto index = static_cast<std::size_t>(
          std::lower_bound(listed.begin(), listed.end(), hop) - listed.begin());
      ++counts[index];
    }
  }
  for (std::size_t i = 0; i < listed.size(); ++i) {
    through_offsets_[i + 1] = through_offsets_[i] + counts[i];
  }
  through_paths_.assign(through_offsets_.back(), 0u);
  std::vector<std::uint32_t> cursor(through_offsets_.begin(), through_offsets_.end() - 1);
  for (std::uint32_t p = 0; p < paths_.size(); ++p) {
    std::vector<ResourceId> unique_hops = paths_[p].hops;
    std::sort(unique_hops.begin(), unique_hops.end());
    unique_hops.erase(std::unique(unique_hops.begin(), unique_hops.end()), unique_hops.end());
    for (const ResourceId hop : unique_hops) {
      const auto index = static_cast<std::size_t>(
          std::lower_bound(listed.begin(), listed.end(), hop) - listed.begin());
      through_paths_[cursor[index]++] = p;
    }
  }
  for (std::size_t i = 0; i < listed.size(); ++i) {
    auto begin = through_paths_.begin() + through_offsets_[i];
    auto end = through_paths_.begin() + through_offsets_[i + 1];
    std::sort(begin, end);
  }

  std::vector<std::byte> encoded;
  encoded.reserve(paths_.size() * 32);
  ByteWriter writer(encoded);
  writer.u32(static_cast<std::uint32_t>(paths_.size()));
  for (const PathRecord& path : paths_) {
    writer.u64(path.id.value());
    writer.u64(path.flow.value());
    writer.u32(static_cast<std::uint32_t>(path.hops.size()));
    for (const ResourceId hop : path.hops) writer.u64(hop.value());
  }
  digest_ = digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
  id_ = PathSetId::from(digest_.fnv);
  built_ = true;
  return Status::success();
}

const PathRecord* PathSnapshot::find(PathId id) const {
  if (!built_ || !id.valid()) return nullptr;
  const auto it = std::lower_bound(paths_.begin(), paths_.end(), id,
                                   [](const PathRecord& entry, PathId key) { return entry.id < key; });
  if (it == paths_.end() || it->id != id) return nullptr;
  return &(*it);
}

std::span<const std::uint32_t> PathSnapshot::paths_through(ResourceId resource) const {
  if (!built_ || !resource.valid()) return {};
  const auto it = std::lower_bound(through_resources_.begin(), through_resources_.end(), resource);
  if (it == through_resources_.end() || *it != resource) return {};
  const auto index = static_cast<std::size_t>(it - through_resources_.begin());
  const std::uint32_t begin = through_offsets_[index];
  const std::uint32_t end = through_offsets_[index + 1];
  return std::span<const std::uint32_t>(through_paths_.data() + begin,
                                        static_cast<std::size_t>(end - begin));
}

}  // namespace hgm
