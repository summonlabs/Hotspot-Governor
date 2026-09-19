// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/explanation.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hgm {
namespace {

class LineWriter {
 public:
  explicit LineWriter(const ExplanationOptions& options) : options_(options) {}

  void line(const std::string& text) {
    if (lines_.size() >= options_.max_lines) {
      truncated_ = true;
      return;
    }
    std::string bounded = text;
    if (bounded.size() > Limits::kMaxTextBytes) bounded.resize(Limits::kMaxTextBytes);
    const std::size_t projected = bytes_ + bounded.size() + 1;
    if (projected > options_.max_bytes) {
      truncated_ = true;
      return;
    }
    bytes_ = projected;
    lines_.push_back(std::move(bounded));
  }

  // Renders the collected lines and, when anything was dropped, guarantees the
  // truncation marker is present in full within the byte bound. The marker is
  // never itself clipped, and the result never exceeds max_bytes.
  std::string finish() {
    const std::string note = "... output truncated at the bound";
    std::string out;
    out.reserve(bytes_ + note.size() + 16);
    for (const std::string& entry : lines_) {
      out.append(entry);
      out.push_back('\n');
    }
    if (!out.empty()) out.pop_back();
    if (truncated_) {
      if (note.size() > options_.max_bytes) {
        out.assign(note.substr(0, options_.max_bytes));
        return out;
      }
      const std::size_t reserve = note.size() + (out.empty() ? 0 : 1);
      if (out.size() + reserve > options_.max_bytes) {
        out.resize(options_.max_bytes - reserve);
        // Do not leave a half-written line in front of the marker.
        const std::size_t last_newline = out.find_last_of('\n');
        if (last_newline != std::string::npos) out.resize(last_newline);
      }
      if (!out.empty()) out.push_back('\n');
      out.append(note);
    }
    return out;
  }

  std::size_t line_count() const noexcept { return lines_.size(); }

 private:
  ExplanationOptions options_;
  std::vector<std::string> lines_;
  std::size_t bytes_ = 0;
  bool truncated_ = false;
};

// Renders parts-per-million as a percentage with two decimals:
// 1,000,000 ppm -> "100.00%", 980,000 ppm -> "98.00%".
std::string ppm_text(std::uint32_t ppm) {
  const std::uint32_t scaled = ppm / 100u;  // hundredths of a percent
  std::string out = std::to_string(scaled / 100u);
  out.push_back('.');
  const std::uint32_t fraction = scaled % 100u;
  if (fraction < 10u) out.push_back('0');
  out.append(std::to_string(fraction));
  out.push_back('%');
  return out;
}

void render_binding(LineWriter& writer, const EvidenceBinding& binding) {
  std::string text = "binding: topology=";
  text.append(binding.topology.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.topology_generation.value()));
  text.append(" capacity=");
  text.append(binding.capacity.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.capacity_generation.value()));
  text.append(" signals=");
  text.append(binding.signals.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.signals_generation.value()));
  text.append(" paths=");
  text.append(binding.paths.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.path_generation.value()));
  text.append(" traffic=");
  text.append(binding.traffic.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.traffic_generation.value()));
  text.append(" policy=");
  text.append(binding.policy.hex());
  text.append(" gen=");
  text.append(std::to_string(binding.policy_generation.value()));
  writer.line(text);
}

