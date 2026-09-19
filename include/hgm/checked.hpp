// Checked arithmetic for every externally influenced size, capacity, rate,
// counter and time unit.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace hgm {

inline constexpr std::uint64_t kPpmScale = 1000000ull;

template <class T>
constexpr bool add_ok(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "add_ok requires an unsigned type");
  if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) return false;
  out = static_cast<T>(a + b);
  return true;
}

template <class T>
constexpr bool sub_ok(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "sub_ok requires an unsigned type");
  if (b > a) return false;
  out = static_cast<T>(a - b);
  return true;
}

template <class T>
constexpr bool mul_ok(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "mul_ok requires an unsigned type");
  if (a != 0 && b > static_cast<T>(std::numeric_limits<T>::max() / a)) return false;
  out = static_cast<T>(a * b);
  return true;
}

// Saturating addition; used only where saturation is the documented behaviour.
template <class T>
constexpr T sat_add(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "sat_add requires an unsigned type");
  T out = 0;
  return add_ok(a, b, out) ? out : std::numeric_limits<T>::max();
}

// Narrowing conversion that fails instead of truncating.
template <class T>
constexpr std::optional<T> narrow(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return std::nullopt;
  return static_cast<T>(value);
}

constexpr bool fits_uint32(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

// part/whole expressed in parts-per-million, saturated at UINT32_MAX when the
// ratio is out of representable range. Returns nullopt when whole == 0.
inline std::optional<std::uint32_t> ratio_ppm(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0) return std::nullopt;
  // Compute in 128-bit-free fashion: part * 1e6 may overflow 64-bit for very
  // large parts, so reduce first.
  const std::uint64_t q = part / whole;
  if (q >= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  const std::uint64_t rem = part % whole;
  // rem < whole <= UINT64_MAX; rem * 1e6 could still overflow if whole is huge.
  std::uint64_t scaled = 0;
  if (!mul_ok<std::uint64_t>(rem, kPpmScale, scaled)) {
    // Fall back to a division-based approximation that cannot overflow.
    scaled = (rem / whole) * kPpmScale;  // always 0 here, keeps the branch honest
    scaled = static_cast<std::uint64_t>(static_cast<long double>(rem) * 1e6L /
                                        static_cast<long double>(whole));
  }
  const std::uint64_t total = q * kPpmScale + scaled / whole;
  if (total > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(total);
}

// Percentage of a bound, clamped to [0, 100].
inline std::uint32_t percent_of(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0) return 0;
  const auto ppm = ratio_ppm(part, whole);
  if (!ppm.has_value()) return 0;
  const std::uint64_t pct = static_cast<std::uint64_t>(*ppm) / 10000ull;
  return static_cast<std::uint32_t>(pct > 100 ? 100 : pct);
}

// True when |a - b| <= tolerance.
inline bool within_tolerance(std::uint64_t a, std::uint64_t b, std::uint64_t tolerance) noexcept {
  const std::uint64_t diff = a > b ? (a - b) : (b - a);
  return diff <= tolerance;
}

}  // namespace hgm
