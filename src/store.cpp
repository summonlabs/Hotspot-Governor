// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/store.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "file_io.hpp"
#include "hgm/byteio.hpp"
#include "hgm/digest.hpp"
#include "hgm/version.hpp"

namespace hgm {
namespace {

constexpr std::uint32_t kSnapshotMagic = 0x31534748u;  // "HGS1"
constexpr std::size_t kSnapshotHeaderBytes = 32;

Status validate_state(const DurableState& state) {
  if (state.hotspots.size() > Limits::kMaxHotspots) {
    return Status::failure(ErrorCode::BoundExceeded, "durable hotspot history exceeds the bound");
  }
  if (state.interventions.size() > Limits::kMaxHotspots * 4) {
    return Status::failure(ErrorCode::BoundExceeded, "durable intervention history exceeds the bound");
  }
  for (const DurableHotspot& hotspot : state.hotspots) {
    if (hotspot.key.size() > Limits::kMaxTextBytes) {
      return Status::failure(ErrorCode::BoundExceeded, "durable hotspot key exceeds the bound");
    }
  }
  return Status::success();
}

}  // namespace

void encode_durable_state(const DurableState& state, std::vector<std::byte>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u32(state.format_version);
  writer.u64(state.written_by_boot.value());
  writer.u64(state.epoch.value());
  writer.u64(state.evaluations);
  writer.u64(state.journal_sequence);
  writer.i64(state.written_at);
  writer.u32(static_cast<std::uint32_t>(state.hotspots.size()));
  for (const DurableHotspot& hotspot : state.hotspots) {
    writer.u64(hotspot.id.value());
    writer.text_field(hotspot.key);
    writer.u8(static_cast<std::uint8_t>(hotspot.scope));
    writer.u8(static_cast<std::uint8_t>(hotspot.severity));
    writer.u8(static_cast<std::uint8_t>(hotspot.cause));
    writer.i64(hotspot.persistence_ms);
    writer.i64(hotspot.last_observed);
    writer.u64(hotspot.resource_count);
  }
  writer.u32(static_cast<std::uint32_t>(state.interventions.size()));
  for (const DurableIntervention& intervention : state.interventions) {
    writer.u64(intervention.id.value());
    writer.u8(static_cast<std::uint8_t>(intervention.kind));
    writer.u64(intervention.hotspot.value());
    writer.i64(intervention.at);
    writer.u32(intervention.scope_share_ppm);
    writer.u8(intervention.escalation ? 1u : 0u);
  }
}

Status decode_durable_state(std::span<const std::byte> bytes, DurableState& out) {
  ByteReader reader(bytes);
  DurableState state;
  std::uint64_t boot = 0;
  std::uint64_t epoch = 0;
  if (!reader.u32(state.format_version) || !reader.u64(boot) || !reader.u64(epoch) ||
      !reader.u64(state.evaluations) || !reader.u64(state.journal_sequence) ||
      !reader.i64(state.written_at)) {
    return Status::failure(ErrorCode::PersistenceTruncated, "durable state header truncated");
  }
  state.written_by_boot = BootId{boot};
  state.epoch = CoordinatorEpoch{epoch};

  std::uint32_t hotspot_count = 0;
  if (!reader.u32(hotspot_count)) {
    return Status::failure(ErrorCode::PersistenceTruncated, "durable hotspot count truncated");
  }
  if (static_cast<std::size_t>(hotspot_count) > Limits::kMaxHotspots) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "durable hotspot count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < hotspot_count; ++i) {
    DurableHotspot hotspot;
    std::uint64_t id = 0;
    std::uint8_t scope = 0;
    std::uint8_t severity = 0;
    std::uint8_t cause = 0;
    if (!reader.u64(id) || !reader.text_field(Limits::kMaxTextBytes, hotspot.key) ||
        !reader.u8(scope) || !reader.u8(severity) || !reader.u8(cause) ||
        !reader.i64(hotspot.persistence_ms) || !reader.i64(hotspot.last_observed) ||
        !reader.u64(hotspot.resource_count)) {
      return Status::failure(ErrorCode::PersistenceTruncated, "durable hotspot entry truncated");
    }
    hotspot.id = HotspotId{id};
    hotspot.scope = static_cast<CongestionScope>(scope);
    hotspot.severity = static_cast<Severity>(severity);
    hotspot.cause = static_cast<SaturationCause>(cause);
    state.hotspots.push_back(std::move(hotspot));
  }

  std::uint32_t intervention_count = 0;
  if (!reader.u32(intervention_count)) {
    return Status::failure(ErrorCode::PersistenceTruncated, "durable intervention count truncated");
  }
  if (static_cast<std::size_t>(intervention_count) > Limits::kMaxHotspots * 4) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "durable intervention count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < intervention_count; ++i) {
    DurableIntervention intervention;
    std::uint64_t id = 0;
    std::uint8_t kind = 0;
    std::uint64_t hotspot = 0;
    std::uint8_t escalation = 0;
    if (!reader.u64(id) || !reader.u8(kind) || !reader.u64(hotspot) || !reader.i64(intervention.at) ||
        !reader.u32(intervention.scope_share_ppm) || !reader.u8(escalation)) {
      return Status::failure(ErrorCode::PersistenceTruncated, "durable intervention entry truncated");
    }
    intervention.id = InterventionId{id};
    intervention.kind = static_cast<MitigationKind>(kind);
    intervention.hotspot = HotspotId{hotspot};
    intervention.escalation = escalation != 0;
    state.interventions.push_back(std::move(intervention));
  }
  if (!reader.at_end()) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "durable state has trailing bytes");
  }
  out = std::move(state);
  return Status::success();
}

