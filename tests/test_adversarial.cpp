// Adversarial input: malformed, truncated, oversized, contradictory, forged.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"
#include "hgm/byteio.hpp"
#include "hgm/frame.hpp"
#include "hgm/limits.hpp"
#include "hgm/store.hpp"
#include "hgm/synthetic.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace hgm;
using hgmt::Harness;
using hgmt::TempDir;

namespace {

// Feeds a buffer to every payload decoder. None may crash, and each must either
// succeed or report a structured failure.
void hammer_decoders(std::span<const std::byte> bytes) {
  TopologySnapshot topology;
  CapacitySnapshot capacity;
  SignalSnapshot signals;
  PathSnapshot paths;
  TrafficSnapshot traffic;
  PolicySnapshot policy;
  HelloPayload hello;
  WelcomePayload welcome;
  RejectedPayload rejected;
  static_cast<void>(decode_topology_payload(bytes, topology));
  static_cast<void>(decode_capacity_payload(bytes, capacity));
  static_cast<void>(decode_signals_payload(bytes, signals));
  static_cast<void>(decode_paths_payload(bytes, paths));
  static_cast<void>(decode_traffic_payload(bytes, traffic));
  static_cast<void>(decode_policy_payload(bytes, policy));
  static_cast<void>(decode_hello(bytes, hello));
  static_cast<void>(decode_welcome(bytes, welcome));
  static_cast<void>(decode_rejected(bytes, rejected));
}

}  // namespace

HGM_TEST(adversarial, random_bytes_never_crash_a_decoder) {
  SyntheticRng rng(0xDEADBEEFull);
  for (int i = 0; i < 400; ++i) {
    const std::size_t length = 1 + rng.below(512);
    std::vector<std::byte> bytes(length);
    for (std::size_t k = 0; k < length; ++k) {
      bytes[k] = static_cast<std::byte>(rng.below(256));
    }
    hammer_decoders(std::span<const std::byte>(bytes.data(), bytes.size()));
  }
  HGM_CHECK(true);
}

HGM_TEST(adversarial, truncated_valid_payloads_never_crash_a_decoder) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);
  std::vector<std::byte> bytes;
  HGM_CHECK(encode_topology_payload(fabric.topology(), bytes).ok());
  for (std::size_t cut = 0; cut <= bytes.size(); cut += 7) {
    hammer_decoders(std::span<const std::byte>(bytes.data(), cut));
  }
  HGM_CHECK(encode_signals_payload(fabric.signals(), bytes).ok());
  for (std::size_t cut = 0; cut <= bytes.size(); cut += 11) {
    hammer_decoders(std::span<const std::byte>(bytes.data(), cut));
  }
  HGM_CHECK(true);
}

HGM_TEST(adversarial, mutated_valid_payloads_are_refused_or_survived) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);
  std::vector<std::byte> original;
  HGM_CHECK(encode_topology_payload(fabric.topology(), original).ok());
  SyntheticRng rng(0xABCDEFull);
  std::size_t refused = 0;
  for (int i = 0; i < 200; ++i) {
    std::vector<std::byte> mutated = original;
    const std::size_t index = rng.below(static_cast<std::uint32_t>(mutated.size()));
    mutated[index] = static_cast<std::byte>(static_cast<unsigned>(mutated[index]) ^ (1u << rng.below(8)));
    TopologySnapshot decoded;
    const Status status =
        decode_topology_payload(std::span<const std::byte>(mutated.data(), mutated.size()), decoded);
    if (!status.ok()) ++refused;
  }
  // Mutations almost always break either the field bounds or the structure; the
  // point is that nothing crashes and every outcome is structured.
  HGM_CHECK(refused > 0);
}

HGM_TEST(adversarial, huge_declared_counts_are_refused_without_allocation) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u16(static_cast<std::uint16_t>(StreamKind::Topology));
    writer.u64(1);
    writer.i64(1000);
    writer.i64(0);
    writer.u32(0xFFFFFFFFu);  // region count
  }
  TopologySnapshot topology;
  HGM_CHECK_EQ(decode_topology_payload(std::span<const std::byte>(bytes.data(), bytes.size()), topology).code(),
               ErrorCode::BoundExceeded);

  std::vector<std::byte> signal_bytes;
  {
    ByteWriter writer(signal_bytes);
    writer.u16(static_cast<std::uint16_t>(StreamKind::Signals));
    writer.u64(1);
    writer.i64(1000);
    writer.u64(1);
    writer.u64(1);
    writer.i64(0);
    writer.u32(0xFFFFFFFFu);
  }
  SignalSnapshot signals;
  HGM_CHECK_EQ(decode_signals_payload(std::span<const std::byte>(signal_bytes.data(), signal_bytes.size()), signals).code(),
               ErrorCode::BoundExceeded);
}

HGM_TEST(adversarial, path_hop_count_is_bounded) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u16(static_cast<std::uint16_t>(StreamKind::Paths));
    writer.u64(1);
    writer.i64(1000);
    writer.u64(1);
    writer.u32(1);
    writer.u64(1);
    writer.u64(1);
    writer.u32(static_cast<std::uint32_t>(Limits::kMaxHopsPerPath) + 1u);
  }
  PathSnapshot paths;
  HGM_CHECK_EQ(decode_paths_payload(std::span<const std::byte>(bytes.data(), bytes.size()), paths).code(),
               ErrorCode::BoundExceeded);
}

