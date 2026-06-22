#pragma once

#include "SettingsScene.h"
#include "../context.h"
#include "../path.h"
#include "../rendering/Color.h"
#include "../view/Button.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "../view/View.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

namespace settings_scene {
inline constexpr long long kPreviewLoopMicros = 8000000LL;
inline constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

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

static SafeAreaInsets getSafeAreaInsetsUi() {
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

static LayoutMetrics resolveLayoutMetrics() {
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

static bool sameColor(const Color &lhs, const Color &rhs) {
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

static View::ThemeColorProvider themeTextColorProvider(const Color &color) {
  if (sameColor(color, ui_theme::textPrimary())) {
    return ui_theme::textPrimary;
  }
  if (sameColor(color, ui_theme::textSecondary())) {
    return ui_theme::textSecondary;
  }
  if (sameColor(color, ui_theme::textMuted())) {
    return ui_theme::textMuted;
  }
  if (sameColor(color, ui_theme::cyan())) {
    return ui_theme::cyan;
  }
  if (sameColor(color, ui_theme::lime())) {
    return ui_theme::lime;
  }
  if (sameColor(color, ui_theme::amber())) {
    return ui_theme::amber;
  }
  if (sameColor(color, ui_theme::coral())) {
    return ui_theme::coral;
  }
  return {};
}

static View::ThemeColorProvider themeSurfaceColorProvider(const Color &color) {
  if (sameColor(color, ui_theme::control())) {
    return ui_theme::control;
  }
  if (sameColor(color, ui_theme::controlHover())) {
    return ui_theme::controlHover;
  }
  if (sameColor(color, ui_theme::controlPressed())) {
    return ui_theme::controlPressed;
  }
  if (sameColor(color, ui_theme::panel())) {
    return ui_theme::panel;
  }
  if (sameColor(color, ui_theme::panelStrong())) {
    return ui_theme::panelStrong;
  }
  if (sameColor(color, ui_theme::panelSubtle())) {
    return ui_theme::panelSubtle;
  }
  if (sameColor(color, ui_theme::hairline())) {
    return ui_theme::hairline;
  }
  if (sameColor(color, ui_theme::hairlineSubtle())) {
    return ui_theme::hairlineSubtle;
  }
  if (sameColor(color, ui_theme::hairlineStrong())) {
    return ui_theme::hairlineStrong;
  }
  if (sameColor(color, ui_theme::accentBorder())) {
    return ui_theme::accentBorder;
  }
  if (sameColor(color, ui_theme::accentBorderStrong())) {
    return ui_theme::accentBorderStrong;
  }
  if (sameColor(color, ui_theme::cyan())) {
    return ui_theme::cyan;
  }
  if (sameColor(color, ui_theme::coral())) {
    return ui_theme::coral;
  }
  if (sameColor(color, ui_theme::lime())) {
    return ui_theme::lime;
  }
  if (sameColor(color, ui_theme::amber())) {
    return ui_theme::amber;
  }
  return {};
}

static TextView *makeText(const std::string &text, int size, const Color &color,
                          TextView::TextAlign align = TextView::LEFT,
                          TextView::TextVAlign valign = TextView::TOP) {
  auto *view = new TextView(kFontPath, size);
  view->setText(text);
  const auto themedProvider = themeTextColorProvider(color);
  if (themedProvider) {
    view->setThemedColor(themedProvider);
  } else {
    view->setColor({color.r, color.g, color.b, color.a});
  }
  view->setAlign(align);
  view->setVAlign(valign);
  return view;
}

static TextView *makeWrappedText(const std::string &text, int size,
                                 const Color &color,
                                 TextView::TextAlign align = TextView::LEFT,
                                 TextView::TextVAlign valign = TextView::TOP) {
  auto *view = makeText(text, size, color, align, valign);
  view->setWrap(true);
  return view;
}

static Button *makeButton(int width, int height, TextView *label,
                          const Color &normalBackground,
                          const Color &hoverBackground,
                          const Color &pressedBackground,
                          const Color &normalBorder, const Color &hoverBorder,
                          const Color &pressedBorder, int borderWidth = 1) {
  auto *button = new Button(0, 0, width, height);
  label->setAlign(TextView::CENTER);
  label->setVAlign(TextView::MIDDLE);
  button->setContentView(label);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setBackgroundColors(normalBackground, hoverBackground,
                              pressedBackground);
  button->setBorderColors(normalBorder, hoverBorder, pressedBorder);
  const auto normalBackgroundProvider =
      themeSurfaceColorProvider(normalBackground);
  const auto hoverBackgroundProvider =
      themeSurfaceColorProvider(hoverBackground);
  const auto pressedBackgroundProvider =
      themeSurfaceColorProvider(pressedBackground);
  if (normalBackgroundProvider && hoverBackgroundProvider &&
      pressedBackgroundProvider) {
    button->setThemedBackgroundColors(normalBackgroundProvider,
                                      hoverBackgroundProvider,
                                      pressedBackgroundProvider);
  }
  const auto normalBorderProvider = themeSurfaceColorProvider(normalBorder);
  const auto hoverBorderProvider = themeSurfaceColorProvider(hoverBorder);
  const auto pressedBorderProvider = themeSurfaceColorProvider(pressedBorder);
  if (normalBorderProvider && hoverBorderProvider && pressedBorderProvider) {
    button->setThemedBorderColors(normalBorderProvider, hoverBorderProvider,
                                  pressedBorderProvider);
  }
  button->setStyledBorderWidth(borderWidth);
  return button;
}

static Button *makeControlButton(int width, int height, TextView *label) {
  auto *button = makeButton(
      width, height, label, ui_theme::control(), ui_theme::controlHover(),
      ui_theme::controlPressed(), ui_theme::hairline(),
      ui_theme::accentBorder(), ui_theme::accentBorderStrong());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairline, ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  return button;
}

static Button *makeAccentButton(int width, int height, TextView *label,
                                const Color &accent) {
  const auto accentProvider = themeSurfaceColorProvider(accent);
  const bool light = ui_theme::activeMode() == ui_theme::ThemeMode::Light;
  const uint8_t normalAlpha = light ? 54 : 82;
  const uint8_t hoverAlpha = light ? 74 : 108;
  const uint8_t pressedAlpha = light ? 100 : 136;
  auto *button = makeButton(width, height, label,
                            Color(accent.r, accent.g, accent.b, normalAlpha),
                            Color(accent.r, accent.g, accent.b, hoverAlpha),
                            Color(accent.r, accent.g, accent.b, pressedAlpha),
                            Color(accent.r, accent.g, accent.b, 178),
                            Color(accent.r, accent.g, accent.b, 216), accent);
  if (accentProvider) {
    auto withModeAlpha = [accentProvider](uint8_t lightAlpha,
                                          uint8_t darkAlpha) {
      return [accentProvider, lightAlpha, darkAlpha]() {
        Color color = accentProvider();
        color.a = ui_theme::activeMode() == ui_theme::ThemeMode::Light
                      ? lightAlpha
                      : darkAlpha;
        return color;
      };
    };
    button->setThemedBackgroundColors(
        withModeAlpha(54, 82), withModeAlpha(74, 108), withModeAlpha(100, 136));
    button->setThemedBorderColors(
        [accentProvider]() {
          return ui_theme::withAlpha(accentProvider(), 178);
        },
        [accentProvider]() {
          return ui_theme::withAlpha(accentProvider(), 216);
        },
        accentProvider);
  }
  return button;
}

static Button *makeStepButton(const LayoutMetrics &metrics, int width,
                              const std::string &label) {
  return makeControlButton(width, metrics.actionButtonHeight,
                           makeText(label, metrics.bodyTextSize + 4,
                                    ui_theme::textPrimary(), TextView::CENTER,
                                    TextView::MIDDLE));
}

static Button *makeResetButton(const LayoutMetrics &metrics) {
  return makeAccentButton(metrics.resetButtonWidth, metrics.actionButtonHeight,
                          makeText("Reset", metrics.bodyTextSize + 4,
                                   ui_theme::textPrimary(), TextView::CENTER,
                                   TextView::MIDDLE),
                          ui_theme::coral());
}

static TextInputBox *makeNumericInput(const LayoutMetrics &metrics) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  input->setText("");
  input->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  input->setBackgroundColor(Color(0, 0, 0, 0));
  input->setBorderWidth(0);
  input->setAlign(TextView::CENTER);
  input->setVAlign(TextView::MIDDLE);
  input->setThemedColor(ui_theme::textPrimary);
  return input;
}

static TextInputBox *makeTextInput(const LayoutMetrics &metrics, int minWidth) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize);
  input->setText("");
  input->setSize(minWidth, metrics.actionButtonHeight);
  input->setThemedBackgroundColor(ui_theme::control);
  input->setCornerRadius(ui_theme::controlRadius());
  input->setThemedBorderColor(ui_theme::hairline);
  input->setBorderWidth(1);
  input->setVAlign(TextView::MIDDLE);
  input->setThemedColor(ui_theme::textPrimary);
  return input;
}

static View *makeInputFrame(const LayoutMetrics &metrics, TextInputBox *input) {
  auto *value = new View();
  value->setWidth(static_cast<float>(metrics.offsetValueWidth));
  value->setHeight(static_cast<float>(metrics.actionButtonHeight));
  value->setThemedBackgroundColor(ui_theme::control);
  value->setCornerRadius(ui_theme::controlRadius());
  value->setThemedBorderColor(ui_theme::hairline);
  value->setBorderWidth(1);
  value->addView(input);
  return value;
}

static View *makeCard(const LayoutMetrics &metrics, const std::string &title,
                      const std::string &description, View *body, int minHeight,
                      int width = 0) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column);
  card->setGap(metrics.cardGap);
  card->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  card->setThemedBackgroundColor(ui_theme::panel);
  card->setCornerRadius(ui_theme::panelRadius());
  card->setThemedShadow(ui_theme::cardShadow, ui_theme::kCardShadow);
  card->setThemedBorderColor(ui_theme::hairline);
  card->setBorderWidth(1);
  card->setMinHeight(static_cast<float>(minHeight));
  if (width > 0) {
    card->setWidth(static_cast<float>(width));
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setGap(metrics.compact ? 6.0f : 8.0f);
  auto *titleText =
      makeWrappedText(title, metrics.sectionTitleSize, ui_theme::textPrimary());
  header->addView(titleText);
  auto *descriptionText = makeWrappedText(description, metrics.bodyTextSize,
                                          ui_theme::textSecondary());
  header->addView(descriptionText);
  card->addView(header);
  card->addView(body);
  return card;
}

