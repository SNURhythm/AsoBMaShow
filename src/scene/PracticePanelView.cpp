#include "PracticePanelView.h"

#include "../view/Button.h"
#include "../view/DropdownView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/SnappedSlider.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {
constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";
constexpr std::string_view kLastUsedPresetId = "last-used";

std::string formatMicros(long long micros) {
  const long long totalMillis = std::max(0LL, micros) / 1000LL;
  const long long minutes = totalMillis / 60000LL;
  const long long seconds = totalMillis / 1000LL % 60LL;
  const long long millis = totalMillis % 1000LL;
  std::ostringstream stream;
  stream << minutes << ':' << std::setfill('0') << std::setw(2) << seconds
         << '.' << std::setw(3) << millis;
  return stream.str();
}

TextView *makeText(std::string text, int size, Color color) {
  auto *view = new TextView(kFont, size);
  view->setText(text);
  view->setColor(ui_theme::sdl(color));
  view->setVAlign(TextView::MIDDLE);
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

Button *makeButton(std::string label, int width = 96,
                   TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, width, 44);
  button->setWidth(width);
  button->setHeight(44);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setBackgroundColors(ui_theme::control(), ui_theme::controlHover(),
                              ui_theme::controlPressed());
  button->setBorderColors(ui_theme::hairline(), ui_theme::cyan(),
                          ui_theme::cyan());
  auto *text = makeText(std::move(label), 16, ui_theme::textPrimary());
  text->setAlign(TextView::CENTER);
  if (textOut != nullptr) {
    *textOut = text;
  }
  button->setContentView(text);
  return button;
}

View *makeRow(std::string label, View *control) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Column);
  row->setAlignItems(YGAlignStretch);
  row->setGap(4);
  auto *labelView = makeText(std::move(label), 14, ui_theme::textMuted());
  labelView->setHeight(20);
  row->addView(labelView);
  control->setWidthPercent(100.0f);
  row->addView(control);
  return row;
}

View *makeSliderRow(std::string label, SnappedSlider *slider,
                    TextView **valueOut) {
  auto *group = new View();
  group->setFlexDirection(FlexDirection::Column);
  group->setAlignItems(YGAlignStretch);
  group->setGap(2);

  auto *header = new View();
  header->setHeight(20);
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);
  header->addView(makeText(std::move(label), 14, ui_theme::textMuted()));
  auto *value = makeText("", 14, ui_theme::textPrimary());
  value->setAlign(TextView::RIGHT);
  header->addView(value);
  *valueOut = value;
  group->addView(header);
  slider->setWidthPercent(100.0F);
  group->addView(slider);
  return group;
}
} // namespace

PracticePanelView::PracticePanelView(
    long long chartEndMicros, PracticePanelCallbacks callbacks,
    OverlayPortal *portal,
    std::function<void(practice::Marker)> onMarkerSelected)
    : chartEndMicros(std::max(0LL, chartEndMicros)),
      callbacks(std::move(callbacks)),
      onMarkerSelected(std::move(onMarkerSelected)),
      dropdownOpen(static_cast<size_t>(DropDownIndex::Count), false),
      dropdowns(static_cast<size_t>(DropDownIndex::Count), nullptr) {
  setWidth(370);
  setFlexShrink(0.0f);
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setBackgroundColor(ui_theme::panelStrong());
  setBorderColor(ui_theme::hairline());
  setBorderWidth(1);
  build(portal);
}

