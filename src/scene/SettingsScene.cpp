#include "SettingsScene.h"
#include "../context.h"
#include "../rendering/Color.h"
#include "../view/BlockingOverlayView.h"
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
#include <filesystem>
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

TextInputBox *makeTextInput(const LayoutMetrics &metrics, int minWidth) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize);
  input->setText("");
  input->setSize(minWidth, metrics.actionButtonHeight);
  input->setBackgroundColor(Color(10, 17, 28, 255));
  input->setBorderColor(Color(78, 105, 140, 255));
  input->setBorderWidth(2);
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
  return std::clamp(value, AppSettings::kMinAudioOffsetMs,
                    AppSettings::kMaxAudioOffsetMs);
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

float clampJudgementIndicatorY(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorY;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorY,
                    AppSettings::kMaxJudgementIndicatorY);
}

int judgementIndicatorYToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorY(value) * 100.0f));
}

float judgementIndicatorPercentToY(int percent) {
  return clampJudgementIndicatorY(static_cast<float>(percent) / 100.0f);
}

float clampJudgementIndicatorWidthScale(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorWidthScale;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorWidthScale,
                    AppSettings::kMaxJudgementIndicatorWidthScale);
}

int judgementIndicatorWidthScaleToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorWidthScale(value) * 100.0f));
}

float judgementIndicatorWidthPercentToScale(int percent) {
  return clampJudgementIndicatorWidthScale(static_cast<float>(percent) /
                                           100.0f);
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

std::string formatJudgementIndicatorRenderModeLabel(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return "3D Space";
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return "2D HUD";
  }
  return "3D Space";
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

std::string formatNotePriorityModeLabel(AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Lowest:
    return "Lowest";
  case AppSettings::NotePriorityMode::Combo:
    return "Combo";
  case AppSettings::NotePriorityMode::Duration:
    return "Duration";
  case AppSettings::NotePriorityMode::Score:
    return "Score";
  }
  return "Lowest";
}

std::string formatTableCount(int chartCount) {
  return std::to_string(chartCount) +
         (chartCount == 1 ? " chart" : " charts");
}

std::string formatTableSource(const std::string &sourceUrl) {
  if (sourceUrl.empty()) {
    return "No source URL";
  }
  return sourceUrl;
}

std::string formatChartEntryPath(const ChartEntry &entry) {
  return path_t_to_utf8(entry.path);
}

std::string formatChartEntryName(const ChartEntry &entry) {
  const std::filesystem::path path(entry.path);
  const std::filesystem::path name = path.filename();
  if (name.empty()) {
    return formatChartEntryPath(entry);
  }
  return path_t_to_utf8(fspath_to_path_t(name));
}

std::string formatChartEntrySource(const ChartEntry &entry) {
  const std::string pathText = formatChartEntryPath(entry);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (!entry.iosBookmark.empty()) {
    return pathText + "\nFiles access saved for future scans.";
  }
#endif
  return pathText;
}

std::string formatImportProgressText(int current, int total) {
  if (total <= 0) {
    return "Preparing";
  }
  const int safeCurrent = std::clamp(current, 0, total);
  return std::to_string(safeCurrent) + " / " + std::to_string(total) +
         (total == 1 ? " table" : " tables");
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

AppSettings::NotePriorityMode
nextNotePriorityMode(AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Lowest:
    return AppSettings::NotePriorityMode::Combo;
  case AppSettings::NotePriorityMode::Combo:
    return AppSettings::NotePriorityMode::Duration;
  case AppSettings::NotePriorityMode::Duration:
    return AppSettings::NotePriorityMode::Score;
  case AppSettings::NotePriorityMode::Score:
    return AppSettings::NotePriorityMode::Lowest;
  }
  return AppSettings::NotePriorityMode::Lowest;
}

AppSettings::JudgementIndicatorRenderMode nextJudgementIndicatorRenderMode(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return AppSettings::JudgementIndicatorRenderMode::Hud2D;
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return AppSettings::JudgementIndicatorRenderMode::World3D;
  }
  return AppSettings::JudgementIndicatorRenderMode::World3D;
}

constexpr long long kPreviewLoopMicros = 8000000LL;
constexpr int kPreviewTimelineLanes = 16;
constexpr double kPreviewBpm = 120.0;
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
    Judge previewJudge(previewChart->Meta.Rank);
    previewRenderer =
        new BMSRenderer(previewChart, previewJudge.timingWindows,
                        context.settings.visibleTimeGreenNumber, false);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
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
      for (auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          note->Reset();
        }
      }
    }
  }
}

void SettingsScene::loadDifficultyTables() {
  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *settingsDb = dbHelper.Connect();
  if (settingsDb == nullptr) {
    difficultyTables.clear();
    difficultyTableStatusMessage = "Could not open chart database.";
    difficultyTableStatusColor = {255, 177, 170, 255};
    return;
  }

  dbHelper.CreateDifficultyTableTables(settingsDb);
  difficultyTables = dbHelper.SelectDifficultyTables(settingsDb);
  dbHelper.Close(settingsDb);

  if (pendingDeleteDifficultyTableId != 0) {
    const auto it =
        std::find_if(difficultyTables.begin(), difficultyTables.end(),
                     [this](const DifficultyTableInfo &table) {
                       return table.id == pendingDeleteDifficultyTableId;
                     });
    if (it == difficultyTables.end()) {
      pendingDeleteDifficultyTableId = 0;
    }
  }
}

void SettingsScene::loadChartEntries() {
  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *settingsDb = dbHelper.Connect();
  if (settingsDb == nullptr) {
    chartEntries.clear();
    difficultyTableStatusMessage = "Could not open chart database.";
    difficultyTableStatusColor = {255, 177, 170, 255};
    return;
  }

  dbHelper.CreateEntriesTable(settingsDb);
  chartEntries = dbHelper.SelectAllEntries(settingsDb);
  dbHelper.Close(settingsDb);

  if (!pendingDeleteChartEntryPath.empty()) {
    const auto it = std::find_if(
        chartEntries.begin(), chartEntries.end(),
        [this](const ChartEntry &entry) {
          return formatChartEntryPath(entry) == pendingDeleteChartEntryPath;
        });
    if (it == chartEntries.end()) {
      pendingDeleteChartEntryPath.clear();
    }
  }
}

void SettingsScene::requestDifficultyTableStatus(const std::string &text,
                                                 const SDL_Color &color,
                                                 bool reloadTables) {
  std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
  pendingDifficultyTableStatus = true;
  pendingDifficultyTableStatusText = text;
  pendingDifficultyTableStatusColor = color;
  pendingDifficultyTableReload = pendingDifficultyTableReload || reloadTables;
}

