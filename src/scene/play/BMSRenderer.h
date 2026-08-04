//
// Created by XF on 9/2/2024.
//

#pragma once

#include "../../ReplayData.h"
#include "../../ReplayGhostUtils.h"
#include "../../AppSettings.h"
#include "../../audio/PlaybackRate.h"
#include "../../view/View.h"
#include "../../bms_parser.hpp"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../rendering/TexBatchRenderer.h"
#include "../../view/TextView.h"
#include "../../rendering/Color.h"
#include "../../rendering/Camera.h"
#include "JudgementIndicatorRenderer.h"
#include "Judge.h"
#include "Pacemaker.h"
#include "GameplayChartEntityRenderBudget.h"
#include "GameplayGaugeRules.h"
#include "GameplayNoteSubmissionOrder.h"
#include "PlayfieldPresentation.h"
#include "StartLaneIndicatorGeometry.h"
#include <bx/math.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rendering {
class SimpleBatchRenderer;
class TexBatchRenderer;
} // namespace rendering

class SpriteLoader;
struct PlayfieldProjectionResult;
struct BuiltInRendererTraversal;
struct LaneState {
  long long lastStateTime = -1;
  long long lastPressedTime = -1;
  bool isPressed = false;
  JudgeResult lastPressedJudge = JudgeResult(None, 0);
};

struct AtomicLaneState {
  std::atomic<long long> lastStateTime{-1};
  std::atomic<long long> lastPressedTime{-1};
  std::atomic<bool> isPressed{false};
  std::atomic<int> lastPressedJudgement{None};
  std::atomic<long long> lastPressedDiff{0};

  AtomicLaneState() = default;
  AtomicLaneState(const AtomicLaneState &) = delete;
  AtomicLaneState &operator=(const AtomicLaneState &) = delete;
  AtomicLaneState(AtomicLaneState &&other) noexcept;
  AtomicLaneState &operator=(AtomicLaneState &&other) noexcept;
};

class JudgeResult;

struct NoteUvRegion {
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;
};

struct NoteSheet {
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyOffTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyOnTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle hellChargeBodyOffTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle hellChargeBodyOnTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle hellChargeDamageTexture = BGFX_INVALID_HANDLE;
  NoteUvRegion note;
  NoteUvRegion longHead;
  NoteUvRegion longBodyOff;
  NoteUvRegion longBodyOn;
  NoteUvRegion longTail;
  NoteUvRegion hellChargeHead;
  NoteUvRegion hellChargeBodyOff;
  NoteUvRegion hellChargeBodyOn;
  NoteUvRegion hellChargeDamage;
  NoteUvRegion hellChargeTail;
  NoteUvRegion mine;
};

class BMSRendererState {
public:
  ~BMSRendererState() = default;
  std::unordered_set<bms_parser::LongNote *>
      orphanLongNotes; // long note whose head is dead but tail is alive
  size_t currentTimelineIndex = 0;
  void reset();
};

struct JudgementCounterSnapshot {
  int pgreat = 0;
  int great = 0;
  int good = 0;
  int bad = 0;
  int poor = 0;
  int kpoor = 0;
  int comboBreak = 0;
};

