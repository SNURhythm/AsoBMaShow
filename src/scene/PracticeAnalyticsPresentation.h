#pragma once

#include "../practice/PracticeAnalytics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

enum class PracticeAnalyticsMode { Histogram, Lanes, Sections };

namespace practice_analytics_presentation {

inline constexpr float kPreferredAnalyticsHeight = 250.0f;
inline constexpr float kMinimumAnalyticsHeight = 236.0f;
inline constexpr float kPhotoCompactAnalyticsHeight = 120.0f;

enum class SectionTone : std::uint8_t {
  Neutral,
  Stable,
  Early,
  Late,
  Danger,
};

struct VisualSectionGroup {
  std::size_t firstSection = 0;
  std::size_t lastSection = 0;
  double pooledBadMissRate = 0.0;
  double maximumBadMissRate = 0.0;
  std::optional<double> pooledMeanMillis;
  std::optional<double> dominantMeanMillis;
  double severity = 0.0;
  SectionTone tone = SectionTone::Neutral;
};

[[nodiscard]] std::vector<VisualSectionGroup>
visualSectionGroups(std::span<const practice::SectionAnalysis> sections,
                    float availableWidth, float minimumVisualWidth);

[[nodiscard]] std::size_t exactSectionForX(std::size_t sectionCount, float x,
                                           float width) noexcept;

enum class PointerPhase : std::uint8_t { Down, Move, Up, Cancel };
enum class PointerTransition : std::uint8_t {
  Ignored,
  Begin,
  Update,
  End,
  Cancelled,
};

class PointerCaptureState {
public:
  [[nodiscard]] PointerTransition handleMouse(PointerPhase phase,
                                              bool syntheticTouchMouse);
  [[nodiscard]] PointerTransition handleTouch(PointerPhase phase,
                                              long long fingerId);
  void cancelAll() noexcept;
  [[nodiscard]] bool mouseActive() const noexcept { return mouseCaptured; }
  [[nodiscard]] bool touchActive() const noexcept {
    return touchFinger.has_value();
  }

private:
  bool mouseCaptured = false;
  std::optional<long long> touchFinger;
};

[[nodiscard]] float resolvedAnalyticsHeight(float availableHeight) noexcept;

[[nodiscard]] constexpr std::array<PracticeAnalyticsMode, 3>
exportAnalyticsModes() noexcept {
  return {PracticeAnalyticsMode::Histogram, PracticeAnalyticsMode::Lanes,
          PracticeAnalyticsMode::Sections};
}

[[nodiscard]] constexpr bool
photoExportShowsSharedInformation(PracticeAnalyticsMode mode) noexcept {
  return mode == PracticeAnalyticsMode::Histogram;
}

[[nodiscard]] constexpr float
photoExportAnalyticsHeight(PracticeAnalyticsMode mode) noexcept {
  return photoExportShowsSharedInformation(mode)
             ? kPreferredAnalyticsHeight
             : kPhotoCompactAnalyticsHeight;
}

[[nodiscard]] constexpr PracticeAnalyticsMode
analyticsModeForSlideshow(long long elapsedMicros,
                          long long durationMicros) noexcept {
  if (durationMicros <= 0 || elapsedMicros <= 0) {
    return PracticeAnalyticsMode::Histogram;
  }
  if (elapsedMicros >= durationMicros) {
    return PracticeAnalyticsMode::Sections;
  }
  const long double progress = static_cast<long double>(elapsedMicros) /
                               static_cast<long double>(durationMicros);
  if (progress < 1.0L / 3.0L) {
    return PracticeAnalyticsMode::Histogram;
  }
  if (progress < 2.0L / 3.0L) {
    return PracticeAnalyticsMode::Lanes;
  }
  return PracticeAnalyticsMode::Sections;
}

} // namespace practice_analytics_presentation
