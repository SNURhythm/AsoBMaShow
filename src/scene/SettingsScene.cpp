#include "SettingsScene.h"
#include "../context.h"
#include "../rendering/Color.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "play/BMSRenderer.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct LayoutMetrics {
  SafeAreaInsets safe;
  bool compact = false;
  bool ultraCompact = false;
  bool stackBody = false;
  bool useDualCardRow = true;
  int contentWidth = 0;
  int horizontalPadding = 52;
  int verticalPadding = 60;
  int rootGap = 28;
  int headerGap = 10;
  int bodyGap = 28;
  int secondaryGap = 22;
  int summaryWidth = 400;
  int cardsWidth = 0;
  int secondaryCardWidth = 0;
  int titleSize = 72;
  int subtitleSize = 26;
  int sectionTitleSize = 34;
  int bodyTextSize = 22;
  int summaryValueSize = 24;
  int smallTextSize = 20;
  int cardPadding = 28;
  int cardGap = 22;
  int offsetButtonWidthLarge = 110;
  int offsetButtonWidthSmall = 96;
  int offsetValueWidth = 270;
  int resetButtonWidth = 140;
  int actionButtonWidth = 290;
  int actionButtonHeight = 72;
  int backButtonWidth = 180;
  int backButtonHeight = 64;
  int offsetCardHeight = 190;
  int visibleTimeCardHeight = 250;
  int modeCardHeight = 180;
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
#endif
  return insets;
}

LayoutMetrics resolveLayoutMetrics() {
  LayoutMetrics metrics;
  metrics.safe = getSafeAreaInsetsUi();
  metrics.compact = rendering::window_height < 980;
  metrics.ultraCompact = rendering::window_height < 860;

  if (metrics.ultraCompact) {
    metrics.horizontalPadding = 26;
    metrics.verticalPadding = 18;
    metrics.rootGap = 16;
    metrics.headerGap = 6;
    metrics.bodyGap = 16;
    metrics.secondaryGap = 16;
    metrics.summaryWidth = 320;
    metrics.titleSize = 52;
    metrics.subtitleSize = 18;
    metrics.sectionTitleSize = 27;
    metrics.bodyTextSize = 18;
    metrics.summaryValueSize = 20;
    metrics.smallTextSize = 18;
    metrics.cardPadding = 18;
    metrics.cardGap = 14;
    metrics.offsetButtonWidthLarge = 92;
    metrics.offsetButtonWidthSmall = 82;
    metrics.offsetValueWidth = 208;
    metrics.resetButtonWidth = 112;
    metrics.actionButtonWidth = 240;
    metrics.actionButtonHeight = 58;
    metrics.backButtonWidth = 146;
    metrics.backButtonHeight = 56;
    metrics.offsetCardHeight = 148;
    metrics.visibleTimeCardHeight = 208;
    metrics.modeCardHeight = 148;
  } else if (metrics.compact) {
    metrics.horizontalPadding = 32;
    metrics.verticalPadding = 24;
    metrics.rootGap = 20;
    metrics.headerGap = 8;
    metrics.bodyGap = 20;
    metrics.secondaryGap = 18;
    metrics.summaryWidth = 340;
    metrics.titleSize = 58;
    metrics.subtitleSize = 20;
    metrics.sectionTitleSize = 30;
    metrics.bodyTextSize = 20;
    metrics.summaryValueSize = 22;
    metrics.smallTextSize = 18;
    metrics.cardPadding = 22;
    metrics.cardGap = 16;
    metrics.offsetButtonWidthLarge = 96;
    metrics.offsetButtonWidthSmall = 84;
    metrics.offsetValueWidth = 220;
    metrics.resetButtonWidth = 116;
    metrics.actionButtonWidth = 250;
    metrics.actionButtonHeight = 62;
    metrics.backButtonWidth = 156;
    metrics.backButtonHeight = 58;
    metrics.offsetCardHeight = 156;
    metrics.visibleTimeCardHeight = 224;
    metrics.modeCardHeight = 156;
  }

  const int availableWidth =
      std::max(0, rendering::window_width - metrics.safe.left -
                      metrics.safe.right - metrics.horizontalPadding * 2);
  metrics.contentWidth = availableWidth;
  metrics.stackBody = metrics.compact || availableWidth < 1500;
  if (metrics.stackBody) {
    metrics.summaryWidth = availableWidth;
    metrics.cardsWidth = availableWidth;
  } else {
    metrics.cardsWidth =
        std::max(0, availableWidth - metrics.summaryWidth - metrics.bodyGap);
  }
  metrics.useDualCardRow =
      !metrics.compact && !metrics.stackBody && metrics.cardsWidth >= 980;
  metrics.secondaryCardWidth =
      metrics.useDualCardRow
          ? std::max(0, (metrics.cardsWidth - metrics.secondaryGap) / 2)
          : metrics.cardsWidth;

  return metrics;
}

TextView *makeText(const std::string &text, int size, const Color &color,
                   TextView::TextAlign align = TextView::LEFT,
                   TextView::TextVAlign valign = TextView::TOP) {
  auto *view = new TextView(kFontPath, size);
  view->setText(text);
  view->setColor({color.r, color.g, color.b, color.a});
  view->setAlign(align);
  view->setVAlign(valign);
  return view;
}

TextView *makeWrappedText(const std::string &text, int size, const Color &color,
                          TextView::TextAlign align = TextView::LEFT,
                          TextView::TextVAlign valign = TextView::TOP) {
  auto *view = makeText(text, size, color, align, valign);
  view->setWrap(true);
  return view;
}

Button *makeButton(int width, int height, TextView *label,
                   const Color &normalBackground, const Color &hoverBackground,
                   const Color &pressedBackground, const Color &normalBorder,
                   const Color &hoverBorder, const Color &pressedBorder,
                   int borderWidth = 2) {
  auto *button = new Button(0, 0, width, height);
  label->setAlign(TextView::CENTER);
  label->setVAlign(TextView::MIDDLE);
  button->setContentView(label);
  button->setBackgroundColors(normalBackground, hoverBackground,
                              pressedBackground);
  button->setBorderColors(normalBorder, hoverBorder, pressedBorder);
  button->setStyledBorderWidth(borderWidth);
  return button;
}

Button *makeStepButton(const LayoutMetrics &metrics, int width,
                       const std::string &label) {
  return makeButton(width, metrics.actionButtonHeight,
                    makeText(label, metrics.bodyTextSize + 4,
                             Color(239, 244, 251), TextView::CENTER,
                             TextView::MIDDLE),
                    Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                    Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                    Color(108, 136, 174, 255), Color(139, 172, 217, 255));
}

Button *makeResetButton(const LayoutMetrics &metrics) {
  return makeButton(metrics.resetButtonWidth, metrics.actionButtonHeight,
                    makeText("Reset", metrics.bodyTextSize + 4,
                             Color(248, 241, 236), TextView::CENTER,
                             TextView::MIDDLE),
                    Color(96, 57, 44, 255), Color(117, 72, 55, 255),
                    Color(153, 96, 74, 255), Color(165, 105, 79, 255),
                    Color(193, 124, 93, 255), Color(219, 145, 108, 255));
}

TextInputBox *makeNumericInput(const LayoutMetrics &metrics) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  input->setText("");
  input->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  input->setBackgroundColor(Color(0, 0, 0, 0));
  input->setBorderWidth(0);
  input->setAlign(TextView::CENTER);
  input->setVAlign(TextView::MIDDLE);
  input->setColor({244, 248, 255, 255});
  return input;
}

View *makeInputFrame(const LayoutMetrics &metrics, TextInputBox *input) {
  auto *value = new View();
  value->setWidth(static_cast<float>(metrics.offsetValueWidth));
  value->setHeight(static_cast<float>(metrics.actionButtonHeight));
  value->setBackgroundColor(Color(10, 17, 28, 255));
  value->setBorderColor(Color(78, 105, 140, 255));
  value->setBorderWidth(2);
  value->addView(input);
  return value;
}

