#include "ChartViewerScene.h"

#include "../PlayOptionUtils.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/View.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
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

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

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
  while (text.size() > 1 && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
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
  button->setBackgroundColors(Color(22, 32, 45, 224),
                              Color(34, 49, 68, 235),
                              Color(48, 69, 94, 245));
  button->setBorderColors(Color(88, 111, 139, 230),
                          Color(126, 155, 189, 245),
                          Color(166, 194, 226, 255));
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
    setBackgroundColor(Color(0, 0, 0, 255));
    batch.setSubmitView(rendering::ui_view);
  }

  void setChart(bms_parser::Chart *newChart) {
    chart = newChart;
    rebuildLayout();
  }

  void setZoom(float newZoom) {
    zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
    rebuildLayout();
  }

  [[nodiscard]] float getZoom() const { return zoom; }

protected:
  void renderImpl(RenderContext &context) override {
    if (chart == nullptr) {
      return;
    }

    if (layoutDirty) {
      rebuildLayout();
    }

    batch.begin();
    drawGrid();
    drawLongNotes();
    drawNotes();
    drawMarkers();
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
      scrollX += horizontal * 88.0f;
      scrollY += static_cast<float>(-event.wheel.y) * 12.0f;
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
      return false;
    }
    case SDL_MOUSEMOTION: {
      if (!mouseDragging) {
        return true;
      }
      int uiX = 0;
      int uiY = 0;
      mouseMotionToUi(event.motion, uiX, uiY);
      scrollX -= static_cast<float>(uiX - lastMouseX);
      scrollY -= static_cast<float>(uiY - lastMouseY);
      lastMouseX = uiX;
      lastMouseY = uiY;
      clampScroll();
      return false;
    }
    case SDL_MOUSEBUTTONUP:
      if (event.button.button == SDL_BUTTON_LEFT && mouseDragging) {
        mouseDragging = false;
        return false;
      }
      return true;
    case SDL_FINGERDOWN: {
      if (activeTouchId != -1) {
        return true;
      }
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
      if (!containsPoint(uiX, uiY)) {
        return true;
      }
      activeTouchId = event.tfinger.fingerId;
      lastTouchX = uiX;
      lastTouchY = uiY;
      return false;
    }
    case SDL_FINGERMOTION: {
      if (event.tfinger.fingerId != activeTouchId) {
        return true;
      }
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
      scrollX -= uiX - lastTouchX;
      scrollY -= uiY - lastTouchY;
      lastTouchX = uiX;
      lastTouchY = uiY;
      clampScroll();
      return false;
    }
    case SDL_FINGERUP:
      if (event.tfinger.fingerId == activeTouchId) {
        activeTouchId = -1;
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
    std::unique_ptr<TextView> text;
  };

  bms_parser::Chart *chart = nullptr;
  rendering::SimpleBatchRenderer batch;
  std::vector<int> laneOrder;
  std::unordered_map<int, size_t> laneToOrderIndex;
  std::vector<MeasureLayout> measureLayouts;
  std::vector<ColumnLayout> columnLayouts;
  std::unordered_map<const bms_parser::TimeLine *, float> timelineY;
  std::unordered_map<const bms_parser::TimeLine *, size_t> timelineMeasure;
  std::vector<std::unique_ptr<TextView>> measureLabels;
  std::vector<MarkerLabel> markerLabels;
  float zoom = 1.0f;
  float laneWidth = 24.0f;
  float laneAreaWidth = 0.0f;
  float gutterWidth = 54.0f;
  float columnGap = 36.0f;
  float columnWidth = 0.0f;
  float contentWidth = 0.0f;
  float contentHeight = 0.0f;
  float scrollX = 0.0f;
  float scrollY = 0.0f;
  bool layoutDirty = false;
  bool mouseDragging = false;
  int lastMouseX = 0;
  int lastMouseY = 0;
  SDL_FingerID activeTouchId = -1;
  float lastTouchX = 0.0f;
  float lastTouchY = 0.0f;

  void rebuildLayout() {
    layoutDirty = false;
    measureLayouts.clear();
    columnLayouts.clear();
    timelineY.clear();
    timelineMeasure.clear();
    measureLabels.clear();
    markerLabels.clear();
    laneToOrderIndex.clear();

    if (chart == nullptr) {
      contentWidth = 0.0f;
      contentHeight = 0.0f;
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
    const float maxColumnHeight =
        std::max(280.0f, static_cast<float>(getHeight() - 16));
    const float baseMeasureHeight = 68.0f * zoom;

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

      const float columnX = static_cast<float>(column) * columnWidth;
      float cursorY = maxColumnHeight;
      ColumnLayout columnLayout{columnX, maxColumnHeight, 0.0f};
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

    contentHeight = maxColumnHeight;
    contentWidth =
        columnLayouts.empty()
            ? 0.0f
            : columnLayouts.back().x + gutterWidth + laneAreaWidth + 78.0f;

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
        timelineY[timeline] = y;
        timelineMeasure[timeline] = layoutIndex;
        appendMarkerLabel(timeline);
      }
    }

    clampScroll();
  }

  void appendMarkerLabel(const bms_parser::TimeLine *timeline) {
    auto addLabel = [&](MarkerType type, std::string text,
                        SDL_Color color) {
      auto label =
          std::make_unique<TextView>("assets/fonts/notosanscjkjp.ttf", 13);
      label->setText(std::move(text));
      label->setColor(color);
      label->setAlign(TextView::LEFT);
      label->setVAlign(TextView::MIDDLE);
      label->setOverflow(TextView::TextOverflow::Hidden);
      label->setSize(74, 18);
      markerLabels.push_back({timeline, type, std::move(label)});
    };

    if (timeline->BpmChange) {
      addLabel(MarkerType::Bpm, formatDouble(timeline->Bpm),
               {108, 255, 94, 255});
    }
    if (timeline->StopLength > 0.0) {
      addLabel(MarkerType::Stop,
               formatDouble(timeline->StopLength, 1) + " STOP",
               {255, 245, 48, 255});
    }
    if (timeline->ScrollChange) {
      addLabel(MarkerType::Scroll,
               "SCROLL " + formatDouble(timeline->Scroll),
               {78, 223, 255, 255});
    }
  }

  void drawGrid() {
    const uint32_t gutterColor = Color(126, 128, 128, 255).toABGR();
    const uint32_t laneBackground = Color(4, 5, 5, 255).toABGR();
    const uint32_t majorLine = Color(190, 195, 199, 210).toABGR();
    const uint32_t minorLine = Color(63, 66, 68, 190).toABGR();
    const uint32_t fineLine = Color(38, 41, 43, 170).toABGR();

    for (const auto &layout : measureLayouts) {
      if (!contentRectIntersects(layout.x, layout.y, gutterWidth + laneAreaWidth,
                                 layout.height)) {
        continue;
      }
      const float laneX = layout.x + gutterWidth;
      drawRectClip(layout.x, layout.y, gutterWidth, layout.height, gutterColor);
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
    const uint32_t bpmColor = Color(95, 255, 72, 238).toABGR();
    const uint32_t stopColor = Color(255, 246, 36, 220).toABGR();
    const uint32_t scrollColor = Color(82, 215, 255, 230).toABGR();

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

  void renderLabels(RenderContext &context) {
    for (size_t i = 0; i < measureLayouts.size() && i < measureLabels.size();
         ++i) {
      const auto &layout = measureLayouts[i];
      if (!contentRectIntersects(layout.x, layout.y, gutterWidth,
                                 layout.height)) {
        continue;
      }
      auto *label = measureLabels[i].get();
      label->setPositionNoLayout(
          static_cast<int>(std::round(getX() + layout.x - scrollX)),
          static_cast<int>(
              std::round(getY() + layout.y + layout.height - 24.0f - scrollY)),
          YGPositionTypeAbsolute);
      label->render(context);
    }

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
      if (!contentRectIntersects(x, y, 74.0f, 18.0f)) {
        continue;
      }
      marker.text->setPositionNoLayout(
          static_cast<int>(std::round(getX() + x - scrollX)),
          static_cast<int>(std::round(getY() + y - scrollY)),
          YGPositionTypeAbsolute);
      marker.text->render(context);
    }
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

    const float bodyWidth = std::max(4.0f, laneWidth * 0.42f);
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
            : static_cast<float>(column) * columnWidth;
    return columnX + gutterWidth + static_cast<float>(orderIndex) * laneWidth;
  }

  uint32_t noteColor(int lane, const bms_parser::Note *note) const {
    if (dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
      return Color(220, 48, 38, 245).toABGR();
    }
    if (isScratchLane(lane)) {
      return Color(218, 52, 32, 245).toABGR();
    }
    if (dynamic_cast<const bms_parser::LongNote *>(note) != nullptr) {
      return lane % 2 == 0 ? Color(218, 225, 230, 245).toABGR()
                           : Color(50, 132, 230, 245).toABGR();
    }
    return lane % 2 == 0 ? Color(232, 236, 238, 248).toABGR()
                         : Color(42, 128, 230, 248).toABGR();
  }

  uint32_t longNoteColor(int lane) const {
    if (isScratchLane(lane)) {
      return Color(220, 74, 44, 116).toABGR();
    }
    return lane % 2 == 0 ? Color(230, 234, 238, 116).toABGR()
                         : Color(48, 126, 220, 116).toABGR();
  }

  bool isScratchLane(int lane) const {
    const auto scratchLanes = chart->Meta.GetScratchLaneIndices();
    return std::find(scratchLanes.begin(), scratchLanes.end(), lane) !=
           scratchLanes.end();
  }

  bool contentRectIntersects(float x, float y, float width, float height) const {
    const float sx = x - scrollX;
    const float sy = y - scrollY;
    return sx + width >= 0.0f && sx <= static_cast<float>(getWidth()) &&
           sy + height >= 0.0f && sy <= static_cast<float>(getHeight());
  }

  void drawRectClip(float x, float y, float width, float height,
                    uint32_t color) {
    if (width <= 0.0f || height <= 0.0f) {
      return;
    }
    const float screenX = static_cast<float>(getX()) + x - scrollX;
    const float screenY = static_cast<float>(getY()) + y - scrollY;
    const float left = std::max(screenX, static_cast<float>(getX()));
    const float top = std::max(screenY, static_cast<float>(getY()));
    const float right =
        std::min(screenX + width, static_cast<float>(getX() + getWidth()));
    const float bottom =
        std::min(screenY + height, static_cast<float>(getY() + getHeight()));
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
    const float maxX =
        std::max(0.0f, contentWidth - static_cast<float>(getWidth()) + 24.0f);
    const float maxY =
        std::max(0.0f, contentHeight - static_cast<float>(getHeight()));
    scrollX = std::clamp(scrollX, 0.0f, maxX);
    scrollY = std::clamp(scrollY, 0.0f, maxY);
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
  rootLayout->setBackgroundColor(Color(0, 0, 0, 255));

  auto *header = new View();
  header->setHeight(safe.top + 98);
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setPadding(Edge::Top, safe.top + 10);
  header->setPadding(Edge::Left, safe.left + kHeaderPadding);
  header->setPadding(Edge::Right, safe.right + kHeaderPadding);
  header->setPadding(Edge::Bottom, 10);
  header->setGap(12);
  header->setBackgroundColor(Color(62, 88, 123, 255));
  header->setBorderColor(Color(118, 137, 158, 255));
  header->setBorderWidth(1);

  auto *titleColumn = new View();
  titleColumn->setFlexDirection(FlexDirection::Column);
  titleColumn->setAlignItems(YGAlignStretch);
  titleColumn->setFlex(1);
  titleColumn->setMinWidth(0);
  titleColumn->setGap(2);

  titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 29);
  titleText->setColor({246, 248, 252, 255});
  titleText->setHeight(36);
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  subtitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  subtitleText->setColor({218, 226, 236, 255});
  subtitleText->setHeight(23);
  subtitleText->setOverflow(TextView::TextOverflow::Hidden);
  randomSummaryText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  randomSummaryText->setColor({255, 245, 145, 255});
  randomSummaryText->setHeight(21);
  randomSummaryText->setOverflow(TextView::TextOverflow::Hidden);
  titleColumn->addView(titleText);
  titleColumn->addView(subtitleText);
  titleColumn->addView(randomSummaryText);
  header->addView(titleColumn);

  statusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  statusText->setColor({229, 237, 247, 255});
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
  zoomText->setColor({238, 243, 249, 255});
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

  canvasView = new ChartCanvasView();
  canvasView->setFlex(1);

  rootLayout->addView(header);
  rootLayout->addView(canvasView);
  addView(rootLayout);

  updateZoomText();
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
    randomDrawerRoot->setBackgroundColor(Color(0, 0, 0, 136));

    auto *spacer = new View();
    spacer->setFlex(1);
    randomDrawerRoot->addView(spacer);

    auto *panel = new View();
    panel->setWidth(std::min(520, rendering::window_width - 64));
    panel->setFlexDirection(FlexDirection::Column);
    panel->setAlignItems(YGAlignStretch);
    panel->setPadding(Edge::All, 20);
    panel->setGap(14);
    panel->setBackgroundColor(Color(10, 15, 22, 244));
    panel->setBorderColor(Color(88, 112, 142, 255));
    panel->setBorderWidth(2);

    auto *drawerHeader = new View();
    drawerHeader->setFlexDirection(FlexDirection::Row);
    drawerHeader->setAlignItems(YGAlignCenter);
    drawerHeader->setGap(12);
    drawerHeader->setHeight(58);

    auto *drawerTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    drawerTitle->setText("#RANDOM");
    drawerTitle->setColor({247, 249, 252, 255});
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
    randomDrawerScroll->setBorderColor(Color(55, 72, 94, 255));
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
    auto *empty = new TextView("assets/fonts/notosanscjkjp.ttf", 19);
    empty->setText("No active #RANDOM in this interpretation.");
    empty->setColor({181, 197, 217, 255});
    empty->setWrap(true);
    empty->setHeight(84);
    content->addView(empty);
  } else {
    for (const auto &option : randomOptions) {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setAlignItems(YGAlignCenter);
      row->setGap(10);
      row->setHeight(62);
      row->setPadding(Edge::Left, 10 + option.depth * 18);
      row->setPadding(Edge::Right, 10);
      row->setBackgroundColor(Color(19, 29, 43, 220));
      row->setBorderColor(Color(58, 78, 103, 220));
      row->setBorderWidth(1);

      auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
      label->setText("#" + std::to_string(option.index + 1));
      label->setColor({235, 241, 249, 255});
      label->setVAlign(TextView::MIDDLE);
      label->setWidth(58);
      label->setHeight(42);
      row->addView(label);

      auto *range = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
      range->setText("1-" + std::to_string(option.maxValue));
      range->setColor({158, 178, 203, 255});
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
      value->setColor({255, 246, 147, 255});
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
  rebuildRandomDrawer();
}

void ChartViewerScene::setRandomValue(size_t index, int value) {
  if (index >= randomOptions.size()) {
    return;
  }
  const auto &option = randomOptions[index];
  std::vector<int> nextValues = selectedRandomValues;
  if (nextValues.size() <= index) {
    nextValues.resize(index + 1, 1);
  }
  nextValues[index] = std::clamp(value, 1, option.maxValue);
  nextValues.resize(index + 1);
  parseAndRefresh(nextValues);
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
  zoomText->setText(formatDouble(canvasView->getZoom() * 100.0, 0) + "%");
}

void ChartViewerScene::goBack() {
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
  while (std::getline(stream, line)) {
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
                                  parentSkipped || !matched});
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
      options.push_back({index, n, selected, activeRandomDepth()});
      randomStack.push_back(selected);
      randomFrames.push_back({true});
      continue;
    }
    if (matchHeader(line, "#ENDRANDOM")) {
      if (randomFrames.empty()) {
        continue;
      }
      const bool wasActive = randomFrames.back().active;
      randomFrames.pop_back();
      if (wasActive && !randomStack.empty()) {
        randomStack.pop_back();
      }
      continue;
    }
  }

  return options;
}

std::string ChartViewerScene::randomSummary() const {
  if (selectedRandomValues.empty()) {
    return "RANDOM: none";
  }
  return "RANDOM: " + joinRandomValues(selectedRandomValues);
}
