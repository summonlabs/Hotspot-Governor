// Concurrency: many threads through one governor, many publishers through one
// coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "hgm/coordinator.hpp"
#include "hgm/governor.hpp"
#include "hgm/transport.hpp"
#include "testing.hpp"

using namespace hgm;

namespace {

struct MultiPublisherFixture {
  GovernorConfig config;
  std::unique_ptr<Governor> governor;
  SyntheticFabric fabric;
  std::vector<PublisherId> publishers;
  std::vector<Incarnation> incarnations;
  std::vector<BootId> boots;
  Millis now = 10000;

  MultiPublisherFixture(std::size_t count, SyntheticConfig synthetic = SyntheticConfig{})
      : fabric(synthetic) {
    config.node_name = "concurrency-governor";
    governor = std::make_unique<Governor>(config);
    for (std::size_t i = 0; i < count; ++i) {
      publishers.push_back(PublisherId::from(1000 + i));
      incarnations.push_back(Incarnation::from(1));
      boots.push_back(BootId::from(2000 + i));
    }
  }

  void start() {
    HGM_CHECK(governor->recover(now).ok());
    HGM_CHECK(governor->install_epoch(CoordinatorEpoch::from(1), BootId::from(1), now).ok());
    for (std::size_t i = 0; i < publishers.size(); ++i) {
      HelloPayload hello;
      hello.publisher = publishers[i];
      hello.incarnation = incarnations[i];
      hello.boot = boots[i];
      hello.protocol_version = kFrameProtocolVersion;
      hello.name = "publisher";
      WelcomePayload welcome;
      HGM_CHECK(governor->register_publisher(hello, now, welcome).ok());
    }
  }
};

}  // namespace

