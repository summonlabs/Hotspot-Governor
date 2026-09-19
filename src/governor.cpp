// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "hgm/governor.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

#include "hgm/byteio.hpp"
#include "hgm/checked.hpp"
#include "hgm/digest.hpp"
#include "hgm/limits.hpp"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <process.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace hgm {
namespace {

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::atomic<std::uint64_t> g_boot_counter{0};

std::string bounded(std::string text, std::size_t limit) {
  if (text.size() > limit) text.resize(limit);
  return text;
}

}  // namespace

Governor::Governor(GovernorConfig config) : config_(std::move(config)) {
  EngineConfig engine_config = config_.engine;
  engine_config.include_explanation =
      engine_config.include_explanation && config_.include_explanation;
  engine_ = Engine(engine_config);
  metrics_ = Metrics{};
  durable_.format_version = kDurableFormatVersion;
}

Governor::~Governor() { stop_workers(); }

BootId Governor::make_boot_id(Millis now) const {
  const std::uint64_t counter = g_boot_counter.fetch_add(1) + 1;
  std::string name = config_.node_name;
  name.push_back(':');
  name.append(std::to_string(process_id()));
  name.push_back(':');
  name.append(std::to_string(static_cast<std::uint64_t>(now)));
  name.push_back(':');
  name.append(std::to_string(counter));
  return BootId::from_name(name);
}

Status Governor::recover(Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  boot_ = make_boot_id(now);
  epoch_ = config_.initial_epoch.valid() ? config_.initial_epoch : CoordinatorEpoch::from(1);

  if (!config_.store.has_value()) {
    recovery_ = RecoveredState{};
    recovery_.notes.emplace_back("no durable store configured");
    return Status::success();
  }

  store_ = std::make_unique<Store>(config_.store.value());
  Status status = store_->open();
  if (!status.ok()) {
    store_.reset();
    return status;
  }

  Result<RecoveredState> recovered = store_->recover();
  if (!recovered.ok()) {
    // A missing snapshot is a fresh boot, not a failure. Corruption is.
    if (recovered.status().code() == ErrorCode::MissingEvidence) {
      recovery_ = RecoveredState{};
      recovery_.notes.emplace_back("fresh boot: no durable state present");
      return Status::success();
    }
    store_.reset();
    return recovered.status();
  }

  recovery_ = std::move(recovered).value();
  metrics_.count(metrics_.recoveries);

  // Restart always advances the epoch: an incarnation that died can never
  // resume its authority. Committed history is restored; live authority is not.
  if (recovery_.last_epoch.valid()) {
    epoch_ = recovery_.last_epoch.next();
    metrics_.count(metrics_.epoch_advances);
  }
  durable_ = recovery_.state;
  durable_.written_by_boot = boot_;
  durable_.epoch = epoch_;

  // Publisher authority, evidence and leases are explicitly not restored.
  publishers_.clear();
  snapshots_ = Snapshot{};
  engine_.reset();
  return Status::success();
}

Status Governor::install_epoch(CoordinatorEpoch epoch, BootId coordinator_boot, Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!epoch.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "epoch 0 is reserved");
  }
  if (epoch_.valid() && epoch < epoch_) {
    metrics_.count(metrics_.rejected_stale_epoch);
    return Status::failure(ErrorCode::StaleEpoch, "epoch is older than the installed epoch");
  }
  // Order matters: the new epoch becomes durable before it becomes live, so a
  // crash between the two can only lose an unacknowledged change.
  const BootId next_boot = coordinator_boot.valid() ? coordinator_boot : boot_;
  if (store_ != nullptr) {
    std::vector<std::byte> payload;
    {
      ByteWriter writer(payload);
      writer.u64(epoch.value());
      writer.u64(next_boot.value());
      writer.i64(now);
    }
    DurableState candidate = durable_;
    candidate.format_version = kDurableFormatVersion;
    candidate.written_by_boot = next_boot;
    candidate.epoch = epoch;
    candidate.evaluations = engine_.evaluations();
    candidate.written_at = now;
    candidate.journal_sequence = store_->journal_sequence();
    Status durable = store_->commit(
        candidate, {JournalRecord{JournalRecordType::EpochAdvance, 0, now, std::move(payload)}});
    if (!durable.ok()) return durable;
    durable_ = std::move(candidate);
  }

  const bool advanced = epoch_ != epoch;
  epoch_ = epoch;
  if (advanced) metrics_.count(metrics_.epoch_advances);
  boot_ = next_boot;

  // Fencing: every publisher registered under the old epoch loses authority.
  for (PublisherState& publisher : publishers_) {
    if (publisher.epoch != epoch_) {
      publisher.alive = false;
      publisher.died_at = now;
      publisher.generation = StreamGeneration{};
      publisher.stream_generations.fill(0);
      publisher.last_sequence = 0;
    }
  }
  return Status::success();
}

