// Provenance: who produced a statement, under which authority, at which
// sequence and generation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

#include "hgm/digest.hpp"
#include "hgm/ids.hpp"
#include "hgm/time.hpp"

namespace hgm {

// Which authoritative stream a statement belongs to. Each stream has exactly
// one authoritative publisher at a time; the governor refuses to mix them.
enum class StreamKind : std::uint16_t {
  Unknown = 0,
  Topology = 1,
  Capacity = 2,
  Signals = 3,
  Paths = 4,
  Traffic = 5,
  Policy = 6,
};

std::string_view to_string(StreamKind kind) noexcept;

// Number of authoritative streams; StreamKind::Unknown counts as slot 0.
inline constexpr std::size_t kStreamCount = 7;

// Slot for per-stream bookkeeping. Unknown and out-of-range map to slot 0,
// which is never used for a real stream.
inline std::size_t stream_index(StreamKind kind) noexcept {
  const auto value = static_cast<std::size_t>(kind);
  return value < kStreamCount ? value : 0;
}

// The generation type carried on the wire is uniform; the meaning is fixed by
// StreamKind.
struct StreamGeneration {
  StreamKind stream = StreamKind::Unknown;
  std::uint64_t value = 0;

  friend bool operator==(const StreamGeneration& a, const StreamGeneration& b) noexcept {
    return a.stream == b.stream && a.value == b.value;
  }
};

struct Provenance {
  PublisherId publisher{};
  Incarnation incarnation{};
  BootId boot{};
  CoordinatorEpoch epoch{};
  Sequence sequence = 0;
  StreamGeneration generation{};
  Millis emitted_at = kNoTime;
  Digest payload_digest{};
  std::string source;

  bool valid() const noexcept {
    return publisher.valid() && incarnation.valid() && epoch.valid() && payload_digest.valid();
  }
};

// Bounded description of the evidence a decision was bound to.
struct EvidenceBinding {
  TopologyId topology{};
  TopologyGeneration topology_generation{};
  CapacityId capacity{};
  CapacityGeneration capacity_generation{};
  EvidenceId signals{};
  EvidenceGeneration signals_generation{};
  PathSetId paths{};
  PathGeneration path_generation{};
  TrafficId traffic{};
  TrafficGeneration traffic_generation{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};

  friend bool operator==(const EvidenceBinding& a, const EvidenceBinding& b) noexcept {
    return a.topology == b.topology && a.topology_generation == b.topology_generation &&
           a.capacity == b.capacity && a.capacity_generation == b.capacity_generation &&
           a.signals == b.signals && a.signals_generation == b.signals_generation &&
           a.paths == b.paths && a.path_generation == b.path_generation &&
           a.traffic == b.traffic && a.traffic_generation == b.traffic_generation &&
           a.policy == b.policy && a.policy_generation == b.policy_generation;
  }
  friend bool operator!=(const EvidenceBinding& a, const EvidenceBinding& b) noexcept {
    return !(a == b);
  }
};

}  // namespace hgm