Store::Store(StoreConfig config) : config_(std::move(config)), journal_(config_.directory / "journal.log", config_.max_bytes) {
  snapshot_path_ = config_.directory / "state.bin";
  snapshot_temp_path_ = config_.directory / "state.bin.tmp";
}

Store::~Store() { static_cast<void>(close()); }

Status Store::open() {
  if (opened_) return Status::failure(ErrorCode::AlreadyStarted, "store is already open");
  Status status = ensure_directory(config_.directory);
  if (!status.ok()) return status;
  status = journal_.open();
  if (!status.ok()) return status;

  // A leftover temporary snapshot means a previous commit died before the
  // atomic replace. The old snapshot is authoritative; the temporary file is
  // discarded rather than trusted.
  if (file_exists(snapshot_temp_path_)) {
    static_cast<void>(remove_file(snapshot_temp_path_));
  }
  opened_ = true;
  return Status::success();
}

Status Store::close() {
  if (!opened_) return Status::success();
  Status status = journal_.close();
  opened_ = false;
  return status;
}

Status Store::write_snapshot(const DurableState& state) {
  std::vector<std::byte> body;
  encode_durable_state(state, body);
  if (body.size() > config_.max_bytes) {
    return Status::failure(ErrorCode::DurableGrowthExceeded, "snapshot exceeds the configured bound");
  }

  std::vector<std::byte> framed;
  framed.reserve(kSnapshotHeaderBytes + body.size());
  ByteWriter writer(framed);
  writer.u32(kSnapshotMagic);
  writer.u32(kDurableFormatVersion);
  writer.u32(static_cast<std::uint32_t>(body.size()));
  writer.u32(0);
  writer.u64(crc64(std::span<const std::byte>(body.data(), body.size())));
  writer.u64(crc64(std::span<const std::byte>(framed.data(), framed.size())));
  writer.raw(std::span<const std::byte>(body.data(), body.size()));
  if (framed.size() != kSnapshotHeaderBytes + body.size()) {
    return Status::failure(ErrorCode::Internal, "snapshot framing size mismatch");
  }

  Status status = write_file_all(snapshot_temp_path_, std::span<const std::byte>(framed.data(), framed.size()));
  if (!status.ok()) return status;
  status = fsync_file(snapshot_temp_path_);
  if (!status.ok()) return status;
  return atomic_replace(snapshot_temp_path_, snapshot_path_);
}

Result<DurableState> Store::read_snapshot(std::uint64_t& bytes_out, std::uint64_t& discarded_tail) {
  bytes_out = 0;
  discarded_tail = 0;
  if (!file_exists(snapshot_path_)) {
    return Status::failure(ErrorCode::MissingEvidence, "no durable snapshot present");
  }
  Result<std::vector<std::byte>> read = read_file_all(snapshot_path_, config_.max_bytes);
  if (!read.ok()) return read.status();
  const std::vector<std::byte> bytes = std::move(read).value();
  bytes_out = bytes.size();
  if (bytes.size() < kSnapshotHeaderBytes) {
    discarded_tail = bytes.size();
    return Status::failure(ErrorCode::PersistenceTruncated, "snapshot header is incomplete");
  }

  ByteReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t body_length = 0;
  std::uint32_t reserved = 0;
  std::uint64_t body_crc = 0;
  std::uint64_t header_crc = 0;
  if (!reader.u32(magic) || !reader.u32(version) || !reader.u32(body_length) || !reader.u32(reserved) ||
      !reader.u64(body_crc) || !reader.u64(header_crc)) {
    discarded_tail = bytes.size();
    return Status::failure(ErrorCode::PersistenceTruncated, "snapshot header could not be decoded");
  }
  static_cast<void>(reserved);
  if (magic != kSnapshotMagic) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "snapshot magic does not match");
  }
  if (version != kDurableFormatVersion) {
    return Status::failure(ErrorCode::PersistenceVersionUnsupported,
                           "durable format version " + std::to_string(version) + " is not supported");
  }
  const std::uint64_t expected_header_crc =
      crc64(std::span<const std::byte>(bytes.data(), kSnapshotHeaderBytes - 8));
  if (header_crc != expected_header_crc) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "snapshot header CRC does not match");
  }
  if (static_cast<std::size_t>(body_length) != bytes.size() - kSnapshotHeaderBytes) {
    discarded_tail = bytes.size();
    return Status::failure(ErrorCode::PersistenceTruncated, "snapshot body length does not match");
  }
  const std::span<const std::byte> body = std::span<const std::byte>(bytes.data() + kSnapshotHeaderBytes, body_length);
  if (crc64(body) != body_crc) {
    return Status::failure(ErrorCode::PersistenceCorrupt, "snapshot body CRC does not match");
  }

  DurableState state;
  Status status = decode_durable_state(body, state);
  if (!status.ok()) return status;
  return state;
}