void SettingsScene::requestDifficultyTableImportProgress(
    int current, int total, const std::string &tableName,
    const std::string &statusText, bool finished, bool succeeded) {
  std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
  pendingDifficultyTableImportProgress = true;
  pendingDifficultyTableImportCurrent = current;
  pendingDifficultyTableImportTotal = total;
  pendingDifficultyTableImportName = tableName;
  pendingDifficultyTableImportStatusText = statusText;
  pendingDifficultyTableImportFinished = finished;
  pendingDifficultyTableImportSucceeded = finished && succeeded;
}

void SettingsScene::applyPendingDifficultyTableUpdates() {
  bool shouldReload = false;
  bool shouldRefreshImportModal = false;
  {
    std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
    if (pendingDifficultyTableStatus) {
      difficultyTableStatusMessage = pendingDifficultyTableStatusText;
      difficultyTableStatusColor = pendingDifficultyTableStatusColor;
      if (difficultyTableStatusText != nullptr) {
        difficultyTableStatusText->setText(difficultyTableStatusMessage);
        difficultyTableStatusText->setColor(difficultyTableStatusColor);
      }
      pendingDifficultyTableStatus = false;
    }
    if (pendingDifficultyTableImportProgress) {
      difficultyTableImportCurrent = pendingDifficultyTableImportCurrent;
      difficultyTableImportTotal = pendingDifficultyTableImportTotal;
      difficultyTableImportName = pendingDifficultyTableImportName;
      difficultyTableImportStatusMessage =
          pendingDifficultyTableImportStatusText;
      difficultyTableImportFinished = pendingDifficultyTableImportFinished;
      difficultyTableImportSucceeded = pendingDifficultyTableImportSucceeded;
      difficultyTableImportModalVisible = true;
      pendingDifficultyTableImportProgress = false;
      shouldRefreshImportModal = true;
    }
    shouldReload = pendingDifficultyTableReload;
    pendingDifficultyTableReload = false;
  }

  if (shouldReload) {
    loadDifficultyTables();
    loadChartEntries();
    lastLayoutWidth = -1;
  }
  if (shouldRefreshImportModal) {
    refreshDifficultyTableImportModal();
  }
}

void SettingsScene::refreshDifficultyTableImportModal() {
  if (difficultyTableImportModalRoot == nullptr) {
    return;
  }

  difficultyTableImportModalRoot->setSize(rendering::window_width,
                                          rendering::window_height);
  difficultyTableImportModalRoot->setVisible(difficultyTableImportModalVisible);
  if (!difficultyTableImportModalVisible) {
    return;
  }

  const bool finished = difficultyTableImportFinished;
  const bool succeeded = difficultyTableImportSucceeded;
  const int total = std::max(0, difficultyTableImportTotal);
  const int current = total > 0
                          ? std::clamp(difficultyTableImportCurrent, 0, total)
                          : 0;
  const float progressPercent =
      total > 0 ? (static_cast<float>(current) / static_cast<float>(total)) *
                      100.0f
                : 0.0f;

  if (difficultyTableImportTitleText != nullptr) {
    difficultyTableImportTitleText->setText(
        !finished ? "Importing Difficulty Tables"
                  : (succeeded ? "Import Complete" : "Import Failed"));
  }
  if (difficultyTableImportStatusText != nullptr) {
    if (!difficultyTableImportStatusMessage.empty()) {
      difficultyTableImportStatusText->setText(
          difficultyTableImportStatusMessage);
    } else {
      difficultyTableImportStatusText->setText(
          !finished ? "Downloading and importing tables..."
                    : (succeeded ? "Import finished." : "Import failed."));
    }
  }
  if (difficultyTableImportTableText != nullptr) {
    difficultyTableImportTableText->setText(
        difficultyTableImportName.empty()
            ? "Current table: Resolving table URL"
            : "Current table: " + difficultyTableImportName);
  }
  if (difficultyTableImportProgressText != nullptr) {
    difficultyTableImportProgressText->setText(
        formatImportProgressText(current, total));
  }
  if (difficultyTableImportProgressFill != nullptr) {
    difficultyTableImportProgressFill->setWidthPercent(progressPercent);
    difficultyTableImportProgressFill->setBackgroundColor(
        finished && !succeeded ? Color(191, 82, 92, 255)
                               : Color(97, 157, 142, 255));
  }
  if (difficultyTableImportCloseButton != nullptr) {
    const bool canClose = finished;
    difficultyTableImportCloseButton->setVisible(canClose);
    difficultyTableImportCloseButton->setWidth(canClose ? 160.0f : 0.0f);
    difficultyTableImportCloseButton->setHeight(canClose ? 60.0f : 0.0f);
  }

  difficultyTableImportModalRoot->applyYogaLayout();
}

void SettingsScene::hideDifficultyTableImportModal() {
  if (difficultyTableJobRunning.load() && !difficultyTableImportFinished) {
    return;
  }
  difficultyTableImportModalVisible = false;
  refreshDifficultyTableImportModal();
}

void SettingsScene::addDifficultyTableFromUrl() {
  if (difficultyTableJobRunning) {
    return;
  }

  const std::string url =
      tableUrlInput != nullptr ? tableUrlInput->getText() : tableUrlText;
  if (url.empty()) {
    difficultyTableStatusMessage = "Enter a table webpage URL first.";
    difficultyTableStatusColor = {255, 177, 170, 255};
    if (difficultyTableStatusText != nullptr) {
      difficultyTableStatusText->setText(difficultyTableStatusMessage);
      difficultyTableStatusText->setColor(difficultyTableStatusColor);
    }
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = "Adding table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  difficultyTableImportModalVisible = true;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  difficultyTableImportCurrent = 0;
  difficultyTableImportTotal = 1;
  difficultyTableImportName = url;
  difficultyTableImportStatusMessage = "Preparing import...";
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }
  refreshDifficultyTableImportModal();

  difficultyTableJobThread =
      std::jthread([this, url](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        sqlite3 *settingsDb = dbHelper.Connect();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            difficultyTableJobRunning = false;
            requestDifficultyTableImportProgress(
                0, 1, url, "Could not open chart database.", true, false);
            requestDifficultyTableStatus("Could not open chart database.",
                                         {255, 177, 170, 255});
          }
          return;
        }

        dbHelper.CreateDifficultyTableTables(settingsDb);
        std::string errorMessage;
        DifficultyTableImportProgress lastProgress{0, 1, url};
        auto progressCallback =
            [this, &lastProgress,
             &token](const DifficultyTableImportProgress &progress) {
              if (token.stop_requested()) {
                return;
              }
              lastProgress = progress;
              requestDifficultyTableImportProgress(
                  progress.current, progress.total, progress.tableName,
                  "Downloading and importing tables...", false);
            };
        const bool imported = dbHelper.ImportDifficultyTableFromUrl(
            settingsDb, url, &errorMessage, progressCallback);
        dbHelper.Close(settingsDb);

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        difficultyTableJobRunning = false;
        const std::string finalMessage =
            imported ? (errorMessage.empty() ? "Table added." : errorMessage)
                     : (errorMessage.empty() ? "Add failed." : errorMessage);
        requestDifficultyTableImportProgress(
            lastProgress.current, lastProgress.total, lastProgress.tableName,
            finalMessage, true, imported);
        requestDifficultyTableStatus(finalMessage,
                                     imported ? SDL_Color{181, 228, 165, 255}
                                              : SDL_Color{255, 177, 170, 255},
                                     imported);
      });
}