Status Governor::register_publisher(const HelloPayload& hello, Millis now, WelcomePayload& welcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!hello.publisher.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "publisher id 0 is reserved");
  }
  if (!hello.incarnation.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "publisher incarnation 0 is reserved");
  }
  if (!hello.boot.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "publisher boot id 0 is reserved");
  }
  if (!epoch_.valid()) {
    return Status::failure(ErrorCode::NotStarted, "no coordinator epoch is installed");
  }

  PublisherState* existing = find_publisher(hello.publisher);
  if (existing != nullptr) {
    if (hello.incarnation < existing->incarnation) {
      metrics_.count(metrics_.rejected_stale_incarnation);
      return Status::failure(ErrorCode::StaleIncarnation,
                             "publisher incarnation is older than the registered incarnation");
    }
    if (hello.incarnation == existing->incarnation) {
      if (hello.boot != existing->boot) {
        // Same incarnation, different process lifetime: an inconsistent claim.
        metrics_.count(metrics_.rejected_stale_incarnation);
        return Status::failure(ErrorCode::StaleIncarnation,
                               "publisher incarnation reused with a different boot id");
      }
      existing->alive = true;
      existing->epoch = epoch_;
      existing->last_seen = now;
    } else {
      // A strictly higher incarnation fences the previous one.
      existing->incarnation = hello.incarnation;
      existing->boot = hello.boot;
      existing->epoch = epoch_;
      existing->last_sequence = 0;
      existing->generation = StreamGeneration{};
      existing->stream_generations.fill(0);
      existing->alive = true;
      existing->first_seen = now;
      existing->last_seen = now;
      existing->died_at = kNoTime;
    }
  } else {
    if (publishers_.size() >= Limits::kMaxPublishers) {
      return Status::failure(ErrorCode::BoundExceeded, "publisher registry is full");
    }
    PublisherState state;
    state.id = hello.publisher;
    state.incarnation = hello.incarnation;
    state.boot = hello.boot;
    state.epoch = epoch_;
    state.first_seen = now;
    state.last_seen = now;
    state.alive = true;
    const auto position = std::lower_bound(
        publishers_.begin(), publishers_.end(), hello.publisher,
        [](const PublisherState& entry, PublisherId key) { return entry.id < key; });
    publishers_.insert(position, state);
  }

  welcome.epoch = epoch_;
  welcome.coordinator_boot = boot_;
  welcome.name = bounded(config_.node_name, Limits::kMaxTextBytes);
  welcome.protocol_version = kFrameProtocolVersion;
  welcome.heartbeat_ms = 1000;
  return Status::success();
}

