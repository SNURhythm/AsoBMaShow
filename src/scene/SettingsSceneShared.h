#include "../rendering/ResponsiveUi.h"
#pragma once
#include "../i18n/Localization.h"

#include "SettingsScene.h"
#include "SettingsPreviewChart.h"
#include "../context.h"
#include "../path.h"
#include "../rendering/Color.h"
#include "../view/Button.h"
#include "../view/ChoiceControlView.h"
#include "../view/ToggleButtonContent.h"
#include "../view/TabButtonStyle.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"
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
#include <string_view>
#include <vector>

namespace settings_scene {
// Let Yoga size the button from its content and horizontal padding.
inline constexpr int kFitContentWidth = -1;
inline constexpr const char *kFontPath = app_theme::font;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct LayoutMetrics {
  SafeAreaInsets safe;
  bool compact = false;
  bool flatSections = false;
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

inline SafeAreaInsets getSafeAreaInsetsUi() {
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

inline LayoutMetrics resolveLayoutMetrics() {
  LayoutMetrics metrics;
  metrics.safe = getSafeAreaInsetsUi();
  metrics.compact = rendering::window_height < 980 ||
                    rendering::window_width < rendering::window_height;
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

  if (rendering::compactMenuCanvasActive) {
    metrics.actionButtonHeight = rendering::compactTouchTarget;
    metrics.backButtonHeight = rendering::compactTouchTarget;
    metrics.bodyTextSize = 24;
    metrics.smallTextSize = 21;
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

inline bool sameColor(const Color &lhs, const Color &rhs) {
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

inline View::ThemeColorProvider themeTextColorProvider(const Color &color) {
  if (sameColor(color, app_theme::textPrimary())) {
    return app_theme::textPrimary;
  }
  if (sameColor(color, app_theme::textSecondary())) {
    return app_theme::textSecondary;
  }
  if (sameColor(color, app_theme::textMuted())) {
    return app_theme::textMuted;
  }
  if (sameColor(color, app_theme::cyan())) {
    return app_theme::cyan;
  }
  if (sameColor(color, app_theme::lime())) {
    return app_theme::lime;
  }
  if (sameColor(color, app_theme::amber())) {
    return app_theme::amber;
  }
  if (sameColor(color, app_theme::coral())) {
    return app_theme::coral;
  }
  return {};
}

inline View::ThemeColorProvider themeSurfaceColorProvider(const Color &color) {
  if (sameColor(color, app_theme::control())) {
    return app_theme::control;
  }
  if (sameColor(color, app_theme::controlHover())) {
    return app_theme::controlHover;
  }
  if (sameColor(color, app_theme::controlPressed())) {
    return app_theme::controlPressed;
  }
  if (sameColor(color, app_theme::panel())) {
    return app_theme::panel;
  }
  if (sameColor(color, app_theme::panelStrong())) {
    return app_theme::panelStrong;
  }
  if (sameColor(color, app_theme::panelSubtle())) {
    return app_theme::panelSubtle;
  }
  if (sameColor(color, app_theme::hairline())) {
    return app_theme::hairline;
  }
  if (sameColor(color, app_theme::hairlineSubtle())) {
    return app_theme::hairlineSubtle;
  }
  if (sameColor(color, app_theme::hairlineStrong())) {
    return app_theme::hairlineStrong;
  }
  if (sameColor(color, app_theme::accentBorder())) {
    return app_theme::accentBorder;
  }
  if (sameColor(color, app_theme::accentBorderStrong())) {
    return app_theme::accentBorderStrong;
  }
  if (sameColor(color, app_theme::cyan())) {
    return app_theme::cyan;
  }
  if (sameColor(color, app_theme::coral())) {
    return app_theme::coral;
  }
  if (sameColor(color, app_theme::lime())) {
    return app_theme::lime;
  }
  if (sameColor(color, app_theme::amber())) {
    return app_theme::amber;
  }
  return {};
}

inline TextView *makeText(const i18n::Text &text, int size, const Color &color,
                          TextView::TextAlign align = TextView::LEFT,
                          TextView::TextVAlign valign = TextView::TOP,
                          TextView::FontWeight weight = TextView::FontWeight::Regular) {
  auto *view = new TextView(weight == TextView::FontWeight::Bold
                                ? app_theme::boldFont : kFontPath, size, weight);
  view->setDeferredTextureMaterialization(true);
  view->setLocalizedText(text);
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

inline TextView *makeHeading(const i18n::Text &text, int size) {
  auto *view = makeText(text, size, app_theme::textPrimary(), TextView::LEFT,
                        TextView::TOP, TextView::FontWeight::Bold);
  view->setWrap(true);
  return view;
}

inline TextView *makeWrappedText(const i18n::Text &text, int size,
                                 const Color &color,
                                 TextView::TextAlign align = TextView::LEFT,
                                 TextView::TextVAlign valign = TextView::TOP) {
  auto *view = makeText(text, size, color, align, valign);
  view->setWrap(true);
  return view;
}

inline Button *makeButton(int width, int height, TextView *label,
                          const Color &normalBackground,
                          const Color &hoverBackground,
                          const Color &pressedBackground,
                          const Color &normalBorder, const Color &hoverBorder,
                          const Color &pressedBorder, int borderWidth = 1, bool cycle = false) {
  // Cycling toggles share content sizing; the requested width is for ordinary buttons.
  const bool fitContent = width == kFitContentWidth && !cycle;
  Button *button = cycle
      ? new toggle_button::CompactButton([label] {
          return label->measureTextWidth(label->getText()) + cycle_button::kContentInset;
        })
      : fitContent ? new Button() : new Button(0, 0, width, height);
  if (cycle) button->setHeight(height);
  label->setAlign(TextView::CENTER);
  label->setVAlign(TextView::MIDDLE);
  if (fitContent) {
    button->setHeight(height);
    button->setPadding(Edge::Left, 16.0f);
    button->setPadding(Edge::Right, 16.0f);
    button->setAlignSelf(YGAlignFlexStart);
    button->setAlignItems(YGAlignCenter);
    button->setJustifyContent(YGJustifyCenter);
    button->setFlexShrink(0.0f);
    button->addView(label);
  } else {
    button->setContentView(cycle ? cycle_button::makeContent(label) : label);
  }
  button->setCornerRadius(app_theme::controlRadius());
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
  if (auto *compact = dynamic_cast<toggle_button::CompactButton *>(button)) compact->refreshWidth();
  return button;
}

inline Button *makeControlButton(int width, int height, TextView *label, bool cycle = false) {
  auto *button = makeButton(
      width, height, label, app_theme::control(), app_theme::controlHover(),
      app_theme::controlPressed(), app_theme::hairline(),
      app_theme::accentBorder(), app_theme::accentBorderStrong(), 1, cycle);
  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairline, app_theme::accentBorder,
                                app_theme::accentBorderStrong);
  return button;
}

inline Button *makeAccentButton(int width, int height, TextView *label,
                                const Color &accent, bool cycle = false) {
  if (sameColor(accent, app_theme::cyan()) ||
      sameColor(accent, app_theme::lime())) {
    auto *button = makeControlButton(width, height, label, cycle);
    button->setThemedBackgroundColors(app_theme::action, app_theme::actionHover,
                                      app_theme::actionPressed);
    button->setThemedBorderColors(app_theme::action, app_theme::actionHover,
                                  app_theme::actionPressed);
    label->setThemedColor(app_theme::onAction);
    return button;
  }
  const auto accentProvider = themeSurfaceColorProvider(accent);
  const bool light = app_theme::activeMode() == app_theme::ThemeMode::Light;
  const uint8_t normalAlpha = light ? 54 : 82;
  const uint8_t hoverAlpha = light ? 74 : 108;
  const uint8_t pressedAlpha = light ? 100 : 136;
  auto *button = makeButton(width, height, label,
                            Color(accent.r, accent.g, accent.b, normalAlpha),
                            Color(accent.r, accent.g, accent.b, hoverAlpha),
                            Color(accent.r, accent.g, accent.b, pressedAlpha),
                            Color(accent.r, accent.g, accent.b, 178),
                            Color(accent.r, accent.g, accent.b, 216), accent, 1, cycle);
  if (accentProvider) {
    auto withModeAlpha = [accentProvider](uint8_t lightAlpha,
                                          uint8_t darkAlpha) {
      return [accentProvider, lightAlpha, darkAlpha]() {
        Color color = accentProvider();
        color.a = app_theme::activeMode() == app_theme::ThemeMode::Light
                      ? lightAlpha
                      : darkAlpha;
        return color;
      };
    };
    button->setThemedBackgroundColors(
        withModeAlpha(54, 82), withModeAlpha(74, 108), withModeAlpha(100, 136));
    button->setThemedBorderColors(
        [accentProvider]() {
          return app_theme::withAlpha(accentProvider(), 178);
        },
        [accentProvider]() {
          return app_theme::withAlpha(accentProvider(), 216);
        },
        accentProvider);
  }
  return button;
}

inline Button *makeStepButton(const LayoutMetrics &metrics, int width,
                              const i18n::Text &label) {
  return makeControlButton(width, metrics.actionButtonHeight,
                           makeText(label, metrics.bodyTextSize + 4,
                                    app_theme::textPrimary(), TextView::CENTER,
                                    TextView::MIDDLE));
}

inline Button *makeResetButton(const LayoutMetrics &metrics) {
  return makeAccentButton(metrics.resetButtonWidth, metrics.actionButtonHeight,
                          makeText(i18n::message("settings.options.reset.label"), metrics.bodyTextSize + 4,
                                   app_theme::textPrimary(), TextView::CENTER,
                                   TextView::MIDDLE),
                          app_theme::coral());
}

inline TextInputBox *makeNumericInput(const LayoutMetrics &metrics) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  input->setText("");
  input->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  input->setBackgroundColor(Color(0, 0, 0, 0));
  input->setBorderWidth(0);
  input->setAlign(TextView::CENTER);
  input->setVAlign(TextView::MIDDLE);
  input->setThemedColor(app_theme::textPrimary);
  return input;
}

inline TextInputBox *makeTextInput(const LayoutMetrics &metrics, int minWidth) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize);
  input->setText("");
  input->setSize(minWidth, metrics.actionButtonHeight);
  input->setThemedBackgroundColor(app_theme::control);
  input->setCornerRadius(app_theme::controlRadius());
  input->setThemedBorderColor(app_theme::hairline);
  input->setBorderWidth(1);
  input->setVAlign(TextView::MIDDLE);
  input->setThemedColor(app_theme::textPrimary);
  return input;
}

inline View *makeSteppedValueRow(const LayoutMetrics &metrics, TextInputBox *input,
                                  const std::string &stepLabel,
                                  std::function<void(int)> adjust, Button *reset) {
  auto *row = new View();
  row->setWidthPercent(100);
  row->setFlexDirection(FlexDirection::Row);
  row->setFlexWrap(YGWrapWrap);
  row->setJustifyContent(YGJustifyFlexStart);
  row->setAlignItems(YGAlignCenter);
  row->setGap(metrics.compact ? 8.0F : 10.0F);
  input->setAlign(TextView::CENTER);
  for (const int direction : {-1, 1}) {
    auto *step = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                (direction < 0 ? "-" : "+") + stepLabel);
    step->setOnClickListener([adjust, direction] { adjust(direction); });
    row->addView(step);
    if (direction < 0) row->addView(input);
  }
  row->addView(reset);
  return row;
}

inline View *makeInputFrame(const LayoutMetrics &metrics, TextInputBox *input) {
  auto *value = new View();
  value->setWidth(static_cast<float>(metrics.offsetValueWidth));
  value->setHeight(static_cast<float>(metrics.actionButtonHeight));
  value->setThemedBackgroundColor(app_theme::control);
  value->setCornerRadius(app_theme::controlRadius());
  value->setThemedBorderColor(app_theme::hairline);
  value->setBorderWidth(1);
  value->addView(input);
  return value;
}

inline View *makeCard(const LayoutMetrics &metrics, const i18n::Text &title,
                      const i18n::Text &description, View *body, int minHeight,
                      int width = 0) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column);
  card->setGap(metrics.cardGap);
  card->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  if (metrics.flatSections) {
    card->setName("settingsSection");
    card->setPadding(Edge::Left, 0)->setPadding(Edge::Right, 0);
    auto *rule = new View();
    rule->setPositionType(YGPositionTypeAbsolute);
    rule->setName("settingsSectionDivider");
    rule->setPosition(Edge::Left, 0)
        ->setPosition(Edge::Right, 0)
        ->setPosition(Edge::Top, 0);
    rule->setHeight(1);
    rule->setThemedBackgroundColor(app_theme::hairline);
    card->addView(rule);
  } else {
    card->setThemedBackgroundColor(app_theme::panel);
    card->setCornerRadius(app_theme::panelRadius());
    card->setThemedBorderColor(app_theme::hairline);
    card->setBorderWidth(1);
    card->setMinHeight(static_cast<float>(minHeight));
  }
  if (width > 0) {
    card->setWidth(static_cast<float>(width));
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setGap(metrics.compact ? 6.0f : 8.0f);
  auto *titleText =
      makeHeading(title, metrics.sectionTitleSize);
  header->addView(titleText);
  if (!description.empty()) {
    auto *descriptionText = makeWrappedText(description, metrics.bodyTextSize,
                                            app_theme::textSecondary());
    header->addView(descriptionText);
  }
  card->addView(header);
  card->addView(body);
  return card;
}

inline void refineSettingsSectionSpacing(View *column,
                                         const LayoutMetrics &metrics) {
  if (!metrics.flatSections) return;
  bool previousSection = false;
  for (auto *child : column->getChildren()) {
    const bool section = child->getName() == "settingsSection";
    if (section) {
      if (auto *divider = child->findViewByName("settingsSectionDivider")) {
        divider->setVisible(previousSection);
      }
      if (previousSection) child->setMargin(Edge::Top, metrics.cardPadding);
    }
    previousSection = section;
  }
}

inline View *makeSummaryRow(const LayoutMetrics &metrics,
                            const i18n::Text &label, TextView **valueOut) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifySpaceBetween);
  row->setAlignItems(YGAlignCenter);

  row->addView(
      makeText(label, metrics.summaryValueSize, app_theme::textSecondary()));
  auto *valueText = makeText("", metrics.summaryValueSize,
                             app_theme::textPrimary(), TextView::RIGHT);
  row->addView(valueText);
  if (valueOut != nullptr) {
    *valueOut = valueText;
  }
  return row;
}

