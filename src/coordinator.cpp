// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "hgm/checked.hpp"
#include "hgm/limits.hpp"
#include "hgm/version.hpp"

namespace hgm {

Coordinator::Coordinator(CoordinatorConfig config, Governor& governor)
    : config_(std::move(config)), governor_(&governor) {}

Coordinator::~Coordinator() { static_cast<void>(stop()); }

Status Coordinator::start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return Status::failure(ErrorCode::AlreadyStarted, "coordinator is already running");
  }
  Status status = listener_.bind_and_listen(config_.endpoint, 32);
  if (!status.ok()) return status;

  status = governor_->install_epoch(governor_->epoch(), governor_->boot(), 0);
  if (!status.ok()) {
    listener_.close();
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return Status::success();
}

Status Coordinator::run() {
  // Blocks until stop() is called from another thread.
  std::unique_lock<std::mutex> lock(mutex_);
  progress_.wait(lock, [this]() { return !running_; });
  return Status::success();
}

Status Coordinator::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ && !accept_thread_.joinable() && connections_.empty()) {
      listener_.close();
      return Status::success();
    }
    running_ = false;
  }
  listener_.close();
  progress_.notify_all();
  if (accept_thread_.joinable()) accept_thread_.join();

  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connections.swap(connections_);
  }
  for (const std::shared_ptr<Connection>& connection : connections) {
    connection->stopping = true;
    connection->socket.shutdown_both();
  }
  for (const std::shared_ptr<Connection>& connection : connections) {
    if (connection->thread.joinable()) connection->thread.join();
  }
  progress_.notify_all();
  return Status::success();
}

void Coordinator::accept_loop() {
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) break;
    }
    Result<Socket> accepted = listener_.accept_one(config_.accept_timeout_ms);
    if (!accepted.ok()) {
      if (accepted.status().code() == ErrorCode::Timeout) continue;
      if (accepted.status().code() == ErrorCode::NotStarted) break;
      break;
    }

    std::size_t existing = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      existing = connections_.size();
    }
    if (existing >= std::min<std::size_t>(config_.max_connections, Limits::kMaxConnections)) {
      Frame frame;
      frame.header.type = MessageType::Rejected;
      frame.header.epoch = governor_->epoch();
      std::vector<std::byte> payload;
      static_cast<void>(encode_rejected(RejectedPayload{ErrorCode::QueueFull,
                                                        "connection limit reached"},
                                        payload));
      frame.payload = std::move(payload);
      static_cast<void>(send_frame(accepted.value(), frame));
      accepted.value().close();
      std::lock_guard<std::mutex> lock(mutex_);
      ++connections_refused_;
      continue;
    }

    static_cast<void>(accepted.value().set_receive_timeout(config_.io_timeout_ms));
    static_cast<void>(accepted.value().set_send_timeout(config_.io_timeout_ms * 10u));

    auto connection = std::make_shared<Connection>();
    connection->socket = std::move(accepted).value();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++connections_accepted_;
      connections_.push_back(connection);
    }
    connection->thread = std::thread([this, connection]() { connection_loop(connection); });
    notify_progress();
  }
}