Status Governor::note_publisher_death(PublisherId publisher, Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  PublisherState* state = find_publisher(publisher);
  if (state == nullptr) {
    return Status::failure(ErrorCode::UnknownPublisher, "publisher is not registered");
  }
  if (store_ != nullptr) {
    std::vector<std::byte> payload;
    {
      ByteWriter writer(payload);
      writer.u64(publisher.value());
      writer.i64(now);
    }
    DurableState candidate = durable_;
    candidate.format_version = kDurableFormatVersion;
    candidate.written_by_boot = boot_;
    candidate.epoch = epoch_;
    candidate.evaluations = engine_.evaluations();
    candidate.written_at = now;
    candidate.journal_sequence = store_->journal_sequence();
    Status durable = store_->commit(
        candidate, {JournalRecord{JournalRecordType::PublisherDeath, 0, now, std::move(payload)}});
    if (!durable.ok()) return durable;
    durable_ = std::move(candidate);
  }

  if (!state->alive) return Status::success();
  state->alive = false;
  state->died_at = now;
  metrics_.count(metrics_.publisher_deaths);

  if (config_.drop_evidence_on_publisher_death) {
    // Drop only the snapshots this publisher owned; other publishers' evidence
    // is untouched.
    if (snapshots_.topology.has_value() &&
        snapshots_.topology->provenance().publisher == publisher) {
      snapshots_.topology.reset();
    }
    if (snapshots_.capacity.has_value() &&
        snapshots_.capacity->provenance().publisher == publisher) {
      snapshots_.capacity.reset();
    }
    if (snapshots_.signals.has_value() &&
        snapshots_.signals->provenance().publisher == publisher) {
      snapshots_.signals.reset();
    }
    if (snapshots_.paths.has_value() && snapshots_.paths->provenance().publisher == publisher) {
      snapshots_.paths.reset();
    }
    if (snapshots_.traffic.has_value() && snapshots_.traffic->provenance().publisher == publisher) {
      snapshots_.traffic.reset();
    }
    if (snapshots_.policy.has_value() && snapshots_.policy->provenance().publisher == publisher) {
      snapshots_.policy.reset();
    }
  }

  return Status::success();
}

Status Governor::verify_references_locked() const {
  if (!snapshots_.topology.has_value()) {
    // Without a topology there is nothing to verify against; detection will
    // refuse the evidence for the same reason.
    return Status::success();
  }
  const TopologySnapshot& topology = snapshots_.topology.value();
  if (snapshots_.capacity.has_value() && !references_resolve(topology, snapshots_.capacity.value())) {
    return Status::failure(ErrorCode::UnknownResource,
                           "capacity names a resource the topology does not define");
  }
  if (snapshots_.signals.has_value() && !references_resolve(topology, snapshots_.signals.value())) {
    return Status::failure(ErrorCode::UnknownResource,
                           "signals name a resource the topology does not define");
  }
  if (snapshots_.paths.has_value() && !references_resolve(topology, snapshots_.paths.value())) {
    return Status::failure(ErrorCode::UnknownResource,
                           "a path traverses a resource the topology does not define");
  }
  return Status::success();
}

PublisherState* Governor::find_publisher(PublisherId id) {
  const auto it = std::lower_bound(publishers_.begin(), publishers_.end(), id,
                                   [](const PublisherState& entry, PublisherId key) {
                                     return entry.id < key;
                                   });
  if (it == publishers_.end() || it->id != id) return nullptr;
  return &(*it);
}

const PublisherState* Governor::find_publisher(PublisherId id) const {
  const auto it = std::lower_bound(publishers_.begin(), publishers_.end(), id,
                                   [](const PublisherState& entry, PublisherId key) {
                                     return entry.id < key;
                                   });
  if (it == publishers_.end() || it->id != id) return nullptr;
  return &(*it);
}

Result<AdmissionReport> Governor::admit(const Frame& frame, Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  AdmissionReport report;
  Status status = admit_locked(frame, now, report);
  if (!status.ok()) return status;
  return report;
}

