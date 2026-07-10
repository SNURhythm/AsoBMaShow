#include "SettingsSceneShared.h"

#include "../RAII.h"
#include "../path.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"
#include "../view/ScrollView.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

using namespace settings_scene;

namespace {
View *makeAudioVideoCardsColumn(const LayoutMetrics &metrics) {
  auto *cardsColumn = new View();
  cardsColumn->setFlexDirection(FlexDirection::Column);
  cardsColumn->setGap(static_cast<float>(metrics.secondaryGap));
  cardsColumn->setWidth(static_cast<float>(metrics.cardsWidth));
  return cardsColumn;
}

std::vector<DropdownView::Option>
dropdownOptions(const ChoiceControlModel &control) {
  std::vector<DropdownView::Option> options;
  options.reserve(control.options.size());
  for (const auto &option : control.options) {
    options.push_back({.id = option.persistedValue,
                       .label = option.label,
                       .available = option.available});
  }
  return options;
}

void refreshDropdown(DropdownView *dropdown, const ChoiceControlModel &control,
                     bool open, const char *label, float width) {
  if (dropdown == nullptr) {
    return;
  }
  dropdown->setWidth(width);
  dropdown->refresh({.label = label,
                     .selectedId = control.selectedValue,
                     .options = dropdownOptions(control),
                     .open = open,
                     .enabled = control.enabled,
                     .maxVisibleItems = 7,
                     .menuWidth = width});
}

View *makeChoiceField(const LayoutMetrics &metrics, const std::string &title,
                      DropdownView *dropdown, const std::string &explanation) {
  auto *field = new View();
  field->setFlexDirection(FlexDirection::Column);
  field->setGap(metrics.compact ? 7.0F : 9.0F);
  field->setMinWidth(static_cast<float>(metrics.compact ? 260 : 320));
  field->setFlex(1.0F);
  field->addView(
      makeText(title, metrics.bodyTextSize, ui_theme::textSecondary()));
  field->addView(dropdown);
  if (!explanation.empty()) {
    field->addView(makeWrappedText(explanation, metrics.smallTextSize,
                                   ui_theme::textMuted()));
  }
  return field;
}

std::uint32_t parseUnsigned(const std::string &text,
                            std::uint32_t fallback = 0) {
  try {
    const auto parsed = std::stoull(text);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
      return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (const std::exception &) {
    return fallback;
  }
}

int parseInt(const std::string &text, int fallback = 0) {
  try {
    return std::stoi(text);
  } catch (const std::exception &) {
    return fallback;
  }
}

player_settings::DisplayMode parseDisplayMode(const std::string &value) {
  if (value == "borderless") {
    return player_settings::DisplayMode::BorderlessFullscreen;
  }
  if (value == "exclusive") {
    return player_settings::DisplayMode::ExclusiveFullscreen;
  }
  return player_settings::DisplayMode::Windowed;
}

bool parseResolution(const std::string &value, int &width, int &height) {
  int parsedWidth = 0;
  int parsedHeight = 0;
  char trailing = '\0';
  if (std::sscanf(value.c_str(), "%dx%d%c", &parsedWidth, &parsedHeight,
                  &trailing) != 2 ||
      parsedWidth <= 0 || parsedHeight <= 0) {
    return false;
  }
  width = parsedWidth;
  height = parsedHeight;
  return true;
}

std::string audioApplyMessage(const audio::ApplyResult &result) {
  if (!result.message.empty()) {
    return result.message;
  }
  switch (result.status) {
  case audio::ApplyStatus::Applied:
    return "Audio settings applied.";
  case audio::ApplyStatus::Unsupported:
    return "This audio configuration is unavailable.";
  case audio::ApplyStatus::FailedRolledBack:
    return "Audio restart failed; the previous stream was restored.";
  case audio::ApplyStatus::FailedStopped:
    return "Audio could not be restored and playback was stopped.";
  }
  return "Audio settings were not applied.";
}

SDL_Color audioApplyColor(audio::ApplyStatus status) {
  return status == audio::ApplyStatus::Applied ? SDL_Color{157, 220, 176, 255}
                                               : SDL_Color{255, 177, 170, 255};
}

std::string displayApplyMessage(const display::ApplyResult &result) {
  if (!result.message.empty()) {
    return result.message;
  }
  switch (result.status) {
  case display::ApplyStatus::Applied:
    return "Display settings applied.";
  case display::ApplyStatus::PreviewPending:
    return "Keep or revert this display preview within 15 seconds.";
  case display::ApplyStatus::Unsupported:
    return "This display configuration is unavailable.";
  case display::ApplyStatus::FailedRolledBack:
    return "Display apply failed; the previous configuration was restored.";
  case display::ApplyStatus::RollbackPending:
    return "Waiting for renderer access to restore the previous display.";
  case display::ApplyStatus::FailedUnrecoverable:
    return "The previous display configuration could not be restored.";
  }
  return "Display settings were not applied.";
}

SDL_Color displayApplyColor(display::ApplyStatus status) {
  if (status == display::ApplyStatus::Applied) {
    return {157, 220, 176, 255};
  }
  if (status == display::ApplyStatus::PreviewPending ||
      status == display::ApplyStatus::RollbackPending) {
    return {255, 209, 128, 255};
  }
  return {255, 177, 170, 255};
}

int volumePercent(float value) {
  return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 100.0F));
}

