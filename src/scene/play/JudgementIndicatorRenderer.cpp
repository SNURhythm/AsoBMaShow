#include "JudgementIndicatorRenderer.h"

#include "GameplayGeometry.h"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr long long kDefaultRangeMicros = 500000LL;
constexpr long long kFadeMicros = 1800000LL;
constexpr float kWorldWidthRatio = 0.4f;
constexpr float kHudWidthRatio = 0.2f;
constexpr float kHudMinWidth = 220.0f;
constexpr float kHudMaxWidth = 720.0f;
constexpr float kMinWidthScale = 0.5f;
constexpr float kMaxWidthScale = 2.0f;

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "Judgement indicator sequence atomic must be lock-free");
static_assert(std::atomic<long long>::is_always_lock_free,
              "Judgement indicator timing atomic must be lock-free");
static_assert(std::atomic<int>::is_always_lock_free,
              "Judgement indicator value atomic must be lock-free");

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

long long indicatorRangeFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  long long range = kDefaultRangeMicros;
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

void addRect(rendering::SimpleBatchRenderer &batch, float width, float height,
             float x, float y, Color color) {
  batch.addRect(x, y, width, height, color.toABGR());
}
} // namespace

JudgementIndicatorRenderer::JudgementIndicatorRenderer(
    const std::map<Judgement, std::pair<long long, long long>> &timingWindows)
    : timingWindows(timingWindows),
      rangeMicros(indicatorRangeFromWindows(timingWindows)) {}

void JudgementIndicatorRenderer::configure(bool enabled, float y,
                                           float widthScale, bool hudMode) {
  const int nextYPermille =
      std::clamp(static_cast<int>(std::lround(std::clamp(y, 0.0f, 1.0f) *
                                              1000.0f)),
                 0, 1000);
  const float clampedWidthScale =
      std::clamp(widthScale, kMinWidthScale, kMaxWidthScale);
  const int nextWidthPermille =
      std::clamp(static_cast<int>(std::lround(clampedWidthScale * 1000.0f)),
                 500, 2000);
  yPermille.store(nextYPermille, std::memory_order_relaxed);
  widthPermille.store(nextWidthPermille, std::memory_order_relaxed);
  this->hudMode.store(hudMode, std::memory_order_relaxed);
  this->enabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    clear();
  }
}

void JudgementIndicatorRenderer::record(const JudgeResult &judgeResult,
                                        long long displayTimeMicros) {
  if (judgeResult.judgement == None ||
      !enabled.load(std::memory_order_relaxed)) {
    return;
  }

  const uint64_t sequence =
      writeSequence.fetch_add(1, std::memory_order_acq_rel);
  auto &sample = samples[sequence % kMaxVisibleSamples];
  sample.sequence.store(0, std::memory_order_release);
  sample.diffMicros.store(judgeResult.Diff, std::memory_order_relaxed);
  sample.createdTimeMicros.store(displayTimeMicros, std::memory_order_relaxed);
  sample.judgement.store(static_cast<int>(judgeResult.judgement),
                         std::memory_order_relaxed);
  sample.sequence.store(sequence + 1, std::memory_order_release);
}

void JudgementIndicatorRenderer::clear() {
  writeSequence.store(0, std::memory_order_release);
  for (auto &sample : samples) {
    sample.sequence.store(0, std::memory_order_release);
    sample.diffMicros.store(0, std::memory_order_relaxed);
    sample.createdTimeMicros.store(0, std::memory_order_relaxed);
    sample.judgement.store(None, std::memory_order_relaxed);
  }
}

