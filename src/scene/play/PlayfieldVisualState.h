#pragma once

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../audio/GameplayBgaMissStateTracker.h"
#include "GameplayGaugeRules.h"
#include "Pacemaker.h"
#include "PlayfieldChartVisualModel.h"
#include "PlayfieldPresentationEvents.h"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct PlayfieldPlayTimerAuthority {
  bool active = false;
  long long startMicros = 0;
  // True only when gameplayTimeMicros - startMicros reproduces the pinned
  // TimerManager elapsed-millisecond domain. Practice's frequency-adjusted
  // lead-in has no equivalent Aso clock authority yet.
  bool elapsedMillisExact = false;
  // Exact BMSPlayer.getPlaytime()-equivalent value when the session can
  // capture a paired elapsed clock. No static chart duration is a substitute:
  // manual, autoplay, and practice have distinct upstream formulas.
  std::optional<std::int32_t> playtimeMillis;

  bool operator==(const PlayfieldPlayTimerAuthority &) const = default;
};

struct PlayfieldFrameClock {
  std::uint64_t serial = 0;
  long long visualTimeMicros = 0;
  long long gameplayTimeMicros = 0;
  long long replayTouchTimeMicros = 0;
  long long bgaTimeMicros = 0;
  PlayfieldPlayTimerAuthority playTimer;

  bool operator==(const PlayfieldFrameClock &) const = default;
};

struct PlayfieldPresentationConfig {
  int visibleTimeGreenNumber = 0;
  float hispeedMultiplier = 1.0F;
  bool visibleTimeUseMilliseconds = false;
  AppSettings::VisibleTimeBpmStrategy visibleTimeBpmStrategy =
      AppSettings::VisibleTimeBpmStrategy::Chart;
  float playAreaWidth = 0.0F;
  bool laneBeamsEnabled = true;
  bool laneCoverFloatingEnabled = true;
  int laneBeamLengthPercent = 100;
  int noteStartPositionPercent = 0;
  bool laneBeamClockUsesRenderTime = false;
  bool showInvisibleNotes = false;
  bool judgementIndicatorEnabled = true;
  float judgementIndicatorY = 0.0F;
  float judgementIndicatorWidthScale = 1.0F;
  bool judgementIndicatorHudMode = false;
  int judgementIndicatorRangeMilliseconds = 0;
  float judgementTextY = 0.0F;
  bool judgementCounterEnabled = false;
  AppSettings::JudgementCounterPosition judgementCounterPosition =
      AppSettings::JudgementCounterPosition::Right;
  AppSettings::JudgementTimingDisplayCriteria fastSlowCriteria =
      AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  AppSettings::JudgementTimingDisplayCriteria millisecondsCriteria =
      AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  AppSettings::GaugeBarPosition gaugeBarPosition =
      AppSettings::GaugeBarPosition::World;
  bool touchVisualizationEnabled = true;
  bool replayGhostRenderingEnabled = true;

  bool operator==(const PlayfieldPresentationConfig &) const = default;
};

// Immutable per-judgement timing counters carried from GameplayScoreState to
// presentation consumers.  This intentionally mirrors only the read model
// needed by skins; it must not expose score mutation through the frame view.
struct PlayfieldJudgementFastSlowCount {
  int fast = 0;
  int slow = 0;

  bool operator==(const PlayfieldJudgementFastSlowCount &) const = default;
};

enum class PlayfieldGameplayMode : std::uint8_t {
  Unknown,
  Play,
  Replay,
  Practice,
};

enum class PlayfieldLoadingState : std::uint8_t {
  Unknown,
  Loading,
  Loaded,
};