View *makeCard(const LayoutMetrics &metrics, const std::string &title,
               const std::string &description, View *body, int minHeight,
               int width = 0) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column);
  card->setGap(metrics.cardGap);
  card->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  card->setBackgroundColor(Color(19, 30, 46, 245));
  card->setBorderColor(Color(76, 104, 136, 255));
  card->setBorderWidth(2);
  card->setMinHeight(static_cast<float>(minHeight));
  if (width > 0) {
    card->setWidth(static_cast<float>(width));
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setGap(metrics.compact ? 6.0f : 8.0f);
  auto *titleText =
      makeWrappedText(title, metrics.sectionTitleSize, Color(244, 248, 255));
  header->addView(titleText);
  auto *descriptionText =
      makeWrappedText(description, metrics.bodyTextSize, Color(168, 186, 209));
  header->addView(descriptionText);
  card->addView(header);
  card->addView(body);
  return card;
}

View *makeSummaryRow(const LayoutMetrics &metrics, const std::string &label,
                     TextView **valueOut) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifySpaceBetween);
  row->setAlignItems(YGAlignCenter);

  row->addView(makeText(label, metrics.summaryValueSize, Color(164, 186, 206)));
  auto *valueText = makeText("", metrics.summaryValueSize, Color(244, 248, 255),
                             TextView::RIGHT);
  row->addView(valueText);
  if (valueOut != nullptr) {
    *valueOut = valueText;
  }
  return row;
}

int clampOffset(int value) {
  return std::clamp(value, AppSettings::kMinInputOffsetMs,
                    AppSettings::kMaxInputOffsetMs);
}

int clampVisualOffset(int value) {
  return std::clamp(value, AppSettings::kMinVisualOffsetMs,
                    AppSettings::kMaxVisualOffsetMs);
}

int clampVisibleTimeGreenNumber(int value) {
  return std::clamp(value, AppSettings::kMinVisibleTimeGreenNumber,
                    AppSettings::kMaxVisibleTimeGreenNumber);
}

int clampBgaBrightness(int value) {
  return std::clamp(value, AppSettings::kMinBgaBrightnessPercent,
                    AppSettings::kMaxBgaBrightnessPercent);
}

float clampBgaBlur(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultBgaBlurStrength;
  }
  return std::clamp(value, AppSettings::kMinBgaBlurStrength,
                    AppSettings::kMaxBgaBlurStrength);
}

float clampLaneAngle(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneAngleDegrees;
  }
  return std::clamp(value, AppSettings::kMinLaneAngleDegrees,
                    AppSettings::kMaxLaneAngleDegrees);
}

float clampLaneLength(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneLength;
  }
  return std::clamp(value, AppSettings::kMinLaneLength,
                    AppSettings::kMaxLaneLength);
}

int greenNumberToMilliseconds(int greenNumber) {
  return static_cast<int>(
      std::lround(static_cast<double>(greenNumber) * 1000.0 / 600.0));
}

int millisecondsToGreenNumber(int milliseconds) {
  return static_cast<int>(
      std::lround(static_cast<double>(milliseconds) * 600.0 / 1000.0));
}

int adjustVisibleTimeGreenNumber(int currentGreenNumber, bool useMilliseconds,
                                 int delta) {
  if (!useMilliseconds) {
    return clampVisibleTimeGreenNumber(currentGreenNumber + delta);
  }

  const int currentMilliseconds = greenNumberToMilliseconds(currentGreenNumber);
  const int nextMilliseconds =
      std::clamp(currentMilliseconds + delta, AppSettings::kMinVisibleTimeMs,
                 AppSettings::kMaxVisibleTimeMs);
  return clampVisibleTimeGreenNumber(
      millisecondsToGreenNumber(nextMilliseconds));
}

std::string formatOffsetLabel(int offsetMs) {
  return (offsetMs > 0 ? "+" : "") + std::to_string(offsetMs) + " ms";
}

std::string formatOffsetInputValue(int offsetMs) {
  return std::to_string(offsetMs);
}

std::string formatVisibleTimeLabel(int greenNumber, bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber)) + " ms";
  }
  return std::to_string(greenNumber) + " green";
}

std::string formatVisibleTimeInputValue(int greenNumber, bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber));
  }
  return std::to_string(greenNumber);
}

std::string formatFloatValue(float value, int precision = 1) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

std::string formatBgaBrightnessLabel(int percent) {
  return std::to_string(percent) + "%";
}

std::string formatBgaBlurLabel(float strength) {
  return formatFloatValue(strength, 1);
}

std::string formatLaneAngleLabel(float degrees) {
  return formatFloatValue(degrees, 1) + " deg";
}

std::string formatLaneLengthLabel(float length) {
  return formatFloatValue(length, 1);
}

std::string formatBgaDisplayModeLabel(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return "Fit";
  case AppSettings::BgaDisplayMode::Fill:
    return "Fill";
  case AppSettings::BgaDisplayMode::Stretch:
    return "Stretch";
  }
  return "Fit";
}

AppSettings::BgaDisplayMode
nextBgaDisplayMode(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return AppSettings::BgaDisplayMode::Fill;
  case AppSettings::BgaDisplayMode::Fill:
    return AppSettings::BgaDisplayMode::Stretch;
  case AppSettings::BgaDisplayMode::Stretch:
    return AppSettings::BgaDisplayMode::Fit;
  }
  return AppSettings::BgaDisplayMode::Fit;
}

constexpr long long kPreviewLoopMicros = 8000000LL;
constexpr int kPreviewTimelineLanes = 16;
constexpr double kPreviewBpm = 120.0;
constexpr long long kPreviewLatePoorMicros = 200000LL;

bms_parser::TimeLine *makePreviewTimeline(long long timingMicros,
                                          bool firstInMeasure = false) {
  auto *timeline = new bms_parser::TimeLine(kPreviewTimelineLanes, false);
  timeline->Timing = timingMicros;
  timeline->BeatPosition = static_cast<double>(timingMicros) / 2000000.0;
  timeline->Bpm = kPreviewBpm;
  timeline->Scroll = 1.0;
  timeline->IsFirstInMeasure = firstInMeasure;
  return timeline;
}

void addPreviewNote(bms_parser::TimeLine *timeline, int lane) {
  timeline->SetNote(lane, new bms_parser::Note(bms_parser::Parser::NoWav));
}

void addPreviewLongNote(bms_parser::TimeLine *headTimeline,
                        bms_parser::TimeLine *tailTimeline, int lane) {
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
}

bms_parser::Chart *makePreviewChart() {
  auto *chart = new bms_parser::Chart();
  chart->Meta.Title = "Settings Preview";
  chart->Meta.Bpm = kPreviewBpm;
  chart->Meta.MinBpm = kPreviewBpm;
  chart->Meta.MaxBpm = kPreviewBpm;
  chart->Meta.KeyMode = 7;
  chart->Meta.IsDP = false;
  chart->Meta.Rank = 3;
  chart->Meta.PlayLength = kPreviewLoopMicros;
  chart->Meta.TotalLength = kPreviewLoopMicros;

  auto *measure = new bms_parser::Measure();
  measure->Timing = 0;
  measure->Scale = 4.0;
  measure->Pos = 0.0;

  auto appendTimeline = [measure](long long timingMicros,
                                  bool firstInMeasure = false) {
    auto *timeline = makePreviewTimeline(timingMicros, firstInMeasure);
    measure->TimeLines.push_back(timeline);
    return timeline;
  };

  addPreviewNote(appendTimeline(500000, true), 0);
  addPreviewNote(appendTimeline(850000), 2);
  addPreviewNote(appendTimeline(1200000), 4);
  addPreviewNote(appendTimeline(1550000), 6);
  addPreviewNote(appendTimeline(1900000), 7);

  auto *longHead = appendTimeline(2400000);
  auto *longTail = appendTimeline(3900000);
  addPreviewLongNote(longHead, longTail, 3);

  addPreviewNote(appendTimeline(4300000), 1);
  addPreviewNote(appendTimeline(4700000), 5);
  addPreviewNote(appendTimeline(5200000), 0);
  addPreviewNote(appendTimeline(5650000), 7);
  addPreviewNote(appendTimeline(6100000), 2);
  addPreviewNote(appendTimeline(6550000), 4);
  addPreviewNote(appendTimeline(7000000), 6);

  chart->Measures.push_back(measure);
  return chart;
}
} // namespace

