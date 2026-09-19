// Wire framing codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hgm/byteio.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

namespace hgm {
namespace {

void encode_header_fields(const FrameHeader& header, std::vector<std::byte>& out) {
  ByteWriter writer(out);
  writer.u32(kFrameMagic);
  writer.u16(header.version);
  writer.u16(static_cast<std::uint16_t>(header.type));
  writer.u32(header.flags);
  writer.u32(header.payload_length);
  writer.u64(header.epoch.value());
  writer.u64(header.publisher.value());
  writer.u64(header.incarnation.value());
  writer.u64(header.boot.value());
  writer.u64(header.sequence);
  writer.u64(header.generation);
  writer.i64(header.emitted_at);
  writer.u64(header.payload_crc);
}

Status encode_stream_header(StreamKind stream, std::uint64_t generation, std::int64_t observed_at,
                            std::vector<std::byte>& out) {
  ByteWriter writer(out);
  writer.u16(static_cast<std::uint16_t>(stream));
  writer.u64(generation);
  writer.i64(observed_at);
  return Status::success();
}

struct StreamHeader {
  StreamKind stream = StreamKind::Unknown;
  std::uint64_t generation = 0;
  Millis observed_at = kNoTime;
};

Status decode_stream_header(ByteReader& reader, StreamKind expected, StreamHeader& out) {
  std::uint16_t stream = 0;
  if (!reader.u16(stream)) return Status::failure(ErrorCode::TruncatedFrame, "stream header truncated");
  if (static_cast<StreamKind>(stream) != expected) {
    return Status::failure(ErrorCode::MalformedFrame, "payload stream kind does not match the message type");
  }
  out.stream = expected;
  if (!reader.u64(out.generation)) {
    return Status::failure(ErrorCode::TruncatedFrame, "stream generation truncated");
  }
  if (!reader.i64(out.observed_at)) {
    return Status::failure(ErrorCode::TruncatedFrame, "stream timestamp truncated");
  }
  return Status::success();
}

Status require_end(const ByteReader& reader) {
  if (!reader.at_end()) {
    return Status::failure(ErrorCode::MalformedFrame, "payload has trailing bytes");
  }
  return Status::success();
}

Status check_count(std::uint32_t count, std::size_t limit, const char* what) {
  if (static_cast<std::size_t>(count) > limit) {
    return Status::failure(ErrorCode::BoundExceeded, std::string(what) + " count exceeds limit");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Unknown: return "unknown";
    case MessageType::Hello: return "hello";
    case MessageType::Welcome: return "welcome";
    case MessageType::Rejected: return "rejected";
    case MessageType::Accepted: return "accepted";
    case MessageType::Goodbye: return "goodbye";
    case MessageType::Topology: return "topology";
    case MessageType::Capacity: return "capacity";
    case MessageType::Signals: return "signals";
    case MessageType::Paths: return "paths";
    case MessageType::Traffic: return "traffic";
    case MessageType::Policy: return "policy";
    case MessageType::Heartbeat: return "heartbeat";
  }
  return "unknown";
}

StreamKind stream_of(MessageType type) noexcept {
  switch (type) {
    case MessageType::Topology: return StreamKind::Topology;
    case MessageType::Capacity: return StreamKind::Capacity;
    case MessageType::Signals: return StreamKind::Signals;
    case MessageType::Paths: return StreamKind::Paths;
    case MessageType::Traffic: return StreamKind::Traffic;
    case MessageType::Policy: return StreamKind::Policy;
    default: return StreamKind::Unknown;
  }
}

void encode_header(const FrameHeader& header, std::vector<std::byte>& out) {
  out.clear();
  out.reserve(kFrameHeaderBytes);
  encode_header_fields(header, out);
  // The header CRC covers everything that precedes it.
  const std::uint64_t crc = crc64(std::span<const std::byte>(out.data(), out.size()));
  ByteWriter tail(out);
  tail.u64(crc);
  static_assert(kFrameHeaderBytes == 4 + 2 + 2 + 4 + 4 + (8 * 9),
                "the fixed frame header size must match the encoded field layout");
}

Result<FrameHeader> decode_header(std::span<const std::byte> bytes) {
  if (bytes.size() < kFrameHeaderBytes) {
    return Status::failure(ErrorCode::TruncatedFrame, "fewer header bytes available than the fixed frame header");
  }
  const std::uint64_t expected_crc = crc64(bytes.first(kFrameHeaderBytes - 8));
  ByteReader reader(bytes.first(kFrameHeaderBytes));
  FrameHeader header;
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(type) || !reader.u32(header.flags) ||
      !reader.u32(header.payload_length)) {
    return Status::failure(ErrorCode::MalformedFrame, "header fields could not be decoded");
  }
  if (magic != kFrameMagic) {
    return Status::failure(ErrorCode::MalformedFrame, "bad frame magic");
  }
  if (version != kFrameProtocolVersion) {
    return Status::failure(ErrorCode::UnsupportedVersion,
                           "frame protocol version " + std::to_string(version) + " is not supported");
  }
  if (static_cast<std::size_t>(header.payload_length) > Limits::kMaxFrameBytes) {
    return Status::failure(ErrorCode::FrameTooLarge, "declared payload exceeds the frame bound");
  }
  std::uint64_t epoch = 0;
  std::uint64_t publisher = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t boot = 0;
  if (!reader.u64(epoch) || !reader.u64(publisher) || !reader.u64(incarnation) ||
      !reader.u64(boot) || !reader.u64(header.sequence) || !reader.u64(header.generation) ||
      !reader.i64(header.emitted_at) || !reader.u64(header.payload_crc) ||
      !reader.u64(header.header_crc)) {
    return Status::failure(ErrorCode::MalformedFrame, "header fields could not be decoded");
  }
  if (header.header_crc != expected_crc) {
    return Status::failure(ErrorCode::IntegrityMismatch, "header CRC does not match the header bytes");
  }
  header.version = version;
  header.type = static_cast<MessageType>(type);
  header.epoch = CoordinatorEpoch{epoch};
  header.publisher = PublisherId{publisher};
  header.incarnation = Incarnation{incarnation};
  header.boot = BootId{boot};
  return header;
}

Status verify_payload(const FrameHeader& header, std::span<const std::byte> payload) {
  if (payload.size() != static_cast<std::size_t>(header.payload_length)) {
    return Status::failure(ErrorCode::TruncatedFrame, "payload length does not match the header");
  }
  if (payload.size() > Limits::kMaxFrameBytes) {
    return Status::failure(ErrorCode::FrameTooLarge, "payload exceeds the frame bound");
  }
  if (crc64(payload) != header.payload_crc) {
    return Status::failure(ErrorCode::IntegrityMismatch, "payload CRC does not match the payload bytes");
  }
  return Status::success();
}

Status encode_frame(const Frame& frame, std::vector<std::byte>& out) {
  FrameHeader header = frame.header;
  if (frame.payload.size() > Limits::kMaxFrameBytes) {
    return Status::failure(ErrorCode::FrameTooLarge, "payload exceeds the frame bound");
  }
  header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
  header.payload_crc = crc64(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  if (header.version == 0) header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);

  out.clear();
  out.reserve(kFrameHeaderBytes + frame.payload.size());
  encode_header(header, out);
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return Status::success();
}

Frame make_frame(MessageType type, const Provenance& provenance, std::vector<std::byte> payload) {
  Frame frame;
  frame.header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  frame.header.type = type;
  frame.header.epoch = provenance.epoch;
  frame.header.publisher = provenance.publisher;
  frame.header.incarnation = provenance.incarnation;
  frame.header.boot = provenance.boot;
  frame.header.sequence = provenance.sequence;
  frame.header.generation = provenance.generation.value;
  frame.header.emitted_at = provenance.emitted_at;
  frame.header.payload_length = static_cast<std::uint32_t>(payload.size());
  // Stamp the payload integrity here as well as on encode, so an in-memory
  // frame is exactly as verifiable as one that arrived on the wire.
  frame.header.payload_crc = crc64(std::span<const std::byte>(payload.data(), payload.size()));
  frame.payload = std::move(payload);
  return frame;
}

// --- payload codecs ---------------------------------------------------------

Status encode_topology_payload(const TopologySnapshot& snapshot, std::vector<std::byte>& out) {
  if (!snapshot.built()) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode an unbuilt topology");
  }
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Topology, snapshot.generation().value(), snapshot.valid_from(), out);
  writer.i64(snapshot.valid_until());
  writer.u32(static_cast<std::uint32_t>(snapshot.regions().size()));
  for (const Region& region : snapshot.regions()) {
    writer.u64(region.id.value());
    writer.u64(region.parent.value());
    writer.text_field(region.name);
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.resources().size()));
  for (const Resource& resource : snapshot.resources()) {
    writer.u64(resource.id.value());
    writer.u64(resource.region.value());
    writer.u8(static_cast<std::uint8_t>(resource.kind));
    writer.text_field(resource.name);
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.links().size()));
  for (const Link& link : snapshot.links()) {
    writer.u64(link.id.value());
    writer.u64(link.endpoint_a.value());
    writer.u64(link.endpoint_b.value());
  }
  return Status::success();
}

