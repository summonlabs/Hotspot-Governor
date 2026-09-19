// Evidence model validation and rejection.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>
#include <vector>

#include "hgm/capacity.hpp"
#include "hgm/paths.hpp"
#include "hgm/policy.hpp"
#include "hgm/signals.hpp"
#include "hgm/limits.hpp"
#include "hgm/topology.hpp"
#include "hgm/traffic.hpp"
#include "testing.hpp"

using namespace hgm;

namespace {

TopologySnapshot make_small_topology() {
  TopologySnapshot topology;
  topology.add_region(RegionId::from_name("r1"), "r1");
  topology.add_region(RegionId::from_name("r2"), "r2", RegionId::from_name("r1"));
  topology.add_resource(ResourceId::from_name("a"), RegionId::from_name("r1"), ResourceKind::Switch,
                        "a");
  topology.add_resource(ResourceId::from_name("b"), RegionId::from_name("r1"), ResourceKind::Port,
                        "b");
  topology.add_resource(ResourceId::from_name("c"), RegionId::from_name("r2"), ResourceKind::Port,
                        "c");
  topology.add_link(LinkId::from_name("ab"), ResourceId::from_name("a"), ResourceId::from_name("b"));
  topology.add_link(LinkId::from_name("bc"), ResourceId::from_name("b"), ResourceId::from_name("c"));
  return topology;
}

}  // namespace

HGM_TEST(evidence, topology_builds_and_indexes_adjacency) {
  TopologySnapshot topology = make_small_topology();
  HGM_CHECK(topology.build().ok());
  HGM_CHECK(topology.built());
  HGM_CHECK_EQ(topology.resource_count(), static_cast<std::size_t>(3));
  HGM_CHECK(topology.find_resource(ResourceId::from_name("b")) != nullptr);
  HGM_CHECK(topology.find_resource(ResourceId::from_name("zz")) == nullptr);
  HGM_CHECK(topology.id().valid());
  const auto neighbours = topology.neighbors(ResourceId::from_name("b"));
  HGM_CHECK_EQ(neighbours.size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(neighbours[0], ResourceId::from_name("a"));
  HGM_CHECK_EQ(neighbours[1], ResourceId::from_name("c"));
  HGM_CHECK_EQ(topology.neighbors(ResourceId::from_name("zz")).size(), static_cast<std::size_t>(0));
  const auto chain = topology.region_chain(ResourceId::from_name("c"));
  HGM_CHECK_EQ(chain.size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(chain[0], RegionId::from_name("r2"));
  HGM_CHECK_EQ(chain[1], RegionId::from_name("r1"));
  HGM_CHECK(topology.fresh_at(1000));
  topology.set_validity(0, 500);
  HGM_CHECK(!topology.fresh_at(1000));
}

HGM_TEST(evidence, topology_identity_is_content_derived) {
  TopologySnapshot first = make_small_topology();
  TopologySnapshot second = make_small_topology();
  HGM_CHECK(first.build().ok());
  HGM_CHECK(second.build().ok());
  HGM_CHECK_EQ(first.id(), second.id());
  second.add_resource(ResourceId::from_name("d"), RegionId::from_name("r1"), ResourceKind::Port, "d");
  HGM_CHECK(second.build().ok());
  HGM_CHECK_NE(first.id(), second.id());
}

HGM_TEST(evidence, topology_rejects_structural_defects) {
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_resource(ResourceId{}, RegionId::from_name("r"), ResourceKind::Port, "zero");
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_resource(ResourceId::from_name("a"), RegionId::from_name("missing"),
                          ResourceKind::Port, "a");
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::UnknownRegion);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_resource(ResourceId::from_name("a"), RegionId::from_name("r"), ResourceKind::Port,
                          "a");
    topology.add_link(LinkId::from_name("loop"), ResourceId::from_name("a"), ResourceId::from_name("a"));
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_resource(ResourceId::from_name("a"), RegionId::from_name("r"), ResourceKind::Port,
                          "a");
    topology.add_resource(ResourceId::from_name("b"), RegionId::from_name("r"), ResourceKind::Port,
                          "b");
    topology.add_link(LinkId::from_name("l1"), ResourceId::from_name("a"), ResourceId::from_name("b"));
    topology.add_link(LinkId::from_name("l2"), ResourceId::from_name("b"), ResourceId::from_name("a"));
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_region(RegionId::from_name("r"), "duplicate");
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r1"), "r1", RegionId::from_name("r1"));
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r1"), "r1", RegionId::from_name("r2"));
    topology.add_region(RegionId::from_name("r2"), "r2", RegionId::from_name("r1"));
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::InvalidArgument);
  }
  {
    TopologySnapshot topology;
    topology.add_region(RegionId::from_name("r"), "r");
    topology.add_resource(ResourceId::from_name("hub"), RegionId::from_name("r"), ResourceKind::Switch,
                          "hub");
    for (std::uint32_t i = 0; i <= Limits::kMaxNeighborsPerResource; ++i) {
      const ResourceId leaf = ResourceId::from(1000000u + i);
      topology.add_resource(leaf, RegionId::from_name("r"), ResourceKind::Port, "leaf");
      topology.add_link(LinkId::from(2000000u + i), ResourceId::from_name("hub"), leaf);
    }
    HGM_CHECK_EQ(topology.build().code(), ErrorCode::BoundExceeded);
  }
}