inline int clampOffset(int value) {
  return std::clamp(value, AppSettings::kMinAudioOffsetMs,
                    AppSettings::kMaxAudioOffsetMs);
}

inline int clampVisualOffset(int value) {
  return std::clamp(value, AppSettings::kMinVisualOffsetMs,
                    AppSettings::kMaxVisualOffsetMs);
}

inline int clampBgaBrightness(int value) {
  return std::clamp(value, AppSettings::kMinBgaBrightnessPercent,
                    AppSettings::kMaxBgaBrightnessPercent);
}

inline float clampBgaBlur(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultBgaBlurStrength;
  }
  return std::clamp(value, AppSettings::kMinBgaBlurStrength,
                    AppSettings::kMaxBgaBlurStrength);
}

inline float clampLaneAngle(const AppSettings &settings, float value) {
  const auto range = settings.geometryPolicy().angle;
  return std::isfinite(value) ? std::clamp(value, range.minimum, range.maximum)
                              : range.defaultValue;
}

inline float clampLaneLength(const AppSettings &settings, float value) {
  const auto range = settings.geometryPolicy().length;
  return std::isfinite(value) ? std::clamp(value, range.minimum, range.maximum)
                              : range.defaultValue;
}

inline int clampLaneBeamLengthPercent(int value) {
  return std::clamp(value, AppSettings::kMinLaneBeamLengthPercent,
                    AppSettings::kMaxLaneBeamLengthPercent);
}

