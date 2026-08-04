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
         judgementCounters == other.judgementCounters &&
         comboBreak == other.comboBreak && gaugeType == other.gaugeType &&
         gaugeAutoShift == other.gaugeAutoShift &&
         currentGauge == other.currentGauge && gaugeRules == other.gaugeRules &&
         sameTarget(pacemakerTarget, other.pacemakerTarget) &&
         sameSnapshot(pacemakerStatus, other.pacemakerStatus) &&
         playOptionLabel == other.playOptionLabel &&
         autoPlayMarkVisible == other.autoPlayMarkVisible &&
         startLaneIndicators == other.startLaneIndicators &&
         startLaneIndicatorsVisible == other.startLaneIndicatorsVisible &&
         laneCoverPercent == other.laneCoverPercent &&
         resetLaneCoverVisibleTimeReference ==
             other.resetLaneCoverVisibleTimeReference;
}

PlayfieldVisualStateStore::PlayfieldVisualStateStore(
    const PlayfieldChartVisualModel &model) {
  resetModel(model);
}

void PlayfieldVisualStateStore::resetModel(
    const PlayfieldChartVisualModel &model) {
  laneOrder_ = model.laneOrder;
  laneIndices_.clear();
  laneIndices_.reserve(laneOrder_.size());
  lanes_.assign(laneOrder_.size(), {});
  for (std::size_t index = 0; index < laneOrder_.size(); ++index) {
    laneIndices_.emplace(laneOrder_[index], index);
  }

  notes_.clear();
  notes_.reserve(model.notes.size());
  noteIndices_.clear();
  noteIndices_.reserve(model.notes.size());
  for (const auto &note : model.notes) {
    noteIndices_.emplace(note.id, notes_.size());
    notes_.push_back({.id = note.id});
  }
  replayTouchSamples_.clear();
  replayTouchCursor_ = 0;
  lastReplayTouchTimeMicros_ = -1;
  replayTouches_ = {};
  liveTouches_ = {};
  touches_.clear();
  bgaMissTracker_.reset();
  lastJudge_ = JudgeResult(None, 0);
  lastJudgeVisualMicros_ = kPlayfieldTimestampOff;
  combo_ = 0;
  score_ = 0;
  fastSlowMicros_ = 0;
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
  const auto it = noteIndices_.find(state.id);
  if (it == noteIndices_.end()) {
    return;
  }
  notes_[it->second] = state;
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
PlayfieldVisualStateStore::capture(PlayfieldFrameClock clock) const {
  captureTouches(clock.replayTouchTimeMicros);
  return {
      .clock = clock,
      .configuration = configuration_,
      .authority = authority_,
      .lanes = lanes_,
      .notes = notes_,
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
}

void PlayfieldVisualStateStore::onLanePressed(int lane, JudgeResult judge,
                                               long long eventMicros) {
  const auto it = laneIndices_.find(lane);
  if (it == laneIndices_.end()) {
    return;
  }
  auto &state = lanes_[it->second];
  state.pressed = true;
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
  (void)recordTimingSample;
  bgaMissTracker_.onJudge(judge, combo, clock);
  if (judge.judgement == None) {
    return;
  }
  lastJudge_ = judge;
  lastJudgeVisualMicros_ = clock.visualTimeMicros;
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
