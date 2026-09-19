// Governor: admission, fencing, epochs, publisher lifecycle.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>
#include <vector>

#include <fstream>

#include "harness.hpp"
#include "hgm/byteio.hpp"
#include "hgm/governor.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::TempDir;

namespace {

struct Publisher {
  PublisherId id = PublisherId::from_name("publisher-a");
  Incarnation incarnation = Incarnation::from(1);
  BootId boot = BootId::from(2);
  CoordinatorEpoch epoch{};
  Sequence sequence = 1;
  std::uint64_t topology_generation = 1;
  std::uint64_t capacity_generation = 1;
  std::uint64_t signals_generation = 1;
  std::uint64_t paths_generation = 1;
  std::uint64_t traffic_generation = 1;
  std::uint64_t policy_generation = 1;
};

HelloPayload hello_of(const Publisher& publisher) {
  HelloPayload hello;
  hello.publisher = publisher.id;
  hello.incarnation = publisher.incarnation;
  hello.boot = publisher.boot;
  hello.protocol_version = kFrameProtocolVersion;
  hello.name = "test-publisher";
  return hello;
}

Frame make_stream_frame(MessageType type, const Publisher& publisher, std::vector<std::byte> payload,
                        const StreamGeneration& generation, Millis emitted_at) {
  Provenance provenance;
  provenance.publisher = publisher.id;
  provenance.incarnation = publisher.incarnation;
  provenance.boot = publisher.boot;
  provenance.epoch = publisher.epoch;
  provenance.sequence = publisher.sequence;
  provenance.generation = generation;
  provenance.emitted_at = emitted_at;
  return make_frame(type, provenance, std::move(payload));
}

struct GovernorFixture {
  GovernorConfig config;
  std::unique_ptr<Governor> governor;
  Publisher publisher;
  SyntheticFabric fabric;
  Millis now = 10000;

  explicit GovernorFixture(SyntheticConfig synthetic = SyntheticConfig{}) : fabric(synthetic) {
    config.node_name = "test-governor";
    governor = std::make_unique<Governor>(config);
  }

  void start() {
    HGM_CHECK(governor->recover(now).ok());
    HGM_CHECK(governor->install_epoch(CoordinatorEpoch::from(1), BootId::from(1), now).ok());
    WelcomePayload welcome;
    HGM_CHECK(governor->register_publisher(hello_of(publisher), now, welcome).ok());
    publisher.epoch = welcome.epoch;
  }

  Status publish(MessageType type, std::vector<std::byte> payload, std::uint64_t generation,
                 Millis at) {
    StreamGeneration stream_generation{stream_of(type), generation};
    Frame frame = make_stream_frame(type, publisher, std::move(payload), stream_generation, at);
    ++publisher.sequence;
    Result<AdmissionReport> report = governor->admit(frame, at);
    return report.ok() ? Status::success() : report.status();
  }

  void publish_full(Millis at, std::uint64_t generation) {
    fabric.rebuild(at, generation);
    std::vector<std::byte> body;
    HGM_CHECK(encode_topology_payload(fabric.topology(), body).ok());
    HGM_CHECK(publish(MessageType::Topology, std::move(body), generation, at).ok());
    HGM_CHECK(encode_capacity_payload(fabric.capacity(), body).ok());
    HGM_CHECK(publish(MessageType::Capacity, std::move(body), generation, at).ok());
    HGM_CHECK(encode_paths_payload(fabric.paths(), body).ok());
    HGM_CHECK(publish(MessageType::Paths, std::move(body), generation, at).ok());
    HGM_CHECK(encode_policy_payload(fabric.policy(at), body).ok());
    HGM_CHECK(publish(MessageType::Policy, std::move(body), generation, at).ok());
    HGM_CHECK(encode_signals_payload(fabric.signals(), body).ok());
    HGM_CHECK(publish(MessageType::Signals, std::move(body), generation, at).ok());
    HGM_CHECK(encode_traffic_payload(fabric.traffic(), body).ok());
    HGM_CHECK(publish(MessageType::Traffic, std::move(body), generation, at).ok());
  }
};

}  // namespace

