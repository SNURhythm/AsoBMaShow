#include "ChartViewerScene.h"
#include "ChartViewerNoteGeometry.h"
#include "ChartListenStart.h"

#include "../ArchiveFile.h"
#include "../ChartPlaybackDuration.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../repositories/ReplayRepository.h"
#include "../ReplayGhostUtils.h"
#include "../path.h"
#include "../practice/PracticeConfiguration.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticePresetStore.h"
#include "../practice/PracticeSession.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/TexBatchRenderer.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/OverlayPortal.h"
#include "../view/PlayOptionsPanelView.h"
#include "../view/ReplaySummaryListView.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "../view/View.h"
#include "PracticePanelView.h"
#include "play/GamePlayScene.h"
#include "play/Pacemaker.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
constexpr int kHeaderButtonHeight = 54;
constexpr int kHeaderPadding = 18;
constexpr float kMinZoom = 0.20f;
constexpr float kMaxZoom = 5.0f;
constexpr float kZoomStep = 0.20f;
constexpr float kMinScreenZoom = 0.65f;
constexpr float kMaxScreenZoom = 3.0f;
constexpr size_t kRandomDrawerPageSize = 96;
constexpr size_t kRandomSummaryLimit = 48;
constexpr size_t kRandomSummaryHead = 32;
constexpr size_t kRandomSummaryTail = 8;
constexpr int kMarkerLabelFontSize = 13;
constexpr float kMarkerLabelWidth = 74.0f;
constexpr float kMarkerLabelHeight = 18.0f;
constexpr float kMarkerLabelRasterStep = 0.25f;
constexpr float kMarkerLabelSupersample = 1.25f;
constexpr int kMarkerLabelMaxRasterFontSize = 128;
constexpr float kChartContentTopPadding = 16.0f;
constexpr float kChartContentBottomPadding = 24.0f;
constexpr float kCursorTapSlop = 10.0f;
constexpr float kCursorEndpointHorizontalSlopUi = 18.0f;
constexpr float kPlaybackAutofocusPaddingX = 48.0f;
constexpr float kPlaybackAutofocusPaddingY = 40.0f;
constexpr long long kPracticeLeadInMicros = 3000000LL;
constexpr int kNoGhostReplayId = -1;
constexpr int kPracticeGhostReplayId = -2;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct MarkerTextRasterConfig {
  int fontSize = kMarkerLabelFontSize;
  float textureToUiScale = 1.0f;
};

struct GaugeSelection {
  GaugeType type = GaugeType::Normal;
  GaugeAutoShiftMode autoShift = GaugeAutoShiftMode::None;
};

GaugeSelection gaugeSelectionFromSettingId(const std::string &id) {
  if (id == "gas") {
    return {.type = GaugeType::ExHard,
            .autoShift = GaugeAutoShiftMode::BestClear};
  }
  if (id == "gas_continue") {
    return {.type = GaugeType::ExHard,
            .autoShift = GaugeAutoShiftMode::Continue};
  }
  if (id == "gas_survival_to_groove") {
    return {.type = GaugeType::ExHard,
            .autoShift = GaugeAutoShiftMode::SurvivalToGroove};
  }
  if (id == "gas_best_clear") {
    return {.type = GaugeType::ExHard,
            .autoShift = GaugeAutoShiftMode::BestClear};
  }
  if (id == "gas_select_to_under") {
    return {.type = GaugeType::ExHard,
            .autoShift = GaugeAutoShiftMode::SelectToUnder};
  }
  if (id == "assisted_easy") {
    return {.type = GaugeType::AssistedEasy};
  }
  if (id == "easy") {
    return {.type = GaugeType::Easy};
  }
  if (id == "hard") {
    return {.type = GaugeType::Hard};
  }
  if (id == "exhard") {
    return {.type = GaugeType::ExHard};
  }
  if (id == "hazard") {
    return {.type = GaugeType::Hazard};
  }
  return {.type = GaugeType::Normal};
}

GaugeAutoShiftMode gaugeAutoShiftFromSettingId(const std::string &id) {
  if (id == "continue") return GaugeAutoShiftMode::Continue;
  if (id == "survival_to_groove") {
    return GaugeAutoShiftMode::SurvivalToGroove;
  }
  if (id == "best_clear") return GaugeAutoShiftMode::BestClear;
  if (id == "select_to_under") {
    return GaugeAutoShiftMode::SelectToUnder;
  }
  return GaugeAutoShiftMode::None;
}

ReplaySummary replaySummaryFromReplay(const ReplayData &replay,
                                      int summaryId,
                                      const std::string &createdAt) {
  ReplaySummary summary;
  summary.id = summaryId;
  summary.initialGaugeType = replay.initialGaugeType;
  summary.gaugeAutoShift = replay.gaugeAutoShift;
  summary.finalScore = replay.finalScore;
  summary.maxScore = std::max(0, replay.chartMeta.TotalNotes) * 2;
  summary.maxCombo = replay.maxCombo;
  summary.finalGauge = replay.finalGauge;
  summary.clearType = replay.clearType;
  summary.createdAt = createdAt;
  summary.eventCount = static_cast<int>(replay.events.size());
  summary.touchSampleCount = static_cast<int>(replay.touchSamples.size());
  summary.chartMeta = replay.chartMeta;
  summary.playOption = replay.playOption;
  summary.playOptionSeed = replay.playOptionSeed;
  summary.playOption2 = replay.playOption2;
  summary.playOption2Seed = replay.playOption2Seed;
  summary.assistOption = replay.assistOption;
  return summary;
}

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  insets.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  insets.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
  insets.bottom = static_cast<int>(std::lround(
      normalized.bottom * static_cast<float>(rendering::window_height)));
#endif
  return insets;
}

std::string formatDouble(double value, int precision = 3) {
  if (!std::isfinite(value)) {
    return "0";
  }
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  std::string text = stream.str();
  if (text.find('.') != std::string::npos) {
    while (text.size() > 1 && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

std::string joinRandomValues(const std::vector<int> &values) {
  std::ostringstream stream;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      stream << "-";
    }
    stream << values[i];
  }
  return stream.str();
}

std::string joinRandomValueRange(const std::vector<int> &values, size_t start,
                                 size_t end) {
  std::ostringstream stream;
  end = std::min(end, values.size());
  for (size_t i = start; i < end; ++i) {
    if (i > start) {
      stream << "-";
    }
    stream << values[i];
  }
  return stream.str();
}

std::string formatMicrosTime(long long micros) {
  micros = std::max(0LL, micros);
  const long long totalMillis = micros / 1000LL;
  const long long minutes = totalMillis / 60000LL;
  const long long seconds = (totalMillis / 1000LL) % 60LL;
  const long long millis = totalMillis % 1000LL;
  std::ostringstream stream;
  stream << minutes << ":" << std::setw(2) << std::setfill('0') << seconds
         << "." << std::setw(3) << std::setfill('0') << millis;
  return stream.str();
}

uint32_t markerLabelColorKey(const SDL_Color &color) {
  return (static_cast<uint32_t>(color.r) << 24) |
         (static_cast<uint32_t>(color.g) << 16) |
         (static_cast<uint32_t>(color.b) << 8) |
         static_cast<uint32_t>(color.a);
}

std::string markerGlyphCacheKey(char glyph, const SDL_Color &color,
                                int fontSize) {
  std::string key = std::to_string(markerLabelColorKey(color));
  key.push_back(':');
  key += std::to_string(fontSize);
  key.push_back(':');
  key.push_back(glyph);
  return key;
}

MarkerTextRasterConfig markerTextRasterConfig(float labelTextScale) {
  labelTextScale = std::max(1.0f, labelTextScale);
  const float uiScale =
      std::max({1.0f, rendering::ui_scale_x, rendering::ui_scale_y});
  const float rawRasterScale =
      labelTextScale * uiScale * kMarkerLabelSupersample;
  const float steppedRasterScale =
      std::ceil(rawRasterScale / kMarkerLabelRasterStep) *
      kMarkerLabelRasterStep;
  const int fontSize = std::clamp(
      static_cast<int>(std::ceil(static_cast<float>(kMarkerLabelFontSize) *
                                 steppedRasterScale)),
      kMarkerLabelFontSize, kMarkerLabelMaxRasterFontSize);
  const float effectiveRasterScale =
      static_cast<float>(fontSize) / static_cast<float>(kMarkerLabelFontSize);
  return {fontSize, effectiveRasterScale / labelTextScale};
}

std::optional<std::string>
storedPlayOption(const std::optional<std::string> &option) {
  if (!option.has_value()) {
    return std::nullopt;
  }
  const std::string normalized = play_options::normalizePlayOption(*option);
  if (play_options::isNormalPlayOption(normalized)) {
    return std::nullopt;
  }
  return normalized;
}

bool usesBlueSymmetricKeyColor(size_t keyPosition, size_t keyLaneCount) {
  if (keyLaneCount == 0 || keyPosition >= keyLaneCount) {
    return false;
  }
  const size_t mirroredPosition =
      std::min(keyPosition, keyLaneCount - keyPosition - 1);
  return (mirroredPosition & 1U) != 0;
}

bool isLaneOrderSummaryOption(const std::optional<std::string> &option) {
  const std::string normalized =
      option.has_value() ? play_options::normalizePlayOption(*option)
                         : "NORMAL";
  return normalized == "NORMAL" || normalized == "MIRROR" ||
         normalized == "RANDOM" || normalized == "R-RANDOM" ||
         normalized == "RANDOM-EX";
}

std::optional<std::string>
formatLaneOrderSummary(const bms_parser::ChartMeta &meta,
                       const std::vector<int> &laneOrder) {
  const std::vector<int> destinationLanes = meta.GetTotalLaneIndices();
  if (destinationLanes.empty() || laneOrder.size() != destinationLanes.size()) {
    return std::nullopt;
  }

  std::unordered_map<int, char> laneToSymbol;
  const auto scratchLanes = meta.GetScratchLaneIndices();
  if (meta.IsDP) {
    if (scratchLanes.size() >= 2) {
      laneToSymbol[scratchLanes.front()] = 'L';
      laneToSymbol[scratchLanes.back()] = 'R';
    }
  } else if (!scratchLanes.empty()) {
    laneToSymbol[scratchLanes.front()] = 'S';
  }

  constexpr std::string_view keySymbols = "123456789ABCDE";
  const auto keyLanes = meta.GetKeyLaneIndices();
  if (keyLanes.size() > keySymbols.size()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < keyLanes.size(); ++i) {
    laneToSymbol[keyLanes[i]] = keySymbols[i];
  }

  std::string result;
  result.reserve(laneOrder.size());
  for (int sourceLane : laneOrder) {
    const auto symbol = laneToSymbol.find(sourceLane);
    if (symbol == laneToSymbol.end()) {
      return std::nullopt;
    }
    result.push_back(symbol->second);
  }
  return result;
}

bool matchHeader(std::string_view line, std::string_view headerUpper) {
  if (line.size() < headerUpper.size()) {
    return false;
  }
  for (size_t i = 0; i < headerUpper.size(); ++i) {
    const auto c = static_cast<unsigned char>(line[i]);
    if (std::toupper(c) != headerUpper[i]) {
      return false;
    }
  }
  return true;
}

Button *makeButton(const std::string &label, int width, int fontSize,
                   TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, width, kHeaderButtonHeight);
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  button->setContentView(text);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setBackgroundColors(ui_theme::control(), ui_theme::controlHover(),
                              ui_theme::controlPressed());
  button->setBorderColors(ui_theme::hairline(), ui_theme::cyan(),
                          ui_theme::cyan());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

GaugeProfile
practiceGaugeProfileForChart(const bms_parser::Chart *chart) {
  return resolveGaugeProfile(GaugeProfile::Standard,
                             chart != nullptr ? chart->Meta.KeyMode : 7);
}

int practiceStartingGaugeMaximum(
    const practice::Configuration &configuration,
    const bms_parser::Chart *chart) {
  const GaugeProfile gaugeProfile = practiceGaugeProfileForChart(chart);
  return static_cast<int>(gaugeStartingMaximumValue(
      configuration.gaugeType, configuration.gaugeAutoShift,
      configuration.gaugeAutoShiftLowerBound, gaugeProfile));
}

practice::SanitizedConfiguration sanitizePracticeConfiguration(
    practice::Configuration configuration, long long chartEndMicros,
    const bms_parser::Chart *chart) {
  const int startingGaugeMaximum =
      practiceStartingGaugeMaximum(configuration, chart);
  return practice::sanitize(std::move(configuration), chartEndMicros,
                            startingGaugeMaximum);
}

} // namespace

class ChartCanvasView : public View {
public:
  ChartCanvasView() {
    setBackgroundColor(Color(8, 9, 11, 255));
    batch.setSubmitView(rendering::ui_view);
    markerTextBatch.setSubmitView(rendering::ui_view);
  }

  void setChart(bms_parser::Chart *newChart) {
    const practice::RangeSelection previousRange = practiceRange;
    const bool hadPracticeRange = practiceRangeSet;
    chart = newChart;
    markerGlyphTextures.clear();
    playbackActive = false;
    replayGhostEvents.clear();
    rebuildLayout();
    practiceRangeSet = false;
    if (chart == nullptr || !hadPracticeRange) {
      return;
    }
    setPracticeRange(previousRange);
  }

  void setShowInvisibleNotes(bool enabled) { showInvisibleNotes = enabled; }

  void setZoom(float newZoom) {
    const std::optional<ZoomFocusAnchor> focusAnchor =
        zoomFocusAnchorAtViewportCenter();
    const float previousZoom = zoom;
    zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
    if (std::abs(zoom - previousZoom) <= 0.001f) {
      return;
    }
    rebuildLayout();
    if (focusAnchor.has_value()) {
      focusZoomAnchor(*focusAnchor);
    }
  }

  [[nodiscard]] float getZoom() const { return zoom; }

  void setSelectionListener(std::function<void(long long)> listener) {
    selectionListener = std::move(listener);
  }

  void setPracticeRange(const practice::RangeSelection &range) {
    practiceRange = range;
    const long long chartEnd =
        chart == nullptr
            ? std::max({0LL, range.startMicros, range.endMicros})
            : chart_playback_duration::ChartTimelineEndMicros(*chart);
    practiceRange.startMicros =
        std::clamp(practiceRange.startMicros, 0LL, std::max(0LL, chartEnd));
    practiceRange.endMicros =
        std::clamp(practiceRange.endMicros, 0LL, std::max(0LL, chartEnd));
    if (practiceRange.startMicros > practiceRange.endMicros) {
      std::swap(practiceRange.startMicros, practiceRange.endMicros);
      practiceRange.active = practiceRange.active == practice::Marker::Start
                                 ? practice::Marker::End
                                 : practice::Marker::Start;
    }
    practiceRangeSet = true;
  }

  [[nodiscard]] practice::RangeSelection getPracticeRange() const {
    return practiceRange;
  }

  void setActivePracticeMarker(practice::Marker marker) {
    practiceRange.active = marker;
  }

  bool moveActivePracticeMarker(practice::TimelineDirection direction) {
    if (chart == nullptr || !practiceRangeSet) {
      return false;
    }
    std::vector<long long> timelineMicros;
    timelineMicros.reserve(orderedTimelines.size());
    for (const auto *timeline : orderedTimelines) {
      if (timeline != nullptr) {
        timelineMicros.push_back(timeline->Timing);
      }
    }
    std::ranges::sort(timelineMicros);
    const auto adjacent = practice::adjacentTimelineMicros(
        timelineMicros, getSelectedTimeMicros(), direction);
    if (!adjacent) {
      return false;
    }
    practiceRange.placeActiveMarker(
        *adjacent, chart_playback_duration::ChartTimelineEndMicros(*chart));
    playbackActive = false;
    if (selectionListener != nullptr) {
      selectionListener(getSelectedTimeMicros());
    }
    if (practiceRangeListener != nullptr) {
      practiceRangeListener(practiceRange);
    }
    return true;
  }

  void setPracticeRangeListener(
      std::function<void(const practice::RangeSelection &)> listener) {
    practiceRangeListener = std::move(listener);
  }

  [[nodiscard]] bool hasSelectedTime() const {
    return practiceRangeSet;
  }

  [[nodiscard]] long long getSelectedTimeMicros() const {
    return practiceRange.active == practice::Marker::Start
               ? practiceRange.startMicros
               : practiceRange.endMicros;
  }

  void setPlaybackTime(long long timeMicros, bool active) {
    playbackTimeMicros = std::max(0LL, timeMicros);
    playbackActive = active;
    if (playbackActive) {
      autofocusPlaybackCursor();
    }
  }

  void clearPlaybackTime() { playbackActive = false; }

  void setGhostReplay(const ReplayData &replayData) {
    replayGhostEvents = replay_ghost::buildReplayGhostEvents(
        replayData, orderedTimelines, laneToOrderIndex,
        [this](long long timeMicros) { return timeToBeatPosition(timeMicros); });
    replayMissMarkers = replay_ghost::buildReplayMissMarkers(
        replayData, orderedTimelines, laneToOrderIndex,
        [this](long long timeMicros) { return timeToBeatPosition(timeMicros); });
  }

  void clearGhostReplay() {
    replayGhostEvents.clear();
    replayMissMarkers.clear();
  }

protected:
  struct TouchPoint {
    float x = 0.0f;
    float y = 0.0f;
  };

  void renderImpl(RenderContext &context) override {
    if (chart == nullptr) {
      return;
    }

    if (layoutDirty) {
      rebuildLayout();
    }

    batch.begin(context.getTransformMatrix());
    drawGrid();
    drawPracticeRangeSpan();
    drawMarkers();
    drawLongNotes();
    drawNotes();
    drawInvisibleNotes();
    drawGhosts();
    drawCursorBars();
    batch.end();

    ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
    renderLabels(context);
  }

  bool handleEventsImpl(SDL_Event &event) override {
    switch (event.type) {
    case SDL_MOUSEWHEEL: {
      int rawX = 0;
      int rawY = 0;
      SDL_GetMouseState(&rawX, &rawY);
      const int screenX = static_cast<int>(rawX * rendering::widthScale);
      const int screenY = static_cast<int>(rawY * rendering::heightScale);
      int uiX = 0;
      int uiY = 0;
      rendering::screenToUi(screenX, screenY, uiX, uiY);
      if (!containsPoint(static_cast<float>(uiX), static_cast<float>(uiY))) {
        return true;
      }
      const float horizontal =
          event.wheel.x != 0 ? static_cast<float>(-event.wheel.x)
                             : static_cast<float>(-event.wheel.y);
      scrollX += horizontal * 88.0f / screenZoom;
      scrollY += static_cast<float>(-event.wheel.y) * 12.0f / screenZoom;
      clampScroll();
      return false;
    }
    case SDL_MOUSEBUTTONDOWN: {
      if (event.button.button != SDL_BUTTON_LEFT ||
          event.button.which == SDL_TOUCH_MOUSEID) {
        return true;
      }
      int uiX = 0;
      int uiY = 0;
      mouseEventToUi(event.button, uiX, uiY);
      if (!containsPoint(static_cast<float>(uiX), static_cast<float>(uiY))) {
        return true;
      }
      mouseDragging = true;
      lastMouseX = uiX;
      lastMouseY = uiY;
      mouseStartX = uiX;
      mouseStartY = uiY;
      mouseDragDistance = 0.0f;
      return false;
    }
    case SDL_MOUSEMOTION: {
      if (!mouseDragging) {
        return true;
      }
      int uiX = 0;
      int uiY = 0;
      mouseMotionToUi(event.motion, uiX, uiY);
      const int dx = uiX - lastMouseX;
      const int dy = uiY - lastMouseY;
      scrollX -= static_cast<float>(dx) / screenZoom;
      scrollY -= static_cast<float>(dy) / screenZoom;
      lastMouseX = uiX;
      lastMouseY = uiY;
      mouseDragDistance =
          std::max(mouseDragDistance,
                   std::hypot(static_cast<float>(uiX - mouseStartX),
                              static_cast<float>(uiY - mouseStartY)));
      clampScroll();
      return false;
    }
    case SDL_MOUSEBUTTONUP:
      if (event.button.button == SDL_BUTTON_LEFT && mouseDragging) {
        int uiX = 0;
        int uiY = 0;
        mouseEventToUi(event.button, uiX, uiY);
        if (mouseDragDistance <= kCursorTapSlop &&
            containsPoint(static_cast<float>(uiX), static_cast<float>(uiY))) {
          selectAtUiPoint(static_cast<float>(uiX), static_cast<float>(uiY));
        }
        mouseDragging = false;
        return false;
      }
      return true;
    case SDL_FINGERDOWN: {
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
      if (!containsPoint(uiX, uiY)) {
        return true;
      }
      const SDL_FingerID fingerId = event.tfinger.fingerId;
      activeTouches[fingerId] = {uiX, uiY};
      if (activeTouches.size() >= 2) {
        touchGestureWasPinch = true;
        beginPinch();
      } else if (activeTouches.size() == 1) {
        pinchActive = false;
        dragTouchId = fingerId;
        touchGestureWasPinch = false;
        touchStartX = uiX;
        touchStartY = uiY;
        touchDragDistance = 0.0f;
      }
      return false;
    }
    case SDL_FINGERMOTION: {
      auto touchIt = activeTouches.find(event.tfinger.fingerId);
      if (touchIt == activeTouches.end()) {
        return true;
      }
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
      const TouchPoint previous = touchIt->second;
      touchIt->second = {uiX, uiY};
      if (activeTouches.size() >= 2) {
        touchGestureWasPinch = true;
        applyPinch();
      } else if (!pinchActive && event.tfinger.fingerId == dragTouchId) {
        scrollX -= (uiX - previous.x) / screenZoom;
        scrollY -= (uiY - previous.y) / screenZoom;
        touchDragDistance =
            std::max(touchDragDistance,
                     std::hypot(uiX - touchStartX, uiY - touchStartY));
        clampScroll();
      }
      return false;
    }
    case SDL_FINGERUP:
      if (activeTouches.erase(event.tfinger.fingerId) > 0) {
        float uiX = 0.0f;
        float uiY = 0.0f;
        rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
        if (!touchGestureWasPinch && event.tfinger.fingerId == dragTouchId &&
            touchDragDistance <= kCursorTapSlop && containsPoint(uiX, uiY)) {
          selectAtUiPoint(uiX, uiY);
        }
        if (activeTouches.size() >= 2) {
          touchGestureWasPinch = true;
          beginPinch();
        } else if (activeTouches.size() == 1) {
          pinchActive = false;
          dragTouchId = activeTouches.begin()->first;
          touchGestureWasPinch = true;
          touchStartX = activeTouches.begin()->second.x;
          touchStartY = activeTouches.begin()->second.y;
          touchDragDistance = 0.0f;
        } else {
          pinchActive = false;
          dragTouchId = -1;
          touchGestureWasPinch = false;
        }
        return false;
      }
      return true;
    default:
      return true;
    }
  }