#if defined(ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION)
namespace bms_renderer_characterization {

inline constexpr std::size_t kNoTimelineOrdinal =
    std::numeric_limits<std::size_t>::max();

enum class Surface {
  Main,
  Ui,
};

enum class SubmissionKind {
  Background,
  JudgeLine,
  MeasureLine,
  NormalNote,
  InvisiblePrimitive,
  Mine,
  LongBody,
  LongTail,
  LongHead,
  ReplayGhostPrimitive,
  ReplayMissPrimitive,
  LaneBeamPass,
  LaneCoverPass,
  StartIndicatorPass,
  JudgementIndicatorPass,
  GaugePass,
  HudPass,
  TouchPass,
};

enum class LongBodyState {
  None,
  Off,
  On,
  Damage,
};

struct Rect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct FrameSnapshot {
  long long renderTimeMicros = 0;
  long long chartTimeMicros = 0;
  long long replayTouchTimeMicros = 0;
  double currentScrollPosition = 0.0;
  double visibleScrollMinimum = 0.0;
  double visibleScrollMaximum = 0.0;
  double visibleReferenceBpm = 0.0;
  float hispeed = 0.0F;
  float rxhs = 0.0F;
  float playAreaLeftX = 0.0F;
  float playAreaWidth = 0.0F;
  float noteRenderWidth = 0.0F;
  float noteRenderHeight = 0.0F;
  float lowerBound = 0.0F;
  float judgeY = 0.0F;
  float upperBound = 0.0F;
  float noteVisibleUpperBound = 0.0F;
  int laneCoverPercent = 0;
  Rect laneCoverHandle;
};

struct TimelineProjection {
  std::size_t timelineOrdinal = kNoTimelineOrdinal;
  long long timelineMicros = 0;
  float y = 0.0F;
  bool future = false;
  bool finite = false;
};

struct Submission {
  SubmissionKind kind = SubmissionKind::Background;
  Surface surface = Surface::Main;
  std::uint32_t depth = 0;
  std::size_t timelineOrdinal = kNoTimelineOrdinal;
  std::size_t pairedTimelineOrdinal = kNoTimelineOrdinal;
  long long timelineMicros = 0;
  int lane = -1;
  std::size_t primitiveOrdinal = 0;
  LongBodyState longBodyState = LongBodyState::None;
  Rect rect;
};

class Recorder {
public:
  virtual ~Recorder() = default;
  virtual void beginFrame(const FrameSnapshot &frame) = 0;
  virtual void project(const TimelineProjection &projection) = 0;
  virtual void submit(const Submission &submission) = 0;
};

} // namespace bms_renderer_characterization
#endif

class BMSRenderer : public PlayfieldPresentation {
public:
  ~BMSRenderer();

private:
  std::unique_ptr<TextView> titleText;
  std::unique_ptr<TextView> judgeText;
  std::unique_ptr<TextView> pacemakerDeltaText;
  std::unique_ptr<TextView> judgementTimingDirectionText;
  std::unique_ptr<TextView> judgementTimingMsText;
  std::unique_ptr<TextView> scoreText;
  std::unique_ptr<TextView> comboText;
  std::unique_ptr<TextView> pacemakerText;
  std::unique_ptr<TextView> gaugeText;
  std::unique_ptr<View> gaugeTypeBadge;
  std::unique_ptr<TextView> gaugeTypeText;
  std::unique_ptr<TextView> gaugeAutoShiftText;
  std::unique_ptr<TextView> playOptionText;
  std::unique_ptr<TextView> autoPlayMarkText;
  std::unique_ptr<TextView> laneCoverWhiteNumberText;
  std::unique_ptr<TextView> laneCoverVisibleTimeText;
  static constexpr size_t kJudgementCounterItemCount = 7;
  std::array<std::unique_ptr<TextView>, kJudgementCounterItemCount>
      judgementCounterLabelTexts;
  std::array<std::unique_ptr<TextView>, kJudgementCounterItemCount>
      judgementCounterValueTexts;
  std::atomic<uint32_t> hudRevision{1};
  uint32_t renderedHudRevision = 0;
  std::atomic<int> pendingJudge{None};
  std::atomic<int> pendingScore{0};
  std::atomic<int> pendingCombo{0};
  std::atomic<long long> pendingJudgeDiffMicros{0};
  std::atomic<long long> pendingJudgeDisplayMicros{0};
  std::atomic<uint32_t> pacemakerRevision{1};
  uint32_t renderedPacemakerRevision = 0;
  std::atomic<bool> pendingPacemakerEnabled{false};
  std::atomic<int> pendingPacemakerCurrentScore{0};
  std::atomic<int> pendingPacemakerTargetScore{0};
  std::atomic<int> pendingPacemakerFinalTargetScore{0};
  std::atomic<int> pendingPacemakerDelta{0};
  std::atomic<int> pendingPacemakerPlayedNotes{0};
  std::atomic<int> pendingPacemakerTotalNotes{0};
  std::atomic<bool> pendingPacemakerUsesReplayProgression{false};
  std::string pacemakerLabel;
  Judgement renderedJudgement = None;
  int renderedCombo = 0;
  bool renderedTimingFastShown = false;
  bool renderedTimingSlowShown = false;
  long long renderedTimingTextUntilMicros = 0;
  std::array<std::atomic<int>, kJudgementCounterItemCount>
      judgementCounterValues{};
  std::atomic<uint32_t> judgementCounterRevision{1};
  uint32_t renderedJudgementCounterRevision = 0;
  JudgementCounterSnapshot renderedJudgementCounterSnapshot;
  std::vector<int> laneOrder;
  std::vector<AtomicLaneState> laneStatesByOrder;
  std::unordered_map<int, size_t> laneToOrderIndex;
  std::vector<std::pair<int, LaneState>> laneStateSnapshot;
  std::vector<float> laneXLookup;
  std::vector<const NoteSheet *> laneSheetLookup;
  std::vector<size_t> whiteKeyLaneIndices;
  std::vector<size_t> blueKeyLaneIndices;
  std::vector<size_t> scratchLaneIndices;
  std::vector<int> startLaneIndicatorLanes;
  std::unordered_map<int, start_lane_indicator::ColorRole>
      startLaneIndicatorColorRoles;