Status Governor::admit_locked(const Frame& frame, Millis now, AdmissionReport& report) {
  metrics_.count(metrics_.frames_received);

  report.stream = stream_of(frame.header.type);
  report.generation = frame.header.generation;
  report.sequence = frame.header.sequence;

  Status status = verify_payload(frame.header,
                                 std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  if (!status.ok()) {
    metrics_.count(metrics_.frames_malformed);
    metrics_.count(metrics_.frames_rejected);
    return status;
  }

  // A heartbeat carries no stream payload; it only refreshes liveness, and it
  // does so under exactly the same epoch, incarnation and sequence fencing as
  // any other frame.
  if (frame.header.type == MessageType::Heartbeat) {
    PublisherState* heartbeat_publisher = find_publisher(frame.header.publisher);
    if (heartbeat_publisher == nullptr) {
      metrics_.count(metrics_.frames_rejected);
      return Status::failure(ErrorCode::UnknownPublisher, "publisher is not registered");
    }
    if (frame.header.incarnation != heartbeat_publisher->incarnation ||
        frame.header.boot != heartbeat_publisher->boot) {
      metrics_.count(metrics_.rejected_stale_incarnation);
      metrics_.count(metrics_.frames_rejected);
      return Status::failure(ErrorCode::StaleIncarnation,
                             "heartbeat incarnation does not match the registered publisher");
    }
    if (!heartbeat_publisher->alive) {
      metrics_.count(metrics_.frames_rejected);
      return Status::failure(ErrorCode::UnknownPublisher, "publisher incarnation is not alive");
    }
    if (frame.header.sequence != 0 && frame.header.sequence <= heartbeat_publisher->last_sequence) {
      metrics_.count(metrics_.frames_rejected);
      return Status::failure(frame.header.sequence == heartbeat_publisher->last_sequence
                                 ? ErrorCode::DuplicateFrame
                                 : ErrorCode::OutOfOrderSequence,
                             "heartbeat sequence is not newer than the last accepted sequence");
    }
    if (frame.header.sequence != 0) heartbeat_publisher->last_sequence = frame.header.sequence;
    heartbeat_publisher->last_seen = now;
    report.accepted = true;
    report.code = ErrorCode::Ok;
    metrics_.count(metrics_.frames_accepted);
    metrics_.count(metrics_.bytes_received,
                   static_cast<std::uint64_t>(frame.payload.size()) + kFrameHeaderBytes);
    return Status::success();
  }

  const StreamKind stream = report.stream;
  if (stream == StreamKind::Unknown) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::MalformedFrame, "message type does not carry a stream");
  }

  if (!epoch_.valid()) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::NotStarted, "no coordinator epoch is installed");
  }
  if (frame.header.epoch < epoch_) {
    metrics_.count(metrics_.rejected_stale_epoch);
    metrics_.count(metrics_.frames_stale);
    metrics_.count(metrics_.frames_rejected);
    report.stale = true;
    return Status::failure(ErrorCode::StaleEpoch,
                           "frame epoch " + frame.header.epoch.hex() + " is older than " + epoch_.hex());
  }
  if (frame.header.epoch > epoch_) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::UnknownEpoch,
                           "frame epoch " + frame.header.epoch.hex() + " is not installed");
  }

  PublisherState* publisher = find_publisher(frame.header.publisher);
  if (publisher == nullptr) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::UnknownPublisher, "publisher is not registered");
  }
  if (frame.header.incarnation != publisher->incarnation ||
      frame.header.boot != publisher->boot) {
    metrics_.count(metrics_.rejected_stale_incarnation);
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::StaleIncarnation,
                           "frame incarnation or boot id does not match the registered publisher");
  }
  if (!publisher->alive) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::UnknownPublisher,
                           "publisher incarnation is not alive; it must re-handshake");
  }
  if (publisher->epoch != epoch_) {
    publisher->epoch = epoch_;
    publisher->last_sequence = 0;
    publisher->generation = StreamGeneration{};
    publisher->stream_generations.fill(0);
  }
  if (frame.header.sequence == publisher->last_sequence) {
    metrics_.count(metrics_.frames_duplicate);
    metrics_.count(metrics_.frames_rejected);
    report.duplicate = true;
    return Status::failure(ErrorCode::DuplicateFrame, "frame sequence was already applied");
  }
  if (frame.header.sequence < publisher->last_sequence) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::OutOfOrderSequence, "frame sequence went backwards");
  }

  if (frame.header.generation == 0) {
    metrics_.count(metrics_.frames_rejected);
    return Status::failure(ErrorCode::InvalidArgument, "generation 0 is reserved");
  }
  const std::size_t slot = stream_index(stream);
  const std::uint64_t live_generation = publisher->stream_generations[slot];
  if (frame.header.generation == live_generation) {
    metrics_.count(metrics_.frames_duplicate);
    metrics_.count(metrics_.frames_rejected);
    report.duplicate = true;
    return Status::failure(ErrorCode::DuplicateFrame, "generation was already applied");
  }
  if (frame.header.generation < live_generation) {
    metrics_.count(metrics_.rejected_stale_generation);
    metrics_.count(metrics_.frames_stale);
    metrics_.count(metrics_.frames_rejected);
    report.stale = true;
    return Status::failure(ErrorCode::StaleGeneration, "generation went backwards");
  }
  const StreamGeneration incoming{stream, frame.header.generation};

  // Provenance of the frame that carried the payload. Decoded snapshots are
  // stamped with it so that ownership, attribution and publisher-death
  // handling can be answered without consulting the wire again.
  Provenance provenance;
  provenance.publisher = frame.header.publisher;
  provenance.incarnation = frame.header.incarnation;
  provenance.boot = frame.header.boot;
  provenance.epoch = frame.header.epoch;
  provenance.sequence = frame.header.sequence;
  provenance.generation = StreamGeneration{stream, frame.header.generation};
  provenance.emitted_at = frame.header.emitted_at;

  // Decode into a local; nothing is installed until the decode fully succeeds.
  std::size_t records = 0;
  switch (frame.header.type) {
    case MessageType::Topology: {
      TopologySnapshot snapshot;
      status = decode_topology_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                       snapshot);
      if (!status.ok()) break;
      if (snapshots_.topology.has_value() &&
          snapshot.generation() <= snapshots_.topology->generation()) {
        status = Status::failure(ErrorCode::StaleGeneration,
                                 "topology generation is not newer than the live snapshot");
        break;
      }
      records = snapshot.resource_count();
      snapshot.set_provenance(provenance);
      snapshots_.topology = std::move(snapshot);
      break;
    }
    case MessageType::Capacity: {
      CapacitySnapshot snapshot;
      status = decode_capacity_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                       snapshot);
      if (!status.ok()) break;
      records = snapshot.size();
      snapshot.set_provenance(provenance);
      snapshots_.capacity = std::move(snapshot);
      break;
    }
    case MessageType::Signals: {
      SignalSnapshot snapshot;
      status = decode_signals_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                      snapshot);
      if (!status.ok()) break;
      records = snapshot.size();
      snapshot.set_provenance(provenance);
      snapshots_.signals = std::move(snapshot);
      break;
    }
    case MessageType::Paths: {
      PathSnapshot snapshot;
      status = decode_paths_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                    snapshot);
      if (!status.ok()) break;
      records = snapshot.size();
      snapshot.set_provenance(provenance);
      snapshots_.paths = std::move(snapshot);
      break;
    }
    case MessageType::Traffic: {
      TrafficSnapshot snapshot;
      status = decode_traffic_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                      snapshot);
      if (!status.ok()) break;
      records = snapshot.size();
      snapshot.set_provenance(provenance);
      snapshots_.traffic = std::move(snapshot);
      break;
    }
    case MessageType::Policy: {
      PolicySnapshot snapshot;
      status = decode_policy_payload(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                                     snapshot);
      if (!status.ok()) break;
      snapshot.set_provenance(provenance);
      snapshots_.policy = std::move(snapshot);
      break;
    }
    default:
      status = Status::failure(ErrorCode::MalformedFrame, "message type is not a stream frame");
      break;
  }

  if (!status.ok()) {
    metrics_.count(metrics_.frames_rejected);
    if (is_integrity(status.code())) metrics_.count(metrics_.frames_malformed);
    if (is_stale(status.code())) metrics_.count(metrics_.frames_stale);
    return status;
  }

  // Structural cross-check against the live topology generation, performed once
  // per admitted frame instead of once per evaluation. A snapshot that names a
  // resource the topology does not define is refused outright and the previous
  // snapshot stays installed.
  {
    const Status verified = verify_references_locked();
    if (!verified.ok()) {
      metrics_.count(metrics_.frames_rejected);
      return verified;
    }
  }
  verified_topology_ = snapshots_.topology.has_value() ? snapshots_.topology->id() : TopologyId{};
  verified_capacity_ = snapshots_.capacity.has_value() ? snapshots_.capacity->id() : CapacityId{};
  verified_signals_ = snapshots_.signals.has_value() ? snapshots_.signals->id() : EvidenceId{};
  verified_paths_ = snapshots_.paths.has_value() ? snapshots_.paths->id() : PathSetId{};

  publisher->last_sequence = frame.header.sequence;
  publisher->generation = incoming;
  publisher->stream_generations[slot] = frame.header.generation;
  publisher->last_seen = now;
  publisher->alive = true;
  publisher->accepted = sat_add<std::uint64_t>(publisher->accepted, 1ull);

  report.accepted = true;
  report.code = ErrorCode::Ok;
  report.records = records;
  metrics_.count(metrics_.frames_accepted);
  metrics_.count(metrics_.bytes_received,
                 static_cast<std::uint64_t>(frame.payload.size()) + kFrameHeaderBytes);
  return Status::success();
}

