#include "SettingsSceneShared.h"

#include "SettingsSceneInputLayout.h"
#include "../input/InputCaptureController.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"

#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_scancode.h>
#include <SDL2/SDL_touch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

using namespace settings_scene;

namespace {

constexpr std::array<int, 7> kInputKeyModes = {4, 5, 6, 7, 8, 10, 14};
constexpr std::string_view kBlankStableIdFilter = "\x1fmissing-stable-id";

struct ActionDefinition {
  input::LogicalAction action;
  std::string label;
};

View *makeInputCardsColumn(const LayoutMetrics &metrics) {
  auto *column = new View();
  column->setFlexDirection(FlexDirection::Column);
  column->setGap(static_cast<float>(metrics.secondaryGap));
  column->setWidth(static_cast<float>(metrics.cardsWidth));
  return column;
}

std::vector<ActionDefinition> actionsForScope(input::InputScope scope) {
  std::vector<ActionDefinition> result;
  int firstLane = 0;
  int noteLanes = scope.keyMode;
  if (scope.keyMode == 10 || scope.keyMode == 14) {
    noteLanes = scope.keyMode / 2;
    firstLane = scope.player == 1 ? 0 : 8;
  }
  for (int localLane = 0; localLane < noteLanes; ++localLane) {
    result.push_back(
        {.action = {input::LogicalActionKind::Lane, firstLane + localLane},
         .label = "Lane " + std::to_string(localLane + 1)});
  }

  if (scope.keyMode == 5 || scope.keyMode == 7 || scope.keyMode == 10 ||
      scope.keyMode == 14) {
    const int scratchLane = scope.player == 1 ? 7 : 15;
    result.push_back({.action = {input::LogicalActionKind::Lane, scratchLane},
                      .label = "Scratch (digital)"});
  }
  result.push_back({.action = {input::LogicalActionKind::ScratchClockwise, 0},
                    .label = "Scratch clockwise"});
  result.push_back(
      {.action = {input::LogicalActionKind::ScratchCounterClockwise, 0},
       .label = "Scratch counter-clockwise"});
  result.push_back(
      {.action = {input::LogicalActionKind::Start, 0}, .label = "Start"});
  result.push_back(
      {.action = {input::LogicalActionKind::Select, 0}, .label = "Select"});
  result.push_back(
      {.action = {input::LogicalActionKind::Pause, 0}, .label = "Pause"});
  result.push_back(
      {.action = {input::LogicalActionKind::Retry, 0}, .label = "Retry"});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverIncrease, 0},
                    .label = "Lane cover increase"});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverDecrease, 0},
                    .label = "Lane cover decrease"});
  return result;
}

std::string deviceClassLabel(input::DeviceClass deviceClass) {
  switch (deviceClass) {
  case input::DeviceClass::Keyboard:
    return "Keyboard";
  case input::DeviceClass::GameController:
    return "Controller";
  case input::DeviceClass::Joystick:
    return "Joystick";
  case input::DeviceClass::Touch:
    return "Touch";
  case input::DeviceClass::Midi:
    return "MIDI";
  }
  return "Input";
}

std::string directionLabel(input::ControlDirection direction) {
  switch (direction) {
  case input::ControlDirection::Any:
    return {};
  case input::ControlDirection::Negative:
    return "-";
  case input::ControlDirection::Positive:
    return "+";
  case input::ControlDirection::Up:
    return "up";
  case input::ControlDirection::Right:
    return "right";
  case input::ControlDirection::Down:
    return "down";
  case input::ControlDirection::Left:
    return "left";
  }
  return {};
}

