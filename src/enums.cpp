// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/enums.hpp"

namespace hgm {

std::string_view to_string(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Unknown: return "unknown";
    case ResourceKind::Switch: return "switch";
    case ResourceKind::Port: return "port";
    case ResourceKind::Link: return "link";
    case ResourceKind::Queue: return "queue";
    case ResourceKind::Buffer: return "buffer";
    case ResourceKind::Host: return "host";
    case ResourceKind::Optical: return "optical";
    case ResourceKind::Other: return "other";
  }
  return "unknown";
}

std::string_view to_string(TelemetryQuality quality) noexcept {
  switch (quality) {
    case TelemetryQuality::Unknown: return "unknown";
    case TelemetryQuality::Missing: return "missing";
    case TelemetryQuality::Suspect: return "suspect";
    case TelemetryQuality::Degraded: return "degraded";
    case TelemetryQuality::Healthy: return "healthy";
  }
  return "unknown";
}

std::string_view to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::None: return "none";
    case Severity::Low: return "low";
    case Severity::Moderate: return "moderate";
    case Severity::High: return "high";
    case Severity::Critical: return "critical";
  }
  return "none";
}

std::string_view to_string(CongestionScope scope) noexcept {
  switch (scope) {
    case CongestionScope::Unknown: return "unknown";
    case CongestionScope::None: return "none";
    case CongestionScope::Localized: return "localized";
    case CongestionScope::Regional: return "regional";
    case CongestionScope::Global: return "global";
  }
  return "unknown";
}

std::string_view to_string(SaturationCause cause) noexcept {
  switch (cause) {
    case SaturationCause::Unknown: return "unknown";
    case SaturationCause::CapacityExhaustion: return "capacity-exhaustion";
    case SaturationCause::QueueBuildup: return "queue-buildup";
    case SaturationCause::BufferExhaustion: return "buffer-exhaustion";
    case SaturationCause::Microburst: return "microburst";
    case SaturationCause::FanInContention: return "fan-in-contention";
    case SaturationCause::StructuralConvergence: return "structural-convergence";
  }
  return "unknown";
}

std::string_view to_string(MitigationKind kind) noexcept {
  switch (kind) {
    case MitigationKind::None: return "none";
    case MitigationKind::FlowRelocation: return "flow-relocation";
    case MitigationKind::PathRebalance: return "path-rebalance";
    case MitigationKind::AdmissionReduction: return "admission-reduction";
    case MitigationKind::RatePacingChange: return "rate-pacing-change";
    case MitigationKind::ResourceIsolation: return "resource-isolation";
    case MitigationKind::EscalateGlobalCongestion: return "escalate-global-congestion";
  }
  return "none";
}

}  // namespace hgm
