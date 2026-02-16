//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"

#include "Judge.h"
#include "bgfx/bgfx.h"
#include "../../rendering/common.h"
#include "stb_image.h"
#include "../../utils/SpriteLoader.h"

#include <assert.h>
#include <string>
#include <unordered_map>
BMSRenderer::BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming)
    : latePoorTiming(latePoorTiming), chart(chart) {
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStates.reserve(laneOrder.size());
  for (int lane : laneOrder) {
    laneStates.emplace(lane, LaneState{});
  }
  // flatten timeline
  size_t timelineCount = 0;
  for (const auto &measure : chart->Measures) {
    timelineCount += measure->TimeLines.size();
  }
  timelines.reserve(timelineCount);
  for (const auto &measure : chart->Measures) {
    for (const auto &timeLine : measure->TimeLines) {
      timelines.push_back(timeLine);
    }
  }
  SpriteLoader spriteLoader(PATH("assets/img/simple_gray.png"));
  if (!spriteLoader.load()) {
    throw std::runtime_error("Failed to load simple_gray.png");
  }

  // int width, height, channels;
  // unsigned char *data =
  //     stbi_load("assets/img/note.png", &width, &height, &channels, 4);
  // if (!data) {
  //   SDL_Log("Failed to load note texture");
  //   throw std::runtime_error("Failed to load note texture");
  // }
  int width = 128;
  int height = 40;
  keyLaneCount = chart->Meta.GetKeyLaneCount();
  noteRenderWidth = 1.0f * 8.0f / chart->Meta.GetTotalLaneCount();
  noteImageHeight = height;
  noteImageWidth = width;
  noteRenderHeight = static_cast<float>(noteImageHeight) /
                     static_cast<float>(noteImageWidth) * noteRenderWidth;
  float offImageHeight = 12.0f;
  float onImageHeight = 24.0f;

  longBodyRenderHeightOff = static_cast<float>(offImageHeight) /
                            static_cast<float>(width) * noteRenderWidth;
  longBodyRenderHeightOn = static_cast<float>(onImageHeight) /
                           static_cast<float>(width) * noteRenderWidth;
  noteTexture = loadCroppedTexture(spriteLoader, 0, 0, width, height, "note");
  longHeadTexture =
      loadCroppedTexture(spriteLoader, 0, 80, 128, 40, "long head");
  longBodyTextureOff =
      loadCroppedTexture(spriteLoader, 0, 120, 128, 12, "long body off");
  longBodyTextureOn =
      loadCroppedTexture(spriteLoader, 0, 132, 128, 24, "long body on");
  longTailTexture =
      loadCroppedTexture(spriteLoader, 0, 40, 128, 40, "long tail");

  SpriteLoader spriteLoader2(PATH("assets/img/simple_blue.png"));
  if (!spriteLoader2.load()) {
    throw std::runtime_error("Failed to load simple_blue.png");
  }
  noteTexture2 = loadCroppedTexture(spriteLoader2, 0, 0, width, height, "note");
  longHeadTexture2 =
      loadCroppedTexture(spriteLoader2, 0, 80, 128, 40, "long head");
  longBodyTextureOff2 =
      loadCroppedTexture(spriteLoader2, 0, 120, 128, 12, "long body off");
  longBodyTextureOn2 =
      loadCroppedTexture(spriteLoader2, 0, 132, 128, 24, "long body on");
  longTailTexture2 =
      loadCroppedTexture(spriteLoader2, 0, 40, 128, 40, "long tail");
  SpriteLoader spriteLoader3(PATH("assets/img/orange.png"));
  if (!spriteLoader3.load()) {
    throw std::runtime_error("Failed to load orange.png");
  }
  scratchTexture =
      loadCroppedTexture(spriteLoader3, 0, 0, width, height, "scratch");
  scratchLongHeadTexture =
      loadCroppedTexture(spriteLoader3, 0, 80, 128, 40, "scratch long head");
  scratchLongBodyTextureOff = loadCroppedTexture(spriteLoader3, 0, 120, 128, 12,
                                                 "scratch long body off");
  scratchLongBodyTextureOn = loadCroppedTexture(spriteLoader3, 0, 132, 128, 24,
                                                "scratch long body on");
  scratchLongTailTexture =
      loadCroppedTexture(spriteLoader3, 0, 40, 128, 40, "scratch long tail");
  judgeText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  judgeText->setPosition(rendering::window_width / 2,
                         rendering::window_height / 2);
  judgeText->setAlign(TextView::CENTER);
  scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  scoreText->setPosition(0, rendering::window_height - 50);
  scoreText->setAlign(TextView::LEFT);

  // Calculate the lane plane screen top intersection
  upperBound = calculateLanePlaneScreenTopIntersection();
}