Decision Governor::evaluate(Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  DecisionInput input;
  if (snapshots_.topology.has_value()) input.topology = &snapshots_.topology.value();
  if (snapshots_.capacity.has_value()) input.capacity = &snapshots_.capacity.value();
  if (snapshots_.signals.has_value()) input.signals = &snapshots_.signals.value();
  if (snapshots_.paths.has_value()) input.paths = &snapshots_.paths.value();
  if (snapshots_.traffic.has_value()) input.traffic = &snapshots_.traffic.value();
  if (snapshots_.policy.has_value()) input.policy = &snapshots_.policy.value();
  input.now = now;
  input.epoch = epoch_;
  input.boot = boot_;
  input.tracker = &engine_.tracker();
  input.references_preverified =
      snapshots_.topology.has_value() && verified_topology_ == snapshots_.topology->id() &&
      verified_capacity_ ==
          (snapshots_.capacity.has_value() ? snapshots_.capacity->id() : CapacityId{}) &&
      verified_signals_ ==
          (snapshots_.signals.has_value() ? snapshots_.signals->id() : EvidenceId{}) &&
      verified_paths_ ==
          (snapshots_.paths.has_value() ? snapshots_.paths->id() : PathSetId{});

  Decision decision = engine_.decide(input);
  record_decision_locked(decision, now);
  return decision;
}

