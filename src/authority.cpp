// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/authority.hpp"

#include <algorithm>

#include "hgm/limits.hpp"

namespace hgm {

std::string_view to_string(AuthorityDomain domain) noexcept {
  switch (domain) {
    case AuthorityDomain::CoordinatorEpoch: return "coordinator-epoch";
    case AuthorityDomain::BootIncarnation: return "boot-incarnation";
    case AuthorityDomain::PublisherIncarnation: return "publisher-incarnation";
    case AuthorityDomain::Topology: return "topology";
    case AuthorityDomain::Capacity: return "capacity";
    case AuthorityDomain::Signals: return "signals";
    case AuthorityDomain::Paths: return "paths";
    case AuthorityDomain::Traffic: return "traffic";
    case AuthorityDomain::Policy: return "policy";
    case AuthorityDomain::Attribution: return "attribution";
    case AuthorityDomain::Persistence: return "persistence";
    case AuthorityDomain::MitigationBudget: return "mitigation-budget";
  }
  return "unknown";
}

void AuthorityVector::grant(AuthorityDomain domain, std::string reason, std::string binding) {
  grants_.push_back(AuthorityGrant{domain, true, std::move(reason), std::move(binding)});
}

void AuthorityVector::deny(AuthorityDomain domain, std::string reason, std::string binding) {
  grants_.push_back(AuthorityGrant{domain, false, std::move(reason), std::move(binding)});
}

bool AuthorityVector::granted(AuthorityDomain domain) const noexcept {
  // Any refusal for a domain means the domain is not granted: a later finding
  // must always be able to override an earlier one.
  bool seen = false;
  for (const AuthorityGrant& grant : grants_) {
    if (grant.domain != domain) continue;
    if (!grant.granted) return false;
    seen = true;
  }
  return seen;
}

bool AuthorityVector::all_granted() const noexcept {
  for (const AuthorityGrant& grant : grants_) {
    if (!grant.granted) return false;
  }
  return true;
}

void AuthorityVector::seal() {
  std::stable_sort(grants_.begin(), grants_.end(), [](const AuthorityGrant& a, const AuthorityGrant& b) {
    return static_cast<std::uint8_t>(a.domain) < static_cast<std::uint8_t>(b.domain);
  });
  // Keep the first occurrence of each domain so the vector stays a function of
  // the domain set.
  // One entry per domain: the last finding wins, so the rendered vector states
  // the effective authority rather than the first thing that happened.
  std::vector<AuthorityGrant> deduped;
  deduped.reserve(std::min(grants_.size(), Limits::kMaxAuthorityGrants));
  for (const AuthorityGrant& grant : grants_) {
    bool replaced = false;
    for (AuthorityGrant& kept : deduped) {
      if (kept.domain == grant.domain) {
        kept = grant;
        replaced = true;
        break;
      }
    }
    if (replaced) continue;
    if (deduped.size() >= Limits::kMaxAuthorityGrants) break;
    deduped.push_back(grant);
  }
  grants_.swap(deduped);
  std::stable_sort(grants_.begin(), grants_.end(), [](const AuthorityGrant& a, const AuthorityGrant& b) {
    return static_cast<std::uint8_t>(a.domain) < static_cast<std::uint8_t>(b.domain);
  });
}

std::string AuthorityVector::render() const {
  std::string out;
  for (const AuthorityGrant& grant : grants_) {
    if (!out.empty()) out.push_back(' ');
    out.append(to_string(grant.domain));
    out.push_back('=');
    out.append(grant.granted ? "granted" : "denied");
    if (!grant.reason.empty()) {
      out.push_back('(');
      out.append(grant.reason.substr(0, Limits::kMaxReasonBytes));
      out.push_back(')');
    }
  }
  return out;
}

}  // namespace hgm
