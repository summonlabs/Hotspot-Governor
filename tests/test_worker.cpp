// Bounded worker pool: bounds, cancellation and shutdown.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "hgm/limits.hpp"
#include "hgm/worker.hpp"
#include "testing.hpp"

using namespace hgm;

HGM_TEST(worker, runs_submitted_tasks_to_completion) {
  WorkerPool pool(4, 64);
  HGM_CHECK(pool.start().ok());
  std::atomic<int> counter{0};
  for (int i = 0; i < 32; ++i) {
    HGM_CHECK(pool.submit([&counter]() { counter.fetch_add(1); }).ok());
  }
  pool.wait_until_idle();
  HGM_CHECK_EQ(counter.load(), 32);
  HGM_CHECK_EQ(pool.completed(), 32ull);
  HGM_CHECK_EQ(pool.pending(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(pool.active(), static_cast<std::size_t>(0));
  pool.drain_and_stop();
  HGM_CHECK(!pool.started());
}

HGM_TEST(worker, refuses_work_when_the_queue_is_full) {
  WorkerPool pool(1, 1);
  HGM_CHECK(pool.start().ok());
  std::atomic<bool> release{false};
  HGM_CHECK(pool.submit([&release]() {
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }).ok());
  // Wait for the single worker to pick the task up, then fill and overflow the
  // queue.
  for (int i = 0; i < 1000 && pool.active() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  HGM_CHECK(pool.submit([]() {}).ok());
  const Status status = pool.submit([]() {});
  HGM_CHECK_EQ(status.code(), ErrorCode::QueueFull);
  HGM_CHECK(pool.rejected() >= 1);
  release.store(true);
  pool.drain_and_stop();
}

HGM_TEST(worker, stop_accepting_refuses_new_work_but_drains) {
  WorkerPool pool(2, 32);
  HGM_CHECK(pool.start().ok());
  std::atomic<int> counter{0};
  for (int i = 0; i < 8; ++i) HGM_CHECK(pool.submit([&counter]() { counter.fetch_add(1); }).ok());
  pool.stop_accepting();
  const Status status = pool.submit([&counter]() { counter.fetch_add(1); });
  HGM_CHECK_EQ(status.code(), ErrorCode::ShuttingDown);
  pool.drain_and_stop();
  HGM_CHECK_EQ(counter.load(), 8);
}

HGM_TEST(worker, cancellation_drops_queued_work_without_running_it) {
  WorkerPool pool(1, 64);
  HGM_CHECK(pool.start().ok());
  std::atomic<bool> release{false};
  std::atomic<int> started{0};
  HGM_CHECK(pool.submit([&release, &started]() {
    started.fetch_add(1);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }).ok());
  for (int i = 0; i < 1000 && started.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::atomic<int> queued{0};
  for (int i = 0; i < 16; ++i) {
    HGM_CHECK(pool.submit([&queued]() { queued.fetch_add(1); }).ok());
  }
  // Drop the queue while the single worker is still busy, then release it.
  HGM_CHECK_EQ(pool.drop_pending(), static_cast<std::size_t>(16));
  HGM_CHECK_EQ(pool.pending(), static_cast<std::size_t>(0));
  release.store(true);
  pool.cancel_and_stop();
  HGM_CHECK_EQ(queued.load(), 0);
  HGM_CHECK(pool.dropped() >= 16);
  HGM_CHECK(!pool.started());
}

HGM_TEST(worker, a_throwing_task_is_accounted_not_fatal) {
  WorkerPool pool(2, 16);
  HGM_CHECK(pool.start().ok());
  HGM_CHECK(pool.submit([]() { throw std::runtime_error("boom"); }).ok());
  std::atomic<int> counter{0};
  HGM_CHECK(pool.submit([&counter]() { counter.fetch_add(1); }).ok());
  pool.wait_until_idle();
  HGM_CHECK_EQ(counter.load(), 1);
  HGM_CHECK_EQ(pool.failed(), 1ull);
  HGM_CHECK_EQ(pool.completed(), 2ull);
  pool.drain_and_stop();
}

HGM_TEST(worker, submitting_from_inside_a_task_does_not_deadlock) {
  WorkerPool pool(2, 16);
  HGM_CHECK(pool.start().ok());
  std::atomic<int> counter{0};
  HGM_CHECK(pool.submit([&pool, &counter]() {
    for (int i = 0; i < 4; ++i) {
      const Status status = pool.submit([&counter]() { counter.fetch_add(1); });
      HGM_CHECK(status.ok());
    }
  }).ok());
  pool.wait_until_idle();
  HGM_CHECK_EQ(counter.load(), 4);
  pool.drain_and_stop();
}

HGM_TEST(worker, thread_and_queue_bounds_are_clamped) {
  WorkerPool pool(Limits::kMaxWorkerThreads * 4, Limits::kMaxQueueDepth * 4);
  HGM_CHECK_EQ(pool.thread_count(), Limits::kMaxWorkerThreads);
  HGM_CHECK_EQ(pool.queue_capacity(), Limits::kMaxQueueDepth);
  WorkerPool tiny(0, 0);
  HGM_CHECK_EQ(tiny.thread_count(), static_cast<std::size_t>(1));
  HGM_CHECK_EQ(tiny.queue_capacity(), static_cast<std::size_t>(1));
}

HGM_TEST(worker, double_start_is_refused) {
  WorkerPool pool(1, 4);
  HGM_CHECK(pool.start().ok());
  HGM_CHECK_EQ(pool.start().code(), ErrorCode::AlreadyStarted);
  pool.drain_and_stop();
}

HGM_TEST(worker, empty_task_is_refused) {
  WorkerPool pool(1, 4);
  HGM_CHECK(pool.start().ok());
  HGM_CHECK_EQ(pool.submit(WorkerPool::Task{}).code(), ErrorCode::InvalidArgument);
  pool.drain_and_stop();
}

HGM_TEST(worker, render_reports_bounded_accounting) {
  WorkerPool pool(2, 4);
  HGM_CHECK(pool.start().ok());
  HGM_CHECK(pool.submit([]() {}).ok());
  pool.wait_until_idle();
  const std::string text = pool.render();
  HGM_CHECK(text.find("threads=2/2") != std::string::npos);
  HGM_CHECK(text.find("completed=1") != std::string::npos);
  HGM_CHECK_EQ(pool.live_threads(), static_cast<std::size_t>(2));
  pool.drain_and_stop();
  HGM_CHECK_EQ(pool.live_threads(), static_cast<std::size_t>(0));
  HGM_CHECK_EQ(pool.thread_count(), static_cast<std::size_t>(2));
}
