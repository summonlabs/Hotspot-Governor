// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/metrics.hpp"

namespace hgm {

std::string Metrics::render() const {
  std::string out;
  out.reserve(512);
  const auto add = [&out](const char* name, std::uint64_t value) {
    out.append(name);
    out.push_back('=');
    out.append(std::to_string(value));
    out.push_back(' ');
  };
  add("frames_received", frames_received);
  add("frames_accepted", frames_accepted);
  add("frames_rejected", frames_rejected);
  add("frames_duplicate", frames_duplicate);
  add("frames_stale", frames_stale);
  add("frames_malformed", frames_malformed);
  add("bytes_received", bytes_received);
  add("bytes_sent", bytes_sent);
  add("publisher_deaths", publisher_deaths);
  add("epoch_advances", epoch_advances);
  add("evaluations", evaluations);
  add("hotspots_confirmed", hotspots_confirmed);
  add("candidates_observed", candidates_observed);
  add("interventions_planned", interventions_planned);
  add("interventions_suppressed", interventions_suppressed);
  add("escalations", escalations);
  add("journal_appends", journal_appends);
  add("journal_replays", journal_replays);
  add("recoveries", recoveries);
  add("rejected_stale_epoch", rejected_stale_epoch);
  add("rejected_stale_generation", rejected_stale_generation);
  add("rejected_stale_incarnation", rejected_stale_incarnation);
  if (!out.empty()) out.pop_back();
  return out;
}

}  // namespace hgm