void SettingsScene::init() { ensureLayoutUpToDate(); }

void SettingsScene::startLanePreview() {
  activeTab = SettingsTab::Lane;
  previewActive = true;
  resetPreviewSimulation();
  ensurePreviewRenderer();
  lastLayoutWidth = -1;
}

void SettingsScene::stopLanePreview() {
  previewActive = false;
  destroyPreviewRenderer();
  lastLayoutWidth = -1;
}

void SettingsScene::ensurePreviewRenderer() {
  if (previewChart == nullptr) {
    previewChart = makePreviewChart();
  }
  if (previewRenderer == nullptr && previewChart != nullptr) {
    previewRenderer =
        new BMSRenderer(previewChart, kPreviewLatePoorMicros,
                        context.settings.visibleTimeGreenNumber, false);
  }
}

void SettingsScene::destroyPreviewRenderer() {
  delete previewRenderer;
  previewRenderer = nullptr;
  delete previewChart;
  previewChart = nullptr;
  previewElapsedMicros = 0;
}

void SettingsScene::resetPreviewSimulation() {
  previewElapsedMicros = 0;
  if (previewRenderer != nullptr) {
    previewRenderer->reset();
  }
  if (previewChart == nullptr) {
    return;
  }
  for (const auto *measure : previewChart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
    }
  }
}

void SettingsScene::resetViewState() {
  for (auto *view : views) {
    delete view;
  }
  views.clear();
  rootLayout = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
}

void SettingsScene::ensureLayoutUpToDate() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  if (rendering::window_width == lastLayoutWidth &&
      rendering::window_height == lastLayoutHeight && safe.top == lastSafeTop &&
      safe.left == lastSafeLeft && safe.bottom == lastSafeBottom &&
      safe.right == lastSafeRight && rootLayout != nullptr) {
    return;
  }

  resetViewState();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  initView();
}

