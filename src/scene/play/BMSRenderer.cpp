//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"

#include "GameplayGeometry.h"
#include "Judge.h"
#include "bgfx/bgfx.h"
#include "../../rendering/common.h"
#include "../../utils/SpriteLoader.h"
#include "../../view/ClearLampColors.h"

#include <assert.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>

namespace {
constexpr long long kDefaultLatePoorTimingMicros = 200000LL;

uint64_t noteTextureBatchKey(bgfx::TextureHandle texture,
                             uint32_t submitDepth) {
  return (static_cast<uint64_t>(submitDepth) << 16U) |
         static_cast<uint64_t>(texture.idx);
}

long long latePoorTimingFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  const auto it = windows.find(Bad);
  return it == windows.end() ? kDefaultLatePoorTimingMicros : it->second.second;
}

bool usesBlueSymmetricKeyColor(size_t keyPosition, size_t keyLaneCount) {
  if (keyLaneCount == 0 || keyPosition >= keyLaneCount) {
    return false;
  }
  const size_t mirroredPosition =
      std::min(keyPosition, keyLaneCount - keyPosition - 1);
  return (mirroredPosition & 1U) != 0;
}

bool wasLongNoteTailReleasedEarly(const bms_parser::LongNote *head) {
  if (head == nullptr || head->Tail == nullptr || !head->Tail->IsPlayed ||
      head->Tail->Timeline == nullptr) {
    return false;
  }
  return head->Tail->PlayedTime < head->Tail->Timeline->Timing;
}
} // namespace

