#include "../i18n/Localization.h"
#include "PlayOptionsPanelView.h"
#include "LaneCoverControlsView.h"

#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../scene/play/Pacemaker.h"
#include "Button.h"
#include "ActionButtonContent.h"
#include "ToggleButtonContent.h"
#include "ChoiceControlView.h"
#include "DropdownView.h"
#include "OverlayPortal.h"
#include "PlayOptionSectionView.h"
#include "SnappedSlider.h"
#include "TextView.h"
#include "AppTheme.h"
#include "../rendering/ResponsiveUi.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
constexpr const char *kFont = app_theme::font;

TextView *makeLabel(const i18n::Text &label) {
  auto *text = new TextView(app_theme::boldFont, 20, TextView::FontWeight::Bold);
  text->setLocalizedText(label);
  text->setThemedColor(app_theme::textPrimary);
  text->setVAlign(TextView::MIDDLE);
  text->setHeight(28);
  return text;
}

View *makeSectionHeader(TextView *label, uint32_t glyph, bool separator = true) {
  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setPadding(Edge::Top, separator ? 10 : 0);
  auto *rule = new View();
  rule->setPositionType(YGPositionTypeAbsolute);
  rule->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  rule->setWidthPercent(100)->setHeight(1);
  rule->setThemedBackgroundColor(app_theme::hairline);
  rule->setVisible(separator);
  header->addView(rule);
  auto *title = action_button::makeInlineContent(label, glyph);
  title->setHeight(28);
  header->addView(title);
  return header;
}

View *makeRow(float height = 48.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(10);
  row->setHeight(rendering::compactMenuCanvasActive ? YGUndefined : height);
  if (rendering::compactMenuCanvasActive) {
    row->setFlexWrap(YGWrapWrap)->setMinHeight(rendering::compactTouchTarget);
  }
  return row;
}

Button *makeButton(const i18n::Text &label, int fontSize, TextView **textOut) {
  auto *button = new Button(0, 0, 0, rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 48);
  button->setFlexGrow(1.0f);
  button->setFlexBasis(rendering::compactMenuCanvasActive ? 120.0f : 0.0f);
  button->setFlexShrink(1.0f);
  button->setCornerRadius(app_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairlineSubtle,
                                app_theme::hairlineStrong,
                                app_theme::accentBorder);
  auto *text = new TextView(kFont, fontSize);
  text->setLocalizedText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor(app_theme::textPrimary);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

void styleButton(Button *button, TextView *text, bool selected, bool enabled) {
  if (button == nullptr || text == nullptr) {
    return;
  }
  button->setEnabled(enabled);
  button->setSelected(selected);
  if (selected) {
    button->setThemedBackgroundColors(app_theme::primaryAction,
                                      app_theme::primaryActionHover,
                                      app_theme::primaryActionPressed);
    button->setThemedBorderColors(app_theme::accentBorderStrong,
                                  app_theme::accentBorderStrong,
                                  app_theme::accentBorderStrong);
    text->setThemedColor(
        [] { return app_theme::textOn(app_theme::primaryAction()); });
  } else if (!enabled) {
    button->setThemedBackgroundColors(
        app_theme::panelSubtle, app_theme::panelSubtle, app_theme::panelSubtle);
    button->setThemedBorderColors(app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle);
    text->setThemedColor(app_theme::textMuted);
  } else {
    button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                      app_theme::controlPressed);
    button->setThemedBorderColors(app_theme::hairlineSubtle,
                                  app_theme::hairlineStrong,
                                  app_theme::accentBorder);
    text->setThemedColor(app_theme::textPrimary);
  }
}

std::string gaugeLabel(GaugeType type, GaugeAutoShiftMode autoShift) {
  if (gaugeAutoShiftEnabled(autoShift)) {
    return gaugeAutoShiftMenuLabel(autoShift);
  }
  switch (type) {
  case GaugeType::AssistedEasy:
    return "A-EASY";
  case GaugeType::Easy:
    return "EASY";
  case GaugeType::Hard:
    return "HARD";
  case GaugeType::ExHard:
    return "EX-HARD";
  case GaugeType::Hazard:
    return "HAZARD";
  case GaugeType::Grade:
  case GaugeType::ExGrade:
  case GaugeType::ExHardGrade:
    return gaugeTypeToLabel(type);
  case GaugeType::Normal:
  default:
    return "NORMAL";
  }
}
constexpr std::array kGaugeTypes{GaugeType::AssistedEasy, GaugeType::Easy,
    GaugeType::Normal, GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard};