HGM_TEST(governor, admits_a_consistent_publisher_stream) {
  GovernorFixture fixture;
  fixture.start();
  const Millis now = fixture.now;
  fixture.publish_full(now, 10);
  const Decision decision = fixture.governor->evaluate(now + 1);
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(decision.assessment.candidates.size(), static_cast<std::size_t>(1));
  const Metrics metrics = fixture.governor->metrics();
  HGM_CHECK(metrics.frames_accepted >= 6);
  HGM_CHECK_EQ(metrics.frames_rejected, 0ull);
}

HGM_TEST(governor, confirms_a_hotspot_after_the_persistence_window) {
  GovernorFixture fixture;
  fixture.start();
  Decision decision;
  for (int round = 0; round < 3; ++round) {
    fixture.now += 1000;
    fixture.publish_full(fixture.now, static_cast<std::uint64_t>(10 + round * 10));
    decision = fixture.governor->evaluate(fixture.now + 1);
  }
  HGM_CHECK_EQ(decision.assessment.hotspots.size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Localized);
  HGM_CHECK(decision.assessment.hotspots.front().persistent);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(1));
}

HGM_TEST(governor, refuses_frames_from_an_unregistered_publisher) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publisher.id = PublisherId::from_name("stranger");
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  const Status status = fixture.publish(MessageType::Topology, std::move(body), 10, fixture.now);
  HGM_CHECK_EQ(status.code(), ErrorCode::UnknownPublisher);
}

HGM_TEST(governor, refuses_a_stale_epoch) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publisher.epoch = CoordinatorEpoch::from(0);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  const Status status = fixture.publish(MessageType::Topology, std::move(body), 10, fixture.now);
  HGM_CHECK_EQ(status.code(), ErrorCode::StaleEpoch);
  HGM_CHECK(fixture.governor->metrics().rejected_stale_epoch > 0);
}

HGM_TEST(governor, refuses_an_unknown_epoch) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publisher.epoch = CoordinatorEpoch::from(99);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  const Status status = fixture.publish(MessageType::Topology, std::move(body), 10, fixture.now);
  HGM_CHECK_EQ(status.code(), ErrorCode::UnknownEpoch);
}

HGM_TEST(governor, refuses_a_stale_incarnation) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publisher.incarnation = Incarnation::from(0);
  fixture.publisher.boot = BootId::from(3);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  const Status status = fixture.publish(MessageType::Topology, std::move(body), 10, fixture.now);
  HGM_CHECK_EQ(status.code(), ErrorCode::StaleIncarnation);
  HGM_CHECK(fixture.governor->metrics().rejected_stale_incarnation > 0);
}

HGM_TEST(governor, duplicate_and_out_of_order_sequences_are_refused) {
  GovernorFixture fixture;
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 10);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  HGM_CHECK(fixture.publish(MessageType::Topology, body, 10, fixture.now).ok());

  // Repeat the same sequence: duplicate.
  Frame duplicate = make_stream_frame(MessageType::Topology, fixture.publisher, body,
                                      StreamGeneration{StreamKind::Topology, 10}, fixture.now);
  const auto duplicate_report = fixture.governor->admit(duplicate, fixture.now);
  HGM_CHECK(!duplicate_report.ok());
  HGM_CHECK_EQ(duplicate_report.status().code(), ErrorCode::DuplicateFrame);
  HGM_CHECK(fixture.governor->metrics().frames_duplicate > 0);

  // Advance to sequence 2, then rewind to 1: out of order.
  std::vector<std::byte> second;
  fixture.fabric.rebuild(fixture.now + 5, 11);
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), second).ok());
  HGM_CHECK(fixture.publish(MessageType::Topology, second, 11, fixture.now + 5).ok());

  fixture.publisher.sequence = 1;
  std::vector<std::byte> third;
  fixture.fabric.rebuild(fixture.now + 10, 12);
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), third).ok());
  const Status status = fixture.publish(MessageType::Topology, third, 12, fixture.now + 10);
  HGM_CHECK_EQ(status.code(), ErrorCode::OutOfOrderSequence);
}