void Governor::record_decision_locked(const Decision& decision, Millis now) {
  for (const Hotspot& hotspot : decision.assessment.hotspots) {
    DurableHotspot entry;
    entry.id = hotspot.id;
    entry.key = hotspot.key;
    entry.scope = hotspot.scope;
    entry.severity = hotspot.severity;
    entry.cause = hotspot.cause;
    entry.persistence_ms = hotspot.persistence_ms;
    entry.last_observed = now;
    entry.resource_count = hotspot.saturated_count;
    const auto it = std::find_if(durable_.hotspots.begin(), durable_.hotspots.end(),
                                 [&entry](const DurableHotspot& existing) { return existing.id == entry.id; });
    if (it != durable_.hotspots.end()) {
      *it = std::move(entry);
    } else {
      if (durable_.hotspots.size() >= std::min(config_.max_history, Limits::kMaxHotspots)) {
        durable_.hotspots.erase(durable_.hotspots.begin());
      }
      durable_.hotspots.push_back(std::move(entry));
    }
  }
  for (const MitigationIntent& intent : decision.plan.intents) {
    DurableIntervention entry;
    entry.id = intent.id;
    entry.kind = intent.kind;
    entry.hotspot = intent.hotspot;
    entry.at = now;
    entry.scope_share_ppm = intent.scope_share_ppm;
    entry.escalation = intent.escalation;
    durable_.interventions.push_back(std::move(entry));
  }
  const std::size_t max_interventions = Limits::kMaxHotspots * 4;
  if (durable_.interventions.size() > max_interventions) {
    durable_.interventions.erase(durable_.interventions.begin(),
                                 durable_.interventions.begin() +
                                     static_cast<std::ptrdiff_t>(durable_.interventions.size() -
                                                                 max_interventions));
  }
}