  void onResize(int newWidth, int newHeight) override {
    (void)newWidth;
    (void)newHeight;
    rebuildLayout();
  }

private:
  enum class MarkerType { Bpm, Stop, Scroll };

  struct MeasureLayout {
    int measureIndex = 0;
    int column = 0;
    float x = 0.0f;
    float y = 0.0f;
    float height = 0.0f;
    double beatStart = 0.0;
    double scale = 1.0;
  };

  struct ColumnLayout {
    float x = 0.0f;
    float yTop = 0.0f;
    float yBottom = 0.0f;
  };

  struct ZoomFocusAnchor {
    std::vector<int> measureIndices;
    int centerMeasureIndex = -1;
    float centerMeasureLocalY = 0.5f;
  };

  struct MarkerLabel {
    const bms_parser::TimeLine *timeline = nullptr;
    MarkerType type = MarkerType::Bpm;
    std::string text;
    SDL_Color color{255, 255, 255, 255};
  };

  struct CachedMarkerGlyph {
    std::unique_ptr<TextView> text;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    float width = 0.0f;
    float height = 0.0f;
  };

  struct MarkerLabelDraw {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float u1 = 1.0f;
  };

  struct CursorDrawPosition {
    int column = 0;
    float x = 0.0f;
    float y = 0.0f;
  };

  enum class BoundaryPreference { PreferEnd, PreferStart };

  enum class CursorSegmentSide { Full, Above, Below };

  bms_parser::Chart *chart = nullptr;
  rendering::SimpleBatchRenderer batch;
  rendering::TexBatchRenderer markerTextBatch;
  std::vector<int> laneOrder;
  std::unordered_map<int, size_t> laneToOrderIndex;
  std::unordered_map<int, bool> laneUsesBlueNoteColor;
  std::vector<MeasureLayout> measureLayouts;
  std::vector<ColumnLayout> columnLayouts;
  std::vector<const bms_parser::TimeLine *> orderedTimelines;
  std::vector<ReplayGhostEvent> replayGhostEvents;
  std::vector<ReplayMissMarker> replayMissMarkers;
  std::unordered_map<const bms_parser::TimeLine *, float> timelineY;
  std::unordered_map<const bms_parser::TimeLine *, size_t> timelineMeasure;
  std::vector<std::unique_ptr<TextView>> measureLabels;
  std::vector<MarkerLabel> markerLabels;
  std::unordered_map<std::string, CachedMarkerGlyph> markerGlyphTextures;
  std::vector<MarkerLabelDraw> visibleMarkerLabelDraws;
  float zoom = 1.0f;
  float laneWidth = 24.0f;
  float laneAreaWidth = 0.0f;
  float gutterWidth = 54.0f;
  float columnGap = 17.0f;
  float columnWidth = 0.0f;
  float contentLeftMargin = 72.0f;
  float contentRightMargin = 72.0f;
  float contentWidth = 0.0f;
  float contentHeight = 0.0f;
  double totalBeatLength = 0.0;
  float scrollX = 0.0f;
  float scrollY = 0.0f;
  float screenZoom = 1.0f;
  bool layoutDirty = false;
  bool mouseDragging = false;
  int lastMouseX = 0;
  int lastMouseY = 0;
  int mouseStartX = 0;
  int mouseStartY = 0;
  float mouseDragDistance = 0.0f;
  std::unordered_map<SDL_FingerID, TouchPoint> activeTouches;
  SDL_FingerID dragTouchId = -1;
  bool pinchActive = false;
  bool touchGestureWasPinch = false;
  float touchStartX = 0.0f;
  float touchStartY = 0.0f;
  float touchDragDistance = 0.0f;
  float pinchStartDistance = 1.0f;
  float pinchStartScreenZoom = 1.0f;
  float pinchAnchorContentX = 0.0f;
  float pinchAnchorContentY = 0.0f;
  practice::RangeSelection practiceRange;
  bool practiceRangeSet = false;
  long long playbackTimeMicros = 0;
  bool playbackActive = false;
  bool showInvisibleNotes = false;
  std::function<void(long long)> selectionListener;
  std::function<void(const practice::RangeSelection &)> practiceRangeListener;

  void rebuildLayout() {
    layoutDirty = false;
    measureLayouts.clear();
    columnLayouts.clear();
    orderedTimelines.clear();
    timelineY.clear();
    timelineMeasure.clear();
    measureLabels.clear();
    markerLabels.clear();
    laneToOrderIndex.clear();
    laneUsesBlueNoteColor.clear();

    if (chart == nullptr) {
      contentWidth = 0.0f;
      contentHeight = 0.0f;
      totalBeatLength = 0.0;
      return;
    }

    laneOrder = chart->Meta.GetTotalLaneIndices();
    if (laneOrder.empty()) {
      laneOrder = chart->Meta.GetKeyLaneIndices();
    }
    const size_t laneCount = std::max<size_t>(1, laneOrder.size());
    for (size_t i = 0; i < laneOrder.size(); ++i) {
      laneToOrderIndex[laneOrder[i]] = i;
    }
    std::vector<int> keyLanes;
    keyLanes.reserve(laneOrder.size());
    for (int lane : laneOrder) {
      if (!isScratchLane(lane)) {
        keyLanes.push_back(lane);
      }
    }
    laneUsesBlueNoteColor.reserve(keyLanes.size());
    for (size_t keyPosition = 0; keyPosition < keyLanes.size(); ++keyPosition) {
      laneUsesBlueNoteColor[keyLanes[keyPosition]] =
          usesBlueSymmetricKeyColor(keyPosition, keyLanes.size());
    }

    laneWidth = laneCount > 12 ? 18.0f : (laneCount > 8 ? 21.0f : 24.0f);
    laneWidth *= std::clamp(zoom, 0.8f, 1.35f);
    gutterWidth = 54.0f;
    laneAreaWidth = static_cast<float>(laneCount) * laneWidth;
    columnGap = 17.0f;
    columnWidth = gutterWidth + laneAreaWidth + columnGap + 78.0f;
    const SafeAreaInsets safe = getSafeAreaInsetsUi();
    const float horizontalSafeInset =
        static_cast<float>(std::max(safe.left, safe.right));
    const float baseSideMargin =
        std::clamp(static_cast<float>(getWidth()) * 0.12f, 84.0f, 156.0f);
    contentLeftMargin = baseSideMargin + horizontalSafeInset;
    contentRightMargin = baseSideMargin + horizontalSafeInset;
    const float maxColumnHeight =
        std::max(280.0f, static_cast<float>(getHeight()) -
                             kChartContentTopPadding -
                             kChartContentBottomPadding);
    const float baseMeasureHeight = 272.0f * zoom;
    totalBeatLength = 0.0;
    for (const auto *measure : chart->Measures) {
      totalBeatLength += measure == nullptr ? 1.0 : measure->Scale;
    }

    std::vector<float> measureHeights;
    measureHeights.reserve(chart->Measures.size());
    for (const auto *measure : chart->Measures) {
      const double scale = measure == nullptr ? 1.0 : measure->Scale;
      measureHeights.push_back(
          std::max(22.0f, baseMeasureHeight * static_cast<float>(
                                           std::max(0.05, scale))));
    }

    int column = 0;
    size_t groupStart = 0;
    while (groupStart < measureHeights.size()) {
      float groupHeight = 0.0f;
      size_t groupEnd = groupStart;
      while (groupEnd < measureHeights.size()) {
        const float nextHeight = measureHeights[groupEnd];
        if (groupEnd > groupStart && groupHeight + nextHeight > maxColumnHeight) {
          break;
        }
        groupHeight += nextHeight;
        ++groupEnd;
      }

      const float columnX = contentLeftMargin +
                            static_cast<float>(column) * columnWidth;
      const float columnBottom = kChartContentTopPadding + maxColumnHeight;
      float cursorY = columnBottom;
      ColumnLayout columnLayout{columnX, columnBottom,
                                kChartContentTopPadding};
      double beatStart = 0.0;
      for (size_t i = 0; i < groupStart; ++i) {
        const auto *measure = chart->Measures[i];
        beatStart += measure == nullptr ? 1.0 : measure->Scale;
      }

      for (size_t i = groupStart; i < groupEnd; ++i) {
        const auto *measure = chart->Measures[i];
        const double scale = measure == nullptr ? 1.0 : measure->Scale;
        const float height = measureHeights[i];
        cursorY -= height;
        measureLayouts.push_back({static_cast<int>(i), column, columnX,
                                  cursorY, height, beatStart, scale});
        columnLayout.yTop = std::min(columnLayout.yTop, cursorY);
        columnLayout.yBottom = std::max(columnLayout.yBottom, cursorY + height);
        beatStart += scale;
      }

      columnLayouts.push_back(columnLayout);
      groupStart = groupEnd;
      ++column;
    }

    contentHeight =
        kChartContentTopPadding + maxColumnHeight + kChartContentBottomPadding;
    contentWidth =
        columnLayouts.empty()
            ? 0.0f
            : columnLayouts.back().x + gutterWidth + laneAreaWidth + 78.0f +
                  contentRightMargin;

    for (size_t layoutIndex = 0; layoutIndex < measureLayouts.size();
         ++layoutIndex) {
      auto &layout = measureLayouts[layoutIndex];
      const auto *measure = chart->Measures[layout.measureIndex];
      auto label = std::make_unique<TextView>("assets/fonts/notosanscjkjp.ttf",
                                              16);
      label->setText(std::to_string(layout.measureIndex));
      label->setColor({245, 247, 250, 255});
      label->setAlign(TextView::CENTER);
      label->setVAlign(TextView::MIDDLE);
      label->setOverflow(TextView::TextOverflow::Hidden);
      label->setSize(static_cast<int>(gutterWidth), 22);
      measureLabels.push_back(std::move(label));

      if (measure == nullptr || layout.scale <= 0.0) {
        continue;
      }
      for (const auto *timeline : measure->TimeLines) {
        if (timeline == nullptr) {
          continue;
        }
        const double localBeat = std::clamp(
            (timeline->BeatPosition - layout.beatStart) / layout.scale, 0.0,
            1.0);
        const float y =
            layout.y + layout.height - static_cast<float>(localBeat) *
                                            layout.height;
        orderedTimelines.push_back(timeline);
        timelineY[timeline] = y;
        timelineMeasure[timeline] = layoutIndex;
        appendMarkerLabel(timeline);
      }
    }

    clampScroll();
  }

  void appendMarkerLabel(const bms_parser::TimeLine *timeline) {
    auto addLabel = [&](MarkerType type, std::string text, SDL_Color color) {
      markerLabels.push_back({timeline, type, std::move(text), color});
    };

    if (timeline->BpmChange) {
      addLabel(MarkerType::Bpm, formatDouble(timeline->Bpm),
               {132, 224, 124, 255});
    }
    if (timeline->StopLength > 0.0) {
      addLabel(MarkerType::Stop,
               formatDouble(timeline->StopLength, 1) + " STOP",
               {242, 211, 80, 255});
    }
    if (timeline->ScrollChange) {
      addLabel(MarkerType::Scroll,
               "SCROLL " + formatDouble(timeline->Scroll),
               {101, 205, 208, 255});
    }
  }

  void drawGrid() {
    const uint32_t gutterColor = Color(43, 46, 47, 248).toABGR();
    const uint32_t gutterAccent = Color(86, 98, 99, 215).toABGR();
    const uint32_t laneBackground = Color(11, 12, 13, 255).toABGR();
    const uint32_t majorLine = Color(120, 128, 130, 178).toABGR();
    const uint32_t minorLine = Color(58, 64, 65, 148).toABGR();
    const uint32_t fineLine = Color(34, 38, 39, 128).toABGR();

    for (const auto &layout : measureLayouts) {
      if (!contentRectIntersects(layout.x, layout.y, gutterWidth + laneAreaWidth,
                                 layout.height)) {
        continue;
      }
      const float laneX = layout.x + gutterWidth;
      drawRectClip(layout.x, layout.y, gutterWidth, layout.height, gutterColor);
      drawRectClip(layout.x + gutterWidth - 2.0f, layout.y, 2.0f,
                   layout.height, gutterAccent);
      drawRectClip(laneX, layout.y, laneAreaWidth, layout.height,
                   laneBackground);
      drawRectClip(layout.x, layout.y, gutterWidth + laneAreaWidth, 1.5f,
                   majorLine);
      drawRectClip(layout.x, layout.y + layout.height - 1.5f,
                   gutterWidth + laneAreaWidth, 1.5f, majorLine);

      for (int i = 1; i < 16; ++i) {
        const float y = layout.y + layout.height * static_cast<float>(i) / 16.0f;
        drawRectClip(laneX, y, laneAreaWidth, i % 4 == 0 ? 1.1f : 0.8f,
                     i % 4 == 0 ? minorLine : fineLine);
      }
      for (size_t i = 0; i <= laneOrder.size(); ++i) {
        const float x = laneX + static_cast<float>(i) * laneWidth;
        drawRectClip(x, layout.y, i == 0 || i == laneOrder.size() ? 1.4f : 1.0f,
                     layout.height, i == 0 || i == laneOrder.size()
                                        ? majorLine
                                        : minorLine);
      }
    }
  }

  void drawLongNotes() {
    if (chart == nullptr) {
      return;
    }
    forEachNote([&](int lane, const bms_parser::Note *note,
                    const bms_parser::TimeLine *timeline) {
      const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
      if (longNote == nullptr || longNote->Head != nullptr ||
          longNote->Tail == nullptr || longNote->Tail->Timeline == nullptr) {
        return;
      }
      drawLongNoteBody(lane, timeline, longNote->Tail->Timeline);
    });
  }

  void drawNotes() {
    forEachNote([&](int lane, const bms_parser::Note *note,
                    const bms_parser::TimeLine *timeline) {
      auto yIt = timelineY.find(timeline);
      auto layoutIt = timelineMeasure.find(timeline);
      if (yIt == timelineY.end() || layoutIt == timelineMeasure.end()) {
        return;
      }
      const auto &layout = measureLayouts[layoutIt->second];
      const float x = laneContentX(layout.column, lane) + 2.0f;
      const float y = yIt->second - 3.0f;
      const float width = std::max(5.0f, laneWidth - 4.0f);
      const bool isLongNote =
          dynamic_cast<const bms_parser::LongNote *>(note) != nullptr;
      const float height = isLongNote ? 7.0f : 6.0f;
      drawRectClip(x, y, width, height, noteColor(lane, note));
    });
  }

  void drawInvisibleNotes() {
    if (!showInvisibleNotes) {
      return;
    }
    forEachInvisibleNote([&](int lane, const bms_parser::Note *note,
                             const bms_parser::TimeLine *timeline) {
      auto yIt = timelineY.find(timeline);
      auto layoutIt = timelineMeasure.find(timeline);
      if (yIt == timelineY.end() || layoutIt == timelineMeasure.end()) {
        return;
      }
      const auto &layout = measureLayouts[layoutIt->second];
      const float x = laneContentX(layout.column, lane) + 2.0F;
      const float y = yIt->second - 3.0F;
      const float width = std::max(5.0F, laneWidth - 4.0F);
      constexpr float height = 6.0F;
      const bool isLongNote =
          dynamic_cast<const bms_parser::LongNote *>(note) != nullptr;
      const float borderThickness =
          height *
          chart_viewer_note_geometry::kInvisibleNoteBorderHeightRatio;
      const auto rectangles =
          chart_viewer_note_geometry::invisibleNoteRectangles(
              x, y, width, height, borderThickness, isLongNote);
      for (std::size_t i = 0; i < rectangles.count; ++i) {
        const auto &rectangle = rectangles.rectangles[i];
        drawRectClip(rectangle.x, rectangle.y, rectangle.width,
                     rectangle.height, invisibleNoteColor());
      }
    });
  }

  void drawGhosts() {
    if (replayGhostEvents.empty() && replayMissMarkers.empty()) {
      return;
    }

    for (const auto &event : replayGhostEvents) {
      CursorDrawPosition position;
      if (!beatToCursorPosition(event.judgeScrollPosition, position)) {
        continue;
      }
      const float x = laneContentX(position.column, event.lane) + 2.0f;
      const float y = position.y - 4.0f;
      const float width = std::max(5.0f, laneWidth - 4.0f);
      const float height = 8.0f;
      if (!contentRectIntersects(x, y, width, height)) {
        continue;
      }

      const uint32_t color = ghostColor(event).toABGR();
      const float thickness = std::max(1.25f, std::min(2.4f, laneWidth * 0.12f));
      drawRectClip(x, y, width, thickness, color);
      drawRectClip(x, y + height - thickness, width, thickness, color);
      drawRectClip(x, y, thickness, height, color);
      drawRectClip(x + width - thickness, y, thickness, height, color);
    }

    for (const auto &marker : replayMissMarkers) {
      CursorDrawPosition position;
      if (!beatToCursorPosition(marker.noteScrollPosition, position,
                                BoundaryPreference::PreferStart)) {
        continue;
      }
      const float x = laneContentX(position.column, marker.lane) + 2.0f;
      const float y = position.y - 4.0f;
      const float width = std::max(5.0f, laneWidth - 4.0f);
      const float height = 8.0f;
      drawMissMarkerX(x, y, width, height);
    }
  }

  void drawMarkers() {
    const uint32_t bpmColor = Color(123, 220, 117, 226).toABGR();
    const uint32_t stopColor = Color(238, 202, 72, 218).toABGR();
    const uint32_t scrollColor = Color(92, 196, 198, 220).toABGR();

    for (const auto &[timeline, y] : timelineY) {
      auto layoutIt = timelineMeasure.find(timeline);
      if (layoutIt == timelineMeasure.end()) {
        continue;
      }
      const auto &layout = measureLayouts[layoutIt->second];
      const float laneX = layout.x + gutterWidth;
      if (timeline->StopLength > 0.0) {
        const float barHeight = std::clamp(
            3.0f + static_cast<float>(timeline->GetStopDuration()) / 180000.0f,
            3.0f, 14.0f);
        drawRectClip(laneX, y - barHeight * 0.5f, laneAreaWidth, barHeight,
                     stopColor);
      }
      if (timeline->BpmChange) {
        drawRectClip(laneX, y - 1.0f, laneAreaWidth, 2.0f, bpmColor);
      }
      if (timeline->ScrollChange) {
        drawRectClip(laneX, y + 2.0f, laneAreaWidth, 2.0f, scrollColor);
      }
    }
  }

  void drawCursorBars() {
    if (practiceRangeSet) {
      drawCursorBar(practiceRange.startMicros, Color(77, 220, 236, 240),
                    practiceRange.active == practice::Marker::Start ? 4.5f
                                                                    : 3.0f);
      drawCursorBar(practiceRange.endMicros, Color(255, 190, 66, 240),
                    practiceRange.active == practice::Marker::End ? 4.5f
                                                                  : 3.0f);
    }
    if (playbackActive) {
      drawCursorBar(playbackTimeMicros, Color(91, 218, 236, 242), 3.0f);
    }
  }