std::string controlLabel(const input::PhysicalControl &control) {
  std::string controlName;
  switch (control.kind) {
  case input::ControlKind::Key: {
    const char *name = SDL_GetScancodeName(
        static_cast<SDL_Scancode>(std::max(0, control.index)));
    controlName = name != nullptr && *name != '\0'
                      ? std::string(name)
                      : "Scancode " + std::to_string(control.index);
    break;
  }
  case input::ControlKind::Button:
    controlName = "Button " + std::to_string(control.index);
    break;
  case input::ControlKind::Axis:
    controlName = "Axis " + std::to_string(control.index);
    break;
  case input::ControlKind::Hat:
    controlName = "Hat " + std::to_string(control.index);
    break;
  case input::ControlKind::TouchRegion:
    controlName = "Touch region " + std::to_string(control.index);
    break;
  case input::ControlKind::MidiNote:
    controlName = "Note ch " + std::to_string(control.index / 128 + 1) + " #" +
                  std::to_string(control.index % 128);
    break;
  case input::ControlKind::MidiControl:
    controlName = "CC ch " + std::to_string(control.index / 128 + 1) + " #" +
                  std::to_string(control.index % 128);
    break;
  }
  const std::string direction = directionLabel(control.direction);
  if (!direction.empty()) {
    controlName += " " + direction;
  }
  return controlName;
}

std::string actionLabel(input::LogicalAction action) {
  switch (action.kind) {
  case input::LogicalActionKind::Lane:
    return "lane " + std::to_string(action.lane);
  case input::LogicalActionKind::ScratchClockwise:
    return "scratch clockwise";
  case input::LogicalActionKind::ScratchCounterClockwise:
    return "scratch counter-clockwise";
  case input::LogicalActionKind::Start:
    return "Start";
  case input::LogicalActionKind::Select:
    return "Select";
  case input::LogicalActionKind::Pause:
    return "Pause";
  case input::LogicalActionKind::Retry:
    return "Retry";
  case input::LogicalActionKind::LaneCoverIncrease:
    return "lane cover increase";
  case input::LogicalActionKind::LaneCoverDecrease:
    return "lane cover decrease";
  }
  return "action";
}

std::string formatThreshold(float value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(2) << value;
  return output.str();
}

float parseThreshold(std::string_view text) {
  try {
    std::size_t consumed = 0;
    const float value = std::stof(std::string(text), &consumed);
    if (consumed != text.size()) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    return value;
  } catch (...) {
    return std::numeric_limits<float>::quiet_NaN();
  }
}

bool matchesDeviceFilter(const input::InputBinding &binding,
                         std::string_view filter) {
  if (filter.empty()) {
    return true;
  }
  if (filter == kBlankStableIdFilter) {
    return binding.control.deviceId.empty();
  }
  return binding.control.deviceId == filter;
}

bool inputPointerTransactionActive() {
  if ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0) {
    return true;
  }
  const int touchDeviceCount = SDL_GetNumTouchDevices();
  for (int index = 0; index < touchDeviceCount; ++index) {
    if (SDL_GetNumTouchFingers(SDL_GetTouchDevice(index)) > 0) {
      return true;
    }
  }
  return false;
}

} // namespace

void SettingsScene::ensureInputCaptureController() {
  if (inputCaptureController != nullptr) {
    return;
  }
  inputCaptureController = std::make_unique<InputCaptureController>(
      context.inputDeviceRegistry, context.inputProfile,
      [this](const InputProfile &profile, std::string &error) {
        if (!context.saveActiveInputProfile) {
          error = "The active profile input save operation is unavailable.";
          return false;
        }
        return context.saveActiveInputProfile(profile, error);
      });
}

void SettingsScene::requestInputViewRebuild() {
  if (!inputViewRebuildGate.request()) {
    return;
  }
  View::deferAfterEvent([this]() { inputViewRebuildGate.markEventComplete(); });
}

std::string SettingsScene::inputViewSignature() const {
  if (inputCaptureController == nullptr) {
    return {};
  }
  std::ostringstream output;
  output << inputSelectedPlayer << ':' << inputSelectedKeyMode << ':'
         << inputSelectedDeviceId << ':'
         << static_cast<int>(inputCaptureController->state()) << ':'
         << inputCaptureController->lastError() << ':';
  if (inputCaptureAction.has_value()) {
    output << static_cast<int>(inputCaptureAction->kind) << ':'
           << inputCaptureAction->lane;
  }
  output << std::setprecision(9);
  for (const auto &binding : context.inputProfile.bindings) {
    output << '|' << binding.id << ':' << binding.scope.player << ':'
           << binding.scope.keyMode << ':'
           << static_cast<int>(binding.action.kind) << ':'
           << binding.action.lane << ':' << binding.control.deviceId << ':'
           << static_cast<int>(binding.control.deviceClass) << ':'
           << static_cast<int>(binding.control.kind) << ':'
           << binding.control.index << ':'
           << static_cast<int>(binding.control.direction) << ':'
           << binding.deadZone << ':' << binding.activationThreshold << ':'
           << binding.releaseThreshold << ':' << binding.inverted;
  }
  for (const auto &conflict : inputCaptureController->pendingConflicts()) {
    output << "|conflict:" << conflict.id;
  }
  for (const auto &device : context.inputDeviceRegistry.snapshot()) {
    output << "|device:" << device.stableId << ':' << device.displayName << ':'
           << device.connected;
  }
  return output.str();
}

