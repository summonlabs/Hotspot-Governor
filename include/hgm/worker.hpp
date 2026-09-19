// Bounded worker pool.
//
// Lock discipline (see also docs/LOCKING.md): the pool mutex is held only for
// queue and counter manipulation. Tasks run with no pool lock held, and the
// shutdown path joins threads after releasing the mutex, so a worker can never
// be asked to finish while the joiner holds a lock the worker needs.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hgm/error.hpp"

namespace hgm {

class WorkerPool {
 public:
  using Task = std::function<void()>;

  WorkerPool(std::size_t thread_count, std::size_t queue_capacity);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Starts the worker threads. Idempotent-safe: a second call is refused.
  Status start();

  // Enqueues a task. Never blocks and never runs the task inline: a full queue
  // is reported as QueueFull, and a stopped pool as ShuttingDown.
  Status submit(Task task);

  // Refuses new work but lets queued and active work finish.
  void stop_accepting();

  // Refuses new work, then drains the queue and joins the workers.
  void drain_and_stop();

  // Refuses new work, drops queued work, waits for active tasks, joins.
  void cancel_and_stop();

  // Clears the queue without stopping the pool. Returns how many tasks were
  // dropped. Active tasks are never interrupted.
  std::size_t drop_pending();

  // Blocks until the queue is empty and no task is active. Returns immediately
  // once the pool has stopped.
  void wait_until_idle();

  bool started() const noexcept;
  bool accepting() const noexcept;
  std::size_t pending() const noexcept;
  std::size_t active() const noexcept;
  // Configured worker bound (clamped at construction), independent of whether
  // the pool has been started yet.
  std::size_t thread_count() const noexcept { return thread_count_; }
  // Workers currently alive.
  std::size_t live_threads() const noexcept;
  std::size_t queue_capacity() const noexcept { return queue_capacity_; }

  std::uint64_t completed() const noexcept;
  std::uint64_t rejected() const noexcept;
  std::uint64_t dropped() const noexcept;
  std::uint64_t failed() const noexcept;

  std::string render() const;

 private:
  void worker_loop();

  const std::size_t thread_count_;
  const std::size_t queue_capacity_;

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<Task> queue_;
  std::vector<std::thread> threads_;

  bool started_ = false;
  bool accepting_ = false;
  bool stopping_ = false;

  std::size_t active_ = 0;
  std::uint64_t completed_ = 0;
  std::uint64_t rejected_ = 0;
  std::uint64_t dropped_ = 0;
  std::uint64_t failed_ = 0;
};

}  // namespace hgm
