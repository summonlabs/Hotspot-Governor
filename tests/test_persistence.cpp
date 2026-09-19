// Durability: journal, crash-safe store, recovery and corruption handling.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "hgm/byteio.hpp"
#include "hgm/journal.hpp"
#include "hgm/limits.hpp"
#include "hgm/store.hpp"
#include "hgm/version.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::TempDir;

namespace {

std::vector<std::byte> payload_of(const std::string& text) {
  std::vector<std::byte> bytes;
  for (const char character : text) bytes.push_back(static_cast<std::byte>(character));
  return bytes;
}

std::uint64_t size_of(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

void append_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void truncate_to(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code error;
  std::filesystem::resize_file(path, static_cast<std::uintmax_t>(size), error);
}

DurableState sample_state(BootId boot, CoordinatorEpoch epoch, std::uint64_t evaluations) {
  DurableState state;
  state.format_version = kDurableFormatVersion;
  state.written_by_boot = boot;
  state.epoch = epoch;
  state.evaluations = evaluations;
  state.written_at = 1234;
  DurableHotspot hotspot;
  hotspot.id = HotspotId::from_name("hotspot-1");
  hotspot.key = "hs-0000000000000001";
  hotspot.scope = CongestionScope::Localized;
  hotspot.severity = Severity::High;
  hotspot.cause = SaturationCause::CapacityExhaustion;
  hotspot.persistence_ms = 4000;
  hotspot.last_observed = 1200;
  hotspot.resource_count = 3;
  state.hotspots.push_back(hotspot);
  DurableIntervention intervention;
  intervention.id = InterventionId::from_name("intent-1");
  intervention.kind = MitigationKind::AdmissionReduction;
  intervention.hotspot = hotspot.id;
  intervention.at = 1250;
  intervention.scope_share_ppm = 12000;
  state.interventions.push_back(intervention);
  return state;
}

}  // namespace

HGM_TEST(persistence, journal_record_round_trips) {
  JournalRecord record;
  record.type = JournalRecordType::HotspotCommit;
  record.sequence = 7;
  record.at = 99;
  record.payload = payload_of("hello journal");
  std::vector<std::byte> encoded;
  encode_journal_record(record, encoded);
  HGM_CHECK_EQ(encoded.size(), kJournalHeaderBytes + std::string("hello journal").size());

  JournalRecord decoded;
  const std::size_t consumed = decode_journal_record(std::span<const std::byte>(encoded.data(), encoded.size()), decoded);
  HGM_CHECK_EQ(consumed, encoded.size());
  HGM_CHECK_EQ(decoded.type, JournalRecordType::HotspotCommit);
  HGM_CHECK_EQ(decoded.sequence, 7ull);
  HGM_CHECK_EQ(decoded.at, static_cast<Millis>(99));
  HGM_CHECK_EQ(decoded.payload, record.payload);
}

HGM_TEST(persistence, journal_record_rejects_corruption_and_truncation) {
  JournalRecord record;
  record.type = JournalRecordType::EpochAdvance;
  record.sequence = 1;
  record.payload = payload_of("abc");
  std::vector<std::byte> encoded;
  encode_journal_record(record, encoded);

  JournalRecord decoded;
  HGM_CHECK_EQ(decode_journal_record(std::span<const std::byte>(encoded.data(), encoded.size() - 1), decoded),
               static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decode_journal_record(std::span<const std::byte>(encoded.data(), kJournalHeaderBytes - 1), decoded),
               static_cast<std::size_t>(0));

  std::vector<std::byte> corrupted = encoded;
  corrupted[4] = static_cast<std::byte>(0xAB);
  HGM_CHECK_EQ(decode_journal_record(std::span<const std::byte>(corrupted.data(), corrupted.size()), decoded),
               static_cast<std::size_t>(0));

  std::vector<std::byte> payload_corrupted = encoded;
  payload_corrupted[kJournalHeaderBytes] =
      static_cast<std::byte>(static_cast<unsigned>(payload_corrupted[kJournalHeaderBytes]) ^ 0xFFu);
  HGM_CHECK_EQ(decode_journal_record(std::span<const std::byte>(payload_corrupted.data(), payload_corrupted.size()), decoded),
               static_cast<std::size_t>(0));
}

HGM_TEST(persistence, journal_appends_and_replays) {
  TempDir dir("journal");
  Journal journal(dir.file("journal.log"));
  HGM_CHECK(journal.open().ok());
  HGM_CHECK(journal.append(JournalRecordType::BootRecord, payload_of("boot"), 1).ok());
  HGM_CHECK(journal.append(JournalRecordType::EpochAdvance, payload_of("epoch"), 2).ok());
  HGM_CHECK_EQ(journal.last_sequence(), 2ull);
  HGM_CHECK(journal.sync().ok());

  Result<std::vector<JournalRecord>> records = journal.replay(64);
  HGM_CHECK(records.ok());
  HGM_CHECK_EQ(records.value().size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(records.value()[1].type, JournalRecordType::EpochAdvance);
  journal.close();
}

HGM_TEST(persistence, journal_truncates_only_a_torn_tail) {
  TempDir dir("torn");
  const std::filesystem::path path = dir.file("journal.log");
  {
    Journal journal(path);
    HGM_CHECK(journal.open().ok());
    HGM_CHECK(journal.append(JournalRecordType::BootRecord, payload_of("one"), 1).ok());
    HGM_CHECK(journal.append(JournalRecordType::EpochAdvance, payload_of("two"), 2).ok());
    journal.close();
  }
  const std::uint64_t full = size_of(path);
  // Simulate a crash in the middle of a third record.
  JournalRecord torn;
  torn.type = JournalRecordType::HotspotCommit;
  torn.sequence = 3;
  torn.at = 3;
  torn.payload = payload_of("three");
  std::vector<std::byte> encoded;
  encode_journal_record(torn, encoded);
  append_bytes(path, std::span<const std::byte>(encoded.data(), encoded.size() / 2));
  HGM_CHECK(size_of(path) > full);

  Journal journal(path);
  HGM_CHECK(journal.open().ok());
  HGM_CHECK(journal.report().truncated_tail);
  HGM_CHECK_EQ(journal.report().records, 2ull);
  HGM_CHECK_EQ(journal.report().discarded_tail_bytes, static_cast<std::uint64_t>(encoded.size() / 2));
  HGM_CHECK_EQ(size_of(path), full);
  Result<std::vector<JournalRecord>> records = journal.replay(64);
  HGM_CHECK(records.ok());
  HGM_CHECK_EQ(records.value().size(), static_cast<std::size_t>(2));
  journal.close();
}

HGM_TEST(persistence, journal_refuses_to_grow_past_its_bound) {
  TempDir dir("bound");
  Journal journal(dir.file("journal.log"), 256);
  HGM_CHECK(journal.open().ok());
  Status status = Status::success();
  for (int i = 0; i < 64 && status.ok(); ++i) {
    status = journal.append(JournalRecordType::HotspotCommit, payload_of("0123456789012345678901234567890123456789"), 1);
  }
  HGM_CHECK(!status.ok());
  HGM_CHECK_EQ(status.code(), ErrorCode::DurableGrowthExceeded);
  journal.close();
}

HGM_TEST(persistence, journal_refuses_oversized_records) {
  TempDir dir("oversize");
  Journal journal(dir.file("journal.log"));
  HGM_CHECK(journal.open().ok());
  std::vector<std::byte> huge(Limits::kMaxJournalRecordBytes + 1, std::byte{0});
  const Status status = journal.append(JournalRecordType::HotspotCommit, std::span<const std::byte>(huge.data(), huge.size()), 1);
  HGM_CHECK_EQ(status.code(), ErrorCode::BoundExceeded);
  journal.close();
}

HGM_TEST(persistence, durable_state_round_trips) {
  const DurableState state = sample_state(BootId::from(9), CoordinatorEpoch::from(4), 12);
  std::vector<std::byte> bytes;
  encode_durable_state(state, bytes);
  DurableState decoded;
  HGM_CHECK(decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
  HGM_CHECK_EQ(decoded.written_by_boot, BootId::from(9));
  HGM_CHECK_EQ(decoded.epoch, CoordinatorEpoch::from(4));
  HGM_CHECK_EQ(decoded.evaluations, 12ull);
  HGM_CHECK_EQ(decoded.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decoded.hotspots.front().key, std::string("hs-0000000000000001"));
  HGM_CHECK_EQ(decoded.interventions.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decoded.interventions.front().kind, MitigationKind::AdmissionReduction);
}

HGM_TEST(persistence, durable_state_rejects_truncation_and_trailing_bytes) {
  const DurableState state = sample_state(BootId::from(1), CoordinatorEpoch::from(1), 1);
  std::vector<std::byte> bytes;
  encode_durable_state(state, bytes);
  DurableState decoded;
  HGM_CHECK(!decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size() / 2), decoded).ok());
  bytes.push_back(std::byte{0});
  HGM_CHECK(!decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
}

HGM_TEST(persistence, durable_state_rejects_absurd_counts) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u32(kDurableFormatVersion);
    writer.u64(1);
    writer.u64(1);
    writer.u64(0);
    writer.u64(0);
    writer.i64(0);
    writer.u32(0xFFFFFFFFu);
  }
  DurableState decoded;
  HGM_CHECK(!decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
}

HGM_TEST(persistence, store_commits_and_recovers) {
  TempDir dir("store");
  StoreConfig config;
  config.directory = dir.path;
  Store store(config);
  HGM_CHECK(store.open().ok());
  const DurableState state = sample_state(BootId::from(3), CoordinatorEpoch::from(2), 5);
  HGM_CHECK(store.commit(state, {JournalRecord{JournalRecordType::EvaluationCheckpoint, 0, 10, {}}}).ok());

  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().last_epoch, CoordinatorEpoch::from(2));
  HGM_CHECK_EQ(recovered.value().previous_boot, BootId::from(3));
  HGM_CHECK_EQ(recovered.value().state.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK(!recovered.value().publisher_authority_restored);
  HGM_CHECK(!recovered.value().evidence_freshness_restored);
  HGM_CHECK(!recovered.value().leases_restored);
  HGM_CHECK(recovered.value().requires_revalidation);
  HGM_CHECK(recovered.value().discarded_live_authority > 0);
  HGM_CHECK_EQ(size_of(dir.file("journal.log")), 0ull);
  store.close();
}

HGM_TEST(persistence, fresh_boot_without_a_snapshot_is_not_a_failure) {
  TempDir dir("fresh");
  StoreConfig config;
  config.directory = dir.path;
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(!recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().outcome.code(), ErrorCode::MissingEvidence);
  HGM_CHECK(recovered.value().requires_revalidation);
  store.close();
}

HGM_TEST(persistence, corrupt_snapshot_is_rejected) {
  TempDir dir("corrupt");
  StoreConfig config;
  config.directory = dir.path;
  {
    Store store(config);
    HGM_CHECK(store.open().ok());
    HGM_CHECK(store.commit(sample_state(BootId::from(1), CoordinatorEpoch::from(1), 1), {}).ok());
    store.close();
  }
  const std::filesystem::path snapshot = dir.file("state.bin");
  const std::uint64_t size = size_of(snapshot);
  HGM_CHECK(size > 40);
  {
    std::fstream stream(snapshot, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(static_cast<std::streamoff>(size - 4));
    const char bad = 'X';
    stream.write(&bad, 1);
  }
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(!recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().outcome.code(), ErrorCode::PersistenceCorrupt);
  store.close();
}

HGM_TEST(persistence, truncated_snapshot_is_rejected) {
  TempDir dir("truncated");
  StoreConfig config;
  config.directory = dir.path;
  {
    Store store(config);
    HGM_CHECK(store.open().ok());
    HGM_CHECK(store.commit(sample_state(BootId::from(1), CoordinatorEpoch::from(1), 1), {}).ok());
    store.close();
  }
  const std::filesystem::path snapshot = dir.file("state.bin");
  truncate_to(snapshot, size_of(snapshot) - 5);
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(!recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().outcome.code(), ErrorCode::PersistenceTruncated);
  store.close();
}

HGM_TEST(persistence, unsupported_durable_version_is_refused) {
  TempDir dir("version");
  StoreConfig config;
  config.directory = dir.path;
  {
    Store store(config);
    HGM_CHECK(store.open().ok());
    HGM_CHECK(store.commit(sample_state(BootId::from(1), CoordinatorEpoch::from(1), 1), {}).ok());
    store.close();
  }
  const std::filesystem::path snapshot = dir.file("state.bin");
  {
    std::fstream stream(snapshot, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(4);
    const char future[4] = {static_cast<char>(99), 0, 0, 0};
    stream.write(future, 4);
  }
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(!recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().outcome.code(), ErrorCode::PersistenceVersionUnsupported);
  HGM_CHECK(!recovered.value().format_supported);
  store.close();
}

HGM_TEST(persistence, a_leftover_temporary_snapshot_is_discarded) {
  TempDir dir("temp");
  StoreConfig config;
  config.directory = dir.path;
  {
    Store store(config);
    HGM_CHECK(store.open().ok());
    HGM_CHECK(store.commit(sample_state(BootId::from(1), CoordinatorEpoch::from(1), 1), {}).ok());
    store.close();
  }
  const std::filesystem::path snapshot = dir.file("state.bin");
  const std::uint64_t good_size = size_of(snapshot);
  {
    std::ofstream stream(dir.file("state.bin.tmp"), std::ios::binary);
    const char junk[16] = "not a snapshot";
    stream.write(junk, 14);
  }
  Store store(config);
  HGM_CHECK(store.open().ok());
  HGM_CHECK(!std::filesystem::exists(dir.file("state.bin.tmp")));
  HGM_CHECK_EQ(size_of(snapshot), good_size);
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(recovered.value().recovered);
  store.close();
}

HGM_TEST(persistence, a_committed_snapshot_survives_journal_corruption) {
  TempDir dir("journal-corrupt");
  StoreConfig config;
  config.directory = dir.path;
  Store store(config);
  HGM_CHECK(store.open().ok());
  const DurableState state = sample_state(BootId::from(5), CoordinatorEpoch::from(8), 42);
  HGM_CHECK(store.commit(state, {JournalRecord{JournalRecordType::EvaluationCheckpoint, 0, 1, {}}}).ok());
  // Leave a damaged record behind and recover again.
  {
    JournalRecord record;
    record.type = JournalRecordType::HotspotCommit;
    record.sequence = 99;
    record.payload = payload_of("partial");
    std::vector<std::byte> encoded;
    encode_journal_record(record, encoded);
    append_bytes(dir.file("journal.log"), std::span<const std::byte>(encoded.data(), encoded.size() - 3));
  }
  Store reopened(config);
  HGM_CHECK(reopened.open().ok());
  HGM_CHECK(reopened.journal_report().truncated_tail);
  Result<RecoveredState> recovered = reopened.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().last_epoch, CoordinatorEpoch::from(8));
  HGM_CHECK(recovered.value().discarded_tail_bytes > 0);
  reopened.close();
}

HGM_TEST(persistence, journal_epoch_advance_is_replayed) {
  TempDir dir("epoch-replay");
  StoreConfig config;
  config.directory = dir.path;
  {
    Store store(config);
    HGM_CHECK(store.open().ok());
    DurableState state = sample_state(BootId::from(1), CoordinatorEpoch::from(3), 1);
    state.journal_sequence = 0;
    std::vector<std::byte> payload;
    {
      ByteWriter writer(payload);
      writer.u64(9);
    }
    HGM_CHECK(store.commit(state, {JournalRecord{JournalRecordType::EpochAdvance, 0, 5, payload}}).ok());
    store.close();
  }
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(recovered.value().recovered);
  store.close();
}