void SettingsScene::initView() {
  LayoutMetrics metrics = resolveLayoutMetrics();
  View::LayoutBatchScope layoutBatch;

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setPadding(
      Edge::Top,
      static_cast<float>(metrics.safe.top + metrics.verticalPadding));
  rootLayout->setPadding(
      Edge::Left,
      static_cast<float>(metrics.safe.left + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Right,
      static_cast<float>(metrics.safe.right + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Bottom,
      static_cast<float>(metrics.safe.bottom + metrics.verticalPadding));
  rootLayout->setGap(static_cast<float>(metrics.rootGap));

  auto makeVisibleTimeControls = [this,
                                  &metrics](bool includeDescription,
                                            bool compactAdjustments) -> View * {
    auto *visibleTimeControls = new View();
    visibleTimeControls->setFlexDirection(FlexDirection::Column);
    visibleTimeControls->setGap(metrics.compact ? 12.0f : 16.0f);
    visibleTimeControls->setAlignItems(YGAlignFlexStart);
    if (includeDescription) {
      visibleTimeControls->addView(makeWrappedText(
          metrics.compact
              ? "600 green = 1000 ms. This controls how long notes stay "
                "visible."
              : "Green Number is the legacy BMS unit for note visible time. "
                "600 green equals 60 frames on a 60 FPS system, which is "
                "1000 ms.",
          metrics.bodyTextSize, Color(150, 171, 193)));
    }

    visibleTimeModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    visibleTimeModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        visibleTimeModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    visibleTimeModeButton->setOnClickListener([this]() {
      context.settings.visibleTimeUseMilliseconds =
          !context.settings.visibleTimeUseMilliseconds;
      persistSettings();
      syncVisibleTimeInputText(true);
    });
    visibleTimeControls->addView(visibleTimeModeButton);

    auto *visibleTimeValueControls = new View();
    visibleTimeValueControls->setFlexDirection(FlexDirection::Row);
    visibleTimeValueControls->setFlexWrap(YGWrapWrap);
    visibleTimeValueControls->setGap(metrics.compact ? 8.0f : 12.0f);
    visibleTimeValueControls->setAlignItems(YGAlignFlexStart);

    auto updateVisibleTime = [this](int delta) {
      context.settings.visibleTimeGreenNumber = adjustVisibleTimeGreenNumber(
          context.settings.visibleTimeGreenNumber,
          context.settings.visibleTimeUseMilliseconds, delta);
      persistSettings();
      syncVisibleTimeInputText(true);
    };

    if (!compactAdjustments) {
      auto *minusVisibleTimeLarge =
          makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-100");
      minusVisibleTimeLarge->setOnClickListener(
          [updateVisibleTime]() { updateVisibleTime(-100); });
      visibleTimeValueControls->addView(minusVisibleTimeLarge);
    }

    auto *minusVisibleTimeSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10");
    minusVisibleTimeSmall->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-10); });
    visibleTimeValueControls->addView(minusVisibleTimeSmall);

    if (!compactAdjustments) {
      auto *minusVisibleTimeOne =
          makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
      minusVisibleTimeOne->setOnClickListener(
          [updateVisibleTime]() { updateVisibleTime(-1); });
      visibleTimeValueControls->addView(minusVisibleTimeOne);
    }

    visibleTimeInput = makeNumericInput(metrics);
    visibleTimeInput->onEditingFinished(
        [this](const std::string &) { commitVisibleTimeInput(); });
    visibleTimeValueControls->addView(
        makeInputFrame(metrics, visibleTimeInput));

    if (!compactAdjustments) {
      auto *plusVisibleTimeOne =
          makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
      plusVisibleTimeOne->setOnClickListener(
          [updateVisibleTime]() { updateVisibleTime(1); });
      visibleTimeValueControls->addView(plusVisibleTimeOne);
    }

    auto *plusVisibleTimeSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10");
    plusVisibleTimeSmall->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(10); });
    visibleTimeValueControls->addView(plusVisibleTimeSmall);

    if (!compactAdjustments) {
      auto *plusVisibleTimeLarge =
          makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+100");
      plusVisibleTimeLarge->setOnClickListener(
          [updateVisibleTime]() { updateVisibleTime(100); });
      visibleTimeValueControls->addView(plusVisibleTimeLarge);

      auto *resetVisibleTime = makeResetButton(metrics);
      resetVisibleTime->setOnClickListener([this]() {
        context.settings.visibleTimeGreenNumber = 400;
        persistSettings();
        syncVisibleTimeInputText(true);
      });
      visibleTimeValueControls->addView(resetVisibleTime);
    }

    visibleTimeControls->addView(visibleTimeValueControls);
    return visibleTimeControls;
  };

  if (previewActive) {
    rootLayout->setFlexDirection(FlexDirection::Row);
    rootLayout->setJustifyContent(YGJustifyFlexEnd);
    rootLayout->setAlignItems(YGAlignFlexStart);

    const int foldButtonSize = metrics.compact ? 54 : 58;
    const int panelWidth =
        previewPanelFolded
            ? foldButtonSize
            : (metrics.compact ? std::min(metrics.contentWidth, 520) : 380);
    auto *previewPanel = new View();
    previewPanel->setWidth(static_cast<float>(panelWidth));
    previewPanel->setPadding(
        Edge::All,
        static_cast<float>(previewPanelFolded ? 0 : metrics.cardPadding));
    previewPanel->setGap(metrics.compact ? 12.0f : 16.0f);
    previewPanel->setFlexDirection(FlexDirection::Column);
    previewPanel->setAlignItems(previewPanelFolded ? YGAlignFlexEnd
                                                   : YGAlignStretch);
    previewPanel->setBackgroundColor(Color(12, 20, 32, 184));
    previewPanel->setBorderColor(Color(78, 105, 140, 220));
    previewPanel->setBorderWidth(2);

    auto makeFoldButton = [this, foldButtonSize](const std::string &label) {
      auto *button =
          makeButton(foldButtonSize, foldButtonSize,
                     makeText(label, 28, Color(239, 244, 251), TextView::CENTER,
                              TextView::MIDDLE),
                     Color(22, 33, 49, 190), Color(31, 46, 67, 220),
                     Color(53, 78, 110, 240), Color(96, 121, 156, 230),
                     Color(120, 151, 190, 245), Color(148, 186, 231, 255));
      button->setOnClickListener([this]() {
        previewPanelFolded = !previewPanelFolded;
        lastLayoutWidth = -1;
      });
      return button;
    };

    if (previewPanelFolded) {
      previewPanel->addView(makeFoldButton("<"));
      rootLayout->addView(previewPanel);
      addView(rootLayout);
      rootLayout->applyYogaLayout();
      refreshSettingsText();
      return;
    }

    auto *previewHeader = new View();
    previewHeader->setFlexDirection(FlexDirection::Row);
    previewHeader->setAlignItems(YGAlignCenter);
    previewHeader->setJustifyContent(YGJustifySpaceBetween);
    previewHeader->addView(
        makeText("Preview", metrics.sectionTitleSize, Color(244, 248, 255)));
    previewHeader->addView(makeFoldButton(">"));
    previewPanel->addView(previewHeader);
    previewPanel->addView(
        makeSummaryRow(metrics, "Visible Time", &summaryVisibleTimeValueText));
    previewPanel->addView(makeVisibleTimeControls(false, true));
    previewPanel->addView(
        makeSummaryRow(metrics, "Lane Angle", &summaryLaneAngleValueText));
    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateLaneAngle = [this](float delta) {
      context.settings.laneAngleDegrees =
          clampLaneAngle(context.settings.laneAngleDegrees + delta);
      persistSettings();
    };
    auto *minusAngle =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusAngle->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-1.0f); });
    angleControls->addView(minusAngle);
    auto *plusAngle =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusAngle->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(1.0f); });
    angleControls->addView(plusAngle);
    auto *resetAngle = makeResetButton(metrics);
    resetAngle->setOnClickListener([this]() {
      context.settings.laneAngleDegrees = AppSettings::kDefaultLaneAngleDegrees;
      persistSettings();
    });
    angleControls->addView(resetAngle);
    previewPanel->addView(angleControls);

    previewPanel->addView(
        makeSummaryRow(metrics, "Lane Length", &summaryLaneLengthValueText));
    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateLaneLength = [this](float delta) {
      context.settings.laneLength =
          clampLaneLength(context.settings.laneLength + delta);
      persistSettings();
    };
    auto *minusLength =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusLength->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-0.5f); });
    lengthControls->addView(minusLength);
    auto *plusLength =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusLength->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(0.5f); });
    lengthControls->addView(plusLength);
    auto *resetLength = makeResetButton(metrics);
    resetLength->setOnClickListener([this]() {
      context.settings.laneLength = AppSettings::kDefaultLaneLength;
      persistSettings();
    });
    lengthControls->addView(resetLength);
    previewPanel->addView(lengthControls);

    auto *restartButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText("Restart", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    restartButton->setOnClickListener([this]() { resetPreviewSimulation(); });
    previewPanel->addView(restartButton);

    auto *doneButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText("Done", metrics.bodyTextSize + 4, Color(237, 243, 252),
                 TextView::CENTER, TextView::MIDDLE),
        Color(22, 33, 49, 255), Color(31, 46, 67, 255), Color(53, 78, 110, 255),
        Color(96, 121, 156, 255), Color(120, 151, 190, 255),
        Color(148, 186, 231, 255));
    doneButton->setOnClickListener([this]() { stopLanePreview(); });
    previewPanel->addView(doneButton);

    rootLayout->addView(previewPanel);
    addView(rootLayout);
    rootLayout->applyYogaLayout();
    refreshSettingsText();
    return;
  }

  rootLayout->setBackgroundColor(Color(10, 18, 30));

  if (!metrics.compact) {
    auto *accentA = new View(110, 86, 480, 180);
    accentA->setPositionType(YGPositionTypeAbsolute);
    accentA->setBackgroundColor(Color(39, 101, 160, 96));
    rootLayout->addView(accentA);

    auto *accentB = new View(rendering::window_width - 520,
                             rendering::window_height - 250, 420, 160);
    accentB->setPositionType(YGPositionTypeAbsolute);
    accentB->setBackgroundColor(Color(207, 110, 62, 72));
    rootLayout->addView(accentB);
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(static_cast<float>(metrics.headerGap));
  headerText->addView(
      makeText("Settings", metrics.titleSize, Color(244, 248, 255)));
  headerText->addView(makeWrappedText(
      metrics.compact
          ? "Timing, keysound, and visual preferences."
          : "Persistent player preferences for timing, keysounds, and visual "
            "load.",
      metrics.subtitleSize, Color(162, 183, 205)));
  header->addView(headerText);

  auto *backLabel =
      makeText("Back", metrics.bodyTextSize + 6, Color(237, 243, 252),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton =
      makeButton(metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
                 Color(22, 33, 49, 255), Color(31, 46, 67, 255),
                 Color(53, 78, 110, 255), Color(96, 121, 156, 255),
                 Color(120, 151, 190, 255), Color(148, 186, 231, 255));
  backButton->setOnClickListener(
      [this]() { context.sceneManager->changeScene("MainMenu"); });
  header->addView(backButton);
  rootLayout->addView(header);

  const int tabColumnWidth = std::min(
      metrics.contentWidth,
      metrics.compact ? std::clamp(metrics.contentWidth / 4, 150, 190)
                      : std::clamp(metrics.contentWidth / 6, 220, 280));
  metrics.cardsWidth =
      std::max(0, metrics.contentWidth - tabColumnWidth - metrics.bodyGap);
  metrics.useDualCardRow = !metrics.compact && metrics.cardsWidth >= 980;
  metrics.secondaryCardWidth =
      metrics.useDualCardRow
          ? std::max(0, (metrics.cardsWidth - metrics.secondaryGap) / 2)
          : metrics.cardsWidth;

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row);
  content->setGap(static_cast<float>(metrics.bodyGap));
  content->setFlex(1.0f);
  content->setAlignItems(YGAlignStretch);

  auto *tabControls = new View();
  tabControls->setFlexDirection(FlexDirection::Column);
  tabControls->setGap(metrics.compact ? 8.0f : 12.0f);
  tabControls->setWidth(static_cast<float>(tabColumnWidth));
  tabControls->setFlexShrink(0.0f);
  auto makeTabButton = [&](SettingsTab tab, const std::string &label) {
    auto *button = makeButton(
        tabColumnWidth, metrics.actionButtonHeight,
        makeText(label, metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    button->setOnClickListener([this, tab]() {
      if (activeTab == tab) {
        return;
      }
      activeTab = tab;
      lastLayoutWidth = -1;
    });
    return button;
  };
  timingTabButton = makeTabButton(SettingsTab::Timing, "Timing");
  visualTabButton = makeTabButton(SettingsTab::Visual, "Visual");
  laneTabButton = makeTabButton(SettingsTab::Lane, "Lane");
  tabControls->addView(timingTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(laneTabButton);
  content->addView(tabControls);

  scrollView = new ScrollView();
  scrollView->setFlex(1.0f);

  auto *scrollContent = new View();
  scrollContent->setFlexDirection(FlexDirection::Column);
  scrollContent->setGap(static_cast<float>(metrics.rootGap));

  auto *cardsColumn = new View();
  cardsColumn->setFlexDirection(FlexDirection::Column);
  cardsColumn->setGap(static_cast<float>(metrics.secondaryGap));
  cardsColumn->setWidth(static_cast<float>(metrics.cardsWidth));

  if (activeTab == SettingsTab::Timing) {
    auto *offsetControls = new View();
    offsetControls->setFlexDirection(FlexDirection::Row);
    offsetControls->setFlexWrap(YGWrapWrap);
    offsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
    offsetControls->setAlignItems(YGAlignFlexStart);

    auto updateOffset = [this](int delta) {
      context.settings.inputOffsetMs =
          clampOffset(context.settings.inputOffsetMs + delta);
      persistSettings();
      syncOffsetInputText(true);
    };

    auto *minusTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusTen->setOnClickListener([updateOffset]() { updateOffset(-10); });
    offsetControls->addView(minusTen);

    auto *minusOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusOne->setOnClickListener([updateOffset]() { updateOffset(-1); });
    offsetControls->addView(minusOne);

    auto *offsetValue = new View();
    offsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
    offsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
    offsetValue->setBackgroundColor(Color(10, 17, 28, 255));
    offsetValue->setBorderColor(Color(78, 105, 140, 255));
    offsetValue->setBorderWidth(2);
    offsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
    offsetInput->setText("");
    offsetInput->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
    offsetInput->setBackgroundColor(Color(0, 0, 0, 0));
    offsetInput->setBorderWidth(0);
    offsetInput->setAlign(TextView::CENTER);
    offsetInput->setVAlign(TextView::MIDDLE);
    offsetInput->setColor({244, 248, 255, 255});
    offsetInput->onEditingFinished(
        [this](const std::string &) { commitOffsetInput(); });
    offsetValue->addView(offsetInput);
    offsetControls->addView(offsetValue);

    auto *plusOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusOne->setOnClickListener([updateOffset]() { updateOffset(1); });
    offsetControls->addView(plusOne);

    auto *plusTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusTen->setOnClickListener([updateOffset]() { updateOffset(10); });
    offsetControls->addView(plusTen);

    auto *resetOffset = makeButton(
        metrics.resetButtonWidth, metrics.actionButtonHeight,
        makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
                 TextView::CENTER, TextView::MIDDLE),
        Color(96, 57, 44, 255), Color(117, 72, 55, 255),
        Color(153, 96, 74, 255), Color(165, 105, 79, 255),
        Color(193, 124, 93, 255), Color(219, 145, 108, 255));
    resetOffset->setOnClickListener([this]() {
      context.settings.inputOffsetMs = 0;
      persistSettings();
      syncOffsetInputText(true);
    });
    offsetControls->addView(resetOffset);

    cardsColumn->addView(
        makeCard(metrics, "Judgement Offset",
                 metrics.compact
                     ? "Positive values judge later when your hits feel early."
                     : "Positive values judge later. Use this when your hits "
                       "consistently "
                       "feel early relative to the music.",
                 offsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *visualOffsetControls = new View();
    visualOffsetControls->setFlexDirection(FlexDirection::Row);
    visualOffsetControls->setFlexWrap(YGWrapWrap);
    visualOffsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
    visualOffsetControls->setAlignItems(YGAlignFlexStart);

    auto updateVisualOffset = [this](int delta) {
      context.settings.visualOffsetMs =
          clampVisualOffset(context.settings.visualOffsetMs + delta);
      persistSettings();
      syncVisualOffsetInputText(true);
    };

    auto *minusVisualTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisualTen->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(-10); });
    visualOffsetControls->addView(minusVisualTen);

    auto *minusVisualOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisualOne->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(-1); });
    visualOffsetControls->addView(minusVisualOne);

    auto *visualOffsetValue = new View();
    visualOffsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
    visualOffsetValue->setHeight(
        static_cast<float>(metrics.actionButtonHeight));
    visualOffsetValue->setBackgroundColor(Color(10, 17, 28, 255));
    visualOffsetValue->setBorderColor(Color(78, 105, 140, 255));
    visualOffsetValue->setBorderWidth(2);
    visualOffsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
    visualOffsetInput->setText("");
    visualOffsetInput->setSize(metrics.offsetValueWidth,
                               metrics.actionButtonHeight);
    visualOffsetInput->setBackgroundColor(Color(0, 0, 0, 0));
    visualOffsetInput->setBorderWidth(0);
    visualOffsetInput->setAlign(TextView::CENTER);
    visualOffsetInput->setVAlign(TextView::MIDDLE);
    visualOffsetInput->setColor({244, 248, 255, 255});
    visualOffsetInput->onEditingFinished(
        [this](const std::string &) { commitVisualOffsetInput(); });
    visualOffsetValue->addView(visualOffsetInput);
    visualOffsetControls->addView(visualOffsetValue);

    auto *plusVisualOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisualOne->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(1); });
    visualOffsetControls->addView(plusVisualOne);

    auto *plusVisualTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisualTen->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(10); });
    visualOffsetControls->addView(plusVisualTen);

    auto *resetVisualOffset = makeButton(
        metrics.resetButtonWidth, metrics.actionButtonHeight,
        makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
                 TextView::CENTER, TextView::MIDDLE),
        Color(96, 57, 44, 255), Color(117, 72, 55, 255),
        Color(153, 96, 74, 255), Color(165, 105, 79, 255),
        Color(193, 124, 93, 255), Color(219, 145, 108, 255));
    resetVisualOffset->setOnClickListener([this]() {
      context.settings.visualOffsetMs = 0;
      persistSettings();
      syncVisualOffsetInputText(true);
    });
    visualOffsetControls->addView(resetVisualOffset);

    cardsColumn->addView(makeCard(
        metrics, "Visual Offset",
        metrics.compact
            ? "Positive values delay notes and BGA to match late audio output."
            : "Positive values delay note rendering and BGA playback. Use this "
              "for late audio paths such as Bluetooth headphones.",
        visualOffsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *secondaryCards = new View();
    secondaryCards->setFlexDirection(
        metrics.useDualCardRow ? FlexDirection::Row : FlexDirection::Column);
    secondaryCards->setGap(static_cast<float>(metrics.secondaryGap));

    auto *keysoundControls = new View();
    keysoundControls->setFlexDirection(FlexDirection::Column);
    keysoundControls->setGap(metrics.compact ? 12.0f : 16.0f);
    keysoundControls->setAlignItems(YGAlignFlexStart);
    keysoundControls->addView(makeWrappedText(
        metrics.compact
            ? "Switch between manual hits and chart-timed playback."
            : "Tap to switch modes. The current selection is shown on "
              "the right.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    keysoundModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    keysoundModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight, keysoundModeText,
        Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    keysoundModeButton->setOnClickListener([this]() {
      context.settings.inputKeysoundEnabled =
          !context.settings.inputKeysoundEnabled;
      persistSettings();
    });
    keysoundControls->addView(keysoundModeButton);
    secondaryCards->addView(makeCard(
        metrics, "Input Keysounds",
        metrics.compact
            ? "Manual hits keep classic feedback. Auto timed follows chart "
              "timing."
            : "Keep manual key clicks for classic BMS feedback, or switch to "
              "auto-timed playback for cleaner timing practice.",
        keysoundControls, metrics.modeCardHeight, metrics.secondaryCardWidth));

    cardsColumn->addView(secondaryCards);
  }

  if (activeTab == SettingsTab::Visual) {
    auto *bgaControls = new View();
    bgaControls->setFlexDirection(FlexDirection::Column);
    bgaControls->setGap(metrics.compact ? 12.0f : 16.0f);
    bgaControls->setAlignItems(YGAlignFlexStart);
    bgaControls->addView(makeWrappedText(
        metrics.compact ? "Toggle BGA rendering for previews and gameplay."
                        : "Tap to switch BGA rendering on or off for future "
                          "previews and charts.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    bgaModeText = makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                           TextView::CENTER, TextView::MIDDLE);
    bgaModeButton =
        makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                   bgaModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
                   Color(59, 98, 147, 255), Color(92, 131, 177, 255),
                   Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    bgaModeButton->setOnClickListener([this]() {
      context.settings.bgaEnabled = !context.settings.bgaEnabled;
      persistSettings();
    });
    bgaControls->addView(bgaModeButton);
    cardsColumn->addView(makeCard(
        metrics, "BGA Playback",
        metrics.compact
            ? "Disable background animation for lower distraction or lighter "
              "rendering."
            : "Disable background animation if you want lower distraction or a "
              "lighter render path on slower hardware.",
        bgaControls, metrics.modeCardHeight, metrics.cardsWidth));

    auto *bgaDisplayControls = new View();
    bgaDisplayControls->setFlexDirection(FlexDirection::Column);
    bgaDisplayControls->setGap(metrics.compact ? 12.0f : 16.0f);
    bgaDisplayControls->setAlignItems(YGAlignFlexStart);
    bgaDisplayControls->addView(makeWrappedText(
        metrics.compact
            ? "Fit preserves the full image. Fill crops. Stretch ignores "
              "aspect."
            : "Fit preserves the whole BGA with letterboxing. Fill preserves "
              "aspect and crops edges. Stretch fills the screen without "
              "preserving aspect.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    bgaDisplayModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    bgaDisplayModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        bgaDisplayModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    bgaDisplayModeButton->setOnClickListener([this]() {
      context.settings.bgaDisplayMode =
          nextBgaDisplayMode(context.settings.bgaDisplayMode);
      persistSettings();
    });
    bgaDisplayControls->addView(bgaDisplayModeButton);
    cardsColumn->addView(makeCard(
        metrics, "BGA Aspect",
        metrics.compact ? "Choose how BGA fits the playfield."
                        : "Choose how BGA media is fitted to the playfield.",
        bgaDisplayControls, metrics.modeCardHeight, metrics.cardsWidth));

    auto *brightnessControls = new View();
    brightnessControls->setFlexDirection(FlexDirection::Row);
    brightnessControls->setFlexWrap(YGWrapWrap);
    brightnessControls->setGap(metrics.compact ? 8.0f : 12.0f);
    brightnessControls->setAlignItems(YGAlignFlexStart);
    auto updateBgaBrightness = [this](int delta) {
      context.settings.bgaBrightnessPercent =
          clampBgaBrightness(context.settings.bgaBrightnessPercent + delta);
      persistSettings();
      syncBgaBrightnessInputText(true);
    };
    auto *minusBrightnessTen =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
    minusBrightnessTen->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(-10); });
    brightnessControls->addView(minusBrightnessTen);
    auto *minusBrightnessOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusBrightnessOne->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(-1); });
    brightnessControls->addView(minusBrightnessOne);
    bgaBrightnessInput = makeNumericInput(metrics);
    bgaBrightnessInput->onEditingFinished(
        [this](const std::string &) { commitBgaBrightnessInput(); });
    brightnessControls->addView(makeInputFrame(metrics, bgaBrightnessInput));
    auto *plusBrightnessOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusBrightnessOne->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(1); });
    brightnessControls->addView(plusBrightnessOne);
    auto *plusBrightnessTen =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
    plusBrightnessTen->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(10); });
    brightnessControls->addView(plusBrightnessTen);
    auto *resetBrightness = makeResetButton(metrics);
    resetBrightness->setOnClickListener([this]() {
      context.settings.bgaBrightnessPercent =
          AppSettings::kDefaultBgaBrightnessPercent;
      persistSettings();
      syncBgaBrightnessInputText(true);
    });
    brightnessControls->addView(resetBrightness);
    cardsColumn->addView(makeCard(
        metrics, "BGA Brightness",
        metrics.compact ? "Dim or restore the blurred BGA behind the lane."
                        : "Dim the BGA composite behind the lane when the "
                          "background competes with notes.",
        brightnessControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *blurControls = new View();
    blurControls->setFlexDirection(FlexDirection::Row);
    blurControls->setFlexWrap(YGWrapWrap);
    blurControls->setGap(metrics.compact ? 8.0f : 12.0f);
    blurControls->setAlignItems(YGAlignFlexStart);
    auto updateBgaBlur = [this](float delta) {
      context.settings.bgaBlurStrength =
          clampBgaBlur(context.settings.bgaBlurStrength + delta);
      persistSettings();
      syncBgaBlurInputText(true);
    };
    auto *minusBlurLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
    minusBlurLarge->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(-1.0f); });
    blurControls->addView(minusBlurLarge);
    auto *minusBlurSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusBlurSmall->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(-0.5f); });
    blurControls->addView(minusBlurSmall);
    bgaBlurInput = makeNumericInput(metrics);
    bgaBlurInput->onEditingFinished(
        [this](const std::string &) { commitBgaBlurInput(); });
    blurControls->addView(makeInputFrame(metrics, bgaBlurInput));
    auto *plusBlurSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusBlurSmall->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(0.5f); });
    blurControls->addView(plusBlurSmall);
    auto *plusBlurLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
    plusBlurLarge->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(1.0f); });
    blurControls->addView(plusBlurLarge);
    auto *resetBlur = makeResetButton(metrics);
    resetBlur->setOnClickListener([this]() {
      context.settings.bgaBlurStrength = AppSettings::kDefaultBgaBlurStrength;
      persistSettings();
      syncBgaBlurInputText(true);
    });
    blurControls->addView(resetBlur);
    cardsColumn->addView(makeCard(
        metrics, "BGA Blur Strength",
        metrics.compact ? "Higher values soften background motion."
                        : "Higher values soften background motion before it is "
                          "composited behind the lane.",
        blurControls, metrics.offsetCardHeight, metrics.cardsWidth));
  }

  if (activeTab == SettingsTab::Lane) {
    auto *visibleTimeControls = makeVisibleTimeControls(true, false);
    cardsColumn->addView(makeCard(
        metrics, "Visible Time",
        metrics.compact
            ? "Controls how long notes stay on screen before the judgement "
              "line."
            : "Controls how long notes stay visible before reaching the "
              "judgement line. Switch units if you prefer legacy green number "
              "or direct milliseconds.",
        visibleTimeControls, metrics.visibleTimeCardHeight,
        metrics.cardsWidth));

    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 12.0f);
    angleControls->setAlignItems(YGAlignFlexStart);
    auto updateLaneAngle = [this](float delta) {
      context.settings.laneAngleDegrees =
          clampLaneAngle(context.settings.laneAngleDegrees + delta);
      persistSettings();
      syncLaneAngleInputText(true);
    };
    auto *minusAngleLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5");
    minusAngleLarge->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-5.0f); });
    angleControls->addView(minusAngleLarge);
    auto *minusAngleSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusAngleSmall->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-1.0f); });
    angleControls->addView(minusAngleSmall);
    laneAngleInput = makeNumericInput(metrics);
    laneAngleInput->onEditingFinished(
        [this](const std::string &) { commitLaneAngleInput(); });
    angleControls->addView(makeInputFrame(metrics, laneAngleInput));
    auto *plusAngleSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusAngleSmall->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(1.0f); });
    angleControls->addView(plusAngleSmall);
    auto *plusAngleLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5");
    plusAngleLarge->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(5.0f); });
    angleControls->addView(plusAngleLarge);
    auto *resetAngle = makeResetButton(metrics);
    resetAngle->setOnClickListener([this]() {
      context.settings.laneAngleDegrees = AppSettings::kDefaultLaneAngleDegrees;
      persistSettings();
      syncLaneAngleInputText(true);
    });
    angleControls->addView(resetAngle);
    cardsColumn->addView(makeCard(
        metrics, "Lane Angle",
        metrics.compact ? "Adjust visual lane tilt and touch mapping together."
                        : "Adjust the gameplay camera pitch. Touch lane "
                          "conversion uses the same lane plane, so this stays "
                          "aligned for touch play.",
        angleControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 12.0f);
    lengthControls->setAlignItems(YGAlignFlexStart);
    auto updateLaneLength = [this](float delta) {
      context.settings.laneLength =
          clampLaneLength(context.settings.laneLength + delta);
      persistSettings();
      syncLaneLengthInputText(true);
    };
    auto *minusLengthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
    minusLengthLarge->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-1.0f); });
    lengthControls->addView(minusLengthLarge);
    auto *minusLengthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusLengthSmall->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-0.5f); });
    lengthControls->addView(minusLengthSmall);
    laneLengthInput = makeNumericInput(metrics);
    laneLengthInput->onEditingFinished(
        [this](const std::string &) { commitLaneLengthInput(); });
    lengthControls->addView(makeInputFrame(metrics, laneLengthInput));
    auto *plusLengthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusLengthSmall->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(0.5f); });
    lengthControls->addView(plusLengthSmall);
    auto *plusLengthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
    plusLengthLarge->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(1.0f); });
    lengthControls->addView(plusLengthLarge);
    auto *resetLength = makeResetButton(metrics);
    resetLength->setOnClickListener([this]() {
      context.settings.laneLength = AppSettings::kDefaultLaneLength;
      persistSettings();
      syncLaneLengthInputText(true);
    });
    lengthControls->addView(resetLength);
    cardsColumn->addView(makeCard(
        metrics, "Lane Length",
        metrics.compact ? "Adjust how far the visible lane reaches."
                        : "Adjust how far the visible lane reaches toward the "
                          "top of the screen.",
        lengthControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *previewControls = new View();
    previewControls->setFlexDirection(FlexDirection::Column);
    previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
    previewControls->setAlignItems(YGAlignFlexStart);
    previewControls->addView(makeWrappedText(
        metrics.compact
            ? "Open a live gameplay preview with falling notes."
            : "Open a live gameplay preview with falling notes. It uses the "
              "same lane renderer, camera, viewport, and note textures as "
              "gameplay.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    auto *previewButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText("Preview", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(35, 68, 62, 255), Color(45, 88, 80, 255),
        Color(63, 118, 107, 255), Color(97, 157, 142, 255),
        Color(120, 187, 169, 255), Color(145, 214, 195, 255));
    previewButton->setOnClickListener([this]() { startLanePreview(); });
    previewControls->addView(previewButton);
    cardsColumn->addView(makeCard(
        metrics, "Gameplay Preview",
        metrics.compact ? "Test lane setup in the gameplay renderer."
                        : "Test lane setup in the gameplay renderer before "
                          "entering a chart.",
        previewControls, metrics.modeCardHeight, metrics.cardsWidth));
  }
  scrollContent->addView(cardsColumn);

  auto *footer = new View();
  footer->setPadding(Edge::All, static_cast<float>(metrics.cardPadding - 4));
  footer->setBackgroundColor(Color(14, 22, 34, 220));
  footer->setBorderColor(Color(59, 80, 108, 255));
  footer->setBorderWidth(2);
  footer->addView(makeWrappedText(
      metrics.compact
          ? "Settings save automatically in the app documents directory."
          : "Settings are saved automatically in the app documents directory.",
      metrics.bodyTextSize, Color(165, 185, 205)));
  scrollContent->addView(footer);

  scrollView->setContentView(scrollContent);
  content->addView(scrollView);
  rootLayout->addView(content);

  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshSettingsText();
}

void SettingsScene::refreshSettingsText() {
  const int offsetMs = context.settings.inputOffsetMs;
  const int visualOffsetMs = context.settings.visualOffsetMs;
  const int visibleTimeGreenNumber = context.settings.visibleTimeGreenNumber;
  const std::string offsetLabel = formatOffsetLabel(offsetMs);
  const std::string visualOffsetLabel = formatOffsetLabel(visualOffsetMs);
  const std::string visibleTimeLabel = formatVisibleTimeLabel(
      visibleTimeGreenNumber, context.settings.visibleTimeUseMilliseconds);
  const std::string keysoundLabel =
      context.settings.inputKeysoundEnabled ? "Input Trigger" : "Auto Timed";
  const std::string bgaLabel =
      context.settings.bgaEnabled ? "Enabled" : "Disabled";
  const std::string bgaDisplayLabel =
      formatBgaDisplayModeLabel(context.settings.bgaDisplayMode);
  const std::string bgaBrightnessLabel =
      formatBgaBrightnessLabel(context.settings.bgaBrightnessPercent);
  const std::string bgaBlurLabel =
      formatBgaBlurLabel(context.settings.bgaBlurStrength);
  const std::string laneAngleLabel =
      formatLaneAngleLabel(context.settings.laneAngleDegrees);
  const std::string laneLengthLabel =
      formatLaneLengthLabel(context.settings.laneLength);

  syncOffsetInputText();
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setText(offsetLabel);
  }
  syncVisualOffsetInputText();
  if (summaryVisualOffsetValueText != nullptr) {
    summaryVisualOffsetValueText->setText(visualOffsetLabel);
  }
  syncVisibleTimeInputText();
  if (summaryVisibleTimeValueText != nullptr) {
    summaryVisibleTimeValueText->setText(visibleTimeLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setText(bgaLabel);
  }
  if (summaryBgaDisplayValueText != nullptr) {
    summaryBgaDisplayValueText->setText(bgaDisplayLabel);
  }
  syncBgaBrightnessInputText();
  if (summaryBgaBrightnessValueText != nullptr) {
    summaryBgaBrightnessValueText->setText(bgaBrightnessLabel);
  }
  syncBgaBlurInputText();
  if (summaryBgaBlurValueText != nullptr) {
    summaryBgaBlurValueText->setText(bgaBlurLabel);
  }
  syncLaneAngleInputText();
  if (summaryLaneAngleValueText != nullptr) {
    summaryLaneAngleValueText->setText(laneAngleLabel);
  }
  syncLaneLengthInputText();
  if (summaryLaneLengthValueText != nullptr) {
    summaryLaneLengthValueText->setText(laneLengthLabel);
  }
  if (keysoundModeText != nullptr) {
    keysoundModeText->setText(keysoundLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setText(bgaLabel);
  }
  if (bgaDisplayModeText != nullptr) {
    bgaDisplayModeText->setText(bgaDisplayLabel);
  }
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setText(context.settings.visibleTimeUseMilliseconds
                                     ? "Milliseconds"
                                     : "Green Number");
  }

  if (visibleTimeModeButton != nullptr) {
    if (context.settings.visibleTimeUseMilliseconds) {
      visibleTimeModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                                 Color(45, 88, 80, 255),
                                                 Color(63, 118, 107, 255));
      visibleTimeModeButton->setBorderColors(Color(97, 157, 142, 255),
                                             Color(120, 187, 169, 255),
                                             Color(145, 214, 195, 255));
    } else {
      visibleTimeModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                                 Color(43, 72, 110, 255),
                                                 Color(59, 98, 147, 255));
      visibleTimeModeButton->setBorderColors(Color(92, 131, 177, 255),
                                             Color(118, 163, 217, 255),
                                             Color(139, 189, 244, 255));
    }
  }

  if (keysoundModeButton != nullptr) {
    if (context.settings.inputKeysoundEnabled) {
      keysoundModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                              Color(43, 72, 110, 255),
                                              Color(59, 98, 147, 255));
      keysoundModeButton->setBorderColors(Color(92, 131, 177, 255),
                                          Color(118, 163, 217, 255),
                                          Color(139, 189, 244, 255));
    } else {
      keysoundModeButton->setBackgroundColors(Color(73, 56, 35, 255),
                                              Color(96, 72, 45, 255),
                                              Color(127, 95, 59, 255));
      keysoundModeButton->setBorderColors(Color(165, 120, 74, 255),
                                          Color(194, 141, 88, 255),
                                          Color(224, 163, 103, 255));
    }
  }

  if (bgaModeButton != nullptr) {
    if (context.settings.bgaEnabled) {
      bgaModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                         Color(45, 88, 80, 255),
                                         Color(63, 118, 107, 255));
      bgaModeButton->setBorderColors(Color(97, 157, 142, 255),
                                     Color(120, 187, 169, 255),
                                     Color(145, 214, 195, 255));
    } else {
      bgaModeButton->setBackgroundColors(Color(56, 42, 40, 255),
                                         Color(75, 55, 52, 255),
                                         Color(104, 75, 71, 255));
      bgaModeButton->setBorderColors(Color(141, 103, 98, 255),
                                     Color(176, 127, 121, 255),
                                     Color(209, 150, 143, 255));
    }
  }

  auto applyTabStyle = [this](Button *button, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      button->setBackgroundColors(Color(35, 68, 62, 255),
                                  Color(45, 88, 80, 255),
                                  Color(63, 118, 107, 255));
      button->setBorderColors(Color(97, 157, 142, 255),
                              Color(120, 187, 169, 255),
                              Color(145, 214, 195, 255));
    } else {
      button->setBackgroundColors(Color(28, 40, 58, 255),
                                  Color(36, 52, 75, 255),
                                  Color(61, 87, 118, 255));
      button->setBorderColors(Color(84, 107, 139, 255),
                              Color(108, 136, 174, 255),
                              Color(139, 172, 217, 255));
    }
  };
  applyTabStyle(timingTabButton, SettingsTab::Timing);
  applyTabStyle(visualTabButton, SettingsTab::Visual);
  applyTabStyle(laneTabButton, SettingsTab::Lane);

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  context.jukebox.setVisualOffsetMs(context.settings.visualOffsetMs);
  context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
  refreshSettingsText();
}

