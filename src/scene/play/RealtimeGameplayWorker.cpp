#include "RealtimeGameplayWorker.h"
#include "RealtimeGameplayWake.h"

#include "../../bms_parser.hpp"
#include "../../ChartTiming.h"
#include "../../targets.h"

#include <algorithm>
#include <chrono>
#include <tuple>
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
  std::size_t laneStorageSize = 0;
  for (const auto &lane : definition_.lanes()) {
    if (lane.lane >= 0) {
      laneStorageSize =
          std::max(laneStorageSize, static_cast<std::size_t>(lane.lane) + 1);
    }
  }
  ownedInputLanes_.resize(laneStorageSize);
  queuedInputs_.reserve(kRealtimeGameplayIngressSize);
  gameplayInputWork_.reserve(kRealtimeGameplayIngressSize * 2);
  replayInputWork_.reserve(kRealtimeGameplayIngressSize * 2);
  inputAdmissions_.reserve(kRealtimeGameplayIngressSize);
  sampledInputStates_.reserve(laneStorageSize);
  latestInputOwners_.resize(laneStorageSize * 3);
  scratchInputStates_.resize(laneStorageSize);
  for (auto &buffer : snapshots_) {
    buffer.snapshot.noteStates.resize(definition_.noteCount());
    buffer.snapshot.noteChanges.revision = 0;
    buffer.holdingNoteCountsByLane.resize(laneStorageSize);
    buffer.snapshot.lanePressed.resize(laneStorageSize);
    buffer.snapshot.longNoteHoldingByLane.resize(laneStorageSize);
  }
  // Prime every buffer before input admission; the first rotation must not
  // pay a chart-sized initialization cost on the gameplay thread.
  for (std::size_t index = 0; index < snapshots_.size(); ++index) {
    publishSnapshot();
  }
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
  try {
    thread_ = std::thread([this] { run(); });
  } catch (...) {
    started_.store(false, std::memory_order_release);
    throw;
  }
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

bool RealtimeGameplayWorker::requestSuspend() noexcept {
  if (!running()) {
    return false;
  }
  if (!suspendRequested_.load(std::memory_order_acquire)) {
    // An idempotent waiter can observe suspended_ just before its wake token
    // is published. Retire that completed cycle before requesting another.
    (void)suspendAcknowledged_.try_acquire();
  }
  if (!suspendRequested_.exchange(true, std::memory_order_acq_rel)) signal();
  return true;
}

