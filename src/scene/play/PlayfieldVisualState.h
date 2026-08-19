#pragma once

#include "../../AppSettings.h"
#include "../../JudgementIndicatorRange.h"
#include "../../ReplayData.h"
#include "../../audio/GameplayBgaMissStateTracker.h"
#include "GameplayGaugeRules.h"
#include "Pacemaker.h"
#include "PlayfieldChartVisualModel.h"
#include "PlayfieldPresentationEvents.h"
#include "../../practice/PracticeConfiguration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
  // Beatoraja PlayConfig.duration, retained without a green-number round trip.
  int visibleTimeDurationMilliseconds = 667;
  // Live LaneRenderer.getHispeed() state. An engaged value, including zero,
  // is authoritative for note traversal and skin properties; disengaged keeps
  // the legacy preview fallback for callers that do not own lane state.
  std::optional<float> configuredHispeed;
  float hispeedMultiplier = 1.0F;
  bool visibleTimeUseMilliseconds = false;
  AppSettings::HiSpeedFixMode hispeedFixMode =
      AppSettings::HiSpeedFixMode::Main;
  float playAreaWidth = 0.0F;
  bool laneBeamsEnabled = true;
  // Live LaneRenderer::getHispeed() cover factor.  It is intentionally kept
  // separate from noteStartPositionPercent because toggling lane cover in
  // Beatoraja does not reset Hi-Speed.
  float laneCoverHispeedFactor = 1.0F;
  bool laneCoverEnabled = true;
  int laneBeamLengthPercent = 100;
  int noteStartPositionPercent = 0;
  bool laneBeamClockUsesRenderTime = false;
  bool showInvisibleNotes = false;
  bool showPastNotes = false;
  // Config.AudioConfig's system/key/background gains, captured when the
  // presentation begins so gameplay skins read the same profile values in
  // live play, replay watch, and replay export.
  float masterVolume = 1.0F;
  float keysoundVolume = 1.0F;
  float bgmVolume = 1.0F;
  // Config.getBga's ON/OFF subset captured at play start. Aso has no AUTO
  // mode, so callers map true to BGA_ON (0) and false to BGA_OFF (2).
  bool bgaEnabled = true;
  // BMSPlayerConfig.bpmguide controls the optional BPM/STOP guide lines.
  bool bpmGuideEnabled = false;
  // PlayConfig.isEnableHispeedAutoAdjust.
  bool hispeedAutoAdjust = false;
  bool markProcessedNotes = false;
  bool customJudge = false;
  bool showJudgeArea = false;
  bool notesDisplayTimingAutoAdjust = false;
  std::array<int, 4> autoSaveReplay{};
  bool guideSoundEffects = false;
  int extraNoteDepth = 0;
  int mineMode = 0;
  int scrollMode = 0;
  int longNoteModifierMode = 0;
  int sevenToNinePattern = 0;
  int sevenToNineType = 0;
  bool constantScroll = false;
  int constantFadeInMilliseconds = 100;
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
  // IntegerPropertyFactory.judgealgorithm (340) exposes only the three
  // entries in JudgeAlgorithm.defaultAlgorithm. Score is intentionally not
  // an image index and therefore uses Java's Integer.MIN_VALUE sentinel.
  std::int32_t judgeAlgorithmImageIndex =
      std::numeric_limits<std::int32_t>::min();

  bool operator==(const PlayfieldPresentationConfig &) const = default;
};

inline constexpr std::int32_t
beatorajaJudgeAlgorithmImageIndex(AppSettings::NotePriorityMode mode) noexcept {
  switch (mode) {
  case AppSettings::NotePriorityMode::Combo:
    return 0;
  case AppSettings::NotePriorityMode::Duration:
    return 1;
  case AppSettings::NotePriorityMode::Lowest:
    return 2;
  case AppSettings::NotePriorityMode::Score:
    return std::numeric_limits<std::int32_t>::min();
  }
  return std::numeric_limits<std::int32_t>::min();
}

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

// Immutable projection of the local persisted score record read by
// Beatoraja's ScoreDataProperty at BMSPlayer startup. It stays separate from
// the live JudgeManager counters in this frame.
struct PlayfieldPersistedScoreState {
  int score = 0;
  int maxScore = 0;
  int totalNotes = 0;
  std::array<int, 5> judgementCounts{};
  std::optional<std::int64_t> lastPlayedUnixSeconds;

  bool operator==(const PlayfieldPersistedScoreState &) const = default;
};

// Immutable ScoreData-equivalent target record. It is kept distinct from a
// pacemaker, which carries score progression but not source judge counts.
struct PlayfieldRivalScoreState {
  int score = 0;
  int totalNotes = 0;
  std::array<int, 5> judgementCounts{};

  bool operator==(const PlayfieldRivalScoreState &) const = default;
};