inline float clampNoteStartPositionPercent(float value) {
  return lane_cover::ratio(value / 100.0F) * 100.0F;
}

inline float clampPlayAreaWidth(const AppSettings &settings, float value) {
  const auto range = settings.geometryPolicy().width;
  return std::isfinite(value) ? std::clamp(value, range.minimum, range.maximum)
                              : range.defaultValue;
}

inline float clampJudgementIndicatorY(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorY;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorY,
                    AppSettings::kMaxJudgementIndicatorY);
}

inline int judgementIndicatorYToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorY(value) * 100.0f));
}

inline float judgementIndicatorPercentToY(int percent) {
  return clampJudgementIndicatorY(static_cast<float>(percent) / 100.0f);
}

inline int clampJudgementIndicatorRangeMilliseconds(int value) {
  return judgement_indicator::clampEditableRangeMilliseconds(value);
}

inline std::string formatJudgementIndicatorRangeLabel(int value) {
  return judgement_indicator::formatRangeLabel(value);
}

inline float clampJudgementTextY(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementTextY;
  }
  return std::clamp(value, AppSettings::kMinJudgementTextY,
                    AppSettings::kMaxJudgementTextY);
}

inline int judgementTextYToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementTextY(value) * 100.0f));
}

inline float judgementTextPercentToY(int percent) {
  return clampJudgementTextY(static_cast<float>(percent) / 100.0f);
}

