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
#include <cstdlib>
#include <limits>
#include <string>

namespace {
constexpr long long kDefaultLatePoorTimingMicros = 200000LL;
constexpr long long kDefaultJudgementIndicatorRangeMicros = 500000LL;
constexpr long long kJudgementIndicatorFadeMicros = 1800000LL;
constexpr float kJudgementIndicatorWorldWidthRatio = 0.34f;
constexpr float kJudgementIndicatorHudWidthRatio = 0.2f;
constexpr float kJudgementIndicatorHudMinWidth = 220.0f;
constexpr float kJudgementIndicatorHudMaxWidth = 360.0f;

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "Judgement indicator sequence atomic must be lock-free");
static_assert(std::atomic<long long>::is_always_lock_free,
              "Judgement indicator timing atomic must be lock-free");
static_assert(std::atomic<int>::is_always_lock_free,
              "Judgement indicator value atomic must be lock-free");

uint64_t noteTextureBatchKey(bgfx::TextureHandle texture,
                             uint32_t submitDepth) {
  return (static_cast<uint64_t>(submitDepth) << 16U) |
         static_cast<uint64_t>(texture.idx);
}

long long timingWindowEarlyFrom(
    const std::map<Judgement, std::pair<long long, long long>> &windows,
    Judgement judgement, long long fallback) {
  const auto it = windows.find(judgement);
  return it == windows.end() ? fallback : it->second.first;
}

long long timingWindowLateFrom(
    const std::map<Judgement, std::pair<long long, long long>> &windows,
    Judgement judgement, long long fallback) {
  const auto it = windows.find(judgement);
  return it == windows.end() ? fallback : it->second.second;
}

long long latePoorTimingFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  return timingWindowLateFrom(windows, Bad, kDefaultLatePoorTimingMicros);
}

long long judgementIndicatorRangeFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  long long range = kDefaultJudgementIndicatorRangeMicros;
  for (Judgement judgement : {PGreat, Great, Good, Bad, Kpoor}) {
    const auto it = windows.find(judgement);
    if (it == windows.end()) {
      continue;
    }
    range = std::max(range, std::llabs(it->second.first));
    range = std::max(range, std::llabs(it->second.second));
  }
  return std::max(1LL, range);
}

uint8_t alphaByte(float alpha) {
  return static_cast<uint8_t>(
      std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255));
}

Color judgementColor(Judgement judgement, uint8_t alpha) {
  switch (judgement) {
  case PGreat:
    return Color(200, 255, 255, alpha);
  case Great:
    return Color(200, 255, 200, alpha);
  case Good:
    return Color(200, 200, 255, alpha);
  case Bad:
    return Color(255, 200, 200, alpha);
  case Kpoor:
    return Color(255, 50, 50, alpha);
  case Poor:
    return Color(255, 100, 100, alpha);
  case None:
  case JudgementCount:
    break;
  }
  return Color(255, 255, 255, alpha);
}
} // namespace