Status decode_topology_payload(std::span<const std::byte> payload, TopologySnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Topology, header);
  if (!status.ok()) return status;

  TopologySnapshot snapshot;
  Millis valid_until = 0;
  if (!reader.i64(valid_until)) {
    return Status::failure(ErrorCode::TruncatedFrame, "topology validity truncated");
  }
  snapshot.set_validity(header.observed_at, valid_until);

  std::uint32_t region_count = 0;
  if (!reader.u32(region_count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "topology region count truncated");
  }
  status = check_count(region_count, Limits::kMaxRegions, "region");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < region_count; ++i) {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::string name;
    if (!reader.u64(id) || !reader.u64(parent) ||
        !reader.text_field(Limits::kMaxRegionNameBytes, name)) {
      return Status::failure(ErrorCode::TruncatedFrame, "topology region entry truncated");
    }
    snapshot.add_region(RegionId{id}, std::move(name), RegionId{parent});
  }

  std::uint32_t resource_count = 0;
  if (!reader.u32(resource_count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "topology resource count truncated");
  }
  status = check_count(resource_count, Limits::kMaxResources, "resource");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < resource_count; ++i) {
    std::uint64_t id = 0;
    std::uint64_t region = 0;
    std::uint8_t kind = 0;
    std::string name;
    if (!reader.u64(id) || !reader.u64(region) || !reader.u8(kind) ||
        !reader.text_field(Limits::kMaxResourceNameBytes, name)) {
      return Status::failure(ErrorCode::TruncatedFrame, "topology resource entry truncated");
    }
    snapshot.add_resource(ResourceId{id}, RegionId{region}, static_cast<ResourceKind>(kind),
                          std::move(name));
  }

  std::uint32_t link_count = 0;
  if (!reader.u32(link_count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "topology link count truncated");
  }
  status = check_count(link_count, Limits::kMaxLinks, "link");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < link_count; ++i) {
    std::uint64_t id = 0;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    if (!reader.u64(id) || !reader.u64(a) || !reader.u64(b)) {
      return Status::failure(ErrorCode::TruncatedFrame, "topology link entry truncated");
    }
    snapshot.add_link(LinkId{id}, ResourceId{a}, ResourceId{b});
  }

  status = require_end(reader);
  if (!status.ok()) return status;
  status = snapshot.build();
  if (!status.ok()) return status;
  snapshot.set_generation(TopologyGeneration::from(header.generation));
  out = std::move(snapshot);
  return Status::success();
}

