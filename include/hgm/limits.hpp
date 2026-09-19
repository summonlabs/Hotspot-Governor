// Hard bounds on every externally influenced collection, buffer and size.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace hgm {

// All bounds are compile-time constants so that no code path can be driven to
// unbounded allocation by remote input. Values are deliberately generous for
// fabric-scale deployments yet finite.
struct Limits {
  // Topology
  static constexpr std::size_t kMaxResources = 1u << 20;   // 1,048,576
  static constexpr std::size_t kMaxRegions = 1u << 16;     // 65,536
  static constexpr std::size_t kMaxLinks = 1u << 21;       // 2,097,152
  static constexpr std::size_t kMaxNeighborsPerResource = 4096;
  static constexpr std::size_t kMaxResourceNameBytes = 128;
  static constexpr std::size_t kMaxRegionNameBytes = 128;

  // Capacity / evidence / paths / traffic
  static constexpr std::size_t kMaxCapacityEntries = 1u << 20;
  static constexpr std::size_t kMaxSignalRecords = 1u << 20;
  static constexpr std::size_t kMaxPaths = 1u << 20;
  static constexpr std::size_t kMaxHopsPerPath = 256;
  static constexpr std::size_t kMaxTrafficRecords = 1u << 20;

  // Decisions
  static constexpr std::size_t kMaxHotspots = 4096;
  static constexpr std::size_t kMaxHotspotResources = 256;
  static constexpr std::size_t kMaxContributors = 256;
  static constexpr std::size_t kMaxIntentAffectedResources = 64;
  static constexpr std::size_t kMaxIntentAffectedPaths = 256;
  static constexpr std::size_t kMaxIntentAffectedFlows = 256;
  static constexpr std::size_t kMaxSuppressedEntries = 64;
  static constexpr std::size_t kMaxPressureEntries = 8192;
  static constexpr std::size_t kMaxContributorAccumulators = 4096;
  static constexpr std::size_t kMaxAuthorityGrants = 32;
  static constexpr std::size_t kMaxDiagnostics = 128;
  static constexpr std::size_t kMaxExplanationLines = 512;
  static constexpr std::size_t kMaxReasonBytes = 192;
  static constexpr std::size_t kMaxTextBytes = 512;

  // Transport
  static constexpr std::size_t kMaxFrameBytes = 16u << 20;  // 16 MiB
  static constexpr std::size_t kMaxPublishers = 512;
  static constexpr std::size_t kMaxConnections = 512;

  // Concurrency
  static constexpr std::size_t kMaxWorkerThreads = 64;
  static constexpr std::size_t kMaxQueueDepth = 1u << 16;

  // Durability
  static constexpr std::size_t kMaxJournalRecordBytes = 1u << 20;
  static constexpr std::uint64_t kMaxDurableBytes = 1ull << 32;  // 4 GiB
  static constexpr std::size_t kMaxJournalRecords = 1u << 20;
  static constexpr std::uint32_t kMaxRetries = 8;

  // History retained in memory for persistence/severity decisions.
  static constexpr std::size_t kMaxHistoryResources = 1u << 20;
  static constexpr std::size_t kMaxEvaluationsRetained = 4096;
};

}  // namespace hgm