static View *makeSummaryRow(const LayoutMetrics &metrics,
                            const std::string &label, TextView **valueOut) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifySpaceBetween);
  row->setAlignItems(YGAlignCenter);

  row->addView(
      makeText(label, metrics.summaryValueSize, ui_theme::textSecondary()));
  auto *valueText = makeText("", metrics.summaryValueSize,
                             ui_theme::textPrimary(), TextView::RIGHT);
  row->addView(valueText);
  if (valueOut != nullptr) {
    *valueOut = valueText;
  }
  return row;
}

static int clampOffset(int value) {
  return std::clamp(value, AppSettings::kMinAudioOffsetMs,
                    AppSettings::kMaxAudioOffsetMs);
}

static int clampVisualOffset(int value) {
  return std::clamp(value, AppSettings::kMinVisualOffsetMs,
                    AppSettings::kMaxVisualOffsetMs);
}

static int clampVisibleTimeGreenNumber(int value) {
  return std::clamp(value, AppSettings::kMinVisibleTimeGreenNumber,
                    AppSettings::kMaxVisibleTimeGreenNumber);
}

static int clampBgaBrightness(int value) {
  return std::clamp(value, AppSettings::kMinBgaBrightnessPercent,
                    AppSettings::kMaxBgaBrightnessPercent);
}

