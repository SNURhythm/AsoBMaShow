#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "../view/ScrollView.h"
#include "../view/DropdownView.h"
#include "play/StartLaneIndicatorGeometry.h"
#include "../view/UiTheme.h"
#include "../settings/BuiltInNoteEditing.h"
#include "../settings/BuiltInScratchGradient.h"

#include <charconv>

using namespace settings_scene;

namespace {
View *makeAppearanceColorPresets(const LayoutMetrics &metrics, std::uint32_t defaultColor,
                                 std::optional<std::uint32_t> selectedColor,
                                 std::function<void(std::uint32_t)> apply) {
  auto *swatches = new View();
  swatches->setFlexDirection(FlexDirection::Row);
  swatches->setFlexWrap(YGWrapWrap);
  swatches->setGap(6.0F);
  for (const auto rgb : built_in_notes::colorPresets(defaultColor)) {
    const Color color(0xFF000000U | rgb);
    auto *button = makeButton(78, metrics.actionButtonHeight,
        makeText(built_in_notes::colorHex(rgb), metrics.smallTextSize, ui_theme::textOn(color),
                 TextView::CENTER, TextView::MIDDLE),
        color, color, color, selectedColor == rgb ? ui_theme::textPrimary() : color,
        ui_theme::textPrimary(), ui_theme::textPrimary());
    button->setOnClickListener([apply, rgb] { apply(rgb); });
    swatches->addView(button);
  }
  return swatches;
}

class ScratchNoteSample : public View {
public:
  explicit ScratchNoteSample(std::uint32_t color) : color(color) {}

protected:
  void renderImpl(RenderContext &context) override {
    const auto stops = built_in_notes::scratchGradient(color);
    const auto program = rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    const auto state = context.makeUiBatchState(program, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    constexpr std::array<std::uint16_t, 6> indices{0, 1, 2, 2, 3, 0};
    for (std::size_t i = 1; i < stops.size(); ++i) {
      const float x0 = getX() + getWidth() * stops[i - 1].position;
      const float x1 = getX() + getWidth() * stops[i].position;
      const float y0 = getY();
      const float y1 = getY() + getHeight();
      const auto left = built_in_notes::abgr(stops[i - 1].color);
      const auto right = built_in_notes::abgr(stops[i].color);
      const std::array vertices{
          rendering::PosColorVertex{x0, y0, 0, left},
          rendering::PosColorVertex{x1, y0, 0, right},
          rendering::PosColorVertex{x1, y1, 0, right},
          rendering::PosColorVertex{x0, y1, 0, left}};
      context.appendUiColor(vertices, indices, state);
    }
  }

private:
  std::uint32_t color;
};

enum class SettingsButtonTone {
  Neutral,
  Primary,
  Info,
  Success,
  Warning,
  Danger,
  Violet
};

enum class ButtonVisualState { Normal, Hover, Pressed };

View::ThemeColorProvider semanticBackgroundProvider(SettingsButtonTone tone,
                                                    ButtonVisualState state) {
  return [tone, state]() {
    switch (tone) {
    case SettingsButtonTone::Primary:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::primaryAction();
      case ButtonVisualState::Hover:
        return ui_theme::primaryActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::primaryActionPressed();
      }
      break;
    case SettingsButtonTone::Info:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::infoAction();
      case ButtonVisualState::Hover:
        return ui_theme::infoActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::infoActionPressed();
      }
      break;
    case SettingsButtonTone::Success:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::successAction();
      case ButtonVisualState::Hover:
        return ui_theme::successActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::successActionPressed();
      }
      break;
    case SettingsButtonTone::Warning:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::warningAction();
      case ButtonVisualState::Hover:
        return ui_theme::warningActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::warningActionPressed();
      }
      break;
    case SettingsButtonTone::Danger:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::dangerAction();
      case ButtonVisualState::Hover:
        return ui_theme::dangerActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::dangerActionPressed();
      }
      break;
    case SettingsButtonTone::Violet:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::violetAction();
      case ButtonVisualState::Hover:
        return ui_theme::violetActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::violetActionPressed();
      }
      break;
    case SettingsButtonTone::Neutral:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::control();
      case ButtonVisualState::Hover:
        return ui_theme::controlHover();
      case ButtonVisualState::Pressed:
        return ui_theme::controlPressed();
      }
      break;
    }
    return ui_theme::control();
  };
}

View::ThemeColorProvider semanticBorderProvider(SettingsButtonTone tone,
                                                ButtonVisualState state) {
  return [tone, state]() {
    if (tone == SettingsButtonTone::Neutral) {
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::hairline();
      case ButtonVisualState::Hover:
        return ui_theme::accentBorder();
      case ButtonVisualState::Pressed:
        return ui_theme::accentBorderStrong();
      }
    }

    const View::ThemeColorProvider baseProvider =
        semanticBackgroundProvider(tone, state);
    const uint8_t alpha = state == ButtonVisualState::Normal
                              ? 164
                              : (state == ButtonVisualState::Hover ? 206 : 232);
    return ui_theme::withAlpha(baseProvider(), alpha);
  };
}

void applySemanticButtonStyle(Button *button, TextView *text,
                              SettingsButtonTone tone) {
  if (button == nullptr) {
    return;
  }

  auto normal = semanticBackgroundProvider(tone, ButtonVisualState::Normal);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(
      normal, semanticBackgroundProvider(tone, ButtonVisualState::Hover),
      semanticBackgroundProvider(tone, ButtonVisualState::Pressed));
  button->setThemedBorderColors(
      semanticBorderProvider(tone, ButtonVisualState::Normal),
      semanticBorderProvider(tone, ButtonVisualState::Hover),
      semanticBorderProvider(tone, ButtonVisualState::Pressed));
  button->setStyledBorderWidth(1);

  if (text == nullptr) {
    return;
  }
  if (tone == SettingsButtonTone::Neutral) {
    text->setThemedColor(ui_theme::textPrimary);
  } else {
    text->setThemedColor([normal]() { return ui_theme::textOn(normal()); });
  }
}
} // namespace

void SettingsScene::styleVisibilityButton(Button *button, TextView *text, bool visible) {
  applySemanticButtonStyle(button, text,
      visible ? SettingsButtonTone::Success : SettingsButtonTone::Danger);
}