Status encode_capacity_payload(const CapacitySnapshot& snapshot, std::vector<std::byte>& out) {
  if (!snapshot.built()) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode an unbuilt capacity snapshot");
  }
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Capacity, snapshot.generation().value(), snapshot.observed_at(), out);
  writer.u64(snapshot.topology_generation().value());
  writer.u32(static_cast<std::uint32_t>(snapshot.entries().size()));
  for (const ResourceCapacity& entry : snapshot.entries()) {
    writer.u64(entry.resource.value());
    writer.u64(entry.capacity_units);
    writer.u64(entry.queue_limit_units);
    writer.u64(entry.buffer_limit_bytes);
    writer.u8(entry.usable ? 1u : 0u);
  }
  return Status::success();
}

Status decode_capacity_payload(std::span<const std::byte> payload, CapacitySnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Capacity, header);
  if (!status.ok()) return status;

  CapacitySnapshot snapshot;
  std::uint64_t topology_generation = 0;
  if (!reader.u64(topology_generation)) {
    return Status::failure(ErrorCode::TruncatedFrame, "capacity topology generation truncated");
  }
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "capacity entry count truncated");
  }
  status = check_count(count, Limits::kMaxCapacityEntries, "capacity entry");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t resource = 0;
    std::uint64_t capacity = 0;
    std::uint64_t queue_limit = 0;
    std::uint64_t buffer_limit = 0;
    std::uint8_t usable = 0;
    if (!reader.u64(resource) || !reader.u64(capacity) || !reader.u64(queue_limit) ||
        !reader.u64(buffer_limit) || !reader.u8(usable)) {
      return Status::failure(ErrorCode::TruncatedFrame, "capacity entry truncated");
    }
    ResourceCapacity& entry = snapshot.add(ResourceId{resource});
    entry.capacity_units = capacity;
    entry.queue_limit_units = queue_limit;
    entry.buffer_limit_bytes = buffer_limit;
    entry.usable = usable != 0;
  }
  status = require_end(reader);
  if (!status.ok()) return status;
  status = snapshot.build();
  if (!status.ok()) return status;
  snapshot.set_generation(CapacityGeneration::from(header.generation));
  snapshot.set_topology_generation(TopologyGeneration::from(topology_generation));
  snapshot.set_observed_at(header.observed_at);
  out = std::move(snapshot);
  return Status::success();
}