static float clampBgaBlur(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultBgaBlurStrength;
  }
  return std::clamp(value, AppSettings::kMinBgaBlurStrength,
                    AppSettings::kMaxBgaBlurStrength);
}

static float clampLaneAngle(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneAngleDegrees;
  }
  return std::clamp(value, AppSettings::kMinLaneAngleDegrees,
                    AppSettings::kMaxLaneAngleDegrees);
}

static float clampLaneLength(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneLength;
  }
  return std::clamp(value, AppSettings::kMinLaneLength,
                    AppSettings::kMaxLaneLength);
}

static int clampLaneBeamLengthPercent(int value) {
  return std::clamp(value, AppSettings::kMinLaneBeamLengthPercent,
                    AppSettings::kMaxLaneBeamLengthPercent);
}

static int clampNoteStartPositionPercent(int value) {
  return std::clamp(value, AppSettings::kMinNoteStartPositionPercent,
                    AppSettings::kMaxNoteStartPositionPercent);
}

static float clampPlayAreaWidth(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultPlayAreaWidth;
  }
  return std::clamp(value, AppSettings::kMinPlayAreaWidth,
                    AppSettings::kMaxPlayAreaWidth);
}

static float clampJudgementIndicatorY(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorY;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorY,
                    AppSettings::kMaxJudgementIndicatorY);
}

static int judgementIndicatorYToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorY(value) * 100.0f));
}

static float judgementIndicatorPercentToY(int percent) {
  return clampJudgementIndicatorY(static_cast<float>(percent) / 100.0f);
}

static float clampJudgementTextY(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementTextY;
  }
  return std::clamp(value, AppSettings::kMinJudgementTextY,
                    AppSettings::kMaxJudgementTextY);
}

static int judgementTextYToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementTextY(value) * 100.0f));
}

