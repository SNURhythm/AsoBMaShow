#include "RealtimeGameplayWorker.h"

#include "../../bms_parser.hpp"
#include "../../targets.h"

#include <algorithm>
#include <chrono>
#include <utility>

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include <pthread.h>
#endif

namespace gameplay {

RealtimeGameplayWorker::SnapshotLease::SnapshotLease(
    const RealtimeGameplayWorker *owner, std::size_t index,
    const RealtimeGameplaySnapshot *snapshot) noexcept
    : owner_(owner), index_(index), snapshot_(snapshot) {}

RealtimeGameplayWorker::SnapshotLease::SnapshotLease(
    SnapshotLease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_),
      snapshot_(std::exchange(other.snapshot_, nullptr)) {}

RealtimeGameplayWorker::SnapshotLease &
RealtimeGameplayWorker::SnapshotLease::operator=(SnapshotLease &&other) noexcept {
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
    index_ = other.index_;
    snapshot_ = std::exchange(other.snapshot_, nullptr);
  }
  return *this;
}

RealtimeGameplayWorker::SnapshotLease::~SnapshotLease() { release(); }

void RealtimeGameplayWorker::SnapshotLease::release() noexcept {
  if (owner_ != nullptr) {
    owner_->releaseSnapshot(index_);
  }
  owner_ = nullptr;
  snapshot_ = nullptr;
}

RealtimeGameplayWorker::RealtimeGameplayWorker(
    GameplayDefinition definition, RealtimeGameplayWorkerConfig config)
    : definition_(std::move(definition)), config_(std::move(config)),
      simulation_(definition_, config_.simulation) {
  for (auto &buffer : snapshots_) {
    buffer.snapshot.noteStates.resize(definition_.noteCount());
  }
  publishSnapshot();
}

RealtimeGameplayWorker::~RealtimeGameplayWorker() { stop(); }

bool RealtimeGameplayWorker::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return false;
  }
  if (fault() != RealtimeGameplayFault::None) {
    started_.store(false, std::memory_order_release);
    return false;
  }
  stopRequested_.store(false, std::memory_order_release);
  thread_ = std::thread([this] { run(); });
  return true;
}

void RealtimeGameplayWorker::stop() {
  if (!started_.load(std::memory_order_acquire)) {
    return;
  }
  stopRequested_.store(true, std::memory_order_release);
  signal();
  if (thread_.joinable()) {
    thread_.join();
  }
  started_.store(false, std::memory_order_release);
}

bool RealtimeGameplayWorker::enqueueInput(
    const RealtimeGameplayInput &input) noexcept {
  if (input.epoch != config_.epoch ||
      fault() != RealtimeGameplayFault::None) {
    return false;
  }
  if (!ingress_.tryPush(input)) {
    latchFault(RealtimeGameplayFault::IngressOverflow);
    return false;
  }
  signal();
  return true;
}

RealtimeGameplayWorker::SnapshotLease
RealtimeGameplayWorker::acquireLatestSnapshot() const noexcept {
  for (;;) {
    const std::size_t index =
        latestSnapshot_.load(std::memory_order_acquire);
    snapshots_[index].readers.fetch_add(1, std::memory_order_acq_rel);
    if (latestSnapshot_.load(std::memory_order_acquire) == index) {
      return SnapshotLease(this, index, &snapshots_[index].snapshot);
    }
    snapshots_[index].readers.fetch_sub(1, std::memory_order_release);
  }
}

RealtimeGameplayFault RealtimeGameplayWorker::fault() const noexcept {
  return fault_.load(std::memory_order_acquire);
}

bool RealtimeGameplayWorker::running() const noexcept {
  return started_.load(std::memory_order_acquire) &&
         !stopRequested_.load(std::memory_order_acquire);
}

std::vector<GameplayReplayEvent>
RealtimeGameplayWorker::copyReplayEventsAfterStop() const {
  if (running()) {
    return {};
  }
  const auto events = simulation_.replayEvents();
  return {events.begin(), events.end()};
}

void RealtimeGameplayWorker::run() {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  using namespace std::chrono_literals;
  while (!stopRequested_.load(std::memory_order_acquire)) {
    (void)wake_.try_acquire_for(1ms);
    wakePending_.store(false, std::memory_order_release);

    bool changed = false;
    RealtimeGameplayInput input;
    while (ingress_.tryPop(input)) {
      processInput(input);
      changed = true;
      if (fault() != RealtimeGameplayFault::None) {
        break;
      }
    }
    if (fault() == RealtimeGameplayFault::None) {
      changed = advanceAutomatic() || changed;
    }
    if (changed || fault() != RealtimeGameplayFault::None) {
      publishSnapshot();
    }

    if (fault() != RealtimeGameplayFault::None) {
      stopRequested_.store(true, std::memory_order_release);
      break;
    }

    while (ingress_.tryPop(input)) {
      processInput(input);
      publishSnapshot();
      if (fault() != RealtimeGameplayFault::None) {
        stopRequested_.store(true, std::memory_order_release);
        break;
      }
    }
  }
}

void RealtimeGameplayWorker::signal() noexcept {
  if (!wakePending_.exchange(true, std::memory_order_acq_rel)) {
    wake_.release();
  }
}

