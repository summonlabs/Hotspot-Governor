// The stateful governor: authority fencing, evidence admission, deterministic
// evaluation and durable commit.
//
// Locking (see docs/LOCKING.md): one mutex guards all authoritative state. It
// is never held across a callback, a worker task, or a lock owned by another
// component. Decisions are returned to the caller rather than emitted from
// inside the lock.
//
// Durability boundary: admit() acknowledges IN-MEMORY admission of telemetry
// only. Durable acknowledgement happens exclusively in commit_durable(), which
// returns success only after the snapshot is on stable storage.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hgm/capacity.hpp"
#include "hgm/engine.hpp"
#include "hgm/error.hpp"
#include "hgm/frame.hpp"
#include "hgm/ids.hpp"
#include "hgm/metrics.hpp"
#include "hgm/paths.hpp"
#include "hgm/policy.hpp"
#include "hgm/signals.hpp"
#include "hgm/store.hpp"
#include "hgm/time.hpp"
#include "hgm/topology.hpp"
#include "hgm/traffic.hpp"
#include "hgm/worker.hpp"

namespace hgm {

struct GovernorConfig {
  std::string node_name = "hotspot-governor";
  CoordinatorEpoch initial_epoch{};
  std::size_t worker_threads = 2;
  std::size_t queue_capacity = 256;
  bool include_explanation = true;
  EngineConfig engine{};
  std::optional<StoreConfig> store{};
  // When true, a publisher's death immediately drops the evidence it published.
  // When false (default), that evidence stays admissible until the policy
  // freshness window expires, and then ages out on its own.
  bool drop_evidence_on_publisher_death = false;
  std::size_t max_history = Limits::kMaxHotspots;
};

struct PublisherState {
  PublisherId id{};
  Incarnation incarnation{};
  BootId boot{};
  CoordinatorEpoch epoch{};
  Sequence last_sequence = 0;
  StreamGeneration generation{};
  // Monotonic generation per authoritative stream, so one publisher may own
  // several streams without their counters interfering.
  std::array<std::uint64_t, kStreamCount> stream_generations{};
  Millis first_seen = kNoTime;
  Millis last_seen = kNoTime;
  Millis died_at = kNoTime;
  bool alive = false;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
};

struct AdmissionReport {
  bool accepted = false;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  StreamKind stream = StreamKind::Unknown;
  std::uint64_t generation = 0;
  Sequence sequence = 0;
  bool duplicate = false;
  bool stale = false;
  std::size_t records = 0;
};

class Governor {
 public:
  explicit Governor(GovernorConfig config = GovernorConfig{});
  ~Governor();

  Governor(const Governor&) = delete;
  Governor& operator=(const Governor&) = delete;

  // --- lifecycle ------------------------------------------------------------

  // Opens the durable store when configured and recovers committed history.
  // Live authority is never restored.
  Status recover(Millis now);

  Status start_workers();
  Status stop_workers();

  // --- authority ------------------------------------------------------------

  // Installs coordinator authority. A strictly higher epoch fences every
  // registered publisher incarnation so that stale epochs cannot mutate state.
  Status install_epoch(CoordinatorEpoch epoch, BootId coordinator_boot, Millis now);

  // Registers or refreshes a publisher incarnation. A strictly higher
  // incarnation fences the previous one.
  Status register_publisher(const HelloPayload& hello, Millis now, WelcomePayload& welcome);

  // Records that a publisher is gone. Its authority is revoked immediately;
  // its evidence ages out under the policy freshness rules.
  Status note_publisher_death(PublisherId publisher, Millis now);

  // --- evidence -------------------------------------------------------------

  Result<AdmissionReport> admit(const Frame& frame, Millis now);

  // --- decisions ------------------------------------------------------------

  Decision evaluate(Millis now);

  // --- durability -----------------------------------------------------------

  // Writes the committed state durably. Returns only after the snapshot is on
  // stable storage; failure means nothing was acknowledged.
  Status commit_durable(Millis now);

  // --- inspection -----------------------------------------------------------

  CoordinatorEpoch epoch() const;
  BootId boot() const;
  std::string node_name() const;
  Metrics metrics() const;
  const Metrics& metrics_ref() const noexcept { return metrics_; }
  std::vector<PublisherState> publishers() const;
  SaturationTracker::Entry history_entry(ResourceId resource) const;
  std::size_t tracked_saturations() const;
  const RecoveredState& recovery() const noexcept { return recovery_; }
  bool store_open() const noexcept { return store_ != nullptr; }
  const DurableState& durable_state() const noexcept { return durable_; }
  Engine& engine() noexcept { return engine_; }
  const Engine& engine() const noexcept { return engine_; }

 private:
  struct Snapshot {
    std::optional<TopologySnapshot> topology;
    std::optional<CapacitySnapshot> capacity;
    std::optional<SignalSnapshot> signals;
    std::optional<PathSnapshot> paths;
    std::optional<TrafficSnapshot> traffic;
    std::optional<PolicySnapshot> policy;
  };

  // Validates that a snapshot's references exist in the live topology
  // generation. Runs once per admitted frame rather than once per evaluation.
  Status verify_references_locked() const;

  PublisherState* find_publisher(PublisherId id);
  const PublisherState* find_publisher(PublisherId id) const;
  Status admit_locked(const Frame& frame, Millis now, AdmissionReport& report);
  void record_decision_locked(const Decision& decision, Millis now);
  DurableState durable_snapshot_locked(Millis now) const;
  BootId make_boot_id(Millis now) const;

  mutable std::mutex mutex_;
  GovernorConfig config_{};
  Engine engine_;
  Snapshot snapshots_{};
  std::vector<PublisherState> publishers_;  // sorted by PublisherId
  CoordinatorEpoch epoch_{};
  BootId boot_{};
  Metrics metrics_{};
  DurableState durable_{};
  RecoveredState recovery_{};
  // Verification stamp: which topology generation the installed snapshots were
  // checked against, and which snapshot identities were checked.
  TopologyId verified_topology_{};
  CapacityId verified_capacity_{};
  EvidenceId verified_signals_{};
  PathSetId verified_paths_{};
  std::unique_ptr<Store> store_;
  std::unique_ptr<WorkerPool> workers_;
  bool workers_started_ = false;
};

}  // namespace hgm
