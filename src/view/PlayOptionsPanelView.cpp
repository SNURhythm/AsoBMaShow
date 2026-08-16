#include "PlayOptionsPanelView.h"

#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../scene/play/Pacemaker.h"
#include "Button.h"
#include "CheckboxButtonContent.h"
#include "DropdownView.h"
#include "OverlayPortal.h"
#include "PlayOptionSectionView.h"
#include "SnappedSlider.h"
#include "TextView.h"
#include "UiTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";

TextView *makeLabel(const std::string &label) {
  auto *text = new TextView(kFont, 20);
  text->setText(label);
  text->setThemedColor(ui_theme::textSecondary);
  text->setVAlign(TextView::MIDDLE);
  text->setHeight(28);
  return text;
}

View *makeRow(float height = 58.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(10);
  row->setHeight(height);
  return row;
}

Button *makeButton(const std::string &label, int fontSize, TextView **textOut) {
  auto *button = new Button(0, 0, 0, 54);
  button->setFlexGrow(1.0f);
  button->setFlexBasis(0.0f);
  button->setFlexShrink(1.0f);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                ui_theme::hairlineStrong,
                                ui_theme::accentBorder);
  auto *text = new TextView(kFont, fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor(ui_theme::textPrimary);
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
    button->setThemedBackgroundColors(ui_theme::primaryAction,
                                      ui_theme::primaryActionHover,
                                      ui_theme::primaryActionPressed);
    button->setThemedBorderColors(ui_theme::accentBorderStrong,
                                  ui_theme::accentBorderStrong,
                                  ui_theme::accentBorderStrong);
    text->setThemedColor(
        [] { return ui_theme::textOn(ui_theme::primaryAction()); });
  } else if (!enabled) {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
  } else {
    button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                      ui_theme::controlPressed);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineStrong,
                                  ui_theme::accentBorder);
    text->setThemedColor(ui_theme::textPrimary);
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
  case GaugeType::Normal:
  default:
    return "NORMAL";
  }
}
} // namespace