struct PlayfieldPlayerScoreHistoryState {
  int playCount = 0;
  int clearCount = 0;
  std::array<int, 5> judgementCounts{};
  std::int64_t playDurationSeconds = 0;

  bool operator==(const PlayfieldPlayerScoreHistoryState &) const = default;
};

struct PlayfieldAuthorityUpdate {
  double currentBpm = 0.0;
  // TimeLine.getScroll() currently active at the authoritative gameplay
  // cursor. LaneRenderer's duration_green divides by this value.
  double currentScrollRate = 1.0;
  std::map<Judgement, int> judgementCounters;
  std::map<Judgement, PlayfieldJudgementFastSlowCount>
      judgementFastSlowCounters;
  int comboBreak = 0;
  int maximumCombo = 0;
  // These are JudgeManager's per-chart values.  They intentionally remain
  // separate from the displayed course combo so TIMER_FULLCOMBO_1P has the
  // same source as Beatoraja.
  int stageCombo = 0;
  int stagePassedNotes = 0;
  // ScoreDataProperty's persisted best score.  It is zero when the chart has
  // no local best record, matching its gameplay-side initialization.
  int bestScore = 0;
  // ScoreDataProperty.scoreData is the local persisted record, not the live
  // judgement state. It drives properties 80-89, their float counterparts,
  // and the last-play calendar values.
  std::optional<PlayfieldPersistedScoreState> persistedScore;
  std::optional<PlayfieldRivalScoreState> rivalScore;
  PlayfieldPlayerScoreHistoryState playerScoreHistory;
  // ScoreDataProperty projects the persisted best ghost to the current passed
  // note count for NUMBER_DIFF_HIGHSCORE.  This target carries that optional
  // progression independently of the player-selected pacemaker target.
  pacemaker::Target bestScoreTarget;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  float currentGauge = 0.0F;
  GameplayGaugeRules gaugeRules;
  pacemaker::Target pacemakerTarget;
  pacemaker::Snapshot pacemakerStatus;
  // IndexType.option_1p/option_2p/option_dp values. These retain the raw
  // canonical option identity; display text is deliberately separate.
  int player1RandomOption = 0;
  int player2RandomOption = 0;
  int doublePlayOption = 0;
  // IndexType.option_target1_* reads ScoreData.option when the active target
  // owns one. Aso has no rival/target ScoreData transport yet, so absence is
  // retained distinctly from an authored NORMAL option value of zero.
  std::optional<int> targetPlayOption;
  // SongReview.favorite_chart has states none/favorite/invisible. The chart
  // repository currently owns only none/favorite, represented as 0/1.
  int favoriteChartState = 0;
  // SongData.CONTENT_TEXT captured by the chart-library scan. Beatoraja
  // assigns it to every chart in a folder containing an immediate .txt file.
  bool chartHasDocument = false;
  // BMSResource.setBMSFile() exposes these only after PixmapResourcePool has
  // decoded the declared image and created the gameplay resource.
  bool stageFileAvailable = false;
  bool backBmpAvailable = false;
  // StringPropertyFactory.player reads PlayerConfig.name. The application's
  // active profile supplies the equivalent immutable name for a presentation.
  std::string playerName;
  std::string playOptionLabel;
  // IntegerPropertyFactory.current_fps and MainController.getPlayTime(),
  // sampled by the outer application loop rather than inferred from chart
  // timing.
  int currentFramesPerSecond = 0;
  std::int64_t applicationUptimeMillis = 0;
  bool autoPlayMarkVisible = false;
  PlayfieldGameplayMode gameplayMode = PlayfieldGameplayMode::Unknown;
  PlayfieldLoadingState loadingState = PlayfieldLoadingState::Unknown;
  // Captured from the active CoursePlaySession or replay-export stage loop.
  // A negative index and zero count denote ordinary single-chart gameplay.
  bool courseMode = false;
  int courseStageIndex = -1;
  int courseStageCount = 0;
  std::vector<std::string> courseStageTitles;
  std::vector<int> startLaneIndicators;
  bool startLaneIndicatorsVisible = false;
  int laneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool liftEnabled = false;
  float liftRatio = 0.0F;
  bool hiddenEnabled = false;
  float hiddenRatio = 0.0F;
  // BMSPlayer starts TIMER_FAILED when its active survival gauge transitions
  // into failure. Keep that event distinct from a display gauge at zero.
  bool failureAnimationActive = false;
  // BMSPlayer's lane-cover-changing option is true while either physical
  // Start or Select is held, not only when an adjustment was emitted.
  bool laneCoverAdjustmentHeld = false;
  bool laneCoverChanged = false;
  ReplayLaneCoverChangeKind laneCoverChangeKind =
      ReplayLaneCoverChangeKind::Value;
  bool resetLaneCoverVisibleTimeReference = false;
  std::optional<practice::SkinMenuState> practiceMenu;

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

// A chronological bounded copy of the timing samples that feed the built-in
// judgement indicator. It is part of the immutable presentation snapshot so
// a prepared frame has the same recent window as the event-driven renderer.
struct PlayfieldJudgementIndicatorSample {
  JudgeResult judge = JudgeResult(None, 0);
  long long visualTimeMicros = kPlayfieldTimestampOff;
};

struct PlayfieldVisualState {
  PlayfieldFrameClock clock;
  PlayfieldPresentationConfig configuration;
  PlayfieldAuthorityUpdate authority;
  std::vector<LanePresentationState> lanes;
  std::vector<NotePresentationState> notes;
  // Presentation frames normally retain the state-store snapshot by shared
  // ownership. `notes` remains the owning value form for callers that need a
  // standalone DTO (tests, diagnostics, and explicit capture()).
  std::shared_ptr<const std::vector<NotePresentationState>> noteSnapshot;
  std::shared_ptr<const std::unordered_map<ChartVisualId, std::size_t>>
      noteSnapshotIndices;
  std::vector<PresentationTouchPoint> touches;
  JudgeResult lastJudge = JudgeResult(None, 0);
  long long lastJudgeVisualMicros = kPlayfieldTimestampOff;
  std::array<PlayfieldJudgementIndicatorSample,
             judgement_indicator::kRecentTimingSampleCapacity>
      judgementIndicatorSamples{};
  std::size_t judgementIndicatorSampleCount = 0;
  GameplayBgaMissState bgaMiss;
  int combo = 0;
  int score = 0;
  int fastSlowMicros = 0;
  long long sceneStartMicros = kPlayfieldTimestampOff;
  long long playStartMicros = kPlayfieldTimestampOff;