void render_hotspot(LineWriter& writer, const Hotspot& hotspot, bool confirmed,
                    const ExplanationOptions& options) {
  std::string head = confirmed ? "hotspot " : "candidate ";
  head.append(hotspot.key);
  head.append(" scope=");
  head.append(to_string(hotspot.scope));
  head.append(" cause=");
  head.append(to_string(hotspot.cause));
  head.append(" severity=");
  head.append(to_string(hotspot.severity));
  head.append(" persistence_ms=");
  head.append(std::to_string(hotspot.persistence_ms));
  head.append(" samples=");
  head.append(std::to_string(hotspot.sample_count));
  head.append(" share=");
  head.append(ppm_text(hotspot.share_ppm));
  head.append(" regions=");
  head.append(std::to_string(hotspot.regions.size()));
  if (hotspot.saturated_truncated) head.append(" resources_truncated=yes");
  writer.line(head);

  std::string resources = "  resources(" + std::to_string(hotspot.saturated_count) + "):";
  for (std::size_t i = 0; i < hotspot.saturated_resources.size() && i < 8; ++i) {
    resources.push_back(' ');
    resources.append(hotspot.saturated_resources[i].hex());
  }
  if (hotspot.saturated_count > 8) resources.append(" ...");
  writer.line(resources);

  std::string peaks = "  peaks: utilization=" + ppm_text(hotspot.peak_utilization_ppm) +
                      " queue=" + ppm_text(hotspot.peak_queue_ppm) +
                      " buffer=" + ppm_text(hotspot.peak_buffer_ppm) +
                      " pressure=" + ppm_text(hotspot.peak_pressure_ppm);
  writer.line(peaks);

  std::string neighbours = "  healthy_neighbours(" + std::to_string(hotspot.healthy_neighbor_count) + "):";
  if (hotspot.healthy_neighbors.empty()) {
    neighbours.append(" none observed");
  } else {
    for (std::size_t i = 0; i < hotspot.healthy_neighbors.size() && i < options.max_neighbors; ++i) {
      neighbours.push_back(' ');
      neighbours.append(hotspot.healthy_neighbors[i].hex());
    }
  }
  writer.line(neighbours);

  std::string contributors = "  contributors(" + std::to_string(hotspot.contributor_count) +
                             ") attribution_confidence=" + ppm_text(hotspot.attribution_confidence_ppm) +
                             ":";
  if (hotspot.contributors.empty()) {
    contributors.append(" none");
  } else {
    for (std::size_t i = 0; i < hotspot.contributors.size() && i < options.max_contributors; ++i) {
      const Contributor& contributor = hotspot.contributors[i];
      contributors.append(" flow=");
      contributors.append(contributor.flow.hex());
      contributors.append("/path=");
      contributors.append(contributor.path.hex());
      contributors.append("@");
      contributors.append(ppm_text(contributor.share_ppm));
    }
  }
  writer.line(contributors);

  std::string demand = "  demand: attributed_bps=" + std::to_string(hotspot.attributed_demand_bps) +
                       " stale_bps=" + std::to_string(hotspot.unattributed_demand_bps);
  writer.line(demand);

  std::string remediation = "  remediation=";
  remediation.append(to_string(hotspot.remediation));
  if (!hotspot.suppression_reason.empty()) {
    remediation.append(" reason=");
    remediation.append(hotspot.suppression_reason);
  }
  writer.line(remediation);

  if (options.include_authority) {
    writer.line("  authority: " + hotspot.authority.render());
  }
}

void render_assessment(LineWriter& writer, const FabricAssessment& assessment,
                       const ExplanationOptions& options) {
  writer.line(std::string("verdict: ") + std::string(to_string(assessment.scope)) +
              " — " + assessment.scope_reason);
  render_binding(writer, assessment.binding);
  writer.line("coverage: covered=" + ppm_text(assessment.coverage_ppm) +
              " admissible=" + ppm_text(assessment.admissible_coverage_ppm) +
              " capacity=" + ppm_text(assessment.capacity_coverage_ppm) +
              " resources=" + std::to_string(assessment.resource_count) +
              " observed=" + std::to_string(assessment.observed_count) +
              " admissible=" + std::to_string(assessment.admissible_count) +
              " saturated=" + std::to_string(assessment.saturated_count) +
              " contradictory=" + std::to_string(assessment.contradictory_count) +
              " stale=" + std::to_string(assessment.stale_signal_count));
  writer.line("capacity: fabric=" + std::to_string(assessment.fabric_capacity_units) +
              " covered=" + std::to_string(assessment.covered_capacity_units) +
              " admissible=" + std::to_string(assessment.admissible_capacity_units) +
              " saturated=" + std::to_string(assessment.saturated_capacity_units) +
              " global_share=" + ppm_text(assessment.global_share_ppm));

  writer.line("hotspots=" + std::to_string(assessment.hotspots.size()) +
              " candidates=" + std::to_string(assessment.candidates.size()));
  for (const Hotspot& hotspot : assessment.hotspots) {
    render_hotspot(writer, hotspot, true, options);
  }
  for (const Hotspot& candidate : assessment.candidates) {
    render_hotspot(writer, candidate, false, options);
  }

  if (!assessment.pressures.empty()) {
    writer.line("pressures:");
    for (std::size_t i = 0; i < assessment.pressures.size() && i < options.max_pressures; ++i) {
      const ResourcePressure& pressure = assessment.pressures[i];
      std::string text = "  ";
      text.append(pressure.resource.hex());
      text.append(" util=");
      text.append(ppm_text(pressure.utilization_ppm));
      text.append(" queue=");
      text.append(ppm_text(pressure.queue_ppm));
      text.append(" buffer=");
      text.append(ppm_text(pressure.buffer_ppm));
      text.append(" quality=");
      text.append(to_string(pressure.quality));
      text.append(pressure.saturated ? " saturated" : "");
      text.append(pressure.contradictory ? " contradictory" : "");
      text.append(!pressure.admissible && !pressure.contradictory ? " inadmissible" : "");
      writer.line(text);
    }
  }

  if (assessment.escalation_required) {
    writer.line("escalation: required — " + assessment.escalation_reason);
  } else {
    writer.line("escalation: not required");
  }

  if (!assessment.suppressed.empty()) {
    writer.line("suppressed:");
    for (const SuppressedAction& action : assessment.suppressed) {
      writer.line(std::string("  ") + std::string(to_string(action.kind)) + " " + action.hotspot.hex() +
                  " — " + action.reason);
    }
  }

  if (!assessment.diagnostics.empty()) {
    writer.line("diagnostics:");
    for (const Diagnostic& diagnostic : assessment.diagnostics) {
      writer.line(std::string("  ") + std::string(to_string(diagnostic.code)) + " — " +
                  diagnostic.detail);
    }
  }

  if (options.include_authority) {
    writer.line("authority: " + assessment.authority.render());
  }
}