struct PlayfieldAuthorityUpdate {
  double currentBpm = 0.0;
  std::map<Judgement, int> judgementCounters;
  std::map<Judgement, PlayfieldJudgementFastSlowCount>
      judgementFastSlowCounters;
  int comboBreak = 0;
  int maximumCombo = 0;
  // ScoreDataProperty's persisted best score.  It is zero when the chart has
  // no local best record, matching its gameplay-side initialization.
  int bestScore = 0;
  // ScoreDataProperty projects the persisted best ghost to the current passed
  // note count for NUMBER_DIFF_HIGHSCORE.  This target carries that optional
  // progression independently of the player-selected pacemaker target.
  pacemaker::Target bestScoreTarget;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  float currentGauge = 0.0F;
  GameplayGaugeRules gaugeRules;
  pacemaker::Target pacemakerTarget;
  pacemaker::Snapshot pacemakerStatus;
  std::string playOptionLabel;
  bool autoPlayMarkVisible = false;
  PlayfieldGameplayMode gameplayMode = PlayfieldGameplayMode::Unknown;
  PlayfieldLoadingState loadingState = PlayfieldLoadingState::Unknown;
  std::vector<int> startLaneIndicators;
  bool startLaneIndicatorsVisible = false;
  int laneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool liftEnabled = false;
  float liftRatio = 0.0F;
  bool hiddenEnabled = false;
  float hiddenRatio = 0.0F;
  bool resetLaneCoverVisibleTimeReference = false;

  bool operator==(const PlayfieldAuthorityUpdate &other) const;
};

struct GameplayLaneCoverAuthority {
  int percent = 0;
  bool enabled = true;
};

inline constexpr GameplayLaneCoverAuthority
gameplayLaneCoverAuthority(int percent, bool enabled = true) noexcept {
  return {.percent = percent, .enabled = enabled};
}

inline constexpr long long kPlayfieldTimestampOff =
    std::numeric_limits<long long>::min();

struct LanePresentationState {
  bool pressed = false;
  JudgeResult lastPressedJudge = JudgeResult(None, 0);
  // The exact encoded JudgeManager.getJudge() value used by Beatoraja image
  // selectors 500-519.  A Kpoor deliberately leaves this unchanged.
  int beatorajaJudgeValue = 0;
  long long pressMicros = kPlayfieldTimestampOff;
  long long releaseMicros = kPlayfieldTimestampOff;
  long long bombMicros = kPlayfieldTimestampOff;

  bool operator==(const LanePresentationState &other) const {
    return pressed == other.pressed &&
           lastPressedJudge.judgement == other.lastPressedJudge.judgement &&
           lastPressedJudge.Diff == other.lastPressedJudge.Diff &&
           beatorajaJudgeValue == other.beatorajaJudgeValue &&
           pressMicros == other.pressMicros &&
           releaseMicros == other.releaseMicros &&
           bombMicros == other.bombMicros;
  }
};

struct NotePresentationState {
  ChartVisualId id = 0;
  bool judged = false;
  bool dead = false;
  // Exact parser/gameplay event time for resolved-note rendering decisions.
  // Unplayed notes use the shared OFF sentinel rather than an ambiguous zero.
  long long playedTimeMicros = kPlayfieldTimestampOff;
  bool longActive = false;
  bool longDamaged = false;
  bool longReactive = false;

  bool operator==(const NotePresentationState &) const = default;
};

struct PresentationTouchPoint {
  long long fingerId = 0;
  ReplayTouchAction action = ReplayTouchAction::Move;
  float normalizedX = 0.0F;
  float normalizedY = 0.0F;
  long long songTimeMicros = 0;

  bool operator==(const PresentationTouchPoint &) const = default;
};

struct PlayfieldVisualState {
  PlayfieldFrameClock clock;
  PlayfieldPresentationConfig configuration;
  PlayfieldAuthorityUpdate authority;
  std::vector<LanePresentationState> lanes;
  std::vector<NotePresentationState> notes;
  std::vector<PresentationTouchPoint> touches;
  JudgeResult lastJudge = JudgeResult(None, 0);
  long long lastJudgeVisualMicros = kPlayfieldTimestampOff;
  GameplayBgaMissState bgaMiss;
  int combo = 0;
  int score = 0;
  int fastSlowMicros = 0;
  long long sceneStartMicros = kPlayfieldTimestampOff;
  long long playStartMicros = kPlayfieldTimestampOff;
};

// Skin destinations and timers share TimerManager's MainState clock. The
// gameplay projection retains chart/visual time, so derive the skin clock at
// this boundary without changing note-positioning semantics.
[[nodiscard]] inline constexpr long long
skinStateTimestampMicros(const PlayfieldVisualState &state,
                         long long visualTimestampMicros) noexcept {
  if (visualTimestampMicros == kPlayfieldTimestampOff ||
      state.sceneStartMicros == kPlayfieldTimestampOff) {
    return visualTimestampMicros;
  }
  const auto value = static_cast<__int128>(visualTimestampMicros) -
                     static_cast<__int128>(state.sceneStartMicros);
  if (value > std::numeric_limits<long long>::max()) {
    return std::numeric_limits<long long>::max();
  }
  // Preserve the explicit OFF sentinel for timer consumers.
  if (value <= std::numeric_limits<long long>::min()) {
    return std::numeric_limits<long long>::min() + 1;
  }
  return static_cast<long long>(value);
}

