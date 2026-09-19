// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/time.hpp"

#include <chrono>

namespace hgm {

Millis SystemClock::now_ms() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace hgm
