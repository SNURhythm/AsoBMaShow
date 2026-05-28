//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"

#include "Judge.h"
#include "bgfx/bgfx.h"
#include "../../rendering/common.h"
#include "../../utils/SpriteLoader.h"

#include <assert.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
BMSRenderer::BMSRenderer(bms_parser::Chart *chart, long long latePoorTiming,
                         int visibleTimeGreenNumber, bool renderHud)
    : latePoorTiming(latePoorTiming), chart(chart),
      visibleTimeGreenNumber(visibleTimeGreenNumber), renderHud(renderHud) {
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  evenKeyLaneIndices.reserve(laneOrder.size());
  oddKeyLaneIndices.reserve(laneOrder.size());
  scratchLaneIndices.reserve(2);
  for (size_t i = 0; i < laneOrder.size(); ++i) {
    const int lane = laneOrder[i];
    laneToOrderIndex.emplace(lane, i);
    if (lane < 0) {
      continue;
    }
    const size_t laneIndex = static_cast<size_t>(lane);
    if (isScratch(lane)) {
      scratchLaneIndices.push_back(laneIndex);
    } else if ((lane & 1) == 0) {
      evenKeyLaneIndices.push_back(laneIndex);
    } else {
      oddKeyLaneIndices.push_back(laneIndex);
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
  groupedTimelineNotes.reserve(timelineCount);
  for (const auto &measure : chart->Measures) {
    for (const auto &timeLine : measure->TimeLines) {
      timelines.push_back(timeLine);
      auto &timelineNotes = groupedTimelineNotes.emplace_back();
      timelineNotes.reserve(laneOrder.size());
      auto appendLaneGroup = [&](const std::vector<size_t> &laneGroup) {
        for (size_t laneIndex : laneGroup) {
          if (laneIndex >= timeLine->Notes.size()) {
            continue;
          }
          if (auto *note = timeLine->Notes[laneIndex]; note != nullptr) {
            timelineNotes.push_back(note);
          }
        }
      };
      appendLaneGroup(evenKeyLaneIndices);
      appendLaneGroup(oddKeyLaneIndices);
      appendLaneGroup(scratchLaneIndices);
    }
  }
  buildTimelineScrollPositions();
  groupedReplayGhostEvents.resize(timelines.size());
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
  if (!laneOrder.empty()) {
    const int maxLane = *std::max_element(laneOrder.begin(), laneOrder.end());
    if (maxLane >= 0) {
      laneXLookup.assign(static_cast<size_t>(maxLane + 1),
                         std::numeric_limits<float>::quiet_NaN());
      laneSheetLookup.assign(static_cast<size_t>(maxLane + 1), nullptr);
      for (int lane : laneOrder) {
        if (lane < 0) {
          continue;
        }
        const size_t laneIndex = static_cast<size_t>(lane);
        laneXLookup[laneIndex] = computeLaneX(lane);
        laneSheetLookup[laneIndex] =
            isScratch(lane) ? &scratchSheet
                            : ((lane % 2 == 0) ? &graySheet : &blueSheet);
      }
    }
  }
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
  configureSheet(blueSheet, spriteLoader2.getWidth(),
                 spriteLoader2.getHeight());
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

  refreshGeometry();
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
  {
    std::lock_guard<std::mutex> lock(hudMutex);
    pendingJudgeText = std::move(judgeLine);
    pendingScore = score;
    hudDirty = true;
  }
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

  const NoteSheet &sheet = sheetForLane(head->Lane);
  const NoteUvRegion &headUv = sheet.longHead;
  const NoteUvRegion &tailUv = sheet.longTail;
  const auto bodyTexture =
      head->IsHolding ? sheet.longBodyOnTexture : sheet.longBodyOffTexture;

  // Body
  if (bodyHeight > 0.0f && bgfx::isValid(bodyTexture)) {
    float tileV = bodyHeight / (head->IsHolding ? longBodyRenderHeightOn
                                                : longBodyRenderHeightOff);
    texBatchRenderer.addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight,
                             1.0f, tileV, bodyTexture);
  }

  // Tail
  texBatchRenderer.addRectUV(laneToX(head->Tail->Lane), tailY, noteRenderWidth,
                             noteRenderHeight, tailUv.u0, tailUv.v0, tailUv.u1,
                             tailUv.v1, sheet.texture);

  if (head->IsPlayed)
    return;

  // Head
  texBatchRenderer.addRectUV(laneToX(head->Lane), startY, noteRenderWidth,
                             noteRenderHeight, headUv.u0, headUv.v0, headUv.u1,
                             headUv.v1, sheet.texture);
}
void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed)
    return;

  const NoteSheet &sheet = sheetForLane(note->Lane);

  texBatchRenderer.addRectUV(laneToX(note->Lane), y, noteRenderWidth,
                             noteRenderHeight, sheet.note.u0, sheet.note.v0,
                             sheet.note.u1, sheet.note.v1, sheet.texture);
}