void SettingsScene::syncOffsetInputText(bool force) {
  if (offsetInput == nullptr) {
    return;
  }
  if (!force && offsetInput->getSelected()) {
    return;
  }
  offsetInput->setEditingText(
      formatOffsetInputValue(context.settings.inputOffsetMs));
}

void SettingsScene::syncVisualOffsetInputText(bool force) {
  if (visualOffsetInput == nullptr) {
    return;
  }
  if (!force && visualOffsetInput->getSelected()) {
    return;
  }
  visualOffsetInput->setEditingText(
      formatOffsetInputValue(context.settings.visualOffsetMs));
}

void SettingsScene::syncVisibleTimeInputText(bool force) {
  if (visibleTimeInput == nullptr) {
    return;
  }
  if (!force && visibleTimeInput->getSelected()) {
    return;
  }
  visibleTimeInput->setEditingText(
      formatVisibleTimeInputValue(context.settings.visibleTimeGreenNumber,
                                  context.settings.visibleTimeUseMilliseconds));
}

void SettingsScene::syncBgaBrightnessInputText(bool force) {
  if (bgaBrightnessInput == nullptr) {
    return;
  }
  if (!force && bgaBrightnessInput->getSelected()) {
    return;
  }
  bgaBrightnessInput->setEditingText(
      std::to_string(context.settings.bgaBrightnessPercent));
}

