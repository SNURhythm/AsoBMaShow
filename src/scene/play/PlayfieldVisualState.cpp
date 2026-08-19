#include "PlayfieldVisualState.h"

#include "TouchVisualizationTiming.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr long long kTouchPointReleaseLingerMicros = 180'000;

bool sameTarget(const pacemaker::Target &left,
                const pacemaker::Target &right) {
  return left.enabled == right.enabled && left.label == right.label &&
         left.finalScore == right.finalScore && left.maxScore == right.maxScore &&
         left.totalNotes == right.totalNotes &&
         left.usesReplayProgression == right.usesReplayProgression &&
         left.scoreAfterNotes == right.scoreAfterNotes;
}

bool sameSnapshot(const pacemaker::Snapshot &left,
                  const pacemaker::Snapshot &right) {
  return left.enabled == right.enabled && left.label == right.label &&
         left.currentScore == right.currentScore &&
         left.targetScore == right.targetScore &&
         left.finalTargetScore == right.finalTargetScore &&
         left.maxScore == right.maxScore && left.delta == right.delta &&
         left.playedNotes == right.playedNotes &&
         left.totalNotes == right.totalNotes &&
         left.usesReplayProgression == right.usesReplayProgression;
}

} // namespace

bool PlayfieldAuthorityUpdate::operator==(
    const PlayfieldAuthorityUpdate &other) const {
  return currentBpm == other.currentBpm &&
         currentScrollRate == other.currentScrollRate &&
         currentSpeedMultiplier == other.currentSpeedMultiplier &&
         judgementCounters == other.judgementCounters &&
         judgementFastSlowCounters == other.judgementFastSlowCounters &&
         comboBreak == other.comboBreak && gaugeType == other.gaugeType &&
         maximumCombo == other.maximumCombo &&
         stageCombo == other.stageCombo &&
         stagePassedNotes == other.stagePassedNotes &&
         bestScore == other.bestScore &&
         persistedScore == other.persistedScore &&
         rivalScore == other.rivalScore &&
         playerScoreHistory == other.playerScoreHistory &&
         sameTarget(bestScoreTarget, other.bestScoreTarget) &&
         gaugeAutoShift == other.gaugeAutoShift &&
         gaugeAutoShiftLowerBound == other.gaugeAutoShiftLowerBound &&
         currentGauge == other.currentGauge && gaugeRules == other.gaugeRules &&
         sameTarget(pacemakerTarget, other.pacemakerTarget) &&
         sameSnapshot(pacemakerStatus, other.pacemakerStatus) &&
         player1RandomOption == other.player1RandomOption &&
         player2RandomOption == other.player2RandomOption &&
         doublePlayOption == other.doublePlayOption &&
         targetPlayOption == other.targetPlayOption &&
         songReviewFavorite == other.songReviewFavorite &&
         chartHasDocument == other.chartHasDocument &&
         stageFileAvailable == other.stageFileAvailable &&
         backBmpAvailable == other.backBmpAvailable &&
         playerName == other.playerName &&
         irProviderName == other.irProviderName &&
         playOptionLabel == other.playOptionLabel &&
         tableName == other.tableName && tableLevel == other.tableLevel &&
         tableFullName == other.tableFullName &&
         currentFramesPerSecond == other.currentFramesPerSecond &&
         modeFilterName == other.modeFilterName && sortId == other.sortId &&
         difficultyFilterName == other.difficultyFilterName &&
         chartReplicationMode == other.chartReplicationMode &&
         skinTargetId == other.skinTargetId &&
         skinTargetList == other.skinTargetList &&
         applicationUptimeMillis == other.applicationUptimeMillis &&
         autoPlayMarkVisible == other.autoPlayMarkVisible &&
         gameplayMode == other.gameplayMode &&
         loadingState == other.loadingState &&
         courseMode == other.courseMode &&
         courseStageIndex == other.courseStageIndex &&
         courseStageCount == other.courseStageCount &&
         courseStageTitles == other.courseStageTitles &&
         startLaneIndicators == other.startLaneIndicators &&
         startLaneIndicatorsVisible == other.startLaneIndicatorsVisible &&
         laneCoverPercent == other.laneCoverPercent &&
         laneCoverEnabled == other.laneCoverEnabled &&
         liftEnabled == other.liftEnabled && liftRatio == other.liftRatio &&
         hiddenEnabled == other.hiddenEnabled &&
         hiddenRatio == other.hiddenRatio &&
         failureAnimationActive == other.failureAnimationActive &&
         laneCoverAdjustmentHeld == other.laneCoverAdjustmentHeld &&
         laneCoverChanged == other.laneCoverChanged &&
         laneCoverChangeKind == other.laneCoverChangeKind &&
         resetLaneCoverVisibleTimeReference ==
             other.resetLaneCoverVisibleTimeReference &&
         practiceMenuActive == other.practiceMenuActive;
}