  float noteImageHeight = 0;
  float noteImageWidth = 0;
  std::vector<bms_parser::TimeLine *> timelines;
  std::vector<std::vector<bms_parser::Note *>> groupedTimelineNotes;
  std::vector<ReplayGhostEvent> replayGhostEvents;
  std::vector<ReplayMissMarker> replayMissMarkers;
  std::vector<ReplayTouchSample> replayTouchSamples;
  struct TouchPointVisual {
    float x = 0.0f;
    float y = 0.0f;
    long long eventTimeMicros = 0;
    long long releaseTimeMicros = 0;
    bool released = false;
  };
  std::unordered_map<long long, TouchPointVisual> replayActiveTouchSamples;
  std::vector<TouchPointVisual> replayReleasedTouchSamples;
  std::unordered_map<long long, TouchPointVisual> liveTouchSamples;
  std::vector<TouchPointVisual> liveReleasedTouchSamples;
  JudgementIndicatorRenderer judgementIndicator;
  std::vector<double> timelineScrollPositions;
  struct LongNoteLookahead {
    float headY = 0.0F;
    gameplay_note_submission_order::LongNoteOrder order;
    bool renderBudgetReserved = false;
  };
  std::unordered_map<bms_parser::LongNote *, LongNoteLookahead>
      longNoteLookaheadScratch;
  gameplay_chart_entity_render_budget::Budget chartEntityRenderBudget;
  BMSRendererState state;
  int scratchLaneCount = 0;
  float playAreaWidth = AppSettings::kDefaultPlayAreaWidth;
  float playAreaLeftX = 0.0f;
  float noteRenderWidth = 1.0f;
  float noteRenderHeight = 1.0f;

  float longBodyRenderHeightOff = 1.0f;
  float longBodyRenderHeightOn = 1.0f;
  long long currentRenderMicros = 0;
  float lowerBound = -1.0f;
  float upperBound = 10.0f; // Calculated from camera projection
  float noteVisibleUpperBound = 10.0f;
  float judgeY = 0.0f;
  long long latePoorTiming;
  int visibleTimeGreenNumber = 400;
  audio::PlaybackRate playbackRate;
  bool visibleTimeUseMilliseconds = false;
  double currentBpm = 0.0;
  std::optional<double> floatingVisibleTimeReferenceBpm;
  AppSettings::VisibleTimeBpmStrategy visibleTimeBpmStrategy =
      AppSettings::VisibleTimeBpmStrategy::Chart;
  double mostPrevalentBpm = 0.0;
  bool renderHud = true;
  float judgementTextY = AppSettings::kDefaultJudgementTextY;
  bool judgementCounterEnabled = true;
  AppSettings::JudgementCounterPosition judgementCounterPosition =
      AppSettings::JudgementCounterPosition::Right;
  AppSettings::JudgementTimingDisplayCriteria judgementTimingFastSlowCriteria =
      AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  AppSettings::JudgementTimingDisplayCriteria
      judgementTimingMillisecondsCriteria =
          AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  AppSettings::GaugeBarPosition gaugeBarPosition =
      AppSettings::GaugeBarPosition::World;
  GaugeType currentGaugeType = GaugeType::Normal;
  GaugeProfile currentGaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode currentGaugeAutoShift = GaugeAutoShiftMode::None;
  float currentGaugeValue = 0.0f;
  float currentGaugeMaximum = 100.0f;
  float currentGaugeBorder = 80.0f;
  float currentGaugeReducedDamageZone = 0.0f;
  bool renderLaneBeams = true;
  bool laneCoverFloatingEnabled = true;
  bool useRenderTimeForLaneBeams = false;
  bool showInvisibleNotes = false;
  int laneBeamLengthPercent = AppSettings::kDefaultLaneBeamLengthPercent;
  int noteStartPositionPercent =
      AppSettings::kDefaultNoteStartPositionPercent;
  size_t replayTouchCursor = 0;
  long long lastReplayTouchTimeMicros = -1;
  bool touchVisualizationEnabled = true;
  bool replayGhostRenderingEnabled = true;
  bool autoPlayMarkVisible = false;
  bool startLaneIndicatorsVisible = false;