void SettingsScene::syncBgaBlurInputText(bool force) {
  if (bgaBlurInput == nullptr) {
    return;
  }
  if (!force && bgaBlurInput->getSelected()) {
    return;
  }
  bgaBlurInput->setEditingText(
      formatFloatValue(context.settings.bgaBlurStrength));
}

void SettingsScene::syncLaneAngleInputText(bool force) {
  if (laneAngleInput == nullptr) {
    return;
  }
  if (!force && laneAngleInput->getSelected()) {
    return;
  }
  laneAngleInput->setEditingText(
      formatFloatValue(context.settings.laneAngleDegrees));
}

void SettingsScene::syncLaneLengthInputText(bool force) {
  if (laneLengthInput == nullptr) {
    return;
  }
  if (!force && laneLengthInput->getSelected()) {
    return;
  }
  laneLengthInput->setEditingText(
      formatFloatValue(context.settings.laneLength));
}

void SettingsScene::commitOffsetInput() {
  if (offsetInput == nullptr) {
    return;
  }

  const std::string rawText = offsetInput->getText();
  if (rawText.empty()) {
    syncOffsetInputText(true);
    return;
  }

  try {
    context.settings.inputOffsetMs = clampOffset(std::stoi(rawText));
    persistSettings();
    syncOffsetInputText(true);
  } catch (const std::exception &) {
    syncOffsetInputText(true);
  }
}

