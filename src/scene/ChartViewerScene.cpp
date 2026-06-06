#include "ChartViewerScene.h"

#include "../PlayOptionUtils.h"
#include "../path.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/TexBatchRenderer.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/View.h"
#include "play/GamePlayScene.h"

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
constexpr float kMinZoom = 0.55f;
constexpr float kMaxZoom = 2.0f;
constexpr float kMinScreenZoom = 0.65f;
constexpr float kMaxScreenZoom = 3.0f;
constexpr size_t kRandomDrawerPageSize = 96;
constexpr size_t kRandomSummaryLimit = 48;
constexpr size_t kRandomSummaryHead = 32;
constexpr size_t kRandomSummaryTail = 8;
constexpr int kMarkerLabelFontSize = 13;
constexpr float kMarkerLabelWidth = 74.0f;
constexpr float kMarkerLabelHeight = 18.0f;
constexpr float kChartContentTopPadding = 48.0f;
constexpr float kChartContentBottomPadding = 64.0f;
constexpr float kCursorTapSlop = 10.0f;
constexpr long long kListenStopTailMicros = 1000000LL;
constexpr long long kPracticeLeadInMicros = 3000000LL;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct GaugeSelection {
  GaugeType type = GaugeType::Normal;
  bool autoShift = false;
};