inline float clampJudgementIndicatorWidthScale(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultJudgementIndicatorWidthScale;
  }
  return std::clamp(value, AppSettings::kMinJudgementIndicatorWidthScale,
                    AppSettings::kMaxJudgementIndicatorWidthScale);
}

inline int judgementIndicatorWidthScaleToPercent(float value) {
  return static_cast<int>(
      std::lround(clampJudgementIndicatorWidthScale(value) * 100.0f));
}

inline float judgementIndicatorWidthPercentToScale(int percent) {
  return clampJudgementIndicatorWidthScale(static_cast<float>(percent) /
                                           100.0f);
}

inline int adjustVisibleTimeDurationMilliseconds(int currentMilliseconds,
                                                 bool useMilliseconds,
                                                 int delta) {
  if (useMilliseconds) {
    return std::clamp(currentMilliseconds + delta,
                      AppSettings::kMinVisibleTimeMs,
                      AppSettings::kMaxVisibleTimeMs);
  }
  return AppSettings::greenNumberToDurationMilliseconds(
      AppSettings::durationMillisecondsToGreenNumber(currentMilliseconds) +
      delta);
}

inline std::string formatOffsetLabel(int offsetMs) {
  return (offsetMs > 0 ? "+" : "") + std::to_string(offsetMs) + " ms";
}