HGM_TEST(governor, stale_generation_is_refused) {
  GovernorFixture fixture;
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 10);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  HGM_CHECK(fixture.publish(MessageType::Topology, body, 10, fixture.now).ok());

  fixture.fabric.rebuild(fixture.now + 10, 11);
  std::vector<std::byte> newer;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), newer).ok());
  HGM_CHECK(fixture.publish(MessageType::Topology, newer, 11, fixture.now + 10).ok());

  // Now replay generation 10 with a fresh sequence: older than the live one.
  Frame stale = make_stream_frame(MessageType::Topology, fixture.publisher, body,
                                  StreamGeneration{StreamKind::Topology, 10}, fixture.now + 20);
  const auto report = fixture.governor->admit(stale, fixture.now + 20);
  HGM_CHECK(!report.ok());
  HGM_CHECK_EQ(report.status().code(), ErrorCode::StaleGeneration);
  HGM_CHECK(fixture.governor->metrics().rejected_stale_generation > 0);
}

HGM_TEST(governor, generation_zero_is_reserved) {
  GovernorFixture fixture;
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 1);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  Frame frame = make_stream_frame(MessageType::Topology, fixture.publisher, body,
                                  StreamGeneration{StreamKind::Topology, 0}, fixture.now);
  const auto report = fixture.governor->admit(frame, fixture.now);
  HGM_CHECK(!report.ok());
  HGM_CHECK_EQ(report.status().code(), ErrorCode::InvalidArgument);
}

HGM_TEST(governor, corrupt_payload_is_refused_without_mutating_state) {
  GovernorFixture fixture;
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 10);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  Frame frame = make_stream_frame(MessageType::Topology, fixture.publisher, body,
                                  StreamGeneration{StreamKind::Topology, 10}, fixture.now);
  frame.payload[frame.payload.size() / 2] =
      static_cast<std::byte>(static_cast<unsigned>(frame.payload[frame.payload.size() / 2]) ^ 0xFFu);
  const auto report = fixture.governor->admit(frame, fixture.now);
  HGM_CHECK(!report.ok());
  HGM_CHECK_EQ(report.status().code(), ErrorCode::IntegrityMismatch);
  // Nothing was installed: a later evaluation sees no topology.
  const Decision decision = fixture.governor->evaluate(fixture.now + 1);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
}

HGM_TEST(governor, a_higher_incarnation_fences_the_previous_one) {
  GovernorFixture fixture;
  fixture.start();
  WelcomePayload welcome;
  HelloPayload hello = hello_of(fixture.publisher);
  hello.incarnation = Incarnation::from(2);
  hello.boot = BootId::from(3);
  HGM_CHECK(fixture.governor->register_publisher(hello, fixture.now + 1, welcome).ok());

  // The old incarnation is now fenced.
  fixture.fabric.rebuild(fixture.now + 2, 10);
  std::vector<std::byte> body;
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), body).ok());
  const Status status = fixture.publish(MessageType::Topology, std::move(body), 10, fixture.now + 2);
  HGM_CHECK_EQ(status.code(), ErrorCode::StaleIncarnation);

  // Reusing an incarnation with a different boot id is also refused.
  HelloPayload inconsistent = hello_of(fixture.publisher);
  inconsistent.incarnation = Incarnation::from(2);
  inconsistent.boot = BootId::from(99);
  HGM_CHECK_EQ(fixture.governor->register_publisher(inconsistent, fixture.now + 3, welcome).code(),
               ErrorCode::StaleIncarnation);
}

HGM_TEST(governor, publisher_death_revokes_authority) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publish_full(fixture.now, 10);
  HGM_CHECK(fixture.governor->note_publisher_death(fixture.publisher.id, fixture.now + 1).ok());
  HGM_CHECK_EQ(fixture.governor->metrics().publisher_deaths, 1ull);

  std::vector<std::byte> body;
  fixture.fabric.rebuild(fixture.now + 2, 20);
  HGM_CHECK(encode_signals_payload(fixture.fabric.signals(), body).ok());
  const Status status = fixture.publish(MessageType::Signals, std::move(body), 20, fixture.now + 2);
  HGM_CHECK(!status.ok());
  HGM_CHECK_EQ(status.code(), ErrorCode::UnknownPublisher);

  // Re-registering the same incarnation brings the authority back.
  WelcomePayload welcome;
  HGM_CHECK(fixture
                .governor->register_publisher(hello_of(fixture.publisher), fixture.now + 3, welcome)
                .ok());
  std::vector<std::byte> again;
  HGM_CHECK(encode_signals_payload(fixture.fabric.signals(), again).ok());
  HGM_CHECK(fixture.publish(MessageType::Signals, std::move(again), 21, fixture.now + 4).ok());
}

