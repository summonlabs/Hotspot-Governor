// Hotspot Governor 1.0.0 — version and build identity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#define HGM_STRINGIFY_IMPL(x) #x
#define HGM_STRINGIFY(x) HGM_STRINGIFY_IMPL(x)

namespace hgm {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProductName = "Hotspot Governor";

// Version of the durable on-disk state format. Bumped whenever the persisted
// layout changes in an incompatible way.
inline constexpr std::uint32_t kDurableFormatVersion = 1;

// Version of the publisher/coordinator wire framing protocol.
inline constexpr std::uint32_t kFrameProtocolVersion = 1;

// Human readable build description: compiler and build configuration.
std::string_view build_info() noexcept;

// "1.0.0 (<compiler>)"
std::string version_string();

}  // namespace hgm