inline std::string formatOffsetInputValue(int offsetMs) {
  return std::to_string(offsetMs);
}

inline std::string formatVisibleTimeLabel(int milliseconds,
                                          bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(milliseconds) + " ms";
  }
  return std::to_string(
             AppSettings::durationMillisecondsToGreenNumber(milliseconds)) +
         " green";
}

inline std::string formatVisibleTimeInputValue(int milliseconds,
                                               bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(milliseconds);
  }
  return std::to_string(
      AppSettings::durationMillisecondsToGreenNumber(milliseconds));
}

inline i18n::Text formatVisibleTimeBpmStrategyLabel(
    AppSettings::HiSpeedFixMode mode) {
  switch (mode) {
  case AppSettings::HiSpeedFixMode::Off:
    return i18n::message("settings.options.off.label");
  case AppSettings::HiSpeedFixMode::Start:
    return i18n::message("settings.options.start_bpm.label");
  case AppSettings::HiSpeedFixMode::Max:
    return i18n::message("settings.options.max_bpm.label");
  case AppSettings::HiSpeedFixMode::Main:
    return i18n::message("settings.options.main_bpm.label");
  case AppSettings::HiSpeedFixMode::Min:
    return i18n::message("settings.options.min_bpm.label");
  }
  return i18n::message("settings.options.main_bpm.label");
}

inline std::string formatFloatValue(float value, int precision = 1) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

inline std::string formatBgaBrightnessLabel(int percent) {
  return std::to_string(percent) + "%";
}

inline std::string formatBgaBlurLabel(float strength) {
  return formatFloatValue(strength, 1);
}

inline std::string formatLaneAngleLabel(float degrees) {
  return formatFloatValue(degrees, 1) + " deg";
}

inline std::string formatLaneLengthLabel(float length) {
  return formatFloatValue(length, 1);
}

inline std::string formatLaneBeamLengthLabel(int percent) {
  return std::to_string(clampLaneBeamLengthPercent(percent)) + "%";
}

inline std::string formatNoteStartPositionLabel(float percent) {
  std::ostringstream text;
  text << clampNoteStartPositionPercent(percent) << '%';
  return text.str();
}

