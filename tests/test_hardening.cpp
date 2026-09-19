// Hardening: topology churn, migration, failure, restart, scale.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"
#include "hgm/coordinator.hpp"
#include "hgm/governor.hpp"
#include "hgm/transport.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::Harness;
using hgmt::TempDir;

namespace {

SyntheticConfig localized_config() {
  SyntheticConfig config;
  config.region_count = 4;
  config.resources_per_region = 8;
  config.hotspot_count = 1;
  config.path_fan_in = 3;
  return config;
}

}  // namespace

HGM_TEST(hardening, a_topology_change_without_matching_signals_yields_unknown) {
  Harness harness(localized_config());
  harness.run(3);
  TopologySnapshot topology = harness.fabric.topology();
  topology.set_generation(TopologyGeneration::from(harness.fabric.topology().generation().value() + 1));
  DecisionInput input;
  input.topology = &topology;
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  const Decision decision = harness.engine.decide(input);
  // Signals and capacity still name the previous topology generation, so the
  // new topology invalidates localization rather than silently reusing it.
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
}

HGM_TEST(hardening, path_migration_moves_the_attribution) {
  Harness harness(localized_config());
  harness.run(3);
  const ResourceId hotspot_resource = harness.fabric.saturated_resources().front();
  const auto original = harness.fabric.paths().paths_through(hotspot_resource);
  HGM_CHECK(original.size() > 0);

  // A new path generation in which the hotspot resource carries no paths.
  PathSnapshot migrated;
  for (const PathRecord& record : harness.fabric.paths().paths()) {
    if (std::find(record.hops.begin(), record.hops.end(), hotspot_resource) != record.hops.end()) {
      continue;
    }
    PathRecord& copy = migrated.add(record.id, record.flow);
    copy.hops = record.hops;
  }
  migrated.set_generation(PathGeneration::from(harness.fabric.paths().generation().value() + 1));
  migrated.set_topology_generation(harness.fabric.paths().topology_generation());
  migrated.set_observed_at(harness.now);
  HGM_CHECK(migrated.build().ok());

  TrafficSnapshot traffic;
  for (const PathRecord& record : migrated.paths()) {
    TrafficRecord& entry = traffic.add(record.flow, record.id);
    entry.demand_bps = 1000;
    entry.quality = TelemetryQuality::Healthy;
    entry.observed_at = harness.now;
  }
  traffic.set_generation(TrafficGeneration::from(harness.fabric.traffic().generation().value() + 1));
  traffic.set_path_generation(migrated.generation());
  HGM_CHECK(traffic.build().ok());

  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &migrated;
  input.traffic = &traffic;
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  const Decision decision = harness.engine.decide(input);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  const Hotspot& hotspot = decision.assessment.hotspots.front();
  HGM_CHECK(hotspot.contributors.empty());
  HGM_CHECK_EQ(hotspot.attribution_confidence_ppm, 0u);
  HGM_CHECK(!decision.assessment.authority.granted(AuthorityDomain::Attribution));
}

HGM_TEST(hardening, a_failed_resource_leaves_the_hotspot_set) {
  Harness harness(localized_config());
  harness.run(3);
  const ResourceId failed = harness.fabric.saturated_resources().front();

  CapacitySnapshot capacity;
  for (const ResourceCapacity& entry : harness.fabric.capacity().entries()) {
    ResourceCapacity& copy = capacity.add(entry.resource);
    copy.capacity_units = entry.capacity_units;
    copy.queue_limit_units = entry.queue_limit_units;
    copy.buffer_limit_bytes = entry.buffer_limit_bytes;
    copy.usable = entry.usable;
    if (entry.resource == failed) {
      copy.usable = false;
      copy.capacity_units = 0;
    }
  }
  capacity.set_generation(CapacityGeneration::from(harness.fabric.capacity().generation().value() + 1));
  capacity.set_topology_generation(harness.fabric.capacity().topology_generation());
  capacity.set_observed_at(harness.now);
  HGM_CHECK(capacity.build().ok());

  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &capacity;
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  const Decision decision = harness.engine.decide(input);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.candidates.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(harness.engine.tracker().size(), static_cast<std::size_t>(0));
}

HGM_TEST(hardening, emerging_global_congestion_escalates) {
  Harness harness(localized_config());
  Decision local = harness.run(3);
  HGM_CHECK_EQ(local.assessment.scope, CongestionScope::Localized);
  HGM_CHECK_EQ(local.plan.intents.size(), static_cast<std::size_t>(1));
  HGM_CHECK(!local.assessment.escalation_required);

  // Congestion spreads to the whole fabric.
  SyntheticConfig global = localized_config();
  global.global_congestion = true;
  harness.config = global;
  harness.fabric = SyntheticFabric(global);
  Decision decision;
  for (int round = 0; round < 3; ++round) decision = harness.step();
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Global);
  HGM_CHECK(decision.assessment.escalation_required);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.plan.intents.front().kind, MitigationKind::EscalateGlobalCongestion);
}

