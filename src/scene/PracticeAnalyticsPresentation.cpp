#include "PracticeAnalyticsPresentation.h"

#include <algorithm>
#include <cmath>

namespace practice_analytics_presentation {
namespace {

VisualSectionGroup
makeVisualGroup(std::span<const practice::SectionAnalysis> sections,
                std::size_t firstSection, std::size_t lastSection) {
  VisualSectionGroup result{
      .firstSection = firstSection,
      .lastSection = lastSection,
  };
  long double accuracyTotal = 0.0L;
  std::size_t opportunityTotal = 0;

  for (std::size_t index = firstSection; index <= lastSection; ++index) {
    const auto &section = sections[index];
    const std::size_t opportunities =
        section.timing.samples + section.timing.misses;
    if (!section.accuracy.has_value() || opportunities == 0) {
      continue;
    }
    accuracyTotal += static_cast<long double>(*section.accuracy) *
                     static_cast<long double>(opportunities);
    opportunityTotal += opportunities;
  }

  if (opportunityTotal > 0) {
    result.accuracy = static_cast<double>(
        accuracyTotal / static_cast<long double>(opportunityTotal));
  }
  return result;
}

} // namespace

std::vector<VisualSectionGroup>
visualSectionGroups(std::span<const practice::SectionAnalysis> sections,
                    float availableWidth, float minimumVisualWidth) {
  std::vector<VisualSectionGroup> result;
  if (sections.empty()) {
    return result;
  }
  const float exactWidth =
      availableWidth > 0.0f
          ? availableWidth / static_cast<float>(sections.size())
          : minimumVisualWidth;
  const std::size_t groupSize =
      exactWidth > 0.0f && minimumVisualWidth > exactWidth
          ? std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(
                                         minimumVisualWidth / exactWidth)))
          : 1;
  result.reserve((sections.size() + groupSize - 1) / groupSize);
  for (std::size_t first = 0; first < sections.size(); first += groupSize) {
    const std::size_t last =
        std::min(sections.size() - 1, first + groupSize - 1);
    result.push_back(makeVisualGroup(sections, first, last));
  }
  return result;
}

std::size_t exactSectionForX(std::size_t sectionCount, float x,
                             float width) noexcept {
  if (sectionCount <= 1 || width <= 0.0f) {
    return 0;
  }
  const float fraction =
      std::clamp(x / width, 0.0f, std::nextafter(1.0f, 0.0f));
  return std::min(sectionCount - 1,
                  static_cast<std::size_t>(fraction * sectionCount));
}

PointerTransition PointerCaptureState::handleMouse(PointerPhase phase,
                                                   bool syntheticTouchMouse) {
  if (syntheticTouchMouse) {
    return PointerTransition::Ignored;
  }
  switch (phase) {
  case PointerPhase::Down:
    if (mouseCaptured) {
      return PointerTransition::Ignored;
    }
    mouseCaptured = true;
    return PointerTransition::Begin;
  case PointerPhase::Move:
    return mouseCaptured ? PointerTransition::Update
                         : PointerTransition::Ignored;
  case PointerPhase::Up:
    if (!mouseCaptured) {
      return PointerTransition::Ignored;
    }
    mouseCaptured = false;
    return PointerTransition::End;
  case PointerPhase::Cancel:
    if (!mouseCaptured) {
      return PointerTransition::Ignored;
    }
    mouseCaptured = false;
    return PointerTransition::Cancelled;
  }
  return PointerTransition::Ignored;
}

PointerTransition PointerCaptureState::handleTouch(PointerPhase phase,
                                                   long long fingerId) {
  switch (phase) {
  case PointerPhase::Down:
    if (touchFinger.has_value()) {
      return PointerTransition::Ignored;
    }
    touchFinger = fingerId;
    return PointerTransition::Begin;
  case PointerPhase::Move:
    return touchFinger == fingerId ? PointerTransition::Update
                                   : PointerTransition::Ignored;
  case PointerPhase::Up:
    if (touchFinger != fingerId) {
      return PointerTransition::Ignored;
    }
    touchFinger.reset();
    return PointerTransition::End;
  case PointerPhase::Cancel:
    if (touchFinger != fingerId) {
      return PointerTransition::Ignored;
    }
    touchFinger.reset();
    return PointerTransition::Cancelled;
  }
  return PointerTransition::Ignored;
}

void PointerCaptureState::cancelAll() noexcept {
  mouseCaptured = false;
  touchFinger.reset();
}

float resolvedAnalyticsHeight(float availableHeight) noexcept {
  return std::clamp(availableHeight, 0.0f, kPreferredAnalyticsHeight);
}

} // namespace practice_analytics_presentation
