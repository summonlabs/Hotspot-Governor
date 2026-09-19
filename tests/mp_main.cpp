// Multiprocess proof surface.
//
// Every test in this file starts REAL operating-system processes that talk to
// each other over REAL loopback TCP with the framed protocol. Publisher kills
// are ungraceful TerminateProcess/SIGKILL. Nothing here is simulated in
// process.
//
// Synchronisation uses bounded readiness polling so that a genuine failure is
// reported as a failed test with a clear message. No test timeout terminates a
// running test.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "process.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

namespace {

std::string executable_env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::string coordinator_binary() { return executable_env("HGM_COORDINATOR_BIN"); }
std::string publisher_binary() { return executable_env("HGM_PUBLISHER_BIN"); }

bool binaries_available() {
  return !coordinator_binary().empty() && !publisher_binary().empty() &&
         std::filesystem::exists(coordinator_binary()) && std::filesystem::exists(publisher_binary());
}

std::string field_of(const std::filesystem::path& path, const std::string& prefix,
                     const std::string& key) {
  return hgmt::extract_field(path, prefix, key);
}

std::uint64_t number_field(const std::filesystem::path& path, const std::string& prefix,
                           const std::string& key) {
  const std::string text = field_of(path, prefix, key);
  if (text.empty()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return 0;
  }
}

void require_binaries() {
  if (!binaries_available()) {
    HGM_FAIL("HGM_COORDINATOR_BIN and HGM_PUBLISHER_BIN must name existing executables");
  }
}

// Starts a coordinator and waits for its READY line. Returns the port.
std::uint16_t start_coordinator(hgmt::ChildProcess& process, const std::filesystem::path& log,
                                const std::vector<std::string>& extra = {}) {
  std::vector<std::string> args = {"--port", "0", "--name", "mp-coordinator"};
  args.insert(args.end(), extra.begin(), extra.end());
  HGM_CHECK(process.spawn(coordinator_binary(), args, log));
  if (!hgmt::wait_for_line(log, "READY port=")) {
    HGM_FAIL("coordinator did not report READY: " + hgmt::read_text(log));
  }
  const std::string port_text = field_of(log, "READY", "port");
  HGM_CHECK(!port_text.empty());
  return static_cast<std::uint16_t>(std::stoul(port_text));
}

std::vector<std::string> publisher_args(std::uint16_t port, const std::string& name,
                                        const std::string& incarnation, const std::string& boot) {
  return {"--host", "127.0.0.1",
          "--port", std::to_string(port),
          "--name", name,
          "--incarnation", incarnation,
          "--boot", boot,
          "--regions", "4",
          "--resources", "6",
          "--hotspots", "1",
          "--fan-in", "3"};
}

}  // namespace

HGM_TEST(mp, a_publisher_process_feeds_a_coordinator_process) {
  require_binaries();
  hgmt::TempDir dir("mp-basic");
  const std::filesystem::path coordinator_log = dir.file("coordinator.log");
  const std::filesystem::path publisher_log = dir.file("publisher.log");

  hgmt::ChildProcess coordinator;
  const std::uint16_t port = start_coordinator(coordinator, coordinator_log);
  HGM_CHECK(port != 0);

  hgmt::ChildProcess publisher;
  std::vector<std::string> args = publisher_args(port, "publisher-one", "1", "1");
  args.insert(args.end(), {"--rounds", "6"});
  HGM_CHECK(publisher.spawn(publisher_binary(), args, publisher_log));
  HGM_CHECK(publisher.wait() == 0);
  HGM_CHECK(hgmt::file_contains(publisher_log, "WELCOME epoch="));
  HGM_CHECK(hgmt::file_contains(publisher_log, "PUBLISHED "));

  // The coordinator evaluates once per admitted frame; six rounds publish
  // signals and traffic, so several evaluations must complete.
  HGM_CHECK(hgmt::wait_for_line(coordinator_log, "EVAL n="));
  HGM_CHECK(hgmt::wait_for_line(coordinator_log, "STATUS admitted="));
  const std::uint64_t admitted = number_field(coordinator_log, "STATUS", "admitted");
  HGM_CHECK(admitted >= 6);
  HGM_CHECK_EQ(number_field(coordinator_log, "STATUS", "refused"), 0ull);
  coordinator.kill();
}