  void drawPracticeRangeSpan() {
    if (!practiceRangeSet ||
        practiceRange.startMicros >= practiceRange.endMicros) {
      return;
    }
    const double startBeat = timeToBeatPosition(practiceRange.startMicros);
    const double endBeat = timeToBeatPosition(practiceRange.endMicros);
    const uint32_t fill = Color(31, 173, 173, 56).toABGR();
    for (const auto &layout : measureLayouts) {
      if (layout.scale <= 0.0) {
        continue;
      }
      const double overlapStart = std::max(startBeat, layout.beatStart);
      const double overlapEnd =
          std::min(endBeat, layout.beatStart + layout.scale);
      if (overlapStart >= overlapEnd) {
        continue;
      }
      const auto startPosition = cursorPositionForLayout(layout, overlapStart);
      const auto endPosition = cursorPositionForLayout(layout, overlapEnd);
      drawRectClip(layout.x + gutterWidth, endPosition.y, laneAreaWidth,
                   std::max(0.0f, startPosition.y - endPosition.y), fill);
    }
  }

  void drawCursorBar(long long timeMicros, Color color, float thickness) {
    drawCursorBarAtBeat(timeToBeatPosition(timeMicros), color, thickness);
  }

  void drawCursorBarAtBeat(double beatPosition, Color color, float thickness) {
    const std::vector<CursorDrawPosition> positions =
        beatToCursorPositions(beatPosition);
    if (positions.empty()) {
      return;
    }
    if (positions.size() == 1) {
      drawCursorBarSegment(positions.front(), color, thickness,
                           CursorSegmentSide::Full);
      return;
    }

    for (const auto &position : positions) {
      CursorSegmentSide side = CursorSegmentSide::Full;
      if (position.column >= 0 &&
          position.column < static_cast<int>(columnLayouts.size())) {
        const auto &column =
            columnLayouts[static_cast<size_t>(position.column)];
        constexpr float kEndpointEpsilon = 0.5f;
        if (std::abs(position.y - column.yTop) <= kEndpointEpsilon) {
          side = CursorSegmentSide::Below;
        } else if (std::abs(position.y - column.yBottom) <=
                   kEndpointEpsilon) {
          side = CursorSegmentSide::Above;
        }
      }
      drawCursorBarSegment(position, color, thickness, side);
    }
  }

  void drawCursorBarSegment(const CursorDrawPosition &position, Color color,
                            float thickness, CursorSegmentSide side) {
    const float laneX = position.x + gutterWidth;
    float laneY = position.y - thickness * 0.5f;
    float laneHeight = thickness;
    float markerY = position.y - thickness * 1.4f;
    float markerHeight = thickness * 2.8f;
    if (side == CursorSegmentSide::Above) {
      laneHeight = thickness * 0.5f;
      laneY = position.y - laneHeight;
      markerHeight = thickness * 1.4f;
      markerY = position.y - markerHeight;
    } else if (side == CursorSegmentSide::Below) {
      laneHeight = thickness * 0.5f;
      laneY = position.y;
      markerHeight = thickness * 1.4f;
      markerY = position.y;
    }
    drawRectClip(laneX, laneY, laneAreaWidth, laneHeight, color.toABGR());
    drawRectClip(position.x + gutterWidth - 6.0f, markerY, 6.0f,
                 markerHeight, color.toABGR());
  }

  void selectAtUiPoint(float uiX, float uiY) {
    if (chart == nullptr) {
      return;
    }
    if (layoutDirty) {
      rebuildLayout();
    }

    const float contentX = uiToContentX(uiX);
    const float contentY = uiToContentY(uiY);
    for (const auto &layout : measureLayouts) {
      const float laneLeft = layout.x;
      const float laneRight = layout.x + gutterWidth + laneAreaWidth;
      if (contentX < laneLeft || contentX > laneRight ||
          contentY < layout.y || contentY > layout.y + layout.height ||
          layout.scale <= 0.0) {
        continue;
      }

      const double local =
          std::clamp(static_cast<double>(layout.y + layout.height - contentY) /
                         static_cast<double>(layout.height),
                     0.0, 1.0);
      selectBeatPosition(layout.beatStart + layout.scale * local);
      return;
    }

    (void)selectAtColumnEndpoint(contentX, contentY);
  }

  void selectBeatPosition(double beatPosition) {
    const double selectedBeatPosition =
        std::clamp(beatPosition, 0.0, std::max(0.0, totalBeatLength));
    const long long timeMicros = beatToTimeMicros(selectedBeatPosition);
    const long long chartEnd = chart == nullptr
                                   ? std::max(0LL, timeMicros)
                                   : chart_playback_duration::
                                         ChartTimelineEndMicros(*chart);
    practiceRange.placeActiveMarker(timeMicros, chartEnd);
    practiceRangeSet = true;
    playbackActive = false;
    if (selectionListener != nullptr) {
      selectionListener(getSelectedTimeMicros());
    }
    if (practiceRangeListener != nullptr) {
      practiceRangeListener(practiceRange);
    }
  }

  bool selectAtColumnEndpoint(float contentX, float contentY) {
    const float horizontalSlop =
        kCursorEndpointHorizontalSlopUi / std::max(0.001f, screenZoom);
    for (size_t columnIndex = 0; columnIndex < columnLayouts.size();
         ++columnIndex) {
      const auto &column = columnLayouts[columnIndex];
      const float laneLeft = column.x - horizontalSlop;
      const float laneRight =
          column.x + gutterWidth + laneAreaWidth + horizontalSlop;
      if (contentX < laneLeft || contentX > laneRight) {
        continue;
      }

      const MeasureLayout *bottomMeasure = nullptr;
      const MeasureLayout *topMeasure = nullptr;
      for (const auto &layout : measureLayouts) {
        if (layout.column != static_cast<int>(columnIndex) ||
            layout.scale <= 0.0) {
          continue;
        }
        if (bottomMeasure == nullptr ||
            layout.y + layout.height > bottomMeasure->y + bottomMeasure->height) {
          bottomMeasure = &layout;
        }
        if (topMeasure == nullptr || layout.y < topMeasure->y) {
          topMeasure = &layout;
        }
      }

      const float bottomEndpointY =
          bottomMeasure != nullptr ? bottomMeasure->y + bottomMeasure->height
                                   : column.yBottom;
      const float topEndpointY =
          topMeasure != nullptr ? topMeasure->y : column.yTop;
      const bool inBottomMargin =
          bottomMeasure != nullptr && contentY >= bottomEndpointY;
      const bool inTopMargin =
          topMeasure != nullptr && contentY <= topEndpointY;

      if (inBottomMargin && inTopMargin) {
        if (std::abs(contentY - bottomEndpointY) <=
            std::abs(contentY - topEndpointY)) {
          selectBeatPosition(bottomMeasure->beatStart);
        } else {
          selectBeatPosition(topMeasure->beatStart + topMeasure->scale);
        }
        return true;
      }
      if (inBottomMargin) {
        selectBeatPosition(bottomMeasure->beatStart);
        return true;
      }
      if (inTopMargin) {
        selectBeatPosition(topMeasure->beatStart + topMeasure->scale);
        return true;
      }
      return false;
    }
    return false;
  }

  bool timeToCursorPosition(long long timeMicros,
                            CursorDrawPosition &position) const {
    if (measureLayouts.empty()) {
      return false;
    }
    return beatToCursorPosition(timeToBeatPosition(timeMicros), position);
  }

  std::vector<CursorDrawPosition>
  timeToCursorPositions(long long timeMicros) const {
    if (measureLayouts.empty()) {
      return {};
    }
    return beatToCursorPositions(timeToBeatPosition(timeMicros));
  }

  bool beatToCursorPosition(double beatPosition,
                            CursorDrawPosition &position,
                            BoundaryPreference boundaryPreference =
                                BoundaryPreference::PreferEnd) const {
    if (measureLayouts.empty()) {
      return false;
    }

    constexpr double epsilon = 0.000001;
    if (boundaryPreference == BoundaryPreference::PreferStart) {
      for (const auto &layout : measureLayouts) {
        if (layout.scale <= 0.0 ||
            std::abs(beatPosition - layout.beatStart) > epsilon) {
          continue;
        }
        position = cursorPositionForLayout(layout, beatPosition);
        return true;
      }
    }

    for (const auto &layout : measureLayouts) {
      if (layout.scale <= 0.0) {
        continue;
      }
      const double start = layout.beatStart;
      const double end = layout.beatStart + layout.scale;
      if (beatPosition + epsilon < start || beatPosition - epsilon > end) {
        continue;
      }
      position = cursorPositionForLayout(layout, beatPosition);
      return true;
    }

    const auto &fallback =
        beatPosition < measureLayouts.front().beatStart ? measureLayouts.front()
                                                        : measureLayouts.back();
    position = cursorPositionForLayout(
        fallback, beatPosition < fallback.beatStart ? fallback.beatStart
                                                    : fallback.beatStart +
                                                          fallback.scale);
    return true;
  }

  std::vector<CursorDrawPosition> beatToCursorPositions(
      double beatPosition) const {
    std::vector<CursorDrawPosition> positions;
    if (measureLayouts.empty()) {
      return positions;
    }

    constexpr double epsilon = 0.000001;
    for (const auto &layout : measureLayouts) {
      if (layout.scale <= 0.0) {
        continue;
      }
      const double start = layout.beatStart;
      const double end = layout.beatStart + layout.scale;
      if (beatPosition + epsilon < start || beatPosition - epsilon > end) {
        continue;
      }
      CursorDrawPosition position = cursorPositionForLayout(layout, beatPosition);
      const auto duplicate = std::find_if(
          positions.begin(), positions.end(), [&](const auto &existing) {
            return existing.column == position.column &&
                   std::abs(existing.x - position.x) <= 0.5f &&
                   std::abs(existing.y - position.y) <= 0.5f;
          });
      if (duplicate == positions.end()) {
        positions.push_back(position);
      }
    }

    if (!positions.empty()) {
      return positions;
    }

    CursorDrawPosition fallbackPosition;
    if (beatToCursorPosition(beatPosition, fallbackPosition)) {
      positions.push_back(fallbackPosition);
    }
    return positions;
  }

  CursorDrawPosition cursorPositionForLayout(const MeasureLayout &layout,
                                             double beatPosition) const {
    const double local =
        layout.scale <= 0.0
            ? 0.0
            : std::clamp((beatPosition - layout.beatStart) / layout.scale, 0.0,
                         1.0);
    CursorDrawPosition position;
    position.column = layout.column;
    position.x = layout.x;
    position.y = layout.y + layout.height -
                 static_cast<float>(local) * layout.height;
    return position;
  }

  double timeToBeatPosition(long long timeMicros) const {
    if (orderedTimelines.empty()) {
      return 0.0;
    }

    const long long clampedTime = std::max(0LL, timeMicros);
    const auto *first = orderedTimelines.front();
    if (clampedTime <= first->Timing) {
      if (first->Timing <= 0 || first->BeatPosition <= 0.0) {
        return first->BeatPosition;
      }
      return first->BeatPosition *
             std::clamp(static_cast<double>(clampedTime) /
                            static_cast<double>(first->Timing),
                        0.0, 1.0);
    }

    for (size_t i = 1; i < orderedTimelines.size(); ++i) {
      const auto *prev = orderedTimelines[i - 1];
      const auto *current = orderedTimelines[i];
      const long long stopEnd =
          prev->Timing +
          std::max(0LL, static_cast<long long>(prev->GetStopDuration()));
      if (clampedTime <= stopEnd) {
        return prev->BeatPosition;
      }
      if (clampedTime <= current->Timing) {
        const long long duration = current->Timing - stopEnd;
        if (duration <= 0) {
          return current->BeatPosition;
        }
        const double progress =
            std::clamp(static_cast<double>(clampedTime - stopEnd) /
                           static_cast<double>(duration),
                       0.0, 1.0);
        return prev->BeatPosition +
               (current->BeatPosition - prev->BeatPosition) * progress;
      }
    }

    const auto *last = orderedTimelines.back();
    const long long stopEnd =
        last->Timing +
        std::max(0LL, static_cast<long long>(last->GetStopDuration()));
    if (clampedTime <= stopEnd) {
      return last->BeatPosition;
    }

    const long long totalLength =
        chart != nullptr ? std::max(chart->Meta.TotalLength, last->Timing)
                         : last->Timing;
    if (totalLength <= stopEnd || totalBeatLength <= last->BeatPosition) {
      return last->BeatPosition;
    }
    const double progress =
        std::clamp(static_cast<double>(clampedTime - stopEnd) /
                       static_cast<double>(totalLength - stopEnd),
                   0.0, 1.0);
    return last->BeatPosition +
           (totalBeatLength - last->BeatPosition) * progress;
  }

  long long beatToTimeMicros(double beatPosition) const {
    if (orderedTimelines.empty()) {
      return 0LL;
    }

    constexpr double epsilon = 0.000001;
    const double clampedBeat =
        std::clamp(beatPosition, 0.0, std::max(0.0, totalBeatLength));
    const auto *first = orderedTimelines.front();
    if (clampedBeat <= first->BeatPosition + epsilon) {
      if (first->BeatPosition <= epsilon || first->Timing <= 0) {
        return first->Timing;
      }
      const double progress =
          std::clamp(clampedBeat / first->BeatPosition, 0.0, 1.0);
      return static_cast<long long>(
          std::llround(static_cast<double>(first->Timing) * progress));
    }

    for (size_t i = 1; i < orderedTimelines.size(); ++i) {
      const auto *prev = orderedTimelines[i - 1];
      const auto *current = orderedTimelines[i];
      if (clampedBeat <= current->BeatPosition + epsilon) {
        if (clampedBeat <= prev->BeatPosition + epsilon) {
          return prev->Timing;
        }
        const long long stopEnd =
            prev->Timing +
            std::max(0LL, static_cast<long long>(prev->GetStopDuration()));
        const double beatDistance = current->BeatPosition - prev->BeatPosition;
        if (beatDistance <= epsilon) {
          return current->Timing;
        }
        const double progress =
            std::clamp((clampedBeat - prev->BeatPosition) / beatDistance, 0.0,
                       1.0);
        return static_cast<long long>(std::llround(
            static_cast<double>(stopEnd) +
            static_cast<double>(current->Timing - stopEnd) * progress));
      }
    }

    const auto *last = orderedTimelines.back();
    if (clampedBeat <= last->BeatPosition + epsilon) {
      return last->Timing;
    }
    const long long stopEnd =
        last->Timing +
        std::max(0LL, static_cast<long long>(last->GetStopDuration()));
    const long long totalLength =
        chart != nullptr ? std::max(chart->Meta.TotalLength, last->Timing)
                         : last->Timing;
    const double beatDistance = totalBeatLength - last->BeatPosition;
    if (beatDistance <= epsilon || totalLength <= stopEnd) {
      return last->Timing;
    }
    const double progress =
        std::clamp((clampedBeat - last->BeatPosition) / beatDistance, 0.0,
                   1.0);
    return static_cast<long long>(std::llround(
        static_cast<double>(stopEnd) +
        static_cast<double>(totalLength - stopEnd) * progress));
  }

  void renderLabels(RenderContext &context) {
    for (size_t i = 0; i < measureLayouts.size() && i < measureLabels.size();
         ++i) {
      const auto &layout = measureLayouts[i];
      if (!contentRectIntersects(layout.x, layout.y, gutterWidth,
                                 layout.height)) {
        continue;
      }
      auto *label = measureLabels[i].get();
      label->setSize(std::max(28, static_cast<int>(std::lround(gutterWidth *
                                                               screenZoom))),
                     std::max(16, static_cast<int>(std::lround(22.0f *
                                                               screenZoom))));
      label->setPositionNoLayout(
          static_cast<int>(std::round(contentToScreenX(layout.x))),
          static_cast<int>(
              std::round(contentToScreenY(layout.y + layout.height - 24.0f))),
          YGPositionTypeAbsolute);
      label->render(context);
    }

    visibleMarkerLabelDraws.clear();
    for (auto &marker : markerLabels) {
      auto yIt = timelineY.find(marker.timeline);
      auto layoutIt = timelineMeasure.find(marker.timeline);
      if (yIt == timelineY.end() || layoutIt == timelineMeasure.end()) {
        continue;
      }
      const auto &layout = measureLayouts[layoutIt->second];
      const float x = layout.x + gutterWidth + laneAreaWidth + 4.0f;
      float y = yIt->second - 10.0f;
      if (marker.type == MarkerType::Stop) {
        y += 10.0f;
      } else if (marker.type == MarkerType::Scroll) {
        y += 20.0f;
      }
      if (!contentRectIntersects(x, y, kMarkerLabelWidth,
                                 kMarkerLabelHeight)) {
        continue;
      }
      const float labelBoxWidth =
          std::max(60.0f, kMarkerLabelWidth * screenZoom);
      const float labelBoxHeight =
          std::max(16.0f, kMarkerLabelHeight * screenZoom);
      const float labelTextScale = std::max(1.0f, screenZoom);
      const MarkerTextRasterConfig raster =
          markerTextRasterConfig(labelTextScale);
      const float labelScreenX = contentToScreenX(x);
      const float labelScreenY = contentToScreenY(y);
      const float labelRight = labelScreenX + labelBoxWidth;
      float cursorX = labelScreenX;
      for (char glyphChar : marker.text) {
        const auto *glyph =
            cachedMarkerGlyph(glyphChar, marker.color, raster.fontSize);
        if (glyph == nullptr) {
          continue;
        }

        const float glyphWidth =
            std::max(1.0f, glyph->width) / raster.textureToUiScale;
        const float glyphHeight = glyph->height / raster.textureToUiScale;
        if (cursorX >= labelRight) {
          break;
        }
        const float clippedWidth = std::min(glyphWidth, labelRight - cursorX);
        if (glyphChar != ' ' && bgfx::isValid(glyph->texture) &&
            glyphHeight > 0.0f && clippedWidth > 0.0f) {
          visibleMarkerLabelDraws.push_back(
              {glyph->texture, cursorX,
               labelScreenY + (labelBoxHeight - glyphHeight) * 0.5f,
               clippedWidth, glyphHeight,
               std::clamp(clippedWidth / glyphWidth, 0.0f, 1.0f)});
        }
        cursorX += glyphWidth;
      }
    }

    if (visibleMarkerLabelDraws.empty()) {
      return;
    }

    std::sort(visibleMarkerLabelDraws.begin(), visibleMarkerLabelDraws.end(),
              [](const MarkerLabelDraw &lhs, const MarkerLabelDraw &rhs) {
                return lhs.texture.idx < rhs.texture.idx;
              });
    markerTextBatch.setScissor(context.scissor.x, context.scissor.y,
                               context.scissor.width, context.scissor.height);
    markerTextBatch.begin(context.getTransformMatrix());
    for (const auto &draw : visibleMarkerLabelDraws) {
      markerTextBatch.addRectUV(draw.x, draw.y, draw.width, draw.height, 0.0f,
                                1.0f, draw.u1, 0.0f, draw.texture);
    }
    markerTextBatch.end();
    markerTextBatch.clearScissor();
  }

  const CachedMarkerGlyph *cachedMarkerGlyph(char glyph, const SDL_Color &color,
                                             int fontSize) {
    const std::string key = markerGlyphCacheKey(glyph, color, fontSize);
    if (const auto it = markerGlyphTextures.find(key);
        it != markerGlyphTextures.end()) {
      return &it->second;
    }

    CachedMarkerGlyph label;
    label.text =
        std::make_unique<TextView>("assets/fonts/notosanscjkjp.ttf", fontSize);
    label.text->setColor(color);
    label.text->setText(std::string(1, glyph));
    label.texture = label.text->textureHandle();
    label.width = glyph == ' ' ? static_cast<float>(fontSize) * 0.34f
                               : static_cast<float>(label.text->textureWidth());
    label.height = static_cast<float>(label.text->textureHeight());
    auto [it, inserted] =
        markerGlyphTextures.emplace(key, std::move(label));
    (void)inserted;
    return &it->second;
  }

  template <typename Fn> void forEachNote(Fn &&fn) {
    if (chart == nullptr) {
      return;
    }
    for (const auto *measure : chart->Measures) {
      if (measure == nullptr) {
        continue;
      }
      for (const auto *timeline : measure->TimeLines) {
        if (timeline == nullptr) {
          continue;
        }
        for (int lane : laneOrder) {
          if (lane < 0 ||
              lane >= static_cast<int>(timeline->Notes.size())) {
            continue;
          }
          const auto *note = timeline->Notes[static_cast<size_t>(lane)];
          if (note != nullptr) {
            fn(lane, note, timeline);
          }
        }
      }
    }
  }

  template <typename Fn> void forEachInvisibleNote(Fn &&fn) {
    if (chart == nullptr) {
      return;
    }
    for (const auto *measure : chart->Measures) {
      if (measure == nullptr) {
        continue;
      }
      for (const auto *timeline : measure->TimeLines) {
        if (timeline == nullptr) {
          continue;
        }
        for (int lane : laneOrder) {
          if (lane < 0 ||
              lane >= static_cast<int>(timeline->InvisibleNotes.size())) {
            continue;
          }
          const auto *note =
              timeline->InvisibleNotes[static_cast<size_t>(lane)];
          if (note != nullptr) {
            fn(lane, note, timeline);
          }
        }
      }
    }
  }