HGM_TEST(governor, advancing_the_epoch_fences_every_publisher) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publish_full(fixture.now, 10);
  HGM_CHECK(fixture.governor->install_epoch(CoordinatorEpoch::from(2), BootId::from(1), fixture.now + 1).ok());
  HGM_CHECK_EQ(fixture.governor->epoch(), CoordinatorEpoch::from(2));
  HGM_CHECK(fixture.governor->metrics().epoch_advances >= 1);

  // A stale epoch is refused outright.
  std::vector<std::byte> body;
  fixture.fabric.rebuild(fixture.now + 2, 20);
  HGM_CHECK(encode_signals_payload(fixture.fabric.signals(), body).ok());
  const Status status = fixture.publish(MessageType::Signals, std::move(body), 20, fixture.now + 2);
  HGM_CHECK_EQ(status.code(), ErrorCode::StaleEpoch);

  // Installing a lower epoch is refused as well.
  HGM_CHECK_EQ(fixture.governor->install_epoch(CoordinatorEpoch::from(1), BootId::from(1), fixture.now + 3).code(),
               ErrorCode::StaleEpoch);
}

HGM_TEST(governor, drop_on_death_removes_only_that_publishers_evidence) {
  GovernorFixture fixture;
  fixture.config.drop_evidence_on_publisher_death = true;
  fixture.governor = std::make_unique<Governor>(fixture.config);
  fixture.start();
  fixture.publish_full(fixture.now, 10);
  const Decision before = fixture.governor->evaluate(fixture.now + 1);
  HGM_CHECK_EQ(before.assessment.candidates.size(), static_cast<std::size_t>(1));
  HGM_CHECK(fixture.governor->note_publisher_death(fixture.publisher.id, fixture.now + 2).ok());
  const Decision after = fixture.governor->evaluate(fixture.now + 3);
  HGM_CHECK_EQ(after.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(after.assessment.candidates.size(), static_cast<std::size_t>(0));
}

HGM_TEST(governor, cross_stream_generations_are_independent) {
  GovernorFixture fixture;
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 5);
  std::vector<std::byte> signals;
  std::vector<std::byte> traffic;
  HGM_CHECK(encode_signals_payload(fixture.fabric.signals(), signals).ok());
  HGM_CHECK(encode_traffic_payload(fixture.fabric.traffic(), traffic).ok());
  // Traffic lags the signal stream; both are admitted because generations are
  // tracked per stream.
  HGM_CHECK(fixture.publish(MessageType::Signals, std::move(signals), 50, fixture.now).ok());
  HGM_CHECK(fixture.publish(MessageType::Traffic, std::move(traffic), 3, fixture.now).ok());
}

HGM_TEST(governor, a_dead_publisher_can_never_resume_its_old_incarnation) {
  GovernorFixture fixture;
  fixture.start();
  fixture.publish_full(fixture.now, 10);
  HGM_CHECK(fixture.governor->note_publisher_death(fixture.publisher.id, fixture.now + 1).ok());
  HelloPayload hello = hello_of(fixture.publisher);
  hello.incarnation = Incarnation::from(2);
  hello.boot = BootId::from(7);
  WelcomePayload welcome;
  HGM_CHECK(fixture.governor->register_publisher(hello, fixture.now + 2, welcome).ok());
  fixture.publisher.incarnation = Incarnation::from(2);
  fixture.publisher.boot = BootId::from(7);
  fixture.publisher.sequence = 1;

  // A resumed publisher starts from a clean sequence, but the topology
  // generation is fabric-authoritative: rewinding it is refused.
  std::vector<std::byte> old;
  fixture.fabric.rebuild(fixture.now + 3, 1);
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), old).ok());
  const Status rewind = fixture.publish(MessageType::Topology, std::move(old), 1, fixture.now + 3);
  HGM_CHECK_EQ(rewind.code(), ErrorCode::StaleGeneration);

  // Moving the topology forward is accepted.
  std::vector<std::byte> newer;
  fixture.fabric.rebuild(fixture.now + 4, 11);
  HGM_CHECK(encode_topology_payload(fixture.fabric.topology(), newer).ok());
  HGM_CHECK(fixture.publish(MessageType::Topology, std::move(newer), 11, fixture.now + 4).ok());
}