BMSRenderer::BMSRenderer(
    bms_parser::Chart *chart,
    const std::map<Judgement, std::pair<long long, long long>> &timingWindows,
    int visibleTimeGreenNumber, bool renderHud)
    : timingWindows(timingWindows),
      latePoorTiming(latePoorTimingFromWindows(timingWindows)),
      judgementIndicatorRangeMicros(
          judgementIndicatorRangeFromWindows(timingWindows)),
      visibleTimeGreenNumber(visibleTimeGreenNumber), renderHud(renderHud),
      chart(chart) {
  scratchLaneCount = chart->Meta.GetScratchLaneCount();
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  evenKeyLaneIndices.reserve(laneOrder.size());
  oddKeyLaneIndices.reserve(laneOrder.size());
  scratchLaneIndices.reserve(2);
  noteTextureBatchRenderers.reserve(16);
  noteTextureBatchLookup.reserve(16);
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
  SpriteLoader spriteLoader(PATH("assets/img/simple_gray.png"));
  if (!spriteLoader.load()) {
    throw std::runtime_error("Failed to load simple_gray.png");
  }
  constexpr int width = 128;
  constexpr int height = 40;
  noteRenderWidth =
      laneOrder.empty()
          ? gameplay_geometry::kStandardNoteWidth
          : gameplay_geometry::kPlayAreaWidth /
                static_cast<float>(laneOrder.size());
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

JudgementIndicatorLayout
BMSRenderer::judgementIndicatorLayout(bool hudMode) const {
  const float normalizedY =
      static_cast<float>(
          std::clamp(judgementIndicatorYPermille.load(std::memory_order_relaxed),
                     0, 1000)) /
      1000.0f;

  JudgementIndicatorLayout layout{};
  if (hudMode) {
    layout.width = std::clamp(
        static_cast<float>(rendering::window_width) *
            kJudgementIndicatorHudWidthRatio,
        kJudgementIndicatorHudMinWidth, kJudgementIndicatorHudMaxWidth);
    layout.x = (static_cast<float>(rendering::window_width) - layout.width) *
               0.5f;
    layout.barHeight = 5.0f;
    layout.markerHeight = 18.0f;
    layout.markerWidth = 2.0f;
    layout.centerY = static_cast<float>(rendering::window_height) *
                     (1.0f - normalizedY);
    layout.centerY =
        std::clamp(layout.centerY, layout.markerHeight * 0.5f,
                   static_cast<float>(rendering::window_height) -
                       layout.markerHeight * 0.5f);
    return layout;
  }

  layout.width =
      gameplay_geometry::kPlayAreaWidth * kJudgementIndicatorWorldWidthRatio;
  layout.x = gameplay_geometry::kPlayAreaCenterX - layout.width * 0.5f;
  const float laneHeight = std::max(0.1f, upperBound - judgeY);
  layout.centerY = judgeY + laneHeight * normalizedY;
  layout.barHeight = std::max(0.02f, noteRenderHeight * 0.08f);
  layout.markerHeight = std::max(0.16f, noteRenderHeight * 0.5f);
  layout.markerWidth = std::max(0.01f, noteRenderWidth * 0.014f);
  return layout;
}

float BMSRenderer::judgementOffsetToX(
    long long diffMicros, const JudgementIndicatorLayout &layout) const {
  const long long clampedDiff =
      std::clamp(diffMicros, -judgementIndicatorRangeMicros,
                 judgementIndicatorRangeMicros);
  const float normalized =
      static_cast<float>(clampedDiff) /
      static_cast<float>(judgementIndicatorRangeMicros);
  return layout.x + layout.width * 0.5f + normalized * layout.width * 0.5f;
}

long long BMSRenderer::timingWindowEarly(Judgement judgement) const {
  return timingWindowEarlyFrom(timingWindows, judgement, 0);
}

long long BMSRenderer::timingWindowLate(Judgement judgement) const {
  return timingWindowLateFrom(timingWindows, judgement, 0);
}

void BMSRenderer::clearJudgementIndicatorSamples() {
  judgementIndicatorWriteSequence.store(0, std::memory_order_release);
  for (auto &sample : judgementIndicatorSamples) {
    sample.sequence.store(0, std::memory_order_release);
    sample.diffMicros.store(0, std::memory_order_relaxed);
    sample.createdTimeMicros.store(0, std::memory_order_relaxed);
    sample.judgement.store(None, std::memory_order_relaxed);
  }
}

bool BMSRenderer::readJudgementIndicatorSample(
    uint64_t sequence, JudgementIndicatorSample &sample) const {
  const auto &slot =
      judgementIndicatorSamples[sequence % kJudgementIndicatorMaxVisibleSamples];
  const uint64_t expectedSequence = sequence + 1;
  const uint64_t firstSequence =
      slot.sequence.load(std::memory_order_acquire);
  if (firstSequence != expectedSequence) {
    return false;
  }

  JudgementIndicatorSample snapshot{
      .diffMicros = slot.diffMicros.load(std::memory_order_relaxed),
      .createdTimeMicros =
          slot.createdTimeMicros.load(std::memory_order_relaxed),
      .judgement =
          static_cast<Judgement>(slot.judgement.load(std::memory_order_relaxed)),
  };

  const uint64_t secondSequence =
      slot.sequence.load(std::memory_order_acquire);
  if (secondSequence != firstSequence) {
    return false;
  }

  sample = snapshot;
  return true;
}

void BMSRenderer::drawJudgementIndicatorSegment(long long startMicros,
                                                long long endMicros,
                                                const JudgementIndicatorLayout
                                                    &layout,
                                                float barY, Color color) {
  if (endMicros <= startMicros) {
    return;
  }
  const float x0 = std::clamp(judgementOffsetToX(startMicros, layout), layout.x,
                              layout.x + layout.width);
  const float x1 = std::clamp(judgementOffsetToX(endMicros, layout), layout.x,
                              layout.x + layout.width);
  if (x1 <= x0) {
    return;
  }
  drawRect(x1 - x0, layout.barHeight, x0, barY, color);
}

void BMSRenderer::drawJudgementIndicator(long long currentTimeMicros) {
  if (!judgementIndicatorEnabled.load(std::memory_order_relaxed)) {
    return;
  }

  std::array<JudgementIndicatorSample, kJudgementIndicatorMaxVisibleSamples>
      samples{};
  size_t sampleCount = 0;
  long long averageSum = 0;
  size_t averageCount = 0;
  const uint64_t nextSequence =
      judgementIndicatorWriteSequence.load(std::memory_order_acquire);
  const uint64_t firstSequence =
      nextSequence > kJudgementIndicatorMaxVisibleSamples
          ? nextSequence - kJudgementIndicatorMaxVisibleSamples
          : 0;

  for (uint64_t sequence = firstSequence; sequence < nextSequence; ++sequence) {
    JudgementIndicatorSample sample;
    if (!readJudgementIndicatorSample(sequence, sample)) {
      continue;
    }
    if (currentTimeMicros >= sample.createdTimeMicros &&
        currentTimeMicros - sample.createdTimeMicros <=
            kJudgementIndicatorFadeMicros &&
        sampleCount < samples.size()) {
      samples[sampleCount++] = sample;
    }
  }

  for (uint64_t sequence = nextSequence; sequence > firstSequence;) {
    --sequence;
    JudgementIndicatorSample sample;
    if (!readJudgementIndicatorSample(sequence, sample)) {
      continue;
    }
    if (sample.judgement == Kpoor) {
      continue;
    }
    averageSum += sample.diffMicros;
    averageCount++;
    if (averageCount >= kJudgementIndicatorAverageSampleCount) {
      break;
    }
  }

  const bool hudMode = judgementIndicatorHudMode.load(std::memory_order_relaxed);
  const JudgementIndicatorLayout layout = judgementIndicatorLayout(hudMode);
  const float barY = layout.centerY - layout.barHeight * 0.5f;
  const float markerY = layout.centerY - layout.markerHeight * 0.5f;

  drawRect(layout.width, layout.barHeight + (hudMode ? 2.0f : 0.026f),
           layout.x, barY - (hudMode ? 1.0f : 0.013f),
           Color(0, 0, 0, hudMode ? 132 : 118));
  drawJudgementIndicatorSegment(-judgementIndicatorRangeMicros,
                                timingWindowEarly(Bad), layout, barY,
                                judgementColor(Poor, 118));
  drawJudgementIndicatorSegment(timingWindowEarly(Bad),
                                timingWindowEarly(Good), layout, barY,
                                judgementColor(Bad, 126));
  drawJudgementIndicatorSegment(timingWindowEarly(Good),
                                timingWindowEarly(Great), layout, barY,
                                judgementColor(Good, 134));
  drawJudgementIndicatorSegment(timingWindowEarly(Great),
                                timingWindowEarly(PGreat), layout, barY,
                                judgementColor(Great, 142));
  drawJudgementIndicatorSegment(timingWindowEarly(PGreat),
                                timingWindowLate(PGreat), layout, barY,
                                judgementColor(PGreat, 170));
  drawJudgementIndicatorSegment(timingWindowLate(PGreat),
                                timingWindowLate(Great), layout, barY,
                                judgementColor(Great, 142));
  drawJudgementIndicatorSegment(timingWindowLate(Great), timingWindowLate(Good),
                                layout, barY, judgementColor(Good, 134));
  drawJudgementIndicatorSegment(timingWindowLate(Good), timingWindowLate(Bad),
                                layout, barY, judgementColor(Bad, 126));
  drawJudgementIndicatorSegment(timingWindowLate(Bad),
                                judgementIndicatorRangeMicros, layout, barY,
                                judgementColor(Poor, 118));

  const float centerTickWidth = layout.markerWidth * 0.8f;
  drawRect(centerTickWidth, layout.markerHeight * 0.64f,
           layout.x + layout.width * 0.5f - centerTickWidth * 0.5f,
           layout.centerY - layout.markerHeight * 0.32f,
           Color(255, 255, 255, hudMode ? 118 : 105));

  for (size_t i = 0; i < sampleCount; ++i) {
    const auto &sample = samples[i];
    long long ageMicros = currentTimeMicros - sample.createdTimeMicros;
    if (ageMicros < 0) {
      ageMicros = 0;
    }
    const float alpha = 1.0f - static_cast<float>(ageMicros) /
                                   static_cast<float>(
                                       kJudgementIndicatorFadeMicros);
    if (alpha <= 0.0f) {
      continue;
    }
    const float x = judgementOffsetToX(sample.diffMicros, layout);
    drawRect(layout.markerWidth, layout.markerHeight,
             x - layout.markerWidth * 0.5f, markerY,
             judgementColor(sample.judgement, alphaByte(alpha * 0.95f)));
  }

  if (averageCount > 0) {
    const long long averageDiff =
        averageSum / static_cast<long long>(averageCount);
    const float x = judgementOffsetToX(averageDiff, layout);
    const float averageWidth = layout.markerWidth * 2.4f;
    const float averagePad = hudMode ? 3.0f : 0.04f;
    drawRect(averageWidth, layout.markerHeight + averagePad * 2.0f,
             x - averageWidth * 0.5f, markerY - averagePad,
             Color(0, 0, 0, 205));
    drawRect(averageWidth * 0.44f, layout.markerHeight + averagePad,
             x - averageWidth * 0.22f, markerY - averagePad * 0.5f,
             Color(255, 245, 140, 240));
  }
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
  if (recordTimingSample &&
      judgementIndicatorEnabled.load(std::memory_order_relaxed)) {
    const uint64_t sequence =
        judgementIndicatorWriteSequence.fetch_add(1, std::memory_order_acq_rel);
    auto &sample =
        judgementIndicatorSamples[sequence %
                                  kJudgementIndicatorMaxVisibleSamples];
    sample.sequence.store(0, std::memory_order_release);
    sample.diffMicros.store(judgeResult.Diff, std::memory_order_relaxed);
    sample.createdTimeMicros.store(displayTimeMicros,
                                   std::memory_order_relaxed);
    sample.judgement.store(static_cast<int>(judgeResult.judgement),
                           std::memory_order_relaxed);
    sample.sequence.store(sequence + 1, std::memory_order_release);
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
    longBodyBatchFor(sheet, head->IsHolding)
        .addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight, 1.0f,
                 tileV, bodyTexture);
  }

  // Tail
  sheetBatchFor(sheet).addRectUV(laneToX(head->Tail->Lane), tailY,
                                 noteRenderWidth, noteRenderHeight, tailUv.u0,
                                 tailUv.v0, tailUv.u1, tailUv.v1,
                                 sheet.texture);

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
  constexpr uint32_t kDepthLongBodies = 190;
  constexpr uint32_t kDepthNotes = 200;
  constexpr uint32_t kDepthGhosts = 250;
  constexpr uint32_t kDepthBeams = 300;
  constexpr uint32_t kDepthJudgementIndicator = 330;

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kDepthBackground);
  ghostBatchRenderer.setSubmitDepth(kDepthGhosts);
  simpleBatchRenderer.begin();
  ghostBatchRenderer.begin();
  beginNoteTextureBatches(kDepthLongBodies, kDepthNotes);
  // background
  drawRect(gameplay_geometry::kPlayAreaWidth, upperBound - judgeY, 0.0f,
           judgeY, Color(20, 20, 20, 122));
  // judge line
  drawRect(gameplay_geometry::kPlayAreaWidth, noteRenderHeight, 0.0f, judgeY,
           Color(255, 255, 255, 255));
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
        drawRect(gameplay_geometry::kPlayAreaWidth, 0.05f, 0.0f, y,
                 Color(255, 255, 255, 128));
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
  drawReplayGhosts(rxhs, micro, currentScrollPosition);

  // Flush background/measure pass before notes.
  simpleBatchRenderer.flush();
  flushNoteTextureBatches();
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

  if (judgementIndicatorEnabled.load(std::memory_order_relaxed)) {
    const bool indicatorHudMode =
        judgementIndicatorHudMode.load(std::memory_order_relaxed);
    simpleBatchRenderer.setSubmitView(indicatorHudMode ? rendering::ui_view
                                                       : rendering::main_view);
    simpleBatchRenderer.setSubmitDepth(indicatorHudMode
                                           ? 0
                                           : kDepthJudgementIndicator);
    simpleBatchRenderer.begin();
    drawJudgementIndicator(micro);
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
  clearJudgementIndicatorSamples();
}