static float judgementTextPercentToY(int percent) {
  return clampJudgementTextY(static_cast<float>(percent) / 100.0f);
}

static float clampJudgementIndicatorWidthScale(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorWidthScale;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorWidthScale,
                    AppSettings::kMaxJudgementIndicatorWidthScale);
}

static int judgementIndicatorWidthScaleToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorWidthScale(value) * 100.0f));
}

static float judgementIndicatorWidthPercentToScale(int percent) {
  return clampJudgementIndicatorWidthScale(static_cast<float>(percent) /
                                           100.0f);
}

static int greenNumberToMilliseconds(int greenNumber) {
  return static_cast<int>(
      std::lround(static_cast<double>(greenNumber) * 1000.0 / 600.0));
}

static int millisecondsToGreenNumber(int milliseconds) {
  return static_cast<int>(
      std::lround(static_cast<double>(milliseconds) * 600.0 / 1000.0));
}

static int adjustVisibleTimeGreenNumber(int currentGreenNumber,
                                        bool useMilliseconds, int delta) {
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

static std::string formatOffsetLabel(int offsetMs) {
  return (offsetMs > 0 ? "+" : "") + std::to_string(offsetMs) + " ms";
}

static std::string formatOffsetInputValue(int offsetMs) {
  return std::to_string(offsetMs);
}

static std::string formatVisibleTimeLabel(int greenNumber,
                                          bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber)) + " ms";
  }
  return std::to_string(greenNumber) + " green";
}

static std::string formatVisibleTimeInputValue(int greenNumber,
                                               bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber));
  }
  return std::to_string(greenNumber);
}

static std::string formatVisibleTimeBpmStrategyLabel(
    AppSettings::VisibleTimeBpmStrategy strategy) {
  switch (strategy) {
  case AppSettings::VisibleTimeBpmStrategy::Chart:
    return "Chart BPM";
  case AppSettings::VisibleTimeBpmStrategy::MostPrevalent:
    return "Most prevalent";
  }
  return "Chart BPM";
}

static std::string formatFloatValue(float value, int precision = 1) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

static std::string formatBgaBrightnessLabel(int percent) {
  return std::to_string(percent) + "%";
}

static std::string formatBgaBlurLabel(float strength) {
  return formatFloatValue(strength, 1);
}

static std::string formatLaneAngleLabel(float degrees) {
  return formatFloatValue(degrees, 1) + " deg";
}

static std::string formatLaneLengthLabel(float length) {
  return formatFloatValue(length, 1);
}

static std::string formatLaneBeamLengthLabel(int percent) {
  return std::to_string(clampLaneBeamLengthPercent(percent)) + "%";
}

static std::string formatNoteStartPositionLabel(int percent) {
  return std::to_string(clampNoteStartPositionPercent(percent)) + "%";
}

static std::string formatJudgementPercentLabel(int percent) {
  return std::to_string(std::clamp(percent, 0, 100)) + "%";
}

static std::string formatPlayAreaWidthLabel(float width) {
  return formatFloatValue(clampPlayAreaWidth(width), 1);
}

static std::string formatJudgementIndicatorRenderModeLabel(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return "3D Space";
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return "2D HUD";
  }
  return "3D Space";
}

static std::string formatJudgementCounterPositionLabel(
    AppSettings::JudgementCounterPosition position) {
  switch (position) {
  case AppSettings::JudgementCounterPosition::Top:
    return "Top";
  case AppSettings::JudgementCounterPosition::Left:
    return "Left";
  case AppSettings::JudgementCounterPosition::Right:
    return "Right";
  }
  return "Top";
}

static std::string formatJudgementTimingDisplayCriteriaLabel(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow:
    return "PGREAT OR BELOW";
  case AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow:
    return "GREAT OR BELOW";
  case AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow:
    return "GOOD OR BELOW";
  case AppSettings::JudgementTimingDisplayCriteria::BadOrBelow:
    return "BAD OR BELOW";
  case AppSettings::JudgementTimingDisplayCriteria::Off:
    return "OFF";
  }
  return "GREAT OR BELOW";
}

static std::string
formatGaugeBarPositionLabel(AppSettings::GaugeBarPosition position) {
  switch (position) {
  case AppSettings::GaugeBarPosition::World:
    return "World";
  case AppSettings::GaugeBarPosition::Left:
    return "Left HUD";
  case AppSettings::GaugeBarPosition::Right:
    return "Right HUD";
  }
  return "World";
}