  [[nodiscard]] std::span<const NotePresentationState>
  noteStates() const noexcept {
    if (noteSnapshot) {
      return *noteSnapshot;
    }
    return notes;
  }

  [[nodiscard]] const NotePresentationState *
  noteState(ChartVisualId id) const noexcept {
    if (noteSnapshot && noteSnapshotIndices) {
      const auto found = noteSnapshotIndices->find(id);
      if (found == noteSnapshotIndices->end() ||
          found->second >= noteSnapshot->size()) {
        return nullptr;
      }
      return &(*noteSnapshot)[found->second];
    }
    for (const auto &state : notes) {
      if (state.id == id) {
        return &state;
      }
    }
    return nullptr;
  }
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
  if (state.sceneStartMicros < 0 &&
      visualTimestampMicros >
          std::numeric_limits<long long>::max() + state.sceneStartMicros) {
    return std::numeric_limits<long long>::max();
  }
  // Preserve the explicit OFF sentinel for timer consumers.
  if (state.sceneStartMicros > 0 &&
      visualTimestampMicros <=
          std::numeric_limits<long long>::min() + state.sceneStartMicros) {
    return std::numeric_limits<long long>::min() + 1;
  }
  return visualTimestampMicros - state.sceneStartMicros;
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
  [[nodiscard]] PlayfieldVisualState
  capture(PlayfieldFrameClock clock, bool includeNotes = true) const;
  // Rendering consumes this value before the next gameplay update. It keeps a
  // stable copy-on-write note snapshot, avoiding a chart-sized copy per frame.
  [[nodiscard]] PlayfieldVisualState
  captureForPresentation(PlayfieldFrameClock clock) const;

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
  void detachNoteSnapshot();

  std::vector<int> laneOrder_;
  std::unordered_map<int, std::size_t> laneIndices_;
  PlayfieldPresentationConfig configuration_;
  PlayfieldAuthorityUpdate authority_;
  std::vector<LanePresentationState> lanes_;
  std::shared_ptr<std::vector<NotePresentationState>> notes_;
  std::shared_ptr<const std::unordered_map<ChartVisualId, std::size_t>>
      noteIndices_;
  std::vector<ReplayTouchSample> replayTouchSamples_;
  mutable std::size_t replayTouchCursor_ = 0;
  mutable long long lastReplayTouchTimeMicros_ = -1;
  mutable TouchLifecycle replayTouches_;
  mutable TouchLifecycle liveTouches_;
  mutable std::vector<PresentationTouchPoint> touches_;
  GameplayBgaMissStateTracker bgaMissTracker_;
  JudgeResult lastJudge_ = JudgeResult(None, 0);
  long long lastJudgeVisualMicros_ = kPlayfieldTimestampOff;
  std::array<PlayfieldJudgementIndicatorSample,
             judgement_indicator::kRecentTimingSampleCapacity>
      judgementIndicatorSamples_{};
  std::size_t judgementIndicatorSampleCount_ = 0;
  std::size_t nextJudgementIndicatorSample_ = 0;
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