  rendering::SimpleBatchRenderer simpleBatchRenderer;
  rendering::SimpleBatchRenderer gimmickBatchRenderer;
  rendering::SimpleBatchRenderer ghostBatchRenderer;
  rendering::TexBatchRenderer noteTextureBatchRenderer;
  uint32_t activeNoteTextureDepth = std::numeric_limits<uint32_t>::max();
  uint32_t activeInvisibleDepth = std::numeric_limits<uint32_t>::max();
  int judgementLayoutWidth = 0;
  int judgementLayoutHeight = 0;
  bool judgementLayoutHasTimingDirection = false;
  bool judgementLayoutHasTimingMs = false;
  bool judgementLayoutHasPacemakerDelta = false;

  void drawRect(float width, float height, float x, float y, Color color);
  void drawHudRoundedPanel(float x, float y, float width, float height,
                           float radius, const Color &fill,
                           const Color &border);
  void drawRoundedPanel(float x, float y, float width, float height,
                        float radius, float borderWidth, const Color &fill,
                        const Color &border);
  void drawGameplayHudPanels();
  void drawGaugeBar();
  void drawWorldGaugeBar();
  void drawHudGaugeBar();
  void drawJudgementAccentBar();
  void drawJudgementCounterPanels();
  void layoutGameplayHud();
  void layoutAutoPlayMark();
  void layoutGaugeText();
  void refreshGaugeTextStyle();
  std::array<float, 4> worldGaugeRect() const;
  std::array<float, 4> hudGaugeRect() const;
  float gameplayHudRightReserveLeft() const;
  float gameplayHudTitleWidth() const;
  float projectedLaneLeftUiInBand(float bandTop, float bandBottom) const;
  void layoutCenteredJudgementText();
  void updateJudgementCounterText();
  void publishJudgementCounterSnapshot(
      const JudgementCounterSnapshot &snapshot);
  void drawLaneBeam(int lane, const LaneState &laneState, long long time);
  void drawStartLaneIndicators();
  void drawLaneCover();
  void layoutLaneCoverNumberTexts();
  void drawTitle(RenderContext &context) const;
  void drawJudgement(RenderContext context) const;
  void drawScore(RenderContext &context) const;
  void drawGauge(RenderContext &context) const;
  void drawPlayOption(RenderContext &context) const;
  void drawAutoPlayMark(RenderContext &context) const;
  void drawLongNote(
      float headY, float tailY, bms_parser::LongNote *const &head,
      gameplay_note_submission_order::LongNoteOrder order,
      bool renderBudgetReserved);
  void drawNormalNote(float y, bms_parser::Note *const &note,
                      uint32_t submitDepth);
  void drawInvisibleNote(float y, bms_parser::Note *const &note,
                         uint32_t submitDepth);
  void drawLandmineNote(float y, bms_parser::LandmineNote *const &note,
                        uint32_t submitDepth);
  // The captured route deliberately receives no mutable parser state.  The
  // legacy overload supplies nullptr and retains the historical traversal for
  // preview/export callers.
  void renderFrame(RenderContext &context, long long micro,
                   long long replayTouchTimeMicros,
                   const PlayfieldProjectionResult *projection);
  void drawReplayGhosts(float rxhs, long long currentTimeMicros,
                        double currentScrollPosition);
  void drawGhostNoteOutline(float y, const ReplayGhostEvent &event);
  void drawReplayMissMarkers(float rxhs, double currentScrollPosition);
  void drawMissMarkerX(float y, const ReplayMissMarker &marker);
  void drawTouchPoints(long long replayTouchTimeMicros);
  void advanceReplayTouches(long long replayTouchTimeMicros);
  void applyTouchSample(
      std::unordered_map<long long, TouchPointVisual> &activeTouches,
      std::vector<TouchPointVisual> &releasedTouches,
      const ReplayTouchSample &sample);
  void pruneReleasedTouchSamples(
      std::vector<TouchPointVisual> &releasedTouches,
      long long currentTimeMicros);
  void drawTouchSample(const TouchPointVisual &sample,
                       long long currentTimeMicros);
  void buildTimelineScrollPositions();
  double calculateMostPrevalentBpm() const;
  double visibleTimeReferenceBpm() const;
  double scrollPositionAtTime(long long timeMicros) const;
  void applyPendingHudText(long long currentMicros);
  void applyPendingPacemakerText();
  void expireLingeringTimingText(long long currentMicros);
  bgfx::TextureHandle loadSheetTexture(SpriteLoader &loader, const char *label);
  bgfx::TextureHandle loadCroppedTexture(SpriteLoader &loader, int x, int y,
                                         int width, int height,
                                         const char *label);
  bool isLeftScratch(int lane) const;
  bool isRightScratch(int lane) const;
  bool isScratch(int lane) const;
  struct LaneCoverHandleGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };
  struct LaneCoverVirtualHandleGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };
  LaneCoverHandleGeometry laneCoverHandleGeometry() const;
  std::optional<LaneCoverVirtualHandleGeometry>
  laneCoverVirtualHandleGeometry() const;
  std::optional<bx::Vec3> lanePlanePointAtRenderPosition(float renderX,
                                                         float renderY) const;
  std::optional<std::pair<float, float>> projectLanePointToUi(
      float worldX, float worldY) const;
  int effectiveVisibleTimeGreenNumber() const;
  std::string laneCoverVisibleTimeLabel() const;
  float computeLaneX(int lane) const;
  void rebuildPlayAreaGeometry();
  float laneToX(int lane) const;
  const NoteSheet &sheetForLane(int lane) const;
  rendering::TexBatchRenderer &
  noteTextureBatchAtDepth(uint32_t submitDepth);
  void setInvisibleBatchDepth(uint32_t submitDepth);
  void beginOrderedNoteBatches();
  void flushOrderedNoteBatches();
  void destroyNoteSheetTextures();
  float calculateLanePlaneScreenTopIntersection();
  NoteSheet graySheet;
  NoteSheet blueSheet;
  NoteSheet scratchSheet;
  bms_parser::Chart *chart;
  bool laneIsCurrentlyPressed(int lane) const;
