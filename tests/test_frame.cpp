// Wire framing: round trips, integrity, bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>
#include <vector>

#include "hgm/byteio.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"
#include "hgm/frame.hpp"
#include "hgm/synthetic.hpp"
#include "testing.hpp"

using namespace hgm;

namespace {

FrameHeader round_trip_header(const FrameHeader& header) {
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  Result<FrameHeader> decoded = decode_header(std::span<const std::byte>(bytes.data(), bytes.size()));
  HGM_CHECK(decoded.ok());
  return decoded.value();
}

void recompute_header_crc(std::vector<std::byte>& bytes) {
  const std::uint64_t crc = crc64(std::span<const std::byte>(bytes.data(), kFrameHeaderBytes - 8));
  for (int i = 0; i < 8; ++i) {
    bytes[static_cast<std::size_t>(kFrameHeaderBytes - 8 + i)] =
        static_cast<std::byte>((crc >> (8 * i)) & 0xFFull);
  }
}

}  // namespace

HGM_TEST(frame, header_round_trips) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  header.type = MessageType::Signals;
  header.flags = 7;
  header.payload_length = 123;
  header.epoch = CoordinatorEpoch::from(9);
  header.publisher = PublisherId::from(11);
  header.incarnation = Incarnation::from(12);
  header.boot = BootId::from(13);
  header.sequence = 14;
  header.generation = 15;
  header.emitted_at = 16;
  header.payload_crc = 17;
  const FrameHeader decoded = round_trip_header(header);
  HGM_CHECK_EQ(decoded.type, MessageType::Signals);
  HGM_CHECK_EQ(decoded.payload_length, 123u);
  HGM_CHECK_EQ(decoded.epoch, CoordinatorEpoch::from(9));
  HGM_CHECK_EQ(decoded.publisher, PublisherId::from(11));
  HGM_CHECK_EQ(decoded.sequence, 14ull);
  HGM_CHECK_EQ(decoded.generation, 15ull);
  HGM_CHECK_EQ(decoded.emitted_at, static_cast<Millis>(16));
}

HGM_TEST(frame, truncated_header_is_refused) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  bytes.resize(kFrameHeaderBytes - 1);
  HGM_CHECK_EQ(decode_header(std::span<const std::byte>(bytes.data(), bytes.size())).status().code(),
               ErrorCode::TruncatedFrame);
}

HGM_TEST(frame, bad_magic_is_refused) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  bytes[0] = static_cast<std::byte>(0x00);
  recompute_header_crc(bytes);
  HGM_CHECK_EQ(decode_header(std::span<const std::byte>(bytes.data(), bytes.size())).status().code(),
               ErrorCode::MalformedFrame);
}

HGM_TEST(frame, unsupported_version_is_refused) {
  FrameHeader header;
  header.version = 99;
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  recompute_header_crc(bytes);
  HGM_CHECK_EQ(decode_header(std::span<const std::byte>(bytes.data(), bytes.size())).status().code(),
               ErrorCode::UnsupportedVersion);
}

HGM_TEST(frame, header_corruption_is_detected) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  header.sequence = 42;
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  bytes[30] = static_cast<std::byte>(static_cast<unsigned>(bytes[30]) ^ 0xFFu);
  HGM_CHECK_EQ(decode_header(std::span<const std::byte>(bytes.data(), bytes.size())).status().code(),
               ErrorCode::IntegrityMismatch);
}

HGM_TEST(frame, oversized_payload_is_refused) {
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(kFrameProtocolVersion);
  header.payload_length = static_cast<std::uint32_t>(Limits::kMaxFrameBytes) + 1u;
  std::vector<std::byte> bytes;
  encode_header(header, bytes);
  recompute_header_crc(bytes);
  HGM_CHECK_EQ(decode_header(std::span<const std::byte>(bytes.data(), bytes.size())).status().code(),
               ErrorCode::FrameTooLarge);
}

HGM_TEST(frame, payload_crc_is_verified) {
  Provenance provenance;
  provenance.epoch = CoordinatorEpoch::from(1);
  provenance.publisher = PublisherId::from(1);
  provenance.incarnation = Incarnation::from(1);
  provenance.boot = BootId::from(1);
  std::vector<std::byte> payload(32, std::byte{0x11});
  Frame frame = make_frame(MessageType::Heartbeat, provenance, std::move(payload));
  std::vector<std::byte> wire;
  HGM_CHECK(encode_frame(frame, wire).ok());
  Result<FrameHeader> header = decode_header(std::span<const std::byte>(wire.data(), wire.size()));
  HGM_CHECK(header.ok());
  HGM_CHECK(verify_payload(header.value(),
                           std::span<const std::byte>(wire.data() + kFrameHeaderBytes,
                                                     wire.size() - kFrameHeaderBytes))
                .ok());
  wire[kFrameHeaderBytes] = static_cast<std::byte>(static_cast<unsigned>(wire[kFrameHeaderBytes]) ^ 0x01u);
  HGM_CHECK_EQ(verify_payload(header.value(),
                              std::span<const std::byte>(wire.data() + kFrameHeaderBytes,
                                                        wire.size() - kFrameHeaderBytes))
                   .code(),
               ErrorCode::IntegrityMismatch);
  HGM_CHECK_EQ(verify_payload(header.value(),
                              std::span<const std::byte>(wire.data() + kFrameHeaderBytes,
                                                        wire.size() - kFrameHeaderBytes - 1))
                   .code(),
               ErrorCode::TruncatedFrame);
}