std::optional<std::vector<unsigned char>>
readPlatformAssetBytes(const path_t &path) {
  const std::string assetPath = path_t_to_utf8(path);
  size_t size = 0;
  UniqueResource<void, SDL_free> data(
      SDL_LoadFile_RW(SDL_RWFromFile(assetPath.c_str(), "rb"), &size, 1));
  if (!data || size == 0) {
    return std::nullopt;
  }
  const auto *begin = static_cast<const unsigned char *>(data.get());
  return std::vector<unsigned char>(begin, begin + size);
}
} // namespace

void SettingsScene::ensureAudioVideoSession() {
  if (audioVideoSession != nullptr ||
      context.displaySettingsManager == nullptr) {
    return;
  }
  audioDraft = context.settings.audioVideo.audio;
  displayDraft = context.settings.audioVideo.video;
  audioDeviceDropdownOpen = false;
  audioSampleRateDropdownOpen = false;
  audioBufferDropdownOpen = false;
  displayModeDropdownOpen = false;
  displayIndexDropdownOpen = false;
  displayResolutionDropdownOpen = false;
  displayVsyncDropdownOpen = false;
  displayFrameCapDropdownOpen = false;
  audioStatusMessage.clear();
  displayStatusMessage.clear();
  audioVideoSession = std::make_unique<SettingsAudioVideoSession>(
      context.settings, context.audioDeviceManager,
      *context.displaySettingsManager,
      SettingsAudioVideoSession::Callbacks{
          .persist = [this]() { persistSettings(); },
          .playTestSound = [this]() { return playSettingsTestSound(); },
      });
  if (context.audioStartupApplyResult.status != audio::ApplyStatus::Applied) {
    setAudioStatus(audioApplyMessage(context.audioStartupApplyResult),
                   audioApplyColor(context.audioStartupApplyResult.status));
  }
}

