// The authority vector: the explicit list of authorities consulted for a
// decision, and whether each one granted.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hgm {

enum class AuthorityDomain : std::uint8_t {
  CoordinatorEpoch = 0,
  BootIncarnation = 1,
  PublisherIncarnation = 2,
  Topology = 3,
  Capacity = 4,
  Signals = 5,
  Paths = 6,
  Traffic = 7,
  Policy = 8,
  Attribution = 9,
  Persistence = 10,
  MitigationBudget = 11,
};

std::string_view to_string(AuthorityDomain domain) noexcept;

struct AuthorityGrant {
  AuthorityDomain domain = AuthorityDomain::Topology;
  bool granted = false;
  std::string reason;
  std::string binding;
};

class AuthorityVector {
 public:
  void grant(AuthorityDomain domain, std::string reason, std::string binding = std::string());
  void deny(AuthorityDomain domain, std::string reason, std::string binding = std::string());

  bool granted(AuthorityDomain domain) const noexcept;
  bool all_granted() const noexcept;
  bool empty() const noexcept { return grants_.empty(); }

  std::span<const AuthorityGrant> grants() const noexcept { return grants_; }

  // Sorts by domain and caps the number of grants. Deterministic.
  void seal();

  std::string render() const;

 private:
  std::vector<AuthorityGrant> grants_;
};

}  // namespace hgm