void BMSRenderer::buildTimelineScrollPositions() {
  timelineScrollPositions.clear();
  timelineScrollPositions.reserve(timelines.size());
  if (timelines.empty()) {
    return;
  }

  double position = timelines.front()->BeatPosition;
  timelineScrollPositions.push_back(position);
  for (size_t i = 1; i < timelines.size(); ++i) {
    const auto *prevTimeline = timelines[i - 1];
    const auto *timeline = timelines[i];
    position += (timeline->BeatPosition - prevTimeline->BeatPosition) *
                prevTimeline->Scroll;
    timelineScrollPositions.push_back(position);
  }
}

double BMSRenderer::scrollPositionAtTime(long long timeMicros) const {
  if (timelines.empty()) {
    return 0.0;
  }

  const auto timelineIt = std::lower_bound(
      timelines.begin(), timelines.end(), timeMicros,
      [](const bms_parser::TimeLine *timeline, long long timing) {
        return timeline->Timing < timing;
      });

  if (timelineIt == timelines.begin()) {
    const auto *timeline = timelines.front();
    if (timeline->Timing <= 0) {
      return timelineScrollPositions.front();
    }
    const double progress =
        std::clamp(static_cast<double>(timeMicros) /
                       static_cast<double>(timeline->Timing),
                   0.0, 1.0);
    return timelineScrollPositions.front() * progress;
  }

  if (timelineIt == timelines.end()) {
    return timelineScrollPositions.back();
  }

  const size_t timelineIndex =
      static_cast<size_t>(std::distance(timelines.begin(), timelineIt));
  const auto *timeline = timelines[timelineIndex];
  if (timeline->Timing == timeMicros) {
    return timelineScrollPositions[timelineIndex];
  }

  const size_t prevTimelineIndex = timelineIndex - 1;
  const auto *prevTimeline = timelines[prevTimelineIndex];
  const long long stopDuration =
      static_cast<long long>(prevTimeline->GetStopDuration());
  const long long stopEnd = prevTimeline->Timing + stopDuration;
  if (timeMicros <= stopEnd) {
    return timelineScrollPositions[prevTimelineIndex];
  }

  const long long scrollDuration =
      timeline->Timing - prevTimeline->Timing - stopDuration;
  if (scrollDuration <= 0) {
    return timelineScrollPositions[timelineIndex];
  }

  const double progress =
      std::clamp(static_cast<double>(timeMicros - stopEnd) /
                     static_cast<double>(scrollDuration),
                 0.0, 1.0);
  return timelineScrollPositions[prevTimelineIndex] +
         (timelineScrollPositions[timelineIndex] -
          timelineScrollPositions[prevTimelineIndex]) *
             progress;
}

void BMSRenderer::drawReplayGhosts(size_t timelineIndex, float rxhs,
                                   long long currentTimeMicros,
                                   double currentScrollPosition) {
  if (timelineIndex >= groupedReplayGhostEvents.size()) {
    return;
  }

  for (const auto &event : groupedReplayGhostEvents[timelineIndex]) {
    if (event.judgeTimeMicros < currentTimeMicros) {
      continue;
    }
    const double eventScrollPosition =
        scrollPositionAtTime(event.judgeTimeMicros);
    const float ghostY =
        judgeY +
        static_cast<float>(eventScrollPosition - currentScrollPosition) * rxhs;
    drawGhostNoteOutline(ghostY, event);
  }
}