void PracticePanelView::build(OverlayPortal *portal) {
  auto *header = makeText("Practice", 26, ui_theme::textPrimary());
  header->setHeight(54);
  header->setPadding(Edge::Left, 18);
  addView(header);

  auto *scroll = new ScrollView();
  scroll->setFlex(1.0f);
  scroll->clearBackgroundColor();
  scroll->setContentPadding(Edge::Left, 16);
  scroll->setContentPadding(Edge::Right, 16);
  scroll->setContentPadding(Edge::Bottom, 18);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignStretch);
  content->setGap(10);

  for (size_t i = 0; i < dropdowns.size(); ++i) {
    const auto index = static_cast<DropDownIndex>(i);
    dropdowns[i] = new DropdownView(
        {.onOpenChanged = [this,
                           index](bool open) { setDropdownOpen(index, open); },
         .onOptionSelected =
             [this, index](const std::string &id) {
               selectDropdownOption(index, id);
             }},
        portal);
  }

  content->addView(
      makeRow("Preset", dropdowns[static_cast<size_t>(DropDownIndex::Preset)]));

  rangeText = makeText("", 16, ui_theme::textSecondary());
  rangeText->setHeight(30);
  content->addView(rangeText);

  diagnosticText = makeText("", 14, ui_theme::textSecondary());
  diagnosticText->setWrap(true);
  diagnosticText->setHeight(42);
  content->addView(diagnosticText);

  auto *markerRow = new View();
  markerRow->setFlexDirection(FlexDirection::Row);
  markerRow->setGap(8);
  auto *startMarkerButton = makeButton("Set Start", 0);
  startMarkerButton->setFlex(1.0f);
  startMarkerButton->setBorderColors(ui_theme::cyan(), ui_theme::cyan(),
                                     ui_theme::cyan());
  startMarkerButton->setOnClickListener([this]() {
    activeMarker = practice::Marker::Start;
    if (onMarkerSelected) {
      onMarkerSelected(activeMarker);
    }
    refreshControls();
  });
  markerRow->addView(startMarkerButton);
  auto *endMarkerButton = makeButton("Set End", 0);
  endMarkerButton->setFlex(1.0f);
  endMarkerButton->setBorderColors(ui_theme::amber(), ui_theme::amber(),
                                   ui_theme::amber());
  endMarkerButton->setOnClickListener([this]() {
    activeMarker = practice::Marker::End;
    if (onMarkerSelected) {
      onMarkerSelected(activeMarker);
    }
    refreshControls();
  });
  markerRow->addView(endMarkerButton);
  content->addView(markerRow);

  loopButton = makeButton("Off", 0, &loopButtonText);
  loopButton->setOnClickListener([this]() {
    currentConfiguration.loop = !currentConfiguration.loop;
    publishConfiguration();
    refreshControls();
  });
  content->addView(makeRow("Loop", loopButton));

  countInSlider = new SnappedSlider([this](int value) {
    currentConfiguration.countInBeats = value;
    publishConfiguration();
    refreshControls();
  });
  content->addView(makeSliderRow("Count-in", countInSlider, &countInValueText));

  content->addView(
      makeRow("Gauge", dropdowns[static_cast<size_t>(DropDownIndex::Gauge)]));
  content->addView(makeRow(
      "Auto Shift",
      dropdowns[static_cast<size_t>(DropDownIndex::GaugeAutoShift)]));
  gaugeLowerBoundRow = makeRow(
      "Auto Shift Lower Bound",
      dropdowns[static_cast<size_t>(DropDownIndex::GaugeLowerBound)]);
  gaugeLowerBoundRow->setDisplay(YGDisplayNone);
  content->addView(gaugeLowerBoundRow);

  startingGaugeSlider = new SnappedSlider([this](int value) {
    currentConfiguration.startingGaugePercent = value;
    publishConfiguration();
    refreshControls();
  });
  auto *startingGaugeGroup = makeSliderRow("Start gauge", startingGaugeSlider,
                                           &startingGaugeValueText);
  startingGaugeDefaultButton =
      makeButton("Use Default", 0, &startingGaugeDefaultText);
  startingGaugeDefaultButton->setWidthPercent(100.0F);
  startingGaugeDefaultButton->setOnClickListener([this]() {
    currentConfiguration.startingGaugePercent.reset();
    publishConfiguration();
    refreshControls();
  });
  startingGaugeGroup->addView(startingGaugeDefaultButton);
  content->addView(startingGaugeGroup);

  judgeSlider = new SnappedSlider([this](int value) {
    currentConfiguration.judge.kind = practice::JudgeOverrideKind::Scale;
    currentConfiguration.judge.scalePercent = value;
    publishConfiguration();
    refreshControls();
  });
  content->addView(makeSliderRow("Judge", judgeSlider, &judgeValueText));

  rateSlider = new SnappedSlider([this](int value) {
    currentConfiguration.playback.percent = value;
    publishConfiguration();
    refreshControls();
  });
  content->addView(makeSliderRow("Rate", rateSlider, &rateValueText));

  content->addView(
      makeRow("Mode", dropdowns[static_cast<size_t>(DropDownIndex::Mode)]));

  presetNameInput = new TextInputBox(kFont, 17);
  presetNameInput->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  presetNameInput->setBackgroundColor(ui_theme::control());
  presetNameInput->setCornerRadius(ui_theme::controlRadius());
  presetNameInput->setBorderColor(ui_theme::hairline());
  presetNameInput->setBorderWidth(1);
  presetNameInput->setPadding(Edge::Left, 12);
  presetNameInput->setPadding(Edge::Right, 12);
  presetNameInput->setHeight(44);
  presetNameInput->setOverflow(TextView::TextOverflow::Hidden);
  content->addView(makeRow("Name", presetNameInput));

  presetMessageText = makeText("", 14, ui_theme::textSecondary());
  presetMessageText->setWrap(true);
  presetMessageText->setHeight(0);
  presetMessageText->setDisplay(YGDisplayNone);
  content->addView(presetMessageText);
  presetMessageSecondLine = makeText("", 14, ui_theme::textSecondary());
  presetMessageSecondLine->setHeight(0);
  presetMessageSecondLine->setDisplay(YGDisplayNone);
  content->addView(presetMessageSecondLine);

  auto *saveRow = new View();
  saveRow->setFlexDirection(FlexDirection::Row);
  saveRow->setGap(8);
  auto *saveButton = makeButton("Save", 0);
  saveButton->setFlex(1.0f);
  saveButton->setOnClickListener([this]() {
    if (callbacks.onSaveAs && presetNameInput != nullptr) {
      callbacks.onSaveAs(presetNameInput->getText());
    }
  });
  saveRow->addView(saveButton);
  renameButton = makeButton("Rename", 0);
  renameButton->setFlex(1.0f);
  renameButton->setOnClickListener([this]() {
    if (callbacks.onRename && presetNameInput != nullptr) {
      callbacks.onRename(presetNameInput->getText());
    }
  });
  saveRow->addView(renameButton);
  content->addView(saveRow);

  auto *mutationRow = new View();
  mutationRow->setFlexDirection(FlexDirection::Row);
  mutationRow->setGap(8);
  updateButton = makeButton("Update", 0);
  updateButton->setFlex(1.0f);
  updateButton->setOnClickListener([this]() {
    if (callbacks.onUpdateNamed) {
      callbacks.onUpdateNamed();
    }
  });
  mutationRow->addView(updateButton);
  deleteButton = makeButton("Delete", 0);
  deleteButton->setFlex(1.0f);
  deleteButton->setOnClickListener([this]() {
    if (callbacks.onDeleteNamed) {
      callbacks.onDeleteNamed();
    }
  });
  mutationRow->addView(deleteButton);
  content->addView(mutationRow);

  startButton = makeButton("Start", 0);
  startButton->setWidthPercent(100.0f);
  startButton->setHeight(52);
  startButton->setBorderColors(ui_theme::cyan(), ui_theme::cyan(),
                               ui_theme::cyan());
  startButton->setOnClickListener([this]() {
    if (callbacks.onStart) {
      callbacks.onStart();
    }
  });
  content->addView(startButton);

  scroll->setContentView(content);
  addView(scroll);
}