void SettingsScene::updateDifficultyTableFromSource(int tableId) {
  if (difficultyTableJobRunning || tableId <= 0) {
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = "Updating table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  difficultyTableJobThread =
      std::jthread([this, tableId](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        sqlite3 *settingsDb = dbHelper.Connect();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            requestDifficultyTableStatus("Could not open chart database.",
                                         {255, 177, 170, 255});
            difficultyTableJobRunning = false;
          }
          return;
        }

        std::string errorMessage;
        const bool updated = dbHelper.UpdateDifficultyTableFromSourceUrl(
            settingsDb, tableId, &errorMessage);
        dbHelper.Close(settingsDb);

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        requestDifficultyTableStatus(
            updated ? "Table updated."
                    : (errorMessage.empty() ? "Update failed." : errorMessage),
            updated ? SDL_Color{181, 228, 165, 255}
                    : SDL_Color{255, 177, 170, 255},
            updated);
        difficultyTableJobRunning = false;
      });
}

void SettingsScene::deleteDifficultyTable(int tableId) {
  if (difficultyTableJobRunning || tableId <= 0) {
    return;
  }

  if (pendingDeleteDifficultyTableId != tableId) {
    pendingDeleteDifficultyTableId = tableId;
    pendingDeleteChartEntryPath.clear();
    difficultyTableStatusMessage = "Tap Confirm on that table to delete it.";
    difficultyTableStatusColor = {255, 213, 151, 255};
    lastLayoutWidth = -1;
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  difficultyTableStatusMessage = "Deleting table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  difficultyTableJobThread =
      std::jthread([this, tableId](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        sqlite3 *settingsDb = dbHelper.Connect();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            requestDifficultyTableStatus("Could not open chart database.",
                                         {255, 177, 170, 255});
            difficultyTableJobRunning = false;
          }
          return;
        }

        const bool deleted = dbHelper.DeleteDifficultyTable(settingsDb, tableId);
        dbHelper.Close(settingsDb);

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        requestDifficultyTableStatus(
            deleted ? "Table deleted." : "Delete failed.",
            deleted ? SDL_Color{181, 228, 165, 255}
                    : SDL_Color{255, 177, 170, 255},
            deleted);
        difficultyTableJobRunning = false;
      });
}