Status Store::commit(const DurableState& state, const std::vector<JournalRecord>& records) {
  if (!opened_) return Status::failure(ErrorCode::NotStarted, "store is not open");
  Status status = validate_state(state);
  if (!status.ok()) return status;

  // 1. Journal the intent. Nothing is acknowledged until this is durable.
  for (const JournalRecord& record : records) {
    status = journal_.append(record.type, std::span<const std::byte>(record.payload.data(), record.payload.size()),
                             record.at);
    if (!status.ok()) return status;
  }
  status = journal_.sync();
  if (!status.ok()) return status;

  // 2. Snapshot the committed state, which subsumes the journal records.
  status = write_snapshot(state);
  if (!status.ok()) return status;

  // 3. Retire the journal: everything it held is now inside the snapshot.
  status = journal_.reset();
  if (!status.ok()) return status;
  return journal_.sync();
}

Result<RecoveredState> Store::recover() {
  if (!opened_) return Status::failure(ErrorCode::NotStarted, "store is not open");

  RecoveredState recovered;
  recovered.format_version = kDurableFormatVersion;
  recovered.notes.emplace_back(
      "recovery restores durable configuration and committed history only");
  recovered.notes.emplace_back(
      "publisher incarnations, leases, evidence freshness and worker authority are NOT restored");
  recovered.discarded_live_authority = 5;  // publishers, leases, freshness, workers, coordinator

  if (!file_exists(snapshot_path_)) {
    recovered.recovered = false;
    recovered.outcome = Status::failure(ErrorCode::MissingEvidence, "no durable snapshot present");
    recovered.notes.emplace_back("fresh boot: no durable state to recover");
    return recovered;
  }

  std::uint64_t snapshot_bytes = 0;
  std::uint64_t discarded_tail = 0;
  Result<DurableState> snapshot = read_snapshot(snapshot_bytes, discarded_tail);
  if (!snapshot.ok()) {
    recovered.outcome = snapshot.status();
    recovered.recovered = false;
    recovered.discarded_tail_bytes = discarded_tail;
    recovered.snapshot_bytes = snapshot_bytes;
    recovered.format_supported = snapshot.status().code() != ErrorCode::PersistenceVersionUnsupported;
    recovered.notes.emplace_back("durable snapshot was rejected: " + snapshot.status().text());
    return recovered;
  }

  DurableState state = std::move(snapshot).value();
  recovered.snapshot_bytes = snapshot_bytes;
  recovered.format_version = state.format_version;
  recovered.last_epoch = state.epoch;
  recovered.previous_boot = state.written_by_boot;
  recovered.written_at = state.written_at;
  recovered.journal_sequence = state.journal_sequence;

  // Replay the journal on top of the snapshot, skipping records the snapshot
  // already covers and stopping at the first damaged record.
  Result<std::vector<JournalRecord>> records = journal_.replay(config_.max_journal_records);
  if (!records.ok()) {
    recovered.outcome = records.status();
    recovered.notes.emplace_back("journal could not be read: " + records.status().text());
    recovered.state = std::move(state);
    recovered.recovered = true;
    return recovered;
  }

  Sequence highest = state.journal_sequence;
  std::uint64_t applied = 0;
  for (const JournalRecord& record : records.value()) {
    if (record.sequence <= state.journal_sequence) continue;
    switch (record.type) {
      case JournalRecordType::EpochAdvance: {
        if (record.payload.size() >= sizeof(std::uint64_t)) {
          ByteReader reader(std::span<const std::byte>(record.payload.data(), record.payload.size()));
          std::uint64_t epoch = 0;
          if (reader.u64(epoch)) {
            if (CoordinatorEpoch{epoch} > recovered.last_epoch) {
              recovered.last_epoch = CoordinatorEpoch{epoch};
            }
          }
        }
        break;
      }
      case JournalRecordType::PolicyCommit:
        // The policy itself must be re-published; only its presence is history.
        recovered.notes.emplace_back("policy from the journal requires revalidation");
        break;
      case JournalRecordType::BootRecord:
        break;
      default:
        break;
    }
    highest = std::max(highest, record.sequence);
    ++applied;
  }

  recovered.applied_journal_records = applied;
  recovered.journal_sequence = highest;
  recovered.discarded_tail_bytes = journal_.report().discarded_tail_bytes;
  if (recovered.discarded_tail_bytes > 0) {
    recovered.notes.emplace_back("a torn journal tail was discarded, not replayed");
  }
  recovered.state = std::move(state);
  recovered.state.journal_sequence = highest;
  recovered.recovered = true;
  recovered.outcome = Status::success();
  return recovered;
}

}  // namespace hgm