BMSRenderer::BMSRenderer(
    bms_parser::Chart *chart,
    const std::map<Judgement, std::pair<long long, long long>> &timingWindows,
    int visibleTimeGreenNumber, bool renderHud)
    : judgementIndicator(timingWindows),
      latePoorTiming(latePoorTimingFromWindows(timingWindows)),
      visibleTimeGreenNumber(visibleTimeGreenNumber), renderHud(renderHud),
      chart(chart) {
  scratchLaneCount = chart->Meta.GetScratchLaneCount();
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  whiteKeyLaneIndices.reserve(laneOrder.size());
  blueKeyLaneIndices.reserve(laneOrder.size());
  scratchLaneIndices.reserve(2);
  noteTextureBatchRenderers.reserve(16);
  noteTextureBatchLookup.reserve(16);
  std::vector<int> keyLanes;
  keyLanes.reserve(laneOrder.size());
  for (size_t i = 0; i < laneOrder.size(); ++i) {
    const int lane = laneOrder[i];
    laneToOrderIndex.emplace(lane, i);
    if (lane < 0) {
      continue;
    }
    const size_t laneIndex = static_cast<size_t>(lane);
    if (isScratch(lane)) {
      scratchLaneIndices.push_back(laneIndex);
    } else {
      keyLanes.push_back(lane);
    }
  }
  std::unordered_map<int, bool> laneUsesBlueSheet;
  laneUsesBlueSheet.reserve(keyLanes.size());
  for (size_t keyPosition = 0; keyPosition < keyLanes.size(); ++keyPosition) {
    const int lane = keyLanes[keyPosition];
    if (lane < 0) {
      continue;
    }
    const bool usesBlue =
        usesBlueSymmetricKeyColor(keyPosition, keyLanes.size());
    laneUsesBlueSheet.emplace(lane, usesBlue);
    const size_t laneIndex = static_cast<size_t>(lane);
    if (usesBlue) {
      blueKeyLaneIndices.push_back(laneIndex);
    } else {
      whiteKeyLaneIndices.push_back(laneIndex);
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
      appendLaneGroup(whiteKeyLaneIndices);
      appendLaneGroup(blueKeyLaneIndices);
      appendLaneGroup(scratchLaneIndices);
    }
  }
  buildTimelineScrollPositions();
  mostPrevalentBpm = calculateMostPrevalentBpm();
  SpriteLoader spriteLoader(PATH("assets/img/simple_gray.png"));
  if (!spriteLoader.load()) {
    throw std::runtime_error("Failed to load simple_gray.png");
  }
  constexpr int width = 128;
  constexpr int height = 40;
  noteImageHeight = height;
  noteImageWidth = width;
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
        if (isScratch(lane)) {
          laneSheetLookup[laneIndex] = &scratchSheet;
        } else {
          const auto colorIt = laneUsesBlueSheet.find(lane);
          laneSheetLookup[laneIndex] =
              colorIt != laneUsesBlueSheet.end() && colorIt->second ? &blueSheet
                                                                     : &graySheet;
        }
      }
    }
  }
  rebuildPlayAreaGeometry();

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

  titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  titleText->setText(chart->Meta.Title);
  titleText->setPosition(10, 10);
  titleText->setAlign(TextView::LEFT);
  judgeText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  judgeText->setPosition(rendering::window_width / 2,
                         rendering::window_height / 2);
  judgeText->setAlign(TextView::CENTER);
  scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  scoreText->setPosition(0, rendering::window_height - 50);
  scoreText->setAlign(TextView::LEFT);
  scoreText->setText("Score: 0");
  gaugeText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  gaugeText->setPosition(10, 50);
  setGaugeStatus(GaugeType::Normal, false, gaugeInitialValue(GaugeType::Normal));
  playOptionText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  playOptionText->setPosition(10, 82);
  playOptionText->setColor({255, 205, 37, 255});
  playOptionText->setVisible(false);

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
void BMSRenderer::drawTitle(RenderContext &context) const {
  titleText->render(context);
}
void BMSRenderer::drawJudgement(RenderContext context) const {
  judgeText->render(context);
}
void BMSRenderer::drawScore(RenderContext &context) const {
  scoreText->render(context);
}
void BMSRenderer::drawGauge(RenderContext &context) const {
  gaugeText->render(context);
}
void BMSRenderer::drawPlayOption(RenderContext &context) const {
  playOptionText->render(context);
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
void BMSRenderer::onJudge(JudgeResult judgeResult, int combo, int score,
                          long long displayTimeMicros,
                          bool recordTimingSample) {
  if (judgeResult.judgement == None) {
    return;
  }
  if (recordTimingSample) {
    judgementIndicator.record(judgeResult, displayTimeMicros);
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
  const bool tailReleasedEarly = wasLongNoteTailReleasedEarly(head);
  if (head->Tail->IsPlayed && !tailReleasedEarly)
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
    longBodyBatchFor(sheet, head->IsHolding)
        .addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight, 1.0f,
                 tileV, bodyTexture);
  }

  if (!tailReleasedEarly || tailY > judgeY) {
    sheetBatchFor(sheet).addRectUV(laneToX(head->Tail->Lane), tailY,
                                   noteRenderWidth, noteRenderHeight, tailUv.u0,
                                   tailUv.v0, tailUv.u1, tailUv.v1,
                                   sheet.texture);
  }

  if (head->IsPlayed)
    return;

  // Head
  sheetBatchFor(sheet).addRectUV(laneToX(head->Lane), startY, noteRenderWidth,
                                 noteRenderHeight, headUv.u0, headUv.v0,
                                 headUv.u1, headUv.v1, sheet.texture);
}
void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed)
    return;

  const NoteSheet &sheet = sheetForLane(note->Lane);

  sheetBatchFor(sheet).addRectUV(laneToX(note->Lane), y, noteRenderWidth,
                                 noteRenderHeight, sheet.note.u0,
                                 sheet.note.v0, sheet.note.u1, sheet.note.v1,
                                 sheet.texture);
}

void BMSRenderer::drawInvisibleNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed || note->IsDead) {
    return;
  }

  gimmickBatchRenderer.addRect(laneToX(note->Lane), y, noteRenderWidth,
                               noteRenderHeight,
                               Color(255, 149, 36, 224).toABGR());
}