bgfx::TextureHandle BMSRenderer::loadCroppedTexture(SpriteLoader &loader, int x,
                                                    int y, int width,
                                                    int height,
                                                    const char *label) {
  auto data = loader.crop(x, y, width, height);
  if (!data) {
    SDL_Log("Failed to load %s texture", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  int channels = loader.getChannels();
  auto handle =
      bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8,
                            0, bgfx::copy(data, width * height * channels));
  SDL_free(data);
  return handle;
}
void BMSRenderer::drawJudgement(RenderContext context) const {
  judgeText->render(context);
}
void BMSRenderer::drawScore(RenderContext &context) const {
  scoreText->render(context);
}

void BMSRenderer::onLanePressed(int lane, const JudgeResult judge,
                                long long time) {
  std::lock_guard<std::mutex> lock(laneMutex);
  auto [it, inserted] = laneStates.try_emplace(lane);
  LaneState &laneState = it->second;
  laneState.isPressed = true;
  laneState.lastPressedJudge = judge;
  laneState.lastStateTime = time;
  if (inserted) {
    laneOrder.push_back(lane);
  }
}

void BMSRenderer::onLaneReleased(int lane, long long time) {
  std::lock_guard<std::mutex> lock(laneMutex);
  auto [it, inserted] = laneStates.try_emplace(lane);
  LaneState &laneState = it->second;
  laneState.isPressed = false;
  laneState.lastStateTime = time;
  if (inserted) {
    laneOrder.push_back(lane);
  }
}
void BMSRenderer::onJudge(JudgeResult judgeResult, int combo, int score) {
  if (judgeResult.judgement == None) {
    return;
  }
  state.latestJudgeResult = judgeResult;
  state.latestJudgeResultTime = std::chrono::system_clock::now();
  state.latestCombo = combo;
  state.latestScore = score;

  std::string judgeLine = judgeResult.toString();
  if (combo > 0) {
    judgeLine.push_back(' ');
    judgeLine += std::to_string(combo);
  }
  judgeText->setText(judgeLine);
  scoreText->setText("Score: " + std::to_string(score));
}
void BMSRenderer::drawLongNote(float headY, float tailY,
                               bms_parser::LongNote *const &head) {
  // assert head
  assert(!head->IsTail() && "head is tail");
  if (head->Tail->IsPlayed)
    return;
  float startY = head->IsPlayed ? judgeY : headY;
  const float bodyHeight = tailY - startY;
  const float bodyWidth = noteRenderWidth;

  // Body
  float tileV = bodyHeight / (head->IsHolding ? longBodyRenderHeightOn
                                              : longBodyRenderHeightOff);
  bgfx::TextureHandle bodyTexture{};
  bgfx::TextureHandle tailTexture{};
  bgfx::TextureHandle headTexture{};
  if (isScratch(head->Lane)) {
    headTexture = scratchLongHeadTexture;
    tailTexture = scratchLongTailTexture;
    if (head->IsHolding) {
      bodyTexture = scratchLongBodyTextureOn;
    } else {
      bodyTexture = scratchLongBodyTextureOff;
    }
  } else {
    headTexture = head->Lane % 2 == 0 ? longHeadTexture : longHeadTexture2;
    tailTexture = head->Lane % 2 == 0 ? longTailTexture : longTailTexture2;
    if (head->IsHolding) {
      bodyTexture =
          head->Lane % 2 == 0 ? longBodyTextureOn : longBodyTextureOn2;
    } else {
      bodyTexture =
          head->Lane % 2 == 0 ? longBodyTextureOff : longBodyTextureOff2;
    }
  }

  texBatchRenderer.addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight,
                           1.0f, tileV, bodyTexture);

  // Tail
  texBatchRenderer.addRect(laneToX(head->Tail->Lane), tailY, noteRenderWidth,
                           noteRenderHeight, 1.0f, 1.0f, tailTexture);

  if (head->IsPlayed)
    return;

  // Head
  texBatchRenderer.addRect(laneToX(head->Lane), startY, noteRenderWidth,
                           noteRenderHeight, 1.0f, 1.0f, headTexture);
}
void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed)
    return;

  const auto &texture =
      isScratch(note->Lane)
          ? scratchTexture
          : (note->Lane % 2 == 0 ? noteTexture : noteTexture2);

  texBatchRenderer.addRect(laneToX(note->Lane), y, noteRenderWidth,
                           noteRenderHeight, 1.0f, 1.0f, texture);
}