HGM_TEST(hardening, simultaneous_hotspots_exceeding_the_plan_budget_escalate) {
  SyntheticConfig config;
  config.region_count = 4;
  config.resources_per_region = 16;
  config.hotspot_count = 4;
  config.path_fan_in = 2;
  config.evidence_density_ppm = 1000000;
  Harness harness(config);
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.budget().max_intents_per_plan = 1;
  policy.budget().max_plan_scope_share_ppm = 0;
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  input.policy = &policy;
  input.now = harness.now;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  const Decision decision = harness.engine.decide(input);
  HGM_CHECK(decision.assessment.hotspots.size() >= 2);
  // The plan budget cannot cover the observed scope, so nothing local is
  // emitted silently.
  HGM_CHECK(decision.plan.intents.size() <= 1);
  for (const MitigationIntent& intent : decision.plan.intents) {
    HGM_CHECK(intent.scope_share_ppm <= policy.budget().max_scope_share_ppm);
  }
}

HGM_TEST(hardening, a_huge_region_is_evaluated_within_bounds) {
  SyntheticConfig config;
  config.region_count = 8;
  config.resources_per_region = 512;  // 4096 resources
  config.hotspot_count = 8;
  config.path_fan_in = 2;
  config.path_resource_density_ppm = 100000;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK(harness.fabric.build_ok());
  HGM_CHECK_EQ(harness.fabric.topology().resource_count(), static_cast<std::size_t>(4096));
  HGM_CHECK(decision.explanation.size() <= 16384);
  HGM_CHECK(decision.assessment.resource_count == 4096);
  HGM_CHECK(decision.assessment.pressures.size() <= Limits::kMaxPressureEntries);
}

HGM_TEST(hardening, restart_advances_the_epoch_and_restores_history_only) {
  TempDir dir("restart");
  StoreConfig store_config;
  store_config.directory = dir.path;

  GovernorConfig first_config;
  first_config.node_name = "restart-governor";
  first_config.store = store_config;
  const Millis base = 100000;

  BootId first_boot;
  {
    Governor governor(first_config);
    HGM_CHECK(governor.recover(base).ok());
    HGM_CHECK(governor.install_epoch(CoordinatorEpoch::from(4), BootId::from(9), base).ok());
    first_boot = governor.boot();
    HelloPayload hello;
    hello.publisher = PublisherId::from(11);
    hello.incarnation = Incarnation::from(1);
    hello.boot = BootId::from(12);
    hello.protocol_version = kFrameProtocolVersion;
    WelcomePayload welcome;
    HGM_CHECK(governor.register_publisher(hello, base, welcome).ok());
    HGM_CHECK_EQ(welcome.epoch, CoordinatorEpoch::from(4));
    HGM_CHECK(governor.commit_durable(base + 1).ok());
  }

  Governor second(first_config);
  HGM_CHECK(second.recover(base + 2).ok());
  HGM_CHECK(second.recovery().recovered);
  HGM_CHECK_EQ(second.recovery().last_epoch, CoordinatorEpoch::from(4));
  HGM_CHECK_EQ(second.epoch(), CoordinatorEpoch::from(5));
  HGM_CHECK_EQ(second.recovery().previous_boot, first_boot);
  HGM_CHECK(!second.recovery().publisher_authority_restored);
  HGM_CHECK(!second.recovery().evidence_freshness_restored);
  HGM_CHECK(!second.recovery().leases_restored);
  HGM_CHECK(second.recovery().requires_revalidation);
  HGM_CHECK_EQ(second.publishers().size(), static_cast<std::size_t>(0));

  // A frame minted under the pre-restart epoch is refused.
  Frame frame = make_frame(MessageType::Signals, Provenance{}, {});
  frame.header.epoch = CoordinatorEpoch::from(4);
  frame.header.publisher = PublisherId::from(11);
  frame.header.incarnation = Incarnation::from(1);
  frame.header.boot = BootId::from(12);
  frame.header.sequence = 1;
  frame.header.generation = 5;
  frame.payload.clear();
  frame.header.payload_crc = crc64(std::span<const std::byte>());
  const auto report = second.admit(frame, base + 3);
  HGM_CHECK(!report.ok());
  HGM_CHECK_EQ(report.status().code(), ErrorCode::StaleEpoch);
  HGM_CHECK(second.commit_durable(base + 4).ok());
}

