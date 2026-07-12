#include "PracticePanelView.h"

#include "../view/Button.h"
#include "../view/DropdownView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <array>
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

Button *makeButton(std::string label, int width = 96) {
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

std::vector<DropdownView::Option>
numericOptions(int minimum, int maximum, int step, std::string suffix = {}) {
  std::vector<DropdownView::Option> options;
  for (int value = minimum; value <= maximum; value += step) {
    options.push_back(
        {.id = std::to_string(value), .label = std::to_string(value) + suffix});
  }
  return options;
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

  auto *shortcutLabel = makeText("Shortcuts", 13, ui_theme::textMuted());
  shortcutLabel->setHeight(19);
  content->addView(shortcutLabel);
  auto *shortcutText = makeText(
      "1/2 or LB/RB: select marker\nLeft/Right or D-pad: step timeline", 13,
      ui_theme::textMuted());
  shortcutText->setWrap(true);
  shortcutText->setHeight(42);
  content->addView(shortcutText);

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

  const std::array<std::pair<DropDownIndex, const char *>, 7> fields = {{
      {DropDownIndex::Loop, "Loop"},
      {DropDownIndex::CountIn, "Count-in beats"},
      {DropDownIndex::Gauge, "Gauge"},
      {DropDownIndex::StartingGauge, "Starting gauge"},
      {DropDownIndex::Judge, "Judge windows"},
      {DropDownIndex::Rate, "Playback rate"},
      {DropDownIndex::Mode, "Playback mode"},
  }};
  for (const auto &[index, label] : fields) {
    content->addView(makeRow(label, dropdowns[static_cast<size_t>(index)]));
  }

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
  content->addView(makeRow("Preset name", presetNameInput));

  presetMessageText = makeText("", 14, ui_theme::textSecondary());
  presetMessageText->setWrap(true);
  presetMessageText->setHeight(0);
  presetMessageText->setDisplay(YGDisplayNone);
  content->addView(presetMessageText);

  auto *saveRow = new View();
  saveRow->setFlexDirection(FlexDirection::Row);
  saveRow->setGap(8);
  auto *saveButton = makeButton("Save As", 0);
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

  startButton = makeButton("Start Practice", 0);
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
  if (presetMessageText == nullptr) {
    return;
  }
  const bool empty = message.empty();
  presetMessageText->setText(std::move(message));
  presetMessageText->setColor(
      ui_theme::sdl(error ? ui_theme::coral() : ui_theme::cyan()));
  presetMessageText->setHeight(empty ? 0 : 40);
  presetMessageText->setDisplay(empty ? YGDisplayNone : YGDisplayFlex);
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
  case DropDownIndex::Loop:
    currentConfiguration.loop = id == "on";
    publishConfiguration();
    break;
  case DropDownIndex::CountIn:
    currentConfiguration.countInBeats = std::stoi(id);
    publishConfiguration();
    break;
  case DropDownIndex::Gauge:
    if (practice::applyPracticeGaugeOption(currentConfiguration, id)) {
      publishConfiguration();
    }
    break;
  case DropDownIndex::StartingGauge:
    currentConfiguration.startingGaugePercent =
        id == "default" ? std::nullopt : std::optional<int>(std::stoi(id));
    publishConfiguration();
    break;
  case DropDownIndex::Judge:
    currentConfiguration.judge.kind = practice::JudgeOverrideKind::Scale;
    currentConfiguration.judge.scalePercent = std::stoi(id);
    publishConfiguration();
    break;
  case DropDownIndex::Rate:
    currentConfiguration.playback.percent = std::stoi(id);
    publishConfiguration();
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
  refresh(DropDownIndex::Loop, "Loop", currentConfiguration.loop ? "on" : "off",
          {{.id = "off", .label = "Off"}, {.id = "on", .label = "On"}});
  refresh(DropDownIndex::CountIn, "Beats",
          std::to_string(currentConfiguration.countInBeats),
          numericOptions(0, 16, 1));
  std::vector<DropdownView::Option> practiceGaugeOptions;
  for (const auto &option : practice::practiceGaugeOptions()) {
    practiceGaugeOptions.push_back(
        {.id = std::string(option.id), .label = std::string(option.label)});
  }
  refresh(DropDownIndex::Gauge, "Gauge",
          practice::practiceGaugeOptionId(currentConfiguration),
          std::move(practiceGaugeOptions));
  auto gaugeOptions = numericOptions(0, 100, 1, "%");
  gaugeOptions.insert(gaugeOptions.begin(),
                      {.id = "default", .label = "Default"});
  refresh(DropDownIndex::StartingGauge, "Start",
          currentConfiguration.startingGaugePercent
              ? std::to_string(*currentConfiguration.startingGaugePercent)
              : "default",
          std::move(gaugeOptions));
  refresh(DropDownIndex::Judge, "Judge",
          std::to_string(currentConfiguration.judge.scalePercent),
          numericOptions(25, 200, 5, "%"));
  refresh(DropDownIndex::Rate, "Rate",
          std::to_string(currentConfiguration.playback.percent),
          numericOptions(50, 200, 5, "%"));
  const std::string modeId =
      currentConfiguration.playback.mode == audio::PlaybackMode::TimeStretch
          ? "stretch"
          : "pitch";
  refresh(DropDownIndex::Mode, "Mode", modeId,
          {{.id = "pitch", .label = "Pitch Shift"},
           {.id = "stretch",
            .label = "Time Stretch (Unavailable)",
            .available = false}});

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
