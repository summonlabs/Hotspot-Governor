// Publisher/coordinator wire framing: a fixed 88-byte header plus a
// length-prefixed, CRC-64 protected payload.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hgm/capacity.hpp"
#include "hgm/error.hpp"
#include "hgm/ids.hpp"
#include "hgm/paths.hpp"
#include "hgm/policy.hpp"
#include "hgm/provenance.hpp"
#include "hgm/signals.hpp"
#include "hgm/time.hpp"
#include "hgm/topology.hpp"
#include "hgm/traffic.hpp"
#include "hgm/version.hpp"

namespace hgm {

inline constexpr std::uint32_t kFrameMagic = 0x464D4748u;  // "HGFM"
// Fixed wire header: magic(4) version(2) type(2) flags(4) payload_length(4)
// epoch, publisher, incarnation, boot, sequence, generation, emitted_at,
// payload_crc, header_crc (9 * 8).
inline constexpr std::size_t kFrameHeaderBytes = 4 + 2 + 2 + 4 + 4 + (8 * 9);  // 88

enum class MessageType : std::uint16_t {
  Unknown = 0,
  Hello = 1,
  Welcome = 2,
  Rejected = 3,
  Accepted = 4,
  Goodbye = 5,
  Topology = 10,
  Capacity = 11,
  Signals = 12,
  Paths = 13,
  Traffic = 14,
  Policy = 15,
  Heartbeat = 20,
};

std::string_view to_string(MessageType type) noexcept;

// Maps a message type onto the authoritative stream it publishes.
StreamKind stream_of(MessageType type) noexcept;

struct FrameHeader {
  std::uint16_t version = 0;
  MessageType type = MessageType::Unknown;
  std::uint32_t flags = 0;
  std::uint32_t payload_length = 0;
  CoordinatorEpoch epoch{};
  PublisherId publisher{};
  Incarnation incarnation{};
  BootId boot{};
  Sequence sequence = 0;
  std::uint64_t generation = 0;
  Millis emitted_at = kNoTime;
  std::uint64_t payload_crc = 0;
  std::uint64_t header_crc = 0;
};

struct Frame {
  FrameHeader header{};
  std::vector<std::byte> payload;

  std::size_t wire_size() const noexcept {
    return kFrameHeaderBytes + static_cast<std::size_t>(header.payload_length);
  }
};

// --- header / framing -------------------------------------------------------

// Encodes the header into exactly kFrameHeaderBytes bytes, computing and
// storing the header CRC.
void encode_header(const FrameHeader& header, std::vector<std::byte>& out);

// Decodes a header from exactly kFrameHeaderBytes bytes and verifies its CRC.
Result<FrameHeader> decode_header(std::span<const std::byte> bytes);

// Encodes a complete frame (header + payload) and verifies the payload CRC.
Status encode_frame(const Frame& frame, std::vector<std::byte>& out);

// Builds a frame for the given payload, computing the payload CRC.
Frame make_frame(MessageType type, const Provenance& provenance, std::vector<std::byte> payload);

// Verifies that the payload matches the header CRC and length. Never trusts
// the header's own claim without recomputation.
Status verify_payload(const FrameHeader& header, std::span<const std::byte> payload);

// --- payload codecs ---------------------------------------------------------

Status encode_topology_payload(const TopologySnapshot& snapshot, std::vector<std::byte>& out);
Status decode_topology_payload(std::span<const std::byte> payload, TopologySnapshot& out);

Status encode_capacity_payload(const CapacitySnapshot& snapshot, std::vector<std::byte>& out);
Status decode_capacity_payload(std::span<const std::byte> payload, CapacitySnapshot& out);

Status encode_signals_payload(const SignalSnapshot& snapshot, std::vector<std::byte>& out);
Status decode_signals_payload(std::span<const std::byte> payload, SignalSnapshot& out);

Status encode_paths_payload(const PathSnapshot& snapshot, std::vector<std::byte>& out);
Status decode_paths_payload(std::span<const std::byte> payload, PathSnapshot& out);

Status encode_traffic_payload(const TrafficSnapshot& snapshot, std::vector<std::byte>& out);
Status decode_traffic_payload(std::span<const std::byte> payload, TrafficSnapshot& out);

Status encode_policy_payload(const PolicySnapshot& snapshot, std::vector<std::byte>& out);
Status decode_policy_payload(std::span<const std::byte> payload, PolicySnapshot& out);

// --- control payloads -------------------------------------------------------

struct HelloPayload {
  PublisherId publisher{};
  Incarnation incarnation{};
  BootId boot{};
  std::uint32_t protocol_version = kFrameProtocolVersion;
  std::string name;
};

struct WelcomePayload {
  CoordinatorEpoch epoch{};
  BootId coordinator_boot{};
  std::uint32_t protocol_version = kFrameProtocolVersion;
  std::string name;
  std::uint32_t heartbeat_ms = 1000;
};

struct RejectedPayload {
  ErrorCode code = ErrorCode::Internal;
  std::string reason;
};

Status encode_hello(const HelloPayload& payload, std::vector<std::byte>& out);
Status decode_hello(std::span<const std::byte> payload, HelloPayload& out);
Status encode_welcome(const WelcomePayload& payload, std::vector<std::byte>& out);
Status decode_welcome(std::span<const std::byte> payload, WelcomePayload& out);
Status encode_rejected(const RejectedPayload& payload, std::vector<std::byte>& out);
Status decode_rejected(std::span<const std::byte> payload, RejectedPayload& out);

}  // namespace hgm