View *SettingsScene::buildAudioTab(const LayoutMetrics &metrics) {
  ensureAudioVideoSession();
  auto *cardsColumn = makeAudioVideoCardsColumn(metrics);
  if (audioVideoSession == nullptr) {
    cardsColumn->addView(makeCard(
        metrics, "Audio Runtime", "Audio controls are still initializing.",
        makeWrappedText("Return to this tab after initialization completes.",
                        metrics.bodyTextSize, ui_theme::textSecondary()),
        metrics.modeCardHeight, metrics.cardsWidth));
    return cardsColumn;
  }

  const auto model = BuildAudioControlModel(
      audioDraft, context.audioDeviceManager.capabilities(),
      context.jukebox.audioRuntime().runtimeState());

  auto *streamControls = new View();
  streamControls->setFlexDirection(FlexDirection::Column);
  streamControls->setGap(metrics.compact ? 14.0F : 18.0F);

  auto closeOtherAudioDropdowns = [this](int openIndex, bool open) {
    audioDeviceDropdownOpen = open && openIndex == 0;
    audioSampleRateDropdownOpen = open && openIndex == 1;
    audioBufferDropdownOpen = open && openIndex == 2;
    refreshAudioVideoControls();
  };

  audioDeviceDropdown = new DropdownView({
      .onOpenChanged = [closeOtherAudioDropdowns](
                           bool open) { closeOtherAudioDropdowns(0, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            audioDraft.outputDeviceId = id;
            audioDraft.requestedSampleRate = 0;
            audioDraft.requestedBufferFrames = 0;
            audioDeviceDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  audioSampleRateDropdown = new DropdownView({
      .onOpenChanged = [closeOtherAudioDropdowns](
                           bool open) { closeOtherAudioDropdowns(1, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            audioDraft.requestedSampleRate = parseUnsigned(id);
            audioSampleRateDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  audioBufferDropdown = new DropdownView({
      .onOpenChanged = [closeOtherAudioDropdowns](
                           bool open) { closeOtherAudioDropdowns(2, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            audioDraft.requestedBufferFrames = parseUnsigned(id);
            audioBufferDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });

  const float dropdownWidth = static_cast<float>(
      std::max(260, metrics.cardsWidth - metrics.cardPadding * 2));
  refreshDropdown(audioDeviceDropdown, model.devices, audioDeviceDropdownOpen,
                  "Output", dropdownWidth);
  refreshDropdown(audioSampleRateDropdown, model.sampleRates,
                  audioSampleRateDropdownOpen, "Rate", dropdownWidth);
  refreshDropdown(audioBufferDropdown, model.bufferFrames,
                  audioBufferDropdownOpen, "Buffer", dropdownWidth);
  streamControls->addView(makeChoiceField(metrics, "Output device",
                                          audioDeviceDropdown,
                                          model.devices.explanation));
  streamControls->addView(makeChoiceField(metrics, "Sample rate",
                                          audioSampleRateDropdown,
                                          model.sampleRates.explanation));
  streamControls->addView(makeChoiceField(metrics, "Buffer size",
                                          audioBufferDropdown,
                                          model.bufferFrames.explanation));

  if (model.devices.enabled || model.sampleRates.enabled ||
      model.bufferFrames.enabled) {
    auto *applyButton = makeAccentButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText("Apply Audio Stream", metrics.bodyTextSize + 2,
                 ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
        ui_theme::cyan());
    applyButton->setOnClickListener([this]() { applyAudioStreamDraft(); });
    streamControls->addView(applyButton);
  }
  cardsColumn->addView(makeCard(
      metrics, "Output and Latency",
      metrics.compact
          ? "Stream changes restart audio safely and save only on success."
          : "Choose an available backend configuration. Active playback is "
            "suspended, restarted, and restored transactionally; failed "
            "choices do not replace your working intent.",
      streamControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *volumeControls = new View();
  volumeControls->setFlexDirection(FlexDirection::Column);
  volumeControls->setGap(metrics.compact ? 14.0F : 18.0F);
  auto makeVolumeRow = [this, &metrics](const char *label, int busIndex,
                                        TextInputBox **inputOut) {
    auto *group = new View();
    group->setFlexDirection(FlexDirection::Column);
    group->setGap(metrics.compact ? 7.0F : 9.0F);
    group->addView(
        makeText(label, metrics.bodyTextSize, ui_theme::textSecondary()));
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row);
    row->setFlexWrap(YGWrapWrap);
    row->setGap(metrics.compact ? 8.0F : 10.0F);
    row->setAlignItems(YGAlignCenter);
    auto *minus =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10");
    minus->setOnClickListener(
        [this, busIndex]() { adjustVolume(busIndex, -10); });
    row->addView(minus);
    auto *input = makeNumericInput(metrics);
    input->onEditingFinished([this, input, busIndex](const std::string &) {
      commitVolumeInput(input, busIndex);
    });
    *inputOut = input;
    row->addView(makeInputFrame(metrics, input));
    auto *plus = makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10");
    plus->setOnClickListener(
        [this, busIndex]() { adjustVolume(busIndex, 10); });
    row->addView(plus);
    group->addView(row);
    return group;
  };
  volumeControls->addView(
      makeVolumeRow("Master volume (%)", 0, &masterVolumeInput));
  volumeControls->addView(makeVolumeRow("BGM volume (%)", 1, &bgmVolumeInput));
  volumeControls->addView(
      makeVolumeRow("Keysound volume (%)", 2, &keysoundVolumeInput));
  cardsColumn->addView(makeCard(
      metrics, "Volume Groups",
      "Volume changes apply and save immediately without restarting audio.",
      volumeControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *diagnostics = new View();
  diagnostics->setFlexDirection(FlexDirection::Column);
  diagnostics->setGap(metrics.compact ? 12.0F : 16.0F);
  audioEffectiveText =
      makeWrappedText("", metrics.bodyTextSize, ui_theme::textSecondary());
  diagnostics->addView(audioEffectiveText);
  auto *testSoundButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Test Keysound", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  testSoundButton->setOnClickListener([this]() {
    const bool played =
        audioVideoSession != nullptr && audioVideoSession->playTestSound();
    setAudioStatus(played ? "Keysound test played."
                          : "The keysound test could not be played.",
                   played ? SDL_Color{157, 220, 176, 255}
                          : SDL_Color{255, 177, 170, 255});
  });
  diagnostics->addView(testSoundButton);
  audioStatusText =
      makeWrappedText("", metrics.bodyTextSize, ui_theme::textSecondary());
  diagnostics->addView(audioStatusText);
  cardsColumn->addView(makeCard(
      metrics, "Effective Runtime",
      "Effective values come from the active backend, not the requested "
      "profile intent.",
      diagnostics, metrics.modeCardHeight, metrics.cardsWidth));

  refreshAudioVideoControls();
  return cardsColumn;
}

View *SettingsScene::buildDisplayTab(const LayoutMetrics &metrics) {
  ensureAudioVideoSession();
  auto *cardsColumn = makeAudioVideoCardsColumn(metrics);
  if (audioVideoSession == nullptr ||
      context.displaySettingsManager == nullptr) {
    cardsColumn->addView(makeCard(
        metrics, "Display Runtime", "Display controls are still initializing.",
        makeWrappedText("Return to this tab after initialization completes.",
                        metrics.bodyTextSize, ui_theme::textSecondary()),
        metrics.modeCardHeight, metrics.cardsWidth));
    return cardsColumn;
  }

  const auto model = BuildDisplayControlModel(
      displayDraft, context.displaySettingsManager->capabilities());
  auto *controls = new View();
  controls->setFlexDirection(FlexDirection::Column);
  controls->setGap(metrics.compact ? 14.0F : 18.0F);

  auto closeOtherDisplayDropdowns = [this](int openIndex, bool open) {
    displayModeDropdownOpen = open && openIndex == 0;
    displayIndexDropdownOpen = open && openIndex == 1;
    displayResolutionDropdownOpen = open && openIndex == 2;
    displayVsyncDropdownOpen = open && openIndex == 3;
    displayFrameCapDropdownOpen = open && openIndex == 4;
    refreshAudioVideoControls();
  };

  displayModeDropdown = new DropdownView({
      .onOpenChanged = [closeOtherDisplayDropdowns](
                           bool open) { closeOtherDisplayDropdowns(0, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            displayDraft.mode = parseDisplayMode(id);
            displayModeDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  displayIndexDropdown = new DropdownView({
      .onOpenChanged = [closeOtherDisplayDropdowns](
                           bool open) { closeOtherDisplayDropdowns(1, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            displayDraft.displayIndex = parseInt(id, displayDraft.displayIndex);
            if (context.displaySettingsManager != nullptr) {
              const auto capabilities =
                  context.displaySettingsManager->capabilities();
              const auto selected = std::ranges::find_if(
                  capabilities.displays,
                  [this](const display::DisplayInfo &entry) {
                    return entry.index == displayDraft.displayIndex;
                  });
              if (selected != capabilities.displays.end() &&
                  !selected->resolutions.empty()) {
                displayDraft.width = selected->resolutions.front().width;
                displayDraft.height = selected->resolutions.front().height;
              }
            }
            displayIndexDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  displayResolutionDropdown = new DropdownView({
      .onOpenChanged = [closeOtherDisplayDropdowns](
                           bool open) { closeOtherDisplayDropdowns(2, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            parseResolution(id, displayDraft.width, displayDraft.height);
            displayResolutionDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  displayVsyncDropdown = new DropdownView({
      .onOpenChanged = [closeOtherDisplayDropdowns](
                           bool open) { closeOtherDisplayDropdowns(3, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            displayDraft.vsync = id == "on";
            displayVsyncDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });
  displayFrameCapDropdown = new DropdownView({
      .onOpenChanged = [closeOtherDisplayDropdowns](
                           bool open) { closeOtherDisplayDropdowns(4, open); },
      .onOptionSelected =
          [this](const std::string &id) {
            displayDraft.frameCap = parseUnsigned(id);
            displayFrameCapDropdownOpen = false;
            refreshAudioVideoControls();
          },
  });

  const float dropdownWidth = static_cast<float>(
      std::max(260, metrics.cardsWidth - metrics.cardPadding * 2));
  refreshDropdown(displayModeDropdown, model.modes, displayModeDropdownOpen,
                  "Mode", dropdownWidth);
  refreshDropdown(displayIndexDropdown, model.displays,
                  displayIndexDropdownOpen, "Display", dropdownWidth);
  refreshDropdown(displayResolutionDropdown, model.resolutions,
                  displayResolutionDropdownOpen, "Resolution", dropdownWidth);
  refreshDropdown(displayVsyncDropdown, model.vsync, displayVsyncDropdownOpen,
                  "VSync", dropdownWidth);
  refreshDropdown(displayFrameCapDropdown, model.frameCaps,
                  displayFrameCapDropdownOpen, "Frame cap", dropdownWidth);

  controls->addView(makeChoiceField(metrics, "Window mode", displayModeDropdown,
                                    model.modes.explanation));
  controls->addView(makeChoiceField(metrics, "Display", displayIndexDropdown,
                                    model.displays.explanation));
  controls->addView(makeChoiceField(metrics, "Resolution",
                                    displayResolutionDropdown,
                                    model.resolutions.explanation));
  controls->addView(makeChoiceField(
      metrics, "Vertical sync", displayVsyncDropdown, model.vsync.explanation));
  controls->addView(makeChoiceField(metrics, "Frame cap",
                                    displayFrameCapDropdown,
                                    model.frameCaps.explanation));

  auto *applyButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Apply Display", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::cyan());
  applyButton->setOnClickListener([this]() { applyDisplayDraft(); });
  controls->addView(applyButton);
  displayStatusText =
      makeWrappedText("", metrics.bodyTextSize, ui_theme::textSecondary());
  controls->addView(displayStatusText);

  cardsColumn->addView(makeCard(
      metrics, "Display and Frame Pacing",
      metrics.compact
          ? "Risky display changes require confirmation within 15 seconds."
          : "Mode, display, resolution, and VSync changes open a reversible "
            "15-second preview. Timeout, focus loss, tab exit, and scene "
            "cleanup restore the previous working configuration.",
      controls, metrics.modeCardHeight, metrics.cardsWidth));
  refreshAudioVideoControls();
  return cardsColumn;
}

void SettingsScene::buildDisplayPreviewOverlay(const LayoutMetrics &metrics) {
  displayPreviewOverlayRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  displayPreviewOverlayRoot->setPositionType(YGPositionTypeAbsolute);
  displayPreviewOverlayRoot->setPosition(Edge::Left, 0);
  displayPreviewOverlayRoot->setPosition(Edge::Top, 0);
  displayPreviewOverlayRoot->setZIndex(1100);
  displayPreviewOverlayRoot->setFlexDirection(FlexDirection::Column);
  displayPreviewOverlayRoot->setAlignItems(YGAlignCenter);
  displayPreviewOverlayRoot->setJustifyContent(YGJustifyCenter);
  displayPreviewOverlayRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(static_cast<float>(std::min(
      metrics.compact ? 620 : 760, std::max(280, metrics.contentWidth - 32))));
  panel->setMinHeight(static_cast<float>(metrics.compact ? 300 : 340));
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setGap(metrics.compact ? 14.0F : 18.0F);
  panel->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setCornerRadius(ui_theme::panelRadius());
  panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);
  panel->setThemedBorderColor(ui_theme::hairline);
  panel->setBorderWidth(1);
  panel->addView(makeWrappedText("Keep these display settings?",
                                 metrics.sectionTitleSize,
                                 ui_theme::textPrimary()));
  displayPreviewCountdownText =
      makeWrappedText("Reverting in 15 seconds", metrics.bodyTextSize + 4,
                      ui_theme::amber(), TextView::CENTER);
  panel->addView(displayPreviewCountdownText);
  displayPreviewStatusText = makeWrappedText(
      "If the window is unreadable, wait—AsoBMaShow will restore the previous "
      "configuration automatically.",
      metrics.bodyTextSize, ui_theme::textSecondary());
  panel->addView(displayPreviewStatusText);

  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(metrics.compact ? 10.0F : 14.0F);
  actions->setJustifyContent(YGJustifyCenter);
  auto *revertButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Revert", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::coral());
  revertButton->setOnClickListener([this]() { revertDisplayPreview(); });
  actions->addView(revertButton);
  displayPreviewKeepButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Keep", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  displayPreviewKeepButton->setOnClickListener(
      [this]() { keepDisplayPreview(); });
  actions->addView(displayPreviewKeepButton);
  panel->addView(actions);
  displayPreviewOverlayRoot->addView(panel);
  rootLayout->addView(displayPreviewOverlayRoot);
  updateDisplayPreviewUi();
}

void SettingsScene::refreshAudioVideoControls() {
  if (audioVideoSession == nullptr) {
    return;
  }
  const auto audioModel = BuildAudioControlModel(
      audioDraft, context.audioDeviceManager.capabilities(),
      context.jukebox.audioRuntime().runtimeState());
  LayoutMetrics metrics = resolveLayoutMetrics();
  const int tabColumnWidth = std::min(
      metrics.contentWidth,
      metrics.compact ? std::clamp(metrics.contentWidth / 4, 150, 190)
                      : std::clamp(metrics.contentWidth / 6, 220, 280));
  const int scrollRightPadding = metrics.compact ? 12 : 16;
  const int cardsWidth = std::max(0, metrics.contentWidth - tabColumnWidth -
                                         metrics.bodyGap - scrollRightPadding);
  const float dropdownWidth =
      static_cast<float>(std::max(260, cardsWidth - metrics.cardPadding * 2));
  refreshDropdown(audioDeviceDropdown, audioModel.devices,
                  audioDeviceDropdownOpen, "Output", dropdownWidth);
  refreshDropdown(audioSampleRateDropdown, audioModel.sampleRates,
                  audioSampleRateDropdownOpen, "Rate", dropdownWidth);
  refreshDropdown(audioBufferDropdown, audioModel.bufferFrames,
                  audioBufferDropdownOpen, "Buffer", dropdownWidth);

  if (audioEffectiveText != nullptr) {
    std::ostringstream text;
    text << "Active output: " << audioModel.effectiveDeviceLabel << "\n";
    if (audioModel.effectiveSampleRate > 0) {
      text << "Effective format: " << audioModel.effectiveSampleRate << " Hz, "
           << audioModel.effectiveBufferFrames << " frames\n";
      text << std::fixed << std::setprecision(2)
           << "Effective buffer latency: " << audioModel.effectiveLatencyMs
           << " ms";
    } else {
      text << "Effective format is not currently available.";
    }
    audioEffectiveText->setText(text.str());
  }
  syncVolumeInputText(false);
  if (audioStatusText != nullptr) {
    audioStatusText->setText(audioStatusMessage);
    audioStatusText->setColor(audioStatusColor);
  }

  if (context.displaySettingsManager != nullptr) {
    const auto displayModel = BuildDisplayControlModel(
        displayDraft, context.displaySettingsManager->capabilities());
    refreshDropdown(displayModeDropdown, displayModel.modes,
                    displayModeDropdownOpen, "Mode", dropdownWidth);
    refreshDropdown(displayIndexDropdown, displayModel.displays,
                    displayIndexDropdownOpen, "Display", dropdownWidth);
    refreshDropdown(displayResolutionDropdown, displayModel.resolutions,
                    displayResolutionDropdownOpen, "Resolution", dropdownWidth);
    refreshDropdown(displayVsyncDropdown, displayModel.vsync,
                    displayVsyncDropdownOpen, "VSync", dropdownWidth);
    refreshDropdown(displayFrameCapDropdown, displayModel.frameCaps,
                    displayFrameCapDropdownOpen, "Frame cap", dropdownWidth);
  }
  if (displayStatusText != nullptr) {
    displayStatusText->setText(displayStatusMessage);
    displayStatusText->setColor(displayStatusColor);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::updateDisplayPreviewUi() {
  if (displayPreviewOverlayRoot == nullptr) {
    return;
  }
  const bool visible =
      audioVideoSession != nullptr && audioVideoSession->hasDisplayPreview();
  displayPreviewOverlayRoot->setVisible(visible);
  if (!visible) {
    return;
  }
  const bool confirmable =
      audioVideoSession->displayPreviewCandidate().has_value();
  if (displayPreviewKeepButton != nullptr) {
    displayPreviewKeepButton->setVisible(confirmable);
  }
  const int seconds = audioVideoSession->displayPreviewSecondsRemaining(
      std::chrono::steady_clock::now());
  if (displayPreviewCountdownText != nullptr) {
    displayPreviewCountdownText->setText(
        confirmable ? "Reverting in " + std::to_string(seconds) +
                          (seconds == 1 ? " second" : " seconds")
                    : "Restoring the previous display settings...");
  }
}

void SettingsScene::applyAudioStreamDraft() {
  if (audioVideoSession == nullptr) {
    return;
  }
  const auto result = audioVideoSession->applyStreamIntent(audioDraft);
  setAudioStatus(audioApplyMessage(result), audioApplyColor(result.status));
  if (result.status == audio::ApplyStatus::Applied) {
    audioDraft = context.settings.audioVideo.audio;
  }
  refreshAudioVideoControls();
}

void SettingsScene::applyDisplayDraft() {
  if (audioVideoSession == nullptr) {
    return;
  }
  const auto result = audioVideoSession->beginDisplayPreview(
      displayDraft, std::chrono::steady_clock::now());
  setDisplayStatus(displayApplyMessage(result),
                   displayApplyColor(result.status));
  if (result.status == display::ApplyStatus::Applied) {
    displayDraft = context.settings.audioVideo.video;
  }
  updateDisplayPreviewUi();
  refreshAudioVideoControls();
}

void SettingsScene::keepDisplayPreview() {
  if (audioVideoSession == nullptr) {
    return;
  }
  const auto result = audioVideoSession->keepDisplayPreview();
  setDisplayStatus(displayApplyMessage(result),
                   displayApplyColor(result.status));
  if (result.status == display::ApplyStatus::Applied) {
    displayDraft = context.settings.audioVideo.video;
  }
  updateDisplayPreviewUi();
  refreshAudioVideoControls();
}

void SettingsScene::revertDisplayPreview() {
  if (audioVideoSession == nullptr) {
    return;
  }
  const auto result = audioVideoSession->revertDisplayPreview();
  setDisplayStatus(displayApplyMessage(result),
                   displayApplyColor(result.status));
  if (!audioVideoSession->hasDisplayPreview()) {
    displayDraft = context.settings.audioVideo.video;
  }
  updateDisplayPreviewUi();
  refreshAudioVideoControls();
}

void SettingsScene::cancelDisplayPreviewForTabExit() {
  if (audioVideoSession == nullptr || !audioVideoSession->hasDisplayPreview()) {
    return;
  }
  const auto result = audioVideoSession->leaveDisplayTab();
  setDisplayStatus(displayApplyMessage(result),
                   displayApplyColor(result.status));
  displayDraft = context.settings.audioVideo.video;
  updateDisplayPreviewUi();
}

void SettingsScene::setAudioStatus(const std::string &message,
                                   const SDL_Color &color) {
  audioStatusMessage = message;
  audioStatusColor = color;
  if (audioStatusText != nullptr) {
    audioStatusText->setText(message);
    audioStatusText->setColor(color);
  }
}

void SettingsScene::setDisplayStatus(const std::string &message,
                                     const SDL_Color &color) {
  displayStatusMessage = message;
  displayStatusColor = color;
  if (displayStatusText != nullptr) {
    displayStatusText->setText(message);
    displayStatusText->setColor(color);
  }
  if (displayPreviewStatusText != nullptr && !message.empty()) {
    displayPreviewStatusText->setText(message);
  }
}

void SettingsScene::syncVolumeInputText(bool force) {
  const auto &audio = context.settings.audioVideo.audio;
  auto sync = [force](TextInputBox *input, float value) {
    if (input == nullptr || (!force && input->getSelected())) {
      return;
    }
    input->setEditingText(std::to_string(volumePercent(value)));
  };
  sync(masterVolumeInput, audio.masterVolume);
  sync(bgmVolumeInput, audio.bgmVolume);
  sync(keysoundVolumeInput, audio.keysoundVolume);
}

void SettingsScene::commitVolumeInput(TextInputBox *input, int busIndex) {
  if (input == nullptr || audioVideoSession == nullptr) {
    return;
  }
  try {
    const int percent = std::clamp(std::stoi(input->getText()), 0, 100);
    const auto &current = context.settings.audioVideo.audio;
    float master = current.masterVolume;
    float bgm = current.bgmVolume;
    float keysound = current.keysoundVolume;
    const float value = static_cast<float>(percent) / 100.0F;
    if (busIndex == 0) {
      master = value;
    } else if (busIndex == 1) {
      bgm = value;
    } else {
      keysound = value;
    }
    const auto result = audioVideoSession->applyVolumes(master, bgm, keysound);
    audioDraft = context.settings.audioVideo.audio;
    if (result.status != audio::ApplyStatus::Applied) {
      setAudioStatus(audioApplyMessage(result), audioApplyColor(result.status));
    }
  } catch (const std::exception &) {
  }
  syncVolumeInputText(true);
  refreshAudioVideoControls();
}

void SettingsScene::adjustVolume(int busIndex, int deltaPercent) {
  const auto &audio = context.settings.audioVideo.audio;
  int percent = busIndex == 0   ? volumePercent(audio.masterVolume)
                : busIndex == 1 ? volumePercent(audio.bgmVolume)
                                : volumePercent(audio.keysoundVolume);
  percent = std::clamp(percent + deltaPercent, 0, 100);
  TextInputBox *input = busIndex == 0   ? masterVolumeInput
                        : busIndex == 1 ? bgmVolumeInput
                                        : keysoundVolumeInput;
  if (input != nullptr) {
    input->setEditingText(std::to_string(percent));
  }
  commitVolumeInput(input, busIndex);
}

bool SettingsScene::playSettingsTestSound() {
  static const path_t kTestSoundPath = PATH("assets/audio/sample.wav");
  std::atomic_bool cancelled = false;
  auto &runtime = context.jukebox.audioRuntime();
  return PlaySettingsTestSoundAsset(
      kTestSoundPath,
      {.readAssetBytes = readPlatformAssetBytes,
       .loadSoundFromMemory =
           [&](const path_t &path, const std::vector<unsigned char> &bytes) {
             return runtime.loadSoundFromMemory(path, bytes, cancelled);
           },
       .playKeysound =
           [&](const path_t &path) {
             return runtime.playSound(
                 path, audioBusForJukeboxSource(
                           JukeboxAudioSource::SettingsTestTone));
           }});
}