#if defined(ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION)
  bms_renderer_characterization::Recorder *characterizationRecorder = nullptr;
  [[nodiscard]] std::size_t characterizationTimelineOrdinal(
      const bms_parser::TimeLine *timeline) const;
  void recordCharacterizationSubmission(
      bms_renderer_characterization::SubmissionKind kind,
      bms_renderer_characterization::Surface surface, std::uint32_t depth,
      const bms_parser::TimeLine *timeline,
      const bms_parser::TimeLine *pairedTimeline, int lane,
      std::size_t primitiveOrdinal,
      bms_renderer_characterization::LongBodyState longBodyState,
      bms_renderer_characterization::Rect rect);
#endif

public:
  static std::unique_ptr<TextView> createAutoPlayMarkText();
  static std::array<float, 4> autoPlayMarkRect();
  static void layoutAutoPlayMark(TextView *text);
  static void renderAutoPlayMark(TextView *text, RenderContext &context);

  void configure(const PlayfieldPresentationConfig &configuration) override;
  void onLanePressed(int lane, JudgeResult judge,
                     long long time) override;
  void onLaneReleased(int lane, long long time) override;
  void onJudge(JudgeResult judgeResult, int combo, int score,
               PlayfieldJudgeEventClock clock,
               bool recordTimingSample = true) override;
  explicit BMSRenderer(
      bms_parser::Chart *chart,
      const std::map<Judgement, std::pair<long long, long long>> &timingWindows,
      int visibleTimeGreenNumber, bool renderHud = true,
      audio::PlaybackRate playbackRate = {});

  void render(RenderContext &context, long long micro);
  void render(RenderContext &context, long long micro,
              long long replayTouchTimeMicros);
  void render(RenderContext &context, const PlayfieldVisualState &state,
              const PlayfieldProjectionResult &projection);
  // Projection capture must use the identical late-poor boundary as the
  // legacy renderer so its immutable row/depth plan has the same eligibility.
  [[nodiscard]] long long projectionLatePoorTimingMicros() const noexcept {
    return latePoorTiming;
  }
  [[nodiscard]] BuiltInRendererTraversal
  builtInProjectionTraversal() const;
  void reset() override;
  void refreshGeometry() override;
  void setVisibleTimeGreenNumber(int greenNumber);
  void setVisibleTimeUseMilliseconds(bool enabled);
  void setCurrentBpm(double bpm);
  void setVisibleTimeBpmStrategy(
      AppSettings::VisibleTimeBpmStrategy strategy);
  void setPlayAreaWidth(float width);
  void setLaneBeamsEnabled(bool enabled);
  void setLaneCoverFloatingEnabled(bool enabled);
  [[nodiscard]] std::optional<std::array<std::pair<float, float>, 4>>
  gameplayTouchBoundsUi() const;
  void setLaneBeamLengthPercent(int percent);
  void setNoteStartPositionPercent(int percent);
  void applyLaneCoverState(int percent, bool resetVisibleTimeReference);
  bool isLaneCoverHandleHit(float renderX, float renderY) const;
  std::optional<float> laneCoverHandleGrabOffset(float renderX,
                                                float renderY) const;
  int dragLaneCoverHandleTo(float renderX, float renderY,
                            float lanePointYOffset);
  void setLaneBeamClockUsesRenderTime(bool enabled);
  void setShowInvisibleNotes(bool enabled);
  void setJudgementIndicatorConfig(bool enabled, float y, float widthScale,
                                   bool hudMode, int rangeMilliseconds);
  void setJudgementTextY(float y);
  void setJudgementCounterEnabled(bool enabled);
  void setJudgementCounterPosition(
      AppSettings::JudgementCounterPosition position);
  void setJudgementTimingFastSlowCriteria(
      AppSettings::JudgementTimingDisplayCriteria criteria);
  void setJudgementTimingMillisecondsCriteria(
      AppSettings::JudgementTimingDisplayCriteria criteria);
  void setJudgementCounter(Judgement judgement, int count, int comboBreak);
  void setJudgementCounters(const std::map<Judgement, int> &judgeCounts,
                            int comboBreak);
  void setGaugeBarPosition(AppSettings::GaugeBarPosition position);
  void setGaugeStatus(GaugeType gaugeType, GaugeAutoShiftMode gaugeAutoShift,
                      float currentGauge,
                      const GameplayGaugeRules &gaugeRules);
  void setPacemakerTarget(const pacemaker::Target &target);
  void setPacemakerStatus(const pacemaker::Snapshot &snapshot);
  void setPlayOptionStatus(const std::string &label);
  void setReplayData(const ReplayData *replayData);
  void setAutoPlayMarkVisible(bool visible);
  void setTouchVisualizationEnabled(bool enabled);
  void setReplayGhostRenderingEnabled(bool enabled);
  void setStartLaneIndicators(std::vector<int> lanes);
  void setStartLaneIndicatorsVisible(bool visible);
#if defined(ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION)
  void setCharacterizationRecorder(
      bms_renderer_characterization::Recorder *recorder) {
    characterizationRecorder = recorder;
  }
#endif
  void setLiveTouchPoint(long long fingerId, ReplayTouchAction action, float x,
                         float y, long long songTimeMicros);
  void clearLiveTouchPoints();
};