void BMSRenderer::refreshGeometry() {
  upperBound = calculateLanePlaneScreenTopIntersection();
}

void BMSRenderer::setVisibleTimeGreenNumber(int greenNumber) {
  visibleTimeGreenNumber = greenNumber;
}

void BMSRenderer::setLaneBeamsEnabled(bool enabled) {
  renderLaneBeams = enabled;
}

void BMSRenderer::setLaneBeamClockUsesRenderTime(bool enabled) {
  useRenderTimeForLaneBeams = enabled;
}

void BMSRenderer::setJudgementIndicatorConfig(bool enabled, float y,
                                              bool hudMode) {
  const int yPermille =
      std::clamp(static_cast<int>(std::lround(std::clamp(y, 0.0f, 1.0f) *
                                              1000.0f)),
                 0, 1000);
  judgementIndicatorYPermille.store(yPermille, std::memory_order_relaxed);
  judgementIndicatorHudMode.store(hudMode, std::memory_order_relaxed);
  judgementIndicatorEnabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    clearJudgementIndicatorSamples();
  }
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
  if (replayData == nullptr) {
    return;
  }

  for (const auto &event : replayData->events) {
    if ((event.action != ReplayEventAction::Press &&
         event.action != ReplayEventAction::Release) ||
        event.judgement == None || event.noteTimeMicros < 0) {
      continue;
    }
    if (laneToOrderIndex.find(event.lane) == laneToOrderIndex.end()) {
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

    replayGhostEvents.push_back({
        .lane = event.lane,
        .noteTimeMicros = event.noteTimeMicros,
        .judgeTimeMicros = event.judgeTimeMicros,
        .judgeScrollPosition = scrollPositionAtTime(event.judgeTimeMicros),
        .judgement = event.judgement,
    });
  }

  std::sort(replayGhostEvents.begin(), replayGhostEvents.end(),
            [](const ReplayGhostEvent &a, const ReplayGhostEvent &b) {
              if (a.judgeScrollPosition != b.judgeScrollPosition) {
                return a.judgeScrollPosition < b.judgeScrollPosition;
              }
              if (a.judgeTimeMicros != b.judgeTimeMicros) {
                return a.judgeTimeMicros < b.judgeTimeMicros;
              }
              if (a.noteTimeMicros != b.noteTimeMicros) {
                return a.noteTimeMicros < b.noteTimeMicros;
              }
              return a.lane < b.lane;
            });
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
  const float beamHeight = std::max(0.0f, upperBound - judgeY);
  if (beamHeight <= 0.0f) {
    return;
  }
  drawRect(noteRenderWidth, beamHeight, laneToX(lane), judgeY, color);
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
inline float BMSRenderer::computeLaneX(int lane) const {
  if (const auto it = laneToOrderIndex.find(lane);
      it != laneToOrderIndex.end()) {
    return static_cast<float>(it->second) * noteRenderWidth;
  }

  return 0.0f;
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