void JudgementIndicatorRenderer::render(rendering::SimpleBatchRenderer &batch,
                                        long long currentTimeMicros,
                                        const Geometry &geometry) {
  if (!enabled.load(std::memory_order_relaxed)) {
    return;
  }

  std::array<Sample, kMaxVisibleSamples> visibleSamples{};
  size_t visibleSampleCount = 0;
  long long averageSum = 0;
  size_t averageCount = 0;
  const uint64_t nextSequence = writeSequence.load(std::memory_order_acquire);
  const uint64_t firstSequence =
      nextSequence > kMaxVisibleSamples ? nextSequence - kMaxVisibleSamples : 0;

  for (uint64_t sequence = firstSequence; sequence < nextSequence; ++sequence) {
    Sample sample;
    if (!readSample(sequence, sample)) {
      continue;
    }
    if (currentTimeMicros >= sample.createdTimeMicros &&
        currentTimeMicros - sample.createdTimeMicros <= kFadeMicros &&
        visibleSampleCount < visibleSamples.size()) {
      visibleSamples[visibleSampleCount++] = sample;
    }
  }

  for (uint64_t sequence = nextSequence; sequence > firstSequence;) {
    --sequence;
    Sample sample;
    if (!readSample(sequence, sample)) {
      continue;
    }
    if (sample.judgement == Kpoor) {
      continue;
    }
    averageSum += sample.diffMicros;
    averageCount++;
    if (averageCount >= kAverageSampleCount) {
      break;
    }
  }

  const bool currentHudMode = isHudMode();
  const Layout indicatorLayout = layout(geometry, currentHudMode);
  const float barY = indicatorLayout.centerY - indicatorLayout.barHeight * 0.5f;
  const float markerY =
      indicatorLayout.centerY - indicatorLayout.markerHeight * 0.5f;

  addRect(batch, indicatorLayout.width,
          indicatorLayout.barHeight + (currentHudMode ? 2.0f : 0.026f),
          indicatorLayout.x, barY - (currentHudMode ? 1.0f : 0.013f),
          Color(0, 0, 0, currentHudMode ? 132 : 118));
  drawSegment(batch, -rangeMicros, timingWindowEarly(Bad), indicatorLayout,
              barY, judgementColor(Poor, 118));
  drawSegment(batch, timingWindowEarly(Bad), timingWindowEarly(Good),
              indicatorLayout, barY, judgementColor(Bad, 126));
  drawSegment(batch, timingWindowEarly(Good), timingWindowEarly(Great),
              indicatorLayout, barY, judgementColor(Good, 134));
  drawSegment(batch, timingWindowEarly(Great), timingWindowEarly(PGreat),
              indicatorLayout, barY, judgementColor(Great, 142));
  drawSegment(batch, timingWindowEarly(PGreat), timingWindowLate(PGreat),
              indicatorLayout, barY, judgementColor(PGreat, 170));
  drawSegment(batch, timingWindowLate(PGreat), timingWindowLate(Great),
              indicatorLayout, barY, judgementColor(Great, 142));
  drawSegment(batch, timingWindowLate(Great), timingWindowLate(Good),
              indicatorLayout, barY, judgementColor(Good, 134));
  drawSegment(batch, timingWindowLate(Good), timingWindowLate(Bad),
              indicatorLayout, barY, judgementColor(Bad, 126));
  drawSegment(batch, timingWindowLate(Bad), rangeMicros, indicatorLayout, barY,
              judgementColor(Poor, 118));

  const float centerTickWidth = indicatorLayout.markerWidth * 0.8f;
  addRect(batch, centerTickWidth, indicatorLayout.markerHeight * 0.64f,
          indicatorLayout.x + indicatorLayout.width * 0.5f -
              centerTickWidth * 0.5f,
          indicatorLayout.centerY - indicatorLayout.markerHeight * 0.32f,
          Color(255, 255, 255, currentHudMode ? 118 : 105));

  for (size_t i = 0; i < visibleSampleCount; ++i) {
    const auto &sample = visibleSamples[i];
    long long ageMicros = currentTimeMicros - sample.createdTimeMicros;
    if (ageMicros < 0) {
      ageMicros = 0;
    }
    const float alpha =
        1.0f - static_cast<float>(ageMicros) / static_cast<float>(kFadeMicros);
    if (alpha <= 0.0f) {
      continue;
    }
    const float x = offsetToX(sample.diffMicros, indicatorLayout);
    addRect(batch, indicatorLayout.markerWidth, indicatorLayout.markerHeight,
            x - indicatorLayout.markerWidth * 0.5f, markerY,
            judgementColor(sample.judgement, alphaByte(alpha * 0.95f)));
  }

  if (averageCount > 0) {
    const long long averageDiff =
        averageSum / static_cast<long long>(averageCount);
    const float x = offsetToX(averageDiff, indicatorLayout);
    const float averageWidth = indicatorLayout.markerWidth * 2.4f;
    const float averagePad = currentHudMode ? 3.0f : 0.04f;
    addRect(batch, averageWidth,
            indicatorLayout.markerHeight + averagePad * 2.0f,
            x - averageWidth * 0.5f, markerY - averagePad,
            Color(0, 0, 0, 205));
    addRect(batch, averageWidth * 0.44f,
            indicatorLayout.markerHeight + averagePad,
            x - averageWidth * 0.22f, markerY - averagePad * 0.5f,
            Color(255, 245, 140, 240));
  }
}

bool JudgementIndicatorRenderer::isEnabled() const {
  return enabled.load(std::memory_order_relaxed);
}