inline std::string formatJudgementPercentLabel(int percent) {
  return std::to_string(std::clamp(percent, 0, 100)) + "%";
}

inline std::string formatPlayAreaWidthLabel(float width) {
  return formatFloatValue(width, 1);
}

inline i18n::Text formatJudgementIndicatorRenderModeLabel(
    AppSettings::JudgementIndicatorRenderMode mode) {
  switch (mode) {
  case AppSettings::JudgementIndicatorRenderMode::World3D:
    return i18n::message("settings.options.hud.position.world.label");
  case AppSettings::JudgementIndicatorRenderMode::Hud2D:
    return i18n::message("settings.options.hud.position.overlay.label");
  }
  return i18n::message("settings.options.hud.position.world.label");
}

inline i18n::Text formatJudgementCounterPositionLabel(
    AppSettings::JudgementCounterPosition position) {
  switch (position) {
  case AppSettings::JudgementCounterPosition::Top:
    return i18n::message("settings.options.top.label");
  case AppSettings::JudgementCounterPosition::Left:
    return i18n::message("settings.options.left.label");
  case AppSettings::JudgementCounterPosition::Right:
    return i18n::message("settings.options.right.label");
  }
  return i18n::message("settings.options.top.label");
}

inline i18n::Text formatJudgementTimingDisplayCriteriaLabel(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow:
    return i18n::message("settings.options.pgreat_below.badge");
  case AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow:
    return i18n::message("settings.options.great_below.badge");
  case AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow:
    return i18n::message("settings.options.good_below.badge");
  case AppSettings::JudgementTimingDisplayCriteria::BadOrBelow:
    return i18n::message("settings.options.bad_below.badge");
  case AppSettings::JudgementTimingDisplayCriteria::Off:
    return "OFF";
  }
  return i18n::message("settings.options.great_below.badge");
}

inline i18n::Text
formatGaugeBarPositionLabel(AppSettings::GaugeBarPosition position) {
  switch (position) {
  case AppSettings::GaugeBarPosition::World:
    return i18n::message("settings.options.world.label");
  case AppSettings::GaugeBarPosition::Left:
    return i18n::message("settings.options.left_hud.label");
  case AppSettings::GaugeBarPosition::Right:
    return i18n::message("settings.options.right_hud.label");
  }
  return i18n::message("settings.options.world.label");
}

inline i18n::Text formatBgaDisplayModeLabel(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return i18n::message("settings.options.fit.label");
  case AppSettings::BgaDisplayMode::Fill:
    return i18n::message("settings.options.fill.label");
  case AppSettings::BgaDisplayMode::Stretch:
    return i18n::message("settings.options.stretch.label");
  case AppSettings::BgaDisplayMode::NoExpand:
    return i18n::message("settings.options.no_expansion.label");
  }
  return i18n::message("settings.options.fit.label");
}

inline i18n::Text
formatNotePriorityModeLabel(AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Lowest:
    return i18n::message("settings.options.lowest.label");
  case AppSettings::NotePriorityMode::Combo:
    return i18n::message("settings.options.combo.label");
  case AppSettings::NotePriorityMode::Duration:
    return i18n::message("settings.options.duration.label");
  case AppSettings::NotePriorityMode::Score:
    return i18n::message("settings.options.score.label");
  }
  return i18n::message("settings.options.lowest.label");
}

inline i18n::Text formatUiThemeModeLabel(AppSettings::UiThemeMode mode) {
  switch (mode) {
  case AppSettings::UiThemeMode::Dark:
    return i18n::message("settings.options.dark.label");
  case AppSettings::UiThemeMode::Light:
    return i18n::message("settings.options.light.label");
  }
  return i18n::message("settings.options.dark.label");
}

inline i18n::Text formatTableCount(int chartCount) {
  return i18n::message(chartCount == 1 ? "settings.options.chart_count.one" : "settings.options.chart_count.other",
                      {{"count", std::to_string(chartCount)}});
}

inline i18n::Text formatTableSource(const std::string &sourceUrl) {
  if (sourceUrl.empty()) {
    return i18n::message("settings.options.no_source_url.label");
  }
  return sourceUrl;
}

inline std::string formatChartEntryPath(const ChartEntry &entry) {
  return path_t_to_utf8(entry.path);
}