float BMSRenderer::calculateLanePlaneScreenTopIntersection() {
  // Get the camera from the rendering context
  Camera &camera = rendering::game_camera;

  // Screen top in screen coordinates (Y=0 is top of screen)
  float screenTopY = 0.0f;
  float screenCenterX = rendering::window_width / 2.0f;

  // Get camera position from camera
  bx::Vec3 eye = camera.getEye();

  // Deproject the screen top center to get a point in world space
  float testDistance = 5.0f;
  bx::Vec3 screenTopWorld =
      camera.deproject(screenCenterX, screenTopY, testDistance);

  SDL_Log("Camera eye: (%.2f, %.2f, %.2f)", eye.x, eye.y, eye.z);
  SDL_Log("Screen top world: (%.2f, %.2f, %.2f)", screenTopWorld.x,
          screenTopWorld.y, screenTopWorld.z);

  // Calculate ray direction from camera to screen top
  bx::Vec3 rayDir = {screenTopWorld.x - eye.x, screenTopWorld.y - eye.y,
                     screenTopWorld.z - eye.z};
  float rayLength = bx::length(rayDir);
  rayDir = {rayDir.x / rayLength, rayDir.y / rayLength, rayDir.z / rayLength};

  SDL_Log("Ray direction: (%.2f, %.2f, %.2f)", rayDir.x, rayDir.y, rayDir.z);

  // The lane plane is parallel to X-axis at z=0 (facing the camera)
  // We need to find where the ray from camera intersects this plane
  // Ray equation: eye + t * rayDir
  // At intersection: eye.z + t * rayDir.z = 0

  // Check if ray direction is nearly parallel to the lane plane (z=0)
  if (std::abs(rayDir.z) < 0.001f) {
    SDL_Log(
        "Warning: Ray is nearly parallel to lane plane, using fallback value");
    return 8.5f; // Fallback to original hardcoded value
  }

  // Solve for t where z=0: eye.z + t * rayDir.z = 0
  float t = -eye.z / rayDir.z;

  SDL_Log("Calculated t: %.2f", t);

  // Check if intersection is behind camera
  if (t < 0) {
    SDL_Log("Warning: Intersection is behind camera, using fallback value");
    return 8.5f; // Fallback to original hardcoded value
  }

  // Calculate the intersection point
  bx::Vec3 intersection = {eye.x + t * rayDir.x, eye.y + t * rayDir.y,
                           eye.z + t * rayDir.z};

  SDL_Log("Intersection point: (%.2f, %.2f, %.2f)", intersection.x,
          intersection.y, intersection.z);

  // Verify that z is actually 0 at intersection
  float actualZ = eye.z + t * rayDir.z;
  SDL_Log("Actual Z at intersection: %.6f", actualZ);

  return intersection.y;
}

