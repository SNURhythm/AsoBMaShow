#include "RealtimeGameplayWorker.h"

#include "../../bms_parser.hpp"
#include "../../targets.h"

#include <algorithm>
#include <chrono>
#include <utility>

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include <pthread.h>
#elif TARGET_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <avrt.h>
#endif

namespace gameplay {

#if TARGET_OS_WINDOWS
namespace {
class MmcssGameplayScope {
public:
  MmcssGameplayScope() {
    handle_ = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex_);
    if (handle_ != nullptr) {
      (void)AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
    }
  }

  ~MmcssGameplayScope() {
    if (handle_ != nullptr) {
      (void)AvRevertMmThreadCharacteristics(handle_);
    }
  }

private:
  DWORD taskIndex_ = 0;
  HANDLE handle_ = nullptr;
};
} // namespace
#endif

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

bool RealtimeGameplayWorker::suspend() {
  if (!running()) {
    return false;
  }
  if (suspendRequested_.exchange(true, std::memory_order_acq_rel)) {
    return suspended_.load(std::memory_order_acquire);
  }
  signal();
  using namespace std::chrono_literals;
  while (running()) {
    if (suspendAcknowledged_.try_acquire_for(10ms)) {
      return suspended_.load(std::memory_order_acquire);
    }
  }
  return false;
}

bool RealtimeGameplayWorker::resume() {
  if (!started_.load(std::memory_order_acquire)) {
    return false;
  }
  if (!suspendRequested_.exchange(false, std::memory_order_acq_rel)) {
    return true;
  }
  signal();
  using namespace std::chrono_literals;
  while (running()) {
    if (resumeAcknowledged_.try_acquire_for(10ms)) {
      return !suspended_.load(std::memory_order_acquire);
    }
  }
  return false;
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

std::optional<std::vector<replay::InputTransition>>
RealtimeGameplayWorker::copyAcceptedReplayInputAfterStop() const {
  if (running() || !replayCaptureValid_) {
    return std::nullopt;
  }
  return acceptedReplayInput_;
}

std::vector<float>
RealtimeGameplayWorker::copyGaugeHistoryAfterStop() const {
  if (running()) {
    return {};
  }
  return simulation_.scoreState().gaugeHistory;
}

GaugeHistoryCollection
RealtimeGameplayWorker::copyGaugeHistoriesAfterStop() const {
  if (running()) {
    return {};
  }
  return simulation_.scoreState().gaugeHistories;
}

void RealtimeGameplayWorker::run() {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif TARGET_OS_WINDOWS
  const MmcssGameplayScope mmcss;
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
    if (suspendRequested_.load(std::memory_order_acquire)) {
      if (changed || fault() != RealtimeGameplayFault::None) {
        publishSnapshot();
      }
      if (!suspended_.exchange(true, std::memory_order_acq_rel)) {
        suspendAcknowledged_.release();
      }
      while (suspendRequested_.load(std::memory_order_acquire) &&
             !stopRequested_.load(std::memory_order_acquire)) {
        (void)wake_.try_acquire_for(1ms);
        wakePending_.store(false, std::memory_order_release);
      }
      if (suspended_.exchange(false, std::memory_order_acq_rel)) {
        resumeAcknowledged_.release();
      }
      continue;
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
  if (input.replayOnly) {
    recordAcceptedReplayInput(input, *songTime);
    return;
  }
  const bool preparationInput =
      config_.activationSongTimeMicros.has_value() &&
      *songTime < *config_.activationSongTimeMicros;
  const GameplayInputContext context{
      .songTimeMicros = *songTime,
      .laneBeamTimeMicros = input.steadyTimestampMicros,
      .inputDelayMicros = input.inputDelayMicros,
  };

  if (!preparationInput) {
    const auto advanced = simulation_.advanceTo(
        *songTime - input.inputDelayMicros, input.steadyTimestampMicros);
    if (!commitAutomaticTransactions(advanced.transactions)) {
      return;
    }
    if (simulation_.terminal()) {
      return;
    }
  }

  if (input.type == RealtimeGameplayInputType::Release) {
    if (preparationInput) {
      recordTransaction(
          simulation_.releaseLaneForPreparation(input.lane, context));
    } else {
      const auto batch =
          simulation_.releaseLane(input.lane, context, input.backSpin);
      for (const auto &transaction : batch.transactions) {
        recordTransaction(transaction);
      }
    }
    recordAcceptedReplayInput(input, *songTime);
    return;
  }

  const int compensateLane =
      input.compensateLane >= 0 ? input.compensateLane : input.lane;
  const NoteId preview =
      preparationInput
          ? simulation_.previewPreparationPressSoundNote(
                input.lane, compensateLane, context)
          : simulation_.previewPressSoundNote(input.lane, compensateLane,
                                              context);
  const bool requiresSound =
      config_.inputTriggeredKeysounds && preview != kInvalidNoteId &&
      definition_.keysoundSource(preview).wav !=
          bms_parser::Parser::NoWav;
  RealtimeGameplayAudioReservation reservation;
  if (requiresSound &&
      (config_.audio.reserve == nullptr ||
       !config_.audio.reserve(config_.audio.context, preview, reservation))) {
    latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
    return;
  }

  std::size_t previewMatchCount = 0;
  bool unexpectedSound = false;
  if (preparationInput) {
    const auto transaction = simulation_.pressLaneForPreparation(
        input.lane, compensateLane, context);
    previewMatchCount = transaction.soundNoteId == preview ? 1 : 0;
    unexpectedSound = transaction.soundNoteId != kInvalidNoteId &&
                      transaction.soundNoteId != preview;
    recordTransaction(transaction);
  } else {
    const auto batch =
        simulation_.pressLane(input.lane, compensateLane, context);
    for (const auto &transaction : batch.transactions) {
      if (transaction.soundNoteId == preview) {
        ++previewMatchCount;
      } else if (transaction.soundNoteId != kInvalidNoteId) {
        unexpectedSound = true;
      }
      recordTransaction(transaction);
    }
  }
  recordAcceptedReplayInput(input, *songTime);
  if (!requiresSound) {
    return;
  }
  if (unexpectedSound || previewMatchCount != 1) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::InternalConsistency);
    return;
  }
  if (config_.audio.commit == nullptr) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
    return;
  }
  if (!config_.audio.commit(config_.audio.context, reservation, preview)) {
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
  }
}