static std::string formatBgaDisplayModeLabel(AppSettings::BgaDisplayMode mode) {
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

static std::string
formatNotePriorityModeLabel(AppSettings::NotePriorityMode mode) {
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

static std::string formatUiThemeModeLabel(AppSettings::UiThemeMode mode) {
  switch (mode) {
  case AppSettings::UiThemeMode::Dark:
    return "Dark";
  case AppSettings::UiThemeMode::Light:
    return "Light";
  }
  return "Dark";
}

static std::string formatTableCount(int chartCount) {
  return std::to_string(chartCount) + (chartCount == 1 ? " chart" : " charts");
}

static std::string formatTableSource(const std::string &sourceUrl) {
  if (sourceUrl.empty()) {
    return "No source URL";
  }
  return sourceUrl;
}

static std::string formatChartEntryPath(const ChartEntry &entry) {
  return path_t_to_utf8(entry.path);
}

static std::string formatChartEntryName(const ChartEntry &entry) {
  const std::filesystem::path path(entry.path);
  const std::filesystem::path name = path.filename();
  if (name.empty()) {
    return formatChartEntryPath(entry);
  }
  return path_t_to_utf8(fspath_to_path_t(name));
}

static std::string formatChartEntrySource(const ChartEntry &entry) {
  const std::string pathText = formatChartEntryPath(entry);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (!entry.iosBookmark.empty()) {
    return pathText + "\nFiles access saved for future scans.";
  }
#endif
  return pathText;
}

static std::string formatImportProgressText(int current, int total) {
  if (total <= 0) {
    return "Preparing";
  }
  const int safeCurrent = std::clamp(current, 0, total);
  return std::to_string(safeCurrent) + " / " + std::to_string(total) +
         (total == 1 ? " table" : " tables");
}

static AppSettings::BgaDisplayMode
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

static AppSettings::NotePriorityMode
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

static AppSettings::VisibleTimeBpmStrategy
nextVisibleTimeBpmStrategy(AppSettings::VisibleTimeBpmStrategy strategy) {
  switch (strategy) {
  case AppSettings::VisibleTimeBpmStrategy::Chart:
    return AppSettings::VisibleTimeBpmStrategy::MostPrevalent;
  case AppSettings::VisibleTimeBpmStrategy::MostPrevalent:
    return AppSettings::VisibleTimeBpmStrategy::Chart;
  }
  return AppSettings::VisibleTimeBpmStrategy::Chart;
}

static AppSettings::JudgementIndicatorRenderMode
nextJudgementIndicatorRenderMode(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return AppSettings::JudgementIndicatorRenderMode::Hud2D;
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return AppSettings::JudgementIndicatorRenderMode::World3D;
  }
  return AppSettings::JudgementIndicatorRenderMode::World3D;
}

static AppSettings::JudgementCounterPosition
nextJudgementCounterPosition(
    AppSettings::JudgementCounterPosition position) {
  switch (position) {
  case AppSettings::JudgementCounterPosition::Top:
    return AppSettings::JudgementCounterPosition::Left;
  case AppSettings::JudgementCounterPosition::Left:
    return AppSettings::JudgementCounterPosition::Right;
  case AppSettings::JudgementCounterPosition::Right:
    return AppSettings::JudgementCounterPosition::Top;
  }
  return AppSettings::JudgementCounterPosition::Top;
}

static AppSettings::JudgementTimingDisplayCriteria
nextJudgementTimingDisplayCriteria(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow:
    return AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow;
  case AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow:
    return AppSettings::JudgementTimingDisplayCriteria::BadOrBelow;
  case AppSettings::JudgementTimingDisplayCriteria::BadOrBelow:
    return AppSettings::JudgementTimingDisplayCriteria::Off;
  case AppSettings::JudgementTimingDisplayCriteria::Off:
    return AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow;
  case AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow:
    return AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
  }
  return AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow;
}

static AppSettings::GaugeBarPosition
nextGaugeBarPosition(AppSettings::GaugeBarPosition position) {
  switch (position) {
  case AppSettings::GaugeBarPosition::World:
    return AppSettings::GaugeBarPosition::Left;
  case AppSettings::GaugeBarPosition::Left:
    return AppSettings::GaugeBarPosition::Right;
  case AppSettings::GaugeBarPosition::Right:
    return AppSettings::GaugeBarPosition::World;
  }
  return AppSettings::GaugeBarPosition::World;
}

static AppSettings::UiThemeMode nextUiThemeMode(AppSettings::UiThemeMode mode) {
  switch (mode) {
  case AppSettings::UiThemeMode::Dark:
    return AppSettings::UiThemeMode::Light;
  case AppSettings::UiThemeMode::Light:
    return AppSettings::UiThemeMode::Dark;
  }
  return AppSettings::UiThemeMode::Dark;
}

} // namespace settings_scene