  void drawLongNoteBody(int lane, const bms_parser::TimeLine *headTimeline,
                        const bms_parser::TimeLine *tailTimeline) {
    auto headYIt = timelineY.find(headTimeline);
    auto tailYIt = timelineY.find(tailTimeline);
    auto headLayoutIt = timelineMeasure.find(headTimeline);
    auto tailLayoutIt = timelineMeasure.find(tailTimeline);
    if (headYIt == timelineY.end() || tailYIt == timelineY.end() ||
        headLayoutIt == timelineMeasure.end() ||
        tailLayoutIt == timelineMeasure.end()) {
      return;
    }

    const auto &headLayout = measureLayouts[headLayoutIt->second];
    const auto &tailLayout = measureLayouts[tailLayoutIt->second];
    int firstColumn = headLayout.column;
    int lastColumn = tailLayout.column;
    if (firstColumn > lastColumn) {
      std::swap(firstColumn, lastColumn);
    }

    const float bodyWidth = std::max(6.0f, laneWidth * 0.62f);
    const uint32_t color = longNoteColor(lane);
    for (int column = firstColumn; column <= lastColumn; ++column) {
      if (column < 0 || column >= static_cast<int>(columnLayouts.size())) {
        continue;
      }
      const auto &columnLayout = columnLayouts[static_cast<size_t>(column)];
      float y0 = columnLayout.yTop;
      float y1 = columnLayout.yBottom;
      if (column == headLayout.column && column == tailLayout.column) {
        y0 = std::min(headYIt->second, tailYIt->second);
        y1 = std::max(headYIt->second, tailYIt->second);
      } else if (column == headLayout.column) {
        y0 = columnLayout.yTop;
        y1 = headYIt->second;
      } else if (column == tailLayout.column) {
        y0 = tailYIt->second;
        y1 = columnLayout.yBottom;
      }
      const float x = laneContentX(column, lane) + laneWidth * 0.5f -
                      bodyWidth * 0.5f;
      drawRectClip(x, y0, bodyWidth, std::max(2.0f, y1 - y0), color);
    }
  }

  float laneContentX(int column, int lane) const {
    size_t orderIndex = 0;
    if (const auto it = laneToOrderIndex.find(lane);
        it != laneToOrderIndex.end()) {
      orderIndex = it->second;
    }
    const float columnX =
        column >= 0 && column < static_cast<int>(columnLayouts.size())
            ? columnLayouts[static_cast<size_t>(column)].x
            : contentLeftMargin + static_cast<float>(column) * columnWidth;
    return columnX + gutterWidth + static_cast<float>(orderIndex) * laneWidth;
  }

  uint32_t noteColor(int lane, const bms_parser::Note *note) const {
    if (dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
      return Color(217, 69, 58, 246).toABGR();
    }
    if (isScratchLane(lane)) {
      return Color(231, 94, 58, 246).toABGR();
    }
    const bool useBlue = usesBlueNoteColor(lane);
    if (dynamic_cast<const bms_parser::LongNote *>(note) != nullptr) {
      return useBlue ? Color(84, 151, 224, 245).toABGR()
                     : Color(225, 232, 230, 245).toABGR();
    }
    return useBlue ? Color(82, 154, 226, 248).toABGR()
                   : Color(236, 240, 238, 248).toABGR();
  }

  uint32_t longNoteColor(int lane) const {
    if (isScratchLane(lane)) {
      return Color(231, 94, 58, 164).toABGR();
    }
    return usesBlueNoteColor(lane) ? Color(82, 154, 226, 164).toABGR()
                                   : Color(226, 232, 230, 154).toABGR();
  }

  uint32_t invisibleNoteColor() const {
    return Color(255, 149, 36, 224).toABGR();
  }

  bool usesBlueNoteColor(int lane) const {
    const auto it = laneUsesBlueNoteColor.find(lane);
    return it != laneUsesBlueNoteColor.end() && it->second;
  }

  Color ghostColor(const ReplayGhostEvent &event) const {
    if (event.judgement == PGreat) {
      return Color(255, 255, 255, 222);
    }
    return event.judgeTimeMicros < event.noteTimeMicros
               ? Color(53, 134, 255, 222)
               : Color(255, 65, 72, 222);
  }

  void drawMissMarkerX(float x, float y, float width, float height) {
    if (!contentRectIntersects(x, y, width, height)) {
      return;
    }

    constexpr int kSteps = 7;
    const uint32_t color = Color(255, 48, 56, 236).toABGR();
    const float block = std::max(1.5f, std::min(width, height) * 0.24f);
    const float maxX = std::max(0.0f, width - block);
    const float maxY = std::max(0.0f, height - block);
    for (int i = 0; i < kSteps; ++i) {
      const float t = kSteps == 1 ? 0.0f
                                  : static_cast<float>(i) /
                                        static_cast<float>(kSteps - 1);
      const float yOffset = maxY * t;
      drawRectClip(x + maxX * t, y + yOffset, block, block, color);
      drawRectClip(x + maxX * (1.0f - t), y + yOffset, block, block, color);
    }
  }

  bool isScratchLane(int lane) const {
    const auto scratchLanes = chart->Meta.GetScratchLaneIndices();
    return std::find(scratchLanes.begin(), scratchLanes.end(), lane) !=
           scratchLanes.end();
  }

  bool contentRectIntersects(float x, float y, float width, float height) const {
    const float viewportWidth = static_cast<float>(getWidth()) / screenZoom;
    const float viewportHeight = static_cast<float>(getHeight()) / screenZoom;
    return x + width >= scrollX && x <= scrollX + viewportWidth &&
           y + height >= scrollY && y <= scrollY + viewportHeight;
  }

  float contentToScreenX(float contentX) const {
    return static_cast<float>(getX()) + (contentX - scrollX) * screenZoom;
  }

  float contentToScreenY(float contentY) const {
    return static_cast<float>(getY()) + (contentY - scrollY) * screenZoom;
  }

  float uiToContentX(float uiX) const {
    return scrollX + (uiX - static_cast<float>(getX())) / screenZoom;
  }

  float uiToContentY(float uiY) const {
    return scrollY + (uiY - static_cast<float>(getY())) / screenZoom;
  }

  void drawRectClip(float x, float y, float width, float height,
                    uint32_t color) {
    if (width <= 0.0f || height <= 0.0f) {
      return;
    }
    const float screenX = contentToScreenX(x);
    const float screenY = contentToScreenY(y);
    const float screenWidth = width * screenZoom;
    const float screenHeight = height * screenZoom;
    const float left = std::max(screenX, static_cast<float>(getX()));
    const float top = std::max(screenY, static_cast<float>(getY()));
    const float right =
        std::min(screenX + screenWidth,
                 static_cast<float>(getX() + getWidth()));
    const float bottom =
        std::min(screenY + screenHeight,
                 static_cast<float>(getY() + getHeight()));
    if (right <= left || bottom <= top) {
      return;
    }
    batch.addRect(left, top, right - left, bottom - top, color);
  }

  bool containsPoint(float uiX, float uiY) const {
    return uiX >= getX() && uiX <= getX() + getWidth() && uiY >= getY() &&
           uiY <= getY() + getHeight();
  }

  float columnVisualWidth() const {
    return gutterWidth + laneAreaWidth + 78.0f;
  }

  std::optional<int> columnAtViewportCenter() const {
    if (columnLayouts.empty() || getWidth() <= 0 || screenZoom <= 0.0f) {
      return std::nullopt;
    }

    const float viewportWidth = static_cast<float>(getWidth()) / screenZoom;
    const float centerX = scrollX + viewportWidth * 0.5f;
    const float visualWidth = columnVisualWidth();
    int nearestColumn = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < columnLayouts.size(); ++i) {
      const float left = columnLayouts[i].x;
      const float right = left + visualWidth;
      if (centerX >= left && centerX <= right) {
        return static_cast<int>(i);
      }

      const float columnCenter = left + visualWidth * 0.5f;
      const float distance = std::abs(centerX - columnCenter);
      if (distance < nearestDistance) {
        nearestDistance = distance;
        nearestColumn = static_cast<int>(i);
      }
    }
    return nearestColumn;
  }

  bool zoomAnchorContainsMeasure(const ZoomFocusAnchor &anchor,
                                 int measureIndex) const {
    return std::find(anchor.measureIndices.begin(), anchor.measureIndices.end(),
                     measureIndex) != anchor.measureIndices.end();
  }

  std::optional<ZoomFocusAnchor> zoomFocusAnchorAtViewportCenter() const {
    const std::optional<int> centerColumn = columnAtViewportCenter();
    if (!centerColumn.has_value() || getHeight() <= 0 || screenZoom <= 0.0f) {
      return std::nullopt;
    }

    const float viewportHeight = static_cast<float>(getHeight()) / screenZoom;
    const float centerY = scrollY + viewportHeight * 0.5f;
    ZoomFocusAnchor anchor;
    float nearestMeasureDistance = std::numeric_limits<float>::max();
    for (const auto &layout : measureLayouts) {
      if (layout.column != *centerColumn) {
        continue;
      }

      anchor.measureIndices.push_back(layout.measureIndex);
      const float measureTop = layout.y;
      const float measureBottom = layout.y + layout.height;
      const float measureCenter = (measureTop + measureBottom) * 0.5f;
      const float distance =
          centerY >= measureTop && centerY <= measureBottom
              ? 0.0f
              : std::abs(centerY - measureCenter);
      if (distance < nearestMeasureDistance && layout.height > 0.0f) {
        nearestMeasureDistance = distance;
        anchor.centerMeasureIndex = layout.measureIndex;
        anchor.centerMeasureLocalY =
            std::clamp((centerY - layout.y) / layout.height, 0.0f, 1.0f);
      }
    }

    if (anchor.measureIndices.empty()) {
      return std::nullopt;
    }
    return anchor;
  }

  std::optional<int> columnForZoomAnchor(const ZoomFocusAnchor &anchor) const {
    if (anchor.measureIndices.empty() || columnLayouts.empty()) {
      return std::nullopt;
    }

    int bestColumn = -1;
    int bestOverlap = 0;
    bool bestHasCenterMeasure = false;
    for (size_t columnIndex = 0; columnIndex < columnLayouts.size();
         ++columnIndex) {
      int overlap = 0;
      bool hasCenterMeasure = false;
      for (const auto &layout : measureLayouts) {
        if (layout.column != static_cast<int>(columnIndex)) {
          continue;
        }
        if (!zoomAnchorContainsMeasure(anchor, layout.measureIndex)) {
          continue;
        }

        ++overlap;
        if (layout.measureIndex == anchor.centerMeasureIndex) {
          hasCenterMeasure = true;
        }
      }

      if (overlap <= 0) {
        continue;
      }
      if (overlap > bestOverlap ||
          (overlap == bestOverlap && hasCenterMeasure &&
           !bestHasCenterMeasure)) {
        bestColumn = static_cast<int>(columnIndex);
        bestOverlap = overlap;
        bestHasCenterMeasure = hasCenterMeasure;
      }
    }

    if (bestColumn < 0) {
      return std::nullopt;
    }
    return bestColumn;
  }

  const MeasureLayout *
  measureForZoomAnchorInColumn(const ZoomFocusAnchor &anchor,
                               int column) const {
    const MeasureLayout *fallback = nullptr;
    int fallbackDistance = std::numeric_limits<int>::max();
    for (const auto &layout : measureLayouts) {
      if (layout.column != column ||
          !zoomAnchorContainsMeasure(anchor, layout.measureIndex)) {
        continue;
      }

      if (layout.measureIndex == anchor.centerMeasureIndex) {
        return &layout;
      }

      const int distance =
          std::abs(layout.measureIndex - anchor.centerMeasureIndex);
      if (fallback == nullptr || distance < fallbackDistance) {
        fallback = &layout;
        fallbackDistance = distance;
      }
    }
    return fallback;
  }

  void focusZoomAnchor(const ZoomFocusAnchor &anchor) {
    const std::optional<int> targetColumn = columnForZoomAnchor(anchor);
    if (!targetColumn.has_value()) {
      return;
    }

    focusColumn(*targetColumn);
    const auto *targetMeasure =
        measureForZoomAnchorInColumn(anchor, *targetColumn);
    if (targetMeasure == nullptr || getHeight() <= 0 || screenZoom <= 0.0f) {
      return;
    }

    const float viewportHeight = static_cast<float>(getHeight()) / screenZoom;
    const float localY = targetMeasure->measureIndex == anchor.centerMeasureIndex
                             ? anchor.centerMeasureLocalY
                             : 0.5f;
    scrollY = targetMeasure->y + targetMeasure->height * localY -
              viewportHeight * 0.5f;
    clampScroll();
  }

  void focusColumn(int column) {
    if (columnLayouts.empty() || getWidth() <= 0 || screenZoom <= 0.0f) {
      return;
    }

    const int clampedColumn =
        std::clamp(column, 0, static_cast<int>(columnLayouts.size()) - 1);
    const float viewportWidth = static_cast<float>(getWidth()) / screenZoom;
    const float columnCenter =
        columnLayouts[clampedColumn].x + columnVisualWidth() * 0.5f;
    scrollX = columnCenter - viewportWidth * 0.5f;
    clampScroll();
  }

  bool isUserInteracting() const {
    return mouseDragging || pinchActive || !activeTouches.empty();
  }

  void ensureContentRectVisible(float x, float y, float width, float height,
                                float paddingUiX, float paddingUiY) {
    if (getWidth() <= 0 || getHeight() <= 0 || screenZoom <= 0.0f) {
      return;
    }

    const float viewportWidth = static_cast<float>(getWidth()) / screenZoom;
    const float viewportHeight = static_cast<float>(getHeight()) / screenZoom;
    const float paddingX =
        std::min(viewportWidth * 0.25f, paddingUiX / screenZoom);
    const float paddingY =
        std::min(viewportHeight * 0.25f, paddingUiY / screenZoom);

    bool changed = false;
    if (width + paddingX * 2.0f >= viewportWidth) {
      const float nextScrollX = x + width * 0.5f - viewportWidth * 0.5f;
      changed = changed || std::abs(scrollX - nextScrollX) > 0.001f;
      scrollX = nextScrollX;
    } else if (x < scrollX + paddingX) {
      scrollX = x - paddingX;
      changed = true;
    } else if (x + width > scrollX + viewportWidth - paddingX) {
      scrollX = x + width - viewportWidth + paddingX;
      changed = true;
    }

    if (height + paddingY * 2.0f >= viewportHeight) {
      const float nextScrollY = y + height * 0.5f - viewportHeight * 0.5f;
      changed = changed || std::abs(scrollY - nextScrollY) > 0.001f;
      scrollY = nextScrollY;
    } else if (y < scrollY + paddingY) {
      scrollY = y - paddingY;
      changed = true;
    } else if (y + height > scrollY + viewportHeight - paddingY) {
      scrollY = y + height - viewportHeight + paddingY;
      changed = true;
    }

    if (changed) {
      clampScroll();
    }
  }

  void autofocusPlaybackCursor() {
    if (chart == nullptr || isUserInteracting()) {
      return;
    }
    if (layoutDirty) {
      rebuildLayout();
    }

    CursorDrawPosition position;
    if (!timeToCursorPosition(playbackTimeMicros, position)) {
      return;
    }

    constexpr float kCursorFocusHeight = 24.0f;
    ensureContentRectVisible(
        position.x, position.y - kCursorFocusHeight * 0.5f,
        gutterWidth + laneAreaWidth, kCursorFocusHeight,
        kPlaybackAutofocusPaddingX, kPlaybackAutofocusPaddingY);
  }

  void clampScroll() {
    screenZoom = std::clamp(screenZoom, kMinScreenZoom, kMaxScreenZoom);
    const float viewportWidth = static_cast<float>(getWidth()) / screenZoom;
    const float viewportHeight = static_cast<float>(getHeight()) / screenZoom;
    const float maxX =
        std::max(0.0f, contentWidth - viewportWidth);
    const float maxY =
        std::max(0.0f, contentHeight - viewportHeight);
    scrollX = std::clamp(scrollX, 0.0f, maxX);
    scrollY = std::clamp(scrollY, 0.0f, maxY);
  }

  bool twoTouchGeometry(float &distance, float &centerX, float &centerY) const {
    if (activeTouches.size() < 2) {
      return false;
    }
    auto touchIt = activeTouches.begin();
    const TouchPoint first = touchIt->second;
    ++touchIt;
    const TouchPoint second = touchIt->second;
    const float dx = second.x - first.x;
    const float dy = second.y - first.y;
    distance = std::max(1.0f, std::hypot(dx, dy));
    centerX = (first.x + second.x) * 0.5f;
    centerY = (first.y + second.y) * 0.5f;
    return true;
  }

  void beginPinch() {
    float distance = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    if (!twoTouchGeometry(distance, centerX, centerY)) {
      pinchActive = false;
      return;
    }
    pinchActive = true;
    mouseDragging = false;
    dragTouchId = -1;
    pinchStartDistance = distance;
    pinchStartScreenZoom = screenZoom;
    pinchAnchorContentX = uiToContentX(centerX);
    pinchAnchorContentY = uiToContentY(centerY);
  }

  void applyPinch() {
    if (!pinchActive) {
      beginPinch();
      return;
    }
    float distance = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    if (!twoTouchGeometry(distance, centerX, centerY)) {
      pinchActive = false;
      return;
    }
    screenZoom = std::clamp(pinchStartScreenZoom * distance /
                                std::max(1.0f, pinchStartDistance),
                            kMinScreenZoom, kMaxScreenZoom);
    scrollX = pinchAnchorContentX -
              (centerX - static_cast<float>(getX())) / screenZoom;
    scrollY = pinchAnchorContentY -
              (centerY - static_cast<float>(getY())) / screenZoom;
    clampScroll();
  }

  static void mouseEventToUi(const SDL_MouseButtonEvent &event, int &uiX,
                             int &uiY) {
    const int screenX = static_cast<int>(event.x * rendering::widthScale);
    const int screenY = static_cast<int>(event.y * rendering::heightScale);
    rendering::screenToUi(screenX, screenY, uiX, uiY);
  }

  static void mouseMotionToUi(const SDL_MouseMotionEvent &event, int &uiX,
                              int &uiY) {
    const int screenX = static_cast<int>(event.x * rendering::widthScale);
    const int screenY = static_cast<int>(event.y * rendering::heightScale);
    rendering::screenToUi(screenX, screenY, uiX, uiY);
  }
};

ChartViewerScene::ChartViewerScene(
    ApplicationContext &context, ChartMetaRecord record,
    std::optional<unsigned int> randomSeed,
    std::optional<std::string> randomPrng,
    std::optional<std::vector<int>> randomValues,
    std::optional<practice::LaunchRequest> launchRequest)
    : Scene(context), record(std::move(record)), randomSeed(randomSeed),
      randomPrng(std::move(randomPrng)),
      pendingPracticeLaunchRequest(std::move(launchRequest)) {
  if (randomValues.has_value()) {
    selectedRandomValues = *randomValues;
  }
  viewerAssistOption =
      assist_options::normalize(context.settings.selectedAssistOption);
  if (pendingPracticeLaunchRequest.has_value()) {
    practiceRuleset = pendingPracticeLaunchRequest->ruleset;
    practiceRequiredRulesetDescriptor =
        pendingPracticeLaunchRequest->requiredRulesetDescriptor;
    practiceReplayRulesetSnapshot =
        pendingPracticeLaunchRequest->replayRulesetSnapshot;
  } else {
    practiceRuleset = gameplayRulesetSelectionOrDefault(
        context.settings.selectedGameplayRuleset);
  }
  if (pendingPracticeLaunchRequest.has_value() &&
      pendingPracticeLaunchRequest->replayPlayOptions.has_value()) {
    const auto &replayOptions =
        *pendingPracticeLaunchRequest->replayPlayOptions;
    setViewerPlayOptions(
        replayOptions.playOption, replayOptions.playOptionSeed,
        replayOptions.playOption2, replayOptions.playOption2Seed);
  } else {
    const std::optional<std::string> selectedPlayOption =
        context.settings.selectedPlayOption;
    setViewerPlayOptions(selectedPlayOption, std::nullopt,
                         this->record.meta.IsDP ? selectedPlayOption
                                                : std::nullopt,
                         std::nullopt);
  }
}

void ChartViewerScene::init() {
  initView();
  parseAndRefresh(selectedRandomValues.empty()
                      ? std::nullopt
                      : std::optional<std::vector<int>>(selectedRandomValues));
}

void ChartViewerScene::onResume() { applyPendingPracticeLaunchRequest(); }

void ChartViewerScene::setPracticeLaunchRequest(
    practice::LaunchRequest request) {
  pendingPracticeLaunchRequest = std::move(request);
}