void BMSRenderer::render(RenderContext &context, long long micro) {
  simpleBatchRenderer.begin();
  texBatchRenderer.begin();
  // background
  drawRect(8.0f, upperBound - judgeY, 0.0f, judgeY, Color(20, 20, 20, 122));
  // judge line
  drawRect(8.0f, noteRenderHeight, 0.0f, judgeY, Color(255, 255, 255, 255));
  float greenNumber = 400.0f;
  float hispeed =
      240000.0f / chart->Meta.Bpm / greenNumber *
      0.6f; // 0.6: need to convert green number to milliseconds.
            // (300green = 0.5s)
            // ... / ( greenNumber / 0.6f ) = ... / (greenNumber * 0.6f)
  float visibleLaneBottom = judgeY;
  float rxhs = (upperBound - visibleLaneBottom) * hispeed;
  float y = judgeY;
  std::unordered_map<bms_parser::LongNote *, float> longNoteLookahead;
  longNoteLookahead.reserve(state.orphanLongNotes.size() + 16);
  for (auto *orphanLongNote : state.orphanLongNotes) {
    longNoteLookahead[orphanLongNote] = lowerBound;
  }
  // render timeline
  for (size_t i = state.currentTimelineIndex;
       i < timelines.size() && y < upperBound; i++) {
    const auto &timeLine = timelines[i];
    if (timeLine->Timing >= micro) {
      if (y < judgeY)
        y = judgeY;
      if (i > 0) {
        if (const auto &prevTimeLine = timelines[i - 1];
            prevTimeLine->Timing + prevTimeLine->GetStopDuration() > micro) {
          // when the previous timeline is stopped
          y += (timeLine->BeatPosition - prevTimeLine->BeatPosition) *
               prevTimeLine->Scroll * rxhs;
        } else {
          y += (timeLine->BeatPosition - prevTimeLine->BeatPosition) *
               prevTimeLine->Scroll * (timeLine->Timing - micro) /
               (timeLine->Timing - prevTimeLine->Timing -
                prevTimeLine->GetStopDuration()) *
               rxhs;
        }
      } else {
        y += timeLine->BeatPosition * (timeLine->Timing - micro) /
             timeLine->Timing * rxhs;
      }

      if (timeLine->IsFirstInMeasure) {
        // render measure line
        drawRect(8.0f, 0.05f, 0.0f, y, Color(255, 255, 255, 128));
      }
    } else if (timeLine->Timing >= micro - latePoorTiming) {
      y = judgeY + (micro - timeLine->Timing) /
                       static_cast<float>(latePoorTiming) * lowerBound;
    } else {
      state.currentTimelineIndex = i;
    }
    //    SDL_Log("BeatPosition: %f", timeLine->BeatPosition);
    // render notes
    for (const auto &note : timeLine->Notes) {
      if (note != nullptr) {
        if (timeLine->Timing >= micro - latePoorTiming) {
          // note is in the hittable timing
          if (note->IsDead) {
            continue;
          }
          // render note
          if (note->IsLongNote()) {
            auto *longNote = static_cast<bms_parser::LongNote *>(note);
            if (longNote->IsTail()) {
              if (longNote->Head == nullptr) {
                // ignore malformed chart: long note is not terminated
                continue;
              }
              // find head's y
              if (auto it = longNoteLookahead.find(longNote->Head);
                  it != longNoteLookahead.end()) {
                drawLongNote(it->second, y, longNote->Head);
                // remove from lookahead
                longNoteLookahead.erase(longNote->Head);
              } else {
                drawLongNote(lowerBound, y, longNote->Head);
              }
            } else {
              longNoteLookahead[longNote] = y;
            }
          } else {
            drawNormalNote(y, note);
          }
        } else {
          // note has passed the last hittable timing
          if (note->IsLongNote()) {
            auto *longNote = static_cast<bms_parser::LongNote *>(note);
            if (longNote->IsTail()) {
              if (longNote->Head == nullptr) {
                // ignore malformed chart: long note is not terminated
                continue;
              }
              // remove from orphan long note
              state.orphanLongNotes.erase(longNote->Head);
              // and from long note lookahead
              longNoteLookahead.erase(longNote->Head);
            } else {
              // add to orphan long note
              state.orphanLongNotes.insert(longNote);

              // setting to lowerBound in all cases is OK because the played
              // state will be correctly handled by drawLongNote
              longNoteLookahead[longNote] = lowerBound;
            }
          }
        }
      }
    }
    // render landmine notes
    for (const auto &note : timeLine->LandmineNotes) {
      if (note != nullptr) {
        // render note
      }
    }
  }

  // render leftover long notes
  for (const auto &pair : longNoteLookahead) {
    drawLongNote(pair.second, upperBound, pair.first);
  }

  // Flush batch renderer so that lines are drawn first
  // Actually, I was adding notes to texBatch and lines to simpleBatch.
  // I should flush simpleBatch first to draw lines behind notes.
  simpleBatchRenderer.flush();
  texBatchRenderer.flush();

  // render lane beams
  const long long nowMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  {
    std::lock_guard<std::mutex> lock(laneMutex);
    for (int lane : laneOrder) {
      const auto it = laneStates.find(lane);
      if (it == laneStates.end()) {
        continue;
      }
      drawLaneBeam(lane, it->second, nowMicros);
    }
  }
  simpleBatchRenderer.flush();

  // render judgement
  drawJudgement(context);
  drawScore(context);
}
void BMSRenderer::reset() { state.reset(); }
void BMSRenderer::drawRect(float width, float height, float x, float y,
                           Color color) {
  simpleBatchRenderer.addRect(x, y, width, height, color.toABGR());
}
void BMSRenderer::drawLaneBeam(int lane, const LaneState &laneState,
                               const long long time) {
  if (laneState.lastStateTime == -1) {
    return;
  }
  // alpha
  double alpha;
  if (laneState.isPressed) {
    alpha = 0.2;
  } else {
    // fade out
    alpha = 0.2 - (time - laneState.lastStateTime) / 1000000.0 / 1.0;
  }
  if (alpha <= 0.0) {
    return;
  }
  if (alpha > 1.0) {
    alpha = 1.0;
  }
  auto color = Color(255, 255, 255, 255 * alpha);

  if (laneState.lastPressedJudge.judgement == PGreat) {
    color = Color(255, 128, 0, 255 * alpha);
  } else if (laneState.lastPressedJudge.judgement == None) {
    color = Color(255, 255, 255, 255 * alpha);
  } else {
    color = laneState.lastPressedJudge.Diff > 0 ? Color(255, 0, 0, 255 * alpha)
                                                : Color(0, 0, 255, 255 * alpha);
  }
  drawRect(noteRenderWidth, 10.0f, laneToX(lane), 0.0f, color);
}

