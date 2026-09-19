// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "testing.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace hgmtest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void fail(const std::string& message, const char* file, int line) {
  throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

void check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) fail(std::string("check failed: ") + expression, file, line);
}

int run_all(const std::string& suite_filter) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : registry()) {
    if (!suite_filter.empty() && test.suite != suite_filter) continue;
    try {
      test.body();
      ++passed;
      std::printf("  ok   %s.%s\n", test.suite.c_str(), test.name.c_str());
    } catch (const std::exception& error) {
      ++failed;
      std::printf("  FAIL %s.%s: %s\n", test.suite.c_str(), test.name.c_str(), error.what());
      failures.push_back(test.suite + "." + test.name + ": " + error.what());
    } catch (...) {
      ++failed;
      std::printf("  FAIL %s.%s: unknown exception\n", test.suite.c_str(), test.name.c_str());
      failures.push_back(test.suite + "." + test.name + ": unknown exception");
    }
    std::fflush(stdout);
  }
  std::printf("\n%zu passed, %zu failed\n", passed, failed);
  for (const std::string& entry : failures) {
    std::printf("failure: %s\n", entry.c_str());
  }
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace hgmtest
