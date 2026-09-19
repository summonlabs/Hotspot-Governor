// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/digest.hpp"

namespace hgm {
namespace {

struct Crc64Table {
  std::uint64_t entries[256];

  constexpr Crc64Table() : entries{} {
    // Reflected form of 0x42F0E1EBA9EA3693.
    constexpr std::uint64_t kReflectedPoly = 0xC96C5795D7870F42ull;
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint64_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1ull) != 0ull) ? ((crc >> 1) ^ kReflectedPoly) : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

constexpr Crc64Table kCrcTable{};

}  // namespace

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const std::byte b : bytes) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(b));
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  return fnv1a64(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::uint64_t crc64(std::span<const std::byte> bytes) noexcept {
  std::uint64_t crc = 0xFFFFFFFFFFFFFFFFull;
  for (const std::byte b : bytes) {
    const auto index = static_cast<std::size_t>(
        (crc ^ static_cast<std::uint64_t>(static_cast<unsigned char>(b))) & 0xFFull);
    crc = kCrcTable.entries[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

Digest digest_of(std::span<const std::byte> bytes) noexcept {
  Digest d;
  d.fnv = fnv1a64(bytes);
  d.crc = crc64(bytes);
  return d;
}

Digest digest_of(std::string_view text) noexcept {
  return digest_of(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string hex64(std::uint64_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[value & 0xFull];
    value >>= 4;
  }
  return out;
}

std::uint64_t identity_from_bytes(std::string_view text) noexcept {
  std::uint64_t value = fnv1a64(text);
  if (value == 0) value = 1;
  return value;
}

}  // namespace hgm
