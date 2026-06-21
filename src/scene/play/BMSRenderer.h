//
// Created by XF on 9/2/2024.
//

#pragma once

#include "../../ReplayData.h"
#include "../../ReplayGhostUtils.h"
#include "../../AppSettings.h"
#include "../../view/View.h"
#include "../../bms_parser.hpp"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../rendering/TexBatchRenderer.h"
#include "../../view/TextView.h"
#include "../../rendering/Color.h"
#include "../../rendering/Camera.h"
#include "JudgementIndicatorRenderer.h"
#include "Judge.h"
#include <bx/math.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
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
struct LaneState {
  long long lastStateTime = -1;
  bool isPressed = false;
  JudgeResult lastPressedJudge = JudgeResult(None, 0);
};

struct AtomicLaneState {
  std::atomic<long long> lastStateTime{-1};
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
  NoteUvRegion note;
  NoteUvRegion longHead;
  NoteUvRegion longBodyOff;
  NoteUvRegion longBodyOn;
  NoteUvRegion longTail;
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

class BMSRenderer {
public:
  ~BMSRenderer();

private:
  std::unique_ptr<TextView> titleText;
  std::unique_ptr<TextView> judgeText;
  std::unique_ptr<TextView> judgementTimingDirectionText;
  std::unique_ptr<TextView> judgementTimingMsText;
  std::unique_ptr<TextView> scoreText;
  std::unique_ptr<TextView> comboText;
  std::unique_ptr<TextView> gaugeText;
  std::unique_ptr<TextView> playOptionText;
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
  Judgement renderedJudgement = None;
  int renderedCombo = 0;
  bool renderedTimingFastShown = false;
  bool renderedTimingSlowShown = false;
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

  float noteImageHeight = 0;
  float noteImageWidth = 0;
  std::vector<bms_parser::TimeLine *> timelines;
  std::vector<std::vector<bms_parser::Note *>> groupedTimelineNotes;
  std::vector<ReplayGhostEvent> replayGhostEvents;
  std::vector<ReplayMissMarker> replayMissMarkers;
  JudgementIndicatorRenderer judgementIndicator;
  std::vector<double> timelineScrollPositions;
  std::unordered_map<bms_parser::LongNote *, float> longNoteLookaheadScratch;
  BMSRendererState state;
  int scratchLaneCount = 0;
  float playAreaWidth = AppSettings::kDefaultPlayAreaWidth;
  float playAreaLeftX = 0.0f;
  float noteRenderWidth = 1.0f;
  float noteRenderHeight = 1.0f;

  float longBodyRenderHeightOff = 1.0f;
  float longBodyRenderHeightOn = 1.0f;
  float lowerBound = -1.0f;
  float upperBound = 10.0f; // Calculated from camera projection
  float noteVisibleUpperBound = 10.0f;
  float judgeY = 0.0f;
  long long latePoorTiming;
  int visibleTimeGreenNumber = 400;
  AppSettings::VisibleTimeBpmStrategy visibleTimeBpmStrategy =
      AppSettings::VisibleTimeBpmStrategy::Chart;
  double mostPrevalentBpm = 0.0;
  bool renderHud = true;
  float judgementTextY = AppSettings::kDefaultJudgementTextY;
  bool judgementCounterEnabled = true;
  AppSettings::JudgementCounterPosition judgementCounterPosition =
      AppSettings::JudgementCounterPosition::Right;
  AppSettings::JudgementTimingDisplayMode judgementTimingDisplayMode =
      AppSettings::JudgementTimingDisplayMode::Both;
  AppSettings::JudgementTimingDisplayCriteria judgementTimingDisplayCriteria =
      AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  AppSettings::GaugeBarPosition gaugeBarPosition =
      AppSettings::GaugeBarPosition::World;
  GaugeType currentGaugeType = GaugeType::Normal;
  bool currentGaugeAutoShift = false;
  float currentGaugeValue = 0.0f;
  bool renderLaneBeams = true;
  bool useRenderTimeForLaneBeams = false;
  bool showInvisibleNotes = false;
  int laneBeamLengthPercent = AppSettings::kDefaultLaneBeamLengthPercent;
  int noteStartPositionPercent =
      AppSettings::kDefaultNoteStartPositionPercent;

