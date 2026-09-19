// Integrity and identity digests (FNV-1a-64 for identity, CRC-64/XZ for integrity).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hgm {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;
std::uint64_t fnv1a64(std::string_view text) noexcept;

// CRC-64/XZ: poly 0x42F0E1EBA9EA3693 reflected, init/xorout all ones.
std::uint64_t crc64(std::span<const std::byte> bytes) noexcept;

struct Digest {
  std::uint64_t fnv = kFnvOffsetBasis;
  std::uint64_t crc = 0;

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.fnv == b.fnv && a.crc == b.crc;
  }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  bool valid() const noexcept { return crc != 0; }
};

Digest digest_of(std::span<const std::byte> bytes) noexcept;
Digest digest_of(std::string_view text) noexcept;

// Zero-padded lowercase hex rendering, fixed width 16.
std::string hex64(std::uint64_t value);

// Stable, deterministic 64-bit hash of a byte string. Never zero so that the
// value can always be used as a strong identity.
std::uint64_t identity_from_bytes(std::string_view text) noexcept;

}  // namespace hgm