EventHandleResult ChartViewerScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    goBack();
    return {};
  }
  const bool editingPresetName =
      practicePanel != nullptr && practicePanel->isEditingPresetName();
  if (event.type == SDL_KEYDOWN && !editingPresetName) {
    switch (event.key.keysym.sym) {
    case SDLK_1:
    case SDLK_KP_1:
      selectActivePracticeMarker(practice::Marker::Start);
      return {};
    case SDLK_2:
    case SDLK_KP_2:
      selectActivePracticeMarker(practice::Marker::End);
      return {};
    case SDLK_LEFT:
      moveActivePracticeMarker(practice::TimelineDirection::Previous);
      return {};
    case SDLK_RIGHT:
      moveActivePracticeMarker(practice::TimelineDirection::Next);
      return {};
    default:
      break;
    }
  }
  if (event.type == SDL_CONTROLLERBUTTONDOWN) {
    switch (event.cbutton.button) {
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
      selectActivePracticeMarker(practice::Marker::Start);
      return {};
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
      selectActivePracticeMarker(practice::Marker::End);
      return {};
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
      moveActivePracticeMarker(practice::TimelineDirection::Previous);
      return {};
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
      moveActivePracticeMarker(practice::TimelineDirection::Next);
      return {};
    default:
      break;
    }
  }
  return Scene::handleEvents(event);
}

void ChartViewerScene::update(float dt) {
  (void)dt;
  if (listenActive && canvasView != nullptr) {
    const long long rawTime = context.jukebox.getTimeMicros();
    const long long displayTime =
        rawTime + static_cast<long long>(context.settings.audioOffsetMs) *
                      1000LL;
    canvasView->setPlaybackTime(displayTime, true);
    if (chart != nullptr && listenEndMicros > 0 && rawTime >= listenEndMicros) {
      stopListening();
    }
    updateListenControls();
  }

  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  if (rootLayout != nullptr &&
      (lastLayoutWidth != rendering::window_width ||
       lastLayoutHeight != rendering::window_height || lastSafeTop != safe.top ||
       lastSafeLeft != safe.left || lastSafeBottom != safe.bottom ||
       lastSafeRight != safe.right)) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    lastSafeTop = safe.top;
    lastSafeLeft = safe.left;
    lastSafeBottom = safe.bottom;
    lastSafeRight = safe.right;
    rootLayout->setSize(rendering::window_width, rendering::window_height);
    rootLayout->applyYogaLayout();
    if (randomDrawerRoot != nullptr) {
      randomDrawerRoot->setSize(rendering::window_width,
                                rendering::window_height);
      randomDrawerRoot->applyYogaLayout();
    }
    if (ghostModalRoot != nullptr) {
      ghostModalRoot->setSize(rendering::window_width,
                              rendering::window_height);
      ghostModalRoot->applyYogaLayout();
    }
    if (optionsDrawerRoot != nullptr) {
      optionsDrawerRoot->setSize(rendering::window_width,
                                 rendering::window_height);
      optionsDrawerRoot->applyYogaLayout();
    }
    if (overlayPortal != nullptr) {
      overlayPortal->setSize(rendering::window_width,
                             rendering::window_height);
      overlayPortal->applyYogaLayout();
    }
  }
}

void ChartViewerScene::renderScene() {}

void ChartViewerScene::cleanupScene() {
  if (listenActive || listenAudioLoaded || retainedListenResourcesForReload) {
    context.jukebox.stop();
    listenActive = false;
    listenAudioLoaded = false;
    retainedListenResourcesForReload = false;
    listenEndMicros = 0;
  }
  chart.reset();
  randomOptions.clear();
  practiceGhostReplay.reset();
  ghostReplaySummaries.clear();
  loadedGhostReplayId = kNoGhostReplayId;
  selectedGhostReplayIndex = -1;
  rootLayout = nullptr;
  canvasView = nullptr;
  titleText = nullptr;
  subtitleText = nullptr;
  statusText = nullptr;
  randomSummaryText = nullptr;
  zoomText = nullptr;
  selectionText = nullptr;
  listenPauseText = nullptr;
  listenPauseButton = nullptr;
  listenStopButton = nullptr;
  ghostLoadButton = nullptr;
  ghostLoadButtonText = nullptr;
  ghostClearButton = nullptr;
  ghostClearButtonText = nullptr;
  ghostModalRoot = nullptr;
  ghostModalEmptyText = nullptr;
  practiceGhostReplayButton = nullptr;
  practiceGhostReplayItem = nullptr;
  ghostReplayListView = nullptr;
  optionsDrawerRoot = nullptr;
  viewerOptionText = nullptr;
  viewerPlayOptionsPanel = nullptr;
  randomDrawerRoot = nullptr;
  randomDrawerScroll = nullptr;
  overlayPortal = nullptr;
  practicePanel = nullptr;
  practicePresetStore.reset();
  practiceNamedPresets.clear();
  selectedPracticePresetId.reset();
  practiceChartEndMicros = 0;
}

void ChartViewerScene::setPracticeGhostReplay(const ReplayData &replayData) {
  if (replayData.events.empty()) {
    practiceGhostReplay.reset();
    clearGhostReplay();
    return;
  }

  practiceGhostReplay = replayData;
  practiceGhostReplay->id = kPracticeGhostReplayId;
  practiceGhostReplay->createdAt = "Practice Ghost";
  loadedGhostReplayId = kPracticeGhostReplayId;
  selectedGhostReplayIndex = -1;

  if (canvasView != nullptr && chart != nullptr) {
    canvasView->setGhostReplay(*practiceGhostReplay);
  }
  if (statusText != nullptr) {
    statusText->setText("Practice ghost loaded");
  }
  updatePracticeGhostReplayButton();
  updateGhostControls();
}

void ChartViewerScene::initView() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;

  rootLayout = new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setBackgroundColor(ui_theme::backdrop());

  auto *header = new View();
  header->setHeight(safe.top + 98);
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setPadding(Edge::Top, safe.top + 10);
  header->setPadding(Edge::Left, safe.left + kHeaderPadding);
  header->setPadding(Edge::Right, safe.right + kHeaderPadding);
  header->setPadding(Edge::Bottom, 10);
  header->setGap(12);
  header->setBackgroundColor(ui_theme::panelStrong());
  header->setShadow(ui_theme::shadow(), ui_theme::kHeaderShadow);
  header->setBorderColor(ui_theme::hairline());
  header->setBorderWidth(1);

  auto *titleColumn = new View();
  titleColumn->setFlexDirection(FlexDirection::Column);
  titleColumn->setAlignItems(YGAlignStretch);
  titleColumn->setFlex(1);
  titleColumn->setMinWidth(0);
  titleColumn->setGap(2);

  titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 29);
  titleText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  titleText->setHeight(36);
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  subtitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  subtitleText->setColor(ui_theme::sdl(ui_theme::textMuted()));
  subtitleText->setHeight(23);
  subtitleText->setOverflow(TextView::TextOverflow::Hidden);
  randomSummaryText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  randomSummaryText->setColor(ui_theme::sdl(ui_theme::amber()));
  randomSummaryText->setHeight(21);
  randomSummaryText->setOverflow(TextView::TextOverflow::Hidden);
  titleColumn->addView(titleText);
  titleColumn->addView(subtitleText);
  titleColumn->addView(randomSummaryText);
  header->addView(titleColumn);

  statusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  statusText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  statusText->setAlign(TextView::RIGHT);
  statusText->setVAlign(TextView::MIDDLE);
  statusText->setWidth(250);
  statusText->setHeight(kHeaderButtonHeight);
  statusText->setOverflow(TextView::TextOverflow::Hidden);
  header->addView(statusText);

  TextView *zoomButtonText = nullptr;
  auto *zoomOutButton = makeButton("-", 52, 26);
  zoomText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  zoomText->setAlign(TextView::CENTER);
  zoomText->setVAlign(TextView::MIDDLE);
  zoomText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  zoomText->setWidth(70);
  zoomText->setHeight(kHeaderButtonHeight);
  auto *zoomInButton = makeButton("+", 52, 26, &zoomButtonText);
  (void)zoomButtonText;
  zoomOutButton->setOnClickListener([this]() {
    if (canvasView != nullptr) {
      canvasView->setZoom(canvasView->getZoom() - kZoomStep);
      updateZoomText();
    }
  });
  zoomInButton->setOnClickListener([this]() {
    if (canvasView != nullptr) {
      canvasView->setZoom(canvasView->getZoom() + kZoomStep);
      updateZoomText();
    }
  });
  header->addView(zoomOutButton);
  header->addView(zoomText);
  header->addView(zoomInButton);

  auto *randomButton = makeButton("Random", 118, 20);
  randomButton->setOnClickListener([this]() { showRandomDrawer(); });
  header->addView(randomButton);

  auto *optionButton = makeButton("Option", 106, 20);
  optionButton->setOnClickListener([this]() { showOptionsDrawer(); });
  header->addView(optionButton);

  auto *backButton = makeButton("Back", 92, 20);
  backButton->setOnClickListener([this]() { goBack(); });
  header->addView(backButton);

  auto *toolbar = new View();
  toolbar->setHeight(64);
  toolbar->setFlexDirection(FlexDirection::Row);
  toolbar->setAlignItems(YGAlignCenter);
  toolbar->setPadding(Edge::Left, safe.left + kHeaderPadding);
  toolbar->setPadding(Edge::Right, safe.right + kHeaderPadding);
  toolbar->setGap(12);
  toolbar->setBackgroundColor(ui_theme::panel());
  toolbar->setBorderColor(ui_theme::hairline());
  toolbar->setBorderWidth(1);

  selectionText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  selectionText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  selectionText->setVAlign(TextView::MIDDLE);
  selectionText->setOverflow(TextView::TextOverflow::Hidden);
  selectionText->setFlex(1);
  selectionText->setHeight(42);
  toolbar->addView(selectionText);

  auto *listenButton = makeButton("Listen", 104, 19);
  listenButton->setOnClickListener([this]() { startListeningFromSelection(); });
  toolbar->addView(listenButton);

  listenPauseButton = makeButton("Pause", 104, 18, &listenPauseText);
  listenPauseButton->setOnClickListener([this]() { toggleListenPause(); });
  listenPauseButton->setVisible(false);
  toolbar->addView(listenPauseButton);

  listenStopButton = makeButton("Stop", 92, 18);
  listenStopButton->setOnClickListener([this]() { stopListening(); });
  listenStopButton->setVisible(false);
  toolbar->addView(listenStopButton);

  auto *practiceButton = makeButton("Practice", 116, 18);
  practiceButton->setOnClickListener(
      [this]() { startPracticeFromSelection(false); });
  toolbar->addView(practiceButton);

  auto *autoPlayButton = makeButton("Auto Play", 126, 18);
  autoPlayButton->setOnClickListener(
      [this]() { startPracticeFromSelection(true); });
  toolbar->addView(autoPlayButton);

  ghostLoadButton = makeButton("Ghost", 90, 18, &ghostLoadButtonText);
  ghostLoadButton->setOnClickListener([this]() { showGhostModal(); });
  toolbar->addView(ghostLoadButton);

  ghostClearButton = makeButton("Clear", 92, 18, &ghostClearButtonText);
  ghostClearButton->setOnClickListener([this]() { clearGhostReplay(); });
  ghostClearButton->setVisible(false);
  toolbar->addView(ghostClearButton);

  canvasView = new ChartCanvasView();
  canvasView->setShowInvisibleNotes(context.settings.showInvisibleNotes);
  canvasView->setFlex(1);
  canvasView->setSelectionListener(
      [this](long long timeMicros) { onCanvasSelectionChanged(timeMicros); });
  canvasView->setPracticeRangeListener(
      [this](const practice::RangeSelection &range) {
        onPracticeRangeChanged(range);
      });

  overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                    rendering::window_height);
  overlayPortal->setPositionType(YGPositionTypeAbsolute);
  overlayPortal->setPosition(Edge::Left, 0);
  overlayPortal->setPosition(Edge::Top, 0);
  overlayPortal->setZIndex(1200);

  practicePanel = new PracticePanelView(
      0,
      {
          .onChanged = [this](const practice::Configuration &configuration) {
            onPracticeConfigurationChanged(configuration);
          },
          .onStart = [this]() { startPracticeFromSelection(false); },
          .onSaveAs = [this](std::string name) {
            savePracticeAs(std::move(name));
          },
          .onRename = [this](std::string name) {
            renamePracticePreset(std::move(name));
          },
          .onUpdateNamed = [this]() { updatePracticePreset(); },
          .onDeleteNamed = [this]() { deletePracticePreset(); },
      },
      overlayPortal, [this](practice::Marker marker) {
        if (canvasView != nullptr) {
          canvasView->setActivePracticeMarker(marker);
          updateSelectionText();
        }
      });

  auto *body = new View();
  body->setFlex(1.0f);
  body->setFlexDirection(FlexDirection::Row);
  body->setAlignItems(YGAlignStretch);
  body->addView(canvasView);
  body->addView(practicePanel);

  rootLayout->addView(header);
  rootLayout->addView(toolbar);
  rootLayout->addView(body);

  updateZoomText();
  updateSelectionText();
  updateListenControls();
  updateGhostControls();
  refreshHeaderText();
  rebuildGhostModal();
  rebuildOptionsDrawer();
  rebuildRandomDrawer();
  rootLayout->addView(overlayPortal);
  rootLayout->applyYogaLayout();
}

void ChartViewerScene::rebuildRandomDrawer() {
  if (rootLayout == nullptr) {
    return;
  }

  const bool wasVisible =
      randomDrawerRoot != nullptr && randomDrawerRoot->getVisible();
  if (randomDrawerRoot == nullptr) {
    randomDrawerRoot =
        new BlockingOverlayView(0, 0, rendering::window_width,
                                rendering::window_height);
    randomDrawerRoot->setPositionType(YGPositionTypeAbsolute);
    randomDrawerRoot->setPosition(Edge::Left, 0);
    randomDrawerRoot->setPosition(Edge::Top, 0);
    randomDrawerRoot->setZIndex(1000);
    randomDrawerRoot->setVisible(false);
    randomDrawerRoot->setFlexDirection(FlexDirection::Row);
    randomDrawerRoot->setAlignItems(YGAlignStretch);
    randomDrawerRoot->setBackgroundColor(Color(2, 5, 9, 170));

    auto *spacer = new View();
    spacer->setFlex(1);
    randomDrawerRoot->addView(spacer);

    auto *panel = new View();
    panel->setWidth(std::min(520, rendering::window_width - 64));
    panel->setFlexDirection(FlexDirection::Column);
    panel->setAlignItems(YGAlignStretch);
    panel->setPadding(Edge::All, 20);
    panel->setGap(14);
    panel->setBackgroundColor(ui_theme::panelStrong());
    panel->setCornerRadius(ui_theme::panelRadius());
    panel->setShadow(ui_theme::shadow(), ui_theme::kSidePanelShadow);
    panel->setBorderColor(ui_theme::hairline());
    panel->setBorderWidth(1);

    auto *drawerHeader = new View();
    drawerHeader->setFlexDirection(FlexDirection::Row);
    drawerHeader->setAlignItems(YGAlignCenter);
    drawerHeader->setGap(12);
    drawerHeader->setHeight(58);

    auto *drawerTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    drawerTitle->setText("#RANDOM");
    drawerTitle->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    drawerTitle->setVAlign(TextView::MIDDLE);
    drawerTitle->setFlex(1);
    drawerHeader->addView(drawerTitle);

    auto *closeButton = makeButton("Close", 92, 19);
    closeButton->setOnClickListener([this]() { hideRandomDrawer(); });
    drawerHeader->addView(closeButton);
    panel->addView(drawerHeader);

    randomDrawerScroll = new ScrollView();
    randomDrawerScroll->setFlex(1);
    randomDrawerScroll->clearBackgroundColor();
    randomDrawerScroll->setCornerRadius(ui_theme::controlRadius());
    randomDrawerScroll->setBorderColor(ui_theme::hairline());
    randomDrawerScroll->setBorderWidth(1);
    randomDrawerScroll->setContentPadding(Edge::All, 12);
    panel->addView(randomDrawerScroll);

    randomDrawerRoot->addView(panel);
    rootLayout->addView(randomDrawerRoot);
  }

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignStretch);
  content->setGap(10);

  if (randomOptions.empty()) {
    randomDrawerPage = 0;
    auto *empty = new TextView("assets/fonts/notosanscjkjp.ttf", 19);
    empty->setText("No active #RANDOM.");
    empty->setColor(ui_theme::sdl(ui_theme::textMuted()));
    empty->setWrap(true);
    empty->setHeight(84);
    content->addView(empty);
  } else {
    const size_t totalOptions = randomOptions.size();
    const size_t maxPage = (totalOptions - 1) / kRandomDrawerPageSize;
    randomDrawerPage = std::min(randomDrawerPage, maxPage);
    const size_t pageStart = randomDrawerPage * kRandomDrawerPageSize;
    const size_t pageEnd =
        std::min(pageStart + kRandomDrawerPageSize, totalOptions);

    if (totalOptions > kRandomDrawerPageSize) {
      auto *pager = new View();
      pager->setFlexDirection(FlexDirection::Row);
      pager->setAlignItems(YGAlignCenter);
      pager->setGap(10);
      pager->setHeight(58);
      pager->setPadding(Edge::Left, 10);
      pager->setPadding(Edge::Right, 10);
      pager->setBackgroundColor(ui_theme::panelSubtle());
      pager->setCornerRadius(ui_theme::controlRadius());
      pager->setBorderColor(ui_theme::hairline());
      pager->setBorderWidth(1);

      auto *pageLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
      pageLabel->setText("Showing " + std::to_string(pageStart + 1) + "-" +
                         std::to_string(pageEnd) + " / " +
                         std::to_string(totalOptions));
      pageLabel->setColor(ui_theme::sdl(ui_theme::textSecondary()));
      pageLabel->setVAlign(TextView::MIDDLE);
      pageLabel->setOverflow(TextView::TextOverflow::Hidden);
      pageLabel->setFlex(1);
      pageLabel->setHeight(42);
      pager->addView(pageLabel);

      auto *prevPage = makeButton("Prev", 82, 17);
      prevPage->setOnClickListener([this]() {
        if (randomDrawerPage > 0) {
          --randomDrawerPage;
          rebuildRandomDrawer();
        }
      });
      pager->addView(prevPage);

      auto *nextPage = makeButton("Next", 82, 17);
      nextPage->setOnClickListener([this, maxPage]() {
        if (randomDrawerPage < maxPage) {
          ++randomDrawerPage;
          rebuildRandomDrawer();
        }
      });
      pager->addView(nextPage);
      content->addView(pager);
    }

    for (size_t optionIndex = pageStart; optionIndex < pageEnd;
         ++optionIndex) {
      const auto &option = randomOptions[optionIndex];
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setAlignItems(YGAlignCenter);
      row->setGap(10);
      row->setHeight(62);
      row->setPadding(Edge::Left, 10 + option.depth * 18);
      row->setPadding(Edge::Right, 10);
      row->setBackgroundColor(ui_theme::panelSubtle());
      row->setCornerRadius(ui_theme::controlRadius());
      row->setBorderColor(ui_theme::hairline());
      row->setBorderWidth(1);

      auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
      label->setText("#" + std::to_string(option.index + 1));
      label->setColor(ui_theme::sdl(ui_theme::textPrimary()));
      label->setVAlign(TextView::MIDDLE);
      label->setWidth(72);
      label->setHeight(42);
      row->addView(label);

      auto *range = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
      range->setText("1-" + std::to_string(option.maxValue));
      range->setColor(ui_theme::sdl(ui_theme::textMuted()));
      range->setVAlign(TextView::MIDDLE);
      range->setFlex(1);
      range->setHeight(42);
      row->addView(range);

      auto *prev = makeButton("<", 52, 20);
      prev->setOnClickListener([this, option]() {
        int next = option.selectedValue - 1;
        if (next < 1) {
          next = option.maxValue;
        }
        setRandomValue(option.index, next);
      });
      row->addView(prev);

      auto *value = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
      value->setText(std::to_string(option.selectedValue));
      value->setColor(ui_theme::sdl(ui_theme::amber()));
      value->setAlign(TextView::CENTER);
      value->setVAlign(TextView::MIDDLE);
      value->setWidth(64);
      value->setHeight(42);
      row->addView(value);

      auto *next = makeButton(">", 52, 20);
      next->setOnClickListener([this, option]() {
        int value = option.selectedValue + 1;
        if (value > option.maxValue) {
          value = 1;
        }
        setRandomValue(option.index, value);
      });
      row->addView(next);

      content->addView(row);
    }
  }

  if (randomDrawerScroll != nullptr) {
    randomDrawerScroll->setContentView(content);
  } else {
    delete content;
  }
  randomDrawerRoot->setVisible(wasVisible);
}