HGM_TEST(adversarial, a_frame_claiming_a_giant_payload_is_refused) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  header.type = MessageType::Topology;
  header.payload_length = 0xFFFFFFF0u;
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  const std::uint64_t crc = crc64(std::span<const std::byte>(bytes.data(), kFrameHeaderBytes - 8));
  for (int i = 0; i < 8; ++i) {
    bytes[static_cast<std::size_t>(kFrameHeaderBytes - 8 + i)] =
        static_cast<std::byte>((crc >> (8 * i)) & 0xFFull);
  }
  const Result<FrameHeader> decoded =
      decode_header(std::span<const std::byte>(bytes.data(), bytes.size()));
  HGM_CHECK(!decoded.ok());
  HGM_CHECK_EQ(decoded.status().code(), ErrorCode::FrameTooLarge);
}

HGM_TEST(adversarial, contradictory_signals_are_refused_at_construction) {
  {
    // Admitted above offered while offered load is reported: impossible.
    SignalSnapshot signals;
    ResourceSignals& record = signals.add(ResourceId::from(1));
    record.offered_bps = 10;
    record.admitted_bps = 11;
    HGM_CHECK_EQ(signals.build().code(), ErrorCode::ContradictoryEvidence);
  }
  {
    // Offered load simply not reported is missing data, not a contradiction.
    SignalSnapshot signals;
    ResourceSignals& record = signals.add(ResourceId::from(1));
    record.offered_bps = 0;
    record.admitted_bps = 5;
    HGM_CHECK(signals.build().ok());
  }
}

HGM_TEST(adversarial, a_hotspot_with_enormous_fan_in_stays_bounded) {
  SyntheticConfig config;
  config.region_count = 1;
  config.resources_per_region = 3;
  config.hotspot_count = 1;
  config.hotspot_index_base = 0;
  config.path_fan_in = 64;
  config.path_resource_density_ppm = 1000000;
  Harness harness(config);
  const Decision decision = harness.run(3);
  HGM_CHECK(harness.fabric.build_ok());
  for (const Hotspot& hotspot : decision.assessment.hotspots) {
    HGM_CHECK(hotspot.contributors.size() <= Limits::kMaxContributors);
    HGM_CHECK(hotspot.contributor_count >= hotspot.contributors.size());
  }
  HGM_CHECK(decision.explanation.size() <= 16384);
}

HGM_TEST(adversarial, corrupt_durable_files_are_refused) {
  TempDir dir("adversarial-store");
  StoreConfig config;
  config.directory = dir.path;
  {
    std::vector<std::byte> junk(256);
    SyntheticRng rng(0x77);
    for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::byte>(rng.below(256));
    Store store(config);
    HGM_CHECK(store.open().ok());
    store.close();
    // Write junk directly, then reopen.
    std::FILE* file = nullptr;
#if defined(_WIN32)
    static_cast<void>(::fopen_s(&file, dir.file("state.bin").string().c_str(), "wb"));
#else
    file = std::fopen(dir.file("state.bin").string().c_str(), "wb");
#endif
    HGM_CHECK(file != nullptr);
    std::fwrite(junk.data(), 1, junk.size(), file);
    std::fclose(file);
  }
  Store store(config);
  HGM_CHECK(store.open().ok());
  Result<RecoveredState> recovered = store.recover();
  HGM_CHECK(recovered.ok());
  HGM_CHECK(!recovered.value().recovered);
  HGM_CHECK_EQ(recovered.value().outcome.code(), ErrorCode::PersistenceCorrupt);
  store.close();
}

HGM_TEST(adversarial, a_policy_with_impossible_thresholds_authorises_nothing_useful) {
  Harness harness;
  harness.run(3);
  PolicySnapshot policy = harness.fabric.policy(harness.now);
  policy.thresholds().min_coverage_ppm = 1000000;
  policy.thresholds().saturation_utilization_ppm = 1000000;
  policy.budget().max_affected_resources = 0;
  policy.budget().max_scope_share_ppm = 0;
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
  HGM_CHECK_EQ(decision.plan.intents.size(), static_cast<std::size_t>(0));
  HGM_CHECK(decision.explanation.size() <= 16384);
}

HGM_TEST(adversarial, an_empty_topology_is_not_a_positive_answer) {
  TopologySnapshot topology;
  HGM_CHECK(topology.build().ok());
  CapacitySnapshot capacity;
  HGM_CHECK(capacity.build().ok());
  SignalSnapshot signals;
  HGM_CHECK(signals.build().ok());
  PolicySnapshot policy;
  policy.set_issued_at(1000);
  policy.set_ttl_ms(10000);

  DecisionInput input;
  input.topology = &topology;
  input.capacity = &capacity;
  input.signals = &signals;
  input.policy = &policy;
  input.now = 1001;
  input.epoch = CoordinatorEpoch::from(1);
  input.boot = BootId::from(1);
  SaturationTracker tracker;
  input.tracker = &tracker;
  const FabricAssessment assessment = detect(input);
  HGM_CHECK_EQ(assessment.hotspots.size(), static_cast<std::size_t>(0));
  HGM_CHECK_NE(assessment.scope, CongestionScope::Localized);
}
