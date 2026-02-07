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
#include <list>
#include <mutex>

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
enum ObjectType {
  Note,
  LongBody,
};
class BMSRendererState {
public:
  ~BMSRendererState() = default;
  std::list<bms_parser::LongNote *>
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
  std::map<int, LaneState> laneStates;

  float noteImageHeight = 0;
  float noteImageWidth = 0;
  std::vector<bms_parser::TimeLine *> timelines;
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

  rendering::SimpleBatchRenderer simpleBatchRenderer;
  rendering::TexBatchRenderer texBatchRenderer;

  void drawRect(float width, float height, float x, float y, Color color);
  void drawLaneBeam(int lane, const long long time);
  void drawJudgement(RenderContext context) const;
  void drawScore(RenderContext &context) const;
  void drawLongNote(float headY, float tailY,
                    bms_parser::LongNote *const &head);
  void drawNormalNote(float y, bms_parser::Note *const &note);
  bgfx::TextureHandle loadCroppedTexture(SpriteLoader &loader, int x, int y,
                                         int width, int height,
                                         const char *label);
  bool isLeftScratch(int lane);
  bool isRightScratch(int lane);
  bool isScratch(int lane);
  float laneToX(int lane);
  float calculateLanePlaneScreenTopIntersection();
  bgfx::TextureHandle noteTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle noteTexture2 = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longHeadTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyTextureOn = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyTextureOff = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longTailTexture = BGFX_INVALID_HANDLE;

  bgfx::TextureHandle longHeadTexture2 = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyTextureOn2 = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longBodyTextureOff2 = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle longTailTexture2 = BGFX_INVALID_HANDLE;

  bgfx::TextureHandle scratchTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle scratchLongHeadTexture = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle scratchLongBodyTextureOn = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle scratchLongBodyTextureOff = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle scratchLongTailTexture = BGFX_INVALID_HANDLE;
  bms_parser::Chart *chart;

public:
  void onLanePressed(int lane, const JudgeResult judge, long long time);
  void onLaneReleased(int lane, long long time);
  void onJudge(JudgeResult judgeResult, int combo, int score);
  explicit BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming);

  void render(RenderContext &context, long long micro);
  void reset();
};