constexpr std::array kAutoShiftModes{GaugeAutoShiftMode::None,
    GaugeAutoShiftMode::Continue, GaugeAutoShiftMode::SurvivalToGroove,
    GaugeAutoShiftMode::BestClear, GaugeAutoShiftMode::SelectToUnder};

std::vector<DropdownView::Option> gaugeOptions(GaugeType selected) {
  std::vector<DropdownView::Option> options;
  for (const auto type : kGaugeTypes) {
    const auto label = gaugeLabel(type, GaugeAutoShiftMode::None);
    options.push_back({.id = label, .label = label});
  }
  if (std::ranges::find(kGaugeTypes, selected) == kGaugeTypes.end()) {
    const auto label = gaugeLabel(selected, GaugeAutoShiftMode::None);
    options.push_back({.id = label, .label = label, .available = false});
  }
  return options;
}

} // namespace

PlayOptionsPanelView::PlayOptionsPanelView(
    PlayOptionsPanelCallbacks newCallbacks, PlayOptionsPanelLayout layout,
    OverlayPortal *overlayPortal)
    : callbacks(std::move(newCallbacks)) {
  setWidth(layout.width);
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(8);

  auto *rulesetSectionLabel = makeLabel(i18n::message("play_options.ruleset.label"));
  rulesetSectionLabel->setName("ruleset-section-label");
  addView(makeSectionHeader(rulesetSectionLabel, ui_icons::kRuleset, false));
  rulesetChoice = new ChoiceControlView([this](const std::string &id) {
    const auto ruleset = gameplayRulesetFromId(id);
    if (!ruleset) return false;
    if (callbacks.onRulesetSelected) callbacks.onRulesetSelected(*ruleset);
    return true;
  }, overlayPortal);
  rulesetChoice->setName("ruleset-choice");
  addView(rulesetChoice);

  if (layout.showGauge) {
    gaugeSectionLabel = makeLabel(i18n::message("play_options.gauge.label"));
    gaugeSectionLabel->setName("gauge-section-label");
    addView(makeSectionHeader(gaugeSectionLabel, ui_icons::kHeart));
    gaugeChoice = new ChoiceControlView([this](const std::string &id) {
      for (const auto type : kGaugeTypes) {
        if (id != gaugeLabel(type, GaugeAutoShiftMode::None)) continue;
        if (callbacks.onGaugeSelected) callbacks.onGaugeSelected(type, state.gaugeAutoShift);
        return true;
      }
      return false;
    }, overlayPortal);
    gaugeChoice->setName("gauge-choice");
    addView(gaugeChoice);

    addView(makeSectionHeader(makeLabel(i18n::message("play_options.auto_shift.label")), ui_icons::kRotate));
    gaugeAutoShiftChoice = new ChoiceControlView([this](const std::string &id) {
      for (const auto mode : kAutoShiftModes) {
        if (id != gaugeAutoShiftMenuLabel(mode)) continue;
        if (callbacks.onGaugeSelected) callbacks.onGaugeSelected(state.gaugeType, mode);
        return true;
      }
      return false;
    }, overlayPortal, 16);
    gaugeAutoShiftChoice->setName("gauge-auto-shift-choice");
    addView(gaugeAutoShiftChoice);

    gaugeAutoShiftBoundsSection = new View();
    gaugeAutoShiftBoundsSection->setName("gauge-auto-shift-bounds");
    gaugeAutoShiftBoundsSection->setFlexDirection(FlexDirection::Column);
    gaugeAutoShiftBoundsSection->setAlignItems(YGAlignStretch);
    gaugeAutoShiftBoundsSection->setGap(8);
    gaugeAutoShiftBoundsSection->setDisplay(YGDisplayNone);
    gaugeAutoShiftBoundsSection->setVisible(false);
    gaugeAutoShiftBoundsSection->addView(makeSectionHeader(makeLabel(i18n::message("play_options.auto_shift_lower_bound.label")), ui_icons::kArrowDown));
    gaugeLowerBoundChoice = new ChoiceControlView([this](const std::string &id) {
      for (const auto type : kGaugeTypes) {
        if (id != gaugeLabel(type, GaugeAutoShiftMode::None)) continue;
        if (callbacks.onGaugeLowerBoundSelected) callbacks.onGaugeLowerBoundSelected(type);
        return true;
      }
      return false;
    }, overlayPortal);
    gaugeLowerBoundChoice->setName("gauge-lower-bound-choice");
    gaugeAutoShiftBoundsSection->addView(gaugeLowerBoundChoice);
    addView(gaugeAutoShiftBoundsSection);
  }

  addView(makeSectionHeader(makeLabel(i18n::message("play_options.section.play_option.label")), ui_icons::kShuffle));
  playOptionSection = new PlayOptionSectionView(
      {.onOptionSelected =
           [this](const std::string &option) {
             if (callbacks.onPlayOptionSelected) {
               callbacks.onPlayOptionSelected(option);
             }
           },
       .onLaneOrderSubmitted =
           [this](const std::string &notation) {
             if (callbacks.onLaneOrderSubmitted) {
               callbacks.onLaneOrderSubmitted(notation);
             }
           },
       .isOptionAllowed =
           [this](const std::string &option) {
             return !callbacks.isPlayOptionAllowed ||
                    callbacks.isPlayOptionAllowed(option);
           }},
      {.columns = layout.playOptionColumns,
       .rowHeight = rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 48.0f,
       .buttonFontSize = 15,
       .showLaneOrder = layout.showLaneOrder,
       .showHeading = false}, overlayPortal);
  playOptionSection->setWidthPercent(100);
  addView(playOptionSection);

  addView(makeSectionHeader(makeLabel(i18n::message("play_options.long_note_mode.label")), ui_icons::kGripLines));
  longNoteModeChoice = new ChoiceControlView([this](const std::string &id) {
    if (state.longNoteModeLocked) return false;
    if (callbacks.onLongNoteModeSelected) callbacks.onLongNoteModeSelected(id);
    return true;
  }, overlayPortal);
  longNoteModeChoice->setName("long-note-mode-choice");
  addView(longNoteModeChoice);

  assistOptionLabel = makeLabel(i18n::message("play_options.assist_option.label"));
  addView(makeSectionHeader(assistOptionLabel, ui_icons::kLifeRing));
  assistOptionChoice = new ChoiceControlView([this](const std::string &id) {
    if (state.assistOptionLocked) return false;
    if (callbacks.onAssistOptionSelected) callbacks.onAssistOptionSelected(id);
    return true;
  }, overlayPortal);
  assistOptionChoice->setName("assist-option-choice");
  addView(assistOptionChoice);

  addView(makeSectionHeader(makeLabel(i18n::message("settings.lane.covers.title")), ui_icons::kBars));
  laneCoverControls = new LaneCoverControlsView([this](const lane_cover::State &covers) {
    state.laneCovers = covers;
    if (callbacks.onLaneCoversChanged) callbacks.onLaneCoversChanged(covers);
  });
  addView(laneCoverControls);

  auto *playbackGroup = new View();
  playbackGroup->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(8);
  playbackGroup->addView(makeSectionHeader(makeLabel(i18n::message("play_options.playback_rate.label")), ui_icons::kGauge));
  auto *playbackRateRow = makeRow();
  playbackRateSlider = new SnappedSlider([this](int percent) {
    if (callbacks.onPlaybackRateSelected) {
      callbacks.onPlaybackRateSelected(percent);
    }
  });
  playbackRateSlider->setFlex(1.0F)->setMinWidth(180);
  playbackRateRow->addView(playbackRateSlider);
  playbackRateText = new TextView(kFont, 22);
  playbackRateText->setAlign(TextView::RIGHT);
  playbackRateText->setVAlign(TextView::MIDDLE);
  playbackRateText->setThemedColor(app_theme::textPrimary);
  playbackRateText->setWidth(72);
  playbackRateRow->addView(playbackRateText);
  playbackGroup->addView(playbackRateRow);
  playbackModeDropdown =
      new DropdownView({.onOpenChanged =
                            [this](bool open) {
                              playbackModeDropdownOpen = open;
                              refresh(state);
                            },
                        .onOptionSelected =
                            [this](const std::string &mode) {
                              playbackModeDropdownOpen = false;
                              if (callbacks.onPlaybackModeSelected) {
                                callbacks.onPlaybackModeSelected(mode);
                              }
                            }},
                       overlayPortal);
  playbackModeDropdown->setName("playback-mode-dropdown");
  playbackModeDropdown->setWidthPercent(100.0f);
  playbackGroup->addView(playbackModeDropdown);
  addView(playbackGroup);

  addView(makeSectionHeader(makeLabel(i18n::message("play_options.club_mode.label")), ui_icons::kPlay));
  clubModeButton = makeButton("", 18, nullptr);
  clubModeButton->setName("club-mode");
  clubModeButtonContent = new ToggleButtonContent("", 18, 17);
  clubModeButtonContent->labelView()->setLocalizedText(i18n::message("play_options.club_beat.label"));
  clubModeButton->setContentView(clubModeButtonContent);
  clubModeButton->setWidthPercent(100.0f);
  clubModeButton->setOnClickListener([this]() {
    if (callbacks.onClubModeToggled) {
      callbacks.onClubModeToggled();
    }
  });
  addView(clubModeButton);

  if (layout.showPacemaker) {
    addView(makeSectionHeader(makeLabel(i18n::message("play_options.pacemaker.label")), ui_icons::kFlagCheckered));
    pacemakerChoice = new ChoiceControlView([this](const std::string &id) {
      if (callbacks.onPacemakerSelected) callbacks.onPacemakerSelected(id);
      return true;
    }, overlayPortal);
    pacemakerChoice->setName("pacemaker-choice");
    addView(pacemakerChoice);
  }
}

