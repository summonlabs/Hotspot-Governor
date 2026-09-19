// Versioned, integrity-checked, crash-safe durable state: an atomic snapshot
// plus a write-ahead journal.
//
// Commit order is: journal -> fsync -> snapshot (temp + fsync + atomic
// replace) -> journal reset. A crash at any point therefore recovers either
// the old snapshot plus the complete journal, or the new snapshot.
//
// Recovery deliberately restores durable configuration and committed history
// only. Publisher authority, evidence freshness, leases and worker authority
// are never restored: they must be re-established by live processes.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hgm/enums.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/journal.hpp"
#include "hgm/limits.hpp"
#include "hgm/time.hpp"

namespace hgm {

struct DurableHotspot {
  HotspotId id{};
  std::string key;
  CongestionScope scope = CongestionScope::Unknown;
  Severity severity = Severity::None;
  SaturationCause cause = SaturationCause::Unknown;
  Millis persistence_ms = 0;
  Millis last_observed = kNoTime;
  std::uint64_t resource_count = 0;
};

struct DurableIntervention {
  InterventionId id{};
  MitigationKind kind = MitigationKind::None;
  HotspotId hotspot{};
  Millis at = kNoTime;
  std::uint32_t scope_share_ppm = 0;
  bool escalation = false;
};

struct DurableState {
  std::uint32_t format_version = 1;
  BootId written_by_boot{};
  CoordinatorEpoch epoch{};
  std::uint64_t evaluations = 0;
  Sequence journal_sequence = 0;
  Millis written_at = kNoTime;
  std::vector<DurableHotspot> hotspots;
  std::vector<DurableIntervention> interventions;
};

struct StoreConfig {
  std::filesystem::path directory;
  std::uint64_t max_bytes = Limits::kMaxDurableBytes;
  std::size_t max_journal_records = Limits::kMaxJournalRecords;
};

struct RecoveredState {
  bool recovered = false;
  bool format_supported = true;
  std::uint32_t format_version = 0;
  Status outcome = Status::success();
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t discarded_tail_bytes = 0;
  std::uint64_t applied_journal_records = 0;
  Sequence journal_sequence = 0;
  CoordinatorEpoch last_epoch{};
  BootId previous_boot{};
  Millis written_at = kNoTime;
  DurableState state{};

  // What restart explicitly does NOT restore.
  bool publisher_authority_restored = false;
  bool evidence_freshness_restored = false;
  bool leases_restored = false;
  bool worker_authority_restored = false;
  bool requires_revalidation = true;
  std::uint32_t discarded_live_authority = 0;
  std::vector<std::string> notes;
};

class Store {
 public:
  explicit Store(StoreConfig config);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Status open();
  Status close();

  // validate -> journal records -> fsync -> snapshot -> atomic replace ->
  // journal reset. Returns a failure without acknowledging when any step is
  // not durable.
  Status commit(const DurableState& state, const std::vector<JournalRecord>& records);

  // Reads the snapshot and replays the journal on top of it.
  Result<RecoveredState> recover();

  bool opened() const noexcept { return opened_; }
  const std::filesystem::path& directory() const noexcept { return config_.directory; }
  std::uint64_t journal_bytes() const noexcept { return journal_.bytes(); }
  Sequence journal_sequence() const noexcept { return journal_.last_sequence(); }
  const JournalOpenReport& journal_report() const noexcept { return journal_.report(); }

  const std::filesystem::path& snapshot_path() const noexcept { return snapshot_path_; }
  const std::filesystem::path& journal_path() const noexcept { return journal_.path(); }

 private:
  Status write_snapshot(const DurableState& state);
  Result<DurableState> read_snapshot(std::uint64_t& bytes_out, std::uint64_t& discarded_tail);

  StoreConfig config_;
  std::filesystem::path snapshot_path_;
  std::filesystem::path snapshot_temp_path_;
  Journal journal_;
  bool opened_ = false;
};

// Serialization of DurableState. Exposed for tests and tooling.
void encode_durable_state(const DurableState& state, std::vector<std::byte>& out);
Status decode_durable_state(std::span<const std::byte> bytes, DurableState& out);

}  // namespace hgm