HGM_TEST(frame, every_stream_payload_round_trips) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);

  std::vector<std::byte> bytes;
  {
    TopologySnapshot decoded;
    HGM_CHECK(encode_topology_payload(fabric.topology(), bytes).ok());
    HGM_CHECK(decode_topology_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.id(), fabric.topology().id());
    HGM_CHECK_EQ(decoded.generation().value(), 5ull);
    HGM_CHECK_EQ(decoded.resource_count(), fabric.topology().resource_count());
  }
  {
    CapacitySnapshot decoded;
    HGM_CHECK(encode_capacity_payload(fabric.capacity(), bytes).ok());
    HGM_CHECK(decode_capacity_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.id(), fabric.capacity().id());
    HGM_CHECK_EQ(decoded.topology_generation().value(), 5ull);
  }
  {
    SignalSnapshot decoded;
    HGM_CHECK(encode_signals_payload(fabric.signals(), bytes).ok());
    HGM_CHECK(decode_signals_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.id(), fabric.signals().id());
    HGM_CHECK_EQ(decoded.size(), fabric.signals().size());
  }
  {
    PathSnapshot decoded;
    HGM_CHECK(encode_paths_payload(fabric.paths(), bytes).ok());
    HGM_CHECK(decode_paths_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.id(), fabric.paths().id());
  }
  {
    TrafficSnapshot decoded;
    HGM_CHECK(encode_traffic_payload(fabric.traffic(), bytes).ok());
    HGM_CHECK(decode_traffic_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.id(), fabric.traffic().id());
  }
  {
    PolicySnapshot decoded;
    HGM_CHECK(encode_policy_payload(fabric.policy(2000), bytes).ok());
    HGM_CHECK(decode_policy_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).ok());
    HGM_CHECK_EQ(decoded.content_digest(), fabric.policy(2000).content_digest());
    HGM_CHECK(!decoded.expired_at(2100));
  }
}

HGM_TEST(frame, payload_stream_kind_must_match_the_message) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);
  std::vector<std::byte> bytes;
  HGM_CHECK(encode_capacity_payload(fabric.capacity(), bytes).ok());
  SignalSnapshot decoded;
  HGM_CHECK_EQ(decode_signals_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).code(),
               ErrorCode::MalformedFrame);
}

HGM_TEST(frame, payload_with_trailing_bytes_is_refused) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);
  std::vector<std::byte> bytes;
  HGM_CHECK(encode_capacity_payload(fabric.capacity(), bytes).ok());
  bytes.push_back(std::byte{0});
  CapacitySnapshot decoded;
  HGM_CHECK_EQ(decode_capacity_payload(std::span<const std::byte>(bytes.data(), bytes.size()), decoded).code(),
               ErrorCode::MalformedFrame);
}

HGM_TEST(frame, truncated_payloads_are_refused) {
  SyntheticFabric fabric;
  fabric.rebuild(1000, 5);
  std::vector<std::byte> bytes;
  HGM_CHECK(encode_topology_payload(fabric.topology(), bytes).ok());
  for (std::size_t cut : {std::size_t{1}, std::size_t{5}, bytes.size() / 2, bytes.size() - 1}) {
    TopologySnapshot decoded;
    const Status status =
        decode_topology_payload(std::span<const std::byte>(bytes.data(), cut), decoded);
    HGM_CHECK(!status.ok());
  }
}

HGM_TEST(frame, control_payloads_round_trip) {
  std::vector<std::byte> bytes;
  HelloPayload hello;
  hello.publisher = PublisherId::from(3);
  hello.incarnation = Incarnation::from(4);
  hello.boot = BootId::from(5);
  hello.protocol_version = kFrameProtocolVersion;
  hello.name = "pub";
  HGM_CHECK(encode_hello(hello, bytes).ok());
  HelloPayload hello_out;
  HGM_CHECK(decode_hello(std::span<const std::byte>(bytes.data(), bytes.size()), hello_out).ok());
  HGM_CHECK_EQ(hello_out.publisher, PublisherId::from(3));
  HGM_CHECK_EQ(hello_out.name, std::string("pub"));

  hello.protocol_version = 12345;
  HGM_CHECK(encode_hello(hello, bytes).ok());
  HGM_CHECK_EQ(decode_hello(std::span<const std::byte>(bytes.data(), bytes.size()), hello_out).code(),
               ErrorCode::UnsupportedVersion);

  WelcomePayload welcome;
  welcome.epoch = CoordinatorEpoch::from(8);
  welcome.coordinator_boot = BootId::from(9);
  welcome.name = "coord";
  HGM_CHECK(encode_welcome(welcome, bytes).ok());
  WelcomePayload welcome_out;
  HGM_CHECK(decode_welcome(std::span<const std::byte>(bytes.data(), bytes.size()), welcome_out).ok());
  HGM_CHECK_EQ(welcome_out.epoch, CoordinatorEpoch::from(8));

  RejectedPayload rejected;
  rejected.code = ErrorCode::StaleEpoch;
  rejected.reason = "old";
  HGM_CHECK(encode_rejected(rejected, bytes).ok());
  RejectedPayload rejected_out;
  HGM_CHECK(decode_rejected(std::span<const std::byte>(bytes.data(), bytes.size()), rejected_out).ok());
  HGM_CHECK_EQ(rejected_out.code, ErrorCode::StaleEpoch);
  HGM_CHECK_EQ(rejected_out.reason, std::string("old"));
}

HGM_TEST(frame, oversized_text_field_is_refused) {
  std::vector<std::byte> bytes;
  {
    ByteWriter writer(bytes);
    writer.u64(1);
    writer.u64(1);
    writer.u64(1);
    writer.u32(kFrameProtocolVersion);
    writer.u32(100000);
    writer.raw(std::string(100000, 'x'));
  }
  HelloPayload hello;
  HGM_CHECK_EQ(decode_hello(std::span<const std::byte>(bytes.data(), bytes.size()), hello).code(),
               ErrorCode::TruncatedFrame);
}