PlayfieldVisualStateStore::PlayfieldVisualStateStore(
    const PlayfieldChartVisualModel &model) {
  resetModel(model);
}

void PlayfieldVisualStateStore::resetModel(
    const PlayfieldChartVisualModel &model) {
  authority_ = {};
  laneOrder_ = model.laneOrder;
  laneIndices_.clear();
  laneIndices_.reserve(laneOrder_.size());
  lanes_.assign(laneOrder_.size(), {});
  for (std::size_t index = 0; index < laneOrder_.size(); ++index) {
    laneIndices_.emplace(laneOrder_[index], index);
  }

  notes_ = std::make_shared<std::vector<NotePresentationState>>();
  notes_->reserve(model.notes.size());
  auto noteIndices =
      std::make_shared<std::unordered_map<ChartVisualId, std::size_t>>();
  noteIndices->reserve(model.notes.size());
  for (const auto &note : model.notes) {
    noteIndices->emplace(note.id, notes_->size());
    notes_->push_back({.id = note.id});
  }
  noteIndices_ = std::move(noteIndices);
  replayTouchSamples_.clear();
  replayTouchCursor_ = 0;
  lastReplayTouchTimeMicros_ = -1;
  replayTouches_ = {};
  liveTouches_ = {};
  touches_.clear();
  bgaMissTracker_.reset();
  lastJudge_ = JudgeResult(None, 0);
  lastJudgeVisualMicros_ = kPlayfieldTimestampOff;
  judgementIndicatorSamples_ = {};
  judgementIndicatorSampleCount_ = 0;
  nextJudgementIndicatorSample_ = 0;
  combo_ = 0;
  score_ = 0;
  fastSlowMicros_ = 0;
  sceneStartMicros_ = kPlayfieldTimestampOff;
  playStartMicros_ = kPlayfieldTimestampOff;
}

void PlayfieldVisualStateStore::setConfiguration(
    const PlayfieldPresentationConfig &configuration) {
  configuration_ = configuration;
}

void PlayfieldVisualStateStore::applyAuthorityUpdate(
    const PlayfieldAuthorityUpdate &update) {
  authority_ = update;
}

void PlayfieldVisualStateStore::setNoteState(NotePresentationState state) {
  if (!noteIndices_) {
    return;
  }
  const auto it = noteIndices_->find(state.id);
  if (it == noteIndices_->end()) {
    return;
  }
  detachNoteSnapshot();
  (*notes_)[it->second] = state;
}

void PlayfieldVisualStateStore::setNoteStates(
    std::vector<NotePresentationState> states) {
  for (auto &state : states) {
    setNoteState(std::move(state));
  }
}

void PlayfieldVisualStateStore::setSceneStartMicros(long long value) noexcept {
  sceneStartMicros_ = value;
}

void PlayfieldVisualStateStore::setPlayStartMicros(long long value) noexcept {
  playStartMicros_ = value;
}

void PlayfieldVisualStateStore::setReplayTouchSamples(
    const std::vector<ReplayTouchSample> &samples) {
  replayTouchSamples_ = samples;
  std::stable_sort(
      replayTouchSamples_.begin(), replayTouchSamples_.end(),
      [](const ReplayTouchSample &left, const ReplayTouchSample &right) {
        return left.songTimeMicros < right.songTimeMicros;
      });
  replayTouchCursor_ = 0;
  lastReplayTouchTimeMicros_ = -1;
  replayTouches_ = {};
  touches_.clear();
}

void PlayfieldVisualStateStore::applyTouchSample(
    TouchLifecycle &lifecycle, const ReplayTouchSample &sample) {
  PresentationTouchPoint point{
      .fingerId = sample.fingerId,
      .action = sample.action,
      .normalizedX = std::clamp(sample.x, 0.0F, 1.0F),
      .normalizedY = std::clamp(sample.y, 0.0F, 1.0F),
      .songTimeMicros = sample.songTimeMicros,
  };

  switch (sample.action) {
  case ReplayTouchAction::Down:
  case ReplayTouchAction::Move:
    lifecycle.active[sample.fingerId] = point;
    break;
  case ReplayTouchAction::Up:
  case ReplayTouchAction::Cancel:
    lifecycle.active.erase(sample.fingerId);
    lifecycle.released.push_back(point);
    break;
  }
}