void ChartViewerScene::showRandomDrawer() {
  if (randomDrawerRoot == nullptr) {
    rebuildRandomDrawer();
  }
  if (randomDrawerRoot != nullptr) {
    randomDrawerRoot->setSize(rendering::window_width, rendering::window_height);
    randomDrawerRoot->setVisible(true);
    randomDrawerRoot->applyYogaLayout();
  }
}

void ChartViewerScene::hideRandomDrawer() {
  if (randomDrawerRoot != nullptr) {
    randomDrawerRoot->setVisible(false);
  }
}

void ChartViewerScene::parseAndRefresh(
    std::optional<std::vector<int>> requestedValues) {
  retainLoadedListenResourcesForChartChange();
  if (canvasView != nullptr) {
    canvasView->clearGhostReplay();
  }
  loadedGhostReplayId = kNoGhostReplayId;
  updatePracticeGhostReplayButton();
  updateGhostControls();
  if (statusText != nullptr) {
    statusText->setText("Parsing...");
  }

  std::atomic_bool cancelled = false;
  std::vector<unsigned char> sourceBytes;
  std::unique_ptr<bms_parser::Chart> parsed;
  try {
    std::string readError;
    if (!archive_file::readFile(record.meta.BmsPath, sourceBytes, &readError) ||
        sourceBytes.empty()) {
      SDL_Log("Chart viewer read failed: %s", readError.c_str());
      archive_file::appendDebugLogLine(
          "Chart viewer read failed: " + fspath_to_utf8(record.meta.BmsPath) +
          (readError.empty() ? "" : ": " + readError));
    } else {
      parsed = play_options::parseChartBytes(
          record.meta.BmsPath, sourceBytes, randomSeed, randomPrng,
          requestedValues, cancelled, "chart viewer");
    }
  } catch (const std::exception &e) {
    SDL_Log("Chart viewer parse failed: %s", e.what());
    archive_file::appendDebugLogLine(
        "Chart viewer parse exception: " + fspath_to_utf8(record.meta.BmsPath) +
        ": " + e.what());
  }

  if (parsed == nullptr || cancelled) {
    chart.reset();
    chartSourceBytes.clear();
    randomOptions.clear();
    if (canvasView != nullptr) {
      canvasView->setChart(nullptr);
    }
    if (statusText != nullptr) {
      statusText->setText("Parse failed");
    }
    refreshHeaderText();
    updateSelectionText();
    rebuildRandomDrawer();
    return;
  }

  if (!applyViewerPlayOptions(*parsed, "chart viewer")) {
    chart.reset();
    chartSourceBytes.clear();
    randomOptions.clear();
    if (canvasView != nullptr) {
      canvasView->setChart(nullptr);
    }
    if (statusText != nullptr) {
      statusText->setText("Play option failed");
    }
    refreshHeaderText();
    updateSelectionText();
    rebuildRandomDrawer();
    return;
  }

  int effectiveLongNoteMode =
      normalizeChartLongNoteModeValue(parsed->Meta.LnMode);
  if (effectiveLongNoteMode == 0) {
    effectiveLongNoteMode = long_note_mode::valueFromId(
        context.settings.selectedLnMode, long_note_mode::kLnValue);
  }
  applyEffectiveLongNoteModeToChart(*parsed, effectiveLongNoteMode);

  randomSeed = parsed->Meta.RandomSeed;
  randomPrng = parsed->Meta.RandomPrng;
  selectedRandomValues = parsed->Meta.RandomValues;
  chart = std::move(parsed);
  chartSourceBytes = std::move(sourceBytes);
  randomOptions = scanActiveRandomOptions(&chartSourceBytes);
  if (canvasView != nullptr) {
    canvasView->setChart(chart.get());
  }
  if (statusText != nullptr) {
    statusText->setText(std::to_string(chart->Meta.TotalNotes) + " notes");
  }
  loadPracticeConfiguration();
  refreshHeaderText();
  updateSelectionText();
  rebuildRandomDrawer();
}

void ChartViewerScene::setRandomValue(size_t index, int value) {
  if (index >= randomOptions.size()) {
    return;
  }
  const auto option = randomOptions[index];
  const std::vector<RandomOption> previousOptions = randomOptions;
  const std::vector<int> previousValues = selectedRandomValues;
  const int clampedValue = std::clamp(value, 1, option.maxValue);
  size_t nextSibling = index + 1;
  while (nextSibling < previousOptions.size() &&
         previousOptions[nextSibling].depth > option.depth) {
    ++nextSibling;
  }

  auto previousSelectedAt = [&](size_t optionIndex) {
    if (optionIndex < previousValues.size()) {
      return previousValues[optionIndex];
    }
    if (optionIndex < previousOptions.size()) {
      return previousOptions[optionIndex].selectedValue;
    }
    return 1;
  };

  std::unordered_map<size_t, int> preservedBySourceLine;
  for (size_t i = 0; i < previousOptions.size(); ++i) {
    if (i == index || (i > index && i < nextSibling)) {
      continue;
    }
    preservedBySourceLine[previousOptions[i].sourceLine] = previousSelectedAt(i);
  }

  std::vector<int> seedValues;
  seedValues.reserve(index + 1);
  for (size_t i = 0; i < index; ++i) {
    seedValues.push_back(previousSelectedAt(i));
  }
  seedValues.push_back(clampedValue);
  parseAndRefresh(seedValues);
  if (chart == nullptr) {
    return;
  }

  std::vector<int> nextValues;
  nextValues.reserve(randomOptions.size());
  for (size_t i = 0; i < randomOptions.size(); ++i) {
    const auto &current = randomOptions[i];
    if (current.sourceLine == option.sourceLine) {
      nextValues.push_back(std::clamp(clampedValue, 1, current.maxValue));
      continue;
    }
    const auto preserved = preservedBySourceLine.find(current.sourceLine);
    if (preserved != preservedBySourceLine.end()) {
      nextValues.push_back(std::clamp(preserved->second, 1, current.maxValue));
      continue;
    }
    if (i < selectedRandomValues.size()) {
      nextValues.push_back(selectedRandomValues[i]);
    } else {
      nextValues.push_back(current.selectedValue);
    }
  }

  if (nextValues != selectedRandomValues) {
    parseAndRefresh(nextValues);
  }
}

void ChartViewerScene::refreshHeaderText() {
  if (titleText != nullptr) {
    const std::string title =
        chart != nullptr && !chart->Meta.Title.empty() ? chart->Meta.Title
                                                       : record.meta.Title;
    titleText->setText(title.empty() ? "Chart Viewer" : title);
  }
  if (subtitleText != nullptr) {
    const auto &meta = chart != nullptr ? chart->Meta : record.meta;
    subtitleText->setText(meta.Artist + " / BPM " + formatDouble(meta.MinBpm) +
                          "-" + formatDouble(meta.MaxBpm) + " / " +
                          std::to_string(meta.KeyMode) + "K");
  }
  if (randomSummaryText != nullptr) {
    randomSummaryText->setText(randomSummary() + " / Option: " +
                               viewerPlayOptionLabel());
  }
}

void ChartViewerScene::updateZoomText() {
  if (zoomText == nullptr || canvasView == nullptr) {
    return;
  }
  zoomText->setText(std::to_string(static_cast<int>(
                        std::lround(canvasView->getZoom() * 100.0f))) +
                    "%");
}

void ChartViewerScene::updateSelectionText() {
  if (selectionText == nullptr) {
    return;
  }
  if (canvasView == nullptr || !canvasView->hasSelectedTime()) {
    selectionText->setText("Tap chart to set cursor.");
    return;
  }

  const auto range = canvasView->getPracticeRange();
  std::string text = "Practice " + formatMicrosTime(range.startMicros) +
                     " - " + formatMicrosTime(range.endMicros) +
                     (range.active == practice::Marker::Start ? " / Start"
                                                              : " / End");
  if (listenActive) {
    text += " / Listening";
  }
  if (loadedGhostReplayId == kPracticeGhostReplayId) {
    text += " / Practice Ghost";
  } else if (loadedGhostReplayId >= 0) {
    text += " / Ghost #" + std::to_string(loadedGhostReplayId);
  }
  selectionText->setText(text);
}

void ChartViewerScene::updateListenControls() {
  if (listenPauseButton != nullptr) {
    listenPauseButton->setVisible(listenActive);
    listenPauseButton->setWidth(listenActive ? 104.0f : 0.0f);
  }
  if (listenStopButton != nullptr) {
    listenStopButton->setVisible(listenActive);
    listenStopButton->setWidth(listenActive ? 92.0f : 0.0f);
  }
  if (listenPauseText != nullptr && listenActive) {
    listenPauseText->setText(context.jukebox.isPaused() ? "Resume" : "Pause");
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ChartViewerScene::updateGhostControls() {
  const bool hasGhost = loadedGhostReplayId != kNoGhostReplayId;
  if (ghostClearButton != nullptr) {
    ghostClearButton->setVisible(hasGhost);
    ghostClearButton->setWidth(hasGhost ? 92.0f : 0.0f);
  }
  if (ghostLoadButtonText != nullptr) {
    ghostLoadButtonText->setText("Ghost");
  }
  updateSelectionText();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ChartViewerScene::rebuildGhostModal() {
  if (rootLayout == nullptr || ghostModalRoot != nullptr) {
    return;
  }

  constexpr float kPanelWidth = 760.0f;
  constexpr float kPanelPadding = 22.0f;
  constexpr float kMinPanelMargin = 36.0f;

  ghostModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  ghostModalRoot->setPositionType(YGPositionTypeAbsolute);
  ghostModalRoot->setPosition(Edge::Left, 0);
  ghostModalRoot->setPosition(Edge::Top, 0);
  ghostModalRoot->setZIndex(1000);
  ghostModalRoot->setVisible(false);
  ghostModalRoot->setFlexDirection(FlexDirection::Column);
  ghostModalRoot->setAlignItems(YGAlignCenter);
  ghostModalRoot->setJustifyContent(YGJustifyCenter);
  ghostModalRoot->setBackgroundColor(Color(2, 5, 9, 174));

  auto *panel = new View();
  panel->setWidth(std::min<float>(kPanelWidth,
                                  rendering::window_width - kMinPanelMargin))
      ->setHeight(std::min<float>(640,
                                  rendering::window_height - kMinPanelMargin))
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kPanelPadding)
      ->setBackgroundColor(ui_theme::panelStrong())
      ->setCornerRadius(ui_theme::panelRadius())
      ->setShadow(ui_theme::shadow(), ui_theme::kModalShadow)
      ->setBorderColor(ui_theme::hairline())
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Load Ghost");
  title->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  title->setHeight(42);
  panel->addView(title);

  ghostModalEmptyText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  ghostModalEmptyText->setText("");
  ghostModalEmptyText->setColor(ui_theme::sdl(ui_theme::textMuted()));
  ghostModalEmptyText->setHeight(28);
  ghostModalEmptyText->setOverflow(TextView::TextOverflow::Hidden);
  panel->addView(ghostModalEmptyText);

  practiceGhostReplayButton = new Button();
  practiceGhostReplayButton->setWidthPercent(100)
      ->setHeight(0)
      ->setFlexShrink(0);
  practiceGhostReplayItem = new ReplaySummaryListItemView();
  practiceGhostReplayButton->setContentView(practiceGhostReplayItem);
  practiceGhostReplayButton->setOnClickListener(
      [this]() { loadPracticeGhostReplay(); });
  panel->addView(practiceGhostReplayButton);

  ghostReplayListView = new ReplaySummaryListView();
  ghostReplayListView->setWidthPercent(100)
      ->setFlexGrow(1)
      ->setFlexShrink(1)
      ->setFlexBasis(0)
      ->setMinHeight(0);
  ghostReplayListView->clearBackgroundColor();
  ghostReplayListView->setCornerRadius(ui_theme::controlRadius());
  ghostReplayListView->setBorderColor(ui_theme::hairline());
  ghostReplayListView->setBorderWidth(1);
  ghostReplayListView->onSelectionChanged = [this](int idx) {
    selectedGhostReplayIndex = idx;
    updateGhostModalActions();
  };
  panel->addView(ghostReplayListView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(8);
  footer->setHeight(kHeaderButtonHeight);
  footer->setFlexShrink(0);

  auto *closeButton = makeButton("Close", 104, 19);
  auto *clearButton = makeButton("Clear Ghost", 138, 18);
  auto *loadButton = makeButton("Load", 104, 19);
  closeButton->setFlexGrow(1)
      ->setFlexShrink(1)
      ->setFlexBasis(0)
      ->setMinWidth(0);
  clearButton->setFlexGrow(1.25f)
      ->setFlexShrink(1)
      ->setFlexBasis(0)
      ->setMinWidth(0);
  loadButton->setFlexGrow(1)
      ->setFlexShrink(1)
      ->setFlexBasis(0)
      ->setMinWidth(0);
  closeButton->setOnClickListener([this]() { hideGhostModal(); });
  clearButton->setOnClickListener([this]() { clearGhostReplay(); });
  loadButton->setOnClickListener([this]() { loadSelectedGhostReplay(); });
  footer->addView(closeButton);
  footer->addView(clearButton);
  footer->addView(loadButton);
  panel->addView(footer);

  ghostModalRoot->addView(panel);
  rootLayout->addView(ghostModalRoot);
  updatePracticeGhostReplayButton();
}

void ChartViewerScene::showGhostModal() {
  if (chart == nullptr) {
    if (statusText != nullptr) {
      statusText->setText("Open a chart first");
    }
    return;
  }
  rebuildGhostModal();
  if (ghostModalRoot == nullptr || ghostReplayListView == nullptr) {
    return;
  }

  ghostReplaySummaries = context.replayRepository.ListReplays(chart->Meta);
  selectedGhostReplayIndex = -1;
  ghostReplayListView->setReplaySummaries(ghostReplaySummaries);
  if (ghostModalEmptyText != nullptr) {
    const bool hasPracticeGhost = practiceGhostReplay.has_value() &&
                                  !practiceGhostReplay->events.empty();
    ghostModalEmptyText->setText(
        hasPracticeGhost
            ? "Practice ghost available."
            : (ghostReplaySummaries.empty()
                   ? "No saved replays."
                   : "Select a replay."));
  }
  updatePracticeGhostReplayButton();
  ghostModalRoot->setSize(rendering::window_width, rendering::window_height);
  ghostModalRoot->setVisible(true);
  updateGhostModalActions();
  ghostModalRoot->applyYogaLayout();
}

void ChartViewerScene::hideGhostModal() {
  if (ghostModalRoot != nullptr) {
    ghostModalRoot->setVisible(false);
  }
  selectedGhostReplayIndex = -1;
}

void ChartViewerScene::updateGhostModalActions() {
  if (ghostModalRoot != nullptr) {
    ghostModalRoot->applyYogaLayout();
  }
}

void ChartViewerScene::updatePracticeGhostReplayButton() {
  if (practiceGhostReplayButton == nullptr ||
      practiceGhostReplayItem == nullptr) {
    return;
  }

  const bool hasPracticeGhost =
      practiceGhostReplay.has_value() && !practiceGhostReplay->events.empty();
  practiceGhostReplayButton->setVisible(hasPracticeGhost);
  practiceGhostReplayButton->setHeight(hasPracticeGhost ? 74 : 0);
  if (!hasPracticeGhost) {
    return;
  }

  practiceGhostReplayItem->setSummary(replaySummaryFromReplay(
      *practiceGhostReplay, kPracticeGhostReplayId, "Practice Ghost"));
  if (loadedGhostReplayId == kPracticeGhostReplayId) {
    practiceGhostReplayItem->onSelected();
  } else {
    practiceGhostReplayItem->onUnselected();
  }
  if (ghostModalRoot != nullptr) {
    ghostModalRoot->applyYogaLayout();
  }
}

void ChartViewerScene::loadPracticeGhostReplay() {
  if (!practiceGhostReplay.has_value() || practiceGhostReplay->events.empty()) {
    if (statusText != nullptr) {
      statusText->setText("No practice ghost available");
    }
    return;
  }

  if (statusText != nullptr) {
    statusText->setText("Loading practice ghost...");
  }
  const ReplayData replay = *practiceGhostReplay;
  defer(
      [this, replay]() {
        applyGhostReplayData(replay, kPracticeGhostReplayId,
                             "Practice ghost loaded");
        return true;
      },
      0, true);
}

bool ChartViewerScene::applyGhostReplayData(const ReplayData &replayData,
                                            int loadedReplayId,
                                            const std::string &successText) {
  if (canvasView == nullptr) {
    return false;
  }

  std::atomic_bool parseCancelled = false;
  std::unique_ptr<bms_parser::Chart> replayChart;
  try {
    replayChart = play_options::parseChartForReplay(record.meta.BmsPath,
                                                    replayData, parseCancelled);
  } catch (const std::exception &e) {
    SDL_Log("Error parsing %s for ghost replay: %s",
            fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
  }

  if (replayChart == nullptr || parseCancelled) {
    if (statusText != nullptr) {
      statusText->setText(parseCancelled ? "Ghost load cancelled"
                                         : "Ghost parse failed");
    }
    return false;
  }

  const auto previousPlayOption = viewerPlayOption;
  const auto previousPlayOptionSeed = viewerPlayOptionSeed;
  const auto previousPlayOption2 = viewerPlayOption2;
  const auto previousPlayOption2Seed = viewerPlayOption2Seed;
  setViewerPlayOptions(replayData.playOption, replayData.playOptionSeed,
                       replayData.playOption2, replayData.playOption2Seed);
  if (!applyViewerPlayOptions(*replayChart, "ghost replay")) {
    viewerPlayOption = previousPlayOption;
    viewerPlayOptionSeed = previousPlayOptionSeed;
    viewerPlayOption2 = previousPlayOption2;
    viewerPlayOption2Seed = previousPlayOption2Seed;
    if (statusText != nullptr) {
      statusText->setText("Ghost play option failed");
    }
    return false;
  }

  retainLoadedListenResourcesForChartChange();

  randomSeed = replayChart->Meta.RandomSeed;
  randomPrng = replayChart->Meta.RandomPrng;
  selectedRandomValues = replayChart->Meta.RandomValues;
  chart = std::move(replayChart);
  randomOptions = scanActiveRandomOptions();
  canvasView->setChart(chart.get());
  canvasView->setGhostReplay(replayData);
  loadedGhostReplayId = loadedReplayId;

  loadPracticeConfiguration(false, successText);
  hideGhostModal();
  updatePracticeGhostReplayButton();
  updateGhostControls();
  refreshHeaderText();
  updateSelectionText();
  rebuildRandomDrawer();
  return true;
}

void ChartViewerScene::loadSelectedGhostReplay() {
  if (chart == nullptr || canvasView == nullptr || selectedGhostReplayIndex < 0 ||
      selectedGhostReplayIndex >=
          static_cast<int>(ghostReplaySummaries.size())) {
    return;
  }

  const int replayId = ghostReplaySummaries[selectedGhostReplayIndex].id;
  if (statusText != nullptr) {
    statusText->setText("Loading ghost...");
  }

  defer(
      [this, replayId]() {
        if (canvasView == nullptr) {
          return true;
        }

        const bms_parser::ChartMeta &loadMeta =
            chart != nullptr ? chart->Meta : record.meta;
        auto replay =
            context.replayRepository.LoadReplay(replayId, loadMeta);
        if (!replay.has_value()) {
          if (statusText != nullptr) {
            statusText->setText("Ghost load failed");
          }
          return true;
        }

        applyGhostReplayData(*replay, replay->id,
                             "Ghost #" + std::to_string(replay->id) +
                                 " loaded");
        return true;
      },
      0, true);
}

void ChartViewerScene::clearGhostReplay() {
  if (canvasView != nullptr) {
    canvasView->clearGhostReplay();
  }
  loadedGhostReplayId = kNoGhostReplayId;
  if (statusText != nullptr && chart != nullptr) {
    statusText->setText(std::to_string(chart->Meta.TotalNotes) + " notes");
  }
  hideGhostModal();
  updatePracticeGhostReplayButton();
  updateGhostControls();
}

void ChartViewerScene::rebuildOptionsDrawer() {
  if (rootLayout == nullptr || optionsDrawerRoot != nullptr) {
    return;
  }

  constexpr float kPanelWidth = 760.0f;
  constexpr float kPanelPadding = 22.0f;
  constexpr float kContentWidth = kPanelWidth - kPanelPadding * 2.0f - 18.0f;
  optionsDrawerRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                              rendering::window_height);
  optionsDrawerRoot->setPositionType(YGPositionTypeAbsolute);
  optionsDrawerRoot->setPosition(Edge::Left, 0);
  optionsDrawerRoot->setPosition(Edge::Top, 0);
  optionsDrawerRoot->setZIndex(1000);
  optionsDrawerRoot->setVisible(false);
  optionsDrawerRoot->setFlexDirection(FlexDirection::Column);
  optionsDrawerRoot->setAlignItems(YGAlignCenter);
  optionsDrawerRoot->setJustifyContent(YGJustifyCenter);
  optionsDrawerRoot->setBackgroundColor(Color(2, 5, 9, 174));

  auto *panel = new View();
  panel->setWidth(std::min<float>(kPanelWidth, rendering::window_width - 36))
      ->setHeight(std::min<float>(760, rendering::window_height - 36))
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, kPanelPadding)
      ->setBackgroundColor(ui_theme::panelStrong())
      ->setCornerRadius(ui_theme::panelRadius())
      ->setShadow(ui_theme::shadow(), ui_theme::kModalShadow)
      ->setBorderColor(ui_theme::hairline())
      ->setBorderWidth(1);

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setGap(12);
  header->setHeight(52);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
  title->setText("Chart Option");
  title->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  title->setVAlign(TextView::MIDDLE);
  title->setFlex(1);
  header->addView(title);

  auto *closeButton = makeButton("Close", 98, 19);
  closeButton->setOnClickListener([this]() { hideOptionsDrawer(); });
  header->addView(closeButton);
  panel->addView(header);

  auto *scroll = new ScrollView();
  scroll->setFlex(1.0f);
  scroll->clearBackgroundColor();
  scroll->setContentPadding(Edge::Right, 12);

  auto *content = new View();
  content->setWidth(kContentWidth);
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignStretch);
  content->setGap(12);

  auto *currentRow = new View();
  currentRow->setFlexDirection(FlexDirection::Row);
  currentRow->setAlignItems(YGAlignCenter);
  currentRow->setGap(12);
  currentRow->setHeight(52);

  auto *currentLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 19);
  currentLabel->setText("Current");
  currentLabel->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  currentLabel->setVAlign(TextView::MIDDLE);
  currentLabel->setWidth(86);
  currentLabel->setHeight(44);
  currentRow->addView(currentLabel);

  viewerOptionText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  viewerOptionText->setColor(ui_theme::sdl(ui_theme::amber()));
  viewerOptionText->setVAlign(TextView::MIDDLE);
  viewerOptionText->setOverflow(TextView::TextOverflow::Hidden);
  viewerOptionText->setFlex(1);
  viewerOptionText->setHeight(44);
  currentRow->addView(viewerOptionText);
  content->addView(currentRow);

  viewerPlayOptionsPanel = new PlayOptionsPanelView(
      {.onPlayOptionSelected = [this](const std::string &option) {
         setViewerNamedPlayOption(option);
       },
       .onLaneOrderSubmitted = [this](const std::string &notation) {
         setViewerLaneAssign(notation);
       },
       .onLongNoteModeSelected = [this](const std::string &mode) {
         setViewerLongNoteMode(mode);
       },
       .onAssistOptionSelected = [this](const std::string &option) {
         setViewerAssistOption(option);
       },
       .onPlaybackRateSelected = [this](int percent) {
         setViewerPlaybackRate(percent);
       },
       .onPlaybackModeSelected = [this](const std::string &mode) {
         setViewerPlaybackMode(mode);
       },
       .onClubModeToggled = [this]() { toggleViewerClubMode(); }},
      {.width = kContentWidth,
       .playOptionColumns = 4,
       .showGauge = false,
       .showLaneOrder = true,
       .showPacemaker = false},
      overlayPortal);
  content->addView(viewerPlayOptionsPanel);

  scroll->setContentView(content);
  panel->addView(scroll);

  optionsDrawerRoot->addView(panel);
  rootLayout->addView(optionsDrawerRoot);
  refreshOptionsDrawer();
}