PlayOptionsPanelView::PlayOptionsPanelView(
    PlayOptionsPanelCallbacks newCallbacks, PlayOptionsPanelLayout layout,
    OverlayPortal *overlayPortal)
    : callbacks(std::move(newCallbacks)) {
  setWidth(layout.width);
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(12);

  auto *rulesetSectionLabel = makeLabel("Ruleset");
  rulesetSectionLabel->setName("ruleset-section-label");
  addView(rulesetSectionLabel);
  auto *rulesetRow = makeRow();
  for (const GameplayRuleset ruleset : {GameplayRuleset::LR2,
                                        GameplayRuleset::Beatoraja}) {
    TextView *text = nullptr;
    auto *button = makeButton(std::string(gameplayRulesetLabel(ruleset)), 18,
                              &text);
    button->setName("ruleset-" + std::string(gameplayRulesetId(ruleset)));
    button->setOnClickListener([this, ruleset]() {
      if (callbacks.onRulesetSelected) {
        callbacks.onRulesetSelected(ruleset);
      }
    });
    rulesetButtons.push_back({.button = button,
                              .text = text,
                              .id = std::string(gameplayRulesetId(ruleset)),
                              .ruleset = ruleset});
    rulesetRow->addView(button);
  }
  addView(rulesetRow);

  if (layout.showGauge) {
    gaugeSectionLabel = makeLabel("Gauge");
    gaugeSectionLabel->setName("gauge-section-label");
    addView(gaugeSectionLabel);
    auto addGaugeButton = [this](View *row, GaugeType type) {
      TextView *text = nullptr;
      const std::string label = gaugeLabel(type, GaugeAutoShiftMode::None);
      auto *button = makeButton(label, 18, &text);
      button->setOnClickListener([this, type]() {
        if (callbacks.onGaugeSelected) {
          callbacks.onGaugeSelected(type, state.gaugeAutoShift);
        }
      });
      gaugeButtons.push_back({.button = button,
                              .text = text,
                              .id = label,
                              .gaugeType = type,
                              .gaugeAutoShift = GaugeAutoShiftMode::None});
      row->addView(button);
    };
    auto *gaugeRowA = makeRow();
    addGaugeButton(gaugeRowA, GaugeType::AssistedEasy);
    addGaugeButton(gaugeRowA, GaugeType::Easy);
    addGaugeButton(gaugeRowA, GaugeType::Normal);
    addView(gaugeRowA);
    auto *gaugeRowB = makeRow();
    addGaugeButton(gaugeRowB, GaugeType::Hard);
    addGaugeButton(gaugeRowB, GaugeType::ExHard);
    addGaugeButton(gaugeRowB, GaugeType::Hazard);
    addView(gaugeRowB);

    addView(makeLabel("Auto Shift"));
    auto addAutoShiftButton = [this](View *row, GaugeAutoShiftMode mode) {
      TextView *text = nullptr;
      const std::string label = gaugeAutoShiftMenuLabel(mode);
      auto *button = makeButton(label, 16, &text);
      button->setOnClickListener([this, mode]() {
        if (callbacks.onGaugeSelected) {
          callbacks.onGaugeSelected(state.gaugeType, mode);
        }
      });
      gaugeAutoShiftButtons.push_back(
          {.button = button,
           .text = text,
           .id = label,
           .gaugeAutoShift = mode});
      row->addView(button);
    };
    auto *gaugeRowC = makeRow();
    addAutoShiftButton(gaugeRowC, GaugeAutoShiftMode::None);
    addAutoShiftButton(gaugeRowC, GaugeAutoShiftMode::Continue);
    addView(gaugeRowC);
    auto *gaugeRowD = makeRow();
    addAutoShiftButton(gaugeRowD, GaugeAutoShiftMode::SurvivalToGroove);
    addAutoShiftButton(gaugeRowD, GaugeAutoShiftMode::BestClear);
    addView(gaugeRowD);
    auto *gaugeRowE = makeRow();
    addAutoShiftButton(gaugeRowE, GaugeAutoShiftMode::SelectToUnder);
    addView(gaugeRowE);

    gaugeAutoShiftBoundsSection = new View();
    gaugeAutoShiftBoundsSection->setFlexDirection(FlexDirection::Column);
    gaugeAutoShiftBoundsSection->setAlignItems(YGAlignStretch);
    gaugeAutoShiftBoundsSection->setGap(12);
    gaugeAutoShiftBoundsSection->setDisplay(YGDisplayNone);
    gaugeAutoShiftBoundsSection->setVisible(false);
    gaugeAutoShiftBoundsSection->addView(makeLabel("Auto Shift Lower Bound"));
    auto addLowerBoundButton = [this](View *row, GaugeType type) {
      TextView *text = nullptr;
      const std::string label = gaugeLabel(type, GaugeAutoShiftMode::None);
      auto *button = makeButton(label, 18, &text);
      button->setOnClickListener([this, type]() {
        if (callbacks.onGaugeLowerBoundSelected) {
          callbacks.onGaugeLowerBoundSelected(type);
        }
      });
      gaugeLowerBoundButtons.push_back(
          {.button = button, .text = text, .id = label, .gaugeType = type});
      row->addView(button);
    };
    auto *lowerRowA = makeRow();
    addLowerBoundButton(lowerRowA, GaugeType::AssistedEasy);
    addLowerBoundButton(lowerRowA, GaugeType::Easy);
    addLowerBoundButton(lowerRowA, GaugeType::Normal);
    gaugeAutoShiftBoundsSection->addView(lowerRowA);
    auto *lowerRowB = makeRow();
    addLowerBoundButton(lowerRowB, GaugeType::Hard);
    addLowerBoundButton(lowerRowB, GaugeType::ExHard);
    addLowerBoundButton(lowerRowB, GaugeType::Hazard);
    gaugeAutoShiftBoundsSection->addView(lowerRowB);
    addView(gaugeAutoShiftBoundsSection);
  }

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
       .rowHeight = 58,
       .buttonFontSize = 15,
       .showLaneOrder = layout.showLaneOrder});
  playOptionSection->setWidth(layout.width);
  addView(playOptionSection);

  addView(makeLabel("Long Note Mode"));
  auto *longNoteModeRow = makeRow();
  for (const char *mode : long_note_mode::kPlayableIds) {
    TextView *text = nullptr;
    auto *button = makeButton(mode, 18, &text);
    button->setOnClickListener([this, mode = std::string(mode)]() {
      if (callbacks.onLongNoteModeSelected) {
        callbacks.onLongNoteModeSelected(mode);
      }
    });
    longNoteModeButtons.push_back({.button = button, .text = text, .id = mode});
    longNoteModeRow->addView(button);
  }
  addView(longNoteModeRow);

  assistOptionLabel = makeLabel("Assist Option");
  addView(assistOptionLabel);
  auto *assistRow = makeRow();
  for (const char *option : {assist_options::kOff, assist_options::kDrag,
                             assist_options::kBpmGuide}) {
    TextView *text = nullptr;
    auto *button = makeButton(option, 18, &text);
    button->setOnClickListener([this, option = std::string(option)]() {
      if (callbacks.onAssistOptionSelected) {
        callbacks.onAssistOptionSelected(option);
      }
    });
    assistOptionButtons.push_back(
        {.button = button, .text = text, .id = option});
    assistRow->addView(button);
  }
  addView(assistRow);

  auto *playbackGroup = new View();
  playbackGroup->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setMargin(Edge::Left, 20)
      ->setPadding(Edge::All, 12)
      ->setThemedBackgroundColor(ui_theme::panelSubtle)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  playbackGroup->addView(makeLabel("Playback Rate"));
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
  playbackRateText->setThemedColor(ui_theme::textPrimary);
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
  playbackModeDropdown->setWidthPercent(100.0f);
  playbackGroup->addView(playbackModeDropdown);
  addView(playbackGroup);

  addView(makeLabel("Club Mode"));
  clubModeButton = makeButton("", 18, nullptr);
  clubModeButton->setName("club-mode");
  clubModeButtonContent = new CheckboxButtonContent("Club Beat", 18, 17);
  clubModeButton->setContentView(clubModeButtonContent);
  clubModeButton->setWidthPercent(100.0f);
  clubModeButton->setOnClickListener([this]() {
    if (callbacks.onClubModeToggled) {
      callbacks.onClubModeToggled();
    }
  });
  addView(clubModeButton);

  if (layout.showPacemaker) {
    addView(makeLabel("Pacemaker"));
    View *row = nullptr;
    for (size_t i = 0; i < pacemaker::kSelectableTargets.size(); ++i) {
      if (i % 3 == 0) {
        row = makeRow();
        addView(row);
      }
      const std::string target = pacemaker::kSelectableTargets[i];
      TextView *text = nullptr;
      auto *button =
          makeButton(pacemaker::displayTargetLabel(target), 18, &text);
      button->setOnClickListener([this, target]() {
        if (callbacks.onPacemakerSelected) {
          callbacks.onPacemakerSelected(target);
        }
      });
      pacemakerButtons.push_back(
          {.button = button, .text = text, .id = target});
      row->addView(button);
    }
  }
}