Status encode_signals_payload(const SignalSnapshot& snapshot, std::vector<std::byte>& out) {
  if (!snapshot.built()) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode an unbuilt signal snapshot");
  }
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Signals, snapshot.generation().value(), snapshot.window_end(), out);
  writer.u64(snapshot.topology_generation().value());
  writer.u64(snapshot.capacity_generation().value());
  writer.i64(snapshot.window_start());
  writer.u32(static_cast<std::uint32_t>(snapshot.records().size()));
  for (const ResourceSignals& record : snapshot.records()) {
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
  return Status::success();
}

Status decode_signals_payload(std::span<const std::byte> payload, SignalSnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Signals, header);
  if (!status.ok()) return status;

  SignalSnapshot snapshot;
  std::uint64_t topology_generation = 0;
  std::uint64_t capacity_generation = 0;
  Millis window_start = kNoTime;
  if (!reader.u64(topology_generation) || !reader.u64(capacity_generation) ||
      !reader.i64(window_start)) {
    return Status::failure(ErrorCode::TruncatedFrame, "signal header truncated");
  }
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "signal record count truncated");
  }
  status = check_count(count, Limits::kMaxSignalRecords, "signal record");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t resource = 0;
    ResourceSignals record;
    std::uint8_t quality = 0;
    if (!reader.u64(resource) || !reader.u64(record.utilized_units) ||
        !reader.u64(record.queue_depth_units) || !reader.u64(record.buffer_used_bytes) ||
        !reader.u64(record.offered_bps) || !reader.u64(record.admitted_bps) ||
        !reader.u64(record.drop_units) || !reader.u32(record.confidence_ppm) ||
        !reader.u8(quality) || !reader.i64(record.observed_at)) {
      return Status::failure(ErrorCode::TruncatedFrame, "signal record truncated");
    }
    record.resource = ResourceId{resource};
    record.quality = static_cast<TelemetryQuality>(quality);
    snapshot.add(record.resource) = record;
  }
  status = require_end(reader);
  if (!status.ok()) return status;
  status = snapshot.build();
  if (!status.ok()) return status;
  snapshot.set_generation(EvidenceGeneration::from(header.generation));
  snapshot.set_topology_generation(TopologyGeneration::from(topology_generation));
  snapshot.set_capacity_generation(CapacityGeneration::from(capacity_generation));
  snapshot.set_window(window_start, header.observed_at);
  out = std::move(snapshot);
  return Status::success();
}

Status encode_paths_payload(const PathSnapshot& snapshot, std::vector<std::byte>& out) {
  if (!snapshot.built()) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode an unbuilt path snapshot");
  }
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Paths, snapshot.generation().value(), snapshot.observed_at(), out);
  writer.u64(snapshot.topology_generation().value());
  writer.u32(static_cast<std::uint32_t>(snapshot.paths().size()));
  for (const PathRecord& path : snapshot.paths()) {
    writer.u64(path.id.value());
    writer.u64(path.flow.value());
    writer.u32(static_cast<std::uint32_t>(path.hops.size()));
    for (const ResourceId hop : path.hops) writer.u64(hop.value());
  }
  return Status::success();
}

