// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/error.hpp"

namespace hgm {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::OutOfRange: return "out-of-range";
    case ErrorCode::BoundExceeded: return "bound-exceeded";
    case ErrorCode::ArithmeticOverflow: return "arithmetic-overflow";
    case ErrorCode::NotImplemented: return "not-implemented";
    case ErrorCode::MalformedFrame: return "malformed-frame";
    case ErrorCode::TruncatedFrame: return "truncated-frame";
    case ErrorCode::IntegrityMismatch: return "integrity-mismatch";
    case ErrorCode::UnsupportedVersion: return "unsupported-version";
    case ErrorCode::FrameTooLarge: return "frame-too-large";
    case ErrorCode::StaleGeneration: return "stale-generation";
    case ErrorCode::StaleEpoch: return "stale-epoch";
    case ErrorCode::UnknownEpoch: return "unknown-epoch";
    case ErrorCode::StaleIncarnation: return "stale-incarnation";
    case ErrorCode::UnknownPublisher: return "unknown-publisher";
    case ErrorCode::DuplicateFrame: return "duplicate-frame";
    case ErrorCode::OutOfOrderSequence: return "out-of-order-sequence";
    case ErrorCode::PublisherFenced: return "publisher-fenced";
    case ErrorCode::StaleEvidence: return "stale-evidence";
    case ErrorCode::MissingEvidence: return "missing-evidence";
    case ErrorCode::ContradictoryEvidence: return "contradictory-evidence";
    case ErrorCode::DegradedTelemetry: return "degraded-telemetry";
    case ErrorCode::StaleTopology: return "stale-topology";
    case ErrorCode::TopologyGenerationMismatch: return "topology-generation-mismatch";
    case ErrorCode::PathGenerationMismatch: return "path-generation-mismatch";
    case ErrorCode::CapacityGenerationMismatch: return "capacity-generation-mismatch";
    case ErrorCode::TrafficGenerationMismatch: return "traffic-generation-mismatch";
    case ErrorCode::InsufficientCoverage: return "insufficient-coverage";
    case ErrorCode::PolicyExpired: return "policy-expired";
    case ErrorCode::PolicyMissing: return "policy-missing";
    case ErrorCode::UnknownResource: return "unknown-resource";
    case ErrorCode::UnknownPath: return "unknown-path";
    case ErrorCode::UnknownRegion: return "unknown-region";
    case ErrorCode::NotAuthorized: return "not-authorized";
    case ErrorCode::ScopeViolation: return "scope-violation";
    case ErrorCode::BudgetExceeded: return "budget-exceeded";
    case ErrorCode::LocalizationLost: return "localization-lost";
    case ErrorCode::AttributionUnavailable: return "attribution-unavailable";
    case ErrorCode::PersistenceNotMet: return "persistence-not-met";
    case ErrorCode::PersistenceCorrupt: return "persistence-corrupt";
    case ErrorCode::PersistenceTruncated: return "persistence-truncated";
    case ErrorCode::PersistenceVersionUnsupported: return "persistence-version-unsupported";
    case ErrorCode::PersistenceIoError: return "persistence-io-error";
    case ErrorCode::JournalRecordInvalid: return "journal-record-invalid";
    case ErrorCode::DurableGrowthExceeded: return "durable-growth-exceeded";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ShuttingDown: return "shutting-down";
    case ErrorCode::QueueFull: return "queue-full";
    case ErrorCode::WorkerFailed: return "worker-failed";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::IoError: return "io-error";
    case ErrorCode::NotConnected: return "not-connected";
    case ErrorCode::ConnectionClosed: return "connection-closed";
    case ErrorCode::AlreadyStarted: return "already-started";
    case ErrorCode::NotStarted: return "not-started";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

bool is_stale(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleIncarnation:
    case ErrorCode::DuplicateFrame:
    case ErrorCode::OutOfOrderSequence:
    case ErrorCode::PublisherFenced:
    case ErrorCode::StaleEvidence:
    case ErrorCode::StaleTopology:
    case ErrorCode::TopologyGenerationMismatch:
    case ErrorCode::PathGenerationMismatch:
    case ErrorCode::CapacityGenerationMismatch:
    case ErrorCode::TrafficGenerationMismatch:
    case ErrorCode::PolicyExpired:
      return true;
    default:
      return false;
  }
}

bool is_integrity(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::MalformedFrame:
    case ErrorCode::TruncatedFrame:
    case ErrorCode::IntegrityMismatch:
    case ErrorCode::UnsupportedVersion:
    case ErrorCode::FrameTooLarge:
    case ErrorCode::PersistenceCorrupt:
    case ErrorCode::PersistenceTruncated:
    case ErrorCode::PersistenceVersionUnsupported:
    case ErrorCode::JournalRecordInvalid:
      return true;
    default:
      return false;
  }
}

std::string Status::text() const {
  if (ok()) return "ok";
  std::string out;
  out.reserve(to_string(code_).size() + detail_.size() + 2);
  out.append(to_string(code_));
  if (!detail_.empty()) {
    out.append(": ");
    out.append(detail_);
  }
  return out;
}

}  // namespace hgm