[[nodiscard]] inline constexpr long long
skinStateClockMicros(const PlayfieldVisualState &state) noexcept {
  return skinStateTimestampMicros(state, state.clock.visualTimeMicros);
}

class PlayfieldVisualStateStore final : public IPlayfieldPresentationEvents {
public:
  PlayfieldVisualStateStore() = default;
  explicit PlayfieldVisualStateStore(const PlayfieldChartVisualModel &model);

  void resetModel(const PlayfieldChartVisualModel &model);
  void setConfiguration(const PlayfieldPresentationConfig &configuration);
  void applyAuthorityUpdate(const PlayfieldAuthorityUpdate &update);
  void setNoteState(NotePresentationState state);
  void setNoteStates(std::vector<NotePresentationState> states);
  void setSceneStartMicros(long long value) noexcept;
  void setPlayStartMicros(long long value) noexcept;
  void setReplayTouchSamples(const std::vector<ReplayTouchSample> &samples);
  void setLiveTouchPoint(long long fingerId, ReplayTouchAction action, float x,
                         float y, long long songTimeMicros);
  void clearLiveTouchPoints();
  [[nodiscard]] PlayfieldVisualState capture(PlayfieldFrameClock clock) const;

  void onLanePressed(int lane, JudgeResult judge,
                     long long eventMicros) override;
  void onLaneReleased(int lane, long long eventMicros) override;
  void onJudge(JudgeResult judge, int combo, int score,
               PlayfieldJudgeEventClock clock,
               bool recordTimingSample) override;

private:
  struct TouchLifecycle {
    std::unordered_map<long long, PresentationTouchPoint> active;
    std::vector<PresentationTouchPoint> released;
  };

  static void applyTouchSample(TouchLifecycle &lifecycle,
                               const ReplayTouchSample &sample);
  static void pruneReleasedTouches(TouchLifecycle &lifecycle,
                                   long long currentTimeMicros);
  void advanceReplayTouches(long long replayTouchTimeMicros) const;
  void captureTouches(long long replayTouchTimeMicros) const;

  std::vector<int> laneOrder_;
  std::unordered_map<int, std::size_t> laneIndices_;
  PlayfieldPresentationConfig configuration_;
  PlayfieldAuthorityUpdate authority_;
  std::vector<LanePresentationState> lanes_;
  std::vector<NotePresentationState> notes_;
  std::unordered_map<ChartVisualId, std::size_t> noteIndices_;
  std::vector<ReplayTouchSample> replayTouchSamples_;
  mutable std::size_t replayTouchCursor_ = 0;
  mutable long long lastReplayTouchTimeMicros_ = -1;
  mutable TouchLifecycle replayTouches_;
  mutable TouchLifecycle liveTouches_;
  mutable std::vector<PresentationTouchPoint> touches_;
  GameplayBgaMissStateTracker bgaMissTracker_;
  JudgeResult lastJudge_ = JudgeResult(None, 0);
  long long lastJudgeVisualMicros_ = kPlayfieldTimestampOff;
  int combo_ = 0;
  int score_ = 0;
  int fastSlowMicros_ = 0;
  long long sceneStartMicros_ = kPlayfieldTimestampOff;
  long long playStartMicros_ = kPlayfieldTimestampOff;
};

class PlayfieldPresentationEventFanout final
    : public IPlayfieldPresentationEvents {
public:
  PlayfieldPresentationEventFanout(PlayfieldVisualStateStore &state,
                                   IPlayfieldPresentationEvents &presentation);

  void setPresentationSink(IPlayfieldPresentationEvents &presentation) noexcept;
  void onLanePressed(int lane, JudgeResult judge,
                     long long eventMicros) override;
  void onLaneReleased(int lane, long long eventMicros) override;
  void onJudge(JudgeResult judge, int combo, int score,
               PlayfieldJudgeEventClock clock,
               bool recordTimingSample) override;

private:
  PlayfieldVisualStateStore &state_;
  IPlayfieldPresentationEvents *presentation_;
};