void PracticePanelView::refresh(
    const practice::Configuration &configuration,
    const std::vector<practice::NamedPreset> &newNamedPresets,
    std::optional<std::string> selectedPresetId,
    practice::Marker newActiveMarker) {
  const auto previousSelection = selectedNamedPresetId;
  currentConfiguration = configuration;
  namedPresets = newNamedPresets;
  selectedNamedPresetId = std::move(selectedPresetId);
  activeMarker = newActiveMarker;
  if (presetNameInput != nullptr &&
      previousSelection != selectedNamedPresetId) {
    const auto selected =
        selectedNamedPresetId
            ? std::ranges::find(namedPresets, *selectedNamedPresetId,
                                &practice::NamedPreset::id)
            : namedPresets.end();
    presetNameInput->setEditingText(
        selected == namedPresets.end() ? std::string{} : selected->name);
  }
  refreshControls();
}

bool PracticePanelView::isEditingPresetName() const {
  return presetNameInput != nullptr && presetNameInput->getSelected();
}

void PracticePanelView::setPresetMessage(std::string message, bool error) {
  if (presetMessageText == nullptr || presetMessageSecondLine == nullptr) {
    return;
  }
  const bool empty = message.empty();
  std::string secondLine;
  if (message.size() > 40) {
    size_t split = message.rfind(' ', 40);
    if (split == std::string::npos || split < 20) {
      split = 40;
    }
    secondLine = message.substr(split + (message[split] == ' ' ? 1 : 0));
    message.erase(split);
  }
  const SDL_Color color =
      ui_theme::sdl(error ? ui_theme::coral() : ui_theme::cyan());
  const bool hasSecondLine = !secondLine.empty();
  presetMessageText->setText(std::move(message));
  presetMessageText->setColor(color);
  presetMessageText->setHeight(empty ? 0 : 20);
  presetMessageText->setDisplay(empty ? YGDisplayNone : YGDisplayFlex);
  presetMessageSecondLine->setText(std::move(secondLine));
  presetMessageSecondLine->setColor(color);
  presetMessageSecondLine->setHeight(hasSecondLine ? 20 : 0);
  presetMessageSecondLine->setDisplay(hasSecondLine ? YGDisplayFlex
                                                    : YGDisplayNone);
}