void SettingsScene::updateInputSettingsState() {
  ensureInputCaptureController();
  if (inputCaptureController->state() == InputCaptureController::State::Idle) {
    inputCaptureAction.reset();
  }
  refreshInputMonitorText();
  if (activeTab != SettingsTab::Input) {
    return;
  }
  const std::string signature = inputViewSignature();
  if (!inputLastViewSignature.empty() && signature != inputLastViewSignature) {
    inputViewRebuildGate.noticeStateChange();
  }
  if (inputViewRebuildGate.consume(inputPointerTransactionActive())) {
    inputLastViewSignature.clear();
    lastLayoutWidth = -1;
  }
}

void SettingsScene::refreshInputMonitorText() {
  if (inputCaptureController == nullptr) {
    return;
  }
  if (inputMonitorText != nullptr) {
    const auto sample = inputCaptureController->monitorSample();
    if (!sample.has_value()) {
      inputMonitorText->setText("Move or press an input to monitor it.");
    } else {
      std::ostringstream text;
      text << deviceClassLabel(sample->control.deviceClass) << " · "
           << (sample->control.deviceId.empty() ? "missing stable ID"
                                                : sample->control.deviceId)
           << " · " << controlLabel(sample->control) << " · raw " << std::fixed
           << std::setprecision(3) << sample->rawValue << " · normalized "
           << sample->normalizedValue;
      inputMonitorText->setText(text.str());
    }
  }
  if (inputCaptureStateText != nullptr) {
    switch (inputCaptureController->state()) {
    case InputCaptureController::State::Idle:
      inputCaptureStateText->setText("Capture idle");
      break;
    case InputCaptureController::State::Listening:
      inputCaptureStateText->setText(
          "Listening — press a key/button or cross 50% on an axis.");
      break;
    case InputCaptureController::State::AwaitingConflictConfirmation:
      inputCaptureStateText->setText(
          "Conflict found — choose Replace or Keep existing.");
      break;
    }
  }
  if (inputErrorText != nullptr) {
    const std::string_view error = inputCaptureController->lastError();
    inputErrorText->setText(error.empty() ? std::string()
                                          : "Not saved: " + std::string(error));
  }
}

void SettingsScene::refreshInputDropdowns() {
  const std::vector<DropdownView::Option> playerOptions = {
      {.id = "1", .label = "Player 1"},
      {.id = "2", .label = "Player 2"},
  };
  std::vector<DropdownView::Option> keyModeOptions;
  for (const int keyMode : kInputKeyModes) {
    keyModeOptions.push_back({.id = std::to_string(keyMode),
                              .label = std::to_string(keyMode) + " key"});
  }

  std::vector<DropdownView::Option> deviceOptions = {
      {.id = "", .label = "All devices"}};
  std::set<std::string> included;
  const auto devices = context.inputDeviceRegistry.snapshot();
  for (const auto &device : devices) {
    std::string label =
        device.displayName.empty() ? device.stableId : device.displayName;
    label += device.connected ? "" : " (missing)";
    deviceOptions.push_back({.id = device.stableId, .label = std::move(label)});
    included.insert(device.stableId);
  }
  const input::InputScope scope{inputSelectedPlayer, inputSelectedKeyMode};
  bool blankStableIdIncluded = false;
  for (const auto &binding : context.inputProfile.bindings) {
    if (binding.scope != scope) {
      continue;
    }
    if (binding.control.deviceId.empty()) {
      blankStableIdIncluded = true;
      continue;
    }
    if (!included.contains(binding.control.deviceId)) {
      deviceOptions.push_back(
          {.id = binding.control.deviceId,
           .label = "Missing: " + binding.control.deviceId});
      included.insert(binding.control.deviceId);
    }
  }
  if (blankStableIdIncluded) {
    deviceOptions.push_back({.id = std::string(kBlankStableIdFilter),
                             .label = "Missing stable ID"});
  }

  if (inputPlayerDropdown != nullptr) {
    inputPlayerDropdown->refresh(
        {.label = "Player",
         .selectedId = std::to_string(inputSelectedPlayer),
         .options = playerOptions,
         .open = inputPlayerDropdownOpen});
  }
  if (inputKeyModeDropdown != nullptr) {
    inputKeyModeDropdown->refresh(
        {.label = "Mode",
         .selectedId = std::to_string(inputSelectedKeyMode),
         .options = std::move(keyModeOptions),
         .open = inputKeyModeDropdownOpen,
         .maxVisibleItems = 7});
  }
  if (inputDeviceDropdown != nullptr) {
    inputDeviceDropdown->refresh({.label = "Device",
                                  .selectedId = inputSelectedDeviceId,
                                  .options = std::move(deviceOptions),
                                  .open = inputDeviceDropdownOpen,
                                  .maxVisibleItems = 7});
  }
}

