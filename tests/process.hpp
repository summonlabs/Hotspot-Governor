// Real child OS processes for the multiprocess proof surface.
//
// Windows is the validated platform for this repository. The POSIX branch is
// implemented for portability but is not exercised by the validation run
// recorded in the README.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace hgmt {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { kill(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Starts the executable with its stdout and stderr redirected to
  // stdout_path. Returns false when the process could not be created.
  bool spawn(const std::string& executable, const std::vector<std::string>& args,
             const std::filesystem::path& stdout_path);

  bool spawned() const noexcept { return spawned_; }

  // Blocks until the process exits and returns its exit code.
  int wait();

  // Hard, ungraceful termination. This is the "kill -9" of the suite: no
  // shutdown path runs, no socket is closed politely.
  void kill();

  bool running() const;

 private:
  bool spawned_ = false;
  bool reaped_ = false;
  int exit_code_ = -1;
#if defined(_WIN32)
  void* process_ = nullptr;
#else
  int pid_ = -1;
#endif
};

// Reads a whole text file; returns an empty string when it does not exist.
std::string read_text(const std::filesystem::path& path);

// True when any line of the file contains the needle.
bool file_contains(const std::filesystem::path& path, const std::string& needle);

// Waits for a line to appear. The attempt budget exists so that a genuine
// failure surfaces as a failed test with a clear message; it is not a watchdog
// that hides a hang, and the test itself is never terminated by it.
bool wait_for_line(const std::filesystem::path& path, const std::string& needle,
                   unsigned attempts = 1500, unsigned sleep_ms = 20);

// Extracts the value of "key=" from the first line containing "prefix".
std::string extract_field(const std::filesystem::path& path, const std::string& prefix,
                          const std::string& key);

}  // namespace hgmt