void BMSRenderer::drawLandmineNote(float y,
                                   bms_parser::LandmineNote *const &note) {
  if (note->IsPlayed || note->IsDead) {
    return;
  }

  gimmickBatchRenderer.addRect(laneToX(note->Lane), y, noteRenderWidth,
                               noteRenderHeight,
                               Color(217, 69, 58, 232).toABGR());
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

double BMSRenderer::calculateMostPrevalentBpm() const {
  const double chartBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  if (timelines.empty()) {
    return chartBpm;
  }

  std::map<double, long long> bpmDurations;
  std::vector<double> bpmOrder;
  auto addDuration = [&](double bpm, long long durationMicros) {
    if (!std::isfinite(bpm) || bpm <= 0.0 || durationMicros <= 0) {
      return;
    }
    if (bpmDurations.find(bpm) == bpmDurations.end()) {
      bpmOrder.push_back(bpm);
    }
    bpmDurations[bpm] += durationMicros;
  };

  addDuration(chartBpm, timelines.front()->Timing);
  const long long chartEnd =
      chart != nullptr
          ? std::max({chart->Meta.TotalLength, chart->Meta.PlayLength,
                      timelines.back()->Timing})
          : timelines.back()->Timing;
  for (size_t i = 0; i < timelines.size(); ++i) {
    const auto *timeline = timelines[i];
    const long long segmentEnd =
        i + 1 < timelines.size() ? timelines[i + 1]->Timing : chartEnd;
    addDuration(timeline->Bpm, segmentEnd - timeline->Timing);
  }

  double bestBpm = chartBpm;
  long long bestDuration = 0;
  for (double bpm : bpmOrder) {
    const long long duration = bpmDurations[bpm];
    if (duration > bestDuration) {
      bestBpm = bpm;
      bestDuration = duration;
    }
  }
  return bestDuration > 0 ? bestBpm : chartBpm;
}

double BMSRenderer::visibleTimeReferenceBpm() const {
  double referenceBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  if (visibleTimeBpmStrategy ==
          AppSettings::VisibleTimeBpmStrategy::MostPrevalent &&
      std::isfinite(mostPrevalentBpm) && mostPrevalentBpm > 0.0) {
    referenceBpm = mostPrevalentBpm;
  }
  if (!std::isfinite(referenceBpm) || referenceBpm <= 0.0) {
    return 1.0;
  }
  return referenceBpm;
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

void BMSRenderer::drawReplayGhosts(float rxhs, long long currentTimeMicros,
                                   double currentScrollPosition) {
  if (replayGhostEvents.empty() || rxhs <= 0.0f) {
    return;
  }

  double firstVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteRenderHeight) /
          static_cast<double>(rxhs);
  double lastVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  if (firstVisibleScrollPosition > lastVisibleScrollPosition) {
    std::swap(firstVisibleScrollPosition, lastVisibleScrollPosition);
  }

  const auto firstVisible = std::lower_bound(
      replayGhostEvents.begin(), replayGhostEvents.end(),
      firstVisibleScrollPosition,
      [](const ReplayGhostEvent &event, double scrollPosition) {
        return event.judgeScrollPosition < scrollPosition;
      });
  const auto lastVisible = std::upper_bound(
      firstVisible, replayGhostEvents.end(), lastVisibleScrollPosition,
      [](double scrollPosition, const ReplayGhostEvent &event) {
        return scrollPosition < event.judgeScrollPosition;
      });

  for (auto it = firstVisible; it != lastVisible; ++it) {
    const auto &event = *it;
    if (event.judgeTimeMicros < currentTimeMicros) {
      continue;
    }
    const float ghostY =
        judgeY +
        static_cast<float>(event.judgeScrollPosition - currentScrollPosition) *
            rxhs;
    drawGhostNoteOutline(ghostY, event);
  }
}

void BMSRenderer::drawReplayMissMarkers(float rxhs,
                                        double currentScrollPosition) {
  if (replayMissMarkers.empty() || rxhs <= 0.0f) {
    return;
  }

  double firstVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteRenderHeight) /
          static_cast<double>(rxhs);
  double lastVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  if (firstVisibleScrollPosition > lastVisibleScrollPosition) {
    std::swap(firstVisibleScrollPosition, lastVisibleScrollPosition);
  }

  const auto firstVisible = std::lower_bound(
      replayMissMarkers.begin(), replayMissMarkers.end(),
      firstVisibleScrollPosition,
      [](const ReplayMissMarker &marker, double scrollPosition) {
        return marker.noteScrollPosition < scrollPosition;
      });
  const auto lastVisible = std::upper_bound(
      firstVisible, replayMissMarkers.end(), lastVisibleScrollPosition,
      [](double scrollPosition, const ReplayMissMarker &marker) {
        return scrollPosition < marker.noteScrollPosition;
      });

  for (auto it = firstVisible; it != lastVisible; ++it) {
    const auto &marker = *it;
    const float markerY =
        judgeY +
        static_cast<float>(marker.noteScrollPosition - currentScrollPosition) *
            rxhs;
    drawMissMarkerX(markerY, marker);
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

void BMSRenderer::drawMissMarkerX(float y, const ReplayMissMarker &marker) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
    return;
  }

  constexpr int kSteps = 7;
  const float x = laneToX(marker.lane);
  const float block =
      std::max(0.018f, std::min(noteRenderWidth, noteRenderHeight) * 0.22f);
  const float maxX = std::max(0.0f, noteRenderWidth - block);
  const float maxY = std::max(0.0f, noteRenderHeight - block);
  const uint32_t color = Color(255, 42, 42, 236).toABGR();

  for (int i = 0; i < kSteps; ++i) {
    const float t = kSteps == 1 ? 0.0f
                                : static_cast<float>(i) /
                                      static_cast<float>(kSteps - 1);
    const float yOffset = maxY * t;
    ghostBatchRenderer.addRect(x + maxX * t, y + yOffset, block, block, color);
    ghostBatchRenderer.addRect(x + maxX * (1.0f - t), y + yOffset, block, block,
                               color);
  }
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
  constexpr uint32_t kDepthLongBodies = 190;
  constexpr uint32_t kDepthNotes = 200;
  constexpr uint32_t kDepthGhosts = 250;
  constexpr uint32_t kDepthBeams = 300;
  constexpr uint32_t kDepthLaneCover = 320;
  constexpr uint32_t kDepthJudgementIndicator = 330;

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kDepthBackground);
  gimmickBatchRenderer.setSubmitView(rendering::main_view);
  gimmickBatchRenderer.setSubmitDepth(kDepthNotes + 1);
  ghostBatchRenderer.setSubmitDepth(kDepthGhosts);
  simpleBatchRenderer.begin();
  gimmickBatchRenderer.begin();
  ghostBatchRenderer.begin();
  beginNoteTextureBatches(kDepthLongBodies, kDepthNotes);
  // background
  drawRect(playAreaWidth, upperBound - judgeY, playAreaLeftX,
           judgeY, Color(20, 20, 20, 122));
  // judge line
  drawRect(playAreaWidth, noteRenderHeight, playAreaLeftX, judgeY,
           Color(255, 255, 255, 255));
  // Green number is the legacy BMS visible-time unit: 600 green = 1000 ms.
  const float visibleTimeMs = std::max(
      1.0f, static_cast<float>(visibleTimeGreenNumber) * (1000.0f / 600.0f));
  const float hispeed =
      240000.0f / static_cast<float>(visibleTimeReferenceBpm()) /
      visibleTimeMs;
  const float laneHeight = std::max(0.001f, upperBound - judgeY);
  const float hiddenRatio =
      static_cast<float>(noteStartPositionPercent) / 100.0f;
  noteVisibleUpperBound = judgeY + laneHeight * (1.0f - hiddenRatio);
  const float visibleTravelHeight =
      std::max(0.001f, noteVisibleUpperBound - judgeY);
  float rxhs = visibleTravelHeight * hispeed;
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
        drawRect(playAreaWidth, 0.05f, playAreaLeftX, y,
                 Color(255, 255, 255, 128));
      }
    } else if (timeLine->Timing >= micro - latePoorTiming) {
      y = judgeY + (micro - timeLine->Timing) /
                       static_cast<float>(latePoorTiming) * lowerBound;
    } else {
      state.currentTimelineIndex = i;
    }
    //    SDL_Log("BeatPosition: %f", timeLine->BeatPosition);
    // Render notes in grouped lane order (white/blue/scratch) to reduce texture
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
        if (note->IsLandmineNote()) {
          if (timeLine->Timing >= micro) {
            drawLandmineNote(y, static_cast<bms_parser::LandmineNote *>(note));
          }
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
        if (note->IsDead) {
          return;
        }
        if (note->IsLandmineNote()) {
          return;
        }
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
    for (const auto &note : timeLine->InvisibleNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= micro) {
        if (showInvisibleNotes) {
          drawInvisibleNote(y, note);
        }
      } else {
        note->IsDead = true;
      }
    }
    for (const auto &note : timeLine->LandmineNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= micro) {
        drawLandmineNote(y, note);
      }
    }
  }

  // render leftover long notes
  for (const auto &pair : longNoteLookahead) {
    drawLongNote(pair.second, upperBound, pair.first);
  }
  drawReplayGhosts(rxhs, micro, currentScrollPosition);
  drawReplayMissMarkers(rxhs, currentScrollPosition);

  // Flush background/measure pass before notes.
  simpleBatchRenderer.flush();
  flushNoteTextureBatches();
  gimmickBatchRenderer.flush();
  ghostBatchRenderer.flush();

  if (renderLaneBeams) {
    simpleBatchRenderer.setSubmitDepth(kDepthBeams);
    const long long nowMicros =
        useRenderTimeForLaneBeams
            ? micro
            : std::chrono::duration_cast<std::chrono::microseconds>(
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

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kDepthLaneCover);
  simpleBatchRenderer.begin();
  drawLaneCover();
  simpleBatchRenderer.flush();

  if (judgementIndicator.isEnabled()) {
    const bool indicatorHudMode = judgementIndicator.isHudMode();
    simpleBatchRenderer.setSubmitView(indicatorHudMode ? rendering::ui_view
                                                       : rendering::main_view);
    simpleBatchRenderer.setSubmitDepth(indicatorHudMode
                                           ? 0
                                           : kDepthJudgementIndicator);
    simpleBatchRenderer.begin();
    judgementIndicator.render(simpleBatchRenderer, micro,
                              {.judgeY = judgeY,
                               .upperBound = upperBound,
                               .playAreaLeftX = playAreaLeftX,
                               .playAreaWidth = playAreaWidth,
                               .noteRenderWidth = noteRenderWidth,
                               .noteRenderHeight = noteRenderHeight});
    simpleBatchRenderer.flush();
    simpleBatchRenderer.setSubmitView(rendering::main_view);
  }

  if (renderHud) {
    drawTitle(context);
    drawJudgement(context);
    drawScore(context);
    drawGauge(context);
    drawPlayOption(context);
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

void BMSRenderer::reset() {
  state.reset();
  judgementIndicator.clear();
}

void BMSRenderer::refreshGeometry() {
  upperBound = calculateLanePlaneScreenTopIntersection();
  noteVisibleUpperBound = upperBound;
}

void BMSRenderer::setVisibleTimeGreenNumber(int greenNumber) {
  visibleTimeGreenNumber = greenNumber;
}

void BMSRenderer::setVisibleTimeBpmStrategy(
    AppSettings::VisibleTimeBpmStrategy strategy) {
  visibleTimeBpmStrategy = strategy;
}

void BMSRenderer::setPlayAreaWidth(float width) {
  if (!std::isfinite(width)) {
    width = AppSettings::kDefaultPlayAreaWidth;
  }
  const float sanitized =
      std::clamp(width, AppSettings::kMinPlayAreaWidth,
                 AppSettings::kMaxPlayAreaWidth);
  if (std::abs(sanitized - playAreaWidth) <= 0.001f) {
    return;
  }
  playAreaWidth = sanitized;
  rebuildPlayAreaGeometry();
}

void BMSRenderer::setLaneBeamsEnabled(bool enabled) {
  renderLaneBeams = enabled;
}

void BMSRenderer::setLaneBeamLengthPercent(int percent) {
  laneBeamLengthPercent =
      std::clamp(percent, AppSettings::kMinLaneBeamLengthPercent,
                 AppSettings::kMaxLaneBeamLengthPercent);
}

void BMSRenderer::setNoteStartPositionPercent(int percent) {
  noteStartPositionPercent =
      std::clamp(percent, AppSettings::kMinNoteStartPositionPercent,
                 AppSettings::kMaxNoteStartPositionPercent);
}

void BMSRenderer::setLaneBeamClockUsesRenderTime(bool enabled) {
  useRenderTimeForLaneBeams = enabled;
}

void BMSRenderer::setShowInvisibleNotes(bool enabled) {
  showInvisibleNotes = enabled;
}

void BMSRenderer::setJudgementIndicatorConfig(bool enabled, float y,
                                              float widthScale, bool hudMode) {
  judgementIndicator.configure(enabled, y, widthScale, hudMode);
}

void BMSRenderer::setGaugeStatus(GaugeType gaugeType, bool gaugeAutoShift,
                                 float currentGauge) {
  if (gaugeText == nullptr) {
    return;
  }

  char text[96];
  std::snprintf(text, sizeof(text), "%s: %s %.1f%%",
                gaugeAutoShift ? "GAS" : "Gauge",
                gaugeTypeToShortLabel(gaugeType), currentGauge);
  gaugeText->setText(text);

  const Color color = clearLampColorForRank(gaugeTypeToClearRank(gaugeType));
  gaugeText->setColor({color.r, color.g, color.b, 255});
}

void BMSRenderer::setPlayOptionStatus(const std::string &label) {
  if (playOptionText == nullptr) {
    return;
  }

  playOptionText->setVisible(!label.empty());
  playOptionText->setText(label);
}

void BMSRenderer::setReplayData(const ReplayData *replayData) {
  replayGhostEvents.clear();
  replayMissMarkers.clear();
  if (replayData == nullptr) {
    return;
  }

  std::vector<const bms_parser::TimeLine *> timelineRefs;
  timelineRefs.reserve(timelines.size());
  for (const auto *timeline : timelines) {
    timelineRefs.push_back(timeline);
  }
  replayGhostEvents = replay_ghost::buildReplayGhostEvents(
      *replayData, timelineRefs, laneToOrderIndex,
      [this](long long timeMicros) { return scrollPositionAtTime(timeMicros); });
  replayMissMarkers = replay_ghost::buildReplayMissMarkers(
      *replayData, timelineRefs, laneToOrderIndex,
      [this](long long timeMicros) { return scrollPositionAtTime(timeMicros); });
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
  const float beamScale = static_cast<float>(laneBeamLengthPercent) / 100.0f;
  const float beamHeight = std::max(0.0f, upperBound - judgeY) * beamScale;
  if (beamHeight <= 0.0f) {
    return;
  }
  drawRect(noteRenderWidth, beamHeight, laneToX(lane), judgeY, color);
}

void BMSRenderer::drawLaneCover() {
  const float coverHeight = upperBound - noteVisibleUpperBound;
  if (coverHeight <= 0.001f) {
    return;
  }

  drawRect(playAreaWidth, coverHeight, playAreaLeftX, noteVisibleUpperBound,
           Color(9, 12, 18, 255));

  const float edgeHeight = std::max(0.025f, noteRenderHeight * 0.12f);
  drawRect(playAreaWidth, edgeHeight, playAreaLeftX,
           noteVisibleUpperBound - edgeHeight * 0.5f,
           Color(214, 224, 236, 255));
}

inline bool BMSRenderer::isLeftScratch(int lane) const {
  return scratchLaneCount > 0 && lane == 7;
}
inline bool BMSRenderer::isRightScratch(int lane) const {
  return scratchLaneCount > 1 && lane == 15;
}
inline bool BMSRenderer::isScratch(int lane) const {
  return isLeftScratch(lane) || isRightScratch(lane);
}
void BMSRenderer::rebuildPlayAreaGeometry() {
  playAreaLeftX = gameplay_geometry::playAreaLeft(playAreaWidth);
  noteRenderWidth =
      laneOrder.empty()
          ? gameplay_geometry::standardNoteWidth(playAreaWidth)
          : playAreaWidth / static_cast<float>(laneOrder.size());
  if (noteImageWidth > 0.0f) {
    noteRenderHeight = static_cast<float>(noteImageHeight) /
                       static_cast<float>(noteImageWidth) * noteRenderWidth;
    constexpr float kLongBodyOffImageHeight = 12.0f;
    constexpr float kLongBodyOnImageHeight = 24.0f;
    longBodyRenderHeightOff =
        kLongBodyOffImageHeight / static_cast<float>(noteImageWidth) *
        noteRenderWidth;
    longBodyRenderHeightOn =
        kLongBodyOnImageHeight / static_cast<float>(noteImageWidth) *
        noteRenderWidth;
  }
  for (int lane : laneOrder) {
    if (lane < 0 || static_cast<size_t>(lane) >= laneXLookup.size()) {
      continue;
    }
    laneXLookup[static_cast<size_t>(lane)] = computeLaneX(lane);
  }
}
inline float BMSRenderer::computeLaneX(int lane) const {
  if (const auto it = laneToOrderIndex.find(lane);
      it != laneToOrderIndex.end()) {
    return playAreaLeftX + static_cast<float>(it->second) * noteRenderWidth;
  }

  return playAreaLeftX;
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
  return graySheet;
}

rendering::TexBatchRenderer &BMSRenderer::noteTextureBatch(
    bgfx::TextureHandle texture, uint32_t submitDepth) {
  const uint64_t key = noteTextureBatchKey(texture, submitDepth);
  if (const auto it = noteTextureBatchLookup.find(key);
      it != noteTextureBatchLookup.end()) {
    return noteTextureBatchRenderers[it->second];
  }

  const size_t index = noteTextureBatchRenderers.size();
  auto &renderer = noteTextureBatchRenderers.emplace_back();
  renderer.setSubmitDepth(submitDepth);
  renderer.begin();
  noteTextureBatchLookup.emplace(key, index);
  return renderer;
}

rendering::TexBatchRenderer &BMSRenderer::sheetBatchFor(
    const NoteSheet &sheet) {
  return noteTextureBatch(sheet.texture, noteSheetSubmitDepth);
}

rendering::TexBatchRenderer &BMSRenderer::longBodyBatchFor(
    const NoteSheet &sheet, bool isHolding) {
  return noteTextureBatch(isHolding ? sheet.longBodyOnTexture
                                    : sheet.longBodyOffTexture,
                          longBodySubmitDepth);
}

void BMSRenderer::beginNoteTextureBatches(uint32_t bodyDepth,
                                          uint32_t sheetDepth) {
  longBodySubmitDepth = bodyDepth;
  noteSheetSubmitDepth = sheetDepth;
  for (auto &renderer : noteTextureBatchRenderers) {
    renderer.begin();
  }
}

void BMSRenderer::flushNoteTextureBatches() {
  for (auto &renderer : noteTextureBatchRenderers) {
    renderer.flush();
  }
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
  delete titleText;
  delete judgeText;
  delete scoreText;
  delete gaugeText;
  delete playOptionText;
}