void PracticePanelView::setDropdownOpen(DropDownIndex index, bool open) {
  std::fill(dropdownOpen.begin(), dropdownOpen.end(), false);
  dropdownOpen[static_cast<size_t>(index)] = open;
  refreshControls();
}

void PracticePanelView::selectDropdownOption(DropDownIndex index,
                                             const std::string &id) {
  switch (index) {
  case DropDownIndex::Preset:
    if (id == kLastUsedPresetId) {
      selectedNamedPresetId.reset();
    } else {
      const auto selected =
          std::ranges::find(namedPresets, id, &practice::NamedPreset::id);
      if (selected != namedPresets.end()) {
        selectedNamedPresetId = selected->id;
        currentConfiguration = selected->configuration;
        if (presetNameInput != nullptr) {
          presetNameInput->setEditingText(selected->name);
        }
      }
    }
    publishConfiguration();
    break;
  case DropDownIndex::Gauge:
    if (practice::applyPracticeGaugeOption(currentConfiguration, id)) {
      publishConfiguration();
    }
    break;
  case DropDownIndex::GaugeAutoShift:
    if (practice::applyPracticeGaugeAutoShiftOption(currentConfiguration,
                                                    id)) {
      publishConfiguration();
    }
    break;
  case DropDownIndex::GaugeLowerBound:
    if (practice::applyPracticeGaugeLowerBoundOption(currentConfiguration,
                                                     id)) {
      publishConfiguration();
    }
    break;
  case DropDownIndex::Mode:
    if (id == "pitch") {
      currentConfiguration.playback.mode = audio::PlaybackMode::PitchShift;
      publishConfiguration();
    }
    break;
  case DropDownIndex::Count:
    break;
  }
  std::fill(dropdownOpen.begin(), dropdownOpen.end(), false);
  refreshControls();
}

void PracticePanelView::publishConfiguration() {
  currentConfiguration =
      practice::sanitize(currentConfiguration, chartEndMicros).configuration;
  if (callbacks.onChanged) {
    callbacks.onChanged(currentConfiguration);
  }
}