  rendering::SimpleBatchRenderer simpleBatchRenderer;
  rendering::SimpleBatchRenderer gimmickBatchRenderer;
  rendering::SimpleBatchRenderer ghostBatchRenderer;
  std::vector<rendering::TexBatchRenderer> noteTextureBatchRenderers;
  std::unordered_map<uint64_t, size_t> noteTextureBatchLookup;
  uint32_t longBodySubmitDepth = 0;
  uint32_t noteSheetSubmitDepth = 0;
  int judgementLayoutWidth = 0;
  int judgementLayoutHeight = 0;
  bool judgementLayoutHasTimingDirection = false;
  bool judgementLayoutHasTimingMs = false;

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
  void layoutGaugeText();
  std::array<float, 4> worldGaugeRect() const;
  std::array<float, 4> hudGaugeRect() const;
  float gameplayHudTitleWidth() const;
  float projectedLaneLeftUiInBand(float bandTop, float bandBottom) const;
  void layoutCenteredJudgementText();
  void updateJudgementCounterText();
  void publishJudgementCounterSnapshot(
      const JudgementCounterSnapshot &snapshot);
  void drawLaneBeam(int lane, const LaneState &laneState, long long time);
  void drawLaneCover();
  void drawTitle(RenderContext &context) const;
  void drawJudgement(RenderContext context) const;
  void drawScore(RenderContext &context) const;
  void drawGauge(RenderContext &context) const;
  void drawPlayOption(RenderContext &context) const;
  void drawLongNote(float headY, float tailY,
                    bms_parser::LongNote *const &head);
  void drawNormalNote(float y, bms_parser::Note *const &note);
  void drawInvisibleNote(float y, bms_parser::Note *const &note);
  void drawLandmineNote(float y, bms_parser::LandmineNote *const &note);
  void drawReplayGhosts(float rxhs, long long currentTimeMicros,
                        double currentScrollPosition);
  void drawGhostNoteOutline(float y, const ReplayGhostEvent &event);
  void drawReplayMissMarkers(float rxhs, double currentScrollPosition);
  void drawMissMarkerX(float y, const ReplayMissMarker &marker);
  void buildTimelineScrollPositions();
  double calculateMostPrevalentBpm() const;
  double visibleTimeReferenceBpm() const;
  double scrollPositionAtTime(long long timeMicros) const;
  void applyPendingHudText();
  bgfx::TextureHandle loadSheetTexture(SpriteLoader &loader, const char *label);
  bgfx::TextureHandle loadCroppedTexture(SpriteLoader &loader, int x, int y,
                                         int width, int height,
                                         const char *label);
  bool isLeftScratch(int lane) const;
  bool isRightScratch(int lane) const;
  bool isScratch(int lane) const;
  float computeLaneX(int lane) const;
  void rebuildPlayAreaGeometry();
  float laneToX(int lane) const;
  const NoteSheet &sheetForLane(int lane) const;
  rendering::TexBatchRenderer &noteTextureBatch(bgfx::TextureHandle texture,
                                                uint32_t submitDepth);
  rendering::TexBatchRenderer &sheetBatchFor(const NoteSheet &sheet);
  rendering::TexBatchRenderer &longBodyBatchFor(const NoteSheet &sheet,
                                                bool isHolding);
  void beginNoteTextureBatches(uint32_t bodyDepth, uint32_t sheetDepth);
  void flushNoteTextureBatches();
  void destroyNoteSheetTextures();
  float calculateLanePlaneScreenTopIntersection();
  NoteSheet graySheet;
  NoteSheet blueSheet;
  NoteSheet scratchSheet;
  bms_parser::Chart *chart;

public:
  void onLanePressed(int lane, const JudgeResult judge, long long time);
  void onLaneReleased(int lane, long long time);
  void onJudge(JudgeResult judgeResult, int combo, int score,
               long long displayTimeMicros, bool recordTimingSample = true);
  explicit BMSRenderer(
      bms_parser::Chart *chart,
      const std::map<Judgement, std::pair<long long, long long>> &timingWindows,
                       int visibleTimeGreenNumber, bool renderHud = true);

  void render(RenderContext &context, long long micro);
  void reset();
  void refreshGeometry();
  void setVisibleTimeGreenNumber(int greenNumber);
  void setVisibleTimeBpmStrategy(
      AppSettings::VisibleTimeBpmStrategy strategy);
  void setPlayAreaWidth(float width);
  void setLaneBeamsEnabled(bool enabled);
  void setLaneBeamLengthPercent(int percent);
  void setNoteStartPositionPercent(int percent);
  void setLaneBeamClockUsesRenderTime(bool enabled);
  void setShowInvisibleNotes(bool enabled);
  void setJudgementIndicatorConfig(bool enabled, float y, float widthScale,
                                   bool hudMode);
  void setJudgementTextY(float y);
  void setJudgementCounterEnabled(bool enabled);
  void setJudgementCounterPosition(
      AppSettings::JudgementCounterPosition position);
  void setJudgementTimingDisplayMode(
      AppSettings::JudgementTimingDisplayMode mode);
  void setJudgementTimingDisplayCriteria(
      AppSettings::JudgementTimingDisplayCriteria criteria);
  void setJudgementCounter(Judgement judgement, int count, int comboBreak);
  void setJudgementCounters(const std::map<Judgement, int> &judgeCounts,
                            int comboBreak);
  void setGaugeBarPosition(AppSettings::GaugeBarPosition position);
  void setGaugeStatus(GaugeType gaugeType, bool gaugeAutoShift,
                      float currentGauge);
  void setPlayOptionStatus(const std::string &label);
  void setReplayData(const ReplayData *replayData);
};