HGM_TEST(evidence, capacity_requires_positive_usable_capacity) {
  CapacitySnapshot capacity;
  capacity.add(ResourceId::from_name("a")).capacity_units = 100;
  capacity.add(ResourceId::from_name("b")).capacity_units = 0;
  HGM_CHECK_EQ(capacity.build().code(), ErrorCode::InvalidArgument);

  CapacitySnapshot ok;
  ok.add(ResourceId::from_name("a")).capacity_units = 100;
  HGM_CHECK(ok.build().ok());
  HGM_CHECK(ok.find(ResourceId::from_name("a")) != nullptr);
  HGM_CHECK(ok.find(ResourceId::from_name("b")) == nullptr);
  HGM_CHECK(!ok.find(ResourceId::from_name("a"))->usable == false);
}

HGM_TEST(evidence, capacity_rejects_duplicates) {
  CapacitySnapshot capacity;
  capacity.add(ResourceId::from_name("a")).capacity_units = 100;
  capacity.add(ResourceId::from_name("a")).capacity_units = 200;
  HGM_CHECK_EQ(capacity.build().code(), ErrorCode::InvalidArgument);
}

HGM_TEST(evidence, signals_bound_confidence_and_physical_consistency) {
  {
    SignalSnapshot signals;
    ResourceSignals& record = signals.add(ResourceId::from_name("a"));
    record.confidence_ppm = 2000000;
    HGM_CHECK_EQ(signals.build().code(), ErrorCode::OutOfRange);
  }
  {
    SignalSnapshot signals;
    ResourceSignals& record = signals.add(ResourceId::from_name("a"));
    record.offered_bps = 10;
    record.admitted_bps = 20;
    HGM_CHECK_EQ(signals.build().code(), ErrorCode::ContradictoryEvidence);
  }
  {
    SignalSnapshot signals;
    ResourceSignals& record = signals.add(ResourceId::from_name("a"));
    record.offered_bps = 20;
    record.admitted_bps = 10;
    record.confidence_ppm = 900000;
    HGM_CHECK(signals.build().ok());
    HGM_CHECK(signals.find(ResourceId::from_name("a")) != nullptr);
  }
  {
    SignalSnapshot signals;
    signals.add(ResourceId::from_name("a"));
    signals.add(ResourceId::from_name("a"));
    HGM_CHECK_EQ(signals.build().code(), ErrorCode::InvalidArgument);
  }
}

HGM_TEST(evidence, paths_index_resources_and_reject_empty_hops) {
  PathSnapshot paths;
  PathRecord& first = paths.add(PathId::from_name("p1"), FlowId::from_name("f1"));
  first.hops = {ResourceId::from_name("a"), ResourceId::from_name("b")};
  PathRecord& second = paths.add(PathId::from_name("p2"), FlowId::from_name("f2"));
  second.hops = {ResourceId::from_name("b"), ResourceId::from_name("c")};
  HGM_CHECK(paths.build().ok());
  HGM_CHECK_EQ(paths.paths_through(ResourceId::from_name("b")).size(), static_cast<std::size_t>(2));
  HGM_CHECK_EQ(paths.paths_through(ResourceId::from_name("a")).size(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(paths.paths_through(ResourceId::from_name("zz")).size(), static_cast<std::size_t>(0));

  PathSnapshot broken;
  broken.add(PathId::from_name("p1"), FlowId::from_name("f1"));
  HGM_CHECK_EQ(broken.build().code(), ErrorCode::InvalidArgument);

  PathSnapshot duplicate;
  PathRecord& a = duplicate.add(PathId::from_name("p1"), FlowId::from_name("f1"));
  a.hops = {ResourceId::from_name("a")};
  PathRecord& b = duplicate.add(PathId::from_name("p1"), FlowId::from_name("f2"));
  b.hops = {ResourceId::from_name("a")};
  HGM_CHECK_EQ(duplicate.build().code(), ErrorCode::InvalidArgument);
}

HGM_TEST(evidence, traffic_rejects_duplicate_records) {
  TrafficSnapshot traffic;
  traffic.add(FlowId::from_name("f1"), PathId::from_name("p1")).demand_bps = 10;
  traffic.add(FlowId::from_name("f1"), PathId::from_name("p1")).demand_bps = 20;
  HGM_CHECK_EQ(traffic.build().code(), ErrorCode::InvalidArgument);

  TrafficSnapshot ok;
  ok.add(FlowId::from_name("f1"), PathId::from_name("p1")).demand_bps = 10;
  ok.add(FlowId::from_name("f2"), PathId::from_name("p1")).demand_bps = 20;
  HGM_CHECK(ok.build().ok());
}

HGM_TEST(evidence, policy_expiry_refuses_backwards_clocks) {
  PolicySnapshot policy;
  policy.set_issued_at(1000);
  policy.set_ttl_ms(500);
  HGM_CHECK(!policy.expired_at(1200));
  HGM_CHECK(policy.expired_at(1600));
  HGM_CHECK(policy.expired_at(900));  // clock moved backwards
  policy.set_ttl_ms(0);
  HGM_CHECK(policy.expired_at(1000));
  policy.set_ttl_ms(500);
  policy.set_issued_at(kNoTime);
  HGM_CHECK(policy.expired_at(1000));
}

HGM_TEST(evidence, policy_content_digest_tracks_thresholds) {
  PolicySnapshot policy;
  const Digest before = policy.content_digest();
  policy.thresholds().saturation_utilization_ppm = 123456;
  HGM_CHECK(policy.content_digest() != before);
}
