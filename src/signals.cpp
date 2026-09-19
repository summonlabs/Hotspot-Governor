// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/signals.hpp"

#include <algorithm>

#include "hgm/byteio.hpp"
#include "hgm/checked.hpp"
#include "hgm/limits.hpp"

namespace hgm {

ResourceSignals& SignalSnapshot::add(ResourceId resource) {
  records_.push_back(ResourceSignals{});
  records_.back().resource = resource;
  built_ = false;
  return records_.back();
}

Status SignalSnapshot::build() {
  built_ = false;
  id_ = EvidenceId{};
  digest_ = Digest{};

  if (records_.size() > Limits::kMaxSignalRecords) {
    return Status::failure(ErrorCode::BoundExceeded, "signal record count exceeds limit");
  }

  std::sort(records_.begin(), records_.end(),
            [](const ResourceSignals& a, const ResourceSignals& b) { return a.resource < b.resource; });
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const ResourceSignals& record = records_[i];
    if (!record.resource.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "signal names resource id 0");
    }
    if (i > 0 && record.resource == records_[i - 1].resource) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "duplicate signal record for " + record.resource.hex());
    }
    if (record.confidence_ppm > kPpmScale) {
      return Status::failure(ErrorCode::OutOfRange,
                             "confidence above 1e6 for " + record.resource.hex());
    }
    if (record.admitted_bps > record.offered_bps && record.offered_bps != 0) {
      // admitted > offered is a physically impossible sample unless the
      // publisher declared zero offered load; treat the pair as unusable.
      return Status::failure(ErrorCode::ContradictoryEvidence,
                             "admitted exceeds offered for " + record.resource.hex());
    }
  }

  std::vector<std::byte> encoded;
  encoded.reserve(records_.size() * 64);
  ByteWriter writer(encoded);
  writer.u32(static_cast<std::uint32_t>(records_.size()));
  for (const ResourceSignals& record : records_) {
    writer.u64(record.resource.value());
    writer.u64(record.utilized_units);
    writer.u64(record.queue_depth_units);
    writer.u64(record.buffer_used_bytes);
    writer.u64(record.offered_bps);
    writer.u64(record.admitted_bps);
    writer.u64(record.drop_units);
    writer.u32(record.confidence_ppm);
    writer.u8(static_cast<std::uint8_t>(record.quality));
    writer.i64(record.observed_at);
  }
  digest_ = digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
  id_ = EvidenceId::from(digest_.fnv);
  built_ = true;
  return Status::success();
}

const ResourceSignals* SignalSnapshot::find(ResourceId id) const {
  if (!built_ || !id.valid()) return nullptr;
  const auto it = std::lower_bound(records_.begin(), records_.end(), id,
                                   [](const ResourceSignals& entry, ResourceId key) {
                                     return entry.resource < key;
                                   });
  if (it == records_.end() || it->resource != id) return nullptr;
  return &(*it);
}

}  // namespace hgm
