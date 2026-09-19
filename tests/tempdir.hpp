// Scratch directory helper for durability tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace hgmt {

struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const std::string& label) {
    static std::atomic<unsigned> counter{0};
    const unsigned id = counter.fetch_add(1) + 1;
    path = std::filesystem::temp_directory_path() /
           ("hgm-test-" + label + "-" + std::to_string(id));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::filesystem::path file(const std::string& name) const { return path / name; }
};

}  // namespace hgmt