bool RealtimeGameplayWorker::suspend() {
  if (!requestSuspend()) return false;
  using namespace std::chrono_literals;
  while (running()) {
    if (suspended_.load(std::memory_order_acquire)) {
      (void)suspendAcknowledged_.try_acquire();
      return true;
    }
    (void)suspendAcknowledged_.try_acquire_for(10ms);
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
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  auto capturedInput = input;
  capturedInput.ingressTimestampMicros = perf::latency::nowMicros();
  const auto &queuedInput = capturedInput;
#else
  const auto &queuedInput = input;
#endif
  if (!ingress_.tryPush(queuedInput)) {
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
    detail::waitForGameplayWake(wake_, wakePending_, 1ms);

    bool changed = processQueuedInputs();
    if (suspendRequested_.load(std::memory_order_acquire)) {
      if (changed || fault() != RealtimeGameplayFault::None) {
        publishSnapshot();
      }
      if (!suspended_.exchange(true, std::memory_order_acq_rel)) {
        suspendAcknowledged_.release();
      }
      while (suspendRequested_.load(std::memory_order_acquire) &&
             !stopRequested_.load(std::memory_order_acquire)) {
        detail::waitForGameplayWake(wake_, wakePending_, 1ms);
      }
      if (suspended_.exchange(false, std::memory_order_acq_rel)) {
        resumeAcknowledged_.release();
      }
      continue;
    }
    if (fault() == RealtimeGameplayFault::None) {
      changed = advanceAutomatic() || changed;
    }
    if (changed || snapshotPending_ || fault() != RealtimeGameplayFault::None) {
      publishSnapshot();
    }

    if (fault() != RealtimeGameplayFault::None) {
      stopRequested_.store(true, std::memory_order_release);
      break;
    }

    while (!suspendRequested_.load(std::memory_order_acquire) &&
           processQueuedInputs()) {
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

bool RealtimeGameplayWorker::processQueuedInputs() {
  if (config_.inputMaintenance.run != nullptr &&
      !suspendRequested_.load(std::memory_order_acquire) &&
      !stopRequested_.load(std::memory_order_acquire)) {
    const auto steadyMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    config_.inputMaintenance.run(config_.inputMaintenance.context, steadyMicros);
  }
  queuedInputs_.clear();
  RealtimeGameplayInput input;
  while (queuedInputs_.size() < kRealtimeGameplayIngressSize && ingress_.tryPop(input)) {
    queuedInputs_.push_back({input, std::nullopt});
  }
  if (queuedInputs_.empty()) return false;
  if (config_.simulation.judge.rules().ruleset != GameplayRuleset::LR2) {
    for (const auto &queued : queuedInputs_) {
      processInput(queued.input);
      if (fault() != RealtimeGameplayFault::None) break;
    }
    return true;
  }
  for (auto &queued : queuedInputs_) {
    if (queued.input.epoch == config_.epoch && config_.clock.mapSteadyToSong != nullptr) {
      queued.songTimeMicros = config_.clock.mapSteadyToSong(
          config_.clock.context, queued.input.steadyTimestampMicros);
    }
  }
  for (std::size_t first = 0; first < queuedInputs_.size();) {
    const auto &queued = queuedInputs_[first];
    if (queued.input.epoch != config_.epoch || config_.clock.mapSteadyToSong == nullptr) {
      ++first;
      continue;
    }
    if (!queued.songTimeMicros.has_value()) {
      latchFault(RealtimeGameplayFault::ClockUnavailable);
      break;
    }
    const auto effectiveTime = chart_timing::subtract(*queued.songTimeMicros,
                                                      queued.input.inputDelayMicros);
    const bool preparation = config_.activationSongTimeMicros.has_value() &&
                             *queued.songTimeMicros < *config_.activationSongTimeMicros;
    std::size_t end = first + 1;
    while (end < queuedInputs_.size()) {
      const auto &next = queuedInputs_[end];
      if (next.input.epoch != config_.epoch || !next.songTimeMicros.has_value() ||
          chart_timing::subtract(*next.songTimeMicros, next.input.inputDelayMicros) != effectiveTime ||
          (config_.activationSongTimeMicros.has_value() &&
           *next.songTimeMicros < *config_.activationSongTimeMicros) != preparation) break;
      ++end;
    }
    processInputBatch(std::span<const QueuedInput>(queuedInputs_).subspan(first, end - first));
    if (fault() != RealtimeGameplayFault::None) break;
    first = end;
  }
  return true;
}

void RealtimeGameplayWorker::processInputBatch(std::span<const QueuedInput> inputs) {
  gameplayInputWork_.clear();
  replayInputWork_.clear();
  inputAdmissions_.clear();
  sampledInputStates_.clear();
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const auto &queued = inputs[index];
    observeInputLatency(queued.input);
    const auto &input = queued.input;
    OwnedInputDecision decision;
    const bool command = input.hasReplayControl &&
        (input.replayControl.kind == replay::LogicalControlKind::Start ||
         input.replayControl.kind == replay::LogicalControlKind::Select);
    if (command || input.source == RealtimeGameplayInputSource::Independent) {
      if (!command && !input.replayOnly) {
        decision.gameplay[decision.gameplayCount++] = input;
      }
      if (input.hasReplayControl) {
        decision.replay[decision.replayCount++] = input;
        if (command) {
          auto &replay = decision.replay[0];
          replay.source = RealtimeGameplayInputSource::Independent;
          replay.lane = -1;
          replay.compensateLane = -1;
          replay.replayOnly = false;
        }
      }
    } else {
      decision = coalesceOwnedInput(input);
    }
    inputAdmissions_.push_back({decision.gameplayCount, true});
    for (std::size_t edge = 0; edge < decision.gameplayCount; ++edge) {
      if (input.hasReplayControl && replay::isDirectionalScratchControl(input.replayControl.kind)) {
        --inputAdmissions_.back().remaining;
        continue;
      }
      gameplayInputWork_.push_back({decision.gameplay[edge], *queued.songTimeMicros,
                                   index, gameplayInputWork_.size()});
      const auto &effective = decision.gameplay[edge];
      const bool pressed = effective.type == RealtimeGameplayInputType::Press || effective.backSpin;
      const auto found = std::ranges::find(sampledInputStates_, effective.lane,
                                           &GameplayLaneInputState::lane);
      if (found == sampledInputStates_.end()) {
        sampledInputStates_.push_back({effective.lane, pressed});
      } else {
        found->pressed = pressed;
      }
    }
    for (std::size_t edge = 0; edge < decision.replayCount; ++edge) {
      replayInputWork_.push_back({decision.replay[edge], *queued.songTimeMicros, index, 0});
      const auto &raw = decision.replay[edge];
      if (!raw.hasReplayControl || !replay::isDirectionalScratchControl(raw.replayControl.kind)) continue;
      const auto lane = replay::physicalChartLaneForLogicalControl(
          definition_.metadata().keyMode, raw.replayControl);
      if (!lane || static_cast<std::size_t>(*lane) >= scratchInputStates_.size()) continue;
      auto effective = raw;
      effective.lane = *lane;
      effective.replayOnly = false;
      effective.backSpin = false;
      gameplayInputWork_.push_back({effective, *queued.songTimeMicros, index,
                                   gameplayInputWork_.size()});
      ++inputAdmissions_.back().remaining;
      auto &keys = scratchInputStates_[*lane];
      keys[raw.replayControl.kind == replay::LogicalControlKind::ScratchClockwise ? 0 : 1] =
          raw.type == RealtimeGameplayInputType::Press;
      const auto found = std::ranges::find(sampledInputStates_, *lane,
                                           &GameplayLaneInputState::lane);
      if (found == sampledInputStates_.end()) sampledInputStates_.push_back({*lane, keys[0] || keys[1]});
      else found->pressed = keys[0] || keys[1];
    }
  }
  const auto &first = inputs.front();
  const GameplayInputContext context{.songTimeMicros = *first.songTimeMicros,
      .laneBeamTimeMicros = first.input.steadyTimestampMicros,
      .inputDelayMicros = first.input.inputDelayMicros};
  const bool preparation = config_.activationSongTimeMicros.has_value() &&
                           context.songTimeMicros < *config_.activationSongTimeMicros;
  // JudgeManager samples the latest changed state once per physical key. Keep
  // raw replay edges, but discard earlier gameplay edges for that same key.
  // Directionless scratch inputs cannot identify which of its two keys changed.
  const auto layout = replay::replayKeyModeLayout(definition_.metadata().keyMode);
  const auto keyIndex = [&](const GameplayInputWork &work) -> std::optional<std::size_t> {
    const int lane = work.input.lane;
    if (lane < 0 || static_cast<std::size_t>(lane) >= ownedInputLanes_.size()) {
      return std::nullopt;
    }
    std::size_t direction = 0;
    if (layout && layout->hasDirectionalScratch && (lane == 7 || lane == 15)) {
      const auto &raw = work.input.hasReplayControl ? work.input : inputs[work.owner].input;
      if (!raw.hasReplayControl || !replay::isDirectionalScratchControl(raw.replayControl.kind)) {
        return std::nullopt;
      }
      direction = raw.replayControl.kind == replay::LogicalControlKind::ScratchClockwise ? 1 : 2;
    }
    return static_cast<std::size_t>(lane) * 3 + direction;
  };
  for (const auto &work : gameplayInputWork_) {
    if (const auto key = keyIndex(work)) latestInputOwners_[*key] = work.owner;
  }
  std::erase_if(gameplayInputWork_, [&](const auto &work) {
    const auto key = keyIndex(work);
    if (!key || latestInputOwners_[*key] == work.owner) return false;
    --inputAdmissions_[work.owner].remaining;
    return true;
  });
  bool accepted = true;
  if (!gameplayInputWork_.empty()) {
    std::ranges::sort(gameplayInputWork_, {}, [](const auto &work) {
      const int key = work.input.hasReplayControl &&
              replay::isDirectionalScratchControl(work.input.replayControl.kind)
          ? (work.input.replayControl.kind == replay::LogicalControlKind::ScratchClockwise ? 0 : 1)
          : 0;
      return std::tuple{work.input.lane, key, work.sequence};
    });
    if (!preparation) {
      accepted = commitAutomaticTransactions(
          simulation_.beginInputUpdate(sampledInputStates_, context).transactions) &&
          !simulation_.terminal();
    }
    for (const auto &work : gameplayInputWork_) {
      if (!accepted || fault() != RealtimeGameplayFault::None) break;
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
      processingStartedMicros_ = perf::latency::nowMicros();
#endif
      const bool edgeAccepted = processGameplayInput(work.input, work.songTimeMicros, true);
      auto &admission = inputAdmissions_[work.owner];
      --admission.remaining;
      admission.accepted = admission.accepted && edgeAccepted;
      accepted = edgeAccepted;
    }
    if (!preparation && fault() == RealtimeGameplayFault::None) {
      (void)commitAutomaticTransactions(simulation_.finishInputUpdate(context).transactions);
    }
  }
  for (const auto &work : replayInputWork_) {
    const auto &admission = inputAdmissions_[work.owner];
    if (admission.accepted && admission.remaining == 0) {
      recordAcceptedReplayInput(work.input, work.songTimeMicros);
    }
  }
}

void RealtimeGameplayWorker::observeInputLatency(
    const RealtimeGameplayInput &input) noexcept {
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  processingStartedMicros_ = perf::latency::nowMicros();
  if (input.ingressTimestampMicros > 0 &&
      processingStartedMicros_ >= input.ingressTimestampMicros) {
    perf::latency::record(perf::latency::Stage::IngressToWorker,
                         processingStartedMicros_ - input.ingressTimestampMicros);
  }
#else
  (void)input;
#endif
}

void RealtimeGameplayWorker::processInput(
    const RealtimeGameplayInput &input) {
  observeInputLatency(input);
  if (input.epoch != config_.epoch || config_.clock.mapSteadyToSong == nullptr) {
    return;
  }
  const auto songTime = config_.clock.mapSteadyToSong(
      config_.clock.context, input.steadyTimestampMicros);
  if (!songTime.has_value()) {
    latchFault(RealtimeGameplayFault::ClockUnavailable);
    return;
  }

  if (input.hasReplayControl &&
      (input.replayControl.kind == replay::LogicalControlKind::Start ||
       input.replayControl.kind == replay::LogicalControlKind::Select)) {
    // Start and Select are stock BRD commands without a physical chart lane.
    // They bypass lane ownership; replayOnly is reserved for scratch handoffs.
    auto command = input;
    command.source = RealtimeGameplayInputSource::Independent;
    command.lane = -1;
    command.compensateLane = -1;
    command.replayOnly = false;
    recordAcceptedReplayInput(command, *songTime);
    return;
  }

  if (input.source == RealtimeGameplayInputSource::Independent) {
    if (input.replayOnly) {
      recordAcceptedReplayInput(input, *songTime);
    } else if (processGameplayInput(input, *songTime)) {
      recordAcceptedReplayInput(input, *songTime);
    }
    return;
  }

  const auto decision = coalesceOwnedInput(input);
  bool accepted = true;
  for (std::size_t index = 0; index < decision.gameplayCount; ++index) {
    accepted = processGameplayInput(decision.gameplay[index], *songTime) &&
               accepted;
    if (fault() != RealtimeGameplayFault::None) {
      break;
    }
  }
  if (!accepted) {
    return;
  }
  for (std::size_t index = 0; index < decision.replayCount; ++index) {
    recordAcceptedReplayInput(decision.replay[index], *songTime);
  }
}

RealtimeGameplayWorker::OwnedInputDecision
RealtimeGameplayWorker::coalesceOwnedInput(
    const RealtimeGameplayInput &input) noexcept {
  OwnedInputDecision decision;
  const auto sourceIndex = [&]() -> std::optional<std::size_t> {
    switch (input.source) {
    case RealtimeGameplayInputSource::Physical:
      return 0;
    case RealtimeGameplayInputSource::Touch:
      return 1;
    case RealtimeGameplayInputSource::LegacyAdapter:
      return 2;
    case RealtimeGameplayInputSource::Independent:
      return std::nullopt;
    }
    return std::nullopt;
  }();
  if (!sourceIndex.has_value()) {
    return decision;
  }

  int lane = input.lane;
  if ((lane < 0 || static_cast<std::size_t>(lane) >= ownedInputLanes_.size()) &&
      input.hasReplayControl) {
    const auto replayLane = replay::physicalChartLaneForLogicalControl(
        definition_.metadata().keyMode, input.replayControl);
    lane = replayLane.value_or(-1);
  }
  if (lane < 0 || static_cast<std::size_t>(lane) >= ownedInputLanes_.size()) {
    if (input.hasReplayControl) {
      decision.replay[0] = input;
      decision.replay[0].replayOnly = true;
      decision.replayCount = 1;
    }
    return decision;
  }

  auto &laneState = ownedInputLanes_[static_cast<std::size_t>(lane)];
  const auto anyHeld = [&]() {
    return std::ranges::any_of(laneState.sources,
                               [](const auto &source) { return source.held; });
  };
  const auto activeReplayControl = [&]()
      -> std::optional<replay::LogicalControl> {
    const OwnedInputSourceState *active = nullptr;
    for (const auto &source : laneState.sources) {
      if (!source.held || !source.hasReplayControl ||
          (active != nullptr &&
           source.claimSequence <= active->claimSequence)) {
        continue;
      }
      active = &source;
    }
    return active == nullptr
               ? std::nullopt
               : std::optional<replay::LogicalControl>(active->replayControl);
  };

  const bool wasHeld = anyHeld();
  const auto previousReplayControl = activeReplayControl();
  auto &source = laneState.sources[*sourceIndex];
  if (!input.replayOnly) {
    source.held = input.type == RealtimeGameplayInputType::Press;
    if (source.held) {
      source.claimSequence = ++ownedInputClaimSequence_;
    }
  }
  if (input.hasReplayControl) {
    if (input.type == RealtimeGameplayInputType::Press) {
      source.hasReplayControl = true;
      source.replayControl = input.replayControl;
      source.claimSequence = ++ownedInputClaimSequence_;
    } else if (source.hasReplayControl &&
               source.replayControl == input.replayControl) {
      source.hasReplayControl = false;
    }
  }

  const bool held = anyHeld();
  const auto replayControl = activeReplayControl();
  if (wasHeld != held) {
    auto &effective = decision.gameplay[decision.gameplayCount++];
    effective = input;
    effective.source = RealtimeGameplayInputSource::Independent;
    effective.type = held ? RealtimeGameplayInputType::Press
                          : RealtimeGameplayInputType::Release;
    effective.lane = lane;
    effective.compensateLane =
        input.compensateLane >= 0 ? input.compensateLane : lane;
    effective.hasReplayControl = false;
    effective.replayOnly = false;
  }

  if (previousReplayControl == replayControl) {
    return decision;
  }
  const bool effectiveRelease =
      decision.gameplayCount > 0 &&
      decision.gameplay[0].type == RealtimeGameplayInputType::Release;
  const bool effectivePress =
      decision.gameplayCount > 0 &&
      decision.gameplay[0].type == RealtimeGameplayInputType::Press;
  if (previousReplayControl.has_value()) {
    auto &released = decision.replay[decision.replayCount++];
    released = input;
    released.source = RealtimeGameplayInputSource::Independent;
    released.type = RealtimeGameplayInputType::Release;
    released.lane = lane;
    released.hasReplayControl = true;
    released.replayControl = *previousReplayControl;
    released.replayOnly = !effectiveRelease;
  }
  if (replayControl.has_value()) {
    auto &pressed = decision.replay[decision.replayCount++];
    pressed = input;
    pressed.source = RealtimeGameplayInputSource::Independent;
    pressed.type = RealtimeGameplayInputType::Press;
    pressed.lane = lane;
    pressed.hasReplayControl = true;
    pressed.replayControl = *replayControl;
    pressed.replayOnly = !effectivePress;
  }
  return decision;
}

bool RealtimeGameplayWorker::processGameplayInput(
    const RealtimeGameplayInput &input, std::int64_t songTimeMicros,
    bool sharedUpdate) {
  const bool preparationInput =
      config_.activationSongTimeMicros.has_value() &&
      songTimeMicros < *config_.activationSongTimeMicros;
  const GameplayInputContext context{
      .songTimeMicros = songTimeMicros,
      .laneBeamTimeMicros = input.steadyTimestampMicros,
      .inputDelayMicros = input.inputDelayMicros,
  };
  const bool scratchKey = sharedUpdate && input.hasReplayControl &&
      replay::isDirectionalScratchControl(input.replayControl.kind);
  const bool clockwise = input.replayControl.kind == replay::LogicalControlKind::ScratchClockwise;

  if (!preparationInput && !sharedUpdate) {
    const auto advanced = simulation_.beginInputUpdate(
        input.lane, input.type == RealtimeGameplayInputType::Press || input.backSpin, context);
    if (!commitAutomaticTransactions(advanced.transactions)) {
      return false;
    }
    if (simulation_.terminal()) {
      return false;
    }
  }

  if (input.type == RealtimeGameplayInputType::Release) {
    if (preparationInput) {
      recordTransaction(
          simulation_.releaseLaneForPreparation(input.lane, context));
    } else {
      const auto batch =
          scratchKey ? simulation_.releaseScratchKey(input.lane, clockwise, context)
                     : simulation_.releaseLane(input.lane, context, input.backSpin);
      for (const auto &transaction : batch.transactions) {
        recordTransaction(transaction);
      }
      if (!sharedUpdate && !commitAutomaticTransactions(
              simulation_.finishInputUpdate(context).transactions)) {
        return false;
      }
    }
    return true;
  }

  const int compensateLane =
      input.compensateLane >= 0 ? input.compensateLane : input.lane;
  const NoteId preview =
      preparationInput
          ? simulation_.previewPreparationPressSoundNote(
                input.lane, compensateLane, context)
          : (scratchKey ? simulation_.previewScratchPressSoundNote(input.lane, context)
                        : simulation_.previewPressSoundNote(input.lane, compensateLane, context));
  const bool requiresSound =
      config_.inputTriggeredKeysounds && preview != kInvalidNoteId &&
      definition_.keysoundSource(preview).wav !=
          bms_parser::Parser::NoWav;
  RealtimeGameplayAudioReservation reservation;
  if (requiresSound &&
      (config_.audio.reserve == nullptr ||
       !config_.audio.reserve(config_.audio.context, preview, reservation))) {
    latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
    return false;
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
        scratchKey ? simulation_.pressScratchKey(input.lane, clockwise, context)
                   : simulation_.pressLane(input.lane, compensateLane, context);
    for (const auto &transaction : batch.transactions) {
      if (transaction.soundNoteId == preview) {
        ++previewMatchCount;
      } else if (transaction.soundNoteId != kInvalidNoteId) {
        unexpectedSound = true;
      }
      recordTransaction(transaction);
    }
    if (!sharedUpdate && !commitAutomaticTransactions(
            simulation_.finishInputUpdate(context).transactions)) {
      return false;
    }
  }
  if (!requiresSound) {
    return true;
  }
  if (unexpectedSound || previewMatchCount != 1) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::InternalConsistency);
    return true;
  }
  if (config_.audio.commit == nullptr) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
    return true;
  }
  if (!config_.audio.commit(config_.audio.context, reservation, preview)) {
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
  }
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  else {
    const auto committedMicros = perf::latency::nowMicros();
    if (committedMicros >= processingStartedMicros_) {
      perf::latency::record(perf::latency::Stage::WorkerToSoundCommit,
                           committedMicros - processingStartedMicros_);
    }
  }
#endif
  return true;
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
      songTimeMicros < replay::kReplayLimits.minimumSongTimeMicros) {
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
  if (config_.clock.currentSongTime == nullptr ||
      suspendRequested_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto songTime =
      config_.clock.currentSongTime(config_.clock.context);
  if (suspendRequested_.load(std::memory_order_acquire)) return false;
  if (!songTime.has_value()) {
    latchFault(RealtimeGameplayFault::ClockUnavailable);
    return true;
  }
  if (config_.activationSongTimeMicros.has_value() &&
      *songTime < *config_.activationSongTimeMicros) {
    return false;
  }
  const auto before = simulation_.snapshot();
  const std::size_t gaugeSamplesBefore =
      simulation_.skinGameplayGraphState().gaugeHistories.front().size();
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
  const std::size_t gaugeSamplesAfter =
      simulation_.skinGameplayGraphState().gaugeHistories.front().size();
  return simulation_.lastAdvanceStats().notesExamined != 0 ||
         !result.transactions.empty() || before.judgeCounts != after.judgeCounts ||
         before.combo != after.combo || before.maxCombo != after.maxCombo ||
         before.comboBreak != after.comboBreak || before.score != after.score ||
         before.gauge != after.gauge || before.gaugeType != after.gaugeType ||
         before.clearTypeRank != after.clearTypeRank ||
         gaugeSamplesBefore != gaugeSamplesAfter ||
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
  snapshotPending_ = true;
  const std::size_t latest =
      latestSnapshot_.load(std::memory_order_acquire);
  for (std::size_t offset = 1; offset < snapshots_.size(); ++offset) {
    const std::size_t index = (latest + offset) % snapshots_.size();
    if (snapshots_[index].readers.load(std::memory_order_acquire) != 0) {
      continue;
    }
    auto &snapshot = snapshots_[index].snapshot;
    const bool firstPublication = snapshot.generation == 0;
    snapshot.generation = ++snapshotGeneration_;
    snapshot.transactionSequence = transactionSequence_;
    auto &holdingCounts = snapshots_[index].holdingNoteCountsByLane;
    simulation_.noteChanges().forEachSince(
        snapshot.noteChanges.revision, definition_.noteCount(), [&](NoteId id) {
      const auto &state = simulation_.noteState(id);
      const auto &note = definition_.note(id);
      if ((note.kind == NoteKind::LongHead || note.kind == NoteKind::LongTail) &&
          note.lane >= 0 &&
          static_cast<std::size_t>(note.lane) < holdingCounts.size()) {
        auto &count = holdingCounts[static_cast<std::size_t>(note.lane)];
        if (snapshot.noteStates[id].holding) {
          --count;
        }
        if (state.holding) {
          ++count;
        }
      }
      snapshot.noteStates[id] = state;
    });
    snapshot.noteChanges.catchUpTo(simulation_.noteChanges());
    for (int lane = 0; lane < static_cast<int>(snapshot.lanePressed.size());
         ++lane) {
      snapshot.lanePressed[lane] = simulation_.lanePressed(lane);
      snapshot.longNoteHoldingByLane[lane] = holdingCounts[lane] != 0;
    }
    snapshot.attempt = simulation_.snapshot();
    const auto &graph = simulation_.skinGameplayGraphState();
    if (firstPublication ||
        snapshot.skinGameplayGraph.judgementRevision != graph.judgementRevision ||
        snapshot.skinGameplayGraph.gaugeRevision != graph.gaugeRevision) {
      snapshot.skinGameplayGraph = graph;
    }
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
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
    snapshot.publishedSteadyMicros = perf::latency::nowMicros();
#endif
    latestSnapshot_.store(index, std::memory_order_release);
    snapshotPending_ = false;
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