void SettingsScene::refreshSettingsText(bool syncInputs) {
  const int offsetMs = context.settings.audioOffsetMs;
  const int visualOffsetMs = context.settings.visualOffsetMs;
  const int visibleTimeDurationMilliseconds =
      context.settings.visibleTimeDurationMilliseconds;
  const i18n::Text offsetLabel = formatOffsetLabel(offsetMs);
  const i18n::Text visualOffsetLabel = formatOffsetLabel(visualOffsetMs);
  const i18n::Text visibleTimeLabel = formatVisibleTimeLabel(
      visibleTimeDurationMilliseconds,
      context.settings.visibleTimeUseMilliseconds);
  const i18n::Text keysoundLabel =
      context.settings.inputKeysoundEnabled ? i18n::message("settings.controls.keysound.input_trigger.label") : i18n::message("settings.controls.keysound.auto_timed.label");
  const i18n::Text prepMetronomeLabel =
      context.settings.prepMetronomeEnabled ? i18n::message("settings.controls.prep_metronome.prep_on.label") : i18n::message("settings.controls.prep_metronome.prep_off.label");
  const i18n::Text bgaLabel =
      context.settings.bgaEnabled ? i18n::message("settings.controls.bga.enabled.label") : i18n::message("settings.controls.bga.disabled.label");
  const i18n::Text bgaDisplayLabel =
      formatBgaDisplayModeLabel(context.settings.bgaDisplayMode);
  const i18n::Text bgaBrightnessLabel =
      formatBgaBrightnessLabel(context.settings.bgaBrightnessPercent);
  const i18n::Text bgaBlurLabel =
      formatBgaBlurLabel(context.settings.bgaBlurStrength);
  const i18n::Text laneAngleLabel =
      formatLaneAngleLabel(context.settings.presentation().laneAngleDegrees);
  const i18n::Text laneLengthLabel =
      formatLaneLengthLabel(context.settings.presentation().laneLength);
  const i18n::Text laneBeamLengthLabel =
      formatLaneBeamLengthLabel(context.settings.presentation().laneBeamLengthPercent);
  const i18n::Text noteStartPositionLabel =
      formatNoteStartPositionLabel(context.settings.presentation().noteStartPositionPercent);
  const i18n::Text previewPlayAreaWidthLabel =
      formatPlayAreaWidthLabel(context.settings.playAreaWidthForKeyMode(previewKeyMode));
  const i18n::Text judgementIndicatorYLabel = formatJudgementPercentLabel(
      judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY));
  const i18n::Text judgementIndicatorWidthLabel =
      std::to_string(judgementIndicatorWidthScaleToPercent(
          context.settings.presentation().judgementIndicatorWidthScale)) +
      "%";
  const i18n::Text judgementIndicatorRangeLabel =
      formatJudgementIndicatorRangeLabel(
          context.settings.presentation().judgementIndicatorRangeMilliseconds);
  const i18n::Text notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const i18n::Text invisibleNotesLabel =
      context.settings.showInvisibleNotes ? i18n::message("settings.controls.invisible_notes.shown.label") : i18n::message("settings.controls.invisible_notes.hidden.label");
  const i18n::Text markProcessedNotesLabel =
      context.settings.markProcessedNotes ? i18n::message("settings.controls.mark_processed_notes.enabled.label") : i18n::message("settings.controls.mark_processed_notes.disabled.label");
  const i18n::Text startLaneIndicatorsLabel =
      context.settings.startLaneIndicatorsEnabled ? i18n::message("settings.controls.start_lane_indicators.shown.label") : i18n::message("settings.controls.start_lane_indicators.hidden.label");
  const i18n::Text touchVisualizationLabel =
      context.settings.touchVisualizationEnabled ? i18n::message("settings.controls.touch_visualization.shown.label") : i18n::message("settings.controls.touch_visualization.hidden.label");
  const i18n::Text hispeedAutoAdjustLabel =
      context.settings.hispeedAutoAdjust ? i18n::message("settings.controls.hispeed_auto_adjust.hi_speed_auto_adjust_on.label")
                                         : i18n::message("settings.controls.hispeed_auto_adjust.hi_speed_auto_adjust_off.label");
  const i18n::Text archiveChartPreviewLabel =
      context.settings.archiveChartPreviewEnabled ? i18n::message("settings.controls.archive_chart_preview.enabled.label") : i18n::message("settings.controls.archive_chart_preview.disabled.label");
  const i18n::Text findBmsSkipUnarchivingLabel =
      context.settings.findBmsSkipUnarchivingForNonSolidArchives ? i18n::message("settings.controls.find_bms_skip_unarchiving.on.label")
                                                                 : i18n::message("settings.controls.find_bms_skip_unarchiving.off.label");
  const i18n::Text judgementIndicatorLabel =
      context.settings.presentation().judgementIndicatorEnabled ? i18n::message("settings.controls.judgement_indicator.enabled.label") : i18n::message("settings.controls.judgement_indicator.disabled.label");
  const i18n::Text judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.presentation().judgementIndicatorRenderMode);
  const i18n::Text judgementCounterPositionLabel =
      formatJudgementCounterPositionLabel(
          context.settings.presentation().judgementCounterPosition);
  const i18n::Text judgementCounterModeLabel =
      context.settings.presentation().judgementCounterEnabled ? i18n::message("settings.controls.judgement_counter_mode.enabled.label") : i18n::message("settings.controls.judgement_counter_mode.disabled.label");
  const i18n::Text judgementCounterSummaryLabel =
      context.settings.presentation().judgementCounterEnabled ? judgementCounterPositionLabel
                                               : i18n::message("settings.controls.judgement_counter_summary.disabled.label");
  const i18n::Text judgementTimingFastSlowLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.presentation().judgementTimingFastSlowCriteria);
  const i18n::Text judgementTimingMillisecondsLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.presentation().judgementTimingMillisecondsCriteria);
  const i18n::Text gaugeBarPositionLabel =
      formatGaugeBarPositionLabel(context.settings.presentation().gaugeBarPosition);
  const i18n::Text uiThemeLabel =
      formatUiThemeModeLabel(context.settings.uiThemeMode);

  if (syncInputs) {
    syncOffsetInputText();
  }
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setLocalizedText(offsetLabel);
  }
  if (syncInputs) {
    syncVisualOffsetInputText();
  }
  if (summaryVisualOffsetValueText != nullptr) {
    summaryVisualOffsetValueText->setLocalizedText(visualOffsetLabel);
  }
  if (syncInputs) {
    syncVisibleTimeInputText();
  }
  if (summaryVisibleTimeValueText != nullptr) {
    summaryVisibleTimeValueText->setLocalizedText(visibleTimeLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setLocalizedText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setLocalizedText(bgaLabel);
  }
  if (summaryBgaDisplayValueText != nullptr) {
    summaryBgaDisplayValueText->setLocalizedText(bgaDisplayLabel);
  }
  if (syncInputs) {
    syncBgaBrightnessInputText();
  }
  if (summaryBgaBrightnessValueText != nullptr) {
    summaryBgaBrightnessValueText->setLocalizedText(bgaBrightnessLabel);
  }
  if (syncInputs) {
    syncBgaBlurInputText();
  }
  if (summaryBgaBlurValueText != nullptr) {
    summaryBgaBlurValueText->setLocalizedText(bgaBlurLabel);
  }
  if (syncInputs) {
    syncLaneAngleInputText();
  }
  if (summaryLaneAngleValueText != nullptr) {
    summaryLaneAngleValueText->setLocalizedText(laneAngleLabel);
  }
  if (syncInputs) {
    syncLaneLengthInputText();
  }
  if (summaryLaneLengthValueText != nullptr) {
    summaryLaneLengthValueText->setLocalizedText(laneLengthLabel);
  }
  if (syncInputs) {
    syncLaneBeamLengthInputText();
  }
  if (summaryLaneBeamLengthValueText != nullptr) {
    summaryLaneBeamLengthValueText->setLocalizedText(laneBeamLengthLabel);
  }
  if (syncInputs) {
    syncNoteStartPositionInputText();
  }
  if (summaryNoteStartPositionValueText != nullptr) {
    summaryNoteStartPositionValueText->setLocalizedText(noteStartPositionLabel);
  }
  if (summaryPreviewPlayAreaWidthValueText != nullptr) {
    summaryPreviewPlayAreaWidthValueText->setLocalizedText(previewPlayAreaWidthLabel);
  }
  if (summaryJudgementIndicatorYValueText != nullptr) {
    summaryJudgementIndicatorYValueText->setLocalizedText(judgementIndicatorYLabel);
  }
  if (summaryJudgementIndicatorWidthValueText != nullptr) {
    summaryJudgementIndicatorWidthValueText->setLocalizedText(
        judgementIndicatorWidthLabel);
  }
  if (summaryJudgementIndicatorRangeValueText != nullptr) {
    summaryJudgementIndicatorRangeValueText->setLocalizedText(
        judgementIndicatorRangeLabel);
  }
  if (summaryJudgementCounterPositionValueText != nullptr) {
    summaryJudgementCounterPositionValueText->setLocalizedText(
        judgementCounterSummaryLabel);
  }
  if (summaryJudgementTimingFastSlowValueText != nullptr) {
    summaryJudgementTimingFastSlowValueText->setLocalizedText(
        judgementTimingFastSlowLabel);
  }
  if (summaryJudgementTimingMillisecondsValueText != nullptr) {
    summaryJudgementTimingMillisecondsValueText->setLocalizedText(
        judgementTimingMillisecondsLabel);
  }
  if (summaryGaugeBarPositionValueText != nullptr) {
    summaryGaugeBarPositionValueText->setLocalizedText(gaugeBarPositionLabel);
  }
  if (summaryNotePriorityValueText != nullptr) {
    summaryNotePriorityValueText->setLocalizedText(notePriorityLabel);
  }
  if (summaryUiThemeValueText != nullptr) {
    summaryUiThemeValueText->setLocalizedText(uiThemeLabel);
  }
  if (syncInputs) {
    syncJudgementIndicatorYInputText();
  }
  if (syncInputs) {
    syncJudgementIndicatorWidthInputText();
  }
  if (syncInputs) {
    syncJudgementIndicatorRangeInputText();
  }
  if (keysoundModeText != nullptr) {
    keysoundModeText->setLocalizedText(keysoundLabel);
  }
  if (prepMetronomeModeText != nullptr) {
    prepMetronomeModeText->setLocalizedText(prepMetronomeLabel);
  }
  if (notePriorityModeText != nullptr) {
    notePriorityModeText->setLocalizedText(notePriorityLabel);
  }
  if (showInvisibleNotesModeText != nullptr) {
    showInvisibleNotesModeText->setLocalizedText(invisibleNotesLabel);
  }
  if (markProcessedNotesModeText != nullptr) {
    markProcessedNotesModeText->setLocalizedText(markProcessedNotesLabel);
  }
  if (ipadGestureReminderModeText != nullptr) {
    ipadGestureReminderModeText->setLocalizedText(i18n::message(
        context.settings.ipadGestureReminderEnabled
            ? "settings.visual.ipad_gesture_reminder.on"
            : "settings.visual.ipad_gesture_reminder.off"));
  }
  if (startLaneIndicatorsModeText != nullptr) {
    startLaneIndicatorsModeText->setLocalizedText(startLaneIndicatorsLabel);
  }
  if (touchVisualizationModeText != nullptr) {
    touchVisualizationModeText->setLocalizedText(touchVisualizationLabel);
  }
  if (hispeedAutoAdjustModeText != nullptr) {
    hispeedAutoAdjustModeText->setLocalizedText(hispeedAutoAdjustLabel);
  }
  if (archiveChartPreviewModeText != nullptr) {
    archiveChartPreviewModeText->setLocalizedText(archiveChartPreviewLabel);
  }
  if (findBmsSkipUnarchivingModeText != nullptr) {
    findBmsSkipUnarchivingModeText->setLocalizedText(findBmsSkipUnarchivingLabel);
  }
  if (judgementIndicatorModeText != nullptr) {
    judgementIndicatorModeText->setLocalizedText(judgementIndicatorLabel);
  }
  if (judgementIndicatorRenderModeText != nullptr) {
    judgementIndicatorRenderModeText->setLocalizedText(
        judgementIndicatorRenderModeLabel);
  }
  if (judgementCounterPositionText != nullptr) {
    judgementCounterPositionText->setLocalizedText(judgementCounterPositionLabel);
  }
  if (judgementCounterModeText != nullptr) {
    judgementCounterModeText->setLocalizedText(judgementCounterModeLabel);
  }
  if (judgementTimingFastSlowCriteriaText != nullptr) {
    judgementTimingFastSlowCriteriaText->setLocalizedText(judgementTimingFastSlowLabel);
  }
  if (judgementTimingMillisecondsCriteriaText != nullptr) {
    judgementTimingMillisecondsCriteriaText->setLocalizedText(
        judgementTimingMillisecondsLabel);
  }
  if (gaugeBarPositionText != nullptr) {
    gaugeBarPositionText->setLocalizedText(gaugeBarPositionLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setLocalizedText(bgaLabel);
  }
  if (bgaDisplayModeText != nullptr) {
    bgaDisplayModeText->setLocalizedText(bgaDisplayLabel);
  }
  if (uiThemeModeText != nullptr) {
    uiThemeModeText->setLocalizedText(uiThemeLabel);
  }
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setLocalizedText(context.settings.visibleTimeUseMilliseconds
                                     ? i18n::message("settings.controls.milliseconds.label")
                                     : i18n::message("settings.controls.green_number.label"));
  }
  applySemanticButtonStyle(visibleTimeModeButton, visibleTimeModeText,
                           context.settings.visibleTimeUseMilliseconds
                               ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(keysoundModeButton, keysoundModeText,
                           context.settings.inputKeysoundEnabled
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Warning);
  applySemanticButtonStyle(prepMetronomeModeButton, prepMetronomeModeText,
                           context.settings.prepMetronomeEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Info);
  applySemanticButtonStyle(notePriorityModeButton, notePriorityModeText,
                           context.settings.notePriorityMode ==
                                   AppSettings::NotePriorityMode::Lowest
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Success);
  applySemanticButtonStyle(
      showInvisibleNotesModeButton, showInvisibleNotesModeText,
      context.settings.showInvisibleNotes ? SettingsButtonTone::Success
                                          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      markProcessedNotesModeButton, markProcessedNotesModeText,
      context.settings.markProcessedNotes ? SettingsButtonTone::Success
                                           : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      ipadGestureReminderModeButton, ipadGestureReminderModeText,
      context.settings.ipadGestureReminderEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      startLaneIndicatorsModeButton, startLaneIndicatorsModeText,
      context.settings.startLaneIndicatorsEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      touchVisualizationModeButton, touchVisualizationModeText,
      context.settings.touchVisualizationEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      hispeedAutoAdjustModeButton, hispeedAutoAdjustModeText,
      context.settings.hispeedAutoAdjust ? SettingsButtonTone::Success
                                          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      archiveChartPreviewModeButton, archiveChartPreviewModeText,
      context.settings.archiveChartPreviewEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      findBmsSkipUnarchivingModeButton, findBmsSkipUnarchivingModeText,
      context.settings.findBmsSkipUnarchivingForNonSolidArchives
          ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      judgementIndicatorModeButton, judgementIndicatorModeText,
      context.settings.presentation().judgementIndicatorEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      judgementIndicatorRenderModeButton, judgementIndicatorRenderModeText,
      context.settings.presentation().judgementIndicatorRenderMode ==
              AppSettings::JudgementIndicatorRenderMode::Hud2D
          ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(judgementCounterModeButton, judgementCounterModeText,
                           context.settings.presentation().judgementCounterEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Danger);
  SettingsButtonTone judgementCounterPositionTone = SettingsButtonTone::Neutral;
  if (context.settings.presentation().judgementCounterEnabled) {
    judgementCounterPositionTone =
        context.settings.presentation().judgementCounterPosition ==
                AppSettings::JudgementCounterPosition::Top
            ? SettingsButtonTone::Info
            : SettingsButtonTone::Success;
  }
  applySemanticButtonStyle(judgementCounterPositionButton,
                           judgementCounterPositionText,
                           judgementCounterPositionTone);
  auto judgementTimingCriteriaTone =
      [](AppSettings::JudgementTimingDisplayCriteria criteria) {
        if (criteria == AppSettings::JudgementTimingDisplayCriteria::Off) {
          return SettingsButtonTone::Danger;
        }
        if (criteria ==
                AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow ||
            criteria ==
                AppSettings::JudgementTimingDisplayCriteria::BadOrBelow) {
          return SettingsButtonTone::Success;
        }
        if (criteria ==
            AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow) {
          return SettingsButtonTone::Warning;
        }
        return SettingsButtonTone::Info;
      };
  applySemanticButtonStyle(
      judgementTimingFastSlowCriteriaButton,
      judgementTimingFastSlowCriteriaText,
      judgementTimingCriteriaTone(
          context.settings.presentation().judgementTimingFastSlowCriteria));
  applySemanticButtonStyle(
      judgementTimingMillisecondsCriteriaButton,
      judgementTimingMillisecondsCriteriaText,
      judgementTimingCriteriaTone(
          context.settings.presentation().judgementTimingMillisecondsCriteria));
  applySemanticButtonStyle(gaugeBarPositionButton, gaugeBarPositionText,
                           context.settings.presentation().gaugeBarPosition ==
                                   AppSettings::GaugeBarPosition::World
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Success);
  applySemanticButtonStyle(bgaModeButton, bgaModeText,
                           context.settings.bgaEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Danger);
  applySemanticButtonStyle(uiThemeModeButton, uiThemeModeText,
                           context.settings.uiThemeMode ==
                                   AppSettings::UiThemeMode::Dark
                               ? SettingsButtonTone::Violet
                               : SettingsButtonTone::Warning);

  auto applyTabStyle = [this](Button *button, TextView *text, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      applySemanticButtonStyle(button, text, SettingsButtonTone::Primary);
    } else {
      applySemanticButtonStyle(button, text, SettingsButtonTone::Neutral);
    }
  };
  applyTabStyle(profileTabButton, profileTabText, SettingsTab::Profile);
  applyTabStyle(timingTabButton, timingTabText, SettingsTab::Timing);
  applyTabStyle(visualTabButton, visualTabText, SettingsTab::Visual);
  applyTabStyle(laneTabButton, laneTabText, SettingsTab::Lane);
  applyTabStyle(inputTabButton, inputTabText, SettingsTab::Input);
  applyTabStyle(miscTabButton, miscTabText, SettingsTab::Misc);
  applyTabStyle(audioTabButton, audioTabText, SettingsTab::Audio);
  applyTabStyle(displayTabButton, displayTabText, SettingsTab::Display);
  applyTabStyle(difficultyTablesTabButton, difficultyTablesTabText,
                SettingsTab::DifficultyTables);
  applyTabStyle(bmsLibraryTabButton, bmsLibraryTabText,
                SettingsTab::BmsLibrary);
  applyTabStyle(gameplaySkinsTabButton, gameplaySkinsTabText,
                SettingsTab::GameplaySkins);
  applyTabStyle(irTabButton, irTabText, SettingsTab::Ir);

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  const ui_theme::ThemeMode previousMode = ui_theme::activeMode();
  const ui_theme::ThemeMode nextMode =
      context.settings.uiThemeMode == AppSettings::UiThemeMode::Light
          ? ui_theme::ThemeMode::Light
          : ui_theme::ThemeMode::Dark;
  ui_theme::setActiveMode(nextMode);
  const bool themeChanged = previousMode != nextMode;
  if (!context.saveSettings()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  context.jukebox.setBgaOffsetMs(context.settings.audioOffsetMs);
  context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
  if (themeChanged && rootLayout != nullptr) {
    rootLayout->propagateThemeChange();
  }
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
      formatVisibleTimeInputValue(
                                  context.settings.visibleTimeDurationMilliseconds,
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
      formatFloatValue(context.settings.presentation().laneAngleDegrees));
}

void SettingsScene::syncLaneLengthInputText(bool force) {
  if (laneLengthInput == nullptr) {
    return;
  }
  if (!force && laneLengthInput->getSelected()) {
    return;
  }
  laneLengthInput->setEditingText(
      formatFloatValue(context.settings.presentation().laneLength));
}

void SettingsScene::syncLaneBeamLengthInputText(bool force) {
  if (laneBeamLengthInput == nullptr) {
    return;
  }
  if (!force && laneBeamLengthInput->getSelected()) {
    return;
  }
  laneBeamLengthInput->setEditingText(
      std::to_string(context.settings.presentation().laneBeamLengthPercent));
}

void SettingsScene::syncNoteStartPositionInputText(bool force) {
  if (noteStartPositionInput == nullptr) {
    return;
  }
  if (!force && noteStartPositionInput->getSelected()) {
    return;
  }
  noteStartPositionInput->setEditingText(
      std::to_string(context.settings.presentation().noteStartPositionPercent));
}

void SettingsScene::syncJudgementIndicatorYInputText(bool force) {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorYInput->getSelected()) {
    return;
  }
  judgementIndicatorYInput->setEditingText(std::to_string(
      judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY)));
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
          context.settings.presentation().judgementIndicatorWidthScale)));
}