void ChartViewerScene::showOptionsDrawer() {
  rebuildOptionsDrawer();
  if (optionsDrawerRoot == nullptr) {
    return;
  }
  refreshOptionsDrawer();
  optionsDrawerRoot->setSize(rendering::window_width, rendering::window_height);
  optionsDrawerRoot->setVisible(true);
  optionsDrawerRoot->applyYogaLayout();
}

void ChartViewerScene::hideOptionsDrawer() {
  if (viewerPlayOptionsPanel != nullptr) {
    viewerPlayOptionsPanel->closeDropdowns();
  }
  if (optionsDrawerRoot != nullptr) {
    optionsDrawerRoot->setVisible(false);
  }
}

void ChartViewerScene::refreshOptionsDrawer() {
  if (viewerOptionText != nullptr) {
    viewerOptionText->setText(viewerPlayOptionLabel());
  }
  refreshViewerOptionControls();
}

void ChartViewerScene::setViewerAssistOption(const std::string &option) {
  viewerAssistOption = assist_options::normalize(option);
  context.settings.selectedAssistOption = viewerAssistOption;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save chart viewer assist option");
  }
  refreshOptionsDrawer();
}

void ChartViewerScene::setViewerLongNoteMode(const std::string &mode) {
  if (normalizeChartLongNoteModeValue(record.meta.LnMode) > 0) {
    return;
  }
  context.settings.selectedLnMode =
      long_note_mode::parseId(mode, AppSettings::kDefaultLnMode);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save chart viewer long note mode");
  }
  parseAndRefresh(selectedRandomValues.empty()
                      ? std::nullopt
                      : std::optional<std::vector<int>>(selectedRandomValues));
  refreshOptionsDrawer();
}

void ChartViewerScene::setViewerPlaybackRate(int percent) {
  auto configuration = practiceConfiguration;
  configuration.playback.percent = std::clamp(percent, 50, 200);
  onPracticeConfigurationChanged(configuration);
  refreshViewerOptionControls();
}

void ChartViewerScene::setViewerPlaybackMode(const std::string &mode) {
  if (mode != "pitch-shift") {
    return;
  }
  auto configuration = practiceConfiguration;
  configuration.playback.mode = audio::PlaybackMode::PitchShift;
  onPracticeConfigurationChanged(configuration);
  refreshViewerOptionControls();
}

void ChartViewerScene::toggleViewerClubMode() {
  context.settings.gameplayClubModeEnabled =
      !context.settings.gameplayClubModeEnabled;
  if (!context.saveSettings()) {
    SDL_Log("Failed to save chart viewer Club mode");
  }
  refreshViewerOptionControls();
}

void ChartViewerScene::refreshViewerOptionControls() {
  if (viewerPlayOptionsPanel == nullptr) {
    return;
  }
  const int fixedLongNoteMode =
      normalizeChartLongNoteModeValue(record.meta.LnMode);
  const bool longNoteModeLocked = fixedLongNoteMode > 0;
  const std::string selectedLongNoteMode =
      longNoteModeLocked
          ? long_note_mode::idFromValue(fixedLongNoteMode,
                                        AppSettings::kDefaultLnMode)
          : long_note_mode::parseId(context.settings.selectedLnMode,
                                    AppSettings::kDefaultLnMode);
  const bms_parser::ChartMeta &meta = chart != nullptr ? chart->Meta : record.meta;
  viewerPlayOptionsPanel->refresh(
      {.playOption = viewerPlayOption.value_or("NORMAL"),
       .defaultLaneOrder = play_options::defaultLaneAssignNotation(meta),
       .laneOrderEnabled = true,
       .longNoteMode = selectedLongNoteMode,
       .longNoteModeLocked = longNoteModeLocked,
       .assistOption = viewerAssistOption,
       .assistOptionLocked = false,
       .playbackRatePercent = practiceConfiguration.playback.percent,
       .playbackLocked = false,
       .clubMode = context.settings.gameplayClubModeEnabled});
}

void ChartViewerScene::setViewerNamedPlayOption(const std::string &option) {
  if (viewerPlayOptionsPanel != nullptr) {
    viewerPlayOptionsPanel->setLaneOrderMessage("");
  }
  const std::string normalized = play_options::normalizePlayOption(option);
  if (play_options::laneAssignNotationFromOption(normalized).has_value()) {
    setViewerLaneAssign(normalized);
    return;
  }

  setViewerPlayOptions(
      play_options::isNormalPlayOption(normalized)
          ? std::nullopt
          : std::optional<std::string>(normalized),
      std::nullopt,
      chart != nullptr && chart->Meta.IsDP &&
              !play_options::isNormalPlayOption(normalized)
          ? std::optional<std::string>(normalized)
          : std::nullopt,
      std::nullopt);
  context.settings.selectedPlayOption = normalized;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save chart viewer play option");
  }
  parseAndRefresh(selectedRandomValues.empty()
                      ? std::nullopt
                      : std::optional<std::vector<int>>(selectedRandomValues));
  refreshOptionsDrawer();
}

void ChartViewerScene::setViewerLaneAssign(const std::string &notation) {
  const std::string option = play_options::makeLaneAssignOption(notation);
  std::string error;
  const bms_parser::ChartMeta &meta = chart != nullptr ? chart->Meta : record.meta;
  if (play_options::isNormalPlayOption(option) ||
      !play_options::validateLaneAssignOption(meta, option, &error)) {
    if (viewerPlayOptionsPanel != nullptr) {
      viewerPlayOptionsPanel->setLaneOrderMessage(
          error.empty() ? "Invalid lane order." : error, true);
    }
    return;
  }

  setViewerPlayOptions(option, std::nullopt, std::nullopt, std::nullopt);
  parseAndRefresh(selectedRandomValues.empty()
                      ? std::nullopt
                      : std::optional<std::vector<int>>(selectedRandomValues));
  refreshOptionsDrawer();
  if (viewerPlayOptionsPanel != nullptr) {
    viewerPlayOptionsPanel->setLaneOrderMessage("Lane order applied.");
  }
}

void ChartViewerScene::setViewerPlayOptions(
    const std::optional<std::string> &option,
    const std::optional<long long> &seed,
    const std::optional<std::string> &option2,
    const std::optional<long long> &seed2) {
  viewerLaneOrderSummary.reset();
  viewerPlayOption = storedPlayOption(option);
  viewerPlayOptionSeed = viewerPlayOption.has_value() ? seed : std::nullopt;
  if (viewerPlayOption.has_value() &&
      play_options::laneAssignNotationFromOption(*viewerPlayOption)
          .has_value()) {
    viewerPlayOptionSeed.reset();
    viewerPlayOption2.reset();
    viewerPlayOption2Seed.reset();
    return;
  }
  viewerPlayOption2 = storedPlayOption(option2);
  viewerPlayOption2Seed = viewerPlayOption2.has_value() ? seed2 : std::nullopt;
}

bool ChartViewerScene::applyViewerPlayOptions(bms_parser::Chart &target,
                                              const char *logContext) {
  viewerLaneOrderSummary.reset();
  if (!target.Meta.IsDP) {
    viewerPlayOption2.reset();
    viewerPlayOption2Seed.reset();
  }

  const bool shouldSummarizeLaneOrder =
      isLaneOrderSummaryOption(viewerPlayOption) &&
      (!target.Meta.IsDP || isLaneOrderSummaryOption(viewerPlayOption2));
  const std::vector<int> identityLaneOrder = target.Meta.GetTotalLaneIndices();
  std::vector<int> combinedLaneOrder = identityLaneOrder;
  auto mergeLaneOrder = [&](const std::vector<int> &laneOrder) {
    if (laneOrder.size() != combinedLaneOrder.size() ||
        laneOrder.size() != identityLaneOrder.size()) {
      return;
    }
    for (size_t i = 0; i < laneOrder.size(); ++i) {
      if (laneOrder[i] != identityLaneOrder[i]) {
        combinedLaneOrder[i] = laneOrder[i];
      }
    }
  };

  if (viewerPlayOption.has_value() &&
      play_options::laneAssignNotationFromOption(*viewerPlayOption)
          .has_value()) {
    std::optional<std::string> appliedOption;
    std::optional<long long> appliedSeed;
    if (!play_options::applyPlayOptionModifier(
            target, *viewerPlayOption, std::nullopt, 0, appliedOption,
            appliedSeed, logContext)) {
      return false;
    }
    viewerPlayOption = appliedOption;
    viewerPlayOptionSeed.reset();
    viewerPlayOption2.reset();
    viewerPlayOption2Seed.reset();
    viewerLaneOrderSummary.reset();
    return true;
  }

  if (viewerPlayOption.has_value()) {
    std::optional<std::string> appliedOption;
    std::optional<long long> appliedSeed;
    std::vector<int> appliedLaneOrder;
    if (!play_options::applyPlayOptionModifier(
            target, *viewerPlayOption, viewerPlayOptionSeed, 0, appliedOption,
            appliedSeed, logContext, &appliedLaneOrder)) {
      return false;
    }
    viewerPlayOption = appliedOption;
    viewerPlayOptionSeed = appliedSeed;
    if (shouldSummarizeLaneOrder) {
      mergeLaneOrder(appliedLaneOrder);
    }
  }

  if (target.Meta.IsDP && viewerPlayOption2.has_value()) {
    std::optional<std::string> appliedOption;
    std::optional<long long> appliedSeed;
    std::vector<int> appliedLaneOrder;
    if (!play_options::applyPlayOptionModifier(
            target, *viewerPlayOption2, viewerPlayOption2Seed, 1,
            appliedOption, appliedSeed, logContext, &appliedLaneOrder)) {
      return false;
    }
    viewerPlayOption2 = appliedOption;
    viewerPlayOption2Seed = appliedSeed;
    if (shouldSummarizeLaneOrder) {
      mergeLaneOrder(appliedLaneOrder);
    }
  }

  if (shouldSummarizeLaneOrder) {
    viewerLaneOrderSummary = formatLaneOrderSummary(target.Meta,
                                                    combinedLaneOrder);
  }

  return true;
}

void ChartViewerScene::onCanvasSelectionChanged(long long timeMicros) {
  (void)timeMicros;
  if (listenActive) {
    stopListening();
  }
  updateSelectionText();
}

void ChartViewerScene::onPracticeRangeChanged(
    const practice::RangeSelection &range) {
  if (chart == nullptr) {
    return;
  }
  practiceConfiguration.startMicros = range.startMicros;
  practiceConfiguration.endMicros = range.endMicros;
  practiceConfiguration =
      sanitizePracticeConfiguration(practiceConfiguration,
                                    practiceChartEndMicros, chart.get())
          .configuration;
  if (practicePresetStore != nullptr) {
    std::string error;
    if (!practicePresetStore->saveLastUsed(practiceConfiguration.chartSha256,
                                           practiceConfiguration, error) &&
        practicePanel != nullptr) {
      practicePanel->setPresetMessage("Could not save practice settings: " +
                                          error,
                                      true);
    }
  }
  refreshPracticePanel();
}

void ChartViewerScene::onPracticeConfigurationChanged(
    const practice::Configuration &configuration) {
  if (chart == nullptr) {
    return;
  }
  selectedPracticePresetId =
      practicePanel == nullptr ? std::nullopt
                               : practicePanel->selectedPresetId();
  practiceConfiguration =
      sanitizePracticeConfiguration(configuration, practiceChartEndMicros,
                                    chart.get())
          .configuration;
  if (canvasView != nullptr) {
    auto range = canvasView->getPracticeRange();
    range.startMicros = practiceConfiguration.startMicros;
    range.endMicros = practiceConfiguration.endMicros;
    canvasView->setPracticeRange(range);
  }
  if (practicePresetStore != nullptr) {
    std::string error;
    if (!practicePresetStore->saveLastUsed(practiceConfiguration.chartSha256,
                                           practiceConfiguration, error) &&
        practicePanel != nullptr) {
      practicePanel->setPresetMessage("Could not save practice settings: " +
                                          error,
                                      true);
    }
  }
  refreshPracticePanel();
  updateSelectionText();
}

void ChartViewerScene::selectActivePracticeMarker(practice::Marker marker) {
  if (canvasView == nullptr) {
    return;
  }
  canvasView->setActivePracticeMarker(marker);
  refreshPracticePanel();
  updateSelectionText();
}

void ChartViewerScene::moveActivePracticeMarker(
    practice::TimelineDirection direction) {
  if (canvasView != nullptr) {
    (void)canvasView->moveActivePracticeMarker(direction);
  }
}

bool ChartViewerScene::applyPracticePresetLoad(
    practice::PresetLoadResult loaded, bool applyLastUsed) {
  const auto notice = loaded.notice();
  const bool usable = practice::installPresetLoadState(
      std::move(loaded), applyLastUsed, practiceConfiguration,
      practiceNamedPresets, selectedPracticePresetId);
  if (notice && practicePanel != nullptr) {
    practicePanel->setPresetMessage(*notice, true);
  }
  return usable;
}

void ChartViewerScene::loadPracticeConfiguration(
    bool applyPendingLaunch,
    std::optional<std::string> chartReplacementSuccessText) {
  if (chart == nullptr) {
    return;
  }
  const long long newChartEndMicros =
      chart_playback_duration::ChartTimelineEndMicros(*chart);
  practicePresetStore = std::make_unique<practice::PresetStore>(
      context.profileManager.activePaths().practiceDirectory);
  const std::string chartHash =
      chart->Meta.SHA256.empty() ? record.meta.SHA256 : chart->Meta.SHA256;
  auto loaded = practicePresetStore->load(chartHash, newChartEndMicros);
  const auto loadStatus = loaded.status;
  const auto applyMissingDefaults = [this, loadStatus]() {
    if (loadStatus != versioned_json::LoadStatus::Missing) {
      return;
    }
    const GaugeSelection gaugeSelection =
        gaugeSelectionFromSettingId(context.settings.selectedGaugeType);
    practiceConfiguration.gaugeType = gaugeSelection.type;
    const GaugeAutoShiftMode storedAutoShift = gaugeAutoShiftFromSettingId(
        context.settings.selectedGaugeAutoShiftMode);
    practiceConfiguration.gaugeAutoShift =
        storedAutoShift != GaugeAutoShiftMode::None ? storedAutoShift
                                                   : gaugeSelection.autoShift;
    practiceConfiguration.gaugeAutoShiftLowerBound =
        gaugeSelectionFromSettingId(
            context.settings.selectedGaugeAutoShiftLowerBound)
            .type;
    practiceConfiguration.countInBeats =
        practice::defaultCountInBeatsForChart(
            chart->Meta.GuessedBeatsPerMeasure);
  };
  const auto refreshInstalledState = [this, &applyMissingDefaults]() {
    applyMissingDefaults();
    practiceConfiguration =
        sanitizePracticeConfiguration(practiceConfiguration,
                                      practiceChartEndMicros, chart.get())
            .configuration;
    if (canvasView != nullptr) {
      canvasView->setPracticeRange(
          {.startMicros = practiceConfiguration.startMicros,
           .endMicros = practiceConfiguration.endMicros,
           .active = practice::Marker::Start});
    }
    refreshPracticePanel();
  };
  if (chartReplacementSuccessText.has_value()) {
    chart_viewer_practice::GhostRefreshState state{
        .chartEndMicros = practiceChartEndMicros,
        .configuration = std::move(practiceConfiguration),
        .namedPresets = std::move(practiceNamedPresets),
        .selectedPresetId = std::move(selectedPracticePresetId),
        .pendingLaunchRequest = std::move(pendingPracticeLaunchRequest),
        .ghostReplay = std::move(practiceGhostReplay),
        .loadedGhostReplayId = loadedGhostReplayId,
        .playOption = std::move(viewerPlayOption),
        .playOptionSeed = std::move(viewerPlayOptionSeed),
        .playOption2 = std::move(viewerPlayOption2),
        .playOption2Seed = std::move(viewerPlayOption2Seed)};
    (void)chart_viewer_practice::installGhostRefreshState(
        std::move(state), newChartEndMicros, std::move(loaded),
        *chartReplacementSuccessText,
        [this, &refreshInstalledState](
            chart_viewer_practice::GhostRefreshState installed) {
          practiceChartEndMicros = installed.chartEndMicros;
          practiceConfiguration = std::move(installed.configuration);
          practiceNamedPresets = std::move(installed.namedPresets);
          selectedPracticePresetId = std::move(installed.selectedPresetId);
          pendingPracticeLaunchRequest =
              std::move(installed.pendingLaunchRequest);
          practiceGhostReplay = std::move(installed.ghostReplay);
          loadedGhostReplayId = installed.loadedGhostReplayId;
          viewerPlayOption = std::move(installed.playOption);
          viewerPlayOptionSeed = std::move(installed.playOptionSeed);
          viewerPlayOption2 = std::move(installed.playOption2);
          viewerPlayOption2Seed = std::move(installed.playOption2Seed);
          refreshInstalledState();
          if (statusText != nullptr) {
            statusText->setText(installed.visibleStatus);
          }
        });
    return;
  }

  practiceChartEndMicros = newChartEndMicros;
  (void)applyPracticePresetLoad(std::move(loaded), true);
  refreshInstalledState();
  if (applyPendingLaunch) {
    applyPendingPracticeLaunchRequest();
  }
}