void PlayfieldVisualStateStore::pruneReleasedTouches(
    TouchLifecycle &lifecycle, long long currentTimeMicros) {
  lifecycle.released.erase(
      std::remove_if(
          lifecycle.released.begin(), lifecycle.released.end(),
          [currentTimeMicros](const PresentationTouchPoint &point) {
            return touch_visualization_timing::shouldPruneReleasedTouch(
                true, point.songTimeMicros, currentTimeMicros,
                kTouchPointReleaseLingerMicros);
          }),
      lifecycle.released.end());
}

void PlayfieldVisualStateStore::advanceReplayTouches(
    long long replayTouchTimeMicros) const {
  if (replayTouchTimeMicros < lastReplayTouchTimeMicros_) {
    replayTouchCursor_ = 0;
    replayTouches_ = {};
  }
  lastReplayTouchTimeMicros_ = replayTouchTimeMicros;

  while (replayTouchCursor_ < replayTouchSamples_.size() &&
         replayTouchSamples_[replayTouchCursor_].songTimeMicros <=
             replayTouchTimeMicros) {
    applyTouchSample(replayTouches_, replayTouchSamples_[replayTouchCursor_]);
    ++replayTouchCursor_;
  }
}

void PlayfieldVisualStateStore::captureTouches(
    long long replayTouchTimeMicros) const {
  advanceReplayTouches(replayTouchTimeMicros);
  pruneReleasedTouches(replayTouches_, replayTouchTimeMicros);
  pruneReleasedTouches(liveTouches_, replayTouchTimeMicros);

  touches_.clear();
  const auto append = [this](const TouchLifecycle &lifecycle) {
    touches_.insert(touches_.end(), lifecycle.released.begin(),
                    lifecycle.released.end());
    for (const auto &[fingerId, point] : lifecycle.active) {
      (void)fingerId;
      touches_.push_back(point);
    }
  };
  append(replayTouches_);
  append(liveTouches_);
  std::sort(touches_.begin(), touches_.end(),
            [](const PresentationTouchPoint &left,
               const PresentationTouchPoint &right) {
              if (left.fingerId != right.fingerId) {
                return left.fingerId < right.fingerId;
              }
              if (left.songTimeMicros != right.songTimeMicros) {
                return left.songTimeMicros < right.songTimeMicros;
              }
              return static_cast<int>(left.action) <
                     static_cast<int>(right.action);
            });
}

void PlayfieldVisualStateStore::setLiveTouchPoint(
    long long fingerId, ReplayTouchAction action, float x, float y,
    long long songTimeMicros) {
  applyTouchSample(liveTouches_, {.action = action,
                                  .fingerId = fingerId,
                                  .songTimeMicros = songTimeMicros,
                                  .x = x,
                                  .y = y});
}

void PlayfieldVisualStateStore::clearLiveTouchPoints() {
  liveTouches_ = {};
  touches_.clear();
}

PlayfieldVisualState
PlayfieldVisualStateStore::capture(PlayfieldFrameClock clock,
                                   bool includeNotes) const {
  captureTouches(clock.replayTouchTimeMicros);
  PlayfieldVisualState result{
      .clock = clock,
      .configuration = configuration_,
      .authority = authority_,
      .lanes = lanes_,
      .notes = includeNotes && notes_ ? *notes_
                                      : std::vector<NotePresentationState>{},
      .touches = touches_,
      .lastJudge = lastJudge_,
      .lastJudgeVisualMicros = lastJudgeVisualMicros_,
      .bgaMiss = bgaMissTracker_.snapshot(),
      .combo = combo_,
      .score = score_,
      .fastSlowMicros = fastSlowMicros_,
      .sceneStartMicros = sceneStartMicros_,
      .playStartMicros = playStartMicros_,
  };

  result.judgementIndicatorSampleCount = judgementIndicatorSampleCount_;
  const std::size_t firstSample =
      judgementIndicatorSampleCount_ ==
              judgement_indicator::kRecentTimingSampleCapacity
          ? nextJudgementIndicatorSample_
          : 0;
  for (std::size_t index = 0; index < judgementIndicatorSampleCount_;
       ++index) {
    result.judgementIndicatorSamples[index] =
        judgementIndicatorSamples_[
            (firstSample + index) %
            judgement_indicator::kRecentTimingSampleCapacity];
  }
  return result;
}