void SettingsScene::syncJudgementIndicatorRangeInputText(bool force) {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorRangeInput->getSelected()) {
    return;
  }
  judgementIndicatorRangeInput->setEditingText(
      std::to_string(context.settings.presentation().judgementIndicatorRangeMilliseconds));
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
      context.settings.visibleTimeDurationMilliseconds = std::clamp(
          parsedValue, AppSettings::kMinVisibleTimeMs,
          AppSettings::kMaxVisibleTimeMs);
    } else {
      context.settings.setVisibleTimeGreenNumber(parsedValue);
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
    context.settings.presentation().laneAngleDegrees = clampLaneAngle(context.settings, std::stof(rawText));
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
    context.settings.presentation().laneLength = clampLaneLength(context.settings, std::stof(rawText));
    persistSettings();
    syncLaneLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneLengthInputText(true);
  }
}

void SettingsScene::commitLaneBeamLengthInput() {
  if (laneBeamLengthInput == nullptr) {
    return;
  }

  const std::string rawText = laneBeamLengthInput->getText();
  if (rawText.empty()) {
    syncLaneBeamLengthInputText(true);
    return;
  }

  try {
    context.settings.presentation().laneBeamLengthPercent =
        clampLaneBeamLengthPercent(std::stoi(rawText));
    persistSettings();
    syncLaneBeamLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneBeamLengthInputText(true);
  }
}