void PlayOptionsPanelView::refresh(const PlayOptionsPanelState &newState) {
  if (laneCoverControls) laneCoverControls->refresh(newState.laneCovers);
  if (playOptionSection != nullptr &&
      (state.profileId != newState.profileId || state.ruleset != newState.ruleset)) {
    playOptionSection->resetLaneOrderDraft();
  }
  state = newState;
  rulesetChoice->refresh({.label = i18n::message("play_options.ruleset.label"),
      .selectedId = std::string(gameplayRulesetId(state.ruleset)),
      .options = {{.id = "lr2", .label = std::string(gameplayRulesetLabel(GameplayRuleset::LR2))},
                  {.id = "beatoraja", .label = std::string(gameplayRulesetLabel(GameplayRuleset::Beatoraja))}}});
  if (gaugeChoice) {
    gaugeChoice->refresh({.selectedId = gaugeLabel(state.gaugeType, GaugeAutoShiftMode::None),
                           .options = gaugeOptions(state.gaugeType)});
  }
  if (gaugeAutoShiftChoice) {
    std::vector<DropdownView::Option> options;
    for (const auto mode : kAutoShiftModes) {
      const std::string label = gaugeAutoShiftMenuLabel(mode);
      options.push_back({.id = label, .label = label});
    }
    gaugeAutoShiftChoice->refresh({.selectedId = gaugeAutoShiftMenuLabel(state.gaugeAutoShift),
                                    .options = std::move(options)});
  }
  const bool showsBounds =
      state.gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
      state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder;
  if (gaugeSectionLabel != nullptr) {
    gaugeSectionLabel->setLocalizedText(
        state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
            ? i18n::message("play_options.gauge_auto_shift_upper_bound.label")
            : i18n::message("play_options.gauge.label"));
  }
  if (gaugeAutoShiftBoundsSection != nullptr) {
    gaugeAutoShiftBoundsSection->setDisplay(showsBounds ? YGDisplayFlex
                                                        : YGDisplayNone);
    gaugeAutoShiftBoundsSection->setVisible(showsBounds);
  }
  if (gaugeLowerBoundChoice) {
    gaugeLowerBoundChoice->refresh({
        .selectedId = gaugeLabel(state.gaugeAutoShiftLowerBound, GaugeAutoShiftMode::None),
        .options = gaugeOptions(state.gaugeAutoShiftLowerBound)});
  }
  if (playOptionSection != nullptr) {
    playOptionSection->refresh(state.playOption, state.defaultLaneOrder,
                               state.laneOrderEnabled);
  }
  std::vector<DropdownView::Option> longNoteOptions;
  for (const char *mode : long_note_mode::kPlayableIds) {
    longNoteOptions.push_back({.id = mode, .label = mode});
  }
  longNoteModeChoice->refresh({.selectedId = state.longNoteMode,
      .options = std::move(longNoteOptions), .enabled = !state.longNoteModeLocked});
  assistOptionChoice->refresh({.selectedId = state.assistOption,
      .options = {{.id = assist_options::kOff, .label = assist_options::kOff},
                  {.id = assist_options::kDrag, .label = assist_options::kDrag},
                  {.id = assist_options::kBpmGuide, .label = assist_options::kBpmGuide}},
      .enabled = !state.assistOptionLocked});
  if (assistOptionLabel != nullptr) {
    if (state.playbackRatePercent != 100) {
      assistOptionLabel->setLocalizedText(i18n::message("play_options.assist_option_light_assist_easy.label"));
    } else if (assist_options::isEnabled(state.assistOption)) {
      assistOptionLabel->setLocalizedText(i18n::message("play_options.assist_option_light_assist_easy.label"));
    } else {
      assistOptionLabel->setLocalizedText(i18n::message("play_options.assist_option.label"));
    }
  }
  if (playbackRateText != nullptr) {
    playbackRateText->setText(std::to_string(state.playbackRatePercent) + "%");
  }
  if (playbackRateSlider != nullptr) {
    playbackRateSlider->refresh({.minimum = 50,
                                 .maximum = 200,
                                 .step = 5,
                                 .value = state.playbackRatePercent,
                                 .enabled = !state.playbackLocked});
  }
  if (playbackModeDropdown != nullptr) {
    playbackModeDropdown->refresh(
        {.label = i18n::message("play_options.mode.label"),
         .selectedId = "pitch-shift",
         .options = {{.id = "pitch-shift", .label = i18n::message("play_options.pitch_shift.label")},
                     {.id = "time-stretch",
                      .label = i18n::message("play_options.time_stretch_unavailable.label"),
                      .available = false}},
         .open = playbackModeDropdownOpen,
         .enabled = !state.playbackLocked});
  }
  if (clubModeButton != nullptr && clubModeButtonContent != nullptr) {
    clubModeButtonContent->setChecked(state.clubMode);
    styleButton(clubModeButton, clubModeButtonContent->labelView(),
                state.clubMode, true);
    clubModeButtonContent->setThemedColor(
        state.clubMode
            ? View::ThemeColorProvider{[] {
                return app_theme::textOn(app_theme::primaryAction());
              }}
            : View::ThemeColorProvider{app_theme::textPrimary});
  }
  if (pacemakerChoice) {
    std::vector<DropdownView::Option> options;
    for (const char *target : pacemaker::kSelectableTargets) {
      options.push_back({.id = target, .label = pacemaker::displayTargetLabel(target)});
    }
    pacemakerChoice->refresh({.selectedId = pacemaker::normalizeTargetId(state.pacemakerTarget),
                               .options = std::move(options)});
  }
}

void PlayOptionsPanelView::setLaneOrderMessage(std::string message,
                                               bool error) {
  if (playOptionSection != nullptr) {
    playOptionSection->setLaneOrderMessage(std::move(message), error);
  }
}

void PlayOptionsPanelView::closeDropdowns() {
  for (auto *choice : {rulesetChoice, gaugeChoice, gaugeAutoShiftChoice,
                      gaugeLowerBoundChoice, longNoteModeChoice,
                      assistOptionChoice, pacemakerChoice}) {
    if (choice) choice->closeMenu();
  }
  if (playOptionSection) playOptionSection->closeDropdowns();
  playbackModeDropdownOpen = false;
  refresh(state);
}
