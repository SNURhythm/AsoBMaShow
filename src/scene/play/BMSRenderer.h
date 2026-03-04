//
// Created by XF on 9/2/2024.
//

#pragma once

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
#include <unordered_map>
#include <unordered_set>
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
  TextView *judgeText = nullptr;
  TextView *scoreText = nullptr;
  std::mutex laneMutex;
  std::unordered_map<int, LaneState> laneStates;
  std::vector<int> laneOrder;
  std::vector<int> evenKeyLanes;
  std::vector<int> oddKeyLanes;
  std::vector<int> scratchLanes;

  float noteImageHeight = 0;
  float noteImageWidth = 0;
  std::vector<bms_parser::TimeLine *> timelines;
  std::unordered_map<bms_parser::LongNote *, float> longNoteLookaheadScratch;
  BMSRendererState state;
  int keyLaneCount;
  float noteRenderWidth = 1.0f;
  float noteRenderHeight = 1.0f;

  float longBodyRenderHeightOff = 1.0f;
  float longBodyRenderHeightOn = 1.0f;
  float lowerBound = -1.0f;
  float upperBound = 10.0f; // Calculated from camera projection
  float judgeY = 0.0f;
  long long latePoorTiming;
  uint64_t lastBatchTelemetryTick = 0;

  rendering::SimpleBatchRenderer simpleBatchRenderer;
  rendering::TexBatchRenderer texBatchRenderer;

  void drawRect(float width, float height, float x, float y, Color color);
  void drawLaneBeam(int lane, const LaneState &laneState, long long time);
  void drawJudgement(RenderContext context) const;
  void drawScore(RenderContext &context) const;
  void drawLongNote(float headY, float tailY,
                    bms_parser::LongNote *const &head);
  void drawNormalNote(float y, bms_parser::Note *const &note);
  bgfx::TextureHandle loadSheetTexture(SpriteLoader &loader, const char *label);
  bgfx::TextureHandle loadCroppedTexture(SpriteLoader &loader, int x, int y,
                                         int width, int height,
                                         const char *label);
  bool isLeftScratch(int lane);
  bool isRightScratch(int lane);
  bool isScratch(int lane);
  float laneToX(int lane);
  float calculateLanePlaneScreenTopIntersection();
  NoteSheet graySheet;
  NoteSheet blueSheet;
  NoteSheet scratchSheet;
  bms_parser::Chart *chart;

public:
  void onLanePressed(int lane, const JudgeResult judge, long long time);
  void onLaneReleased(int lane, long long time);
  void onJudge(JudgeResult judgeResult, int combo, int score);
  explicit BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming);

  void render(RenderContext &context, long long micro);
  void reset();
};