void render_plan(LineWriter& writer, const InterventionPlan& plan,
                 const ExplanationOptions& options) {
  render_binding(writer, plan.binding);
  writer.line("intents=" + std::to_string(plan.intents.size()) +
              " suppressed=" + std::to_string(plan.suppressed.size()) +
              " rejected=" + std::to_string(plan.rejected_intent_count) +
              " plan_scope=" + ppm_text(plan.plan_scope_share_ppm));
  if (plan.escalation_required) {
    writer.line("escalation: required — " + plan.escalation_reason);
  }
  for (const MitigationIntent& intent : plan.intents) {
    std::string head = "intent " + intent.id.hex() + " kind=" + std::string(to_string(intent.kind));
    head.append(" hotspot=");
    head.append(intent.hotspot.hex());
    head.append(" scope=");
    head.append(to_string(intent.hotspot_scope));
    head.append(" severity=");
    head.append(to_string(intent.severity));
    head.append(" scope_share=");
    head.append(ppm_text(intent.scope_share_ppm));
    if (intent.rate_delta_ppm != 0) {
      head.append(" rate_delta_ppm=");
      head.append(std::to_string(intent.rate_delta_ppm));
    }
    writer.line(head);
    std::string affected = "  affected: resources=" + std::to_string(intent.affected_resources.size()) +
                           " paths=" + std::to_string(intent.affected_paths.size()) +
                           " flows=" + std::to_string(intent.affected_flows.size());
    writer.line(affected);
    if (!intent.rationale.empty()) writer.line("  rationale: " + intent.rationale);
    if (options.include_authority) writer.line("  authority: " + intent.authority.render());
  }
  if (!plan.suppressed.empty()) {
    writer.line("suppressed:");
    for (const SuppressedAction& action : plan.suppressed) {
      writer.line(std::string("  ") + std::string(to_string(action.kind)) + " " + action.hotspot.hex() +
                  " — " + action.reason);
    }
  }
  if (options.include_authority) writer.line("authority: " + plan.authority.render());
}

}  // namespace

std::string explain(const FabricAssessment& assessment, const ExplanationOptions& options) {
  LineWriter writer(options);
  render_assessment(writer, assessment, options);
  return writer.finish();
}

std::string explain(const InterventionPlan& plan, const ExplanationOptions& options) {
  LineWriter writer(options);
  render_plan(writer, plan, options);
  return writer.finish();
}

std::string explain(const FabricAssessment& assessment, const InterventionPlan& plan,
                    const ExplanationOptions& options) {
  // One writer, one line budget and one byte budget across both sections, so
  // the truncation marker can never be clipped by a nested budget.
  LineWriter writer(options);
  render_assessment(writer, assessment, options);
  writer.line("-- plan --");
  render_plan(writer, plan, options);
  return writer.finish();
}

}  // namespace hgm
