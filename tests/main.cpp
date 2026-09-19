// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "testing.hpp"

int main(int argc, char** argv) {
  std::string suite;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--suite" && i + 1 < argc) {
      suite = argv[++i];
    }
  }
  return hgmtest::run_all(suite);
}
