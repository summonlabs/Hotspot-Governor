// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/journal.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "file_io.hpp"
#include "hgm/byteio.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"
#include "hgm/version.hpp"

namespace hgm {

std::string_view to_string(JournalRecordType type) noexcept {
  switch (type) {
    case JournalRecordType::Unknown: return "unknown";
    case JournalRecordType::BootRecord: return "boot";
    case JournalRecordType::EpochAdvance: return "epoch-advance";
    case JournalRecordType::PolicyCommit: return "policy-commit";
    case JournalRecordType::HotspotCommit: return "hotspot-commit";
    case JournalRecordType::InterventionCommit: return "intervention-commit";
    case JournalRecordType::EvaluationCheckpoint: return "evaluation-checkpoint";
    case JournalRecordType::PublisherDeath: return "publisher-death";
  }
  return "unknown";
}

void encode_journal_record(const JournalRecord& record, std::vector<std::byte>& out) {
  out.clear();
  out.reserve(kJournalHeaderBytes + record.payload.size());
  ByteWriter writer(out);
  writer.u32(kJournalMagic);
  writer.u32(kDurableFormatVersion);
  writer.u32(static_cast<std::uint32_t>(record.type));
  writer.u32(static_cast<std::uint32_t>(record.payload.size()));
  writer.u64(record.sequence);
  writer.i64(record.at);
  writer.u64(crc64(std::span<const std::byte>(record.payload.data(), record.payload.size())));
  writer.u64(crc64(std::span<const std::byte>(out.data(), out.size())));
  writer.raw(std::span<const std::byte>(record.payload.data(), record.payload.size()));
}

std::size_t decode_journal_record(std::span<const std::byte> bytes, JournalRecord& out) {
  if (bytes.size() < kJournalHeaderBytes) return 0;
  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t type = 0;
  std::uint32_t payload_length = 0;
  if (!reader.u32(magic) || !reader.u32(version) || !reader.u32(type) || !reader.u32(payload_length)) {
    return 0;
  }
  if (magic != kJournalMagic) return 0;
  if (version != kDurableFormatVersion) return 0;
  if (static_cast<std::size_t>(payload_length) > Limits::kMaxJournalRecordBytes) return 0;

  const std::size_t total = kJournalHeaderBytes + static_cast<std::size_t>(payload_length);
  if (bytes.size() < total) return 0;

  const std::uint64_t expected_record_crc = crc64(bytes.first(kJournalHeaderBytes - 8));
  std::uint64_t sequence = 0;
  Millis at = kNoTime;
  std::uint64_t payload_crc = 0;
  std::uint64_t record_crc = 0;
  if (!reader.u64(sequence) || !reader.i64(at) || !reader.u64(payload_crc) ||
      !reader.u64(record_crc)) {
    return 0;
  }
  if (record_crc != expected_record_crc) return 0;
  const std::span<const std::byte> payload = bytes.subspan(kJournalHeaderBytes, payload_length);
  if (crc64(payload) != payload_crc) return 0;

  out.type = static_cast<JournalRecordType>(type);
  out.sequence = sequence;
  out.at = at;
  out.payload.assign(payload.begin(), payload.end());
  return total;
}

Journal::Journal(std::filesystem::path path, std::uint64_t max_bytes)
    : path_(std::move(path)), max_bytes_(max_bytes) {}

Status Journal::open() {
  if (open_) return Status::failure(ErrorCode::AlreadyStarted, "journal is already open");

  std::vector<std::byte> bytes;
  if (file_exists(path_)) {
    Result<std::vector<std::byte>> read = read_file_all(path_, max_bytes_);
    if (!read.ok()) return read.status();
    bytes = std::move(read).value();
  } else {
    // Create the journal eagerly: a store that commits an empty record set must
    // still have a durable, synchronisable file.
    Status created =
        write_file_all(path_, std::span<const std::byte>());
    if (!created.ok()) return created;
  }

  std::size_t offset = 0;
  std::uint64_t records = 0;
  Sequence last_sequence = 0;
  JournalRecord record;
  while (offset < bytes.size()) {
    const std::size_t consumed =
        decode_journal_record(std::span<const std::byte>(bytes.data() + offset, bytes.size() - offset), record);
    if (consumed == 0) break;
    offset += consumed;
    ++records;
    last_sequence = std::max(last_sequence, record.sequence);
  }

  report_.valid_bytes = offset;
  report_.discarded_tail_bytes = static_cast<std::uint64_t>(bytes.size()) - offset;
  report_.records = records;
  report_.last_sequence = last_sequence;
  report_.truncated_tail = offset != bytes.size();

  if (report_.truncated_tail) {
    Status status = truncate_file(path_, report_.valid_bytes);
    if (!status.ok()) return status;
  }
  bytes_ = report_.valid_bytes;
  last_sequence_ = last_sequence;
  open_ = true;
  return Status::success();
}

Status Journal::close() {
  if (!open_) return Status::success();
  open_ = false;
  return Status::success();
}

Status Journal::append(JournalRecordType type, std::span<const std::byte> payload, Millis at) {
  if (!open_) return Status::failure(ErrorCode::NotStarted, "journal is not open");
  if (payload.size() > Limits::kMaxJournalRecordBytes) {
    return Status::failure(ErrorCode::BoundExceeded, "journal record exceeds the bound");
  }
  JournalRecord record;
  record.type = type;
  record.sequence = last_sequence_ + 1;
  record.at = at;
  record.payload.assign(payload.begin(), payload.end());

  std::vector<std::byte> encoded;
  encode_journal_record(record, encoded);
  if (bytes_ + encoded.size() > max_bytes_) {
    return Status::failure(ErrorCode::DurableGrowthExceeded, "journal would exceed its configured bound");
  }
  Status status = append_file_all(path_, std::span<const std::byte>(encoded.data(), encoded.size()));
  if (!status.ok()) return status;
  bytes_ += encoded.size();
  last_sequence_ = record.sequence;
  return Status::success();
}

Status Journal::sync() {
  if (!open_) return Status::failure(ErrorCode::NotStarted, "journal is not open");
  return fsync_file(path_);
}

Result<std::vector<JournalRecord>> Journal::replay(std::size_t max_records) const {
  std::vector<JournalRecord> records;
  if (!file_exists(path_)) return records;
  Result<std::vector<std::byte>> read = read_file_all(path_, max_bytes_);
  if (!read.ok()) return read.status();
  const std::vector<std::byte> bytes = std::move(read).value();

  std::size_t offset = 0;
  JournalRecord record;
  while (offset < bytes.size() && records.size() < max_records) {
    const std::size_t consumed =
        decode_journal_record(std::span<const std::byte>(bytes.data() + offset, bytes.size() - offset), record);
    if (consumed == 0) break;
    offset += consumed;
    records.push_back(record);
  }
  return records;
}

Status Journal::reset() {
  if (!open_) return Status::failure(ErrorCode::NotStarted, "journal is not open");
  Status status = write_file_all(path_, std::span<const std::byte>());
  if (!status.ok()) return status;
  bytes_ = 0;
  return Status::success();
}

}  // namespace hgm
