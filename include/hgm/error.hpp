// Error taxonomy and Result carrier.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hgm {

// Every non-OK code corresponds to a concrete refusal path in the runtime.
// Codes are grouped by the boundary that produced them.
enum class ErrorCode : std::uint32_t {
  Ok = 0,

  // --- structural / argument ---
  InvalidArgument = 100,
  OutOfRange = 101,
  BoundExceeded = 102,
  ArithmeticOverflow = 103,
  NotImplemented = 104,

  // --- wire framing ---
  MalformedFrame = 200,
  TruncatedFrame = 201,
  IntegrityMismatch = 202,
  UnsupportedVersion = 203,
  FrameTooLarge = 204,

  // --- generation / fencing ---
  StaleGeneration = 300,
  StaleEpoch = 301,
  UnknownEpoch = 302,
  StaleIncarnation = 303,
  UnknownPublisher = 304,
  DuplicateFrame = 305,
  OutOfOrderSequence = 306,
  PublisherFenced = 307,

  // --- evidence admissibility ---
  StaleEvidence = 400,
  MissingEvidence = 401,
  ContradictoryEvidence = 402,
  DegradedTelemetry = 403,
  StaleTopology = 404,
  TopologyGenerationMismatch = 405,
  PathGenerationMismatch = 406,
  CapacityGenerationMismatch = 407,
  TrafficGenerationMismatch = 408,
  InsufficientCoverage = 409,
  PolicyExpired = 410,
  PolicyMissing = 411,
  UnknownResource = 412,
  UnknownPath = 413,
  UnknownRegion = 414,

  // --- decision / mitigation ---
  NotAuthorized = 500,
  ScopeViolation = 501,
  BudgetExceeded = 502,
  LocalizationLost = 503,
  AttributionUnavailable = 504,
  PersistenceNotMet = 505,

  // --- durability ---
  PersistenceCorrupt = 600,
  PersistenceTruncated = 601,
  PersistenceVersionUnsupported = 602,
  PersistenceIoError = 603,
  JournalRecordInvalid = 604,
  DurableGrowthExceeded = 605,

  // --- concurrency / lifecycle ---
  Cancelled = 700,
  ShuttingDown = 701,
  QueueFull = 702,
  WorkerFailed = 703,
  Timeout = 704,
  IoError = 705,
  NotConnected = 706,
  ConnectionClosed = 707,
  AlreadyStarted = 708,
  NotStarted = 709,

  Internal = 900,
};

std::string_view to_string(ErrorCode code) noexcept;

// True when the code means "the supplied work was produced against authority
// that is no longer current". Stale work must never mutate authoritative state.
bool is_stale(ErrorCode code) noexcept;

// True when the code describes a refusal caused by untrustworthy or
// unverifiable input bytes / state.
bool is_integrity(ErrorCode code) noexcept;

class Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  static Status success() { return Status{}; }
  static Status failure(ErrorCode code) { return Status(code, std::string()); }
  static Status failure(ErrorCode code, std::string detail) {
    return Status(code, std::move(detail));
  }

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  ErrorCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }

  // "stale-epoch: publisher 7 epoch 3 < current 4"
  std::string text() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string detail_;
};

template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Status status) : status_(std::move(status)) {}

  bool ok() const noexcept { return status_.ok() && value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  const Status& status() const noexcept { return status_; }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

 private:
  Status status_{};
  std::optional<T> value_{};
};

}  // namespace hgm