Status decode_paths_payload(std::span<const std::byte> payload, PathSnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Paths, header);
  if (!status.ok()) return status;

  PathSnapshot snapshot;
  std::uint64_t topology_generation = 0;
  if (!reader.u64(topology_generation)) {
    return Status::failure(ErrorCode::TruncatedFrame, "path topology generation truncated");
  }
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "path count truncated");
  }
  status = check_count(count, Limits::kMaxPaths, "path");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t id = 0;
    std::uint64_t flow = 0;
    std::uint32_t hops = 0;
    if (!reader.u64(id) || !reader.u64(flow) || !reader.u32(hops)) {
      return Status::failure(ErrorCode::TruncatedFrame, "path entry truncated");
    }
    if (static_cast<std::size_t>(hops) > Limits::kMaxHopsPerPath) {
      return Status::failure(ErrorCode::BoundExceeded, "path hop count exceeds limit");
    }
    PathRecord& record = snapshot.add(PathId{id}, FlowId{flow});
    record.hops.reserve(hops);
    for (std::uint32_t hop = 0; hop < hops; ++hop) {
      std::uint64_t resource = 0;
      if (!reader.u64(resource)) {
        return Status::failure(ErrorCode::TruncatedFrame, "path hop truncated");
      }
      record.hops.push_back(ResourceId{resource});
    }
  }
  status = require_end(reader);
  if (!status.ok()) return status;
  status = snapshot.build();
  if (!status.ok()) return status;
  snapshot.set_generation(PathGeneration::from(header.generation));
  snapshot.set_topology_generation(TopologyGeneration::from(topology_generation));
  snapshot.set_observed_at(header.observed_at);
  out = std::move(snapshot);
  return Status::success();
}

Status encode_traffic_payload(const TrafficSnapshot& snapshot, std::vector<std::byte>& out) {
  if (!snapshot.built()) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode an unbuilt traffic snapshot");
  }
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Traffic, snapshot.generation().value(), kNoTime, out);
  writer.u64(snapshot.path_generation().value());
  writer.u32(static_cast<std::uint32_t>(snapshot.records().size()));
  for (const TrafficRecord& record : snapshot.records()) {
    writer.u64(record.flow.value());
    writer.u64(record.path.value());
    writer.u64(record.demand_bps);
    writer.u8(static_cast<std::uint8_t>(record.quality));
    writer.i64(record.observed_at);
  }
  return Status::success();
}

Status decode_traffic_payload(std::span<const std::byte> payload, TrafficSnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Traffic, header);
  if (!status.ok()) return status;

  TrafficSnapshot snapshot;
  std::uint64_t path_generation = 0;
  if (!reader.u64(path_generation)) {
    return Status::failure(ErrorCode::TruncatedFrame, "traffic path generation truncated");
  }
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return Status::failure(ErrorCode::TruncatedFrame, "traffic record count truncated");
  }
  status = check_count(count, Limits::kMaxTrafficRecords, "traffic record");
  if (!status.ok()) return status;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t flow = 0;
    std::uint64_t path = 0;
    std::uint64_t demand = 0;
    std::uint8_t quality = 0;
    Millis observed_at = kNoTime;
    if (!reader.u64(flow) || !reader.u64(path) || !reader.u64(demand) || !reader.u8(quality) ||
        !reader.i64(observed_at)) {
      return Status::failure(ErrorCode::TruncatedFrame, "traffic record truncated");
    }
    TrafficRecord& record = snapshot.add(FlowId{flow}, PathId{path});
    record.demand_bps = demand;
    record.quality = static_cast<TelemetryQuality>(quality);
    record.observed_at = observed_at;
  }
  status = require_end(reader);
  if (!status.ok()) return status;
  status = snapshot.build();
  if (!status.ok()) return status;
  snapshot.set_generation(TrafficGeneration::from(header.generation));
  snapshot.set_path_generation(PathGeneration::from(path_generation));
  out = std::move(snapshot);
  return Status::success();
}

