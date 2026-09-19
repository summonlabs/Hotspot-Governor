// Append-only, CRC-protected write-ahead journal.
//
// Record layout: 48-byte header (magic, version, type, payload length,
// sequence, timestamp, payload CRC, record CRC) followed by the payload. A
// torn tail is detected on open and truncated; it is never replayed.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/time.hpp"

namespace hgm {

inline constexpr std::uint32_t kJournalMagic = 0x314A4748u;  // "HGJ1"
inline constexpr std::size_t kJournalHeaderBytes = 48;

enum class JournalRecordType : std::uint32_t {
  Unknown = 0,
  BootRecord = 1,
  EpochAdvance = 2,
  PolicyCommit = 3,
  HotspotCommit = 4,
  InterventionCommit = 5,
  EvaluationCheckpoint = 6,
  PublisherDeath = 7,
};

std::string_view to_string(JournalRecordType type) noexcept;

struct JournalRecord {
  JournalRecordType type = JournalRecordType::Unknown;
  Sequence sequence = 0;
  Millis at = kNoTime;
  std::vector<std::byte> payload;
};

struct JournalOpenReport {
  std::uint64_t valid_bytes = 0;
  std::uint64_t discarded_tail_bytes = 0;
  std::uint64_t records = 0;
  Sequence last_sequence = 0;
  bool truncated_tail = false;
};

class Journal {
 public:
  explicit Journal(std::filesystem::path path, std::uint64_t max_bytes = 1ull << 32);

  // Scans the journal, truncates any torn tail and prepares for appends.
  Status open();
  Status close();

  // Appends one record and flushes it to the OS. Refuses to grow past the
  // configured bound.
  Status append(JournalRecordType type, std::span<const std::byte> payload, Millis at);

  // Durability barrier for everything appended so far.
  Status sync();

  // Reads every intact record from the beginning of the file.
  Result<std::vector<JournalRecord>> replay(std::size_t max_records) const;

  // Drops all records. Called only after a snapshot that covers them has been
  // committed and made durable.
  Status reset();

  const JournalOpenReport& report() const noexcept { return report_; }
  std::uint64_t bytes() const noexcept { return bytes_; }
  Sequence last_sequence() const noexcept { return last_sequence_; }
  bool is_open() const noexcept { return open_; }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
  std::uint64_t max_bytes_ = 1ull << 32;
  std::uint64_t bytes_ = 0;
  Sequence last_sequence_ = 0;
  bool open_ = false;
  JournalOpenReport report_{};
};

// Encodes one record (header + payload). Exposed for tests and tooling.
void encode_journal_record(const JournalRecord& record, std::vector<std::byte>& out);

// Decodes one record from the front of the buffer. Returns the number of bytes
// consumed, or 0 when the buffer does not hold a complete, intact record.
std::size_t decode_journal_record(std::span<const std::byte> bytes, JournalRecord& out);

}  // namespace hgm