void Coordinator::connection_loop(const std::shared_ptr<Connection>& connection) {
  const auto now = []() { return SystemClock{}.now_ms(); };

  for (;;) {
    if (connection->stopping) break;
    Result<Frame> received = receive_frame(connection->socket);
    if (!received.ok()) {
      const ErrorCode code = received.status().code();
      if (code == ErrorCode::Timeout) continue;
      if (code != ErrorCode::ConnectionClosed) {
        // Malformed, truncated, oversized or integrity-failing input is a
        // refusal and must be accounted as one.
        std::lock_guard<std::mutex> lock(mutex_);
        ++frames_refused_;
        notify_progress();
      }
      break;
    }
    Frame frame = std::move(received).value();

    if (!connection->registered) {
      if (frame.header.type != MessageType::Hello) {
        Frame reply;
        reply.header.type = MessageType::Rejected;
        reply.header.epoch = governor_->epoch();
        std::vector<std::byte> payload;
        static_cast<void>(encode_rejected(
            RejectedPayload{ErrorCode::MalformedFrame, "first frame must be a hello"}, payload));
        reply.payload = std::move(payload);
        static_cast<void>(send_frame(connection->socket, reply));
        break;
      }
      HelloPayload hello;
      Status status = decode_hello(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), hello);
      if (!status.ok()) {
        Frame reply;
        reply.header.type = MessageType::Rejected;
        reply.header.epoch = governor_->epoch();
        std::vector<std::byte> payload;
        static_cast<void>(encode_rejected(RejectedPayload{status.code(), status.detail()}, payload));
        reply.payload = std::move(payload);
        static_cast<void>(send_frame(connection->socket, reply));
        break;
      }

      WelcomePayload welcome;
      // Called with no coordinator lock held.
      status = governor_->register_publisher(hello, now(), welcome);
      if (!status.ok()) {
        Frame reply;
        reply.header.type = MessageType::Rejected;
        reply.header.epoch = governor_->epoch();
        std::vector<std::byte> payload;
        static_cast<void>(encode_rejected(RejectedPayload{status.code(), status.detail()}, payload));
        reply.payload = std::move(payload);
        static_cast<void>(send_frame(connection->socket, reply));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++frames_refused_;
        }
        notify_progress();
        break;
      }

      Frame reply;
      reply.header.type = MessageType::Welcome;
      reply.header.epoch = welcome.epoch;
      std::vector<std::byte> payload;
      static_cast<void>(encode_welcome(welcome, payload));
      reply.payload = std::move(payload);
      status = send_frame(connection->socket, reply);
      if (!status.ok()) break;

      connection->registered = true;
      connection->publisher = hello.publisher;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++hellos_;
      }
      notify_progress();
      continue;
    }

    if (frame.header.type == MessageType::Goodbye) break;

    // Called with no coordinator lock held.
    Result<AdmissionReport> admitted = governor_->admit(frame, now());
    if (admitted.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++frames_admitted_;
      notify_progress();
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++frames_refused_;
    }
    Frame reply;
    reply.header.type = MessageType::Rejected;
    reply.header.epoch = governor_->epoch();
    std::vector<std::byte> payload;
    static_cast<void>(encode_rejected(
        RejectedPayload{admitted.status().code(), admitted.status().detail()}, payload));
    reply.payload = std::move(payload);
    static_cast<void>(send_frame(connection->socket, reply));
    notify_progress();

    // A stale epoch or a fenced incarnation ends the session; the publisher
    // must re-handshake against the current authority.
    const ErrorCode code = admitted.status().code();
    if (code == ErrorCode::StaleEpoch || code == ErrorCode::UnknownEpoch ||
        code == ErrorCode::StaleIncarnation || code == ErrorCode::UnknownPublisher) {
      break;
    }
  }

  if (connection->registered && connection->publisher.valid()) {
    static_cast<void>(governor_->note_publisher_death(connection->publisher, now()));
    std::lock_guard<std::mutex> lock(mutex_);
    ++publishers_lost_;
    notify_progress();
  }
  connection->socket.close();
  forget(connection);
}

void Coordinator::forget(const std::shared_ptr<Connection>& connection) {
  bool erased = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find(connections_.begin(), connections_.end(), connection);
    if (it != connections_.end()) {
      connections_.erase(it);
      erased = true;
    }
  }
  // This runs on the connection's own thread. A connection that has retired
  // itself is no longer visible to stop(), so its thread handle must be
  // detached here: leaving it joinable would make destroying the handle call
  // std::terminate. When stop() got there first the connection is no longer in
  // the list and stop() joins it instead, so exactly one of the two paths owns
  // the handle.
  if (erased && connection->thread.joinable()) connection->thread.detach();
}

void Coordinator::notify_progress() { progress_.notify_all(); }

std::uint16_t Coordinator::port() const { return listener_.port(); }

CoordinatorEpoch Coordinator::epoch() const { return governor_->epoch(); }

BootId Coordinator::boot() const { return governor_->boot(); }

std::size_t Coordinator::connection_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connections_.size();
}

std::uint64_t Coordinator::connections_accepted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connections_accepted_;
}

std::uint64_t Coordinator::connections_refused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connections_refused_;
}

std::uint64_t Coordinator::hellos() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hellos_;
}

std::uint64_t Coordinator::frames_admitted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_admitted_;
}

std::uint64_t Coordinator::frames_refused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_refused_;
}

std::uint64_t Coordinator::publishers_lost() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return publishers_lost_;
}

std::string Coordinator::render() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  out.reserve(256);
  out.append("coordinator port=");
  out.append(std::to_string(listener_.port()));
  out.append(" connections=");
  out.append(std::to_string(connections_.size()));
  out.append(" accepted=");
  out.append(std::to_string(connections_accepted_));
  out.append(" refused=");
  out.append(std::to_string(connections_refused_));
  out.append(" hellos=");
  out.append(std::to_string(hellos_));
  out.append(" frames_admitted=");
  out.append(std::to_string(frames_admitted_));
  out.append(" frames_refused=");
  out.append(std::to_string(frames_refused_));
  out.append(" publishers_lost=");
  out.append(std::to_string(publishers_lost_));
  return out;
}

void Coordinator::wait_for_publishers(std::size_t count) {
  std::unique_lock<std::mutex> lock(mutex_);
  progress_.wait(lock, [this, count]() { return hellos_ >= count || !running_; });
}

void Coordinator::wait_for_admissions(std::uint64_t count) {
  std::unique_lock<std::mutex> lock(mutex_);
  progress_.wait(lock, [this, count]() { return frames_admitted_ >= count || !running_; });
}

}  // namespace hgm
