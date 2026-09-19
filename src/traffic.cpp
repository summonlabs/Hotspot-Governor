// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/traffic.hpp"

#include <algorithm>

#include "hgm/byteio.hpp"
#include "hgm/limits.hpp"

namespace hgm {

TrafficRecord& TrafficSnapshot::add(FlowId flow, PathId path) {
  records_.push_back(TrafficRecord{flow, path, 0, TelemetryQuality::Unknown, kNoTime});
  built_ = false;
  return records_.back();
}

Status TrafficSnapshot::build() {
  built_ = false;
  id_ = TrafficId{};
  digest_ = Digest{};

  if (records_.size() > Limits::kMaxTrafficRecords) {
    return Status::failure(ErrorCode::BoundExceeded, "traffic record count exceeds limit");
  }

  std::sort(records_.begin(), records_.end(), [](const TrafficRecord& a, const TrafficRecord& b) {
    if (a.path != b.path) return a.path < b.path;
    return a.flow < b.flow;
  });
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const TrafficRecord& record = records_[i];
    if (!record.path.valid()) {
      return Status::failure(ErrorCode::InvalidArgument, "traffic record names path id 0");
    }
    if (i > 0 && record.path == records_[i - 1].path && record.flow == records_[i - 1].flow) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "duplicate traffic record for path " + record.path.hex());
    }
  }

  std::vector<std::byte> encoded;
  encoded.reserve(records_.size() * 32);
  ByteWriter writer(encoded);
  writer.u32(static_cast<std::uint32_t>(records_.size()));
  for (const TrafficRecord& record : records_) {
    writer.u64(record.flow.value());
    writer.u64(record.path.value());
    writer.u64(record.demand_bps);
    writer.u8(static_cast<std::uint8_t>(record.quality));
    writer.i64(record.observed_at);
  }
  digest_ = digest_of(std::span<const std::byte>(encoded.data(), encoded.size()));
  id_ = TrafficId::from(digest_.fnv);
  built_ = true;
  return Status::success();
}

}  // namespace hgm
