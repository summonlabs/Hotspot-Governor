// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/capacity.hpp"

#include <algorithm>

#include "hgm/byteio.hpp"
#include "hgm/limits.hpp"

namespace hgm {

ResourceCapacity& CapacitySnapshot::add(ResourceId resource) {
  entries_.push_back(ResourceCapacity{resource, 0, 0, 0, true});
  built_ = false;
  return entries_.back();
}

Status CapacitySnapshot::build() {
  built_ = false;
  id_ = CapacityId{};
  digest_ = Digest{};

  if (entries_.size() > Limits::kMaxCapacityEntries) {
    return Status::failure(ErrorCode::BoundExceeded, "capacity entry count exceeds limit");
  }

  std::sort(entries_.begin(), entries_.end(),
            [](const ResourceCapacity& a, const ResourceCapacity& b) { return a.resource < b.resource; });
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].resource.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "capacity names resource id 0");
    }
    if (i > 0 && entries_[i].resource == entries_[i - 1].resource) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "duplicate capacity entry for " + entries_[i].resource.hex());
    }
    if (entries_[i].capacity_units == 0 && entries_[i].usable) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "usable capacity must be positive for " + entries_[i].resource.hex());
    }
  }

  std::vector<std::byte> encoded;
  encoded.reserve(entries_.size() * 32);
  ByteWriter writer(encoded);
  writer.u32(static_cast<std::uint32_t>(entries_.size()));
  for (const ResourceCapacity& entry : entries_) {
    writer.u64(entry.resource.value());
    writer.u64(entry.capacity_units);
    writer.u64(entry.queue_limit_units);
    writer.u64(entry.buffer_limit_bytes);
    writer.u8(entry.usable ? 1u : 0u);
  }
  digest_ = digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
  id_ = CapacityId::from(digest_.fnv);
  built_ = true;
  return Status::success();
}

const ResourceCapacity* CapacitySnapshot::find(ResourceId id) const {
  if (!built_ || !id.valid()) return nullptr;
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                                   [](const ResourceCapacity& entry, ResourceId key) {
                                     return entry.resource < key;
                                   });
  if (it == entries_.end() || it->resource != id) return nullptr;
  return &(*it);
}

}  // namespace hgm