Status encode_policy_payload(const PolicySnapshot& snapshot, std::vector<std::byte>& out) {
  out.clear();
  ByteWriter writer(out);
  encode_stream_header(StreamKind::Policy, snapshot.generation().value(), snapshot.issued_at(), out);
  writer.u64(snapshot.id().value());
  writer.i64(snapshot.issued_at());
  writer.u32(snapshot.ttl_ms());
  const Thresholds& thresholds = snapshot.thresholds();
  writer.u32(thresholds.saturation_utilization_ppm);
  writer.u32(thresholds.queue_pressure_ppm);
  writer.u32(thresholds.buffer_pressure_ppm);
  writer.u32(thresholds.min_confidence_ppm);
  writer.u32(thresholds.max_signal_age_ms);
  writer.u32(thresholds.max_capacity_age_ms);
  writer.u32(thresholds.max_topology_age_ms);
  writer.u32(thresholds.max_path_age_ms);
  writer.u32(thresholds.max_traffic_age_ms);
  writer.u32(thresholds.persistence_window_ms);
  writer.u32(thresholds.min_persistence_samples);
  writer.u32(thresholds.max_local_share_ppm);
  writer.u32(thresholds.global_share_ppm);
  writer.u32(thresholds.max_local_regions);
  writer.u32(thresholds.max_regional_regions);
  writer.u32(thresholds.min_coverage_ppm);
  writer.u32(thresholds.min_attribution_confidence_ppm);
  writer.u32(thresholds.dominant_contributor_ppm);
  writer.u32(thresholds.contradiction_utilization_ppm);
  const MitigationBudget& budget = snapshot.budget();
  writer.u32(budget.max_affected_resources);
  writer.u32(budget.max_affected_paths);
  writer.u32(budget.max_affected_flows);
  writer.u32(budget.max_rate_delta_ppm);
  writer.u32(budget.max_scope_share_ppm);
  writer.u32(budget.max_plan_scope_share_ppm);
  writer.u32(budget.max_intents_per_plan);
  writer.u8(budget.allow_flow_relocation ? 1u : 0u);
  writer.u8(budget.allow_path_rebalance ? 1u : 0u);
  writer.u8(budget.allow_admission_reduction ? 1u : 0u);
  writer.u8(budget.allow_rate_change ? 1u : 0u);
  writer.u8(budget.allow_resource_isolation ? 1u : 0u);
  writer.u8(budget.allow_global_escalation ? 1u : 0u);
  writer.u8(budget.require_attribution_for_relocation ? 1u : 0u);
  const SeverityBands& bands = snapshot.bands();
  writer.u32(bands.low_ppm);
  writer.u32(bands.moderate_ppm);
  writer.u32(bands.high_ppm);
  writer.u32(bands.critical_ppm);
  return Status::success();
}

Status decode_policy_payload(std::span<const std::byte> payload, PolicySnapshot& out) {
  ByteReader reader(payload);
  StreamHeader header;
  Status status = decode_stream_header(reader, StreamKind::Policy, header);
  if (!status.ok()) return status;

  PolicySnapshot snapshot;
  std::uint64_t id = 0;
  Millis issued_at = kNoTime;
  std::uint32_t ttl = 0;
  if (!reader.u64(id) || !reader.i64(issued_at) || !reader.u32(ttl)) {
    return Status::failure(ErrorCode::TruncatedFrame, "policy header truncated");
  }
  Thresholds& thresholds = snapshot.thresholds();
  MitigationBudget& budget = snapshot.budget();
  SeverityBands& bands = snapshot.bands();
  const auto read_thresholds = [&reader, &thresholds]() -> bool {
    return reader.u32(thresholds.saturation_utilization_ppm) &&
           reader.u32(thresholds.queue_pressure_ppm) &&
           reader.u32(thresholds.buffer_pressure_ppm) &&
           reader.u32(thresholds.min_confidence_ppm) &&
           reader.u32(thresholds.max_signal_age_ms) &&
           reader.u32(thresholds.max_capacity_age_ms) &&
           reader.u32(thresholds.max_topology_age_ms) && reader.u32(thresholds.max_path_age_ms) &&
           reader.u32(thresholds.max_traffic_age_ms) &&
           reader.u32(thresholds.persistence_window_ms) &&
           reader.u32(thresholds.min_persistence_samples) &&
           reader.u32(thresholds.max_local_share_ppm) && reader.u32(thresholds.global_share_ppm) &&
           reader.u32(thresholds.max_local_regions) &&
           reader.u32(thresholds.max_regional_regions) && reader.u32(thresholds.min_coverage_ppm) &&
           reader.u32(thresholds.min_attribution_confidence_ppm) &&
           reader.u32(thresholds.dominant_contributor_ppm) &&
           reader.u32(thresholds.contradiction_utilization_ppm);
  };
  if (!read_thresholds()) {
    return Status::failure(ErrorCode::TruncatedFrame, "policy thresholds truncated");
  }
  if (!reader.u32(budget.max_affected_resources) || !reader.u32(budget.max_affected_paths) ||
      !reader.u32(budget.max_affected_flows) || !reader.u32(budget.max_rate_delta_ppm) ||
      !reader.u32(budget.max_scope_share_ppm) || !reader.u32(budget.max_plan_scope_share_ppm) ||
      !reader.u32(budget.max_intents_per_plan)) {
    return Status::failure(ErrorCode::TruncatedFrame, "policy budget truncated");
  }
  std::uint8_t flags[7] = {0, 0, 0, 0, 0, 0, 0};
  for (std::uint8_t& flag : flags) {
    if (!reader.u8(flag)) {
      return Status::failure(ErrorCode::TruncatedFrame, "policy budget flags truncated");
    }
  }
  budget.allow_flow_relocation = flags[0] != 0;
  budget.allow_path_rebalance = flags[1] != 0;
  budget.allow_admission_reduction = flags[2] != 0;
  budget.allow_rate_change = flags[3] != 0;
  budget.allow_resource_isolation = flags[4] != 0;
  budget.allow_global_escalation = flags[5] != 0;
  budget.require_attribution_for_relocation = flags[6] != 0;
  if (!reader.u32(bands.low_ppm) || !reader.u32(bands.moderate_ppm) ||
      !reader.u32(bands.high_ppm) || !reader.u32(bands.critical_ppm)) {
    return Status::failure(ErrorCode::TruncatedFrame, "policy severity bands truncated");
  }
  status = require_end(reader);
  if (!status.ok()) return status;

  snapshot.set_id(PolicyId{id});
  snapshot.set_generation(PolicyGeneration::from(header.generation));
  snapshot.set_issued_at(issued_at);
  snapshot.set_ttl_ms(ttl);
  out = std::move(snapshot);
  return Status::success();
}