void SettingsScene::deleteChartEntry(const std::string &entryPathText) {
  if (difficultyTableJobRunning || entryPathText.empty()) {
    return;
  }

  if (pendingDeleteChartEntryPath != entryPathText) {
    pendingDeleteChartEntryPath = entryPathText;
    pendingDeleteDifficultyTableId = 0;
    difficultyTableStatusMessage = "Tap Confirm on that folder to remove it.";
    difficultyTableStatusColor = {255, 213, 151, 255};
    lastLayoutWidth = -1;
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = "Removing folder...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  difficultyTableJobThread =
      std::jthread([this, entryPathText](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        sqlite3 *settingsDb = dbHelper.Connect();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            requestDifficultyTableStatus("Could not open chart database.",
                                         {255, 177, 170, 255});
            difficultyTableJobRunning = false;
          }
          return;
        }

        dbHelper.CreateChartMetaTable(settingsDb);
        dbHelper.CreateEntriesTable(settingsDb);

        const auto entries = dbHelper.SelectAllEntries(settingsDb);
        const auto entryIt = std::find_if(
            entries.begin(), entries.end(),
            [&entryPathText](const ChartEntry &entry) {
              return formatChartEntryPath(entry) == entryPathText;
            });

        if (entryIt == entries.end()) {
          dbHelper.Close(settingsDb);
          if (!token.stop_requested()) {
            requestDifficultyTableStatus("Folder entry was not found.",
                                         {255, 177, 170, 255}, true);
            difficultyTableJobRunning = false;
          }
          return;
        }

        const std::filesystem::path entryPath(entryIt->path);
        dbHelper.BeginTransaction(settingsDb);
        const int removedChartCount =
            dbHelper.DeleteChartMetaInDirectory(settingsDb, entryPath);
        const bool removed =
            removedChartCount >= 0 && dbHelper.DeleteEntry(settingsDb, entryPath);
        if (removed) {
          dbHelper.CommitTransaction(settingsDb);
        } else {
          sqlite3_exec(settingsDb, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        dbHelper.Close(settingsDb);

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        std::string statusText;
        if (removed) {
          statusText = removedChartCount == 1
                           ? "Folder removed. Removed 1 cached chart."
                           : "Folder removed. Removed " +
                                 std::to_string(removedChartCount) +
                                 " cached charts.";
        } else {
          statusText = "Remove failed.";
        }
        requestDifficultyTableStatus(
            statusText,
            removed ? SDL_Color{181, 228, 165, 255}
                    : SDL_Color{255, 177, 170, 255},
            true);
        difficultyTableJobRunning = false;
      });
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
  summaryNotePriorityValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  showInvisibleNotesModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  showInvisibleNotesModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  tablesTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
  difficultyTableImportModalRoot = nullptr;
  difficultyTableImportProgressFill = nullptr;
  difficultyTableImportTitleText = nullptr;
  difficultyTableImportStatusText = nullptr;
  difficultyTableImportTableText = nullptr;
  difficultyTableImportProgressText = nullptr;
  difficultyTableImportCloseButton = nullptr;
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
  tablesTabButton = makeTabButton(SettingsTab::Tables, "Tables");
  tabControls->addView(timingTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(laneTabButton);
  tabControls->addView(tablesTabButton);
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
      context.settings.audioOffsetMs =
          clampOffset(context.settings.audioOffsetMs + delta);
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
      context.settings.audioOffsetMs = 0;
      persistSettings();
      syncOffsetInputText(true);
    });
    offsetControls->addView(resetOffset);

    cardsColumn->addView(makeCard(
        metrics, "Audio Offset",
        metrics.compact
            ? "Negative values make chart audio feel earlier."
            : "Negative values delay gameplay and BGA so chart audio, "
              "auto-timed keysounds, and replay keysounds feel earlier.",
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
            ? "Adjusts note display time only."
            : "Adjusts note display time only. BGA timing stays on the chart "
              "audio timeline.",
        visualOffsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *judgementIndicatorControls = new View();
    judgementIndicatorControls->setFlexDirection(FlexDirection::Column);
    judgementIndicatorControls->setGap(metrics.compact ? 12.0f : 16.0f);
    judgementIndicatorControls->setAlignItems(YGAlignFlexStart);

    auto *judgementIndicatorModeControls = new View();
    judgementIndicatorModeControls->setFlexDirection(FlexDirection::Row);
    judgementIndicatorModeControls->setFlexWrap(YGWrapWrap);
    judgementIndicatorModeControls->setGap(metrics.compact ? 8.0f : 12.0f);
    judgementIndicatorModeControls->setAlignItems(YGAlignFlexStart);

    judgementIndicatorModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    judgementIndicatorModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        judgementIndicatorModeText, Color(35, 68, 62, 255),
        Color(45, 88, 80, 255), Color(63, 118, 107, 255),
        Color(97, 157, 142, 255), Color(120, 187, 169, 255),
        Color(145, 214, 195, 255));
    judgementIndicatorModeButton->setOnClickListener([this]() {
      context.settings.judgementIndicatorEnabled =
          !context.settings.judgementIndicatorEnabled;
      persistSettings();
    });
    judgementIndicatorModeControls->addView(judgementIndicatorModeButton);

    judgementIndicatorRenderModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    judgementIndicatorRenderModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        judgementIndicatorRenderModeText, Color(33, 56, 87, 255),
        Color(43, 72, 110, 255), Color(59, 98, 147, 255),
        Color(92, 131, 177, 255), Color(118, 163, 217, 255),
        Color(139, 189, 244, 255));
    judgementIndicatorRenderModeButton->setOnClickListener([this]() {
      context.settings.judgementIndicatorRenderMode =
          nextJudgementIndicatorRenderMode(
              context.settings.judgementIndicatorRenderMode);
      persistSettings();
    });
    judgementIndicatorModeControls->addView(
        judgementIndicatorRenderModeButton);
    judgementIndicatorControls->addView(judgementIndicatorModeControls);

    judgementIndicatorControls->addView(
        makeText("Y Position", metrics.bodyTextSize, Color(168, 186, 209)));
    auto *judgementIndicatorYControls = new View();
    judgementIndicatorYControls->setFlexDirection(FlexDirection::Row);
    judgementIndicatorYControls->setFlexWrap(YGWrapWrap);
    judgementIndicatorYControls->setGap(metrics.compact ? 8.0f : 12.0f);
    judgementIndicatorYControls->setAlignItems(YGAlignFlexStart);
    auto updateJudgementIndicatorY = [this](int deltaPercent) {
      const int currentPercent =
          judgementIndicatorYToPercent(context.settings.judgementIndicatorY);
      const int nextPercent = std::clamp(currentPercent + deltaPercent, 0, 100);
      context.settings.judgementIndicatorY =
          judgementIndicatorPercentToY(nextPercent);
      persistSettings();
      syncJudgementIndicatorYInputText(true);
    };

    auto *minusIndicatorYLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
    minusIndicatorYLarge->setOnClickListener(
        [updateJudgementIndicatorY]() { updateJudgementIndicatorY(-10); });
    judgementIndicatorYControls->addView(minusIndicatorYLarge);
    auto *minusIndicatorYSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
    minusIndicatorYSmall->setOnClickListener(
        [updateJudgementIndicatorY]() { updateJudgementIndicatorY(-1); });
    judgementIndicatorYControls->addView(minusIndicatorYSmall);
    judgementIndicatorYInput = makeNumericInput(metrics);
    judgementIndicatorYInput->onEditingFinished(
        [this](const std::string &) { commitJudgementIndicatorYInput(); });
    judgementIndicatorYControls->addView(
        makeInputFrame(metrics, judgementIndicatorYInput));
    auto *plusIndicatorYSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
    plusIndicatorYSmall->setOnClickListener(
        [updateJudgementIndicatorY]() { updateJudgementIndicatorY(1); });
    judgementIndicatorYControls->addView(plusIndicatorYSmall);
    auto *plusIndicatorYLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
    plusIndicatorYLarge->setOnClickListener(
        [updateJudgementIndicatorY]() { updateJudgementIndicatorY(10); });
    judgementIndicatorYControls->addView(plusIndicatorYLarge);
    auto *resetIndicatorY = makeResetButton(metrics);
    resetIndicatorY->setOnClickListener([this]() {
      context.settings.judgementIndicatorY =
          AppSettings::kDefaultJudgementIndicatorY;
      persistSettings();
      syncJudgementIndicatorYInputText(true);
    });
    judgementIndicatorYControls->addView(resetIndicatorY);
    judgementIndicatorControls->addView(judgementIndicatorYControls);

    judgementIndicatorControls->addView(
        makeText("Width", metrics.bodyTextSize, Color(168, 186, 209)));
    auto *judgementIndicatorWidthControls = new View();
    judgementIndicatorWidthControls->setFlexDirection(FlexDirection::Row);
    judgementIndicatorWidthControls->setFlexWrap(YGWrapWrap);
    judgementIndicatorWidthControls->setGap(metrics.compact ? 8.0f : 12.0f);
    judgementIndicatorWidthControls->setAlignItems(YGAlignFlexStart);
    auto updateJudgementIndicatorWidth = [this](int deltaPercent) {
      const int currentPercent = judgementIndicatorWidthScaleToPercent(
          context.settings.judgementIndicatorWidthScale);
      const int minPercent = judgementIndicatorWidthScaleToPercent(
          AppSettings::kMinJudgementIndicatorWidthScale);
      const int maxPercent = judgementIndicatorWidthScaleToPercent(
          AppSettings::kMaxJudgementIndicatorWidthScale);
      const int nextPercent =
          std::clamp(currentPercent + deltaPercent, minPercent, maxPercent);
      context.settings.judgementIndicatorWidthScale =
          judgementIndicatorWidthPercentToScale(nextPercent);
      persistSettings();
      syncJudgementIndicatorWidthInputText(true);
    };

    auto *minusIndicatorWidthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
    minusIndicatorWidthLarge->setOnClickListener(
        [updateJudgementIndicatorWidth]() {
          updateJudgementIndicatorWidth(-10);
        });
    judgementIndicatorWidthControls->addView(minusIndicatorWidthLarge);
    auto *minusIndicatorWidthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
    minusIndicatorWidthSmall->setOnClickListener(
        [updateJudgementIndicatorWidth]() {
          updateJudgementIndicatorWidth(-1);
        });
    judgementIndicatorWidthControls->addView(minusIndicatorWidthSmall);
    judgementIndicatorWidthInput = makeNumericInput(metrics);
    judgementIndicatorWidthInput->onEditingFinished(
        [this](const std::string &) { commitJudgementIndicatorWidthInput(); });
    judgementIndicatorWidthControls->addView(
        makeInputFrame(metrics, judgementIndicatorWidthInput));
    auto *plusIndicatorWidthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
    plusIndicatorWidthSmall->setOnClickListener(
        [updateJudgementIndicatorWidth]() {
          updateJudgementIndicatorWidth(1);
        });
    judgementIndicatorWidthControls->addView(plusIndicatorWidthSmall);
    auto *plusIndicatorWidthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
    plusIndicatorWidthLarge->setOnClickListener(
        [updateJudgementIndicatorWidth]() {
          updateJudgementIndicatorWidth(10);
        });
    judgementIndicatorWidthControls->addView(plusIndicatorWidthLarge);
    auto *resetIndicatorWidth = makeResetButton(metrics);
    resetIndicatorWidth->setOnClickListener([this]() {
      context.settings.judgementIndicatorWidthScale =
          AppSettings::kDefaultJudgementIndicatorWidthScale;
      persistSettings();
      syncJudgementIndicatorWidthInputText(true);
    });
    judgementIndicatorWidthControls->addView(resetIndicatorWidth);
    judgementIndicatorControls->addView(judgementIndicatorWidthControls);

    cardsColumn->addView(makeCard(
        metrics, "Judgement Indicator",
        metrics.compact
            ? "Y: 0% bottom, 50% center, 100% top. Width: 100% base."
            : "Y position is vertical placement: 0% is the "
              "judgement-line/bottom side, 50% is center, and 100% is top. "
              "Width percent scales the selected mode's base width; 100% is "
              "default. 3D draws on the lane plane; HUD draws screen-flat.",
        judgementIndicatorControls, metrics.visibleTimeCardHeight,
        metrics.cardsWidth));

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

    auto *notePriorityControls = new View();
    notePriorityControls->setFlexDirection(FlexDirection::Column);
    notePriorityControls->setGap(metrics.compact ? 12.0f : 16.0f);
    notePriorityControls->setAlignItems(YGAlignFlexStart);
    notePriorityControls->addView(makeWrappedText(
        metrics.compact
            ? "Choose which hittable note a lane press judges first."
            : "Choose which hittable note a lane press judges first when "
              "multiple notes are inside the input window.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    notePriorityModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    notePriorityModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        notePriorityModeText, Color(33, 56, 87, 255),
        Color(43, 72, 110, 255), Color(59, 98, 147, 255),
        Color(92, 131, 177, 255), Color(118, 163, 217, 255),
        Color(139, 189, 244, 255));
    notePriorityModeButton->setOnClickListener([this]() {
      context.settings.notePriorityMode =
          nextNotePriorityMode(context.settings.notePriorityMode);
      persistSettings();
    });
    notePriorityControls->addView(notePriorityModeButton);
    secondaryCards->addView(makeCard(
        metrics, "Note Priority",
        metrics.compact ? "Lowest keeps the original frontmost-note behavior."
                        : "Lowest keeps the original frontmost-note behavior. "
                          "Other modes can prefer a following note based on "
                          "combo, timing distance, or score.",
        notePriorityControls, metrics.modeCardHeight,
        metrics.secondaryCardWidth));

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

    auto *invisibleNoteControls = new View();
    invisibleNoteControls->setFlexDirection(FlexDirection::Column);
    invisibleNoteControls->setGap(metrics.compact ? 12.0f : 16.0f);
    invisibleNoteControls->setAlignItems(YGAlignFlexStart);
    invisibleNoteControls->addView(makeWrappedText(
        metrics.compact ? "Draw hidden notes as temporary lane markers."
                        : "Draw invisible chart notes as temporary lane "
                          "markers. Judgement and scoring stay unchanged.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    showInvisibleNotesModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    showInvisibleNotesModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        showInvisibleNotesModeText, Color(33, 56, 87, 255),
        Color(43, 72, 110, 255), Color(59, 98, 147, 255),
        Color(92, 131, 177, 255), Color(118, 163, 217, 255),
        Color(139, 189, 244, 255));
    showInvisibleNotesModeButton->setOnClickListener([this]() {
      context.settings.showInvisibleNotes =
          !context.settings.showInvisibleNotes;
      persistSettings();
      if (previewRenderer != nullptr) {
        previewRenderer->setShowInvisibleNotes(
            context.settings.showInvisibleNotes);
      }
    });
    invisibleNoteControls->addView(showInvisibleNotesModeButton);
    cardsColumn->addView(makeCard(
        metrics, "Show Invisible Notes",
        metrics.compact
            ? "Orange rectangles are placeholders until skin art exists."
            : "Invisible notes use orange placeholder rectangles until the "
              "skin system exposes dedicated artwork.",
        invisibleNoteControls, metrics.modeCardHeight, metrics.cardsWidth));

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

  if (activeTab == SettingsTab::Tables) {
    loadDifficultyTables();
    loadChartEntries();

    auto *addControls = new View();
    addControls->setFlexDirection(FlexDirection::Column);
    addControls->setGap(metrics.compact ? 12.0f : 16.0f);
    addControls->setAlignItems(YGAlignFlexStart);

    const int addRowGap = metrics.compact ? 8 : 12;
    const int addButtonWidth = metrics.compact ? 150 : 170;
    const int minInputWidth = 180;
    auto *urlRow = new View();
    urlRow->setFlexDirection(FlexDirection::Row);
    urlRow->setFlexWrap(YGWrapWrap);
    urlRow->setGap(static_cast<float>(addRowGap));
    urlRow->setAlignItems(YGAlignFlexStart);
    urlRow->setAlignSelf(YGAlignStretch);

    tableUrlInput = makeTextInput(metrics, minInputWidth);
    tableUrlInput->setMinWidth(static_cast<float>(minInputWidth));
    tableUrlInput->setFlexGrow(1.0f);
    tableUrlInput->setFlexShrink(1.0f);
    tableUrlInput->setEditingText(tableUrlText);
    tableUrlInput->onTextChanged(
        [this](const std::string &text) { tableUrlText = text; });
    tableUrlInput->onSubmit([this](const std::string &text) {
      tableUrlText = text;
      addDifficultyTableFromUrl();
    });
    urlRow->addView(tableUrlInput);

    auto *addButton = makeButton(
        addButtonWidth, metrics.actionButtonHeight,
        makeText("Add Table", metrics.bodyTextSize + 4,
                 Color(239, 244, 251), TextView::CENTER, TextView::MIDDLE),
        Color(35, 68, 62, 255), Color(45, 88, 80, 255),
        Color(63, 118, 107, 255), Color(97, 157, 142, 255),
        Color(120, 187, 169, 255), Color(145, 214, 195, 255));
    addButton->setOnClickListener([this]() { addDifficultyTableFromUrl(); });
    urlRow->addView(addButton);
    addControls->addView(urlRow);

    difficultyTableStatusText =
        makeWrappedText(difficultyTableStatusMessage, metrics.bodyTextSize,
                        Color(difficultyTableStatusColor.r,
                              difficultyTableStatusColor.g,
                              difficultyTableStatusColor.b,
                              difficultyTableStatusColor.a));
    addControls->addView(difficultyTableStatusText);

    cardsColumn->addView(makeCard(
        metrics, "Add Difficulty Table",
        metrics.compact ? "Import a bmstable page, header, or table list URL."
                        : "Import a bmstable page URL or a direct header JSON "
                          "URL. Table-list JSON URLs import each listed table.",
        addControls, metrics.modeCardHeight, metrics.cardsWidth));

    auto *folderList = new View();
    folderList->setFlexDirection(FlexDirection::Column);
    folderList->setGap(metrics.compact ? 10.0f : 12.0f);

    if (chartEntries.empty()) {
      folderList->addView(makeWrappedText(
          "No chart folders are installed.", metrics.bodyTextSize,
          Color(165, 185, 205)));
    } else {
      for (const auto &entry : chartEntries) {
        const std::string entryPathText = formatChartEntryPath(entry);

        auto *row = new View();
        row->setFlexDirection(FlexDirection::Column);
        row->setGap(metrics.compact ? 8.0f : 10.0f);
        row->setPadding(Edge::All,
                        static_cast<float>(metrics.compact ? 14 : 16));
        row->setBackgroundColor(Color(12, 21, 34, 230));
        row->setBorderColor(Color(63, 86, 113, 255));
        row->setBorderWidth(2);

        row->addView(makeWrappedText(formatChartEntryName(entry),
                                     metrics.bodyTextSize + 6,
                                     Color(244, 248, 255)));
        row->addView(makeWrappedText(formatChartEntrySource(entry),
                                     metrics.smallTextSize,
                                     Color(142, 164, 189)));

        auto *actions = new View();
        actions->setFlexDirection(FlexDirection::Row);
        actions->setFlexWrap(YGWrapWrap);
        actions->setGap(metrics.compact ? 8.0f : 10.0f);

        const int folderActionWidth = metrics.compact ? 136 : 156;
        const bool confirmingDelete =
            pendingDeleteChartEntryPath == entryPathText;
        auto *deleteButton = makeButton(
            folderActionWidth, metrics.actionButtonHeight,
            makeText(confirmingDelete ? "Confirm" : "Delete",
                     metrics.bodyTextSize + 2, Color(248, 241, 236),
                     TextView::CENTER, TextView::MIDDLE),
            confirmingDelete ? Color(130, 58, 45, 255)
                             : Color(96, 57, 44, 255),
            confirmingDelete ? Color(153, 75, 58, 255)
                             : Color(117, 72, 55, 255),
            confirmingDelete ? Color(184, 96, 74, 255)
                             : Color(153, 96, 74, 255),
            Color(165, 105, 79, 255), Color(193, 124, 93, 255),
            Color(219, 145, 108, 255));
        deleteButton->setOnClickListener(
            [this, entryPathText]() { deleteChartEntry(entryPathText); });
        actions->addView(deleteButton);

        row->addView(actions);
        folderList->addView(row);
      }
    }

    cardsColumn->addView(makeCard(
        metrics, "Chart Folders",
        metrics.compact ? "Remove folders from library scanning."
                        : "Remove a folder entry and cached charts under it "
                          "from the library database.",
        folderList, metrics.modeCardHeight, metrics.cardsWidth));

    auto *tableList = new View();
    tableList->setFlexDirection(FlexDirection::Column);
    tableList->setGap(metrics.compact ? 10.0f : 12.0f);

    if (difficultyTables.empty()) {
      tableList->addView(makeWrappedText(
          "No difficulty tables are installed.", metrics.bodyTextSize,
          Color(165, 185, 205)));
    } else {
      for (const auto &table : difficultyTables) {
        auto *row = new View();
        row->setFlexDirection(FlexDirection::Column);
        row->setGap(metrics.compact ? 8.0f : 10.0f);
        row->setPadding(Edge::All,
                        static_cast<float>(metrics.compact ? 14 : 16));
        row->setBackgroundColor(Color(12, 21, 34, 230));
        row->setBorderColor(Color(63, 86, 113, 255));
        row->setBorderWidth(2);

        auto *titleRow = new View();
        titleRow->setFlexDirection(FlexDirection::Row);
        titleRow->setFlexWrap(YGWrapWrap);
        titleRow->setGap(metrics.compact ? 8.0f : 12.0f);
        titleRow->setAlignItems(YGAlignCenter);
        titleRow->addView(makeWrappedText(
            table.name, metrics.bodyTextSize + 6, Color(244, 248, 255)));
        titleRow->addView(makeText(table.symbol, metrics.bodyTextSize,
                                   Color(181, 207, 236)));
        titleRow->addView(makeText(formatTableCount(table.chartCount),
                                   metrics.bodyTextSize,
                                   Color(165, 185, 205)));
        row->addView(titleRow);

        row->addView(makeWrappedText(formatTableSource(table.sourceUrl),
                                     metrics.smallTextSize,
                                     Color(142, 164, 189)));

        auto *actions = new View();
        actions->setFlexDirection(FlexDirection::Row);
        actions->setFlexWrap(YGWrapWrap);
        actions->setGap(metrics.compact ? 8.0f : 10.0f);

        const int smallActionWidth = metrics.compact ? 136 : 156;
        auto *updateButton = makeButton(
            smallActionWidth, metrics.actionButtonHeight,
            makeText("Update", metrics.bodyTextSize + 2,
                     Color(239, 244, 251), TextView::CENTER,
                     TextView::MIDDLE),
            Color(33, 56, 87, 255), Color(43, 72, 110, 255),
            Color(59, 98, 147, 255), Color(92, 131, 177, 255),
            Color(118, 163, 217, 255), Color(139, 189, 244, 255));
        updateButton->setOnClickListener(
            [this, tableId = table.id]() {
              updateDifficultyTableFromSource(tableId);
            });
        actions->addView(updateButton);

        const bool confirmingDelete =
            pendingDeleteDifficultyTableId == table.id;
        auto *deleteButton = makeButton(
            smallActionWidth, metrics.actionButtonHeight,
            makeText(confirmingDelete ? "Confirm" : "Delete",
                     metrics.bodyTextSize + 2, Color(248, 241, 236),
                     TextView::CENTER, TextView::MIDDLE),
            confirmingDelete ? Color(130, 58, 45, 255)
                             : Color(96, 57, 44, 255),
            confirmingDelete ? Color(153, 75, 58, 255)
                             : Color(117, 72, 55, 255),
            confirmingDelete ? Color(184, 96, 74, 255)
                             : Color(153, 96, 74, 255),
            Color(165, 105, 79, 255), Color(193, 124, 93, 255),
            Color(219, 145, 108, 255));
        deleteButton->setOnClickListener(
            [this, tableId = table.id]() { deleteDifficultyTable(tableId); });
        actions->addView(deleteButton);

        row->addView(actions);
        tableList->addView(row);
      }
    }

    cardsColumn->addView(makeCard(
        metrics, "Installed Tables",
        metrics.compact ? "Update from source URL or remove a table."
                        : "Update a table from its stored source URL or remove "
                          "it from the chart database.",
        tableList, metrics.modeCardHeight, metrics.cardsWidth));
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

  difficultyTableImportModalRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  difficultyTableImportModalRoot->setPositionType(YGPositionTypeAbsolute);
  difficultyTableImportModalRoot->setPosition(Edge::Left, 0);
  difficultyTableImportModalRoot->setPosition(Edge::Top, 0);
  difficultyTableImportModalRoot->setZIndex(1000);
  difficultyTableImportModalRoot->setVisible(false);
  difficultyTableImportModalRoot->setFlexDirection(FlexDirection::Column);
  difficultyTableImportModalRoot->setAlignItems(YGAlignCenter);
  difficultyTableImportModalRoot->setJustifyContent(YGJustifyCenter);
  difficultyTableImportModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *importPanel = new View();
  importPanel->setWidth(static_cast<float>(
                            std::min(metrics.compact ? 620 : 760,
                                     std::max(280, metrics.contentWidth - 32))))
      ->setMinHeight(static_cast<float>(metrics.compact ? 320 : 360))
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(metrics.compact ? 14.0f : 18.0f)
      ->setPadding(Edge::All, static_cast<float>(metrics.cardPadding))
      ->setBackgroundColor(Color(17, 27, 42, 248))
      ->setBorderColor(Color(88, 118, 154, 255))
      ->setBorderWidth(2);

  difficultyTableImportTitleText = makeWrappedText(
      "Importing Difficulty Tables", metrics.sectionTitleSize,
      Color(244, 248, 255));
  importPanel->addView(difficultyTableImportTitleText);

  difficultyTableImportStatusText = makeWrappedText(
      "Preparing import...", metrics.bodyTextSize, Color(181, 207, 236));
  importPanel->addView(difficultyTableImportStatusText);

  difficultyTableImportTableText =
      makeWrappedText("Current table: Resolving table URL",
                      metrics.bodyTextSize, Color(239, 244, 251));
  importPanel->addView(difficultyTableImportTableText);

  auto *progressRow = new View();
  progressRow->setFlexDirection(FlexDirection::Column);
  progressRow->setGap(metrics.compact ? 8.0f : 10.0f);
  difficultyTableImportProgressText =
      makeText("0 / 1 table", metrics.bodyTextSize, Color(165, 185, 205));
  progressRow->addView(difficultyTableImportProgressText);

  auto *progressTrack = new View();
  progressTrack->setHeight(static_cast<float>(metrics.compact ? 16 : 18));
  progressTrack->setAlignSelf(YGAlignStretch);
  progressTrack->setFlexDirection(FlexDirection::Row);
  progressTrack->setBackgroundColor(Color(8, 14, 24, 255));
  progressTrack->setBorderColor(Color(66, 91, 122, 255));
  progressTrack->setBorderWidth(2);
  difficultyTableImportProgressFill = new View();
  difficultyTableImportProgressFill->setWidthPercent(0.0f);
  difficultyTableImportProgressFill->setHeight(
      static_cast<float>(metrics.compact ? 16 : 18));
  difficultyTableImportProgressFill->setBackgroundColor(
      Color(97, 157, 142, 255));
  progressTrack->addView(difficultyTableImportProgressFill);
  progressRow->addView(progressTrack);
  importPanel->addView(progressRow);

  auto *modalActions = new View();
  modalActions->setFlexDirection(FlexDirection::Row);
  modalActions->setJustifyContent(YGJustifyFlexEnd);
  difficultyTableImportCloseButton = makeButton(
      160, 60,
      makeText("Close", metrics.bodyTextSize + 2, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(33, 56, 87, 255), Color(43, 72, 110, 255),
      Color(59, 98, 147, 255), Color(92, 131, 177, 255),
      Color(118, 163, 217, 255), Color(139, 189, 244, 255));
  difficultyTableImportCloseButton->setOnClickListener(
      [this]() { hideDifficultyTableImportModal(); });
  modalActions->addView(difficultyTableImportCloseButton);
  importPanel->addView(modalActions);

  difficultyTableImportModalRoot->addView(importPanel);
  rootLayout->addView(difficultyTableImportModalRoot);

  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshDifficultyTableImportModal();
  refreshSettingsText();
}

void SettingsScene::refreshSettingsText() {
  const int offsetMs = context.settings.audioOffsetMs;
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
  const std::string notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const std::string invisibleNotesLabel =
      context.settings.showInvisibleNotes ? "Shown" : "Hidden";
  const std::string judgementIndicatorLabel =
      context.settings.judgementIndicatorEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.judgementIndicatorRenderMode);

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
  if (summaryNotePriorityValueText != nullptr) {
    summaryNotePriorityValueText->setText(notePriorityLabel);
  }
  syncJudgementIndicatorYInputText();
  syncJudgementIndicatorWidthInputText();
  if (keysoundModeText != nullptr) {
    keysoundModeText->setText(keysoundLabel);
  }
  if (notePriorityModeText != nullptr) {
    notePriorityModeText->setText(notePriorityLabel);
  }
  if (showInvisibleNotesModeText != nullptr) {
    showInvisibleNotesModeText->setText(invisibleNotesLabel);
  }
  if (judgementIndicatorModeText != nullptr) {
    judgementIndicatorModeText->setText(judgementIndicatorLabel);
  }
  if (judgementIndicatorRenderModeText != nullptr) {
    judgementIndicatorRenderModeText->setText(judgementIndicatorRenderModeLabel);
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

  if (notePriorityModeButton != nullptr) {
    if (context.settings.notePriorityMode ==
        AppSettings::NotePriorityMode::Lowest) {
      notePriorityModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                                  Color(43, 72, 110, 255),
                                                  Color(59, 98, 147, 255));
      notePriorityModeButton->setBorderColors(Color(92, 131, 177, 255),
                                              Color(118, 163, 217, 255),
                                              Color(139, 189, 244, 255));
    } else {
      notePriorityModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                                  Color(45, 88, 80, 255),
                                                  Color(63, 118, 107, 255));
      notePriorityModeButton->setBorderColors(Color(97, 157, 142, 255),
                                              Color(120, 187, 169, 255),
                                              Color(145, 214, 195, 255));
    }
  }

  if (showInvisibleNotesModeButton != nullptr) {
    if (context.settings.showInvisibleNotes) {
      showInvisibleNotesModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      showInvisibleNotesModeButton->setBorderColors(
          Color(97, 157, 142, 255), Color(120, 187, 169, 255),
          Color(145, 214, 195, 255));
    } else {
      showInvisibleNotesModeButton->setBackgroundColors(
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255));
      showInvisibleNotesModeButton->setBorderColors(
          Color(92, 131, 177, 255), Color(118, 163, 217, 255),
          Color(139, 189, 244, 255));
    }
  }

  if (judgementIndicatorModeButton != nullptr) {
    if (context.settings.judgementIndicatorEnabled) {
      judgementIndicatorModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      judgementIndicatorModeButton->setBorderColors(
          Color(97, 157, 142, 255), Color(120, 187, 169, 255),
          Color(145, 214, 195, 255));
    } else {
      judgementIndicatorModeButton->setBackgroundColors(
          Color(56, 42, 40, 255), Color(75, 55, 52, 255),
          Color(104, 75, 71, 255));
      judgementIndicatorModeButton->setBorderColors(
          Color(141, 103, 98, 255), Color(176, 127, 121, 255),
          Color(209, 150, 143, 255));
    }
  }

  if (judgementIndicatorRenderModeButton != nullptr) {
    if (context.settings.judgementIndicatorRenderMode ==
        AppSettings::JudgementIndicatorRenderMode::Hud2D) {
      judgementIndicatorRenderModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      judgementIndicatorRenderModeButton->setBorderColors(
          Color(97, 157, 142, 255), Color(120, 187, 169, 255),
          Color(145, 214, 195, 255));
    } else {
      judgementIndicatorRenderModeButton->setBackgroundColors(
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255));
      judgementIndicatorRenderModeButton->setBorderColors(
          Color(92, 131, 177, 255), Color(118, 163, 217, 255),
          Color(139, 189, 244, 255));
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
  applyTabStyle(tablesTabButton, SettingsTab::Tables);

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
  context.jukebox.setBgaOffsetMs(context.settings.audioOffsetMs);
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
      formatOffsetInputValue(context.settings.audioOffsetMs));
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

void SettingsScene::syncJudgementIndicatorYInputText(bool force) {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorYInput->getSelected()) {
    return;
  }
  judgementIndicatorYInput->setEditingText(
      std::to_string(judgementIndicatorYToPercent(
          context.settings.judgementIndicatorY)));
}

void SettingsScene::syncJudgementIndicatorWidthInputText(bool force) {
  if (judgementIndicatorWidthInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorWidthInput->getSelected()) {
    return;
  }
  judgementIndicatorWidthInput->setEditingText(
      std::to_string(judgementIndicatorWidthScaleToPercent(
          context.settings.judgementIndicatorWidthScale)));
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
    context.settings.audioOffsetMs = clampOffset(std::stoi(rawText));
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

void SettingsScene::commitJudgementIndicatorYInput() {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorYInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorYInputText(true);
    return;
  }

  try {
    const int percent = std::clamp(std::stoi(rawText), 0, 100);
    context.settings.judgementIndicatorY =
        judgementIndicatorPercentToY(percent);
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorYInputText(true);
  }
}

void SettingsScene::commitJudgementIndicatorWidthInput() {
  if (judgementIndicatorWidthInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorWidthInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorWidthInputText(true);
    return;
  }

  try {
    const int minPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMinJudgementIndicatorWidthScale);
    const int maxPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMaxJudgementIndicatorWidthScale);
    const int percent =
        std::clamp(std::stoi(rawText), minPercent, maxPercent);
    context.settings.judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(percent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorWidthInputText(true);
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
  applyPendingDifficultyTableUpdates();
  ensureLayoutUpToDate();
}

void SettingsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
  if (difficultyTableImportModalRoot != nullptr) {
    difficultyTableImportModalRoot->setSize(rendering::window_width,
                                            rendering::window_height);
  }
  if (previewActive && previewRenderer != nullptr) {
    previewRenderer->setVisibleTimeGreenNumber(
        context.settings.visibleTimeGreenNumber);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
    previewRenderer->setJudgementIndicatorConfig(
        context.settings.judgementIndicatorEnabled,
        context.settings.judgementIndicatorY,
        context.settings.judgementIndicatorWidthScale,
        context.settings.judgementIndicatorRenderMode ==
            AppSettings::JudgementIndicatorRenderMode::Hud2D);
    previewRenderer->refreshGeometry();
    RenderContext renderContext;
    previewRenderer->render(renderContext, previewElapsedMicros);
  }
}

void SettingsScene::cleanupScene() {
  if (difficultyTableJobThread.joinable()) {
    SDL_Log("Joining difficultyTableJobThread");
    difficultyTableJobThread.request_stop();
    difficultyTableJobThread.join();
  }
  pendingDeleteChartEntryPath.clear();
  difficultyTableImportModalVisible = false;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
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
  summaryNotePriorityValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  tablesTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
  difficultyTableImportModalRoot = nullptr;
  difficultyTableImportProgressFill = nullptr;
  difficultyTableImportTitleText = nullptr;
  difficultyTableImportStatusText = nullptr;
  difficultyTableImportTableText = nullptr;
  difficultyTableImportProgressText = nullptr;
  difficultyTableImportCloseButton = nullptr;
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}