void PracticePanelView::refreshControls() {
  if (rangeText != nullptr) {
    rangeText->setText(
        "Start " + formatMicros(currentConfiguration.startMicros) +
        "  /  End " + formatMicros(currentConfiguration.endMicros) +
        (activeMarker == practice::Marker::Start ? "  /  editing start"
                                                 : "  /  editing end"));
  }

  auto refresh = [&](DropDownIndex index, std::string label,
                     std::string selected,
                     std::vector<DropdownView::Option> options) {
    auto *dropdown = dropdowns[static_cast<size_t>(index)];
    if (dropdown != nullptr) {
      dropdown->refresh({.label = std::move(label),
                         .selectedId = std::move(selected),
                         .options = std::move(options),
                         .open = dropdownOpen[static_cast<size_t>(index)],
                         .enabled = true,
                         .maxVisibleItems = 6,
                         .menuWidth = 330});
    }
  };

  std::vector<DropdownView::Option> presetOptions = {
      {.id = std::string(kLastUsedPresetId), .label = "Last Used"}};
  for (const auto &preset : namedPresets) {
    presetOptions.push_back({.id = preset.id, .label = preset.name});
  }
  refresh(DropDownIndex::Preset, "Preset",
          selectedNamedPresetId.value_or(std::string(kLastUsedPresetId)),
          std::move(presetOptions));
  std::vector<DropdownView::Option> practiceGaugeOptions;
  for (const auto &option : practice::practiceGaugeOptions()) {
    practiceGaugeOptions.push_back(
        {.id = std::string(option.id), .label = std::string(option.label)});
  }
  refresh(DropDownIndex::Gauge, "Gauge",
          practice::practiceGaugeOptionId(currentConfiguration),
          std::move(practiceGaugeOptions));
  std::vector<DropdownView::Option> autoShiftOptions;
  for (const auto &option : practice::practiceGaugeAutoShiftOptions()) {
    autoShiftOptions.push_back(
        {.id = std::string(option.id), .label = std::string(option.label)});
  }
  refresh(DropDownIndex::GaugeAutoShift, "Auto Shift",
          practice::practiceGaugeAutoShiftOptionId(currentConfiguration),
          std::move(autoShiftOptions));
  std::vector<DropdownView::Option> lowerBoundOptions;
  for (const auto &option : practice::practiceGaugeOptions()) {
    lowerBoundOptions.push_back(
        {.id = std::string(option.id), .label = std::string(option.label)});
  }
  refresh(DropDownIndex::GaugeLowerBound, "Auto Shift Lower Bound",
          practice::practiceGaugeLowerBoundOptionId(currentConfiguration),
          std::move(lowerBoundOptions));
  if (gaugeLowerBoundRow != nullptr) {
    const bool visible =
        currentConfiguration.gaugeAutoShift ==
            GaugeAutoShiftMode::BestClear ||
        currentConfiguration.gaugeAutoShift ==
            GaugeAutoShiftMode::SelectToUnder;
    gaugeLowerBoundRow->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
  }
  const std::string modeId =
      currentConfiguration.playback.mode == audio::PlaybackMode::TimeStretch
          ? "stretch"
          : "pitch";
  refresh(DropDownIndex::Mode, "Mode", modeId,
          {{.id = "pitch", .label = "Pitch Shift"},
           {.id = "stretch",
            .label = "Time Stretch (Unavailable)",
            .available = false}});

  if (loopButton != nullptr && loopButtonText != nullptr) {
    loopButtonText->setText(currentConfiguration.loop ? "On" : "Off");
    loopButton->setBackgroundColors(
        currentConfiguration.loop ? ui_theme::primaryAction()
                                  : ui_theme::control(),
        currentConfiguration.loop ? ui_theme::primaryActionHover()
                                  : ui_theme::controlHover(),
        currentConfiguration.loop ? ui_theme::primaryActionPressed()
                                  : ui_theme::controlPressed());
  }
  if (countInSlider != nullptr) {
    countInSlider->refresh({.minimum = 0,
                            .maximum = 16,
                            .step = 1,
                            .value = currentConfiguration.countInBeats});
  }
  if (countInValueText != nullptr) {
    countInValueText->setText(
        std::to_string(currentConfiguration.countInBeats));
  }
  const bool usesDefaultGauge =
      !currentConfiguration.startingGaugePercent.has_value();
  if (startingGaugeSlider != nullptr) {
    startingGaugeSlider->refresh(
        {.minimum = 0,
         .maximum = startingGaugeMaximum,
         .step = 1,
         .value = std::min(
             currentConfiguration.startingGaugePercent.value_or(100),
             startingGaugeMaximum)});
  }
  if (startingGaugeValueText != nullptr) {
    startingGaugeValueText->setText(
        usesDefaultGauge
            ? "Default"
            : std::to_string(std::min(
                  *currentConfiguration.startingGaugePercent,
                  startingGaugeMaximum)) +
                  "%");
  }
  if (startingGaugeDefaultButton != nullptr &&
      startingGaugeDefaultText != nullptr) {
    startingGaugeDefaultText->setText(usesDefaultGauge ? "Default ✓"
                                                       : "Use Default");
    startingGaugeDefaultButton->setBackgroundColors(
        usesDefaultGauge ? ui_theme::primaryAction() : ui_theme::control(),
        usesDefaultGauge ? ui_theme::primaryActionHover()
                         : ui_theme::controlHover(),
        usesDefaultGauge ? ui_theme::primaryActionPressed()
                         : ui_theme::controlPressed());
  }
  if (judgeSlider != nullptr) {
    judgeSlider->refresh({.minimum = 25,
                          .maximum = 200,
                          .step = 5,
                          .value = currentConfiguration.judge.scalePercent});
  }
  if (judgeValueText != nullptr) {
    judgeValueText->setText(
        std::to_string(currentConfiguration.judge.scalePercent) + "%");
  }
  if (rateSlider != nullptr) {
    rateSlider->refresh({.minimum = 50,
                         .maximum = 200,
                         .step = 5,
                         .value = currentConfiguration.playback.percent});
  }
  if (rateValueText != nullptr) {
    rateValueText->setText(
        std::to_string(currentConfiguration.playback.percent) + "%");
  }

  const bool hasNamedPreset = selectedNamedPresetId.has_value();
  if (updateButton != nullptr) {
    updateButton->setEnabled(hasNamedPreset);
  }
  if (renameButton != nullptr) {
    renameButton->setEnabled(hasNamedPreset);
  }
  if (deleteButton != nullptr) {
    deleteButton->setEnabled(hasNamedPreset);
  }
  if (startButton != nullptr) {
    const auto issue =
        practice::firstPlayabilityIssue(currentConfiguration, chartEndMicros);
    startButton->setEnabled(
        !issue &&
        practice::sanitize(currentConfiguration, chartEndMicros).playable());
    if (diagnosticText != nullptr) {
      diagnosticText->setText(issue.value_or("Ready to start practice."));
      diagnosticText->setColor(
          ui_theme::sdl(issue ? ui_theme::coral() : ui_theme::cyan()));
    }
  }
}