void PlayOptionsPanelView::refresh(const PlayOptionsPanelState &newState) {
  state = newState;
  for (const auto &item : rulesetButtons) {
    styleButton(item.button, item.text, item.ruleset == state.ruleset, true);
  }
  for (const auto &item : gaugeButtons) {
    styleButton(item.button, item.text, item.gaugeType == state.gaugeType,
                true);
  }
  for (const auto &item : gaugeAutoShiftButtons) {
    styleButton(item.button, item.text,
                item.gaugeAutoShift == state.gaugeAutoShift, true);
  }
  const bool showsBounds =
      state.gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
      state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder;
  if (gaugeSectionLabel != nullptr) {
    gaugeSectionLabel->setText(
        state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
            ? "Gauge / Auto Shift Upper Bound"
            : "Gauge");
  }
  if (gaugeAutoShiftBoundsSection != nullptr) {
    gaugeAutoShiftBoundsSection->setDisplay(showsBounds ? YGDisplayFlex
                                                        : YGDisplayNone);
    gaugeAutoShiftBoundsSection->setVisible(showsBounds);
  }
  for (const auto &item : gaugeLowerBoundButtons) {
    styleButton(item.button, item.text,
                item.gaugeType == state.gaugeAutoShiftLowerBound, true);
  }
  if (playOptionSection != nullptr) {
    playOptionSection->refresh(state.playOption, state.defaultLaneOrder,
                               state.laneOrderEnabled);
  }
  for (const auto &item : longNoteModeButtons) {
    styleButton(item.button, item.text,
                long_note_mode::parseId(item.id) == state.longNoteMode,
                !state.longNoteModeLocked);
  }
  for (const auto &item : assistOptionButtons) {
    styleButton(item.button, item.text,
                assist_options::normalize(item.id) == state.assistOption,
                !state.assistOptionLocked);
  }
  if (assistOptionLabel != nullptr) {
    if (state.playbackRatePercent != 100) {
      assistOptionLabel->setText("Assist Option - Light Assist Easy");
    } else if (assist_options::isEnabled(state.assistOption)) {
      assistOptionLabel->setText("Assist Option - Light Assist Easy");
    } else {
      assistOptionLabel->setText("Assist Option");
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
        {.label = "Mode",
         .selectedId = "pitch-shift",
         .options = {{.id = "pitch-shift", .label = "Pitch Shift"},
                     {.id = "time-stretch",
                      .label = "Time Stretch (Unavailable)",
                      .available = false}},
         .open = playbackModeDropdownOpen,
         .enabled = !state.playbackLocked,
         .maxVisibleItems = 2});
  }
  if (clubModeButton != nullptr && clubModeButtonContent != nullptr) {
    clubModeButtonContent->setChecked(state.clubMode);
    styleButton(clubModeButton, clubModeButtonContent->labelView(),
                state.clubMode, true);
    clubModeButtonContent->setThemedColor(
        state.clubMode
            ? View::ThemeColorProvider{[] {
                return ui_theme::textOn(ui_theme::primaryAction());
              }}
            : View::ThemeColorProvider{ui_theme::textPrimary});
  }
  for (const auto &item : pacemakerButtons) {
    styleButton(item.button, item.text,
                pacemaker::normalizeTargetId(item.id) ==
                    pacemaker::normalizeTargetId(state.pacemakerTarget),
                true);
  }
}

void PlayOptionsPanelView::setLaneOrderMessage(std::string message,
                                               bool error) {
  if (playOptionSection != nullptr) {
    playOptionSection->setLaneOrderMessage(std::move(message), error);
  }
}

void PlayOptionsPanelView::closeDropdowns() {
  playbackModeDropdownOpen = false;
  refresh(state);
}