PlayfieldVisualState PlayfieldVisualStateStore::captureForPresentation(
    PlayfieldFrameClock clock) const {
  PlayfieldVisualState result = capture(clock, false);
  result.noteSnapshot = notes_;
  result.noteSnapshotIndices = noteIndices_;
  return result;
}

void PlayfieldVisualStateStore::detachNoteSnapshot() {
  if (!notes_) {
    notes_ = std::make_shared<std::vector<NotePresentationState>>();
    return;
  }
  if (notes_.use_count() != 1) {
    notes_ = std::make_shared<std::vector<NotePresentationState>>(*notes_);
  }
}

void PlayfieldVisualStateStore::onLanePressed(int lane, JudgeResult judge,
                                               long long eventMicros) {
  const auto it = laneIndices_.find(lane);
  if (it == laneIndices_.end()) {
    return;
  }
  auto &state = lanes_[it->second];
  state.pressed = true;
  state.lastPressedJudge = judge;
  // JudgeManager.updateMicro uses `judge == 0 ? 1 : judge * 2 +
  // (mfast > 0 ? 0 : 1)` and does not overwrite the stored lane value for
  // Kpoor.  Aso's Diff is negative for fast, the inverse of upstream mfast.
  if (judge.judgement == PGreat) {
    state.beatorajaJudgeValue = 1;
  } else if (judge.judgement != None && judge.judgement != Kpoor) {
    state.beatorajaJudgeValue = static_cast<int>(judge.judgement) * 2 +
                                (judge.Diff < 0 ? 0 : 1);
  }
  state.pressMicros = eventMicros;
  if (judge.judgement != None) {
    state.bombMicros = eventMicros;
  }
}

void PlayfieldVisualStateStore::onLaneReleased(int lane,
                                                long long eventMicros) {
  const auto it = laneIndices_.find(lane);
  if (it == laneIndices_.end()) {
    return;
  }
  auto &state = lanes_[it->second];
  state.pressed = false;
  state.releaseMicros = eventMicros;
}

void PlayfieldVisualStateStore::onJudge(JudgeResult judge, int combo,
                                        int score,
                                        PlayfieldJudgeEventClock clock,
                                        bool recordTimingSample) {
  bgaMissTracker_.onJudge(judge, combo, clock);
  if (judge.judgement == None) {
    return;
  }
  lastJudge_ = judge;
  lastJudgeVisualMicros_ = clock.visualTimeMicros;
  if (recordTimingSample) {
    judgementIndicatorSamples_[nextJudgementIndicatorSample_] = {
        .judge = judge,
        .visualTimeMicros = clock.visualTimeMicros,
    };
    nextJudgementIndicatorSample_ =
        (nextJudgementIndicatorSample_ + 1) %
        judgement_indicator::kRecentTimingSampleCapacity;
    judgementIndicatorSampleCount_ = std::min(
        judgementIndicatorSampleCount_ + 1,
        judgement_indicator::kRecentTimingSampleCapacity);
  }
  combo_ = combo;
  score_ = score;
  fastSlowMicros_ = static_cast<int>(std::clamp(
      judge.Diff,
      static_cast<long long>(std::numeric_limits<int>::min()),
      static_cast<long long>(std::numeric_limits<int>::max())));
}

PlayfieldPresentationEventFanout::PlayfieldPresentationEventFanout(
    PlayfieldVisualStateStore &state,
    IPlayfieldPresentationEvents &presentation)
    : state_(state), presentation_(&presentation) {}

void PlayfieldPresentationEventFanout::setPresentationSink(
    IPlayfieldPresentationEvents &presentation) noexcept {
  presentation_ = &presentation;
}

void PlayfieldPresentationEventFanout::onLanePressed(int lane,
                                                      JudgeResult judge,
                                                      long long eventMicros) {
  state_.onLanePressed(lane, judge, eventMicros);
  presentation_->onLanePressed(lane, judge, eventMicros);
}

void PlayfieldPresentationEventFanout::onLaneReleased(int lane,
                                                       long long eventMicros) {
  state_.onLaneReleased(lane, eventMicros);
  presentation_->onLaneReleased(lane, eventMicros);
}

void PlayfieldPresentationEventFanout::onJudge(
    JudgeResult judge, int combo, int score, PlayfieldJudgeEventClock clock,
    bool recordTimingSample) {
  state_.onJudge(judge, combo, score, clock, recordTimingSample);
  presentation_->onJudge(judge, combo, score, clock, recordTimingSample);
}
