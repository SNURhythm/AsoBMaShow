#include "scene/PracticeAnalyticsPresentation.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

practice::SectionAnalysis section(double accuracy, std::size_t samples = 1,
                                  std::size_t misses = 0) {
  practice::SectionAnalysis result;
  result.timing.samples = samples;
  result.timing.misses = misses;
  result.accuracy = accuracy;
  return result;
}

void testVisualGroupsPoolSectionAccuracy() {
  const std::vector<practice::SectionAnalysis> sections = {
      section(1.00, 3),
      section(0.50, 1),
      section(0.25, 1),
      section(0.00, 1, 2),
  };
  const auto groups = practice_analytics_presentation::visualSectionGroups(
      sections, 12.0f, 6.0f);
  require(groups.size() == 2 && groups[0].firstSection == 0 &&
              groups[0].lastSection == 1,
          "narrow measures are grouped into stable adjacent ranges");
  require(groups[0].accuracy.has_value() &&
              *groups[0].accuracy == 0.875,
          "visual group pools accuracy by judgement opportunities");
  require(groups[1].accuracy.has_value() &&
              *groups[1].accuracy == 0.0625,
          "misses contribute zero accuracy with their full weight");
}

void testExactSectionHitMappingIgnoresVisualGrouping() {
  using practice_analytics_presentation::exactSectionForX;
  require(exactSectionForX(100, 0.0f, 20.0f) == 0 &&
              exactSectionForX(100, 9.9f, 20.0f) == 49 &&
              exactSectionForX(100, 20.0f, 20.0f) == 99,
          "hit mapping retains every original section boundary");
}

void testPointerCaptureSeparatesMouseTouchAndCancelsReliably() {
  using namespace practice_analytics_presentation;
  PointerCaptureState state;
  require(state.handleTouch(PointerPhase::Down, 7) ==
                  PointerTransition::Begin &&
              state.touchActive(),
          "touch begins its own capture");
  require(state.handleMouse(PointerPhase::Move, true) ==
                  PointerTransition::Ignored &&
              !state.mouseActive() && state.touchActive(),
          "synthetic touch mouse motion cannot mutate capture state");
  require(state.handleTouch(PointerPhase::Up, 9) ==
                  PointerTransition::Ignored &&
              state.touchActive(),
          "unmatched finger-up leaves the active touch intact");
  require(state.handleTouch(PointerPhase::Up, 7) == PointerTransition::End &&
              !state.touchActive(),
          "matching finger-up always clears touch capture");

  require(state.handleMouse(PointerPhase::Down, false) ==
                  PointerTransition::Begin &&
              state.handleTouch(PointerPhase::Down, 11) ==
                  PointerTransition::Begin,
          "mouse and touch captures are independent");
  state.cancelAll();
  require(!state.mouseActive() && !state.touchActive(),
          "window leave or focus loss cancels every capture");
}

void testBaselineLayoutHasShrinkRoomWithoutClippingControls() {
  using namespace practice_analytics_presentation;
  constexpr float fixedChildren = 96 + 198 + 100 + 108 + 136 + 64;
  constexpr float rootPadding = 64;
  constexpr float gaps = 6 * 12;
  constexpr float available = 1080 - fixedChildren - rootPadding - gaps;
  require(available == 242.0f &&
              resolvedAnalyticsHeight(available) == available &&
              available >= kMinimumAnalyticsHeight,
          "1080 result layout shrinks analytics without clipping controls");
}

void testResultPhotoExportsEveryAnalyticsModeInDisplayOrder() {
  using practice_analytics_presentation::exportAnalyticsModes;
  const auto modes = exportAnalyticsModes();
  require(modes.size() == 3 &&
              modes[0] == PracticeAnalyticsMode::Histogram &&
              modes[1] == PracticeAnalyticsMode::Lanes &&
              modes[2] == PracticeAnalyticsMode::Sections,
          "result photo includes all analytics modes in tab order");
}

void testResultPhotoShowsSharedInformationOnlyOnHistogram() {
  using practice_analytics_presentation::photoExportShowsSharedInformation;
  using practice_analytics_presentation::photoExportAnalyticsHeight;
  require(photoExportShowsSharedInformation(
              PracticeAnalyticsMode::Histogram) &&
              !photoExportShowsSharedInformation(
                  PracticeAnalyticsMode::Lanes) &&
              !photoExportShowsSharedInformation(
                  PracticeAnalyticsMode::Sections),
          "photo export does not duplicate aggregate and timing information");
  require(photoExportAnalyticsHeight(PracticeAnalyticsMode::Histogram) ==
                  206.0f &&
              photoExportAnalyticsHeight(PracticeAnalyticsMode::Lanes) ==
                  120.0f &&
              photoExportAnalyticsHeight(PracticeAnalyticsMode::Sections) ==
                  120.0f,
          "photo export shrinks cards after removing their shared rows");
}

void testResultVideoSlidesAnalyticsInEqualThirds() {
  using practice_analytics_presentation::analyticsModeForSlideshow;
  constexpr long long duration = 9'000'000;
  require(analyticsModeForSlideshow(0, duration) ==
                  PracticeAnalyticsMode::Histogram &&
              analyticsModeForSlideshow(2'999'999, duration) ==
                  PracticeAnalyticsMode::Histogram &&
              analyticsModeForSlideshow(3'000'000, duration) ==
                  PracticeAnalyticsMode::Lanes &&
              analyticsModeForSlideshow(5'999'999, duration) ==
                  PracticeAnalyticsMode::Lanes &&
              analyticsModeForSlideshow(6'000'000, duration) ==
                  PracticeAnalyticsMode::Sections &&
              analyticsModeForSlideshow(duration, duration) ==
                  PracticeAnalyticsMode::Sections,
          "result video gives each analytics mode one third of the tail");
}

} // namespace

int main() {
  testVisualGroupsPoolSectionAccuracy();
  testExactSectionHitMappingIgnoresVisualGrouping();
  testPointerCaptureSeparatesMouseTouchAndCancelsReliably();
  testBaselineLayoutHasShrinkRoomWithoutClippingControls();
  testResultPhotoExportsEveryAnalyticsModeInDisplayOrder();
  testResultPhotoShowsSharedInformationOnlyOnHistogram();
  testResultVideoSlidesAnalyticsInEqualThirds();
  return 0;
}