// --- control payloads -------------------------------------------------------

Status encode_hello(const HelloPayload& payload, std::vector<std::byte>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u64(payload.publisher.value());
  writer.u64(payload.incarnation.value());
  writer.u64(payload.boot.value());
  writer.u32(payload.protocol_version);
  writer.text_field(payload.name);
  return Status::success();
}

Status decode_hello(std::span<const std::byte> payload, HelloPayload& out) {
  ByteReader reader(payload);
  std::uint64_t publisher = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t boot = 0;
  if (!reader.u64(publisher) || !reader.u64(incarnation) || !reader.u64(boot) ||
      !reader.u32(out.protocol_version) || !reader.text_field(Limits::kMaxTextBytes, out.name)) {
    return Status::failure(ErrorCode::TruncatedFrame, "hello payload truncated");
  }
  out.publisher = PublisherId{publisher};
  out.incarnation = Incarnation{incarnation};
  out.boot = BootId{boot};
  if (out.protocol_version != kFrameProtocolVersion) {
    return Status::failure(ErrorCode::UnsupportedVersion, "publisher protocol version is not supported");
  }
  return require_end(reader);
}

Status encode_welcome(const WelcomePayload& payload, std::vector<std::byte>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u64(payload.epoch.value());
  writer.u64(payload.coordinator_boot.value());
  writer.u32(payload.protocol_version);
  writer.u32(payload.heartbeat_ms);
  writer.text_field(payload.name);
  return Status::success();
}

Status decode_welcome(std::span<const std::byte> payload, WelcomePayload& out) {
  ByteReader reader(payload);
  std::uint64_t epoch = 0;
  std::uint64_t boot = 0;
  if (!reader.u64(epoch) || !reader.u64(boot) || !reader.u32(out.protocol_version) ||
      !reader.u32(out.heartbeat_ms) || !reader.text_field(Limits::kMaxTextBytes, out.name)) {
    return Status::failure(ErrorCode::TruncatedFrame, "welcome payload truncated");
  }
  out.epoch = CoordinatorEpoch{epoch};
  out.coordinator_boot = BootId{boot};
  return require_end(reader);
}

Status encode_rejected(const RejectedPayload& payload, std::vector<std::byte>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u32(static_cast<std::uint32_t>(payload.code));
  writer.text_field(payload.reason);
  return Status::success();
}

Status decode_rejected(std::span<const std::byte> payload, RejectedPayload& out) {
  ByteReader reader(payload);
  std::uint32_t code = 0;
  if (!reader.u32(code) || !reader.text_field(Limits::kMaxTextBytes, out.reason)) {
    return Status::failure(ErrorCode::TruncatedFrame, "rejected payload truncated");
  }
  out.code = static_cast<ErrorCode>(code);
  return require_end(reader);
}

}  // namespace hgm