inline bool BMSRenderer::isLeftScratch(int lane) { return lane == 7; }
inline bool BMSRenderer::isRightScratch(int lane) { return lane == 15; }
inline bool BMSRenderer::isScratch(int lane) {
  return isLeftScratch(lane) || isRightScratch(lane);
}
inline float BMSRenderer::laneToX(int lane) {
  if (isLeftScratch(lane)) {
    return 0.0f;
  }
  if (lane >= 8) {
    lane -=
        keyLaneCount == 14
            ? 1
            : (isRightScratch(lane) ? 5
                                    : 3); // skip left scratch index (7), since
                                          // 7 is already placed in the leftmost
  }

  return (lane + 1) * noteRenderWidth;
}
void BMSRendererState::reset() {
  orphanLongNotes.clear();
  currentTimelineIndex = 0;
  latestJudgeResult = JudgeResult(None, 0);
  latestJudgeResultTime = std::chrono::system_clock::now();
  latestCombo = 0;
  latestScore = 0;
}
BMSRenderer::~BMSRenderer() {
  if (bgfx::isValid(noteTexture)) {
    bgfx::destroy(noteTexture);
  }
  if (bgfx::isValid(noteTexture2)) {
    bgfx::destroy(noteTexture2);
  }
  if (bgfx::isValid(longHeadTexture)) {
    bgfx::destroy(longHeadTexture);
  }
  if (bgfx::isValid(longBodyTextureOn)) {
    bgfx::destroy(longBodyTextureOn);
  }
  if (bgfx::isValid(longBodyTextureOff)) {
    bgfx::destroy(longBodyTextureOff);
  }
  if (bgfx::isValid(longTailTexture)) {
    bgfx::destroy(longTailTexture);
  }
  if (bgfx::isValid(longHeadTexture2)) {
    bgfx::destroy(longHeadTexture2);
  }
  if (bgfx::isValid(longBodyTextureOn2)) {
    bgfx::destroy(longBodyTextureOn2);
  }
  if (bgfx::isValid(longBodyTextureOff2)) {
    bgfx::destroy(longBodyTextureOff2);
  }
  if (bgfx::isValid(longTailTexture2)) {
    bgfx::destroy(longTailTexture2);
  }
  if (bgfx::isValid(scratchTexture)) {
    bgfx::destroy(scratchTexture);
  }
  if (bgfx::isValid(scratchLongHeadTexture)) {
    bgfx::destroy(scratchLongHeadTexture);
  }
  if (bgfx::isValid(scratchLongBodyTextureOn)) {
    bgfx::destroy(scratchLongBodyTextureOn);
  }
  if (bgfx::isValid(scratchLongBodyTextureOff)) {
    bgfx::destroy(scratchLongBodyTextureOff);
  }
  if (bgfx::isValid(scratchLongTailTexture)) {
    bgfx::destroy(scratchLongTailTexture);
  }
  delete judgeText;
  delete scoreText;
}