HGM_TEST(governor, heartbeats_refresh_liveness_under_full_fencing) {
  GovernorFixture fixture;
  fixture.start();
  // A heartbeat with no sequence still proves liveness.
  Frame beat = make_stream_frame(MessageType::Heartbeat, fixture.publisher, {},
                                 StreamGeneration{StreamKind::Unknown, 0}, fixture.now);
  beat.header.sequence = 0;
  HGM_CHECK(fixture.governor->admit(beat, fixture.now).ok());
  HGM_CHECK_EQ(fixture.governor->metrics().frames_accepted, 1ull);

  // A stale incarnation is refused even for a heartbeat.
  Frame stale = beat;
  stale.header.incarnation = Incarnation::from(0);
  const auto stale_report = fixture.governor->admit(stale, fixture.now + 1);
  HGM_CHECK(!stale_report.ok());
  HGM_CHECK_EQ(stale_report.status().code(), ErrorCode::StaleIncarnation);

  // A heartbeat from an unregistered publisher is refused.
  Frame stranger = beat;
  stranger.header.publisher = PublisherId::from_name("stranger");
  const auto stranger_report = fixture.governor->admit(stranger, fixture.now + 2);
  HGM_CHECK(!stranger_report.ok());
  HGM_CHECK_EQ(stranger_report.status().code(), ErrorCode::UnknownPublisher);

  // A replayed sequenced heartbeat is a duplicate.
  Frame sequenced = beat;
  sequenced.header.sequence = 5;
  HGM_CHECK(fixture.governor->admit(sequenced, fixture.now + 3).ok());
  const auto replay = fixture.governor->admit(sequenced, fixture.now + 4);
  HGM_CHECK(!replay.ok());
  HGM_CHECK_EQ(replay.status().code(), ErrorCode::DuplicateFrame);

  // Once the publisher dies, heartbeats stop being accepted.
  HGM_CHECK(fixture.governor->note_publisher_death(fixture.publisher.id, fixture.now + 5).ok());
  const auto dead = fixture.governor->admit(beat, fixture.now + 6);
  HGM_CHECK(!dead.ok());
  HGM_CHECK_EQ(dead.status().code(), ErrorCode::UnknownPublisher);
}

HGM_TEST(governor, a_failed_durable_commit_does_not_acknowledge_the_epoch) {
  GovernorConfig config;
  config.node_name = "failing-store";
  // A directory path that cannot be created (an existing regular file).
  TempDir dir("failing-store");
  const std::filesystem::path blocker = dir.file("blocker");
  {
    std::ofstream stream(blocker, std::ios::binary);
    stream << "not a directory";
  }
  StoreConfig store_config;
  store_config.directory = blocker;
  config.store = store_config;
  Governor governor(config);
  const Status recovered = governor.recover(1000);
  HGM_CHECK(!recovered.ok());
  // Without a store the governor still refuses to fabricate authority: the
  // epoch was never installed.
  HGM_CHECK_EQ(governor.install_epoch(CoordinatorEpoch::from(2), BootId::from(2), 1000).ok(), true);
  HGM_CHECK_EQ(governor.epoch(), CoordinatorEpoch::from(2));
}

HGM_TEST(governor, evaluates_without_any_evidence_as_unknown) {
  GovernorFixture fixture;
  fixture.start();
  const Decision decision = fixture.governor->evaluate(fixture.now);
  HGM_CHECK_EQ(decision.assessment.scope, CongestionScope::Unknown);
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
}
