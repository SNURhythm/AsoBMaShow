//
// Created by XF on 9/2/2024.
//

#pragma once

#include "../../ReplayData.h"
#include "../../view/View.h"
#include "../../bms_parser.hpp"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../rendering/TexBatchRenderer.h"
#include "../../view/TextView.h"
#include "../../rendering/Color.h"
#include "../../rendering/Camera.h"
#include "Judge.h"
#include <bx/math.h>
#include <chrono>
#include <cstdint>
#include <mutex>
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

struct ReplayGhostEvent {
  int lane = -1;
  long long noteTimeMicros = 0;
  long long judgeTimeMicros = 0;
  Judgement judgement = None;
};

class BMSRendererState {
public:
  ~BMSRendererState() = default;
  std::unordered_set<bms_parser::LongNote *>
      orphanLongNotes; // long note whose head is dead but tail is alive
  size_t currentTimelineIndex = 0;
  JudgeResult latestJudgeResult = JudgeResult(None, 0);
  std::chrono::system_clock::time_point latestJudgeResultTime;
  int latestCombo = 0;
  int latestScore = 0;
  void reset();
};
class BMSRenderer {
public:
  ~BMSRenderer();

private:
  TextView *titleText = nullptr;
  TextView *judgeText = nullptr;
  TextView *scoreText = nullptr;
  TextView *gaugeText = nullptr;
  std::mutex hudMutex;
  bool hudDirty = false;
  std::string pendingJudgeText;
  int pendingScore = 0;
  std::mutex laneMutex;
  std::vector<int> laneOrder;
  std::vector<LaneState> laneStatesByOrder;
  std::unordered_map<int, size_t> laneToOrderIndex;
  std::vector<std::pair<int, LaneState>> laneStateSnapshot;
  std::vector<float> laneXLookup;
  std::vector<const NoteSheet *> laneSheetLookup;
  std::vector<size_t> evenKeyLaneIndices;
  std::vector<size_t> oddKeyLaneIndices;
  std::vector<size_t> scratchLaneIndices;

  float noteImageHeight = 0;
  float noteImageWidth = 0;
  std::vector<bms_parser::TimeLine *> timelines;
  std::vector<std::vector<bms_parser::Note *>> groupedTimelineNotes;
  std::vector<std::vector<ReplayGhostEvent>> groupedReplayGhostEvents;
  std::vector<double> timelineScrollPositions;
  std::unordered_map<bms_parser::LongNote *, float> longNoteLookaheadScratch;
  BMSRendererState state;
  int scratchLaneCount = 0;
  float noteRenderWidth = 1.0f;
  float noteRenderHeight = 1.0f;

  float longBodyRenderHeightOff = 1.0f;
  float longBodyRenderHeightOn = 1.0f;
  float lowerBound = -1.0f;
  float upperBound = 10.0f; // Calculated from camera projection
  float judgeY = 0.0f;
  long long latePoorTiming;
  int visibleTimeGreenNumber = 400;
  bool renderHud = true;
  bool renderLaneBeams = true;
  bool useRenderTimeForLaneBeams = false;

  rendering::SimpleBatchRenderer simpleBatchRenderer;
  rendering::SimpleBatchRenderer ghostBatchRenderer;
  rendering::TexBatchRenderer texBatchRenderer;

  void drawRect(float width, float height, float x, float y, Color color);
  void drawLaneBeam(int lane, const LaneState &laneState, long long time);
  void drawTitle(RenderContext &context) const;
  void drawJudgement(RenderContext context) const;
  void drawScore(RenderContext &context) const;
  void drawGauge(RenderContext &context) const;
  void drawLongNote(float headY, float tailY,
                    bms_parser::LongNote *const &head);
  void drawNormalNote(float y, bms_parser::Note *const &note);
  void drawReplayGhosts(size_t timelineIndex, float rxhs,
                        long long currentTimeMicros,
                        double currentScrollPosition);
  void drawGhostNoteOutline(float y, const ReplayGhostEvent &event);
  void buildTimelineScrollPositions();
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
  float laneToX(int lane) const;
  const NoteSheet &sheetForLane(int lane) const;
  float calculateLanePlaneScreenTopIntersection();
  NoteSheet graySheet;
  NoteSheet blueSheet;
  NoteSheet scratchSheet;
  bms_parser::Chart *chart;

public:
  void onLanePressed(int lane, const JudgeResult judge, long long time);
  void onLaneReleased(int lane, long long time);
  void onJudge(JudgeResult judgeResult, int combo, int score);
  explicit BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming,
                       int visibleTimeGreenNumber, bool renderHud = true);

  void render(RenderContext &context, long long micro);
  void reset();
  void refreshGeometry();
  void setVisibleTimeGreenNumber(int greenNumber);
  void setLaneBeamsEnabled(bool enabled);
  void setLaneBeamClockUsesRenderTime(bool enabled);
  void setGaugeStatus(GaugeType gaugeType, bool gaugeAutoShift,
                      float currentGauge);
  void setReplayData(const ReplayData *replayData);
};
