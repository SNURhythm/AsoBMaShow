#include "SettingsSceneShared.h"

#include "SettingsSceneInputLayout.h"
#include "../input/ChartLaneBinding.h"
#include "../input/InputCaptureController.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"
#include "VirtualControllerEditorView.h"

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
    int physicalLane = firstLane + localLane;
    if (scope.player == 1) {
      physicalLane = input_profile::chartLaneForKeyPosition(
                         scope.keyMode, localLane)
                         .value_or(physicalLane);
    }
    result.push_back(
        {.action = {input::LogicalActionKind::Lane, physicalLane},
         .label = "Lane " + std::to_string(localLane + 1)});
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
    return axisControlLabel(control.deviceClass, control.index,
                            control.direction);
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

void SettingsScene::commitGyroscopeTurntableSetting(bool stepAngle,
                                                    std::string_view text) {
  const auto value = parseGyroscopeSettingInteger(text);
  if (!value.has_value()) {
    inputGyroscopeSettingsError = stepAngle
                                      ? "Step angle must be a whole number."
                                      : "Release delay must be a whole number.";
    requestInputViewRebuild();
    return;
  }

  input::GyroscopeTurntableConfig config =
      context.inputProfile.gyroscopeTurntable;
  if (stepAngle) {
    config.stepAngleDegrees = *value;
  } else {
    config.releaseDelayMs = *value;
  }
  if (inputCaptureController->updateGyroscopeTurntableConfig(config)) {
    inputGyroscopeSettingsError.clear();
  } else {
    inputGyroscopeSettingsError =
        inputCaptureController->lastError().empty()
            ? "Failed to save input profile."
            : std::string(inputCaptureController->lastError());
  }
  requestInputViewRebuild();
}

void SettingsScene::commitVirtualControllerSetting(
    input::VirtualControllerConfig config) {
  if (inputCaptureController->updateVirtualControllerConfig(config)) {
    inputVirtualControllerSettingsError.clear();
  } else {
    inputVirtualControllerSettingsError =
        inputCaptureController->lastError().empty()
            ? "Failed to save input profile."
            : std::string(inputCaptureController->lastError());
  }
  requestInputViewRebuild();
}

