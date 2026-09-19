// Foundation: identities, digests, checked arithmetic, byte IO, enums.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "hgm/authority.hpp"
#include "hgm/byteio.hpp"
#include "hgm/checked.hpp"
#include "hgm/digest.hpp"
#include "hgm/enums.hpp"
#include "hgm/error.hpp"
#include "hgm/frame.hpp"
#include "hgm/ids.hpp"
#include "hgm/limits.hpp"
#include "hgm/time.hpp"
#include "testing.hpp"

using namespace hgm;

HGM_TEST(foundation, strong_ids_are_typed_and_zero_is_absent) {
  const ResourceId resource = ResourceId::from_name("res-a");
  const RegionId region = RegionId::from_name("region-a");
  HGM_CHECK(resource.valid());
  HGM_CHECK(region.valid());
  HGM_CHECK(!ResourceId{}.valid());
  HGM_CHECK(ResourceId{} == ResourceId{});
  static_assert(!std::is_same_v<ResourceId, RegionId>);
  HGM_CHECK_EQ(ResourceId::from_name("res-a").value(), resource.value());
  HGM_CHECK_NE(ResourceId::from_name("res-a"), ResourceId::from_name("res-b"));
}

HGM_TEST(foundation, generations_are_monotonic_and_saturate) {
  TopologyGeneration generation = TopologyGeneration::none();
  HGM_CHECK(!generation.valid());
  generation = generation.next();
  HGM_CHECK_EQ(generation.value(), 1ull);
  const TopologyGeneration top = TopologyGeneration::from(UINT64_MAX);
  HGM_CHECK_EQ(top.next().value(), UINT64_MAX);
}

HGM_TEST(foundation, digest_is_stable_and_detects_change) {
  const Digest a = digest_of("hotspot-governor");
  const Digest b = digest_of("hotspot-governor");
  const Digest c = digest_of("hotspot-governo");
  HGM_CHECK(a == b);
  HGM_CHECK(a != c);
  HGM_CHECK(a.valid());
  HGM_CHECK_EQ(hex64(0).size(), static_cast<std::size_t>(16));
  HGM_CHECK_EQ(hex64(0xabc).substr(12), std::string("0abc"));
}

HGM_TEST(foundation, checked_arithmetic_refuses_overflow) {
  std::uint64_t out = 0;
  HGM_CHECK(add_ok<std::uint64_t>(1, 2, out));
  HGM_CHECK_EQ(out, 3ull);
  HGM_CHECK(!add_ok<std::uint64_t>(UINT64_MAX, 1, out));
  HGM_CHECK(mul_ok<std::uint64_t>(3, 4, out));
  HGM_CHECK_EQ(out, 12ull);
  HGM_CHECK(!mul_ok<std::uint64_t>(UINT64_MAX, 2, out));
  HGM_CHECK(!sub_ok<std::uint64_t>(1, 2, out));
  HGM_CHECK_EQ(sat_add<std::uint64_t>(UINT64_MAX, 5), UINT64_MAX);
  HGM_CHECK(narrow<std::uint32_t>(5).has_value());
  HGM_CHECK(!narrow<std::uint32_t>(UINT64_MAX).has_value());
}

HGM_TEST(foundation, ratio_ppm_is_bounded_and_exact_at_edges) {
  HGM_CHECK_EQ(ratio_ppm(0, 0).has_value(), false);
  HGM_CHECK_EQ(*ratio_ppm(0, 100), 0u);
  HGM_CHECK_EQ(*ratio_ppm(100, 100), 1000000u);
  HGM_CHECK_EQ(*ratio_ppm(50, 100), 500000u);
  HGM_CHECK_EQ(*ratio_ppm(1, 3), 333333u);
  HGM_CHECK_EQ(*ratio_ppm(150, 100), 1500000u);
  HGM_CHECK_EQ(*ratio_ppm(UINT64_MAX, 1), UINT32_MAX);
  HGM_CHECK_EQ(percent_of(1, 4), 25u);
  HGM_CHECK_EQ(percent_of(1, 0), 0u);
  HGM_CHECK(within_tolerance(10, 12, 2));
  HGM_CHECK(!within_tolerance(10, 13, 2));
}

HGM_TEST(foundation, time_helpers_refuse_missing_and_future_evidence) {
  HGM_CHECK_EQ(elapsed_ms(100, 300), 200);
  HGM_CHECK_EQ(elapsed_ms(300, 100), 0);
  HGM_CHECK_EQ(elapsed_ms(kNoTime, 100), 0);
  HGM_CHECK_EQ(age_ms(kNoTime, 100), kMaxTime);
  HGM_CHECK_EQ(age_ms(500, 100), kMaxTime);
  HGM_CHECK_EQ(age_ms(100, 300), 200);
}