DurableState Governor::durable_snapshot_locked(Millis now) const {
  DurableState state = durable_;
  state.format_version = kDurableFormatVersion;
  state.written_by_boot = boot_;
  state.epoch = epoch_;
  state.evaluations = engine_.evaluations();
  state.written_at = now;
  state.journal_sequence = store_ != nullptr ? store_->journal_sequence() : 0;
  return state;
}

Status Governor::commit_durable(Millis now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (store_ == nullptr) {
    return Status::failure(ErrorCode::NotStarted, "no durable store is configured");
  }
  DurableState state = durable_snapshot_locked(now);
  std::vector<JournalRecord> records;
  records.push_back(JournalRecord{JournalRecordType::EvaluationCheckpoint, 0, now, {}});
  Status status = store_->commit(state, records);
  if (!status.ok()) return status;
  durable_ = state;
  metrics_.count(metrics_.journal_appends,
                 sat_add<std::uint64_t>(1ull, static_cast<std::uint64_t>(records.size())));
  return Status::success();
}

Status Governor::start_workers() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (workers_started_) return Status::failure(ErrorCode::AlreadyStarted, "workers already started");
  workers_ = std::make_unique<WorkerPool>(config_.worker_threads, config_.queue_capacity);
  Status status = workers_->start();
  if (!status.ok()) {
    workers_.reset();
    return status;
  }
  workers_started_ = true;
  return Status::success();
}

Status Governor::stop_workers() {
  std::unique_ptr<WorkerPool> workers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!workers_started_) return Status::success();
    workers_started_ = false;
    workers = std::move(workers_);
  }
  // Joining happens with no governor lock held, so a worker that needs the
  // governor can always finish.
  workers->drain_and_stop();
  return Status::success();
}

CoordinatorEpoch Governor::epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_;
}

BootId Governor::boot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return boot_;
}

std::string Governor::node_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_.node_name;
}

Metrics Governor::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Metrics merged = engine_.metrics();
  merged.frames_received = metrics_.frames_received;
  merged.frames_accepted = metrics_.frames_accepted;
  merged.frames_rejected = metrics_.frames_rejected;
  merged.frames_duplicate = metrics_.frames_duplicate;
  merged.frames_stale = metrics_.frames_stale;
  merged.frames_malformed = metrics_.frames_malformed;
  merged.bytes_received = metrics_.bytes_received;
  merged.bytes_sent = metrics_.bytes_sent;
  merged.publisher_deaths = metrics_.publisher_deaths;
  merged.epoch_advances = metrics_.epoch_advances;
  merged.journal_appends = metrics_.journal_appends;
  merged.journal_replays = metrics_.journal_replays;
  merged.recoveries = metrics_.recoveries;
  merged.rejected_stale_epoch = metrics_.rejected_stale_epoch;
  merged.rejected_stale_generation = metrics_.rejected_stale_generation;
  merged.rejected_stale_incarnation = metrics_.rejected_stale_incarnation;
  return merged;
}

std::vector<PublisherState> Governor::publishers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return publishers_;
}

SaturationTracker::Entry Governor::history_entry(ResourceId resource) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const SaturationTracker::Entry* entry = engine_.tracker().find(resource);
  return entry != nullptr ? *entry : SaturationTracker::Entry{};
}

std::size_t Governor::tracked_saturations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return engine_.tracker().size();
}

}  // namespace hgm
