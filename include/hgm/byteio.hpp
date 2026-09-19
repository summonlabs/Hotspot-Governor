// Explicit little-endian, bounds-checked binary encoding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hgm {

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::byte>& out) noexcept : out_(out) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void raw(std::span<const std::byte> bytes);
  void raw(std::string_view text);
  // Length-prefixed blob, rejected by readers above max_bytes.
  void blob(std::span<const std::byte> bytes);
  void text_field(std::string_view text);

  std::size_t size() const noexcept { return out_.size(); }

 private:
  std::vector<std::byte>& out_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> in) noexcept : in_(in) {}

  bool u8(std::uint8_t& out) noexcept;
  bool u16(std::uint16_t& out) noexcept;
  bool u32(std::uint32_t& out) noexcept;
  bool u64(std::uint64_t& out) noexcept;
  bool i64(std::int64_t& out) noexcept;
  bool raw(std::span<std::byte> out) noexcept;
  bool skip(std::size_t count) noexcept;
  bool blob(std::size_t max_bytes, std::vector<std::byte>& out);
  bool text_field(std::size_t max_bytes, std::string& out);

  std::size_t remaining() const noexcept { return in_.size() - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  bool at_end() const noexcept { return offset_ == in_.size(); }
  std::span<const std::byte> rest() const noexcept { return in_.subspan(offset_); }

 private:
  bool take(std::size_t count, std::span<const std::byte>& out) noexcept;

  std::span<const std::byte> in_;
  std::size_t offset_ = 0;
};

}  // namespace hgm
