// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/provenance.hpp"

namespace hgm {

std::string_view to_string(StreamKind kind) noexcept {
  switch (kind) {
    case StreamKind::Unknown: return "unknown";
    case StreamKind::Topology: return "topology";
    case StreamKind::Capacity: return "capacity";
    case StreamKind::Signals: return "signals";
    case StreamKind::Paths: return "paths";
    case StreamKind::Traffic: return "traffic";
    case StreamKind::Policy: return "policy";
  }
  return "unknown";
}

}  // namespace hgm
