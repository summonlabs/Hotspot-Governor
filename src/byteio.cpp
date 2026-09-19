// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/byteio.hpp"

namespace hgm {

void ByteWriter::u8(std::uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) {
  for (int shift = 0; shift < 16; shift += 8) {
    out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFull));
  }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::raw(std::span<const std::byte> bytes) {
  out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::raw(std::string_view text) {
  const auto* first = reinterpret_cast<const std::byte*>(text.data());
  out_.insert(out_.end(), first, first + text.size());
}

void ByteWriter::blob(std::span<const std::byte> bytes) {
  u32(static_cast<std::uint32_t>(bytes.size()));
  raw(bytes);
}

void ByteWriter::text_field(std::string_view text) {
  u32(static_cast<std::uint32_t>(text.size()));
  raw(text);
}

bool ByteReader::take(std::size_t count, std::span<const std::byte>& out) noexcept {
  if (count > remaining()) return false;
  out = in_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  std::span<const std::byte> bytes;
  if (!take(1, bytes)) return false;
  out = static_cast<std::uint8_t>(bytes[0]);
  return true;
}

bool ByteReader::u16(std::uint16_t& out) noexcept {
  std::span<const std::byte> bytes;
  if (!take(2, bytes)) return false;
  std::uint16_t value = 0;
  for (std::size_t i = 0; i < 2; ++i) {
    value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[i])) << (8 * i);
  }
  out = value;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  std::span<const std::byte> bytes;
  if (!take(4, bytes)) return false;
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << (8 * i);
  }
  out = value;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  std::span<const std::byte> bytes;
  if (!take(8, bytes)) return false;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[i])) << (8 * i);
  }
  out = value;
  return true;
}

bool ByteReader::i64(std::int64_t& out) noexcept {
  std::uint64_t value = 0;
  if (!u64(value)) return false;
  out = static_cast<std::int64_t>(value);
  return true;
}

bool ByteReader::raw(std::span<std::byte> out) noexcept {
  std::span<const std::byte> bytes;
  if (!take(out.size(), bytes)) return false;
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = bytes[i];
  return true;
}

bool ByteReader::skip(std::size_t count) noexcept {
  std::span<const std::byte> bytes;
  return take(count, bytes);
}

bool ByteReader::blob(std::size_t max_bytes, std::vector<std::byte>& out) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (static_cast<std::size_t>(length) > max_bytes) return false;
  std::span<const std::byte> bytes;
  if (!take(length, bytes)) return false;
  out.assign(bytes.begin(), bytes.end());
  return true;
}

bool ByteReader::text_field(std::size_t max_bytes, std::string& out) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (static_cast<std::size_t>(length) > max_bytes) return false;
  std::span<const std::byte> bytes;
  if (!take(length, bytes)) return false;
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

}  // namespace hgm