GaugeSelection gaugeSelectionFromSettingId(const std::string &id) {
  if (id == "gas") {
    return {.type = GaugeType::ExHard, .autoShift = true};
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
  return {.type = GaugeType::Normal};
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

std::string markerGlyphCacheKey(char glyph, const SDL_Color &color) {
  std::string key = std::to_string(markerLabelColorKey(color));
  key.push_back(':');
  key.push_back(glyph);
  return key;
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
  button->setContentView(text);
  button->setStyledBorderWidth(2);
  button->setBackgroundColors(Color(29, 32, 35, 232),
                              Color(42, 47, 49, 242),
                              Color(56, 63, 65, 248));
  button->setBorderColors(Color(78, 86, 89, 232),
                          Color(119, 134, 136, 246),
                          Color(158, 177, 179, 255));
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

class BlockingOverlayView : public View {
public:
  using View::View;

private:
  bool handleEventsImpl(SDL_Event &event) override {
    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      return false;
    default:
      return true;
    }
  }
};

} // namespace

class ChartCanvasView : public View {
public:
  ChartCanvasView() {
    setBackgroundColor(Color(8, 9, 11, 255));
    batch.setSubmitView(rendering::ui_view);
    markerTextBatch.setSubmitView(rendering::ui_view);
  }

  void setChart(bms_parser::Chart *newChart) {
    chart = newChart;
    markerGlyphTextures.clear();
    selectedTimeMicros.reset();
    playbackActive = false;
    rebuildLayout();
  }

  void setZoom(float newZoom) {
    zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
    rebuildLayout();
  }

  [[nodiscard]] float getZoom() const { return zoom; }

  void setSelectionListener(std::function<void(long long)> listener) {
    selectionListener = std::move(listener);
  }

  [[nodiscard]] bool hasSelectedTime() const {
    return selectedTimeMicros.has_value();
  }

  [[nodiscard]] long long getSelectedTimeMicros() const {
    return selectedTimeMicros.value_or(0LL);
  }

  void setPlaybackTime(long long timeMicros, bool active) {
    playbackTimeMicros = std::max(0LL, timeMicros);
    playbackActive = active;
  }

  void clearPlaybackTime() { playbackActive = false; }

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

    batch.begin();
    drawGrid();
    drawMarkers();
    drawLongNotes();
    drawNotes();
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
          touchGestureWasPinch = false;
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

  bms_parser::Chart *chart = nullptr;
  rendering::SimpleBatchRenderer batch;
  rendering::TexBatchRenderer markerTextBatch;
  std::vector<int> laneOrder;
  std::unordered_map<int, size_t> laneToOrderIndex;
  std::vector<MeasureLayout> measureLayouts;
  std::vector<ColumnLayout> columnLayouts;
  std::vector<const bms_parser::TimeLine *> orderedTimelines;
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
  float columnGap = 36.0f;
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
  std::optional<long long> selectedTimeMicros;
  long long playbackTimeMicros = 0;
  bool playbackActive = false;
  std::function<void(long long)> selectionListener;

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

    laneWidth = laneCount > 12 ? 18.0f : (laneCount > 8 ? 21.0f : 24.0f);
    laneWidth *= std::clamp(zoom, 0.8f, 1.35f);
    gutterWidth = 54.0f;
    laneAreaWidth = static_cast<float>(laneCount) * laneWidth;
    columnGap = 34.0f;
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
    const float baseMeasureHeight = 136.0f * zoom;
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
    if (selectedTimeMicros.has_value()) {
      drawCursorBar(*selectedTimeMicros, Color(255, 220, 92, 235), 4.0f);
    }
    if (playbackActive) {
      drawCursorBar(playbackTimeMicros, Color(91, 218, 236, 242), 3.0f);
    }
  }

  void drawCursorBar(long long timeMicros, Color color, float thickness) {
    CursorDrawPosition position;
    if (!timeToCursorPosition(timeMicros, position)) {
      return;
    }
    const float laneX = position.x + gutterWidth;
    drawRectClip(laneX, position.y - thickness * 0.5f, laneAreaWidth,
                 thickness, color.toABGR());
    drawRectClip(position.x + gutterWidth - 6.0f,
                 position.y - thickness * 1.4f, 6.0f, thickness * 2.8f,
                 color.toABGR());
  }

  void selectAtUiPoint(float uiX, float uiY) {
    if (chart == nullptr) {
      return;
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
      const double beatPosition = layout.beatStart + layout.scale * local;
      const long long timeMicros = beatToTimeMicros(beatPosition);
      selectedTimeMicros = std::max(0LL, timeMicros);
      playbackActive = false;
      if (selectionListener != nullptr) {
        selectionListener(*selectedTimeMicros);
      }
      return;
    }
  }

  bool timeToCursorPosition(long long timeMicros,
                            CursorDrawPosition &position) const {
    if (measureLayouts.empty()) {
      return false;
    }
    return beatToCursorPosition(timeToBeatPosition(timeMicros), position);
  }

  bool beatToCursorPosition(double beatPosition,
                            CursorDrawPosition &position) const {
    if (measureLayouts.empty()) {
      return false;
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
      const double local =
          std::clamp((beatPosition - start) / layout.scale, 0.0, 1.0);
      position.column = layout.column;
      position.x = layout.x;
      position.y = layout.y + layout.height -
                   static_cast<float>(local) * layout.height;
      return true;
    }

    const auto &fallback =
        beatPosition < measureLayouts.front().beatStart ? measureLayouts.front()
                                                        : measureLayouts.back();
    const double local = beatPosition < fallback.beatStart ? 0.0 : 1.0;
    position.column = fallback.column;
    position.x = fallback.x;
    position.y = fallback.y + fallback.height -
                 static_cast<float>(local) * fallback.height;
    return true;
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
      const float labelBoxWidth = std::max(60.0f, kMarkerLabelWidth * screenZoom);
      const float labelBoxHeight =
          std::max(16.0f, kMarkerLabelHeight * screenZoom);
      const float labelTextScale = std::max(1.0f, screenZoom);
      const float labelScreenX = contentToScreenX(x);
      const float labelScreenY = contentToScreenY(y);
      const float labelRight = labelScreenX + labelBoxWidth;
      float cursorX = labelScreenX;
      for (char glyphChar : marker.text) {
        const auto *glyph = cachedMarkerGlyph(glyphChar, marker.color);
        if (glyph == nullptr) {
          continue;
        }

        const float glyphWidth = std::max(1.0f, glyph->width) * labelTextScale;
        const float glyphHeight = glyph->height * labelTextScale;
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
    markerTextBatch.begin();
    for (const auto &draw : visibleMarkerLabelDraws) {
      markerTextBatch.addRectUV(draw.x, draw.y, draw.width, draw.height, 0.0f,
                                1.0f, draw.u1, 0.0f, draw.texture);
    }
    markerTextBatch.end();
    markerTextBatch.clearScissor();
  }

  const CachedMarkerGlyph *cachedMarkerGlyph(char glyph,
                                             const SDL_Color &color) {
    const std::string key = markerGlyphCacheKey(glyph, color);
    if (const auto it = markerGlyphTextures.find(key);
        it != markerGlyphTextures.end()) {
      return &it->second;
    }

    CachedMarkerGlyph label;
    label.text =
        std::make_unique<TextView>("assets/fonts/notosanscjkjp.ttf",
                                   kMarkerLabelFontSize);
    label.text->setColor(color);
    label.text->setText(std::string(1, glyph));
    label.texture = label.text->textureHandle();
    label.width = glyph == ' ' ? static_cast<float>(kMarkerLabelFontSize) * 0.34f
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
    if (dynamic_cast<const bms_parser::LongNote *>(note) != nullptr) {
      return lane % 2 == 0 ? Color(225, 232, 230, 245).toABGR()
                           : Color(84, 151, 224, 245).toABGR();
    }
    return lane % 2 == 0 ? Color(236, 240, 238, 248).toABGR()
                         : Color(82, 154, 226, 248).toABGR();
  }

  uint32_t longNoteColor(int lane) const {
    if (isScratchLane(lane)) {
      return Color(231, 94, 58, 164).toABGR();
    }
    return lane % 2 == 0 ? Color(226, 232, 230, 154).toABGR()
                         : Color(82, 154, 226, 164).toABGR();
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
    std::optional<std::vector<int>> randomValues)
    : Scene(context), record(std::move(record)), randomSeed(randomSeed),
      randomPrng(std::move(randomPrng)) {
  if (randomValues.has_value()) {
    selectedRandomValues = *randomValues;
  }
}

void ChartViewerScene::init() {
  initView();
  parseAndRefresh(selectedRandomValues.empty()
                      ? std::nullopt
                      : std::optional<std::vector<int>>(selectedRandomValues));
}

EventHandleResult ChartViewerScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    goBack();
    return {};
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
    if (chart != nullptr &&
        rawTime >= chart->Meta.TotalLength + kListenStopTailMicros) {
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
  }
}

void ChartViewerScene::renderScene() {}

void ChartViewerScene::cleanupScene() {
  if (listenActive || listenAudioLoaded) {
    context.jukebox.stop();
    listenActive = false;
    listenAudioLoaded = false;
  }
  chart.reset();
  randomOptions.clear();
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
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setBackgroundColor(Color(8, 9, 11, 255));

  auto *header = new View();
  header->setHeight(safe.top + 98);
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setPadding(Edge::Top, safe.top + 10);
  header->setPadding(Edge::Left, safe.left + kHeaderPadding);
  header->setPadding(Edge::Right, safe.right + kHeaderPadding);
  header->setPadding(Edge::Bottom, 10);
  header->setGap(12);
  header->setBackgroundColor(Color(20, 22, 25, 250));
  header->setBorderColor(Color(56, 63, 66, 255));
  header->setBorderWidth(1);

  auto *titleColumn = new View();
  titleColumn->setFlexDirection(FlexDirection::Column);
  titleColumn->setAlignItems(YGAlignStretch);
  titleColumn->setFlex(1);
  titleColumn->setMinWidth(0);
  titleColumn->setGap(2);

  titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 29);
  titleText->setColor({244, 246, 245, 255});
  titleText->setHeight(36);
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  subtitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  subtitleText->setColor({177, 187, 189, 255});
  subtitleText->setHeight(23);
  subtitleText->setOverflow(TextView::TextOverflow::Hidden);
  randomSummaryText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  randomSummaryText->setColor({242, 209, 106, 255});
  randomSummaryText->setHeight(21);
  randomSummaryText->setOverflow(TextView::TextOverflow::Hidden);
  titleColumn->addView(titleText);
  titleColumn->addView(subtitleText);
  titleColumn->addView(randomSummaryText);
  header->addView(titleColumn);

  statusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  statusText->setColor({218, 226, 224, 255});
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
  zoomText->setColor({232, 237, 235, 255});
  zoomText->setWidth(70);
  zoomText->setHeight(kHeaderButtonHeight);
  auto *zoomInButton = makeButton("+", 52, 26, &zoomButtonText);
  (void)zoomButtonText;
  zoomOutButton->setOnClickListener([this]() {
    if (canvasView != nullptr) {
      canvasView->setZoom(canvasView->getZoom() - 0.12f);
      updateZoomText();
    }
  });
  zoomInButton->setOnClickListener([this]() {
    if (canvasView != nullptr) {
      canvasView->setZoom(canvasView->getZoom() + 0.12f);
      updateZoomText();
    }
  });
  header->addView(zoomOutButton);
  header->addView(zoomText);
  header->addView(zoomInButton);

  auto *randomButton = makeButton("Random", 118, 20);
  randomButton->setOnClickListener([this]() { showRandomDrawer(); });
  header->addView(randomButton);

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
  toolbar->setBackgroundColor(Color(13, 15, 17, 246));
  toolbar->setBorderColor(Color(40, 46, 49, 255));
  toolbar->setBorderWidth(1);

  selectionText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  selectionText->setColor({206, 216, 214, 255});
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

  auto *practiceButton = makeButton("Practice", 128, 18);
  practiceButton->setOnClickListener([this]() { startPracticeFromSelection(); });
  toolbar->addView(practiceButton);

  canvasView = new ChartCanvasView();
  canvasView->setFlex(1);
  canvasView->setSelectionListener(
      [this](long long timeMicros) { onCanvasSelectionChanged(timeMicros); });

  rootLayout->addView(header);
  rootLayout->addView(toolbar);
  rootLayout->addView(canvasView);
  addView(rootLayout);

  updateZoomText();
  updateSelectionText();
  updateListenControls();
  refreshHeaderText();
  rebuildRandomDrawer();
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
    randomDrawerRoot->setBackgroundColor(Color(0, 0, 0, 154));

    auto *spacer = new View();
    spacer->setFlex(1);
    randomDrawerRoot->addView(spacer);

    auto *panel = new View();
    panel->setWidth(std::min(520, rendering::window_width - 64));
    panel->setFlexDirection(FlexDirection::Column);
    panel->setAlignItems(YGAlignStretch);
    panel->setPadding(Edge::All, 20);
    panel->setGap(14);
    panel->setBackgroundColor(Color(18, 20, 22, 248));
    panel->setBorderColor(Color(79, 88, 91, 255));
    panel->setBorderWidth(2);

    auto *drawerHeader = new View();
    drawerHeader->setFlexDirection(FlexDirection::Row);
    drawerHeader->setAlignItems(YGAlignCenter);
    drawerHeader->setGap(12);
    drawerHeader->setHeight(58);

    auto *drawerTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    drawerTitle->setText("#RANDOM");
    drawerTitle->setColor({244, 246, 245, 255});
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
    randomDrawerScroll->setBorderColor(Color(52, 59, 62, 255));
    randomDrawerScroll->setBorderWidth(1);
    panel->addView(randomDrawerScroll);

    randomDrawerRoot->addView(panel);
    rootLayout->addView(randomDrawerRoot);
  }

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignStretch);
  content->setPadding(Edge::All, 12);
  content->setGap(10);

  if (randomOptions.empty()) {
    randomDrawerPage = 0;
    auto *empty = new TextView("assets/fonts/notosanscjkjp.ttf", 19);
    empty->setText("No active #RANDOM in this interpretation.");
    empty->setColor({178, 187, 188, 255});
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
      pager->setBackgroundColor(Color(22, 25, 27, 232));
      pager->setBorderColor(Color(61, 69, 72, 224));
      pager->setBorderWidth(1);

      auto *pageLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
      pageLabel->setText("Showing " + std::to_string(pageStart + 1) + "-" +
                         std::to_string(pageEnd) + " / " +
                         std::to_string(totalOptions));
      pageLabel->setColor({218, 226, 224, 255});
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
      row->setBackgroundColor(Color(27, 30, 32, 226));
      row->setBorderColor(Color(61, 69, 72, 224));
      row->setBorderWidth(1);

      auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
      label->setText("#" + std::to_string(option.index + 1));
      label->setColor({235, 239, 237, 255});
      label->setVAlign(TextView::MIDDLE);
      label->setWidth(72);
      label->setHeight(42);
      row->addView(label);

      auto *range = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
      range->setText("1-" + std::to_string(option.maxValue));
      range->setColor({159, 172, 173, 255});
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
      value->setColor({242, 209, 106, 255});
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
  stopListening();
  if (listenAudioLoaded) {
    context.jukebox.stop();
    listenAudioLoaded = false;
  }
  if (statusText != nullptr) {
    statusText->setText("Parsing...");
  }

  std::atomic_bool cancelled = false;
  std::unique_ptr<bms_parser::Chart> parsed;
  try {
    parsed = play_options::parseChart(record.meta.BmsPath, randomSeed,
                                      randomPrng, requestedValues, cancelled,
                                      "chart viewer");
  } catch (const std::exception &e) {
    SDL_Log("Chart viewer parse failed: %s", e.what());
  }

  if (parsed == nullptr || cancelled) {
    chart.reset();
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

  randomSeed = parsed->Meta.RandomSeed;
  randomPrng = parsed->Meta.RandomPrng;
  selectedRandomValues = parsed->Meta.RandomValues;
  chart = std::move(parsed);
  randomOptions = scanActiveRandomOptions();
  if (canvasView != nullptr) {
    canvasView->setChart(chart.get());
  }
  if (statusText != nullptr) {
    statusText->setText(std::to_string(chart->Meta.TotalNotes) + " notes");
  }
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
    randomSummaryText->setText(randomSummary());
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
    selectionText->setText("Tap the chart to place a cursor.");
    return;
  }

  std::string text =
      "Cursor " + formatMicrosTime(canvasView->getSelectedTimeMicros());
  if (listenActive) {
    text += " / Listening";
  }
  selectionText->setText(text);
}