HGM_TEST(foundation, byte_io_round_trips_and_bounds) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x789ABCDEu);
    writer.u64(0x0123456789ABCDEFull);
    writer.i64(-7);
    writer.text_field("hello");
  }
  ByteReader reader(bytes);
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::int64_t e = 0;
  std::string f;
  HGM_CHECK(reader.u8(a));
  HGM_CHECK(reader.u16(b));
  HGM_CHECK(reader.u32(c));
  HGM_CHECK(reader.u64(d));
  HGM_CHECK(reader.i64(e));
  HGM_CHECK(reader.text_field(64, f));
  HGM_CHECK_EQ(static_cast<unsigned>(a), 0x12u);
  HGM_CHECK_EQ(static_cast<unsigned>(b), 0x3456u);
  HGM_CHECK_EQ(c, 0x789ABCDEu);
  HGM_CHECK_EQ(d, 0x0123456789ABCDEFull);
  HGM_CHECK_EQ(e, static_cast<std::int64_t>(-7));
  HGM_CHECK_EQ(f, std::string("hello"));
  HGM_CHECK(reader.at_end());
  HGM_CHECK(!reader.u8(a));
}

HGM_TEST(foundation, byte_io_rejects_oversized_length_prefix) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u32(1000);
    writer.raw(std::string("short"));
  }
  ByteReader reader(bytes);
  std::string text;
  HGM_CHECK(!reader.text_field(10, text));
}

HGM_TEST(foundation, error_codes_classify_staleness_and_integrity) {
  HGM_CHECK(is_stale(ErrorCode::StaleEpoch));
  HGM_CHECK(is_stale(ErrorCode::TopologyGenerationMismatch));
  HGM_CHECK(!is_stale(ErrorCode::Ok));
  HGM_CHECK(is_integrity(ErrorCode::IntegrityMismatch));
  HGM_CHECK(is_integrity(ErrorCode::PersistenceCorrupt));
  HGM_CHECK(!is_integrity(ErrorCode::StaleEpoch));
  const Status status = Status::failure(ErrorCode::StaleEpoch, "epoch 3 < 4");
  HGM_CHECK_EQ(status.text(), std::string("stale-epoch: epoch 3 < 4"));
  HGM_CHECK_EQ(Status::success().text(), std::string("ok"));
}

HGM_TEST(foundation, enums_render_stable_names) {
  HGM_CHECK_EQ(to_string(CongestionScope::Localized), std::string_view("localized"));
  HGM_CHECK_EQ(to_string(SaturationCause::FanInContention), std::string_view("fan-in-contention"));
  HGM_CHECK_EQ(to_string(MitigationKind::EscalateGlobalCongestion),
               std::string_view("escalate-global-congestion"));
  HGM_CHECK_EQ(to_string(StreamKind::Traffic), std::string_view("traffic"));
  HGM_CHECK_EQ(to_string(MessageType::Signals), std::string_view("signals"));
}

HGM_TEST(foundation, result_carries_value_or_status) {
  Result<int> good = 7;
  HGM_CHECK(good.ok());
  HGM_CHECK_EQ(good.value(), 7);
  Result<int> bad = Status::failure(ErrorCode::OutOfRange, "nope");
  HGM_CHECK(!bad.ok());
  HGM_CHECK_EQ(bad.status().code(), ErrorCode::OutOfRange);
}

HGM_TEST(foundation, authority_vector_refuses_when_any_finding_denies) {
  AuthorityVector authority;
  authority.grant(AuthorityDomain::Topology, "fresh");
  HGM_CHECK(authority.granted(AuthorityDomain::Topology));
  HGM_CHECK(!authority.granted(AuthorityDomain::Signals));
  authority.deny(AuthorityDomain::Topology, "later contradiction");
  HGM_CHECK(!authority.granted(AuthorityDomain::Topology));
  HGM_CHECK(!authority.all_granted());
  authority.seal();
  // One effective entry per domain: the last finding wins.
  HGM_CHECK_EQ(authority.grants().size(), static_cast<std::size_t>(1));
  HGM_CHECK(!authority.granted(AuthorityDomain::Topology));
  HGM_CHECK(!authority.granted(AuthorityDomain::Capacity));
  HGM_CHECK(authority.render().find("topology=denied") != std::string::npos);
}

HGM_TEST(foundation, limits_are_finite_and_ordered) {
  HGM_CHECK(Limits::kMaxFrameBytes > 0);
  HGM_CHECK(Limits::kMaxWorkerThreads >= 1);
  HGM_CHECK(Limits::kMaxResources >= Limits::kMaxHotspotResources);
  HGM_CHECK(Limits::kMaxHotspotResources >= Limits::kMaxContributors);
}