void SettingsScene::commitNoteStartPositionInput() {
  if (noteStartPositionInput == nullptr) {
    return;
  }

  const std::string rawText = noteStartPositionInput->getText();
  if (rawText.empty()) {
    syncNoteStartPositionInputText(true);
    return;
  }

  try {
    context.settings.presentation().noteStartPositionPercent =
        clampNoteStartPositionPercent(std::stoi(rawText));
    persistSettings();
    syncNoteStartPositionInputText(true);
  } catch (const std::exception &) {
    syncNoteStartPositionInputText(true);
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
    context.settings.presentation().judgementIndicatorY =
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
    const int percent = std::clamp(std::stoi(rawText), minPercent, maxPercent);
    context.settings.presentation().judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(percent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorWidthInputText(true);
  }
}

void SettingsScene::commitJudgementIndicatorRangeInput() {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorRangeInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorRangeInputText(true);
    return;
  }

  try {
    context.settings.presentation().judgementIndicatorRangeMilliseconds =
        clampJudgementIndicatorRangeMilliseconds(std::stoi(rawText));
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorRangeInputText(true);
  }
}

void SettingsScene::appendBuiltInNoteControls(
    View *body, const LayoutMetrics &metrics, int keyMode) {
  using namespace built_in_notes;
  if (keyMode == -5 && context.settings.presentation().skin.follow5K1S) keyMode = 5;
  if (keyMode == -7 && context.settings.presentation().skin.follow7K1S) keyMode = 7;
  bms_parser::ChartMeta meta;
  meta.KeyMode = std::abs(keyMode);
  meta.IsDP = keyMode == 10 || keyMode == 14;
  const auto lanes = keyMode < 0 ? meta.GetKeyLaneIndices() : meta.GetTotalLaneIndices();
  const auto keys = meta.GetKeyLaneIndices();
  const auto scratches = keyMode < 0 ? std::vector<int>{} : meta.GetScratchLaneIndices();
  if (lanes.empty()) return;
  const auto [selectionIt, inserted] = builtInNoteLanes.try_emplace(keyMode);
  auto &selectedLanes = selectionIt->second;
  if (inserted) selectedLanes.insert(lanes.front());
  builtInNoteType = std::clamp(builtInNoteType, 0, int(kTypeNames.size()) - 1);
  const auto type = static_cast<Type>(builtInNoteType);
  std::vector<LaneTarget> targets;
  for (const int lane : lanes) {
    if (!selectedLanes.contains(lane)) continue;
    auto palette = Palette::Gray;
    if (std::find(scratches.begin(), scratches.end(), lane) != scratches.end())
      palette = Palette::Scratch;
    else {
      const auto position = std::find(keys.begin(), keys.end(), lane) - keys.begin();
      if (start_lane_indicator::colorRoleForKey(position, keys.size()) ==
          start_lane_indicator::ColorRole::Blue) palette = Palette::Blue;
    }
    targets.push_back({lane, palette});
  }
  const auto &modeStyles = context.settings.builtInNotesForKeyMode(keyMode);
  const auto common = commonStyle(modeStyles, targets, type);
  const auto apply = [this, keyMode, targets, type](EditKind kind, int value = 0) {
    if (targets.empty()) return;
    editSelected(context.settings.presentation().builtInNotes[keyMode], targets, type, kind, value);
    persistSettings();
    syncPreviewPresentationConfiguration();
    lastLayoutWidth = -1;
  };
  body->addView(makeWrappedText(i18n::message("settings.notes.title"),
                               metrics.bodyTextSize, ui_theme::textPrimary()));
  body->addView(makeWrappedText(i18n::message("settings.notes.help"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  const auto laneLabel = [&scratches, &keys](int candidate) -> std::string {
    const auto scratchIt = std::find(scratches.begin(), scratches.end(), candidate);
    const auto keyIt = std::find(keys.begin(), keys.end(), candidate);
    if (scratchIt != scratches.end())
      return scratches.size() == 1 ? "S" : scratchIt == scratches.begin() ? "LS" : "RS";
    return std::to_string(keyIt - keys.begin() + 1);
  };
  body->addView(makeWrappedText(i18n::message("settings.notes.lanes"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  auto *laneToggles = new View();
  laneToggles->setFlexDirection(FlexDirection::Row);
  laneToggles->setFlexWrap(YGWrapWrap);
  laneToggles->setGap(6.0F);
  for (const int candidate : lanes) {
    auto *toggle = makeControlButton(metrics.actionButtonHeight, metrics.actionButtonHeight,
        makeText(laneLabel(candidate), metrics.smallTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE));
    styleGameplaySkinChoiceButton(toggle, selectedLanes.contains(candidate));
    toggle->setOnClickListener([this, keyMode, candidate] {
      auto &selection = builtInNoteLanes[keyMode];
      if (!selection.erase(candidate)) selection.insert(candidate);
      lastLayoutWidth = -1;
    });
    laneToggles->addView(toggle);
  }
  body->addView(laneToggles);
  auto *unselectAll = makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.notes.unselect_all"), metrics.smallTextSize,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
  unselectAll->setEnabled(!targets.empty());
  unselectAll->setOnClickListener([this, keyMode] {
    builtInNoteLanes[keyMode].clear();
    lastLayoutWidth = -1;
  });
  auto *selectionActions = new View();
  selectionActions->setFlexDirection(FlexDirection::Row);
  selectionActions->setFlexWrap(YGWrapWrap);
  selectionActions->setGap(8.0F);
  selectionActions->addView(unselectAll);
  auto *invert = makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.notes.invert_selection"), metrics.smallTextSize,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
  invert->setOnClickListener([this, keyMode, lanes] {
    auto &selection = builtInNoteLanes[keyMode];
    std::set<int> inverted;
    for (const int lane : lanes)
      if (!selection.contains(lane)) inverted.insert(lane);
    selection = std::move(inverted);
    lastLayoutWidth = -1;
  });
  selectionActions->addView(invert);
  body->addView(selectionActions);
  const auto addDropdown = [this, body](const std::string &id, const i18n::Text &label,
      std::string selected, std::vector<DropdownView::Option> options,
      std::function<void(int)> select) {
    auto *dropdown = new DropdownView(
        {.onOpenChanged = [this, id](bool open) { builtInNoteDropdown = open ? id : ""; },
         .onOptionSelected = [this, select](const std::string &value) {
           select(std::stoi(value));
           builtInNoteDropdown.clear();
           lastLayoutWidth = -1;
         }}, overlayPortal);
    dropdown->setWidthPercent(100.0F);
    dropdown->refresh({.label = label, .selectedId = std::move(selected),
        .options = std::move(options), .open = builtInNoteDropdown == id});
    body->addView(dropdown);
  };
  constexpr std::array<const char *, kTypeNames.size()> typeLabels{
      "settings.notes.type.normal", "settings.notes.type.long_head",
      "settings.notes.type.long_tail", "settings.notes.type.long_body_off",
      "settings.notes.type.long_body_on", "settings.notes.type.hcn_head",
      "settings.notes.type.hcn_tail", "settings.notes.type.hcn_body_off",
      "settings.notes.type.hcn_body_on", "settings.notes.type.hcn_damage",
      "settings.notes.type.mine", "settings.notes.type.invisible"};
  std::vector<DropdownView::Option> typeOptions;
  for (std::size_t i = 0; i < typeLabels.size(); ++i)
    typeOptions.push_back({.id = std::to_string(i),
        .label = i18n::message(typeLabels[i])});
  addDropdown("note-type", i18n::message("settings.notes.type"), std::to_string(builtInNoteType),
              std::move(typeOptions), [this](int value) { builtInNoteType = value; });

  if (targets.empty()) {
    body->addView(makeWrappedText(i18n::message("settings.notes.select_lanes"),
                                 metrics.smallTextSize, ui_theme::textSecondary()));
    return;
  }

  auto *samples = new View();
  samples->setFlexDirection(FlexDirection::Row);
  samples->setFlexWrap(YGWrapWrap);
  samples->setGap(6.0F);
  for (const auto &target : targets) {
    const auto style = resolve(modeStyles, target.lane, type, target.palette);
    auto *sampleColumn = new View();
    sampleColumn->setWidth(180.0F);
    sampleColumn->addView(makeText(laneLabel(target.lane), metrics.smallTextSize,
                                  ui_theme::textSecondary()));
    auto *sampleFrame = new View();
    sampleFrame->setHeight(std::max(80.0F, height(160.0F, type, style) + 16.0F));
    sampleFrame->setWidthPercent(100.0F);
    sampleFrame->setAlignItems(YGAlignCenter);
    sampleFrame->setJustifyContent(YGJustifyCenter);
    sampleFrame->setBackgroundColor(Color(20, 24, 30));
    View *sample = target.palette == Palette::Scratch && hasScratchGradient(type)
        ? static_cast<View *>(new ScratchNoteSample(style.color)) : new View();
    sample->setWidth(isBody(type) ? bodyWidth(80.0F, style) : 160.0F);
    sample->setHeight(isBody(type) ? 64.0F : height(160.0F, type, style));
    sample->setFlexShrink(0);
    sample->setBackgroundColor(Color(0xFF000000U | style.color));
    if (type == Type::Invisible) {
      sample->setBackgroundColor(Color(0, 0, 0, 0));
      sample->setBorderColor(Color(0xE0000000U | style.color));
      sample->setBorderWidth(std::max(1, int(height(160.0F, type, style) * 0.15F)));
    }
    sampleFrame->addView(sample);
    sampleColumn->addView(sampleFrame);
    samples->addView(sampleColumn);
  }
  body->addView(samples);

  body->addView(makeWrappedText(i18n::message("settings.notes.presets"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  body->addView(makeAppearanceColorPresets(metrics, defaultStyle(targets.front().palette, type).color,
      common.color, [apply](std::uint32_t rgb) { apply(EditKind::Color, rgb); }));
  body->addView(makeWrappedText(i18n::message("settings.notes.custom_color"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  auto *colorInput = makeTextInput(metrics, 140);
  colorInput->setEditingText(common.color ? "#" + colorHex(*common.color)
                                         : i18n::message("settings.notes.mixed").resolve());
  colorInput->onEditingFinished([this, apply](const std::string &text) {
    if (const auto rgb = parseColor(text)) {
      apply(EditKind::Color, *rgb);
    } else lastLayoutWidth = -1;
  });
  body->addView(colorInput);
  const auto makePropertyReset = [&metrics, apply](const i18n::Text &label, EditKind kind) {
    auto *button = makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText(label, metrics.smallTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE));
    button->setOnClickListener([apply, kind] { apply(kind); });
    return button;
  };
  body->addView(makePropertyReset(i18n::message("settings.notes.reset_color"), EditKind::ResetColor));
  body->addView(makeWrappedText(i18n::message(isBody(type)
      ? "settings.notes.body_width" : "settings.notes.thickness"),
      metrics.smallTextSize, ui_theme::textSecondary()));
  auto *thicknessInput = makeTextInput(metrics, 140);
  thicknessInput->setEditingText(common.thickness ? std::to_string(*common.thickness)
      : i18n::message("settings.notes.mixed").resolve());
  thicknessInput->onEditingFinished([this, apply](const std::string &text) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
      apply(EditKind::Thickness, value);
    } else lastLayoutWidth = -1;
  });
  body->addView(thicknessInput);
  auto *steps = new View();
  steps->setFlexDirection(FlexDirection::Row);
  steps->setFlexWrap(YGWrapWrap);
  steps->setGap(8.0F);
  for (const int delta : {-10, 10}) {
    auto *button = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                  delta < 0 ? "-10%" : "+10%");
    button->setOnClickListener([apply, delta] { apply(EditKind::AdjustThickness, delta); });
    steps->addView(button);
  }
  steps->addView(makePropertyReset(i18n::message(isBody(type)
      ? "settings.notes.reset_width" : "settings.notes.reset_thickness"), EditKind::ResetThickness));
  body->addView(steps);
}

void SettingsScene::appendBuiltInJudgeLineControls(
    View *body, const LayoutMetrics &metrics, int keyMode) {
  if (keyMode == -5 && context.settings.presentation().skin.follow5K1S) keyMode = 5;
  if (keyMode == -7 && context.settings.presentation().skin.follow7K1S) keyMode = 7;
  const auto style = context.settings.builtInJudgeLineForKeyMode(keyMode);
  const auto apply = [this, keyMode](std::optional<std::uint32_t> color,
                                    std::optional<int> height) {
    auto next = context.settings.builtInJudgeLineForKeyMode(keyMode);
    if (color) next.color = *color;
    if (height) next.heightPercent = *height;
    context.settings.presentation().builtInJudgeLines[keyMode] =
        built_in_judge_line::sanitizeStyle(next);
    persistSettings();
    syncPreviewPresentationConfiguration();
    lastLayoutWidth = -1;
  };
  body->addView(makeWrappedText(i18n::message("settings.judge_line.title"),
                               metrics.bodyTextSize, ui_theme::textPrimary()));
  auto *sampleFrame = new View();
  sampleFrame->setWidthPercent(100.0F);
  sampleFrame->setHeight(std::max(48.0F, built_in_judge_line::height(160.0F, style) + 16.0F));
  sampleFrame->setAlignItems(YGAlignCenter);
  sampleFrame->setJustifyContent(YGJustifyCenter);
  sampleFrame->setBackgroundColor(Color(20, 24, 30));
  auto *sample = new View();
  sample->setWidthPercent(90.0F);
  sample->setHeight(built_in_judge_line::height(160.0F, style));
  sample->setFlexShrink(0);
  sample->setBackgroundColor(Color(0xFF000000U | style.color));
  sampleFrame->addView(sample);
  body->addView(sampleFrame);

  body->addView(makeWrappedText(i18n::message("settings.notes.presets"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  body->addView(makeAppearanceColorPresets(metrics, built_in_judge_line::Style{}.color,
      style.color, [apply](std::uint32_t rgb) { apply(rgb, std::nullopt); }));
  body->addView(makeWrappedText(i18n::message("settings.notes.custom_color"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  auto *colorInput = makeTextInput(metrics, 140);
  colorInput->setEditingText("#" + built_in_notes::colorHex(style.color));
  colorInput->onEditingFinished([this, apply](const std::string &text) {
    if (const auto color = built_in_notes::parseColor(text)) apply(*color, std::nullopt);
    else lastLayoutWidth = -1;
  });
  body->addView(colorInput);
  const auto makeReset = [&metrics](const i18n::Text &label) {
    return makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText(label, metrics.smallTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE));
  };
  auto *resetColor = makeReset(i18n::message("settings.notes.reset_color"));
  resetColor->setOnClickListener([apply] { apply(0xFFFFFF, std::nullopt); });
  body->addView(resetColor);
  body->addView(makeWrappedText(i18n::message("settings.judge_line.height"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  auto *heightInput = makeTextInput(metrics, 140);
  heightInput->setEditingText(std::to_string(style.heightPercent));
  heightInput->onEditingFinished([this, apply](const std::string &text) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
      apply(std::nullopt, value);
    else lastLayoutWidth = -1;
  });
  body->addView(heightInput);
  auto *steps = new View();
  steps->setFlexDirection(FlexDirection::Row);
  steps->setFlexWrap(YGWrapWrap);
  steps->setGap(8.0F);
  for (const int delta : {-10, 10}) {
    auto *button = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                  delta < 0 ? "-10%" : "+10%");
    button->setOnClickListener([this, keyMode, apply, delta] {
      apply(std::nullopt, context.settings.builtInJudgeLineForKeyMode(keyMode).heightPercent + delta);
    });
    steps->addView(button);
  }
  auto *resetHeight = makeReset(i18n::message("settings.judge_line.reset_height"));
  resetHeight->setOnClickListener([apply] { apply(std::nullopt, 100); });
  steps->addView(resetHeight);
  body->addView(steps);
}

void SettingsScene::appendBuiltInLaneControls(
    View *body, const LayoutMetrics &metrics, int keyMode) {
  if (keyMode == -5 && context.settings.presentation().skin.follow5K1S) keyMode = 5;
  if (keyMode == -7 && context.settings.presentation().skin.follow7K1S) keyMode = 7;
  const auto style = context.settings.builtInLaneForKeyMode(keyMode);
  const auto apply = [this, keyMode](std::function<void(built_in_lane::Style &)> edit) {
    auto next = context.settings.builtInLaneForKeyMode(keyMode);
    edit(next);
    context.settings.presentation().builtInLanes[keyMode] = built_in_lane::sanitizeStyle(next);
    persistSettings();
    syncPreviewPresentationConfiguration();
    lastLayoutWidth = -1;
  };
  const auto makeReset = [&metrics](const i18n::Text &label) {
    return makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText(label, metrics.smallTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE));
  };
  body->addView(makeWrappedText(i18n::message("settings.measure_line.title"),
                               metrics.bodyTextSize, ui_theme::textPrimary()));
  body->addView(makeWrappedText(i18n::message("settings.notes.presets"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  const auto setColor = [apply](std::uint32_t color) {
    apply([color](auto &next) { next.measureLineColor = color; });
  };
  body->addView(makeAppearanceColorPresets(metrics, built_in_lane::Style{}.measureLineColor,
                                          style.measureLineColor, setColor));
  body->addView(makeWrappedText(i18n::message("settings.notes.custom_color"),
                               metrics.smallTextSize, ui_theme::textSecondary()));
  auto *colorInput = makeTextInput(metrics, 140);
  colorInput->setEditingText("#" + built_in_notes::colorHex(style.measureLineColor));
  colorInput->onEditingFinished([this, setColor](const std::string &text) {
    if (const auto color = built_in_notes::parseColor(text)) setColor(*color);
    else lastLayoutWidth = -1;
  });
  body->addView(colorInput);
  auto *resetColor = makeReset(i18n::message("settings.notes.reset_color"));
  resetColor->setOnClickListener([setColor] { setColor(built_in_lane::Style{}.measureLineColor); });
  body->addView(resetColor);

  const auto addPercent = [this, body, &metrics, keyMode, apply, &makeReset](
      const i18n::Text &label, int built_in_lane::Style::*property,
      const i18n::Text &resetLabel) {
    body->addView(makeWrappedText(label, metrics.smallTextSize, ui_theme::textSecondary()));
    auto *input = makeTextInput(metrics, 140);
    input->setEditingText(std::to_string(context.settings.builtInLaneForKeyMode(keyMode).*property));
    input->onEditingFinished([this, apply, property](const std::string &text) {
      int value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
        apply([property, value](auto &next) { next.*property = value; });
      else lastLayoutWidth = -1;
    });
    body->addView(input);
    auto *steps = new View();
    steps->setFlexDirection(FlexDirection::Row);
    steps->setFlexWrap(YGWrapWrap);
    steps->setGap(8.0F);
    for (const int delta : {-10, 10}) {
      auto *button = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                    delta < 0 ? "-10%" : "+10%");
      button->setOnClickListener([apply, property, delta] {
        apply([property, delta](auto &next) { next.*property += delta; });
      });
      steps->addView(button);
    }
    auto *reset = makeReset(resetLabel);
    reset->setOnClickListener([apply, property] {
      apply([property](auto &next) { next.*property = built_in_lane::Style{}.*property; });
    });
    steps->addView(reset);
    body->addView(steps);
  };
  addPercent(i18n::message("settings.measure_line.thickness"),
             &built_in_lane::Style::measureLineThicknessPercent,
             i18n::message("settings.notes.reset_thickness"));
  body->addView(makeWrappedText(i18n::message("settings.lane_background.title"),
                               metrics.bodyTextSize, ui_theme::textPrimary()));
  addPercent(i18n::message("settings.lane_background.opacity"),
             &built_in_lane::Style::backgroundOpacityPercent,
             i18n::message("settings.lane_background.reset_opacity"));
}