void RealtimeGameplayWorker::recordAcceptedReplayInput(
    const RealtimeGameplayInput &input,
    std::int64_t songTimeMicros) noexcept {
  if (!input.hasReplayControl || !replayCaptureValid_) {
    return;
  }
  const std::size_t maximum = std::min(
      config_.maximumReplayInputTransitions,
      replay::kReplayLimits.maxInputTransitions);
  if (maximum == 0 || acceptedReplayInput_.size() >= maximum ||
      songTimeMicros < replay::kReplayLimits.minimumSongTimeMicros ||
      (lastReplaySongTimeMicros_.has_value() &&
       songTimeMicros < *lastReplaySongTimeMicros_)) {
    replayCaptureValid_ = false;
    acceptedReplayInput_.clear();
    return;
  }
  try {
    acceptedReplayInput_.push_back(
        {.songTimeMicros = songTimeMicros,
         .control = input.replayControl,
         .pressed = input.type == RealtimeGameplayInputType::Press,
         .replayOnly = input.replayOnly});
    lastReplaySongTimeMicros_ = songTimeMicros;
  } catch (...) {
    replayCaptureValid_ = false;
    acceptedReplayInput_.clear();
  }
}

bool RealtimeGameplayWorker::commitAutomaticTransactions(
    std::span<const GameplayInputResult> transactions) {
  for (const auto &transaction : transactions) {
    const bool requiresSound =
        config_.inputTriggeredKeysounds &&
        transaction.soundNoteId != kInvalidNoteId;
    RealtimeGameplayAudioReservation reservation;
    if (requiresSound &&
        (config_.audio.reserve == nullptr ||
         !config_.audio.reserve(config_.audio.context,
                                transaction.soundNoteId, reservation))) {
      latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
      return false;
    }
    recordTransaction(transaction);
    if (!requiresSound) {
      continue;
    }
    if (config_.audio.commit == nullptr) {
      if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
        config_.audio.cancel(config_.audio.context, reservation,
                             transaction.soundNoteId);
      }
      latchFault(RealtimeGameplayFault::AudioCommitFailed);
      return false;
    }
    if (!config_.audio.commit(config_.audio.context, reservation,
                              transaction.soundNoteId)) {
      latchFault(RealtimeGameplayFault::AudioCommitFailed);
      return false;
    }
  }
  return true;
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
  if (config_.activationSongTimeMicros.has_value() &&
      *songTime < *config_.activationSongTimeMicros) {
    return false;
  }
  const auto before = simulation_.snapshot();
  const std::size_t replayCountBefore = simulation_.replayEvents().size();
  const auto terminalBefore = simulation_.terminalReason();
  const auto practiceEnd = config_.practiceCompletionSongTimeMicros;
  const std::int64_t advanceTime =
      practiceEnd.has_value() && *songTime >= *practiceEnd
          ? *practiceEnd - 1
          : *songTime;
  const auto result = simulation_.advanceTo(advanceTime, advanceTime);
  if (!commitAutomaticTransactions(result.transactions)) {
    return true;
  }
  if (practiceEnd.has_value() && *songTime >= *practiceEnd &&
      !simulation_.terminal()) {
    const auto finalized = simulation_.finalizePracticeRange(
        *practiceEnd - 1, *practiceEnd - 1);
    if (!commitAutomaticTransactions(finalized.transactions)) {
      return true;
    }
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

void RealtimeGameplayWorker::recordTransaction(
    const GameplayInputResult &result) noexcept {
  latestTransaction_ = result;
  ++transactionSequence_;
  transactionHistory_[(transactionSequence_ - 1) %
                      transactionHistory_.size()] = {
      .sequence = transactionSequence_, .result = result};
  transactionHistoryCount_ =
      std::min(transactionHistoryCount_ + 1, transactionHistory_.size());
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
    snapshot.longNoteHoldingByLane.fill(false);
    for (NoteId id = 0; id < definition_.noteCount(); ++id) {
      const auto &state = simulation_.noteState(id);
      snapshot.noteStates[id] = state;
      const auto &note = definition_.note(id);
      if (state.holding &&
          (note.kind == NoteKind::LongHead ||
           note.kind == NoteKind::LongTail) &&
          note.lane >= 0 &&
          static_cast<std::size_t>(note.lane) <
              snapshot.longNoteHoldingByLane.size()) {
        snapshot.longNoteHoldingByLane[static_cast<std::size_t>(note.lane)] =
            true;
      }
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
    snapshot.transactionCount = transactionHistoryCount_;
    if (transactionHistoryCount_ != 0) {
      const std::uint64_t firstSequence =
          transactionSequence_ - transactionHistoryCount_ + 1;
      for (std::size_t offset = 0; offset < transactionHistoryCount_;
           ++offset) {
        const std::uint64_t sequence = firstSequence + offset;
        snapshot.transactions[offset] =
            transactionHistory_[(sequence - 1) % transactionHistory_.size()];
      }
    }
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
