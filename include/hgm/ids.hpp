// Strongly typed identities, generations, epochs and incarnations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "hgm/digest.hpp"

namespace hgm {

// A strong identity. Zero is reserved for "absent" and never names a real
// entity, so an omitted id can never alias a live one.
template <class Tag>
class StrongId {
 public:
  using value_type = std::uint64_t;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(std::uint64_t value) noexcept : value_(value) {}

  static constexpr StrongId from(std::uint64_t value) noexcept { return StrongId(value); }
  static StrongId from_name(std::string_view name) noexcept {
    return StrongId(identity_from_bytes(name));
  }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }
  constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return a.value_ >= b.value_; }

  std::string hex() const { return hex64(value_); }

  // Advances by one, saturating rather than wrapping.
  StrongId next() const noexcept {
    return value_ == UINT64_MAX ? *this : StrongId(value_ + 1);
  }

 private:
  std::uint64_t value_ = 0;
};

// A monotonically increasing generation counter for one authoritative stream.
template <class Tag>
class Generation {
 public:
  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  static constexpr Generation from(std::uint64_t value) noexcept { return Generation(value); }
  // The generation that precedes every real generation.
  static constexpr Generation none() noexcept { return Generation(0); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(Generation a, Generation b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Generation a, Generation b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Generation a, Generation b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept { return a.value_ >= b.value_; }

  // Advances the generation by one, saturating rather than wrapping.
  Generation next() const noexcept {
    return value_ == UINT64_MAX ? *this : Generation(value_ + 1);
  }

 private:
  std::uint64_t value_ = 0;
};

// --- identities -------------------------------------------------------------
using ResourceId = StrongId<struct ResourceTag>;
using RegionId = StrongId<struct RegionTag>;
using LinkId = StrongId<struct LinkTag>;
using PathId = StrongId<struct PathTag>;
using FlowId = StrongId<struct FlowTag>;
using HotspotId = StrongId<struct HotspotTag>;
using EvidenceId = StrongId<struct EvidenceTag>;
using CapacityId = StrongId<struct CapacityTag>;
using TopologyId = StrongId<struct TopologyTag>;
using PathSetId = StrongId<struct PathSetTag>;
using TrafficId = StrongId<struct TrafficTag>;
using PolicyId = StrongId<struct PolicyTag>;
using InterventionId = StrongId<struct InterventionTag>;
using PublisherId = StrongId<struct PublisherTag>;
using NodeId = StrongId<struct NodeTag>;

// --- authority identities ---------------------------------------------------
// Coordinator epoch: advanced on every coordinator incarnation change. A frame
// minted under a lower epoch is stale.
using CoordinatorEpoch = StrongId<struct CoordinatorEpochTag>;
// Boot incarnation: identifies one process lifetime of one node.
using BootId = StrongId<struct BootIdTag>;
// Publisher incarnation: identifies one process lifetime of one publisher.
using Incarnation = StrongId<struct IncarnationTag>;

// --- generations ------------------------------------------------------------
using TopologyGeneration = Generation<struct TopologyGenTag>;
using CapacityGeneration = Generation<struct CapacityGenTag>;
using EvidenceGeneration = Generation<struct EvidenceGenTag>;
using PathGeneration = Generation<struct PathGenTag>;
using TrafficGeneration = Generation<struct TrafficGenTag>;
using PolicyGeneration = Generation<struct PolicyGenTag>;

// Per-publisher, per-stream monotonically increasing frame sequence.
using Sequence = std::uint64_t;

}  // namespace hgm

namespace std {

template <class Tag>
struct hash<hgm::StrongId<Tag>> {
  std::size_t operator()(hgm::StrongId<Tag> id) const noexcept {
    return static_cast<std::size_t>(id.value() * 0x9E3779B97F4A7C15ull);
  }
};

template <class Tag>
struct hash<hgm::Generation<Tag>> {
  std::size_t operator()(hgm::Generation<Tag> g) const noexcept {
    return static_cast<std::size_t>(g.value() * 0x9E3779B97F4A7C15ull);
  }
};

}  // namespace std