void BMSRenderer::drawGhostNoteOutline(float y, const ReplayGhostEvent &event) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
    return;
  }

  Color color(255, 255, 255, 220);
  if (event.judgement != PGreat) {
    color = event.judgeTimeMicros < event.noteTimeMicros
                ? Color(0, 96, 255, 220)
                : Color(255, 40, 40, 220);
  }

  const float x = laneToX(event.lane);
  const float thickness = std::max(0.015f, noteRenderHeight * 0.12f);
  const uint32_t abgr = color.toABGR();
  ghostBatchRenderer.addRect(x, y, noteRenderWidth, thickness, abgr);
  ghostBatchRenderer.addRect(x, y + noteRenderHeight - thickness,
                             noteRenderWidth, thickness, abgr);
  ghostBatchRenderer.addRect(x, y, thickness, noteRenderHeight, abgr);
  ghostBatchRenderer.addRect(x + noteRenderWidth - thickness, y, thickness,
                             noteRenderHeight, abgr);
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
  applyPendingHudText();

  constexpr uint32_t kDepthBackground = 100;
  constexpr uint32_t kDepthNotes = 200;
  constexpr uint32_t kDepthGhosts = 250;
  constexpr uint32_t kDepthBeams = 300;

  simpleBatchRenderer.setSubmitDepth(kDepthBackground);
  ghostBatchRenderer.setSubmitDepth(kDepthGhosts);
  texBatchRenderer.setSubmitDepth(kDepthNotes);
  simpleBatchRenderer.begin();
  ghostBatchRenderer.begin();
  texBatchRenderer.begin();
  // background
  drawRect(8.0f, upperBound - judgeY, 0.0f, judgeY, Color(20, 20, 20, 122));
  // judge line
  drawRect(8.0f, noteRenderHeight, 0.0f, judgeY, Color(255, 255, 255, 255));
  // Green number is the legacy BMS visible-time unit: 600 green = 1000 ms.
  const float visibleTimeMs = std::max(
      1.0f, static_cast<float>(visibleTimeGreenNumber) * (1000.0f / 600.0f));
  float hispeed = 240000.0f / chart->Meta.Bpm / visibleTimeMs;
  float visibleLaneBottom = judgeY;
  float rxhs = (upperBound - visibleLaneBottom) * hispeed;
  float y = judgeY;
  const double currentScrollPosition = scrollPositionAtTime(micro);
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

    if (i < groupedTimelineNotes.size()) {
      for (auto *note : groupedTimelineNotes[i]) {
        processNote(note);
      }
    }
    drawReplayGhosts(i, rxhs, micro, currentScrollPosition);
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
  ghostBatchRenderer.flush();

  if (renderLaneBeams) {
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
  }

  if (renderHud) {
    drawJudgement(context);
    drawScore(context);
  }
}

void BMSRenderer::applyPendingHudText() {
  std::string judgeLine;
  int score = 0;
  {
    std::lock_guard<std::mutex> lock(hudMutex);
    if (!hudDirty) {
      return;
    }
    judgeLine = pendingJudgeText;
    score = pendingScore;
    hudDirty = false;
  }
  judgeText->setText(judgeLine);
  scoreText->setText("Score: " + std::to_string(score));
}

void BMSRenderer::reset() { state.reset(); }

void BMSRenderer::refreshGeometry() {
  upperBound = calculateLanePlaneScreenTopIntersection();
}

void BMSRenderer::setVisibleTimeGreenNumber(int greenNumber) {
  visibleTimeGreenNumber = greenNumber;
}

void BMSRenderer::setLaneBeamsEnabled(bool enabled) {
  renderLaneBeams = enabled;
}

void BMSRenderer::setReplayData(const ReplayData *replayData) {
  groupedReplayGhostEvents.clear();
  groupedReplayGhostEvents.resize(timelines.size());
  if (replayData == nullptr) {
    return;
  }

  for (const auto &event : replayData->events) {
    if ((event.action != ReplayEventAction::Press &&
         event.action != ReplayEventAction::Release) ||
        event.judgement == None || event.noteTimeMicros < 0) {
      continue;
    }

    const auto timelineIt = std::lower_bound(
        timelines.begin(), timelines.end(), event.noteTimeMicros,
        [](const bms_parser::TimeLine *timeline, long long timing) {
          return timeline->Timing < timing;
        });
    if (timelineIt == timelines.end() ||
        (*timelineIt)->Timing != event.noteTimeMicros) {
      continue;
    }

    const size_t timelineIndex =
        static_cast<size_t>(std::distance(timelines.begin(), timelineIt));
    groupedReplayGhostEvents[timelineIndex].push_back({
        .lane = event.lane,
        .noteTimeMicros = event.noteTimeMicros,
        .judgeTimeMicros = event.judgeTimeMicros,
        .judgement = event.judgement,
    });
  }
}

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

inline bool BMSRenderer::isLeftScratch(int lane) const { return lane == 7; }
inline bool BMSRenderer::isRightScratch(int lane) const { return lane == 15; }
inline bool BMSRenderer::isScratch(int lane) const {
  return isLeftScratch(lane) || isRightScratch(lane);
}
inline float BMSRenderer::computeLaneX(int lane) const {
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
inline float BMSRenderer::laneToX(int lane) const {
  if (lane >= 0 && static_cast<size_t>(lane) < laneXLookup.size()) {
    const float cachedX = laneXLookup[static_cast<size_t>(lane)];
    if (!std::isnan(cachedX)) {
      return cachedX;
    }
  }
  return computeLaneX(lane);
}
inline const NoteSheet &BMSRenderer::sheetForLane(int lane) const {
  if (lane >= 0 && static_cast<size_t>(lane) < laneSheetLookup.size()) {
    if (const auto *sheet = laneSheetLookup[static_cast<size_t>(lane)];
        sheet != nullptr) {
      return *sheet;
    }
  }
  if (isScratch(lane)) {
    return scratchSheet;
  }
  return (lane % 2 == 0) ? graySheet : blueSheet;
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