View *SettingsScene::buildInputTab(const LayoutMetrics &metrics) {
  ensureInputCaptureController();
  auto *cards = makeInputCardsColumn(metrics);
  const int bodyWidth =
      std::max(0, metrics.cardsWidth - metrics.cardPadding * 2);
  const InputSettingsLayout layout =
      resolveInputSettingsLayout(bodyWidth, metrics.compact);

  auto *selectorBody = new View();
  selectorBody->setFlexDirection(layout.stackSelectors ? FlexDirection::Column
                                                       : FlexDirection::Row);
  selectorBody->setFlexWrap(YGWrapWrap);
  selectorBody->setGap(static_cast<float>(layout.selectorGap));
  selectorBody->setAlignItems(YGAlignStretch);

  inputPlayerDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            inputPlayerDropdownOpen = open;
            if (open) {
              inputKeyModeDropdownOpen = false;
              inputDeviceDropdownOpen = false;
            }
            refreshInputDropdowns();
          },
      .onOptionSelected =
          [this](const std::string &id) {
            inputCaptureController->cancel();
            inputCaptureAction.reset();
            inputSelectedPlayer = std::clamp(std::stoi(id), 1, 2);
            inputPlayerDropdownOpen = false;
            requestInputViewRebuild();
          },
  });
  inputKeyModeDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            inputKeyModeDropdownOpen = open;
            if (open) {
              inputPlayerDropdownOpen = false;
              inputDeviceDropdownOpen = false;
            }
            refreshInputDropdowns();
          },
      .onOptionSelected =
          [this](const std::string &id) {
            inputCaptureController->cancel();
            inputCaptureAction.reset();
            inputSelectedKeyMode = std::stoi(id);
            inputKeyModeDropdownOpen = false;
            requestInputViewRebuild();
          },
  });
  inputDeviceDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            inputDeviceDropdownOpen = open;
            if (open) {
              inputPlayerDropdownOpen = false;
              inputKeyModeDropdownOpen = false;
            }
            refreshInputDropdowns();
          },
      .onOptionSelected =
          [this](const std::string &id) {
            inputCaptureController->cancel();
            inputCaptureAction.reset();
            inputSelectedDeviceId = id;
            inputDeviceDropdownOpen = false;
            requestInputViewRebuild();
          },
  });
  for (auto *dropdown :
       {inputPlayerDropdown, inputKeyModeDropdown, inputDeviceDropdown}) {
    dropdown->setWidth(static_cast<float>(layout.selectorWidth));
    dropdown->setFlexGrow(layout.stackSelectors ? 0.0F : 1.0F);
    selectorBody->addView(dropdown);
  }
  refreshInputDropdowns();

  auto *resetRow = new View();
  resetRow->setFlexDirection(FlexDirection::Row);
  resetRow->setFlexWrap(YGWrapWrap);
  resetRow->setGap(static_cast<float>(layout.selectorGap));
  resetRow->setAlignItems(YGAlignCenter);
  auto *resetButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Reset this scope", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::coral());
  resetButton->setOnClickListener([this]() {
    inputCaptureController->cancel();
    inputCaptureAction.reset();
    inputCaptureController->resetScopeToDefaults(
        {inputSelectedPlayer, inputSelectedKeyMode});
    requestInputViewRebuild();
  });
  resetRow->addView(resetButton);
  selectorBody->addView(resetRow);
  cards->addView(makeCard(
      metrics, "Binding scope",
      "Bindings are isolated by player and BMS key mode. The device selector "
      "filters this editor without deleting hidden bindings.",
      selectorBody, metrics.compact ? 280 : 220, metrics.cardsWidth));

  auto *monitorBody = new View();
  monitorBody->setFlexDirection(FlexDirection::Column);
  monitorBody->setGap(metrics.compact ? 8.0F : 12.0F);
  inputCaptureStateText = makeWrappedText("Capture idle", metrics.bodyTextSize,
                                          ui_theme::textPrimary());
  inputMonitorText =
      makeWrappedText("Move or press an input to monitor it.",
                      metrics.smallTextSize, ui_theme::textSecondary());
  inputErrorText =
      makeWrappedText("", metrics.smallTextSize, ui_theme::coral());
  monitorBody->addView(inputCaptureStateText);
  monitorBody->addView(inputMonitorText);
  monitorBody->addView(inputErrorText);
  if (inputCaptureController->state() != InputCaptureController::State::Idle) {
    auto *cancelButton = makeControlButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText("Cancel capture", metrics.bodyTextSize + 2,
                 ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
    cancelButton->setOnClickListener([this]() {
      inputCaptureController->cancel();
      inputCaptureAction.reset();
      requestInputViewRebuild();
    });
    monitorBody->addView(cancelButton);
  }
  cards->addView(makeCard(
      metrics, "Live input monitor",
      "Raw samples remain visible even below the binding threshold. Capture "
      "ignores repeats and axis noise until a fresh activation crossing.",
      monitorBody, metrics.compact ? 210 : 190, metrics.cardsWidth));

  auto *bindingsBody = new View();
  bindingsBody->setFlexDirection(FlexDirection::Column);
  bindingsBody->setGap(metrics.compact ? 16.0F : 20.0F);
  const input::InputScope scope{inputSelectedPlayer, inputSelectedKeyMode};
  const std::map<std::string, input::InputDeviceSnapshot> devices = [&]() {
    std::map<std::string, input::InputDeviceSnapshot> result;
    for (const auto &device : context.inputDeviceRegistry.snapshot()) {
      result.emplace(device.stableId, device);
    }
    return result;
  }();

  for (const auto &definition : actionsForScope(scope)) {
    auto *actionGroup = new View();
    actionGroup->setFlexDirection(FlexDirection::Column);
    actionGroup->setGap(metrics.compact ? 8.0F : 10.0F);
    actionGroup->setPadding(Edge::All,
                            static_cast<float>(metrics.compact ? 12 : 16));
    actionGroup->setThemedBackgroundColor(ui_theme::panelSubtle);
    actionGroup->setThemedBorderColor(ui_theme::hairlineSubtle);
    actionGroup->setBorderWidth(1);
    actionGroup->setCornerRadius(ui_theme::controlRadius());

    auto *actionHeader = new View();
    actionHeader->setFlexDirection(FlexDirection::Row);
    actionHeader->setFlexWrap(YGWrapWrap);
    actionHeader->setAlignItems(YGAlignCenter);
    actionHeader->setJustifyContent(YGJustifySpaceBetween);
    actionHeader->setGap(static_cast<float>(layout.selectorGap));
    actionHeader->addView(makeWrappedText(
        definition.label, metrics.bodyTextSize + 2, ui_theme::textPrimary()));
    const bool listeningForAction =
        inputCaptureController->state() ==
            InputCaptureController::State::Listening &&
        inputCaptureAction.has_value() &&
        *inputCaptureAction == definition.action;
    auto *bindButton = makeAccentButton(
        metrics.compact ? 150 : 190, metrics.actionButtonHeight,
        makeText(listeningForAction ? "Listening..." : "Bind",
                 metrics.bodyTextSize + 1, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE),
        listeningForAction ? ui_theme::amber() : ui_theme::cyan());
    bindButton->setOnClickListener([this, action = definition.action]() {
      inputCaptureAction = action;
      inputCaptureController->begin({inputSelectedPlayer, inputSelectedKeyMode},
                                    action);
      requestInputViewRebuild();
    });
    actionHeader->addView(bindButton);
    actionGroup->addView(actionHeader);

    std::vector<input::InputBinding> visibleBindings;
    for (const auto &binding : context.inputProfile.bindings) {
      if (binding.scope == scope && binding.action == definition.action &&
          matchesDeviceFilter(binding, inputSelectedDeviceId)) {
        visibleBindings.push_back(binding);
      }
    }
    if (visibleBindings.empty()) {
      actionGroup->addView(makeWrappedText(
          inputSelectedDeviceId.empty() ? "Unbound"
                                        : "No binding for this device filter.",
          metrics.smallTextSize, ui_theme::textMuted()));
    }

    for (const auto &binding : visibleBindings) {
      auto *bindingRow = new View();
      bindingRow->setFlexDirection(FlexDirection::Column);
      bindingRow->setGap(metrics.compact ? 8.0F : 10.0F);
      bindingRow->setPadding(Edge::Top, 8.0F);
      bindingRow->setThemedBorderColor(ui_theme::hairlineSubtle);
      bindingRow->setBorderWidth(1);

      const auto device = devices.find(binding.control.deviceId);
      const bool missing =
          inputCaptureController->isBindingDeviceMissing(binding.id);
      std::string deviceLabel;
      if (binding.control.deviceId.empty()) {
        deviceLabel = "Missing stable ID";
      } else if (device != devices.end() &&
                 !device->second.displayName.empty()) {
        deviceLabel =
            device->second.displayName + " · " + binding.control.deviceId;
      } else {
        deviceLabel = binding.control.deviceId;
      }
      if (missing) {
        deviceLabel = "Missing: " + deviceLabel;
      }
      bindingRow->addView(makeWrappedText(
          deviceClassLabel(binding.control.deviceClass) + " · " +
              controlLabel(binding.control) + " · " + deviceLabel,
          metrics.smallTextSize,
          missing ? ui_theme::amber() : ui_theme::textSecondary()));

      auto *editor = new View();
      editor->setFlexDirection(layout.stackBindingEditor ? FlexDirection::Column
                                                         : FlexDirection::Row);
      editor->setFlexWrap(YGWrapWrap);
      editor->setGap(static_cast<float>(layout.selectorGap));
      editor->setAlignItems(YGAlignStretch);

      auto makeThresholdField = [&](std::string label, float value,
                                    auto onCommit) {
        auto *field = new View();
        field->setFlexDirection(FlexDirection::Column);
        field->setGap(4.0F);
        field->setWidth(static_cast<float>(layout.numericControlWidth));
        field->setFlexGrow(layout.stackBindingEditor ? 0.0F : 1.0F);
        field->addView(
            makeText(label, metrics.smallTextSize, ui_theme::textMuted()));
        auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize);
        input->setEditingText(formatThreshold(value));
        input->setSize(layout.numericControlWidth, metrics.actionButtonHeight);
        input->setThemedBackgroundColor(ui_theme::control);
        input->setThemedBorderColor(ui_theme::hairline);
        input->setBorderWidth(1);
        input->setCornerRadius(ui_theme::controlRadius());
        input->setAlign(TextView::CENTER);
        input->setVAlign(TextView::MIDDLE);
        input->setThemedColor(ui_theme::textPrimary);
        input->onEditingFinished(
            [onCommit = std::move(onCommit)](const std::string &text) {
              onCommit(parseThreshold(text));
            });
        field->addView(input);
        return field;
      };

      editor->addView(
          makeThresholdField("Dead zone", binding.deadZone,
                             [this, bindingId = binding.id](float value) {
                               inputCaptureController->updateBinding(
                                   bindingId, {.deadZone = value});
                               requestInputViewRebuild();
                             }));
      editor->addView(
          makeThresholdField("Activate", binding.activationThreshold,
                             [this, bindingId = binding.id](float value) {
                               inputCaptureController->updateBinding(
                                   bindingId, {.activationThreshold = value});
                               requestInputViewRebuild();
                             }));
      editor->addView(
          makeThresholdField("Release", binding.releaseThreshold,
                             [this, bindingId = binding.id](float value) {
                               inputCaptureController->updateBinding(
                                   bindingId, {.releaseThreshold = value});
                               requestInputViewRebuild();
                             }));
      auto *invertButton = makeControlButton(
          std::max(0, layout.numericControlWidth), metrics.actionButtonHeight,
          makeText(binding.inverted ? "Inverted: On" : "Inverted: Off",
                   metrics.smallTextSize, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      invertButton->setFlexGrow(layout.stackBindingEditor ? 0.0F : 1.0F);
      invertButton->setOnClickListener([this, bindingId = binding.id]() {
        inputCaptureController->toggleBindingInversion(bindingId);
        requestInputViewRebuild();
      });
      editor->addView(invertButton);
      bindingRow->addView(editor);
      actionGroup->addView(bindingRow);
    }
    bindingsBody->addView(actionGroup);
  }
  cards->addView(makeCard(
      metrics, "Bindings",
      "Press Bind, then activate a control. Threshold and inversion edits "
      "save only when their editor is committed.",
      bindingsBody, metrics.compact ? 400 : 360, metrics.cardsWidth));

  refreshInputMonitorText();
  inputLastViewSignature = inputViewSignature();
  return cards;
}

void SettingsScene::buildInputConflictOverlay(const LayoutMetrics &metrics) {
  if (activeTab != SettingsTab::Input || inputCaptureController == nullptr ||
      inputCaptureController->state() !=
          InputCaptureController::State::AwaitingConflictConfirmation) {
    return;
  }

  inputConflictOverlayRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  inputConflictOverlayRoot->setPositionType(YGPositionTypeAbsolute);
  inputConflictOverlayRoot->setPosition(Edge::Left, 0);
  inputConflictOverlayRoot->setPosition(Edge::Top, 0);
  inputConflictOverlayRoot->setZIndex(1050);
  inputConflictOverlayRoot->setFlexDirection(FlexDirection::Column);
  inputConflictOverlayRoot->setAlignItems(YGAlignCenter);
  inputConflictOverlayRoot->setJustifyContent(YGJustifyCenter);
  inputConflictOverlayRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(static_cast<float>(std::min(
      metrics.compact ? 620 : 760,
      std::max(280, metrics.contentWidth - 32))));
  panel->setMinHeight(static_cast<float>(metrics.compact ? 260 : 300));
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setGap(metrics.compact ? 14.0F : 18.0F);
  panel->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setCornerRadius(ui_theme::panelRadius());
  panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);
  panel->setThemedBorderColor(ui_theme::hairline);
  panel->setBorderWidth(1);
  panel->addView(makeWrappedText("Binding conflict", metrics.sectionTitleSize,
                                 ui_theme::textPrimary()));

  for (const auto &conflict : inputCaptureController->pendingConflicts()) {
    panel->addView(makeWrappedText(
        controlLabel(conflict.control) + " is currently assigned to " +
            actionLabel(conflict.action) + " in this scope.",
        metrics.bodyTextSize, ui_theme::amber()));
  }
  panel->addView(makeWrappedText(
      "The profile is unchanged until Replace is explicitly confirmed.",
      metrics.bodyTextSize, ui_theme::textSecondary()));

  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(metrics.compact ? 10.0F : 14.0F);
  actions->setJustifyContent(YGJustifyCenter);
  auto *replaceButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Replace", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::amber());
  replaceButton->setOnClickListener([this]() {
    inputCaptureController->confirmReplace();
    inputCaptureAction.reset();
    requestInputViewRebuild();
  });
  actions->addView(replaceButton);

  auto *keepButton = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Keep existing", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
  keepButton->setOnClickListener([this]() {
    inputCaptureController->rejectReplace();
    inputCaptureAction.reset();
    requestInputViewRebuild();
  });
  actions->addView(keepButton);
  panel->addView(actions);

  inputConflictOverlayRoot->addView(panel);
  rootLayout->addView(inputConflictOverlayRoot);
}
