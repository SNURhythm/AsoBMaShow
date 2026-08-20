#pragma once

#include "../../JudgementIndicatorRange.h"
#include "../../rendering/Color.h"
#include "Judge.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace rendering {
class SimpleBatchRenderer;
} // namespace rendering

class JudgementIndicatorRenderer {
public:
  struct Geometry {
    float judgeY = 0.0f;
    float upperBound = 10.0f;
    float playAreaLeftX = 0.0f;
    float playAreaWidth = 8.0f;
    float noteRenderWidth = 1.0f;
    float noteRenderHeight = 1.0f;
    // Built-in lanes use increasing world Y; selected-skin HUD coordinates
    // are UI pixels and therefore increase downward.
    float verticalDirection = 1.0f;
    // A selected skin anchors the indicator to its lanes even when the user
    // chose the built-in HUD mode, which otherwise centers on the window.
    std::optional<bool> hudModeOverride;
  };

  explicit JudgementIndicatorRenderer(
      const std::map<Judgement, std::pair<long long, long long>>
          &timingWindows);

  void configure(bool enabled, float y, float widthScale, bool hudMode,
                 int rangeMilliseconds);
  void updateTimingWindows(
      const std::map<Judgement, std::pair<long long, long long>> &windows);
  void record(const JudgeResult &judgeResult, long long displayTimeMicros);
  void clear();
  void render(rendering::SimpleBatchRenderer &batch, long long currentTimeMicros,
              const Geometry &geometry);

  [[nodiscard]] bool isEnabled() const;
  [[nodiscard]] bool isHudMode() const;
#if defined(ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION)
  [[nodiscard]] std::size_t retainedSampleCountForTesting() const noexcept;
  [[nodiscard]] std::pair<long long, long long>
  timingWindowForTesting(Judgement judgement) const;
#endif

private:
  static constexpr size_t kAverageSampleCount = 20;
  static constexpr size_t kMaxVisibleSamples =
      judgement_indicator::kRecentTimingSampleCapacity;

  struct Sample {
    long long diffMicros = 0;
    long long createdTimeMicros = 0;
    Judgement judgement = None;
  };

  struct Layout {
    float x = 0.0f;
    float centerY = 0.0f;
    float width = 1.0f;
    float barHeight = 0.05f;
    float markerHeight = 0.3f;
    float markerWidth = 0.02f;
  };

  struct AtomicSample {
    std::atomic<uint64_t> sequence{0};
    std::atomic<long long> diffMicros{0};
    std::atomic<long long> createdTimeMicros{0};
    std::atomic<int> judgement{None};
  };

  [[nodiscard]] Layout layout(const Geometry &geometry, bool hudMode) const;
  [[nodiscard]] float offsetToX(long long diffMicros,
                                long long displayRangeMicros,
                                const Layout &layout) const;
  [[nodiscard]] long long timingWindowEarly(Judgement judgement) const;
  [[nodiscard]] long long timingWindowLate(Judgement judgement) const;
  bool readSample(uint64_t sequence, Sample &sample) const;
  void drawSegment(rendering::SimpleBatchRenderer &batch, long long startMicros,
                   long long endMicros, long long displayRangeMicros,
                   const Layout &layout, float barY, Color color) const;

  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  std::atomic<long long> rangeMicros{judgement_indicator::rangeMicros(
      judgement_indicator::kDefaultRangeMilliseconds)};
  std::array<AtomicSample, kMaxVisibleSamples> samples;
  std::atomic<uint64_t> writeSequence{0};
  std::atomic_bool enabled{true};
  std::atomic_bool hudMode{false};
  std::atomic<int> yPermille{500};
  std::atomic<int> widthPermille{1000};
};