void ChartViewerScene::applyPendingPracticeLaunchRequest() {
  if (!pendingPracticeLaunchRequest.has_value() || chart == nullptr) {
    return;
  }

  const practice::LaunchRequest request =
      std::move(*pendingPracticeLaunchRequest);
  pendingPracticeLaunchRequest.reset();
  auto application = practice::applyLaunchRequestForParsedChart(
      practiceConfiguration, request, chart->Meta, practiceChartEndMicros);
  if (!application.applied()) {
    if (statusText != nullptr) {
      statusText->setText(application.issue.value_or("Chart unavailable"));
    }
    return;
  }

  practiceRuleset = request.ruleset;
  practiceRequiredRulesetDescriptor = request.requiredRulesetDescriptor;
  practiceReplayRulesetSnapshot = request.replayRulesetSnapshot;

  practiceConfiguration =
      sanitizePracticeConfiguration(std::move(application.configuration),
                                    practiceChartEndMicros, chart.get())
          .configuration;
  selectedPracticePresetId.reset();
  if (canvasView != nullptr) {
    canvasView->setPracticeRange(
        {.startMicros = practiceConfiguration.startMicros,
         .endMicros = practiceConfiguration.endMicros,
         .active = practice::Marker::Start});
  }
  if (practicePanel != nullptr) {
    practicePanel->setDisplay(YGDisplayFlex);
    practicePanel->setVisible(true);
  }
  if (practicePresetStore != nullptr) {
    std::string error;
    if (!practicePresetStore->saveLastUsed(practiceConfiguration.chartSha256,
                                           practiceConfiguration, error) &&
        practicePanel != nullptr) {
      practicePanel->setPresetMessage("Could not save practice settings: " +
                                          error,
                                      true);
      refreshPracticePanel();
      updateSelectionText();
      return;
    }
  }
  if (statusText != nullptr) {
    statusText->setText("Section ready");
  }
  refreshPracticePanel();
  updateSelectionText();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void ChartViewerScene::refreshPracticePanel() {
  if (practicePanel == nullptr) {
    return;
  }
  practicePanel->setChartEndMicros(practiceChartEndMicros);
  practicePanel->setStartingGaugeRange(
      practiceStartingGaugeMaximum(practiceConfiguration, chart.get()),
      practice::defaultStartingGaugePercent(
          practiceConfiguration, practiceGaugeProfileForChart(chart.get())));
  const practice::Marker active =
      canvasView == nullptr ? practice::Marker::Start
                            : canvasView->getPracticeRange().active;
  practicePanel->refresh(practiceConfiguration, practiceNamedPresets,
                         selectedPracticePresetId, active);
}

void ChartViewerScene::savePracticeAs(std::string name) {
  if (practicePresetStore == nullptr) {
    return;
  }
  std::string error;
  const auto id = practicePresetStore->saveNamed(
      practiceConfiguration.chartSha256, std::move(name),
      practiceConfiguration, error);
  if (!id) {
    if (practicePanel != nullptr) {
      practicePanel->setPresetMessage(error, true);
    }
    return;
  }
  auto loaded = practicePresetStore->load(practiceConfiguration.chartSha256,
                                          practiceChartEndMicros);
  if (applyPracticePresetLoad(std::move(loaded), false)) {
    selectedPracticePresetId = *id;
  }
  refreshPracticePanel();
  if (practicePanel != nullptr) {
    practicePanel->setPresetMessage("Preset saved.");
  }
}

void ChartViewerScene::renamePracticePreset(std::string name) {
  if (practicePresetStore == nullptr || !selectedPracticePresetId) {
    return;
  }
  std::string error;
  if (!practicePresetStore->renameNamed(practiceConfiguration.chartSha256,
                                        *selectedPracticePresetId,
                                        std::move(name), error)) {
    if (practicePanel != nullptr) {
      practicePanel->setPresetMessage(error, true);
    }
    return;
  }
  auto loaded = practicePresetStore->load(practiceConfiguration.chartSha256,
                                          practiceChartEndMicros);
  (void)applyPracticePresetLoad(std::move(loaded), false);
  refreshPracticePanel();
  if (practicePanel != nullptr) {
    practicePanel->setPresetMessage("Preset renamed.");
  }
}

void ChartViewerScene::updatePracticePreset() {
  if (practicePresetStore == nullptr || !selectedPracticePresetId) {
    return;
  }
  std::string error;
  if (!practicePresetStore->updateNamed(
          practiceConfiguration.chartSha256, *selectedPracticePresetId,
          practiceConfiguration, error)) {
    if (practicePanel != nullptr) {
      practicePanel->setPresetMessage(error, true);
    }
    return;
  }
  auto loaded = practicePresetStore->load(practiceConfiguration.chartSha256,
                                          practiceChartEndMicros);
  (void)applyPracticePresetLoad(std::move(loaded), false);
  refreshPracticePanel();
  if (practicePanel != nullptr) {
    practicePanel->setPresetMessage("Preset updated.");
  }
}

void ChartViewerScene::deletePracticePreset() {
  if (practicePresetStore == nullptr || !selectedPracticePresetId) {
    return;
  }
  std::string error;
  if (!practicePresetStore->deleteNamed(practiceConfiguration.chartSha256,
                                        *selectedPracticePresetId, error)) {
    if (practicePanel != nullptr) {
      practicePanel->setPresetMessage(error, true);
    }
    return;
  }
  auto loaded = practicePresetStore->load(practiceConfiguration.chartSha256,
                                          practiceChartEndMicros);
  if (applyPracticePresetLoad(std::move(loaded), false)) {
    selectedPracticePresetId.reset();
  }
  refreshPracticePanel();
  if (practicePanel != nullptr) {
    practicePanel->setPresetMessage("Preset deleted.");
  }
}

void ChartViewerScene::retainLoadedListenResourcesForChartChange() {
  stopListening();
  if (listenAudioLoaded) {
    context.jukebox.stop();
    listenAudioLoaded = false;
    retainedListenResourcesForReload = true;
    listenEndMicros = 0;
    return;
  }
  if (retainedListenResourcesForReload) {
    context.jukebox.stop();
    listenEndMicros = 0;
  }
}

void ChartViewerScene::startListeningFromSelection() {
  if (chart == nullptr || canvasView == nullptr ||
      !canvasView->hasSelectedTime()) {
    if (statusText != nullptr) {
      statusText->setText("Set a cursor first");
    }
    return;
  }

  const auto practiceRange = canvasView->getPracticeRange();
  const long long selectedTime = chart_viewer_listen::resolveStartMicros(
      practiceConfiguration, canvasView->getSelectedTimeMicros(),
      practiceRange.active);
  if (statusText != nullptr) {
    statusText->setText(
        listenAudioLoaded
            ? "Seeking audio..."
            : (retainedListenResourcesForReload ? "Updating audio..."
                                                : "Loading audio..."));
  }
  listenActive = false;
  canvasView->clearPlaybackTime();
  updateListenControls();

  defer(
      [this, selectedTime]() {
        if (chart == nullptr || canvasView == nullptr) {
          return true;
        }

        std::atomic_bool cancelled = false;
        std::string musicStopError;
        context.musicPlayer.Stop(musicStopError);
        if (!listenAudioLoaded) {
          const bool previousVisuals = context.jukebox.getVisualsEnabled();
          context.jukebox.stop();
          context.jukebox.setVisualsEnabled(false);
          if (retainedListenResourcesForReload) {
            context.jukebox.reloadChartResources(*chart, true, cancelled);
          } else {
            context.jukebox.loadChart(*chart, true, cancelled);
          }
          context.jukebox.setVisualsEnabled(previousVisuals);
          if (cancelled) {
            if (statusText != nullptr) {
              statusText->setText("Audio load cancelled");
            }
            updateListenControls();
            return true;
          }
          listenAudioLoaded = true;
          retainedListenResourcesForReload = false;
        } else {
          context.jukebox.stop();
        }
        listenEndMicros = std::max(
            chart_playback_duration::ChartTimelineEndMicros(*chart),
            context.jukebox.getScheduledAudioEndMicros());

        context.jukebox.play();
        context.jukebox.seek(std::max(0LL, selectedTime));
        listenActive = true;
        canvasView->setPlaybackTime(
            selectedTime +
                static_cast<long long>(context.settings.audioOffsetMs) *
                    1000LL,
            true);
        if (statusText != nullptr && chart != nullptr) {
          statusText->setText(std::to_string(chart->Meta.TotalNotes) +
                              " notes");
        }
        updateSelectionText();
        updateListenControls();
        return true;
      },
      0, true);
}

void ChartViewerScene::toggleListenPause() {
  if (!listenActive) {
    return;
  }
  if (context.jukebox.isPaused()) {
    context.jukebox.resume();
  } else {
    context.jukebox.pause();
  }
  updateSelectionText();
  updateListenControls();
}

void ChartViewerScene::stopListening() {
  if (listenActive) {
    context.jukebox.stop();
  }
  listenActive = false;
  if (canvasView != nullptr) {
    canvasView->clearPlaybackTime();
  }
  updateSelectionText();
  updateListenControls();
}

void ChartViewerScene::startPracticeFromSelection(bool autoPlay) {
  if (chart == nullptr || canvasView == nullptr) {
    return;
  }

  const auto sanitized =
      sanitizePracticeConfiguration(practiceConfiguration,
                                    practiceChartEndMicros, chart.get());
  if (!sanitized.playable()) {
    if (statusText != nullptr) {
      statusText->setText(sanitized.diagnostics.empty()
                              ? "Practice configuration is not playable"
                              : sanitized.diagnostics.front());
    }
    return;
  }
  const practice::Configuration launchConfiguration =
      sanitized.configuration;
  const long long selectedTime = launchConfiguration.startMicros;
  const auto chartRandomSeed = chart->Meta.RandomSeed;
  const auto chartRandomPrng = chart->Meta.RandomPrng;
  const std::optional<std::vector<int>> chartRandomValues =
      chart->Meta.RandomValues.empty()
          ? std::nullopt
          : std::optional<std::vector<int>>(chart->Meta.RandomValues);
  const GaugeSelection gaugeSelection{
      .type = launchConfiguration.gaugeType,
      .autoShift = launchConfiguration.gaugeAutoShift};
  const bool autoKeySound = autoPlay || !context.settings.inputKeysoundEnabled;
  const std::string assistOption = viewerAssistOption;
  const int selectedLongNoteMode =
      normalizeChartLongNoteModeValue(record.meta.LnMode) > 0
          ? normalizeChartLongNoteModeValue(record.meta.LnMode)
          : long_note_mode::valueFromId(context.settings.selectedLnMode,
                                        long_note_mode::kLnValue);
  const bool canReuseJukeboxResources =
      listenAudioLoaded || retainedListenResourcesForReload;

  stopListening();
  if (statusText != nullptr) {
    statusText->setText(autoPlay ? "Preparing auto play..."
                                 : "Preparing practice...");
  }

  defer(
      [this, selectedTime, chartRandomSeed, chartRandomPrng, chartRandomValues,
       gaugeSelection, autoKeySound, assistOption, selectedLongNoteMode,
       autoPlay, canReuseJukeboxResources, launchConfiguration]() {
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> practiceChart;
        try {
          practiceChart =
              play_options::parseChart(record.meta.BmsPath, chartRandomSeed,
                                       chartRandomPrng, chartRandomValues,
                                       parseCancelled,
                                       autoPlay ? "practice autoplay"
                                                : "practice");
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for %s: %s",
                  fspath_to_utf8(record.meta.BmsPath).c_str(),
                  autoPlay ? "practice autoplay" : "practice", e.what());
          archive_file::appendDebugLogLine(
              std::string(autoPlay ? "Practice autoplay" : "Practice") +
              " parse exception: " + fspath_to_utf8(record.meta.BmsPath) +
              ": " + e.what());
        }
        if (practiceChart == nullptr || parseCancelled) {
          if (statusText != nullptr) {
            statusText->setText(autoPlay ? "Auto play parse failed"
                                         : "Practice parse failed");
          }
          return true;
        }

        if (!applyViewerPlayOptions(*practiceChart,
                                    autoPlay ? "practice autoplay"
                                             : "practice")) {
          if (statusText != nullptr) {
            statusText->setText(autoPlay ? "Auto play option failed"
                                         : "Practice play option failed");
          }
          return true;
        }
        context.jukebox.stop();
        if (canReuseJukeboxResources) {
          context.jukebox.reloadChartResources(*practiceChart, true,
                                               parseCancelled);
        } else {
          context.jukebox.loadChart(*practiceChart, true, parseCancelled);
        }
        if (parseCancelled) {
          if (statusText != nullptr) {
            statusText->setText(autoPlay ? "Auto play load cancelled"
                                         : "Practice load cancelled");
          }
          return true;
        }
        listenAudioLoaded = false;
        retainedListenResourcesForReload = true;
        listenEndMicros = 0;

        if (statusText != nullptr && chart != nullptr) {
          statusText->setText(std::to_string(chart->Meta.TotalNotes) +
                              " notes");
        }
        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(
                context, std::move(practiceChart),
                StartOptions{
                    .startPosition = static_cast<unsigned long long>(
                        std::max(0LL, selectedTime)),
                    .autoKeySound = autoKeySound,
                    .autoPlay = autoPlay,
                    .gaugeType = gaugeSelection.type,
                    .gaugeAutoShift = gaugeSelection.autoShift,
                    .gaugeAutoShiftLowerBound =
                        launchConfiguration.gaugeAutoShiftLowerBound,
                    .playOption = viewerPlayOption,
                    .playOptionSeed = viewerPlayOptionSeed,
                    .playOption2 = viewerPlayOption2,
                    .playOption2Seed = viewerPlayOption2Seed,
                    .longNoteMode = selectedLongNoteMode,
                    .assistOption = assistOption,
                    .pacemakerTarget = pacemaker::kTargetOff,
                    .ownsChart = true,
                    .practiceSession = std::make_shared<practice::Session>(
                        launchConfiguration),
                    .practiceMode = autoPlay,
                    .practiceLeadInMicros =
                        static_cast<unsigned long long>(kPracticeLeadInMicros),
                    .playback = launchConfiguration.playback,
                    .clubMode = context.settings.gameplayClubModeEnabled,
                    .judgeWindowScalePercent =
                        launchConfiguration.judge.scalePercent,
                    .startingGaugePercent =
                        launchConfiguration.startingGaugePercent,
                    .returnScene = this,
                    .touchVisualizationEnabled =
                        autoPlay ? std::optional<bool>(false) : std::nullopt,
                    .replayGhostRenderingEnabled =
                        autoPlay ? std::optional<bool>(false) : std::nullopt,
                    .practiceGhostCallback =
                        [this](const ReplayData &replayData) {
                          setPracticeGhostReplay(replayData);
                        },
                    .ruleset = practiceRuleset,
                    .requiredRulesetDescriptor =
                        practiceRequiredRulesetDescriptor,
                    .replayRulesetOverride = practiceReplayRulesetSnapshot,
                }),
            true);
        return true;
      },
      0, true);
}

void ChartViewerScene::goBack() {
  stopListening();
  context.sceneManager->changeScene("MainMenu", false);
}

std::vector<ChartViewerScene::RandomOption>
ChartViewerScene::scanActiveRandomOptions(
    const std::vector<unsigned char> *sourceBytes) const {
  std::vector<RandomOption> options;
  if (record.meta.BmsPath.empty()) {
    return options;
  }

  std::vector<unsigned char> bytes;
  if (sourceBytes == nullptr && !chartSourceBytes.empty()) {
    sourceBytes = &chartSourceBytes;
  }
  if (sourceBytes == nullptr) {
    if (!archive_file::readFile(record.meta.BmsPath, bytes) || bytes.empty()) {
      return options;
    }
    sourceBytes = &bytes;
  }
  if (sourceBytes->empty()) {
    return options;
  }

  std::string content;
  bms_parser::ShiftJISConverter::BytesToUTF8(sourceBytes->data(),
                                             sourceBytes->size(), content);

  struct ConditionalFrame {
    bool parentSkipped = false;
    bool branchMatched = false;
    bool currentSkipped = false;
    size_t randomDepth = 0;
  };
  struct RandomFrame {
    bool active = false;
  };
  std::vector<int> randomStack;
  std::vector<RandomFrame> randomFrames;
  std::vector<ConditionalFrame> conditionalStack;
  auto isSkipping = [&]() {
    for (const auto &frame : conditionalStack) {
      if (frame.currentSkipped) {
        return true;
      }
    }
    for (const auto &frame : randomFrames) {
      if (!frame.active) {
        return true;
      }
    }
    return false;
  };
  auto popRandomFrame = [&]() {
    if (randomFrames.empty()) {
      return;
    }
    const bool wasActive = randomFrames.back().active;
    randomFrames.pop_back();
    if (wasActive && !randomStack.empty()) {
      randomStack.pop_back();
    }
  };
  auto isInsideConditionalBranchOfRandomDepth = [&](size_t randomDepth) {
    for (auto it = conditionalStack.rbegin(); it != conditionalStack.rend();
         ++it) {
      if (it->randomDepth == randomDepth) {
        return true;
      }
      if (it->randomDepth < randomDepth) {
        return false;
      }
    }
    return false;
  };
  auto closeUnbranchedRandomFrames = [&]() {
    while (!randomFrames.empty() &&
           !isInsideConditionalBranchOfRandomDepth(randomFrames.size())) {
      popRandomFrame();
    }
  };
  auto activeRandomDepth = [&]() {
    int depth = 0;
    for (const auto &frame : randomFrames) {
      if (frame.active) {
        ++depth;
      }
    }
    return depth;
  };

  std::istringstream stream(content);
  std::string line;
  size_t sourceLine = 0;
  while (std::getline(stream, line)) {
    ++sourceLine;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.size() <= 1 || line[0] != '#') {
      continue;
    }

    if (matchHeader(line, "#IF")) {
      const bool parentSkipped = isSkipping();
      if (randomStack.empty() && !parentSkipped) {
        continue;
      }
      const int currentRandom = parentSkipped ? 0 : randomStack.back();
      const int n =
          static_cast<int>(std::strtol(line.substr(4).c_str(), nullptr, 10));
      const bool matched = !parentSkipped && currentRandom == n;
      conditionalStack.push_back({parentSkipped, matched,
                                  parentSkipped || !matched,
                                  randomFrames.size()});
      continue;
    }
    if (matchHeader(line, "#ELSEIF")) {
      if (conditionalStack.empty()) {
        continue;
      }
      auto &frame = conditionalStack.back();
      const int n =
          static_cast<int>(std::strtol(line.substr(8).c_str(), nullptr, 10));
      if (frame.parentSkipped || frame.branchMatched || randomStack.empty()) {
        frame.currentSkipped = true;
        continue;
      }
      const bool matched = randomStack.back() == n;
      frame.branchMatched = matched;
      frame.currentSkipped = !matched;
      continue;
    }
    if (matchHeader(line, "#ELSE")) {
      if (conditionalStack.empty()) {
        continue;
      }
      auto &frame = conditionalStack.back();
      if (frame.parentSkipped) {
        frame.currentSkipped = true;
      } else {
        frame.currentSkipped = frame.branchMatched;
        frame.branchMatched = true;
      }
      continue;
    }
    if (matchHeader(line, "#ENDIF") || matchHeader(line, "#END IF")) {
      if (!conditionalStack.empty()) {
        conditionalStack.pop_back();
      }
      continue;
    }
    if (matchHeader(line, "#RANDOM") || matchHeader(line, "#RONDAM")) {
      closeUnbranchedRandomFrames();
      if (isSkipping()) {
        randomFrames.push_back({false});
        continue;
      }
      const int n =
          static_cast<int>(std::strtol(line.substr(7).c_str(), nullptr, 10));
      if (n <= 0) {
        continue;
      }
      const size_t index = options.size();
      int selected = 1;
      if (index < selectedRandomValues.size()) {
        selected = std::clamp(selectedRandomValues[index], 1, n);
      }
      options.push_back({index, n, selected, activeRandomDepth(), sourceLine});
      randomStack.push_back(selected);
      randomFrames.push_back({true});
      continue;
    }
    if (matchHeader(line, "#ENDRANDOM")) {
      if (randomFrames.empty()) {
        continue;
      }
      popRandomFrame();
      continue;
    }
  }

  return options;
}

std::string ChartViewerScene::randomSummary() const {
  if (selectedRandomValues.empty()) {
    return "RANDOM: none";
  }
  if (selectedRandomValues.size() > kRandomSummaryLimit) {
    return "RANDOM: " +
           joinRandomValueRange(selectedRandomValues, 0, kRandomSummaryHead) +
           " ... " +
           joinRandomValueRange(selectedRandomValues,
                                selectedRandomValues.size() -
                                    kRandomSummaryTail,
                                selectedRandomValues.size()) +
           " (" + std::to_string(selectedRandomValues.size()) + " values)";
  }
  return "RANDOM: " + joinRandomValues(selectedRandomValues);
}

std::string ChartViewerScene::viewerPlayOptionLabel() const {
  const std::string label = play_options::formatPlayOptionLabel(
      viewerPlayOption, viewerPlayOptionSeed, viewerPlayOption2,
      viewerPlayOption2Seed);
  std::string result = label.empty() ? "NORMAL" : label;
  if (viewerLaneOrderSummary.has_value()) {
    result += " / Lane " + *viewerLaneOrderSummary;
  }
  return result;
}
