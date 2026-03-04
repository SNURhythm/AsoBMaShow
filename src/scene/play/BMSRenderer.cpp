//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"

#include "Judge.h"
#include "bgfx/bgfx.h"
#include "../../rendering/common.h"
#include "../../utils/SpriteLoader.h"

#include <assert.h>
#include <cmath>
#include <string>
BMSRenderer::BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming)
    : latePoorTiming(latePoorTiming), chart(chart) {
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  evenKeyLanes.reserve(laneOrder.size());
  oddKeyLanes.reserve(laneOrder.size());
  scratchLanes.reserve(2);
  for (size_t i = 0; i < laneOrder.size(); ++i) {
    const int lane = laneOrder[i];
    laneToOrderIndex.emplace(lane, i);
    if (isScratch(lane)) {
      scratchLanes.push_back(lane);
    } else if ((lane & 1) == 0) {
      evenKeyLanes.push_back(lane);
    } else {
      oddKeyLanes.push_back(lane);
    }
  }
  state.orphanLongNotes.reserve(laneOrder.size() * 2);
  longNoteLookaheadScratch.reserve(laneOrder.size() * 2);
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
  constexpr int width = 128;
  constexpr int height = 40;
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

  SpriteLoader spriteLoader2(PATH("assets/img/simple_blue.png"));
  if (!spriteLoader2.load()) {
    throw std::runtime_error("Failed to load simple_blue.png");
  }

  SpriteLoader spriteLoader3(PATH("assets/img/orange.png"));
  if (!spriteLoader3.load()) {
    throw std::runtime_error("Failed to load orange.png");
  }

  graySheet.texture = loadSheetTexture(spriteLoader, "simple_gray");
  blueSheet.texture = loadSheetTexture(spriteLoader2, "simple_blue");
  scratchSheet.texture = loadSheetTexture(spriteLoader3, "orange");
  graySheet.longBodyOffTexture =
      loadCroppedTexture(spriteLoader, 0, 120, 128, 12, "gray long body off");
  graySheet.longBodyOnTexture =
      loadCroppedTexture(spriteLoader, 0, 132, 128, 24, "gray long body on");
  blueSheet.longBodyOffTexture =
      loadCroppedTexture(spriteLoader2, 0, 120, 128, 12, "blue long body off");
  blueSheet.longBodyOnTexture =
      loadCroppedTexture(spriteLoader2, 0, 132, 128, 24, "blue long body on");
  scratchSheet.longBodyOffTexture = loadCroppedTexture(
      spriteLoader3, 0, 120, 128, 12, "scratch long body off");
  scratchSheet.longBodyOnTexture = loadCroppedTexture(
      spriteLoader3, 0, 132, 128, 24, "scratch long body on");

  auto makeUv = [](int x, int y, int w, int h, int textureW, int textureH) {
    NoteUvRegion uv{};
    uv.u0 = static_cast<float>(x) / static_cast<float>(textureW);
    uv.v0 = static_cast<float>(y) / static_cast<float>(textureH);
    uv.u1 = static_cast<float>(x + w) / static_cast<float>(textureW);
    uv.v1 = static_cast<float>(y + h) / static_cast<float>(textureH);
    return uv;
  };

  auto configureSheet = [&](NoteSheet &sheet, int textureW, int textureH) {
    sheet.note = makeUv(0, 0, 128, 40, textureW, textureH);
    sheet.longTail = makeUv(0, 40, 128, 40, textureW, textureH);
    sheet.longHead = makeUv(0, 80, 128, 40, textureW, textureH);
    sheet.longBodyOff = makeUv(0, 120, 128, 12, textureW, textureH);
    sheet.longBodyOn = makeUv(0, 132, 128, 24, textureW, textureH);
  };

  configureSheet(graySheet, spriteLoader.getWidth(), spriteLoader.getHeight());
  configureSheet(blueSheet, spriteLoader2.getWidth(), spriteLoader2.getHeight());
  configureSheet(scratchSheet, spriteLoader3.getWidth(),
                 spriteLoader3.getHeight());

  judgeText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  judgeText->setPosition(rendering::window_width / 2,
                         rendering::window_height / 2);
  judgeText->setAlign(TextView::CENTER);
  scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  scoreText->setPosition(0, rendering::window_height - 50);
  scoreText->setAlign(TextView::LEFT);
  scoreText->setText("Score: 0");

  // Calculate the lane plane screen top intersection
  upperBound = calculateLanePlaneScreenTopIntersection();
}