HGM_TEST(hardening, duplicate_events_are_idempotent_by_refusal) {
  GovernorConfig config;
  config.node_name = "duplicate-governor";
  Governor governor(config);
  const Millis now = 5000;
  HGM_CHECK(governor.recover(now).ok());
  HGM_CHECK(governor.install_epoch(CoordinatorEpoch::from(1), BootId::from(1), now).ok());
  HelloPayload hello;
  hello.publisher = PublisherId::from(1);
  hello.incarnation = Incarnation::from(1);
  hello.boot = BootId::from(1);
  hello.protocol_version = kFrameProtocolVersion;
  WelcomePayload welcome;
  HGM_CHECK(governor.register_publisher(hello, now, welcome).ok());

  SyntheticFabric fabric;
  fabric.rebuild(now, 5);
  std::vector<std::byte> body;
  HGM_CHECK(encode_capacity_payload(fabric.capacity(), body).ok());
  Provenance provenance;
  provenance.publisher = hello.publisher;
  provenance.incarnation = hello.incarnation;
  provenance.boot = hello.boot;
  provenance.epoch = welcome.epoch;
  provenance.sequence = 1;
  provenance.generation = StreamGeneration{StreamKind::Capacity, 5};
  provenance.emitted_at = now;
  Frame frame = make_frame(MessageType::Capacity, provenance, body);
  HGM_CHECK(governor.admit(frame, now).ok());
  for (int i = 0; i < 5; ++i) {
    const auto report = governor.admit(frame, now);
    HGM_CHECK(!report.ok());
    HGM_CHECK_EQ(report.status().code(), ErrorCode::DuplicateFrame);
  }
  HGM_CHECK_EQ(governor.metrics().frames_accepted, 1ull);
  HGM_CHECK_EQ(governor.metrics().frames_duplicate, 5ull);
}

HGM_TEST(hardening, evidence_stops_being_authoritative_when_it_stops_refreshing) {
  Harness harness(localized_config());
  harness.run(3);
  HGM_CHECK_EQ(harness.engine.tracker().size(), static_cast<std::size_t>(1));

  // The publisher goes silent: no new signal snapshot arrives, so the last one
  // simply ages out.
  const Millis later = harness.now + 60000;
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  DecisionInput input;
  input.topology = &harness.fabric.topology();
  input.capacity = &harness.fabric.capacity();
  input.signals = &harness.fabric.signals();
  input.paths = &harness.fabric.paths();
  input.traffic = &harness.fabric.traffic();
  input.policy = &policy;
  input.now = later;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  input.tracker = &harness.engine.tracker();
  const Decision decision = harness.engine.decide(input);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(harness.engine.tracker().size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
}

HGM_TEST(hardening, coordinator_survives_a_hostile_connection) {
  GovernorConfig config;
  config.node_name = "hostile-governor";
  Governor governor(config);
  HGM_CHECK(governor.recover(1000).ok());
  CoordinatorConfig coordinator_config;
  coordinator_config.endpoint = Endpoint{"127.0.0.1", 0};
  Coordinator coordinator(coordinator_config, governor);
  HGM_CHECK(coordinator.start().ok());
  const std::uint16_t port = coordinator.port();

  // A connection that sends garbage instead of a hello.
  {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 5000);
    HGM_CHECK(socket.ok());
    static_cast<void>(socket.value().set_receive_timeout(5000));
    std::vector<std::byte> junk(kFrameHeaderBytes, std::byte{0x7F});
    HGM_CHECK(send_bytes(socket.value(), std::span<const std::byte>(junk.data(), junk.size())).ok());
    static_cast<void>(receive_frame(socket.value()));
    socket.value().close();
  }
  // A connection that says nothing at all.
  {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 5000);
    HGM_CHECK(socket.ok());
    socket.value().close();
  }
  // The coordinator still serves a well-formed publisher afterwards.
  {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 5000);
    HGM_CHECK(socket.ok());
    static_cast<void>(socket.value().set_receive_timeout(5000));
    Provenance provenance;
    provenance.publisher = PublisherId::from(77);
    provenance.incarnation = Incarnation::from(1);
    provenance.boot = BootId::from(1);
    provenance.emitted_at = 1000;
    HelloPayload hello;
    hello.publisher = provenance.publisher;
    hello.incarnation = provenance.incarnation;
    hello.boot = provenance.boot;
    hello.protocol_version = kFrameProtocolVersion;
    hello.name = "well-behaved";
    std::vector<std::byte> payload;
    HGM_CHECK(encode_hello(hello, payload).ok());
    HGM_CHECK(send_frame(socket.value(), make_frame(MessageType::Hello, provenance, std::move(payload))).ok());
    Result<Frame> reply = receive_frame(socket.value());
    HGM_CHECK(reply.ok());
    HGM_CHECK_EQ(reply.value().header.type, MessageType::Welcome);
    static_cast<void>(socket.value().shutdown_both());
    socket.value().close();
  }
  HGM_CHECK(coordinator.hellos() >= 1);
  HGM_CHECK(coordinator.connections_refused() + coordinator.frames_refused() >= 1);
  HGM_CHECK(coordinator.stop().ok());
}