bool JudgementIndicatorRenderer::isHudMode() const {
  return hudMode.load(std::memory_order_relaxed);
}

JudgementIndicatorRenderer::Layout
JudgementIndicatorRenderer::layout(const Geometry &geometry,
                                   bool hudMode) const {
  const float normalizedY =
      static_cast<float>(
          std::clamp(yPermille.load(std::memory_order_relaxed), 0, 1000)) /
      1000.0f;
  const float widthScale =
      static_cast<float>(
          std::clamp(widthPermille.load(std::memory_order_relaxed), 500, 2000)) /
      1000.0f;

  Layout indicatorLayout{};
  if (hudMode) {
    indicatorLayout.width =
        std::clamp(static_cast<float>(rendering::window_width) *
                       kHudWidthRatio * widthScale,
                   kHudMinWidth, kHudMaxWidth);
    indicatorLayout.x =
        (static_cast<float>(rendering::window_width) - indicatorLayout.width) *
        0.5f;
    indicatorLayout.barHeight = 5.0f;
    indicatorLayout.markerHeight = 18.0f;
    indicatorLayout.markerWidth = 2.0f;
    indicatorLayout.centerY =
        static_cast<float>(rendering::window_height) * (1.0f - normalizedY);
    indicatorLayout.centerY =
        std::clamp(indicatorLayout.centerY,
                   indicatorLayout.markerHeight * 0.5f,
                   static_cast<float>(rendering::window_height) -
                       indicatorLayout.markerHeight * 0.5f);
    return indicatorLayout;
  }

  indicatorLayout.width = geometry.playAreaWidth * kWorldWidthRatio *
                          widthScale;
  indicatorLayout.x = geometry.playAreaLeftX + geometry.playAreaWidth * 0.5f -
                      indicatorLayout.width * 0.5f;
  const float laneHeight = std::max(0.1f, geometry.upperBound - geometry.judgeY);
  indicatorLayout.centerY = geometry.judgeY + laneHeight * normalizedY;
  indicatorLayout.barHeight =
      std::max(0.02f, geometry.noteRenderHeight * 0.08f);
  indicatorLayout.markerHeight =
      std::max(0.16f, geometry.noteRenderHeight * 0.5f);
  indicatorLayout.markerWidth =
      std::max(0.01f, geometry.noteRenderWidth * 0.014f);
  return indicatorLayout;
}

float JudgementIndicatorRenderer::offsetToX(long long diffMicros,
                                            const Layout &layout) const {
  const long long clampedDiff =
      std::clamp(diffMicros, -rangeMicros, rangeMicros);
  const float normalized =
      static_cast<float>(clampedDiff) / static_cast<float>(rangeMicros);
  return layout.x + layout.width * 0.5f + normalized * layout.width * 0.5f;
}

long long
JudgementIndicatorRenderer::timingWindowEarly(Judgement judgement) const {
  return timingWindowEarlyFrom(timingWindows, judgement, 0);
}

long long
JudgementIndicatorRenderer::timingWindowLate(Judgement judgement) const {
  return timingWindowLateFrom(timingWindows, judgement, 0);
}

bool JudgementIndicatorRenderer::readSample(uint64_t sequence,
                                            Sample &sample) const {
  const auto &slot = samples[sequence % kMaxVisibleSamples];
  const uint64_t expectedSequence = sequence + 1;
  const uint64_t firstSequence = slot.sequence.load(std::memory_order_acquire);
  if (firstSequence != expectedSequence) {
    return false;
  }

  Sample snapshot{
      .diffMicros = slot.diffMicros.load(std::memory_order_relaxed),
      .createdTimeMicros =
          slot.createdTimeMicros.load(std::memory_order_relaxed),
      .judgement =
          static_cast<Judgement>(slot.judgement.load(std::memory_order_relaxed)),
  };

  const uint64_t secondSequence = slot.sequence.load(std::memory_order_acquire);
  if (secondSequence != firstSequence) {
    return false;
  }

  sample = snapshot;
  return true;
}

void JudgementIndicatorRenderer::drawSegment(
    rendering::SimpleBatchRenderer &batch, long long startMicros,
    long long endMicros, const Layout &layout, float barY, Color color) const {
  if (endMicros <= startMicros) {
    return;
  }
  const float x0 = std::clamp(offsetToX(startMicros, layout), layout.x,
                              layout.x + layout.width);
  const float x1 = std::clamp(offsetToX(endMicros, layout), layout.x,
                              layout.x + layout.width);
  if (x1 <= x0) {
    return;
  }
  addRect(batch, x1 - x0, layout.barHeight, x0, barY, color);
}