bgfx::TextureHandle BMSRenderer::loadSheetTexture(SpriteLoader &loader,
                                                  const char *label) {
  if (!loader.isLoaded() || loader.getData() == nullptr) {
    SDL_Log("Failed to load %s texture: image is not loaded", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  const int width = loader.getWidth();
  const int height = loader.getHeight();
  if (width <= 0 || height <= 0) {
    SDL_Log("Failed to load %s texture: invalid dimensions", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  constexpr int kBytesPerPixel = 4; // stbi_load(..., 4) in SpriteLoader
  const auto handle = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(loader.getData(), width * height * kBytesPerPixel));
  if (!bgfx::isValid(handle)) {
    SDL_Log("Failed to create bgfx texture for %s", label);
    throw std::runtime_error(std::string("Failed to create texture for ") +
                             label);
  }
  return handle;
}

bgfx::TextureHandle BMSRenderer::loadCroppedTexture(SpriteLoader &loader, int x,
                                                    int y, int width,
                                                    int height,
                                                    const char *label) {
  auto *data = loader.crop(x, y, width, height);
  if (data == nullptr) {
    SDL_Log("Failed to load %s texture", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  constexpr int kBytesPerPixel = 4;
  const auto handle = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(data, width * height * kBytesPerPixel));
  SDL_free(data);
  if (!bgfx::isValid(handle)) {
    SDL_Log("Failed to create %s texture", label);
    throw std::runtime_error(std::string("Failed to create ") + label +
                             " texture");
  }
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
  const auto it = laneToOrderIndex.find(lane);
  if (it == laneToOrderIndex.end()) {
    return;
  }
  LaneState &laneState = laneStatesByOrder[it->second];
  laneState.isPressed = true;
  laneState.lastPressedJudge = judge;
  laneState.lastStateTime = time;
}

void BMSRenderer::onLaneReleased(int lane, long long time) {
  std::lock_guard<std::mutex> lock(laneMutex);
  const auto it = laneToOrderIndex.find(lane);
  if (it == laneToOrderIndex.end()) {
    return;
  }
  LaneState &laneState = laneStatesByOrder[it->second];
  laneState.isPressed = false;
  laneState.lastStateTime = time;
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

  const NoteSheet *sheet = nullptr;
  if (isScratch(head->Lane)) {
    sheet = &scratchSheet;
  } else {
    sheet = (head->Lane % 2 == 0) ? &graySheet : &blueSheet;
  }
  const NoteUvRegion &headUv = sheet->longHead;
  const NoteUvRegion &tailUv = sheet->longTail;
  const auto bodyTexture =
      head->IsHolding ? sheet->longBodyOnTexture : sheet->longBodyOffTexture;

  // Body
  if (bodyHeight > 0.0f && bgfx::isValid(bodyTexture)) {
    float tileV = bodyHeight / (head->IsHolding ? longBodyRenderHeightOn
                                                : longBodyRenderHeightOff);
    texBatchRenderer.addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight,
                             1.0f, tileV, bodyTexture);
  }

  // Tail
  texBatchRenderer.addRectUV(laneToX(head->Tail->Lane), tailY, noteRenderWidth,
                             noteRenderHeight, tailUv.u0, tailUv.v0,
                             tailUv.u1, tailUv.v1, sheet->texture);

  if (head->IsPlayed)
    return;

  // Head
  texBatchRenderer.addRectUV(laneToX(head->Lane), startY, noteRenderWidth,
                             noteRenderHeight, headUv.u0, headUv.v0, headUv.u1,
                             headUv.v1, sheet->texture);
}
void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed)
    return;

  const NoteSheet &sheet =
      isScratch(note->Lane)
          ? scratchSheet
          : ((note->Lane % 2 == 0) ? graySheet : blueSheet);

  texBatchRenderer.addRectUV(laneToX(note->Lane), y, noteRenderWidth,
                             noteRenderHeight, sheet.note.u0, sheet.note.v0,
                             sheet.note.u1, sheet.note.v1, sheet.texture);
}

float BMSRenderer::calculateLanePlaneScreenTopIntersection() {
  Camera &camera = rendering::game_camera;
  constexpr float kFallbackLaneTop = 8.5f;

  const float screenTopY = 0.0f;
  const float screenCenterX = rendering::window_width / 2.0f;
  const bx::Vec3 eye = camera.getEye();
  const bx::Vec3 screenTopWorld =
      camera.deproject(screenCenterX, screenTopY, 5.0f);

  bx::Vec3 rayDir = {screenTopWorld.x - eye.x, screenTopWorld.y - eye.y,
                     screenTopWorld.z - eye.z};
  const float rayLength = bx::length(rayDir);
  if (rayLength <= 0.0001f) {
    return kFallbackLaneTop;
  }
  rayDir = {rayDir.x / rayLength, rayDir.y / rayLength, rayDir.z / rayLength};

  if (std::abs(rayDir.z) < 0.001f) {
    return kFallbackLaneTop;
  }

  const float t = -eye.z / rayDir.z;
  if (t < 0.0f) {
    return kFallbackLaneTop;
  }

  return eye.y + t * rayDir.y;
}

void BMSRenderer::render(RenderContext &context, long long micro) {
  constexpr uint32_t kDepthBackground = 100;
  constexpr uint32_t kDepthNotes = 200;
  constexpr uint32_t kDepthBeams = 300;

  simpleBatchRenderer.setSubmitDepth(kDepthBackground);
  texBatchRenderer.setSubmitDepth(kDepthNotes);
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
  auto &longNoteLookahead = longNoteLookaheadScratch;
  longNoteLookahead.clear();
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
    // Render notes in grouped lane order (even/odd/scratch) to reduce texture
    // switches while keeping per-lane ordering intact.
    auto processNote = [&](bms_parser::Note *note) {
      if (note == nullptr) {
        return;
      }
      if (timeLine->Timing >= micro - latePoorTiming) {
        // note is in the hittable timing
        if (note->IsDead) {
          return;
        }
        // render note
        if (note->IsLongNote()) {
          auto *longNote = static_cast<bms_parser::LongNote *>(note);
          if (longNote->IsTail()) {
            if (longNote->Head == nullptr) {
              // ignore malformed chart: long note is not terminated
              return;
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
              return;
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
    };

    auto processLaneGroup = [&](const std::vector<int> &laneGroup) {
      const size_t noteCount = timeLine->Notes.size();
      for (int lane : laneGroup) {
        if (lane < 0) {
          continue;
        }
        const size_t laneIndex = static_cast<size_t>(lane);
        if (laneIndex >= noteCount) {
          continue;
        }
        processNote(timeLine->Notes[laneIndex]);
      }
    };

    processLaneGroup(evenKeyLanes);
    processLaneGroup(oddKeyLanes);
    processLaneGroup(scratchLanes);
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

  // Flush background/measure pass before notes.
  simpleBatchRenderer.flush();
  texBatchRenderer.flush();

  // render lane beams
  simpleBatchRenderer.setSubmitDepth(kDepthBeams);
  const long long nowMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  laneStateSnapshot.clear();
  {
    std::lock_guard<std::mutex> lock(laneMutex);
    for (size_t i = 0; i < laneOrder.size(); ++i) {
      laneStateSnapshot.emplace_back(laneOrder[i], laneStatesByOrder[i]);
    }
  }
  for (const auto &entry : laneStateSnapshot) {
    drawLaneBeam(entry.first, entry.second, nowMicros);
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
  if (bgfx::isValid(graySheet.texture)) {
    bgfx::destroy(graySheet.texture);
  }
  if (bgfx::isValid(graySheet.longBodyOffTexture)) {
    bgfx::destroy(graySheet.longBodyOffTexture);
  }
  if (bgfx::isValid(graySheet.longBodyOnTexture)) {
    bgfx::destroy(graySheet.longBodyOnTexture);
  }
  if (bgfx::isValid(blueSheet.texture)) {
    bgfx::destroy(blueSheet.texture);
  }
  if (bgfx::isValid(blueSheet.longBodyOffTexture)) {
    bgfx::destroy(blueSheet.longBodyOffTexture);
  }
  if (bgfx::isValid(blueSheet.longBodyOnTexture)) {
    bgfx::destroy(blueSheet.longBodyOnTexture);
  }
  if (bgfx::isValid(scratchSheet.texture)) {
    bgfx::destroy(scratchSheet.texture);
  }
  if (bgfx::isValid(scratchSheet.longBodyOffTexture)) {
    bgfx::destroy(scratchSheet.longBodyOffTexture);
  }
  if (bgfx::isValid(scratchSheet.longBodyOnTexture)) {
    bgfx::destroy(scratchSheet.longBodyOnTexture);
  }
  delete judgeText;
  delete scoreText;
}