HGM_TEST(concurrency, many_threads_admit_into_one_governor) {
  constexpr std::size_t kPublishers = 8;
  constexpr std::size_t kFramesPerPublisher = 24;
  MultiPublisherFixture fixture(kPublishers);
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 5);

  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  threads.reserve(kPublishers);
  for (std::size_t i = 0; i < kPublishers; ++i) {
    threads.emplace_back([&fixture, &accepted, &rejected, i]() {
      for (std::size_t round = 0; round < kFramesPerPublisher; ++round) {
        Provenance provenance;
        provenance.publisher = fixture.publishers[i];
        provenance.incarnation = fixture.incarnations[i];
        provenance.boot = fixture.boots[i];
        provenance.epoch = CoordinatorEpoch::from(1);
        provenance.sequence = round + 1;
        provenance.generation =
            StreamGeneration{StreamKind::Signals, 100 + static_cast<std::uint64_t>(round)};
        provenance.emitted_at = fixture.now;
        std::vector<std::byte> body;
        if (!encode_signals_payload(fixture.fabric.signals(), body).ok()) {
          ++rejected;
          continue;
        }
        Frame frame = make_frame(MessageType::Signals, provenance, std::move(body));
        Result<AdmissionReport> report = fixture.governor->admit(frame, fixture.now);
        if (report.ok()) {
          ++accepted;
        } else {
          ++rejected;
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  HGM_CHECK_EQ(accepted.load(), static_cast<int>(kPublishers * kFramesPerPublisher));
  HGM_CHECK_EQ(rejected.load(), 0);
  const Metrics metrics = fixture.governor->metrics();
  HGM_CHECK_EQ(metrics.frames_accepted, static_cast<std::uint64_t>(kPublishers * kFramesPerPublisher));
  HGM_CHECK_EQ(metrics.frames_rejected, 0ull);
  HGM_CHECK_EQ(fixture.governor->publishers().size(), kPublishers);
}

HGM_TEST(concurrency, evaluation_races_with_admission_without_corruption) {
  MultiPublisherFixture fixture(4);
  fixture.start();
  fixture.fabric.rebuild(fixture.now, 5);
  std::atomic<bool> stop{false};
  std::atomic<int> evaluations{0};

  std::thread evaluator([&]() {
    while (!stop.load()) {
      const Decision decision = fixture.governor->evaluate(fixture.now + 1);
      HGM_CHECK(decision.assessment.coverage_ppm <= 1000000);
      HGM_CHECK(decision.explanation.size() <= 16384);
      evaluations.fetch_add(1);
    }
  });

  std::vector<std::thread> publishers;
  for (std::size_t i = 0; i < 4; ++i) {
    publishers.emplace_back([&fixture, i]() {
      for (std::size_t round = 0; round < 50; ++round) {
        Provenance provenance;
        provenance.publisher = fixture.publishers[i];
        provenance.incarnation = fixture.incarnations[i];
        provenance.boot = fixture.boots[i];
        provenance.epoch = CoordinatorEpoch::from(1);
        provenance.sequence = round + 1;
        provenance.generation =
            StreamGeneration{StreamKind::Signals, 200 + static_cast<std::uint64_t>(round)};
        provenance.emitted_at = fixture.now;
        std::vector<std::byte> body;
        if (!encode_signals_payload(fixture.fabric.signals(), body).ok()) continue;
        Frame frame = make_frame(MessageType::Signals, provenance, std::move(body));
        static_cast<void>(fixture.governor->admit(frame, fixture.now));
      }
    });
  }
  for (std::thread& thread : publishers) thread.join();
  stop.store(true);
  evaluator.join();
  HGM_CHECK(evaluations.load() > 0);
  const Metrics metrics = fixture.governor->metrics();
  HGM_CHECK(metrics.frames_accepted + metrics.frames_rejected == metrics.frames_received);
}

HGM_TEST(concurrency, coordinator_serves_several_publishers) {
  GovernorConfig config;
  config.node_name = "coordinator-concurrency";
  Governor governor(config);
  HGM_CHECK(governor.recover(1000).ok());
  CoordinatorConfig coordinator_config;
  coordinator_config.endpoint = Endpoint{"127.0.0.1", 0};
  Coordinator coordinator(coordinator_config, governor);
  HGM_CHECK(coordinator.start().ok());
  const std::uint16_t port = coordinator.port();
  HGM_CHECK(port != 0);

  constexpr std::size_t kPublishers = 4;
  constexpr std::size_t kFrames = 8;
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);

  std::atomic<int> sent_ok{0};
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < kPublishers; ++i) {
    threads.emplace_back([&, i]() {
      Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 5000);
      if (!socket.ok()) return;
      static_cast<void>(socket.value().set_receive_timeout(5000));

      Provenance provenance;
      provenance.publisher = PublisherId::from(4000 + i);
      provenance.incarnation = Incarnation::from(1);
      provenance.boot = BootId::from(5000 + i);
      provenance.emitted_at = 1000;

      HelloPayload hello;
      hello.publisher = provenance.publisher;
      hello.incarnation = provenance.incarnation;
      hello.boot = provenance.boot;
      hello.protocol_version = kFrameProtocolVersion;
      hello.name = "multi";
      std::vector<std::byte> payload;
      if (!encode_hello(hello, payload).ok()) return;
      if (!send_frame(socket.value(), make_frame(MessageType::Hello, provenance, std::move(payload))).ok()) {
        return;
      }
      Result<Frame> reply = receive_frame(socket.value());
      if (!reply.ok() || reply.value().header.type != MessageType::Welcome) return;
      WelcomePayload welcome;
      if (!decode_welcome(std::span<const std::byte>(reply.value().payload.data(), reply.value().payload.size()),
                          welcome)
               .ok()) {
        return;
      }
      provenance.epoch = welcome.epoch;

      for (std::size_t round = 0; round < kFrames; ++round) {
        provenance.sequence = round + 1;
        provenance.generation = StreamGeneration{StreamKind::Signals, 10 + round};
        std::vector<std::byte> body;
        if (!encode_signals_payload(fabric.signals(), body).ok()) return;
        if (!send_frame(socket.value(), make_frame(MessageType::Signals, provenance, std::move(body))).ok()) {
          return;
        }
        sent_ok.fetch_add(1);
      }
      static_cast<void>(socket.value().shutdown_both());
      socket.value().close();
    });
  }
  for (std::thread& thread : threads) thread.join();

  HGM_CHECK_EQ(sent_ok.load(), static_cast<int>(kPublishers * kFrames));
  coordinator.wait_for_admissions(kPublishers * kFrames);
  HGM_CHECK_EQ(coordinator.frames_admitted(), static_cast<std::uint64_t>(kPublishers * kFrames));
  HGM_CHECK_EQ(coordinator.hellos(), static_cast<std::uint64_t>(kPublishers));
  HGM_CHECK_EQ(coordinator.frames_refused(), 0ull);
  HGM_CHECK(coordinator.stop().ok());
  HGM_CHECK_EQ(coordinator.connection_count(), static_cast<std::size_t>(0));
}

HGM_TEST(concurrency, a_retired_connection_is_not_left_joinable) {
  GovernorConfig config;
  config.node_name = "retire-governor";
  Governor governor(config);
  HGM_CHECK(governor.recover(1000).ok());
  CoordinatorConfig coordinator_config;
  coordinator_config.endpoint = Endpoint{"127.0.0.1", 0};
  Coordinator coordinator(coordinator_config, governor);
  HGM_CHECK(coordinator.start().ok());
  const std::uint16_t port = coordinator.port();

  // Connect, handshake, then leave immediately. The connection thread retires
  // itself before stop() runs, which is the path that must not leave a
  // joinable thread handle behind.
  for (int i = 0; i < 4; ++i) {
    Result<Socket> socket = connect_to(Endpoint{"127.0.0.1", port}, 5000);
    HGM_CHECK(socket.ok());
    static_cast<void>(socket.value().set_receive_timeout(5000));
    Provenance provenance;
    provenance.publisher = PublisherId::from(700 + static_cast<std::uint64_t>(i));
    provenance.incarnation = Incarnation::from(1);
    provenance.boot = BootId::from(1);
    provenance.emitted_at = 1000;
    HelloPayload hello;
    hello.publisher = provenance.publisher;
    hello.incarnation = provenance.incarnation;
    hello.boot = provenance.boot;
    hello.protocol_version = kFrameProtocolVersion;
    hello.name = "short-lived";
    std::vector<std::byte> payload;
    HGM_CHECK(encode_hello(hello, payload).ok());
    HGM_CHECK(send_frame(socket.value(), make_frame(MessageType::Hello, provenance, std::move(payload))).ok());
    Result<Frame> reply = receive_frame(socket.value());
    HGM_CHECK(reply.ok());
    HGM_CHECK_EQ(reply.value().header.type, MessageType::Welcome);
    static_cast<void>(socket.value().shutdown_both());
    socket.value().close();

    // Wait until the coordinator has retired the connection itself.
    bool retired = false;
    for (unsigned attempt = 0; attempt < 1500 && !retired; ++attempt) {
      if (coordinator.connection_count() == 0) retired = true;
      else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    HGM_CHECK(retired);
  }
  HGM_CHECK(coordinator.stop().ok());
  HGM_CHECK_EQ(coordinator.hellos(), 4ull);
}

HGM_TEST(concurrency, governor_shutdown_is_idempotent_and_safe) {
  GovernorConfig config;
  Governor governor(config);
  HGM_CHECK(governor.recover(1000).ok());
  HGM_CHECK(governor.start_workers().ok());
  HGM_CHECK_EQ(governor.start_workers().code(), ErrorCode::AlreadyStarted);
  HGM_CHECK(governor.stop_workers().ok());
  HGM_CHECK(governor.stop_workers().ok());
}

HGM_TEST(concurrency, worker_pool_under_contention) {
  WorkerPool pool(8, 4096);
  HGM_CHECK(pool.start().ok());
  std::atomic<int> completed{0};
  std::vector<std::thread> submitters;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  for (int i = 0; i < kThreads; ++i) {
    submitters.emplace_back([&pool, &completed]() {
      for (int j = 0; j < kPerThread; ++j) {
        const Status status = pool.submit([&completed]() { completed.fetch_add(1); });
        if (!status.ok()) HGM_CHECK_EQ(status.code(), ErrorCode::QueueFull);
      }
    });
  }
  for (std::thread& thread : submitters) thread.join();
  pool.wait_until_idle();
  HGM_CHECK_EQ(completed.load(), static_cast<int>(pool.completed()));
  HGM_CHECK(pool.dropped() == 0);
  pool.drain_and_stop();
}