void RealtimeGameplayWorker::processInput(
    const RealtimeGameplayInput &input) {
  if (input.epoch != config_.epoch || config_.clock.mapSteadyToSong == nullptr) {
    return;
  }
  const auto songTime = config_.clock.mapSteadyToSong(
      config_.clock.context, input.steadyTimestampMicros);
  if (!songTime.has_value()) {
    latchFault(RealtimeGameplayFault::ClockUnavailable);
    return;
  }

  const GameplayInputContext context{
      .songTimeMicros = *songTime,
      .laneBeamTimeMicros = input.steadyTimestampMicros,
      .inputDelayMicros = input.inputDelayMicros,
  };
  simulation_.advanceTo(*songTime, input.steadyTimestampMicros);
  if (simulation_.terminal()) {
    return;
  }

  if (input.type == RealtimeGameplayInputType::Release) {
    latestTransaction_ =
        simulation_.releaseLane(input.lane, context, input.backSpin);
    ++transactionSequence_;
    return;
  }

  const int compensateLane =
      input.compensateLane >= 0 ? input.compensateLane : input.lane;
  const NoteId preview = simulation_.previewPressSoundNote(
      input.lane, compensateLane, context);
  const bool requiresSound =
      config_.inputTriggeredKeysounds && preview != kInvalidNoteId &&
      definition_.note(preview).wav != bms_parser::Parser::NoWav;
  RealtimeGameplayAudioReservation reservation;
  if (requiresSound &&
      (config_.audio.reserve == nullptr ||
       !config_.audio.reserve(config_.audio.context, preview, reservation))) {
    latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
    return;
  }

  latestTransaction_ =
      simulation_.pressLane(input.lane, compensateLane, context);
  ++transactionSequence_;
  if (!requiresSound) {
    return;
  }
  if (latestTransaction_.soundNoteId != preview) {
    latchFault(RealtimeGameplayFault::InternalConsistency);
    return;
  }
  if (config_.audio.commit == nullptr ||
      !config_.audio.commit(config_.audio.context, reservation, preview)) {
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
  }
}

bool RealtimeGameplayWorker::advanceAutomatic() {
  if (config_.clock.currentSongTime == nullptr) {
    return false;
  }
  const auto songTime =
      config_.clock.currentSongTime(config_.clock.context);
  if (!songTime.has_value()) {
    latchFault(RealtimeGameplayFault::ClockUnavailable);
    return true;
  }
  const auto before = simulation_.snapshot();
  const std::size_t replayCountBefore = simulation_.replayEvents().size();
  const auto terminalBefore = simulation_.terminalReason();
  const auto result = simulation_.advanceTo(*songTime, *songTime);
  if (!result.transactions.empty()) {
    latestTransaction_ = result.transactions.back();
    transactionSequence_ += result.transactions.size();
  }
  const auto after = simulation_.snapshot();
  return !result.transactions.empty() || before.judgeCounts != after.judgeCounts ||
         before.combo != after.combo || before.maxCombo != after.maxCombo ||
         before.comboBreak != after.comboBreak || before.score != after.score ||
         before.gauge != after.gauge || before.gaugeType != after.gaugeType ||
         before.clearTypeRank != after.clearTypeRank ||
         replayCountBefore != simulation_.replayEvents().size() ||
         terminalBefore != simulation_.terminalReason();
}

void RealtimeGameplayWorker::publishSnapshot() {
  const std::size_t latest =
      latestSnapshot_.load(std::memory_order_acquire);
  for (std::size_t offset = 1; offset < snapshots_.size(); ++offset) {
    const std::size_t index = (latest + offset) % snapshots_.size();
    if (snapshots_[index].readers.load(std::memory_order_acquire) != 0) {
      continue;
    }
    auto &snapshot = snapshots_[index].snapshot;
    snapshot.generation = ++snapshotGeneration_;
    snapshot.transactionSequence = transactionSequence_;
    for (NoteId id = 0; id < definition_.noteCount(); ++id) {
      snapshot.noteStates[id] = simulation_.noteState(id);
    }
    for (int lane = 0; lane < static_cast<int>(snapshot.lanePressed.size());
         ++lane) {
      snapshot.lanePressed[lane] = simulation_.lanePressed(lane);
    }
    snapshot.attempt = simulation_.snapshot();
    const auto &scoreState = simulation_.scoreState();
    snapshot.gaugeState = scoreState.gaugeSnapshot();
    snapshot.fastCount = scoreState.fastCount;
    snapshot.slowCount = scoreState.slowCount;
    for (int judgement = 0; judgement < JudgementCount; ++judgement) {
      const auto found = scoreState.judgementFastSlowCount.find(
          static_cast<Judgement>(judgement));
      snapshot.fastSlowCounts[judgement] =
          found == scoreState.judgementFastSlowCount.end()
              ? JudgementFastSlowCount{}
              : found->second;
    }
    snapshot.latestTransaction = latestTransaction_;
    snapshot.replayEventCount = simulation_.replayEvents().size();
    snapshot.terminalReason = simulation_.terminalReason();
    latestSnapshot_.store(index, std::memory_order_release);
    return;
  }
}

void RealtimeGameplayWorker::latchFault(
    RealtimeGameplayFault faultValue) noexcept {
  RealtimeGameplayFault expected = RealtimeGameplayFault::None;
  fault_.compare_exchange_strong(expected, faultValue,
                                 std::memory_order_acq_rel);
}

void RealtimeGameplayWorker::releaseSnapshot(std::size_t index) const noexcept {
  snapshots_[index].readers.fetch_sub(1, std::memory_order_release);
}

} // namespace gameplay