std::string SettingsScene::inputViewSignature() const {
  if (inputCaptureController == nullptr) {
    return {};
  }
  std::ostringstream output;
  output << inputSelectedPlayer << ':' << inputSelectedKeyMode << ':'
         << inputSelectedDeviceId << ':'
         << static_cast<int>(inputCaptureController->state()) << ':'
         << inputCaptureController->lastError() << ':'
         << inputGyroscopeSettingsError << ':'
         << inputVirtualControllerSettingsError << ':'
         << context.inputProfile.gyroscopeTurntable.stepAngleDegrees << ':'
         << context.inputProfile.gyroscopeTurntable.releaseDelayMs << ':'
         << context.inputProfile.virtualController.enabled << ':'
         << static_cast<int>(context.inputProfile.virtualController.scratchMode)
         << ':'
         << static_cast<int>(context.inputProfile.virtualController.player)
         << ':'
         << context.inputProfile.virtualController.centerX << ':'
         << context.inputProfile.virtualController.centerY << ':'
         << context.inputProfile.virtualController.buttonSize << ':'
         << context.inputProfile.virtualController.keySpacingX << ':'
         << context.inputProfile.virtualController.keySpacingY << ':'
         << context.inputProfile.virtualController.scratchKeyplateSpacing
         << ':' << inputVirtualControllerEditorVisible << ':';
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
           << device.connected << ':' << static_cast<int>(device.status);
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
  const auto sample = inputCaptureController->monitorSample();
  if (sample.has_value() &&
      sample->control.deviceId == input::kGyroscopeTurntableStableId &&
      sample->control.deviceClass == input::DeviceClass::Gyroscope &&
      sample->control.kind == input::ControlKind::Axis &&
      sample->control.index == input::kGyroscopeTurntableAxis &&
      std::isfinite(sample->normalizedValue)) {
    inputGyroscopeAxisValue = sample->normalizedValue;
  }
  for (const auto &device : context.inputDeviceRegistry.snapshot()) {
    if (device.stableId == input::kGyroscopeTurntableStableId &&
        (!device.connected ||
         device.status != input::InputDeviceStatus::Ready)) {
      inputGyroscopeAxisValue = 0.0F;
      break;
    }
  }
  if (inputMonitorText != nullptr) {
    if (shouldShowGyroscopeSettingsCard(inputSelectedDeviceId)) {
      std::ostringstream text;
      text << "Turntable · " << std::fixed << std::setprecision(3)
           << inputGyroscopeAxisValue;
      inputMonitorText->setText(text.str());
    } else if (!sample.has_value()) {
      inputMonitorText->setText("Waiting for input...");
    } else {
      std::ostringstream text;
      text << deviceClassLabel(sample->control.deviceClass) << " · "
           << controlLabel(sample->control) << " · " << std::fixed
           << std::setprecision(3) << sample->rawValue << " / "
           << sample->normalizedValue;
      inputMonitorText->setText(text.str());
    }
  }
  if (inputCaptureStateText != nullptr) {
    switch (inputCaptureController->state()) {
    case InputCaptureController::State::Idle:
      inputCaptureStateText->setText("");
      break;
    case InputCaptureController::State::Listening:
      inputCaptureStateText->setText("Press a control.");
      break;
    case InputCaptureController::State::AwaitingConflictConfirmation:
      inputCaptureStateText->setText("");
      break;
    }
  }
  if (inputErrorText != nullptr) {
    const std::string_view error = inputCaptureController->lastError();
    inputErrorText->setText(
        error.empty() ||
                (shouldShowGyroscopeSettingsCard(inputSelectedDeviceId) &&
                 !inputGyroscopeSettingsError.empty())
            ? std::string()
            : std::string(error));
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

  inputPlayerDropdown =
      new DropdownView({
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
                                 inputSelectedPlayer =
                                     std::clamp(std::stoi(id), 1, 2);
                                 inputPlayerDropdownOpen = false;
                                 requestInputViewRebuild();
                               },
                       },
                       overlayPortal);
  inputKeyModeDropdown =
      new DropdownView({
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
                       },
                       overlayPortal);
  inputDeviceDropdown =
      new DropdownView({
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
                                 inputGyroscopeSettingsError.clear();
                                 inputSelectedDeviceId = id;
                                 inputDeviceDropdownOpen = false;
                                 requestInputViewRebuild();
                               },
                       },
                       overlayPortal);
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
      makeText("Reset Scope", metrics.bodyTextSize + 2,
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
      metrics, "Binding Scope", "Choose player, key mode, and device.",
      selectorBody, metrics.compact ? 280 : 220, metrics.cardsWidth));

  if (gameplay::virtualControllerTouchInputSupported()) {
    auto *virtualControllerBody = new View();
    virtualControllerBody->setFlexDirection(FlexDirection::Column);
    virtualControllerBody->setGap(metrics.compact ? 10.0F : 14.0F);
    const auto virtualControllerConfig = context.inputProfile.virtualController;
    auto *virtualControllerToggle = makeAccentButton(
        std::min(bodyWidth, metrics.actionButtonWidth),
        metrics.actionButtonHeight,
        makeText(virtualControllerConfig.enabled ? "Virtual Controller: On"
                                                 : "Virtual Controller: Off",
                 metrics.bodyTextSize + 1, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE),
        virtualControllerConfig.enabled ? ui_theme::cyan() : ui_theme::coral());
    virtualControllerToggle->setOnClickListener(
        [this, virtualControllerConfig]() {
          auto next = virtualControllerConfig;
          next.enabled = !next.enabled;
          commitVirtualControllerSetting(next);
        });
    virtualControllerBody->addView(virtualControllerToggle);
    if (virtualControllerConfig.enabled) {
      const bool spinScratch = virtualControllerConfig.scratchMode ==
                               input::VirtualControllerScratchMode::Spin;
      auto *scratchModeButton = makeControlButton(
          std::min(bodyWidth, metrics.actionButtonWidth),
          metrics.actionButtonHeight,
          makeText(spinScratch ? "Scratch: Spin Mode" : "Scratch: Flick Mode",
                   metrics.bodyTextSize + 1, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      scratchModeButton->setOnClickListener(
          [this, virtualControllerConfig, spinScratch]() {
            auto next = virtualControllerConfig;
            next.scratchMode = spinScratch
                                   ? input::VirtualControllerScratchMode::Flick
                                   : input::VirtualControllerScratchMode::Spin;
            commitVirtualControllerSetting(next);
          });
      virtualControllerBody->addView(scratchModeButton);
      const bool playerTwo = virtualControllerConfig.player ==
                             input::VirtualControllerPlayer::Player2;
      auto *playerButton = makeControlButton(
          std::min(bodyWidth, metrics.actionButtonWidth),
          metrics.actionButtonHeight,
          makeText(playerTwo ? "Player: 2P" : "Player: 1P",
                   metrics.bodyTextSize + 1, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      playerButton->setOnClickListener(
          [this, virtualControllerConfig, playerTwo]() {
            auto next = virtualControllerConfig;
            next.player = playerTwo ? input::VirtualControllerPlayer::Player1
                                    : input::VirtualControllerPlayer::Player2;
            commitVirtualControllerSetting(next);
          });
      virtualControllerBody->addView(playerButton);
      auto *editButton =
          makeControlButton(std::min(bodyWidth, metrics.actionButtonWidth),
                            metrics.actionButtonHeight,
                            makeText("Edit Layout", metrics.bodyTextSize + 1,
                                     ui_theme::textPrimary(), TextView::CENTER,
                                     TextView::MIDDLE));
      editButton->setOnClickListener([this]() {
        inputVirtualControllerEditorVisible = true;
        requestInputViewRebuild();
      });
      virtualControllerBody->addView(editButton);
    }
    virtualControllerBody->addView(makeWrappedText(
        inputVirtualControllerSettingsError.empty()
            ? ""
            : "Not saved: " + inputVirtualControllerSettingsError,
        metrics.smallTextSize, ui_theme::coral()));
    cards->addView(makeCard(
        metrics, "Virtual Controller", "", virtualControllerBody,
        virtualControllerConfig.enabled ? (metrics.compact ? 340 : 300)
                                        : (metrics.compact ? 220 : 200),
        metrics.cardsWidth));
  }

  const bool showGyroscopeSettings =
      shouldShowGyroscopeSettingsCard(inputSelectedDeviceId);
  if (showGyroscopeSettings) {
    input::InputDeviceStatus status = input::InputDeviceStatus::Disconnected;
    for (const auto &device : context.inputDeviceRegistry.snapshot()) {
      if (device.stableId != input::kGyroscopeTurntableStableId) {
        continue;
      }
      status =
          !device.connected && device.status == input::InputDeviceStatus::Ready
              ? input::InputDeviceStatus::Disconnected
              : device.status;
      break;
    }

    auto *gyroscopeBody = new View();
    gyroscopeBody->setFlexDirection(FlexDirection::Column);
    gyroscopeBody->setGap(metrics.compact ? 10.0F : 12.0F);
    gyroscopeBody->addView(makeWrappedText(
        "Status · " + std::string(inputDeviceStatusLabel(status)),
        metrics.bodyTextSize, ui_theme::textPrimary()));
    inputMonitorText = makeWrappedText(
        "Turntable · 0.000", metrics.bodyTextSize, ui_theme::textSecondary());
    gyroscopeBody->addView(inputMonitorText);

    const GyroscopeSettingsLayout gyroscopeLayout =
        resolveGyroscopeSettingsLayout(bodyWidth, metrics.compact);
    auto *editors = new View();
    editors->setFlexDirection(gyroscopeLayout.stackEditors
                                  ? FlexDirection::Column
                                  : FlexDirection::Row);
    editors->setGap(static_cast<float>(layout.selectorGap));
    editors->setAlignItems(YGAlignStretch);

    auto makeIntegerEditor = [&](std::string_view label, int value,
                                 bool stepAngle) {
      auto *field = new View();
      field->setFlexDirection(FlexDirection::Column);
      field->setGap(4.0F);
      field->setWidth(static_cast<float>(gyroscopeLayout.editorWidth));
      field->setFlexGrow(gyroscopeLayout.stackEditors ? 0.0F : 1.0F);
      field->addView(makeText(std::string(label), metrics.smallTextSize,
                              ui_theme::textMuted()));
      auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize);
      input->setEditingText(std::to_string(value));
      input->setSize(gyroscopeLayout.editorWidth, metrics.actionButtonHeight);
      input->setThemedBackgroundColor(ui_theme::control);
      input->setThemedBorderColor(ui_theme::hairline);
      input->setBorderWidth(1);
      input->setCornerRadius(ui_theme::controlRadius());
      input->setAlign(TextView::CENTER);
      input->setVAlign(TextView::MIDDLE);
      input->setThemedColor(ui_theme::textPrimary);
      input->onEditingFinished([this, stepAngle](const std::string &text) {
        commitGyroscopeTurntableSetting(stepAngle, text);
      });
      field->addView(input);
      return field;
    };

    const auto &config = context.inputProfile.gyroscopeTurntable;
    editors->addView(makeIntegerEditor(kGyroscopeStepAngleLabel,
                                       config.stepAngleDegrees, true));
    editors->addView(makeIntegerEditor(kGyroscopeReleaseDelayLabel,
                                       config.releaseDelayMs, false));
    gyroscopeBody->addView(editors);
    gyroscopeBody->addView(makeWrappedText(
        gyroscopeSettingsErrorLabel(inputGyroscopeSettingsError),
        metrics.smallTextSize, ui_theme::coral()));
    cards->addView(makeCard(
        metrics, "Gyroscope Turntable", "Use device rotation as a turntable.",
        gyroscopeBody, metrics.compact ? 300 : 240, metrics.cardsWidth));
  }

  const bool showInputMonitor =
      !showGyroscopeSettings ||
      inputCaptureController->state() != InputCaptureController::State::Idle;
  if (showInputMonitor) {
    auto *monitorBody = new View();
    monitorBody->setFlexDirection(FlexDirection::Column);
    monitorBody->setGap(metrics.compact ? 8.0F : 12.0F);
    inputCaptureStateText =
        makeWrappedText("", metrics.bodyTextSize, ui_theme::textPrimary());
    if (!showGyroscopeSettings) {
      inputMonitorText =
          makeWrappedText("Waiting for input...", metrics.smallTextSize,
                          ui_theme::textSecondary());
    }
    inputErrorText =
        makeWrappedText("", metrics.smallTextSize, ui_theme::coral());
    monitorBody->addView(inputCaptureStateText);
    if (!showGyroscopeSettings) {
      monitorBody->addView(inputMonitorText);
    }
    monitorBody->addView(inputErrorText);
    if (inputCaptureController->state() !=
        InputCaptureController::State::Idle) {
      auto *cancelButton = makeControlButton(
          metrics.actionButtonWidth, metrics.actionButtonHeight,
          makeText("Cancel", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      cancelButton->setOnClickListener([this]() {
        inputCaptureController->cancel();
        inputCaptureAction.reset();
        requestInputViewRebuild();
      });
      monitorBody->addView(cancelButton);
    }
    cards->addView(makeCard(metrics, "Input Monitor", "", monitorBody,
                            metrics.compact ? 210 : 190, metrics.cardsWidth));
  }

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
        deviceLabel = "Unknown device";
      } else if (device != devices.end() &&
                 !device->second.displayName.empty()) {
        deviceLabel = device->second.displayName;
      } else {
        deviceLabel = binding.control.deviceId;
      }
      if (missing) {
        deviceLabel = "Missing: " + deviceLabel;
      }
      bindingRow->addView(makeWrappedText(
          std::string(deviceClassLabel(binding.control.deviceClass)) + " · " +
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
      auto *unbindButton = makeAccentButton(
          std::max(0, layout.numericControlWidth), metrics.actionButtonHeight,
          makeText("Unbind", metrics.smallTextSize, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          ui_theme::coral());
      unbindButton->setFlexGrow(layout.stackBindingEditor ? 0.0F : 1.0F);
      unbindButton->setOnClickListener([this, bindingId = binding.id]() {
        inputCaptureController->cancel();
        inputCaptureAction.reset();
        inputCaptureController->removeBinding(bindingId);
        requestInputViewRebuild();
      });
      editor->addView(unbindButton);
      bindingRow->addView(editor);
      actionGroup->addView(bindingRow);
    }
    bindingsBody->addView(actionGroup);
  }
  cards->addView(makeCard(
      metrics, "Bindings", "Select Bind, then press a control.",
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
        controlLabel(conflict.control) + " is already " +
            actionLabel(conflict.action) + ".",
        metrics.bodyTextSize, ui_theme::amber()));
  }

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
    if (inputCaptureController->state() ==
        InputCaptureController::State::Idle) {
      inputCaptureAction.reset();
    }
    requestInputViewRebuild();
  });
  actions->addView(replaceButton);

  auto *keepButton = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Keep", metrics.bodyTextSize + 2,
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

void SettingsScene::buildInputVirtualControllerEditorOverlay(
    const LayoutMetrics &metrics) {
  if (activeTab != SettingsTab::Input || !inputVirtualControllerEditorVisible ||
      !context.inputProfile.virtualController.enabled ||
      !gameplay::virtualControllerTouchInputSupported()) {
    return;
  }

  inputVirtualControllerEditorOverlayRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  inputVirtualControllerEditorOverlayRoot->setPositionType(
      YGPositionTypeAbsolute);
  inputVirtualControllerEditorOverlayRoot->setPosition(Edge::Left, 0);
  inputVirtualControllerEditorOverlayRoot->setPosition(Edge::Top, 0);
  inputVirtualControllerEditorOverlayRoot->setZIndex(1060);
  inputVirtualControllerEditorOverlayRoot->setFlexDirection(
      FlexDirection::Column);
  inputVirtualControllerEditorOverlayRoot->setAlignItems(YGAlignStretch);
  inputVirtualControllerEditorOverlayRoot->setGap(metrics.compact ? 10.0F
                                                                    : 14.0F);
  inputVirtualControllerEditorOverlayRoot->setPadding(
      Edge::Top, static_cast<float>(metrics.safe.top + 18));
  inputVirtualControllerEditorOverlayRoot->setPadding(
      Edge::Left, static_cast<float>(metrics.safe.left + 18));
  inputVirtualControllerEditorOverlayRoot->setPadding(
      Edge::Right, static_cast<float>(metrics.safe.right + 18));
  inputVirtualControllerEditorOverlayRoot->setPadding(
      Edge::Bottom, static_cast<float>(metrics.safe.bottom + 18));
  inputVirtualControllerEditorOverlayRoot->setThemedBackgroundColor(
      ui_theme::backdrop);

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);
  auto *title = new View();
  title->setFlexDirection(FlexDirection::Column);
  title->setFlex(1.0F);
  title->setGap(4.0F);
  title->addView(makeText("Virtual Controller Layout", metrics.sectionTitleSize,
                           ui_theme::textPrimary()));
  title->addView(makeWrappedText(
      "Drag the controller to move it. Use the colored handles for size and spacing; the scratch mode is selected on the Input page.",
      metrics.smallTextSize, ui_theme::textSecondary()));
  header->addView(title);

  auto *doneButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Done", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::cyan());
  doneButton->setFlexShrink(0.0F);
  doneButton->setOnClickListener([this]() {
    inputVirtualControllerEditorVisible = false;
    requestInputViewRebuild();
  });
  header->addView(doneButton);
  inputVirtualControllerEditorOverlayRoot->addView(header);

  inputVirtualControllerEditorOverlayRoot->addView(makeWrappedText(
      "Lime: position · Amber: size · Cyan: key X spacing · Violet: key Y spacing · Coral: scratch-to-keyplate spacing. Negative spacing overlaps controls.",
      metrics.smallTextSize, ui_theme::textSecondary()));

  auto *editor = new VirtualControllerEditorView(
      context.inputProfile.virtualController,
      [this](input::VirtualControllerConfig config) {
        commitVirtualControllerSetting(config);
      });
  editor->setFlex(1.0F);
  inputVirtualControllerEditorOverlayRoot->addView(editor);
  rootLayout->addView(inputVirtualControllerEditorOverlayRoot);
}