void ChartViewerScene::updateListenControls() {
  if (listenPauseButton != nullptr) {
    listenPauseButton->setVisible(listenActive);
  }
  if (listenStopButton != nullptr) {
    listenStopButton->setVisible(listenActive);
  }
  if (listenPauseText != nullptr && listenActive) {
    listenPauseText->setText(context.jukebox.isPaused() ? "Resume" : "Pause");
  }
}

void ChartViewerScene::onCanvasSelectionChanged(long long timeMicros) {
  (void)timeMicros;
  if (listenActive) {
    stopListening();
  }
  updateSelectionText();
}

void ChartViewerScene::startListeningFromSelection() {
  if (chart == nullptr || canvasView == nullptr ||
      !canvasView->hasSelectedTime()) {
    if (statusText != nullptr) {
      statusText->setText("Set a cursor first");
    }
    return;
  }

  const long long selectedTime = canvasView->getSelectedTimeMicros();
  if (statusText != nullptr) {
    statusText->setText(listenAudioLoaded ? "Seeking audio..."
                                          : "Loading audio...");
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
        if (!listenAudioLoaded) {
          const bool previousVisuals = context.jukebox.getVisualsEnabled();
          context.jukebox.stop();
          context.jukebox.setVisualsEnabled(false);
          context.jukebox.loadChart(*chart, true, cancelled);
          context.jukebox.setVisualsEnabled(previousVisuals);
          if (cancelled) {
            if (statusText != nullptr) {
              statusText->setText("Audio load cancelled");
            }
            updateListenControls();
            return true;
          }
          listenAudioLoaded = true;
        } else {
          context.jukebox.stop();
        }

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

void ChartViewerScene::startPracticeFromSelection() {
  if (chart == nullptr || canvasView == nullptr ||
      !canvasView->hasSelectedTime()) {
    if (statusText != nullptr) {
      statusText->setText("Set a cursor first");
    }
    return;
  }

  const long long selectedTime = canvasView->getSelectedTimeMicros();
  const auto chartRandomSeed = chart->Meta.RandomSeed;
  const auto chartRandomPrng = chart->Meta.RandomPrng;
  const std::optional<std::vector<int>> chartRandomValues =
      chart->Meta.RandomValues.empty()
          ? std::nullopt
          : std::optional<std::vector<int>>(chart->Meta.RandomValues);
  const std::string playOption =
      play_options::normalizePlayOption(context.settings.selectedPlayOption);
  const GaugeSelection gaugeSelection =
      gaugeSelectionFromSettingId(context.settings.selectedGaugeType);
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;

  stopListening();
  if (statusText != nullptr) {
    statusText->setText("Preparing practice...");
  }

  defer(
      [this, selectedTime, chartRandomSeed, chartRandomPrng, chartRandomValues,
       playOption, gaugeSelection, autoKeySound]() {
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> practiceChart;
        try {
          practiceChart =
              play_options::parseChart(record.meta.BmsPath, chartRandomSeed,
                                       chartRandomPrng, chartRandomValues,
                                       parseCancelled, "practice");
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for practice: %s",
                  path_t_to_utf8(record.meta.BmsPath).c_str(), e.what());
        }
        if (practiceChart == nullptr || parseCancelled) {
          if (statusText != nullptr) {
            statusText->setText("Practice parse failed");
          }
          return true;
        }

        play_options::PlayOptionReplayInfo playInfo =
            play_options::applySelectedPlayOptions(*practiceChart, playOption);
        context.jukebox.stop();
        context.jukebox.loadChart(*practiceChart, true, parseCancelled);
        if (parseCancelled) {
          if (statusText != nullptr) {
            statusText->setText("Practice load cancelled");
          }
          return true;
        }

        auto *loadedChart = practiceChart.release();
        if (statusText != nullptr && chart != nullptr) {
          statusText->setText(std::to_string(chart->Meta.TotalNotes) +
                              " notes");
        }
        context.sceneManager->changeScene(
            new GamePlayScene(context, loadedChart,
                              {
                                  .startPosition =
                                      static_cast<unsigned long long>(
                                          std::max(0LL, selectedTime)),
                                  .autoKeySound = autoKeySound,
                                  .autoPlay = false,
                                  .gaugeType = gaugeSelection.type,
                                  .gaugeAutoShift = gaugeSelection.autoShift,
                                  .playOption = playInfo.option,
                                  .playOptionSeed = playInfo.seed,
                                  .playOption2 = playInfo.option2,
                                  .playOption2Seed = playInfo.seed2,
                                  .ownsChart = true,
                                  .practiceMode = true,
                                  .practiceLeadInMicros =
                                      static_cast<unsigned long long>(
                                          kPracticeLeadInMicros),
                                  .returnScene = this,
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
ChartViewerScene::scanActiveRandomOptions() const {
  std::vector<RandomOption> options;
  if (record.meta.BmsPath.empty()) {
    return options;
  }

  std::ifstream file(record.meta.BmsPath, std::ios::binary);
  if (!file) {
    return options;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size <= 0) {
    return options;
  }
  file.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  file.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));

  std::string content;
  bms_parser::ShiftJISConverter::BytesToUTF8(bytes.data(), bytes.size(),
                                             content);

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