void SettingsScene::commitVisualOffsetInput() {
  if (visualOffsetInput == nullptr) {
    return;
  }

  const std::string rawText = visualOffsetInput->getText();
  if (rawText.empty()) {
    syncVisualOffsetInputText(true);
    return;
  }

  try {
    context.settings.visualOffsetMs = clampVisualOffset(std::stoi(rawText));
    persistSettings();
    syncVisualOffsetInputText(true);
  } catch (const std::exception &) {
    syncVisualOffsetInputText(true);
  }
}

void SettingsScene::commitVisibleTimeInput() {
  if (visibleTimeInput == nullptr) {
    return;
  }

  const std::string rawText = visibleTimeInput->getText();
  if (rawText.empty()) {
    syncVisibleTimeInputText(true);
    return;
  }

  try {
    const int parsedValue = std::stoi(rawText);
    if (context.settings.visibleTimeUseMilliseconds) {
      const int milliseconds =
          std::clamp(parsedValue, AppSettings::kMinVisibleTimeMs,
                     AppSettings::kMaxVisibleTimeMs);
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(millisecondsToGreenNumber(milliseconds));
    } else {
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(parsedValue);
    }
    persistSettings();
    syncVisibleTimeInputText(true);
  } catch (const std::exception &) {
    syncVisibleTimeInputText(true);
  }
}