HGM_TEST(mp, a_hard_killed_publisher_loses_its_authority) {
  require_binaries();
  hgmt::TempDir dir("mp-kill");
  const std::filesystem::path coordinator_log = dir.file("coordinator.log");
  const std::filesystem::path publisher_log = dir.file("publisher.log");

  hgmt::ChildProcess coordinator;
  const std::uint16_t port = start_coordinator(coordinator, coordinator_log);

  // A long-lived publisher: many rounds with a pause between them, so the
  // process is alive and mid-stream when it is killed.
  hgmt::ChildProcess publisher;
  std::vector<std::string> args = publisher_args(port, "publisher-victim", "1", "1");
  args.insert(args.end(), {"--rounds", "1000000", "--interval-ms", "5"});
  HGM_CHECK(publisher.spawn(publisher_binary(), args, publisher_log));
  HGM_CHECK(hgmt::wait_for_line(coordinator_log, "STATUS admitted="));
  HGM_CHECK(hgmt::wait_for_line(publisher_log, "WELCOME epoch="));
  HGM_CHECK(publisher.running());

  // Hard, ungraceful termination: no goodbye frame, no socket shutdown.
  publisher.kill();
  HGM_CHECK(!publisher.running());

  // The coordinator must observe the loss and revoke the incarnation.
  if (!hgmt::wait_for_line(coordinator_log, "lost=1")) {
    HGM_FAIL("coordinator never observed the publisher loss: " + hgmt::read_text(coordinator_log));
  }
  HGM_CHECK(number_field(coordinator_log, "STATUS", "lost") >= 1);

  // A fresh publisher incarnation is admitted afterwards.
  const std::uint64_t admitted_before = number_field(coordinator_log, "STATUS", "admitted");
  hgmt::ChildProcess successor;
  std::vector<std::string> successor_args = publisher_args(port, "publisher-victim", "2", "9");
  successor_args.insert(successor_args.end(), {"--rounds", "4"});
  HGM_CHECK(successor.spawn(publisher_binary(), successor_args, dir.file("successor.log")));
  HGM_CHECK(successor.wait() == 0);
  HGM_CHECK(hgmt::file_contains(dir.file("successor.log"), "PUBLISHED "));

  // The successor must actually be admitted, which is observable through the
  // STATUS line advancing past what the killed publisher had reached.
  bool grew = false;
  for (unsigned attempt = 0; attempt < 1500 && !grew; ++attempt) {
    if (number_field(coordinator_log, "STATUS", "admitted") > admitted_before) grew = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  HGM_CHECK(grew);

  // A stale incarnation of the same publisher is refused outright.
  hgmt::ChildProcess stale;
  std::vector<std::string> stale_args = publisher_args(port, "publisher-victim", "1", "1");
  stale_args.insert(stale_args.end(), {"--rounds", "1"});
  HGM_CHECK(stale.spawn(publisher_binary(), stale_args, dir.file("stale.log")));
  static_cast<void>(stale.wait());
  HGM_CHECK(hgmt::file_contains(dir.file("stale.log"), "handshake rejected"));
  coordinator.kill();
}

HGM_TEST(mp, an_unknown_epoch_is_refused_by_a_live_coordinator) {
  require_binaries();
  hgmt::TempDir dir("mp-epoch");
  const std::filesystem::path coordinator_log = dir.file("coordinator.log");
  const std::filesystem::path publisher_log = dir.file("publisher.log");

  hgmt::ChildProcess coordinator;
  const std::uint16_t port = start_coordinator(coordinator, coordinator_log);

  hgmt::ChildProcess publisher;
  std::vector<std::string> args = publisher_args(port, "epoch-forger", "1", "1");
  args.insert(args.end(), {"--rounds", "2", "--force-epoch", "4242"});
  HGM_CHECK(publisher.spawn(publisher_binary(), args, publisher_log));
  static_cast<void>(publisher.wait());

  if (!hgmt::wait_for_line(coordinator_log, "refused=")) {
    HGM_FAIL("coordinator never reported a refusal: " + hgmt::read_text(coordinator_log));
  }
  bool refused = false;
  for (unsigned attempt = 0; attempt < 1500 && !refused; ++attempt) {
    if (number_field(coordinator_log, "STATUS", "refused") > 0) refused = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  HGM_CHECK(refused);
  HGM_CHECK_EQ(number_field(coordinator_log, "STATUS", "admitted"), 0ull);
  coordinator.kill();
}

HGM_TEST(mp, coordinator_restart_advances_the_epoch_and_keeps_history) {
  require_binaries();
  hgmt::TempDir dir("mp-restart");
  const std::filesystem::path state_dir = dir.file("state");
  std::filesystem::create_directories(state_dir);
  const std::filesystem::path first_log = dir.file("first.log");
  const std::filesystem::path second_log = dir.file("second.log");
  const std::filesystem::path publisher_log = dir.file("publisher.log");

  std::uint16_t port = 0;
  std::string first_epoch;
  {
    hgmt::ChildProcess coordinator;
    port = start_coordinator(coordinator, first_log,
                             {"--state-dir", state_dir.string(), "--exit-after-evals", "3"});
    HGM_CHECK(port != 0);
    first_epoch = field_of(first_log, "READY", "epoch");
    HGM_CHECK(!first_epoch.empty());

    hgmt::ChildProcess publisher;
    std::vector<std::string> args = publisher_args(port, "restart-publisher", "1", "1");
    args.insert(args.end(), {"--rounds", "6"});
    HGM_CHECK(publisher.spawn(publisher_binary(), args, publisher_log));
    HGM_CHECK(publisher.wait() == 0);
    HGM_CHECK(hgmt::wait_for_line(first_log, "COMMIT ok"));
    HGM_CHECK(coordinator.wait() == 0);
  }

  // Second incarnation of the coordinator over the same durable directory.
  hgmt::ChildProcess second;
  const std::uint16_t second_port =
      start_coordinator(second, second_log, {"--state-dir", state_dir.string()});
  HGM_CHECK(second_port != 0);
  HGM_CHECK(hgmt::wait_for_line(second_log, "RECOVERED epoch="));
  const std::string second_epoch = field_of(second_log, "READY", "epoch");
  HGM_CHECK(!second_epoch.empty());
  HGM_CHECK_NE(first_epoch, second_epoch);
  // The recovered epoch is strictly greater than the epoch that was committed.
  HGM_CHECK(std::stoull(second_epoch, nullptr, 16) > std::stoull(first_epoch, nullptr, 16));

  // A publisher stamped with the pre-restart epoch is refused.
  hgmt::ChildProcess stale;
  std::vector<std::string> stale_args = publisher_args(second_port, "restart-publisher", "5", "5");
  stale_args.insert(stale_args.end(),
                    {"--rounds", "2", "--force-epoch", std::to_string(std::stoull(first_epoch, nullptr, 16))});
  HGM_CHECK(stale.spawn(publisher_binary(), stale_args, dir.file("stale.log")));
  static_cast<void>(stale.wait());
  bool refused = false;
  for (unsigned attempt = 0; attempt < 1500 && !refused; ++attempt) {
    if (number_field(second_log, "STATUS", "refused") > 0) refused = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  HGM_CHECK(refused);
  second.kill();
}

HGM_TEST(mp, two_coordinators_on_separate_ports_are_independent) {
  require_binaries();
  hgmt::TempDir dir("mp-two");
  hgmt::ChildProcess first;
  hgmt::ChildProcess second;
  const std::uint16_t first_port = start_coordinator(first, dir.file("first.log"));
  const std::uint16_t second_port = start_coordinator(second, dir.file("second.log"));
  HGM_CHECK_NE(first_port, second_port);

  hgmt::ChildProcess publisher;
  std::vector<std::string> args = publisher_args(first_port, "solo", "1", "1");
  args.insert(args.end(), {"--rounds", "4"});
  HGM_CHECK(publisher.spawn(publisher_binary(), args, dir.file("publisher.log")));
  HGM_CHECK(publisher.wait() == 0);

  HGM_CHECK(hgmt::wait_for_line(dir.file("first.log"), "EVAL n="));
  HGM_CHECK_EQ(number_field(dir.file("second.log"), "STATUS", "admitted"), 0ull);
  first.kill();
  second.kill();
}

int main(int argc, char** argv) {
  std::string suite;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--suite" && i + 1 < argc) suite = argv[++i];
  }
  return hgmtest::run_all(suite);
}
