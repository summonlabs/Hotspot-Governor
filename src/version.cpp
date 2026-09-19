// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/version.hpp"

namespace hgm {

std::string_view build_info() noexcept {
#if defined(_MSC_VER)
  #if defined(NDEBUG)
  return "msvc-" HGM_STRINGIFY(_MSC_VER) "-release";
  #else
  return "msvc-" HGM_STRINGIFY(_MSC_VER) "-debug";
  #endif
#elif defined(__clang__)
  return "clang-" __clang_version__;
#elif defined(__GNUC__)
  return "gcc-" __VERSION__;
#else
  return "unknown-compiler";
#endif
}

std::string version_string() {
  std::string out;
  out.reserve(64);
  out.append(kVersionString);
  out.append(" (");
  out.append(build_info());
  out.push_back(')');
  return out;
}

}  // namespace hgm