void SettingsScene::commitBgaBrightnessInput() {
  if (bgaBrightnessInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBrightnessInput->getText();
  if (rawText.empty()) {
    syncBgaBrightnessInputText(true);
    return;
  }

  try {
    context.settings.bgaBrightnessPercent =
        clampBgaBrightness(std::stoi(rawText));
    persistSettings();
    syncBgaBrightnessInputText(true);
  } catch (const std::exception &) {
    syncBgaBrightnessInputText(true);
  }
}

void SettingsScene::commitBgaBlurInput() {
  if (bgaBlurInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBlurInput->getText();
  if (rawText.empty()) {
    syncBgaBlurInputText(true);
    return;
  }

  try {
    context.settings.bgaBlurStrength = clampBgaBlur(std::stof(rawText));
    persistSettings();
    syncBgaBlurInputText(true);
  } catch (const std::exception &) {
    syncBgaBlurInputText(true);
  }
}

void SettingsScene::commitLaneAngleInput() {
  if (laneAngleInput == nullptr) {
    return;
  }

  const std::string rawText = laneAngleInput->getText();
  if (rawText.empty()) {
    syncLaneAngleInputText(true);
    return;
  }

  try {
    context.settings.laneAngleDegrees = clampLaneAngle(std::stof(rawText));
    persistSettings();
    syncLaneAngleInputText(true);
  } catch (const std::exception &) {
    syncLaneAngleInputText(true);
  }
}

void SettingsScene::commitLaneLengthInput() {
  if (laneLengthInput == nullptr) {
    return;
  }

  const std::string rawText = laneLengthInput->getText();
  if (rawText.empty()) {
    syncLaneLengthInputText(true);
    return;
  }

  try {
    context.settings.laneLength = clampLaneLength(std::stof(rawText));
    persistSettings();
    syncLaneLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneLengthInputText(true);
  }
}

void SettingsScene::update(float dt) {
  if (previewActive) {
    ensurePreviewRenderer();
    previewElapsedMicros +=
        static_cast<long long>(std::max(0.0f, dt) * 1000000.0f);
    if (previewElapsedMicros >= kPreviewLoopMicros) {
      resetPreviewSimulation();
    }
  }
  ensureLayoutUpToDate();
}

void SettingsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
  if (previewActive && previewRenderer != nullptr) {
    previewRenderer->setVisibleTimeGreenNumber(
        context.settings.visibleTimeGreenNumber);
    previewRenderer->refreshGeometry();
    RenderContext renderContext;
    previewRenderer->render(renderContext, previewElapsedMicros);
  }
}

void SettingsScene::cleanupScene() {
  destroyPreviewRenderer();
  rootLayout = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}
