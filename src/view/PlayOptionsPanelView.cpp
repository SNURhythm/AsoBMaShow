#include "PlayOptionsPanelView.h"

#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../scene/play/Pacemaker.h"
#include "Button.h"
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
    return gaugeAutoShiftShortLabel(autoShift);
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

  if (layout.showGauge) {
    addView(makeLabel("Gauge"));
    auto addGaugeButton = [this](View *row, GaugeType type,
                                 GaugeAutoShiftMode autoShift) {
      TextView *text = nullptr;
      const std::string label = gaugeLabel(type, autoShift);
      auto *button = makeButton(label, 18, &text);
      button->setOnClickListener([this, type, autoShift]() {
        if (callbacks.onGaugeSelected) {
          callbacks.onGaugeSelected(type, autoShift);
        }
      });
      gaugeButtons.push_back({.button = button,
                              .text = text,
                              .id = label,
                              .gaugeType = type,
                              .gaugeAutoShift = autoShift});
      row->addView(button);
    };
    auto *gaugeRowA = makeRow();
    addGaugeButton(gaugeRowA, GaugeType::AssistedEasy,
                   GaugeAutoShiftMode::None);
    addGaugeButton(gaugeRowA, GaugeType::Easy, GaugeAutoShiftMode::None);
    addGaugeButton(gaugeRowA, GaugeType::Normal, GaugeAutoShiftMode::None);
    addView(gaugeRowA);
    auto *gaugeRowB = makeRow();
    addGaugeButton(gaugeRowB, GaugeType::Hard, GaugeAutoShiftMode::None);
    addGaugeButton(gaugeRowB, GaugeType::ExHard, GaugeAutoShiftMode::None);
    addGaugeButton(gaugeRowB, GaugeType::Hazard, GaugeAutoShiftMode::None);
    addView(gaugeRowB);
    auto *gaugeRowC = makeRow();
    addGaugeButton(gaugeRowC, GaugeType::ExHard,
                   GaugeAutoShiftMode::Continue);
    addGaugeButton(gaugeRowC, GaugeType::ExHard,
                   GaugeAutoShiftMode::SurvivalToGroove);
    addGaugeButton(gaugeRowC, GaugeType::ExHard,
                   GaugeAutoShiftMode::BestClear);
    addGaugeButton(gaugeRowC, GaugeType::ExHard,
                   GaugeAutoShiftMode::SelectToUnder);
    addView(gaugeRowC);
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
  for (const char *option : {assist_options::kOff, assist_options::kDrag}) {
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
  clubModeButton = makeButton("Club Beat", 18, &clubModeButtonText);
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
  for (const auto &item : gaugeButtons) {
    styleButton(item.button, item.text,
                item.gaugeType == state.gaugeType &&
                    item.gaugeAutoShift == state.gaugeAutoShift,
                true);
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
    const bool assistedEasy = state.playbackRatePercent != 100 ||
                              assist_options::isEnabled(state.assistOption);
    assistOptionLabel->setText(assistedEasy ? "Assist Option - A-EASY enabled"
                                            : "Assist Option");
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
  if (clubModeButton != nullptr && clubModeButtonText != nullptr) {
    clubModeButtonText->setText(state.clubMode ? "☑ Club Beat" : "☐ Club Beat");
    styleButton(clubModeButton, clubModeButtonText, state.clubMode, true);
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
