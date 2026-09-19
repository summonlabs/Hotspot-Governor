// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/worker.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "hgm/limits.hpp"

namespace hgm {

WorkerPool::WorkerPool(std::size_t thread_count, std::size_t queue_capacity)
    : thread_count_(std::max<std::size_t>(1, std::min(thread_count, Limits::kMaxWorkerThreads))),
      queue_capacity_(std::max<std::size_t>(1, std::min(queue_capacity, Limits::kMaxQueueDepth))) {}

WorkerPool::~WorkerPool() { cancel_and_stop(); }

Status WorkerPool::start() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (started_) return Status::failure(ErrorCode::AlreadyStarted, "worker pool is already started");
  started_ = true;
  accepting_ = true;
  stopping_ = false;
  threads_.reserve(thread_count_);
  for (std::size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this]() { worker_loop(); });
  }
  return Status::success();
}

Status WorkerPool::submit(Task task) {
  if (!task) return Status::failure(ErrorCode::InvalidArgument, "task is empty");
  std::unique_lock<std::mutex> lock(mutex_);
  if (!accepting_ || stopping_) {
    ++rejected_;
    return Status::failure(ErrorCode::ShuttingDown, "worker pool is not accepting work");
  }
  if (queue_.size() >= queue_capacity_) {
    ++rejected_;
    return Status::failure(ErrorCode::QueueFull, "worker queue is full");
  }
  queue_.push_back(std::move(task));
  lock.unlock();
  work_available_.notify_one();
  return Status::success();
}

void WorkerPool::stop_accepting() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
  }
  work_available_.notify_all();
}

void WorkerPool::drain_and_stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    stopping_ = true;
  }
  work_available_.notify_all();
  // Join with no lock held: a worker must never need the pool mutex to exit,
  // and the joiner must never hold state the workers require.
  for (std::thread& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
  threads_.clear();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }
  idle_.notify_all();
}

std::size_t WorkerPool::drop_pending() {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t dropped = queue_.size();
  dropped_ += static_cast<std::uint64_t>(dropped);
  queue_.clear();
  return dropped;
}

void WorkerPool::cancel_and_stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    stopping_ = true;
    dropped_ += static_cast<std::uint64_t>(queue_.size());
    queue_.clear();
  }
  work_available_.notify_all();
  for (std::thread& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
  threads_.clear();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }
  idle_.notify_all();
}

void WorkerPool::wait_until_idle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this]() { return (queue_.empty() && active_ == 0) || !started_; });
}

void WorkerPool::worker_loop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) break;
        continue;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      ++active_;
    }
    try {
      task();
    } catch (const std::exception&) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++failed_;
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++failed_;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_ > 0) --active_;
      ++completed_;
    }
    idle_.notify_all();
  }
  idle_.notify_all();
}

bool WorkerPool::started() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_;
}

bool WorkerPool::accepting() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return accepting_ && !stopping_;
}

std::size_t WorkerPool::pending() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::size_t WorkerPool::live_threads() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return threads_.size();
}

std::size_t WorkerPool::active() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

std::uint64_t WorkerPool::completed() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_;
}

std::uint64_t WorkerPool::rejected() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return rejected_;
}

std::uint64_t WorkerPool::dropped() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

std::uint64_t WorkerPool::failed() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

std::string WorkerPool::render() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out = "threads=";
  out.append(std::to_string(threads_.size()));
  out.append("/");
  out.append(std::to_string(thread_count_));
  out.append(" pending=");
  out.append(std::to_string(queue_.size()));
  out.append(" active=");
  out.append(std::to_string(active_));
  out.append(" completed=");
  out.append(std::to_string(completed_));
  out.append(" rejected=");
  out.append(std::to_string(rejected_));
  out.append(" dropped=");
  out.append(std::to_string(dropped_));
  out.append(" failed=");
  out.append(std::to_string(failed_));
  return out;
}

}  // namespace hgm