inline i18n::Text formatChartEntryName(const ChartEntry &entry) {
  if (!entry.removable) {
    return i18n::message("settings.options.default_bms.label");
  }
  const std::filesystem::path path(entry.path);
  const std::filesystem::path name = path.filename();
  if (name.empty()) {
    return formatChartEntryPath(entry);
  }
  return fspath_to_utf8(name);
}

inline i18n::Text formatChartEntrySource(const ChartEntry &entry) {
  const std::string pathText = formatChartEntryPath(entry);
  if (!entry.removable) {
    return i18n::message("settings.options.chart_entry.source",
                         {{"path", pathText},
                          {"notice", i18n::message("settings.options.folder.builtin_download_fallback_notice")}});
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (!entry.iosBookmark.empty()) {
    return i18n::message("settings.options.chart_entry.source",
                         {{"path", pathText},
                          {"notice", i18n::message("settings.options.folder.access_saved_notice")}});
  }
#endif
  return pathText;
}

inline i18n::Text formatImportProgressText(int current, int total) {
  if (total <= 0) {
    return i18n::message("settings.options.preparing.label");
  }
  const int safeCurrent = std::clamp(current, 0, total);
  return i18n::message(total == 1 ? "settings.options.import.progress.one"
                                 : "settings.options.import.progress.other",
                      {{"current", std::to_string(safeCurrent)},
                       {"total", std::to_string(total)}});
}

inline AppSettings::BgaDisplayMode
nextBgaDisplayMode(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return AppSettings::BgaDisplayMode::Fill;
  case AppSettings::BgaDisplayMode::Fill:
    return AppSettings::BgaDisplayMode::Stretch;
  case AppSettings::BgaDisplayMode::Stretch:
  case AppSettings::BgaDisplayMode::NoExpand:
    return AppSettings::BgaDisplayMode::Fit;
  }
  return AppSettings::BgaDisplayMode::Fit;
}

inline AppSettings::NotePriorityMode
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

inline AppSettings::JudgementIndicatorRenderMode
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

inline AppSettings::JudgementCounterPosition
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

inline AppSettings::JudgementTimingDisplayCriteria
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

inline AppSettings::GaugeBarPosition
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

inline AppSettings::UiThemeMode nextUiThemeMode(AppSettings::UiThemeMode mode) {
  switch (mode) {
  case AppSettings::UiThemeMode::Dark:
    return AppSettings::UiThemeMode::Light;
  case AppSettings::UiThemeMode::Light:
    return AppSettings::UiThemeMode::Dark;
  }
  return AppSettings::UiThemeMode::Dark;
}

} // namespace settings_scene

namespace settings_scene {

struct GameplaySkinChoiceButton {
  i18n::Text label;
  bool selected = false;
  std::function<void()> action;
  std::function<bool()> tryAction;
};

void styleGameplaySkinChoiceButton(Button *button, bool selected);
View *makeGameplaySkinChoiceRow(const LayoutMetrics &metrics, OverlayPortal *portal,
                                const i18n::Text &label, bool enabled,
                                std::vector<GameplaySkinChoiceButton> choices, bool oppositeStates = false,
                                size_t activeChoice = 1);
template <typename Enum, typename Format, typename Apply>
ChoiceControlView *makeEnumChoice(const LayoutMetrics &metrics, OverlayPortal *portal,
                                 Enum selected, std::initializer_list<Enum> values,
                                 Format format, Apply apply) {
  auto *control = new ChoiceControlView(
      [apply](const std::string &id) { apply(static_cast<Enum>(std::stoi(id))); return true; },
      portal, metrics.smallTextSize, metrics.actionButtonHeight);
  std::vector<DropdownView::Option> options;
  for (const auto value : values)
    options.push_back({.id = std::to_string(static_cast<int>(value)), .label = format(value)});
  if (std::find(values.begin(), values.end(), selected) == values.end())
    options.push_back({.id = std::to_string(static_cast<int>(selected)),
                       .label = format(selected), .available = false});
  control->refresh({.selectedId = std::to_string(static_cast<int>(selected)),
                    .options = std::move(options)});
  return control;
}

int sanitizeOffsetComponent(std::string_view text, int fallback);

} // namespace settings_scene
