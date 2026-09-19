// Coordinator: owns the epoch, accepts real publisher connections over real
// OS sockets, and feeds verified frames into the governor.
//
// Lock ordering (see docs/LOCKING.md): the coordinator mutex is never held
// while calling into the governor, and the governor mutex is never held while
// acquiring the coordinator mutex. Connection threads are joined with no lock
// held.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hgm/error.hpp"
#include "hgm/frame.hpp"
#include "hgm/governor.hpp"
#include "hgm/time.hpp"
#include "hgm/transport.hpp"

namespace hgm {

struct CoordinatorConfig {
  Endpoint endpoint{};
  std::string name = "hotspot-governor-coordinator";
  std::size_t max_connections = 32;
  std::uint32_t accept_timeout_ms = 200;
  std::uint32_t io_timeout_ms = 200;
};

class Coordinator {
 public:
  Coordinator(CoordinatorConfig config, Governor& governor);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Binds the listener, installs the initial coordinator epoch and starts the
  // accept loop.
  Status start();
  Status stop();

  Status run();

  std::uint16_t port() const;
  CoordinatorEpoch epoch() const;
  BootId boot() const;

  std::size_t connection_count() const;
  std::uint64_t connections_accepted() const;
  std::uint64_t connections_refused() const;
  std::uint64_t hellos() const;
  std::uint64_t frames_admitted() const;
  std::uint64_t frames_refused() const;
  std::uint64_t publishers_lost() const;
  std::string render() const;

  // Blocks until at least "count" publishers have completed the handshake.
  // Returns immediately when the coordinator stops.
  void wait_for_publishers(std::size_t count);

  // Blocks until at least "count" stream frames have been admitted.
  void wait_for_admissions(std::uint64_t count);

 private:
  struct Connection {
    Socket socket;
    std::thread thread;
    std::atomic<bool> stopping{false};
    PublisherId publisher{};
    bool registered = false;
  };

  void accept_loop();
  void connection_loop(const std::shared_ptr<Connection>& connection);
  void forget(const std::shared_ptr<Connection>& connection);
  void notify_progress();

  CoordinatorConfig config_;
  Governor* governor_ = nullptr;
  Listener listener_;
  std::thread accept_thread_;

  mutable std::mutex mutex_;
  std::condition_variable progress_;
  std::vector<std::shared_ptr<Connection>> connections_;
  bool running_ = false;
  std::uint64_t connections_accepted_ = 0;
  std::uint64_t connections_refused_ = 0;
  std::uint64_t hellos_ = 0;
  std::uint64_t frames_admitted_ = 0;
  std::uint64_t frames_refused_ = 0;
  std::uint64_t publishers_lost_ = 0;
};

}  // namespace hgm
