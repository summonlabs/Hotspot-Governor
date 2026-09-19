// Time abstraction. Durations are milliseconds; instants are epoch
// milliseconds supplied by the caller's authoritative clock.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>

namespace hgm {

using Millis = std::int64_t;

inline constexpr Millis kNoTime = std::numeric_limits<Millis>::min();
inline constexpr Millis kMaxTime = std::numeric_limits<Millis>::max();

// Elapsed milliseconds from "from" to "to", saturating instead of wrapping.
inline Millis elapsed_ms(Millis from, Millis to) noexcept {
  if (from == kNoTime || to == kNoTime) return 0;
  if (to <= from) return 0;
  const auto delta = static_cast<std::uint64_t>(to) - static_cast<std::uint64_t>(from);
  if (delta > static_cast<std::uint64_t>(kMaxTime)) return kMaxTime;
  return static_cast<Millis>(delta);
}

// Age of an observation. A missing observation, or one stamped in the future
// (clock moved backwards), is treated as maximally old so that it can never be
// mistaken for fresh evidence.
inline Millis age_ms(Millis observed, Millis now) noexcept {
  if (observed == kNoTime || now == kNoTime) return kMaxTime;
  if (now < observed) return kMaxTime;
  const auto delta = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(observed);
  if (delta >= static_cast<std::uint64_t>(kMaxTime)) return kMaxTime;
  return static_cast<Millis>(delta);
}

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;
  virtual Millis now_ms() const noexcept = 0;
};

// Wall clock. Used by real processes; never by deterministic tests.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  Millis now_ms() const noexcept override;
};

// Manually advanced clock for deterministic evaluation.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Millis start = 1000) noexcept : now_(start) {}
  Millis now_ms() const noexcept override { return now_; }
  void set(Millis value) noexcept { now_ = value; }
  void advance(Millis delta) noexcept { now_ += delta; }

 private:
  Millis now_ = 1000;
};

}  // namespace hgm
