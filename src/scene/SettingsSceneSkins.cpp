#include "SettingsSceneShared.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "GameplaySkinSettingsController.h"
#include "GameplaySkinSettingsPresentation.h"
#include "../skin/GameplaySkinTraits.h"
#include "../skin/beatoraja/BeatorajaSkinConfiguration.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

using namespace settings_scene;

namespace {

View *makeGameplaySkinsColumn(const LayoutMetrics &metrics) {
  auto *column = new View();
  column->setFlexDirection(FlexDirection::Column);
  column->setGap(static_cast<float>(metrics.secondaryGap));
  column->setWidth(static_cast<float>(metrics.cardsWidth));
  return column;
}

Button *makeGameplaySkinAction(const LayoutMetrics &metrics,
                               const std::string &label, bool enabled,
                               std::function<void()> action,
                               const Color &accent = ui_theme::cyan()) {
  auto *labelView =
      makeText(label, metrics.smallTextSize, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  constexpr int horizontalContentPadding = 32;
  const int minimumWidth = metrics.compact ? 158 : 184;
  const int contentWidth = std::max(0, labelView->textureWidth());
  auto *button = makeAccentButton(
      std::max(minimumWidth, contentWidth + horizontalContentPadding),
      metrics.actionButtonHeight, labelView, accent);
  button->setEnabled(enabled);
  if (enabled) {
    button->setOnClickListener(std::move(action));
  }
  return button;
}

struct GameplaySkinChoiceButton {
  std::string label;
  bool selected = false;
  std::function<void()> action;
  std::function<bool()> tryAction;
};

void styleGameplaySkinChoiceButton(Button *button, bool selected) {
  button->setSelected(selected);
  if (!selected) {
    button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                      ui_theme::controlPressed);
    button->setThemedBorderColors(ui_theme::hairline, ui_theme::accentBorder,
                                  ui_theme::accentBorderStrong);
    return;
  }

  const auto accent = []() { return ui_theme::cyan(); };
  const auto accentWithModeAlpha = [](uint8_t lightAlpha, uint8_t darkAlpha) {
    return [lightAlpha, darkAlpha]() {
      return ui_theme::withAlpha(
          ui_theme::cyan(), ui_theme::activeMode() == ui_theme::ThemeMode::Light
                                ? lightAlpha
                                : darkAlpha);
    };
  };
  button->setThemedBackgroundColors(accentWithModeAlpha(54, 82),
                                    accentWithModeAlpha(74, 108),
                                    accentWithModeAlpha(100, 136));
  button->setThemedBorderColors(
      []() { return ui_theme::withAlpha(ui_theme::cyan(), 178); },
      []() { return ui_theme::withAlpha(ui_theme::cyan(), 216); }, accent);
}

View *makeGameplaySkinChoiceRow(
    const LayoutMetrics &metrics, const std::string &label, bool enabled,
    std::vector<GameplaySkinChoiceButton> choices) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setFlexWrap(YGWrapWrap);
  row->setAlignItems(YGAlignCenter);
  row->setGap(metrics.compact ? 8.0f : 10.0f);

  auto *labelView = makeText(label, metrics.smallTextSize,
                             ui_theme::textSecondary(), TextView::LEFT,
                             TextView::MIDDLE);
  labelView->setMinWidth(0.0f);
  labelView->setFlexShrink(1.0f);
  row->addView(labelView);

  auto *buttons = new View();
  buttons->setFlexDirection(FlexDirection::Row);
  buttons->setFlexWrap(YGWrapWrap);
  buttons->setGap(metrics.compact ? 6.0f : 8.0f);
  auto choiceButtons = std::make_shared<std::vector<Button *>>();
  for (auto &choice : choices) {
    auto *choiceLabel = makeText(choice.label, metrics.smallTextSize,
                                 ui_theme::textPrimary(), TextView::CENTER,
                                 TextView::MIDDLE);
    constexpr int horizontalContentPadding = 28;
    const int minimumWidth = metrics.compact ? 84 : 96;
    const int width = std::max(
        minimumWidth,
        std::max(0, choiceLabel->textureWidth()) + horizontalContentPadding);
    auto *button = choice.selected
                       ? makeAccentButton(width, metrics.actionButtonHeight,
                                          choiceLabel, ui_theme::cyan())
                       : makeControlButton(width, metrics.actionButtonHeight,
                                           choiceLabel);
    button->setSelected(choice.selected);
    button->setEnabled(enabled);
    if (enabled) {
      button->setOnClickListener(
          [button, choiceButtons, action = std::move(choice.action),
           tryAction = std::move(choice.tryAction)]() mutable {
            const bool accepted = tryAction ? tryAction() : (action(), true);
            if (!accepted) {
              return;
            }
            for (auto *candidate : *choiceButtons) {
              styleGameplaySkinChoiceButton(candidate, candidate == button);
            }
          });
    }
    choiceButtons->push_back(button);
    buttons->addView(button);
  }
  row->addView(buttons);
  return row;
}

const char *validationLabel(skin::SkinValidationDisposition disposition) {
  switch (disposition) {
  case skin::SkinValidationDisposition::SelectableGameplay:
    return "Gameplay selectable";
  case skin::SkinValidationDisposition::UnavailableType:
    return "Unsupported skin type";
  case skin::SkinValidationDisposition::Invalid:
    return "Validation failed";
  }
  return "Unavailable";
}

std::string diagnosticPresentation(const skin::SkinDiagnostic &diagnostic) {
  std::string text = diagnostic.code + ": " + diagnostic.message;
  if (!diagnostic.virtualPath.empty()) {
    text += " • " + diagnostic.virtualPath;
  }
  if (diagnostic.source) {
    text += " • " + diagnostic.source->virtualPath + ":" +
            std::to_string(diagnostic.source->line) + ":" +
            std::to_string(diagnostic.source->column);
  }
  return text;
}

float sanitizeViewportComponent(std::string_view text, float fallback,
                                float minimum, float maximum) {
  try {
    std::size_t consumed = 0;
    const float value = std::stof(std::string(text), &consumed);
    if (consumed != text.size() || !std::isfinite(value)) {
      return fallback;
    }
    return std::clamp(value, minimum, maximum);
  } catch (...) {
    return fallback;
  }
}

int sanitizeOffsetComponent(std::string_view text, int fallback) {
  int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? value
             : fallback;
}

std::string formatViewportComponent(float value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(2) << value;
  return std::move(stream).str();
}

const char *safetyLevelLabel(skin::SkinSafetyLevel level) {
  switch (level) {
  case skin::SkinSafetyLevel::Standard:
    return "Standard";
  case skin::SkinSafetyLevel::BeatorajaCompatibility:
    return "Beatoraja compatibility";
  case skin::SkinSafetyLevel::Unrestricted:
    return "Unrestricted";
  }
  return "Standard";
}

} // namespace

bool SettingsScene::handleGameplaySkinActionResult(
    skin::ControllerActionResult result) {
  if (!result.message.empty() &&
      (!result.accepted || !result.asynchronous)) {
    gameplaySkinUiMessage = result.message;
  } else if (result.accepted && result.asynchronous) {
    gameplaySkinUiMessage.clear();
  }
  if (result.accepted && !result.asynchronous) {
    lastLayoutWidth = -1;
  }
  if (gameplaySkinSettingsController != nullptr) {
    updateGameplaySkinSettingsLiveUi(gameplaySkinSettingsController->snapshot());
  }
  return result.accepted;
}

void SettingsScene::ensureGameplaySkinSettingsController() {
  const auto profileId =
      skin::makeSkinProfileId(context.profileManager.activeProfile().id);
  if (!profileId || !context.skinPackageOperationService ||
      !context.skinDiagnosticHistory || !context.skinCommitCoordinator ||
      !context.gameplaySkinLifecycle ||
      !context.profileSettingsPersistenceCoordinator) {
    if (gameplaySkinSettingsController != nullptr) {
      gameplaySkinSettingsController->close();
      gameplaySkinSettingsController.reset();
    }
    gameplaySkinSettingsProfileId.clear();
    return;
  }

  if (gameplaySkinSettingsController != nullptr &&
      gameplaySkinSettingsProfileId == profileId->opaque) {
    return;
  }

  const auto clientId = context.skinCommitCoordinator->createClient();
  if (clientId == 0) {
    if (gameplaySkinSettingsController != nullptr) {
      gameplaySkinSettingsController->close();
      gameplaySkinSettingsController.reset();
    }
    gameplaySkinSettingsProfileId.clear();
    return;
  }
  if (gameplaySkinSettingsController == nullptr) {
    gameplaySkinSettingsController = std::make_unique<
        skin::GameplaySkinSettingsController>(
        skin::GameplaySkinSettingsControllerDependencies{
            .operations = *context.skinPackageOperationService,
            .history = *context.skinDiagnosticHistory,
            .profileId = *profileId,
            .profileOwner = *context.profileSettingsPersistenceCoordinator,
            .profileSnapshots = *context.profileSettingsPersistenceCoordinator,
            .commits = *context.skinCommitCoordinator,
            .clientId = clientId,
            .requestRescan =
                [this]() {
                  if (context.gameplaySkinLifecycle != nullptr) {
                    context.gameplaySkinLifecycle->requestRescan(
                        skin::SkinRescanReason::Explicit);
                  }
                },
            .rescanProgress =
                [this]() {
                  return context.gameplaySkinLifecycle != nullptr
                             ? context.gameplaySkinLifecycle->rescanProgress()
                             : skin::SkinRescanProgress{};
                },
            .requestRevalidation =
                [this](const skin::SkinEntryId &entry) {
                  if (context.gameplaySkinLifecycle != nullptr) {
                    context.gameplaySkinLifecycle->requestRevalidation(entry);
                  }
                },
            .catalogSnapshot =
                [this]() {
                  return context.gameplaySkinLifecycle != nullptr
                             ? context.gameplaySkinLifecycle->catalogSnapshot()
                             : std::shared_ptr<
                                   const skin::SkinPackageCatalogSnapshot>{};
                },
            .beginArchiveHandoff =
                [this]() {
                  return platform_document_handoff::ImportDocumentAsync(
                      {.mimeType = "application/zip",
                       .maxBytes = skin::SkinPackagePolicy::maxArchiveBytes},
                      context.temporaryPathCleanupService);
                },
            .beginFolderHandoff =
                [this](PlatformDirectoryImportRequest request) {
                  return platform_document_handoff::ImportDirectoryAsync(
                      std::move(request), context.temporaryPathCleanupService);
                }});
  } else {
    gameplaySkinSettingsController->profileChanged(*profileId, clientId);
  }
  gameplaySkinSettingsProfileId = profileId->opaque;
  gameplaySkinSettingsLayoutKey.clear();
  gameplaySkinUiMessage.clear();
  gameplaySkinSafetyDropdownOpen = false;
  gameplaySkinConfigurationDropdownOpenKey.clear();
  gameplaySkinReplaceConfirmationArmed = false;
  gameplaySkinRemovalConfirmationKey.clear();
  gameplaySkinControlsBuiltDisabled = false;
}

void SettingsScene::updateGameplaySkinSettingsController() {
  ensureGameplaySkinSettingsController();
  if (gameplaySkinSettingsController == nullptr) {
    return;
  }
  gameplaySkinSettingsController->poll();
  const auto &snapshot = gameplaySkinSettingsController->snapshot();
  if (!snapshot.preparedName) {
    gameplaySkinReplaceConfirmationArmed = false;
  }
  if (!gameplaySkinRemovalConfirmationKey.empty() &&
      std::none_of(snapshot.entries.begin(), snapshot.entries.end(),
                   [this](const auto &entry) {
                     return entry.entry.package.collisionKey ==
                            gameplaySkinRemovalConfirmationKey;
                   })) {
    gameplaySkinRemovalConfirmationKey.clear();
  }
  if (activeTab == SettingsTab::GameplaySkins &&
      gameplaySkinControlsBuiltDisabled &&
      skin::gameplaySkinSettingsActionAvailability(snapshot).ordinaryActions) {
    lastLayoutWidth = -1;
  }
  const std::string next = skin::gameplaySkinSettingsLayoutKey(snapshot);
  if (next != gameplaySkinSettingsLayoutKey) {
    gameplaySkinSettingsLayoutKey = next;
    if (activeTab == SettingsTab::GameplaySkins) {
      lastLayoutWidth = -1;
    }
  }
  updateGameplaySkinSettingsLiveUi(snapshot);
}

skin::ConfigOffset SettingsScene::gameplaySkinOffsetForEntry(
    const skin::SkinEntryId &entry, const std::string &name) const {
  if (gameplaySkinSettingsController == nullptr) {
    return {};
  }
  const auto &entries = gameplaySkinSettingsController->snapshot().entries;
  const auto row = std::ranges::find_if(entries, [&entry](const auto &value) {
    return value.entry == entry;
  });
  if (row == entries.end()) {
    return {};
  }
  const auto offset = row->settings.offsets.find(name);
  return offset == row->settings.offsets.end() ? skin::ConfigOffset{}
                                                : offset->second;
}

skin::ViewportSettings SettingsScene::gameplaySkinViewportForEntry(
    const skin::SkinEntryId &entry) const {
  if (gameplaySkinSettingsController == nullptr) {
    return {};
  }
  const auto &entries = gameplaySkinSettingsController->snapshot().entries;
  const auto row = std::ranges::find_if(entries, [&entry](const auto &value) {
    return value.entry == entry;
  });
  return row == entries.end() ? skin::ViewportSettings{}
                              : row->settings.viewport;
}

void SettingsScene::updateGameplaySkinSettingsLiveUi(
    const skin::GameplaySkinSettingsSnapshot &snapshot) {
  if (activeTab != SettingsTab::GameplaySkins) {
    return;
  }

  if (gameplaySkinStatusText != nullptr) {
    gameplaySkinStatusText->setText(snapshot.statusMessage);
  }
  if (gameplaySkinUiMessageText != nullptr) {
    gameplaySkinUiMessageText->setText(gameplaySkinUiMessage ==
                                               snapshot.statusMessage
                                           ? std::string{}
                                           : gameplaySkinUiMessage);
  }
  if (gameplaySkinConfigurationDigestText != nullptr) {
    const auto selected = snapshot.selectedGameplayEntries.find(
        gameplaySkinActiveTraitSkinType);
    const auto row = selected == snapshot.selectedGameplayEntries.end()
                         ? snapshot.entries.end()
                         : std::ranges::find_if(
                               snapshot.entries, [&selected](const auto &entry) {
                                 return entry.entry == selected->second;
                               });
    if (row != snapshot.entries.end()) {
      gameplaySkinConfigurationDigestText->setText(
          "Revision: " + row->revisionDigest +
          " • Configuration: " + row->configurationDigest);
    }
  }

  if (snapshot.state != skin::GameplaySkinSettingsState::Busy) {
    if (gameplaySkinBusyOverlayRoot != nullptr) {
      gameplaySkinBusyOverlayRoot->setVisible(false);
    }
    return;
  }
  ensureGameplaySkinBusyOverlay(snapshot);
  if (gameplaySkinBusyOverlayRoot != nullptr) {
    gameplaySkinBusyOverlayRoot->setVisible(true);
  }
}

void SettingsScene::ensureGameplaySkinBusyOverlay(
    const skin::GameplaySkinSettingsSnapshot &snapshot) {
  if (rootLayout == nullptr) {
    return;
  }
  if (gameplaySkinBusyOverlayRoot == nullptr) {
    gameplaySkinBusyOverlayRoot = new BlockingOverlayView(
        0, 0, rendering::window_width, rendering::window_height);
    gameplaySkinBusyOverlayRoot->setPositionType(YGPositionTypeAbsolute);
    gameplaySkinBusyOverlayRoot->setPosition(Edge::Left, 0);
    gameplaySkinBusyOverlayRoot->setPosition(Edge::Top, 0);
    gameplaySkinBusyOverlayRoot->setZIndex(1040);
    gameplaySkinBusyOverlayRoot->setFlexDirection(FlexDirection::Column);
    gameplaySkinBusyOverlayRoot->setAlignItems(YGAlignCenter);
    gameplaySkinBusyOverlayRoot->setJustifyContent(YGJustifyCenter);
    gameplaySkinBusyOverlayRoot->setThemedBackgroundColor(ui_theme::scrim);

    auto *panel = new View();
    panel->setWidth(520.0F);
    panel->setFlexDirection(FlexDirection::Column);
    panel->setAlignItems(YGAlignStretch);
    panel->setGap(14.0F);
    panel->setPadding(Edge::All, 28.0F);
    panel->setThemedBackgroundColor(ui_theme::panelStrong);
    panel->setCornerRadius(ui_theme::panelRadius());
    panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);
    panel->setThemedBorderColor(ui_theme::hairline);
    panel->setBorderWidth(1);
    panel->addView(makeWrappedText("Updating gameplay skins", 26,
                                   ui_theme::textPrimary()));
    gameplaySkinBusyOverlayStatusText =
        makeWrappedText({}, 20, ui_theme::textSecondary());
    panel->addView(gameplaySkinBusyOverlayStatusText);
    gameplaySkinBusyOverlayCancelButton = makeControlButton(
        180, 60,
        makeText("Cancel", 20, ui_theme::textPrimary(), TextView::CENTER,
                 TextView::MIDDLE));
    gameplaySkinBusyOverlayCancelButton->setOnClickListener([this]() {
      if (gameplaySkinSettingsController != nullptr) {
        gameplaySkinSettingsController->cancelOperation();
        updateGameplaySkinSettingsLiveUi(
            gameplaySkinSettingsController->snapshot());
      }
    });
    panel->addView(gameplaySkinBusyOverlayCancelButton);
    gameplaySkinBusyOverlayRoot->addView(panel);
    rootLayout->addView(gameplaySkinBusyOverlayRoot);
  }

  std::string message = snapshot.statusMessage;
  if (snapshot.hasPackageProgress) {
    const std::string progress =
        skin::gameplaySkinPackageProgressDisplayText(snapshot.progress);
    if (!progress.empty()) {
      message += message.empty() ? progress : "\n" + progress;
    }
  }
  if (snapshot.rescanProgress.phase != skin::SkinRescanProgressPhase::Idle &&
      snapshot.rescanProgress.phase !=
          skin::SkinRescanProgressPhase::Succeeded &&
      snapshot.rescanProgress.phase !=
          skin::SkinRescanProgressPhase::Failed) {
    const std::string progress =
        skin::gameplaySkinRescanProgressDisplayText(snapshot.rescanProgress);
    if (!progress.empty()) {
      message += message.empty() ? progress : "\n" + progress;
    }
  }
  if (gameplaySkinBusyOverlayStatusText != nullptr) {
    gameplaySkinBusyOverlayStatusText->setText(message);
  }
  if (gameplaySkinBusyOverlayCancelButton != nullptr) {
    gameplaySkinBusyOverlayCancelButton->setEnabled(snapshot.canCancel);
  }
}

bool SettingsScene::gameplaySkinTraitsRuntimeAvailable() const noexcept {
  return skin::gameplaySkinTraitsRuntimeAvailable(
      skin::luaGameplaySkinsAvailable(),
      gameplaySkinSettingsController != nullptr);
}

void SettingsScene::appendSelectedSkinHudSettings(
    View *body, const LayoutMetrics &metrics, bool includeBuiltInOnlySettings) {
  const auto appendHeading = [body, &metrics](const std::string &label) {
    body->addView(
        makeWrappedText(label, metrics.bodyTextSize, ui_theme::cyan()));
  };
  const auto appendNumeric =
      [this, body, &metrics](const std::string &label, const std::string &value,
                             std::function<void(const std::string &)> apply) {
        auto *row = new View();
        row->setFlexDirection(FlexDirection::Row);
        row->setFlexWrap(YGWrapWrap);
        row->setAlignItems(YGAlignCenter);
        row->setGap(metrics.compact ? 8.0F : 10.0F);
        auto *labelView =
            makeText(label, metrics.smallTextSize, ui_theme::textSecondary(),
                     TextView::LEFT, TextView::MIDDLE);
        labelView->setMinWidth(0.0F);
        labelView->setFlexShrink(1.0F);
        row->addView(labelView);
        auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
        input->setEditingText(value);
        input->onEditingFinished(
            [this, input, apply = std::move(apply)](const std::string &) {
              apply(input->getText());
              lastLayoutWidth = -1;
            });
        row->addView(input);
        body->addView(row);
      };
  const auto appendToggle = [this, body,
                             &metrics](const std::string &label, bool value,
                                       std::function<void(bool)> set) {
    body->addView(makeGameplaySkinChoiceRow(
        metrics, label, true,
        {{.label = "Off",
          .selected = !value,
          .action =
              [this, set]() mutable {
                set(false);
                lastLayoutWidth = -1;
              }},
         {.label = "On", .selected = value, .action = [this, set]() mutable {
            set(true);
            lastLayoutWidth = -1;
          }}}));
  };
  const auto appendChoices =
      [this, body, &metrics](const std::string &label,
                             std::vector<GameplaySkinChoiceButton> choices) {
        body->addView(makeGameplaySkinChoiceRow(metrics, label, true,
                                                std::move(choices)));
      };

  appendHeading("Application Judgement HUD");
  appendToggle("Judgement Indicator",
               context.settings.judgementIndicatorEnabled,
               [this](bool enabled) {
                 context.settings.judgementIndicatorEnabled = enabled;
                 persistSettings();
               });
  appendNumeric(
      "Indicator Y (%)",
      std::to_string(
          judgementIndicatorYToPercent(context.settings.judgementIndicatorY)),
      [this](const std::string &text) {
        context.settings.judgementIndicatorY = judgementIndicatorPercentToY(
            std::clamp(sanitizeOffsetComponent(
                           text, judgementIndicatorYToPercent(
                                     context.settings.judgementIndicatorY)),
                       0, 100));
        persistSettings();
      });
  appendNumeric("Indicator Width (%)",
                std::to_string(judgementIndicatorWidthScaleToPercent(
                    context.settings.judgementIndicatorWidthScale)),
                [this](const std::string &text) {
                  const int current = judgementIndicatorWidthScaleToPercent(
                      context.settings.judgementIndicatorWidthScale);
                  context.settings.judgementIndicatorWidthScale =
                      judgementIndicatorWidthPercentToScale(std::clamp(
                          sanitizeOffsetComponent(text, current), 50, 200));
                  persistSettings();
                });
  appendNumeric(
      "Indicator Range (ms)",
      std::to_string(context.settings.judgementIndicatorRangeMilliseconds),
      [this](const std::string &text) {
        context.settings.judgementIndicatorRangeMilliseconds =
            clampJudgementIndicatorRangeMilliseconds(sanitizeOffsetComponent(
                text, context.settings.judgementIndicatorRangeMilliseconds));
        persistSettings();
      });

  if (includeBuiltInOnlySettings) {
    appendChoices(
        "Indicator Layout",
        {{.label = "3D Space",
          .selected = context.settings.judgementIndicatorRenderMode ==
                      AppSettings::JudgementIndicatorRenderMode::World3D,
          .action =
              [this]() {
                context.settings.judgementIndicatorRenderMode =
                    AppSettings::JudgementIndicatorRenderMode::World3D;
                persistSettings();
                lastLayoutWidth = -1;
              }},
         {.label = "2D HUD",
          .selected = context.settings.judgementIndicatorRenderMode ==
                      AppSettings::JudgementIndicatorRenderMode::Hud2D,
          .action = [this]() {
            context.settings.judgementIndicatorRenderMode =
                AppSettings::JudgementIndicatorRenderMode::Hud2D;
            persistSettings();
            lastLayoutWidth = -1;
          }}});
  }

  appendToggle("Judgement Counter", context.settings.judgementCounterEnabled,
               [this](bool enabled) {
                 context.settings.judgementCounterEnabled = enabled;
                 persistSettings();
               });
  appendChoices("Counter Position",
                {{.label = "Top",
                  .selected = context.settings.judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Top,
                  .action =
                      [this]() {
                        context.settings.judgementCounterPosition =
                            AppSettings::JudgementCounterPosition::Top;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = "Left",
                  .selected = context.settings.judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Left,
                  .action =
                      [this]() {
                        context.settings.judgementCounterPosition =
                            AppSettings::JudgementCounterPosition::Left;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = "Right",
                  .selected = context.settings.judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Right,
                  .action = [this]() {
                    context.settings.judgementCounterPosition =
                        AppSettings::JudgementCounterPosition::Right;
                    persistSettings();
                    lastLayoutWidth = -1;
                  }}});

  if (!includeBuiltInOnlySettings) {
    return;
  }

  appendHeading("Judgement Feedback");
  appendNumeric(
      "Judge Text Y (%)",
      std::to_string(judgementTextYToPercent(context.settings.judgementTextY)),
      [this](const std::string &text) {
        context.settings.judgementTextY = judgementTextPercentToY(std::clamp(
            sanitizeOffsetComponent(
                text, judgementTextYToPercent(context.settings.judgementTextY)),
            0, 100));
        persistSettings();
      });
  const auto timingChoices =
      [this](AppSettings::JudgementTimingDisplayCriteria value, auto assign) {
        std::vector<GameplaySkinChoiceButton> choices;
        for (const auto criteria :
             {AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::BadOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::Off}) {
          choices.push_back(
              {.label = formatJudgementTimingDisplayCriteriaLabel(criteria),
               .selected = value == criteria,
               .action = [this, criteria, assign]() mutable {
                 assign(criteria);
                 persistSettings();
                 lastLayoutWidth = -1;
               }});
        }
        return choices;
      };
  appendChoices(
      "FAST/SLOW",
      timingChoices(
          context.settings.judgementTimingFastSlowCriteria,
          [this](AppSettings::JudgementTimingDisplayCriteria criteria) {
            context.settings.judgementTimingFastSlowCriteria = criteria;
          }));
  appendChoices(
      "+/- ms",
      timingChoices(
          context.settings.judgementTimingMillisecondsCriteria,
          [this](AppSettings::JudgementTimingDisplayCriteria criteria) {
            context.settings.judgementTimingMillisecondsCriteria = criteria;
          }));

  appendHeading("Gauge");
  appendChoices("Gauge Position",
                {{.label = "World",
                  .selected = context.settings.gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::World,
                  .action =
                      [this]() {
                        context.settings.gaugeBarPosition =
                            AppSettings::GaugeBarPosition::World;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = "Left HUD",
                  .selected = context.settings.gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::Left,
                  .action =
                      [this]() {
                        context.settings.gaugeBarPosition =
                            AppSettings::GaugeBarPosition::Left;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = "Right HUD",
                  .selected = context.settings.gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::Right,
                  .action = [this]() {
                    context.settings.gaugeBarPosition =
                        AppSettings::GaugeBarPosition::Right;
                    persistSettings();
                    lastLayoutWidth = -1;
                  }}});
}

void SettingsScene::appendBuiltInGameplayTraitSettings(
    View *body, const LayoutMetrics &metrics, int keyMode) {
  body->addView(makeWrappedText("Built-in gameplay", metrics.bodyTextSize,
                                ui_theme::lime()));
  const auto appendNumeric =
      [this, body, &metrics](const std::string &label, const std::string &value,
                             std::function<void(const std::string &)> apply) {
        auto *row = new View();
        row->setFlexDirection(FlexDirection::Row);
        row->setFlexWrap(YGWrapWrap);
        row->setAlignItems(YGAlignCenter);
        row->setGap(metrics.compact ? 8.0F : 10.0F);
        auto *labelView =
            makeText(label, metrics.smallTextSize, ui_theme::textSecondary(),
                     TextView::LEFT, TextView::MIDDLE);
        labelView->setMinWidth(0.0F);
        labelView->setFlexShrink(1.0F);
        row->addView(labelView);
        auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
        input->setEditingText(value);
        input->onEditingFinished(
            [this, input, apply = std::move(apply)](const std::string &) {
              apply(input->getText());
              lastLayoutWidth = -1;
            });
        row->addView(input);
        body->addView(row);
      };

  appendNumeric("Lane Angle (deg)",
                formatFloatValue(context.settings.laneAngleDegrees, 1),
                [this](const std::string &text) {
                  context.settings.laneAngleDegrees = sanitizeViewportComponent(
                      text, context.settings.laneAngleDegrees,
                      AppSettings::kMinLaneAngleDegrees,
                      AppSettings::kMaxLaneAngleDegrees);
                  persistSettings();
                });
  appendNumeric("Lane Length", formatFloatValue(context.settings.laneLength, 1),
                [this](const std::string &text) {
                  context.settings.laneLength = sanitizeViewportComponent(
                      text, context.settings.laneLength,
                      AppSettings::kMinLaneLength, AppSettings::kMaxLaneLength);
                  persistSettings();
                });
  appendNumeric("Beam Length (%)",
                std::to_string(context.settings.laneBeamLengthPercent),
                [this](const std::string &text) {
                  context.settings.laneBeamLengthPercent =
                      clampLaneBeamLengthPercent(sanitizeOffsetComponent(
                          text, context.settings.laneBeamLengthPercent));
                  persistSettings();
                });
  appendNumeric("Play Area Width (" + std::to_string(keyMode) + "K)",
                formatPlayAreaWidthLabel(
                    context.settings.playAreaWidthForKeyMode(keyMode)),
                [this, keyMode](const std::string &text) {
                  context.settings.setPlayAreaWidthForKeyMode(
                      keyMode,
                      sanitizeViewportComponent(
                          text,
                          context.settings.playAreaWidthForKeyMode(keyMode),
                          AppSettings::kMinPlayAreaWidth,
                          AppSettings::kMaxPlayAreaWidth));
                  persistSettings();
                });
  appendSelectedSkinHudSettings(body, metrics, true);
}

View *SettingsScene::buildGameplaySkinsTab(const LayoutMetrics &metrics) {
  gameplaySkinControlsBuiltDisabled = false;
  auto *column = makeGameplaySkinsColumn(metrics);
  if (!gameplaySkinTraitsRuntimeAvailable()) {
    auto *body = new View();
    body->setFlexDirection(FlexDirection::Column);
    body->setGap(static_cast<float>(metrics.cardGap));
    std::string availabilityMessage =
        "Gameplay skin services are not ready. The built-in presentation is "
        "still available.";
    if (context.skinRecoveryResult &&
        !context.skinRecoveryResult->diagnostics.empty()) {
      availabilityMessage += " " + diagnosticPresentation(
          context.skinRecoveryResult->diagnostics.front());
    }
    body->addView(makeWrappedText(
        availabilityMessage + " Built-in gameplay controls remain available "
                              "in the Timing, Visual, and Lane tabs.",
        metrics.bodyTextSize, ui_theme::textSecondary()));
    const bool canRetryStartup =
        skin::luaGameplaySkinsAvailable() && context.skinRecoveryResult &&
        context.skinRecoveryResult->disposition ==
            skin::SkinRecoveryDisposition::Failed;
    if (canRetryStartup) {
      body->addView(makeGameplaySkinAction(
          metrics, "Retry Startup", true, [this]() {
            const bool recovered = context.retryGameplaySkinServices();
            gameplaySkinUiMessage = recovered
                                      ? "Gameplay skin services restarted."
                                      : "Gameplay skin services remain unavailable.";
            ensureGameplaySkinSettingsController();
            lastLayoutWidth = -1;
          }));
    }
    column->addView(makeCard(metrics, "Gameplay Skins", "Availability", body,
                             metrics.modeCardHeight, metrics.cardsWidth));
    return column;
  }

  const auto &snapshot = gameplaySkinSettingsController->snapshot();
  const auto actionAvailability =
      skin::gameplaySkinSettingsActionAvailability(snapshot);
  const bool ordinaryActionsEnabled = actionAvailability.ordinaryActions;
  gameplaySkinControlsBuiltDisabled = !ordinaryActionsEnabled;
  auto rerender = [this]() { lastLayoutWidth = -1; };

  auto *overview = new View();
  overview->setFlexDirection(FlexDirection::Column);
  overview->setGap(static_cast<float>(metrics.cardGap));
  overview->addView(makeWrappedText(
      "Install and configure Beatoraja gameplay skins. Installing, scanning, "
      "and revalidation evaluate Lua declarations in the compatibility runtime.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  const std::filesystem::path visibleSkinRoot =
      context.skinStorageRoots ? context.skinStorageRoots->visiblePackages
                               : Utils::GetDocumentsPath("Skins");
  std::string visibleSkinFolder = Utils::GetStoragePathUtf8RelativeToDocuments(
      visibleSkinRoot, "Skins");
  if (visibleSkinFolder.empty()) {
    visibleSkinFolder = "Documents/Skins";
  }
  overview->addView(makeWrappedText("Files location: " + visibleSkinFolder,
                                    metrics.smallTextSize,
                                    ui_theme::textMuted()));
  gameplaySkinStatusText = makeWrappedText(
      snapshot.statusMessage, metrics.smallTextSize, ui_theme::textSecondary());
  overview->addView(gameplaySkinStatusText);
  gameplaySkinUiMessageText = makeWrappedText(
      gameplaySkinUiMessage != snapshot.statusMessage ? gameplaySkinUiMessage
                                                       : std::string{},
      metrics.smallTextSize, ui_theme::textSecondary());
  overview->addView(gameplaySkinUiMessageText);
  auto *safetyRow = new View();
  safetyRow->setFlexDirection(FlexDirection::Row);
  safetyRow->setFlexWrap(YGWrapWrap);
  safetyRow->setAlignItems(YGAlignCenter);
  safetyRow->setGap(metrics.compact ? 8.0f : 10.0f);
  auto *safetyLabel = makeText("Skin safety", metrics.smallTextSize,
                               ui_theme::textSecondary(), TextView::LEFT,
                               TextView::MIDDLE);
  safetyLabel->setMinWidth(0.0f);
  safetyLabel->setFlexShrink(1.0f);
  safetyRow->addView(safetyLabel);
  auto *safetyDropdown = new DropdownView(
      {.onOpenChanged =
           [this](bool open) {
             gameplaySkinSafetyDropdownOpen = open;
           },
       .onOptionSelectedResult = [this](const std::string &id) {
         gameplaySkinSafetyDropdownOpen = false;
         int value = static_cast<int>(skin::SkinSafetyLevel::Standard);
         const auto parsed = std::from_chars(id.data(), id.data() + id.size(),
                                             value);
         if (parsed.ec == std::errc{} &&
             parsed.ptr == id.data() + id.size() &&
             value >= static_cast<int>(skin::SkinSafetyLevel::Standard) &&
             value <= static_cast<int>(skin::SkinSafetyLevel::Unrestricted)) {
           return handleGameplaySkinActionResult(
               gameplaySkinSettingsController->setSafetyLevel(
                   static_cast<skin::SkinSafetyLevel>(value)));
         }
         return false;
       }},
      overlayPortal);
  safetyDropdown->refresh(
      {.label = "",
       .selectedId = std::to_string(static_cast<int>(snapshot.safetyLevel)),
       .options = {{.id = "0", .label = safetyLevelLabel(skin::SkinSafetyLevel::Standard), .available = ordinaryActionsEnabled},
                   {.id = "1", .label = safetyLevelLabel(skin::SkinSafetyLevel::BeatorajaCompatibility), .available = ordinaryActionsEnabled},
                   {.id = "2", .label = safetyLevelLabel(skin::SkinSafetyLevel::Unrestricted), .available = ordinaryActionsEnabled}},
       .open = gameplaySkinSafetyDropdownOpen,
       .enabled = ordinaryActionsEnabled,
       .maxVisibleItems = 3,
       .menuWidth = 0.0f});
  safetyRow->addView(safetyDropdown);
  overview->addView(safetyRow);
  column->addView(makeCard(metrics, "Gameplay Skins", "Availability and mode",
                           overview, metrics.modeCardHeight,
                           metrics.cardsWidth));

  auto *imports = new View();
  imports->setFlexDirection(FlexDirection::Column);
  imports->setGap(static_cast<float>(metrics.cardGap));
  auto *importActions = new View();
  importActions->setFlexDirection(FlexDirection::Row);
  importActions->setFlexWrap(YGWrapWrap);
  importActions->setGap(metrics.compact ? 8.0f : 10.0f);
  importActions->addView(makeGameplaySkinAction(
      metrics, "Import Archive", ordinaryActionsEnabled, [this]() {
        gameplaySkinReplaceConfirmationArmed = false;
        handleGameplaySkinActionResult(
            gameplaySkinSettingsController->beginArchiveImport());
      }));
  importActions->addView(makeGameplaySkinAction(
      metrics, "Add Folder", ordinaryActionsEnabled, [this]() {
        gameplaySkinReplaceConfirmationArmed = false;
        handleGameplaySkinActionResult(
            gameplaySkinSettingsController->beginFolderImport());
      }));
  importActions->addView(makeGameplaySkinAction(
      metrics, "Rescan", ordinaryActionsEnabled, [this]() {
        handleGameplaySkinActionResult(
            gameplaySkinSettingsController->requestRescan());
      }));
  if (actionAvailability.canCancel) {
    importActions->addView(makeGameplaySkinAction(
        metrics, "Cancel", true,
        [this, rerender]() {
          gameplaySkinSettingsController->cancelOperation();
          rerender();
        },
        ui_theme::coral()));
  }
  imports->addView(importActions);
  if (snapshot.hasPackageProgress) {
    imports->addView(makeWrappedText(
        skin::gameplaySkinPackageProgressDisplayText(snapshot.progress),
        metrics.smallTextSize, ui_theme::cyan()));
  }
  if (snapshot.rescanProgress.phase != skin::SkinRescanProgressPhase::Idle &&
      snapshot.rescanProgress.phase !=
          skin::SkinRescanProgressPhase::Succeeded &&
      snapshot.rescanProgress.phase != skin::SkinRescanProgressPhase::Failed) {
    imports->addView(makeWrappedText(
        skin::gameplaySkinRescanProgressDisplayText(snapshot.rescanProgress),
        metrics.smallTextSize, ui_theme::cyan()));
  }
  if (snapshot.preparedName) {
    auto *nameRow = new View();
    nameRow->setFlexDirection(FlexDirection::Row);
    nameRow->setFlexWrap(YGWrapWrap);
    nameRow->setGap(metrics.compact ? 8.0f : 10.0f);
    const bool collision = snapshot.collisionPackage.has_value();
    if (collision) {
      nameRow->addView(makeWrappedText(
          "Existing package: " + snapshot.collisionPackage->directoryName,
          metrics.bodyTextSize, ui_theme::textSecondary()));
    }
    const std::string installLabel =
        collision ? (gameplaySkinReplaceConfirmationArmed ? "Confirm Replace"
                                                          : "Replace Existing")
                  : "Install";
    nameRow->addView(makeGameplaySkinAction(
        metrics, installLabel, actionAvailability.canInstallPrepared,
        [this, collision]() {
          if (collision && !gameplaySkinReplaceConfirmationArmed) {
            gameplaySkinReplaceConfirmationArmed = true;
            gameplaySkinUiMessage =
                "Tap Confirm Replace to replace the installed package.";
            lastLayoutWidth = -1;
            return;
          }
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->confirmPreparedImport(
                  collision ? skin::PackageCollisionPolicy::Replace
                            : skin::PackageCollisionPolicy::Reject));
          gameplaySkinReplaceConfirmationArmed = false;
        },
        ui_theme::lime()));
    imports->addView(nameRow);
  }
  column->addView(makeCard(metrics, "Install",
                           "Import a .zip or an unpacked folder.", imports,
                           metrics.modeCardHeight, metrics.cardsWidth));

  std::vector<skin::GameplaySkinTrait> traits(
      skin::gameplaySkinTraits().begin(), skin::gameplaySkinTraits().end());
  std::ranges::sort(traits, {}, &skin::GameplaySkinTrait::keyMode);
  if (!std::ranges::any_of(traits, [this](const auto &trait) {
        return trait.skinType == gameplaySkinActiveTraitSkinType;
      })) {
    gameplaySkinActiveTraitSkinType = traits.front().skinType;
  }

  auto *traitWorkspace = new View();
  traitWorkspace->setFlexDirection(FlexDirection::Row);
  traitWorkspace->setGap(static_cast<float>(metrics.secondaryGap));
  traitWorkspace->setAlignItems(YGAlignStretch);
  const int traitTabWidth = metrics.compact ? 96 : 124;
  auto *traitTabs = new View();
  traitTabs->setFlexDirection(FlexDirection::Column);
  traitTabs->setGap(metrics.compact ? 8.0f : 10.0f);
  traitTabs->setWidth(static_cast<float>(traitTabWidth));
  traitTabs->setFlexShrink(0.0f);
  for (const auto &trait : traits) {
    const bool active = trait.skinType == gameplaySkinActiveTraitSkinType;
    auto *tab = active
                    ? makeAccentButton(
                          traitTabWidth, metrics.actionButtonHeight,
                          makeText(std::string(trait.label),
                                   metrics.smallTextSize,
                                   ui_theme::textPrimary(), TextView::CENTER,
                                   TextView::MIDDLE),
                          ui_theme::cyan())
                    : makeControlButton(
                          traitTabWidth, metrics.actionButtonHeight,
                          makeText(std::string(trait.label),
                                   metrics.smallTextSize,
                                   ui_theme::textPrimary(), TextView::CENTER,
                                   TextView::MIDDLE));
    tab->setOnClickListener([this, skinType = trait.skinType]() {
      if (gameplaySkinActiveTraitSkinType == skinType) {
        return;
      }
      gameplaySkinActiveTraitSkinType = skinType;
      gameplaySkinTraitDropdownOpen = false;
      gameplaySkinConfigurationDropdownOpenKey.clear();
      lastLayoutWidth = -1;
    });
    traitTabs->addView(tab);
  }
  traitWorkspace->addView(traitTabs);

  auto *traitPanel = new View();
  traitPanel->setFlexDirection(FlexDirection::Column);
  traitPanel->setGap(metrics.compact ? 10.0f : 12.0f);
  traitPanel->setFlex(1.0f);
  traitPanel->setMinWidth(0.0f);
  const auto activeTrait = std::ranges::find_if(
      traits, [this](const auto &trait) {
        return trait.skinType == gameplaySkinActiveTraitSkinType;
      });
  const std::string traitLabel = std::string(activeTrait->label);
  traitPanel->addView(makeWrappedText(
      traitLabel + " gameplay skin", metrics.bodyTextSize,
      ui_theme::textPrimary()));

  std::vector<const skin::GameplaySkinEntryRow *> selectableRows;
  for (const auto &candidate : snapshot.entries) {
    if (candidate.validation ==
            skin::SkinValidationDisposition::SelectableGameplay &&
        candidate.metadata.skinType == gameplaySkinActiveTraitSkinType) {
      selectableRows.push_back(&candidate);
    }
  }
  const auto selected = snapshot.selectedGameplayEntries.find(
      gameplaySkinActiveTraitSkinType);
  const skin::GameplaySkinEntryRow *selectedRow = nullptr;
  if (selected != snapshot.selectedGameplayEntries.end()) {
    const auto selectedCandidate = std::ranges::find_if(
        selectableRows, [&selected](const auto *candidate) {
          return candidate->entry == selected->second;
        });
    if (selectedCandidate != selectableRows.end()) {
      selectedRow = *selectedCandidate;
    }
  }

  std::vector<skin::SkinEntryId> dropdownEntries;
  std::vector<DropdownView::Option> dropdownOptions = {
      {.id = "", .label = "Built-in", .available = ordinaryActionsEnabled},
  };
  dropdownEntries.reserve(selectableRows.size());
  for (const auto *candidate : selectableRows) {
    dropdownEntries.push_back(candidate->entry);
    const std::string displayName = candidate->metadata.displayName.empty()
                                        ? candidate->entry.packageRelativePath
                                        : candidate->metadata.displayName;
    dropdownOptions.push_back(
        {.id = candidate->entry.collisionKey,
         .label = displayName + " — " + candidate->entry.package.directoryName,
         .available = ordinaryActionsEnabled});
  }
  auto *skinDropdown = new DropdownView(
      {.onOpenChanged =
           [this](bool open) {
             gameplaySkinTraitDropdownOpen = open;
           },
       .onOptionSelectedResult =
           [this, skinType = gameplaySkinActiveTraitSkinType,
            entries = std::move(dropdownEntries)](const std::string &id) {
             gameplaySkinTraitDropdownOpen = false;
             gameplaySkinConfigurationDropdownOpenKey.clear();
             if (id.empty()) {
               return handleGameplaySkinActionResult(
                   gameplaySkinSettingsController->clearGameplayTrait(skinType));
             }
             const auto selectedEntry = std::ranges::find_if(
                 entries, [&id](const auto &entry) {
                   return entry.collisionKey == id;
                 });
             if (selectedEntry != entries.end()) {
               return handleGameplaySkinActionResult(
                   gameplaySkinSettingsController->selectGameplayTrait(
                       skinType, *selectedEntry));
             }
             return false;
           }},
      overlayPortal);
  skinDropdown->refresh(
      {.label = "",
       .selectedId = selectedRow ? selectedRow->entry.collisionKey : "",
       .options = std::move(dropdownOptions),
       .open = gameplaySkinTraitDropdownOpen,
       .enabled = ordinaryActionsEnabled,
       .maxVisibleItems = metrics.compact ? 5 : 7,
       .menuWidth = static_cast<float>(
           std::max(220, metrics.cardsWidth - traitTabWidth -
                             metrics.secondaryGap - metrics.cardPadding * 2))});
  auto *skinDropdownRow = new View();
  skinDropdownRow->setFlexDirection(FlexDirection::Row);
  skinDropdownRow->setFlexWrap(YGWrapWrap);
  skinDropdownRow->setAlignItems(YGAlignCenter);
  skinDropdownRow->setGap(metrics.compact ? 8.0f : 10.0f);
  auto *skinDropdownLabel =
      makeText("Skin", metrics.smallTextSize, ui_theme::textSecondary(),
               TextView::LEFT, TextView::MIDDLE);
  skinDropdownLabel->setMinWidth(0.0f);
  skinDropdownLabel->setFlexShrink(1.0f);
  skinDropdownRow->addView(skinDropdownLabel);
  skinDropdownRow->addView(skinDropdown);
  traitPanel->addView(skinDropdownRow);
  if (selected != snapshot.selectedGameplayEntries.end() &&
      selectedRow == nullptr) {
    traitPanel->addView(makeWrappedText(
        "The selected skin is no longer available. Choose Built-in or another "
        "validated skin.",
        metrics.smallTextSize, ui_theme::coral()));
  } else if (selectableRows.empty()) {
    traitPanel->addView(makeWrappedText(
        "No validated " + traitLabel + " gameplay skin is installed.",
        metrics.smallTextSize, ui_theme::textSecondary()));
  }

  if (selectedRow != nullptr) {
    const auto &row = *selectedRow;
    auto *entryBody = new View();
    entryBody->setFlexDirection(FlexDirection::Column);
    entryBody->setGap(metrics.compact ? 10.0f : 12.0f);
    const std::string title = row.metadata.displayName.empty()
                                  ? row.entry.packageRelativePath
                                  : row.metadata.displayName;
    entryBody->addView(makeWrappedText(
        title + " — " + validationLabel(row.validation),
        metrics.bodyTextSize,
        row.validation == skin::SkinValidationDisposition::SelectableGameplay
            ? ui_theme::lime()
            : ui_theme::textSecondary()));
    std::string metadata = "Package: " + row.entry.package.directoryName;
    if (!row.metadata.author.empty()) {
      metadata += " • " + row.metadata.author;
    }
    if (row.metadata.authoredWidth > 0 && row.metadata.authoredHeight > 0) {
      metadata += " • " + std::to_string(row.metadata.authoredWidth) + "×" +
                  std::to_string(row.metadata.authoredHeight);
    }
    entryBody->addView(makeWrappedText(metadata, metrics.smallTextSize,
                                       ui_theme::textMuted()));
    gameplaySkinConfigurationDigestText = makeWrappedText(
        "Revision: " + row.revisionDigest +
            " • Configuration: " + row.configurationDigest,
        metrics.smallTextSize, ui_theme::textMuted());
    entryBody->addView(gameplaySkinConfigurationDigestText);
    appendSelectedSkinHudSettings(entryBody, metrics, false);
    for (const auto &diagnostic : row.diagnostics) {
      entryBody->addView(makeWrappedText(diagnosticPresentation(diagnostic),
                                         metrics.smallTextSize,
                                         ui_theme::textSecondary()));
    }

    const auto appendOption = [&](const auto &option) {
      if (option.choices.empty()) {
        return;
      }
      auto selected = option.choices.begin();
      if (const auto configured = row.settings.options.find(option.name);
          configured != row.settings.options.end()) {
        if (const auto match =
                std::find_if(option.choices.begin(), option.choices.end(),
                             [&](const auto &choice) {
                               return choice.value == configured->second;
                             });
            match != option.choices.end()) {
          selected = match;
        }
      } else if (const auto match =
                     std::find_if(option.choices.begin(), option.choices.end(),
                                  [&](const auto &choice) {
                                    return choice.label == option.defaultLabel;
                                  });
                 match != option.choices.end()) {
        selected = match;
      }
      const auto selectedIndex = static_cast<std::size_t>(
          std::distance(option.choices.begin(), selected));
      const bool hasBeatorajaRandomChoice =
          !option.choices.empty() && option.choices.back().label == "Random" &&
          option.choices.back().value == -1;
      if (option.choices.size() >= 3 || hasBeatorajaRandomChoice) {
        const std::string dropdownKey =
            row.entry.collisionKey + "|option|" + option.name;
        std::vector<DropdownView::Option> options;
        std::vector<int> values;
        options.reserve(option.choices.size());
        values.reserve(option.choices.size());
        for (std::size_t index = 0; index < option.choices.size(); ++index) {
          options.push_back(
              {.id = std::to_string(index),
               .label = option.choices[index].label,
               .available = ordinaryActionsEnabled});
          values.push_back(option.choices[index].value);
        }
        auto *dropdown = new DropdownView(
            {.onOpenChanged =
                 [this, dropdownKey](bool open) {
                   gameplaySkinConfigurationDropdownOpenKey =
                       open ? dropdownKey : std::string{};
                 },
             .onOptionSelectedResult =
                 [this, entry = row.entry, name = option.name,
                  values = std::move(values)](const std::string &id) {
                   gameplaySkinConfigurationDropdownOpenKey.clear();
                   std::size_t index = 0;
                   const auto parsed = std::from_chars(
                       id.data(), id.data() + id.size(), index);
                   if (parsed.ec == std::errc{} &&
                       parsed.ptr == id.data() + id.size() &&
                       index < values.size()) {
                     return handleGameplaySkinActionResult(
                         gameplaySkinSettingsController->setOption(
                             entry, name, values[index]));
                   }
                   return false;
                 }},
            overlayPortal);
        dropdown->refresh(
            {.label = "",
             .selectedId = std::to_string(selectedIndex),
             .options = std::move(options),
             .open = gameplaySkinConfigurationDropdownOpenKey == dropdownKey,
             .enabled = ordinaryActionsEnabled,
             .maxVisibleItems = metrics.compact ? 5 : 7,
             .menuWidth = 0.0f});
        auto *dropdownRow = new View();
        dropdownRow->setFlexDirection(FlexDirection::Row);
        dropdownRow->setFlexWrap(YGWrapWrap);
        dropdownRow->setAlignItems(YGAlignCenter);
        dropdownRow->setGap(metrics.compact ? 8.0f : 10.0f);
        auto *dropdownLabel =
            makeText(option.name, metrics.smallTextSize,
                     ui_theme::textSecondary(), TextView::LEFT,
                     TextView::MIDDLE);
        dropdownLabel->setMinWidth(0.0f);
        dropdownLabel->setFlexShrink(1.0f);
        dropdownRow->addView(dropdownLabel);
        dropdownRow->addView(dropdown);
        entryBody->addView(dropdownRow);
      } else {
        std::vector<GameplaySkinChoiceButton> choices;
        choices.reserve(option.choices.size());
        for (const auto &choice : option.choices) {
          choices.push_back(
              {.label = choice.label,
               .selected = choice.value == selected->value,
               .tryAction = [this, entry = row.entry, name = option.name,
                             value = choice.value]() {
                 return handleGameplaySkinActionResult(
                     gameplaySkinSettingsController->setOption(entry, name,
                                                               value));
               }});
        }
        entryBody->addView(makeGameplaySkinChoiceRow(
            metrics, option.name, ordinaryActionsEnabled, std::move(choices)));
      }
    };

    const auto appendFile = [&](const auto &file) {
      if (file.choices.empty()) {
        return;
      }
      std::string selected = file.defaultValue;
      if (const auto configured = row.settings.filePaths.find(file.name);
          configured != row.settings.filePaths.end()) {
        selected = configured->second;
      }
      auto current =
          std::find(file.choices.begin(), file.choices.end(), selected);
      if (current == file.choices.end()) {
        current = file.choices.begin();
      }
      const auto selectedIndex = static_cast<std::size_t>(
          std::distance(file.choices.begin(), current));
      if (file.choices.size() >= 3) {
        const std::string dropdownKey =
            row.entry.collisionKey + "|file|" + file.name;
        std::vector<DropdownView::Option> options;
        options.reserve(file.choices.size());
        for (std::size_t index = 0; index < file.choices.size(); ++index) {
          options.push_back(
              {.id = std::to_string(index),
               .label = file.choices[index],
               .available = ordinaryActionsEnabled});
        }
        auto *dropdown = new DropdownView(
            {.onOpenChanged =
                 [this, dropdownKey](bool open) {
                   gameplaySkinConfigurationDropdownOpenKey =
                       open ? dropdownKey : std::string{};
                 },
             .onOptionSelectedResult =
                 [this, entry = row.entry, name = file.name,
                  choices = file.choices](const std::string &id) {
                   gameplaySkinConfigurationDropdownOpenKey.clear();
                   std::size_t index = 0;
                   const auto parsed = std::from_chars(
                       id.data(), id.data() + id.size(), index);
                   if (parsed.ec == std::errc{} &&
                       parsed.ptr == id.data() + id.size() &&
                       index < choices.size()) {
                     return handleGameplaySkinActionResult(
                         gameplaySkinSettingsController->setFileChoice(
                             entry, name, choices[index]));
                   }
                   return false;
                 }},
            overlayPortal);
        dropdown->refresh(
            {.label = "",
             .selectedId = std::to_string(selectedIndex),
             .options = std::move(options),
             .open = gameplaySkinConfigurationDropdownOpenKey == dropdownKey,
             .enabled = ordinaryActionsEnabled,
             .maxVisibleItems = metrics.compact ? 5 : 7,
             .menuWidth = 0.0f});
        auto *dropdownRow = new View();
        dropdownRow->setFlexDirection(FlexDirection::Row);
        dropdownRow->setFlexWrap(YGWrapWrap);
        dropdownRow->setAlignItems(YGAlignCenter);
        dropdownRow->setGap(metrics.compact ? 8.0f : 10.0f);
        auto *dropdownLabel =
            makeText(file.name, metrics.smallTextSize, ui_theme::textSecondary(),
                     TextView::LEFT, TextView::MIDDLE);
        dropdownLabel->setMinWidth(0.0f);
        dropdownLabel->setFlexShrink(1.0f);
        dropdownRow->addView(dropdownLabel);
        dropdownRow->addView(dropdown);
        entryBody->addView(dropdownRow);
      } else {
        std::vector<GameplaySkinChoiceButton> choices;
        choices.reserve(file.choices.size());
        for (const auto &choice : file.choices) {
          choices.push_back(
              {.label = choice,
               .selected = choice == *current,
               .tryAction = [this, entry = row.entry, name = file.name,
                             value = choice]() {
                 return handleGameplaySkinActionResult(
                     gameplaySkinSettingsController->setFileChoice(entry, name,
                                                                   value));
               }});
        }
        entryBody->addView(makeGameplaySkinChoiceRow(
            metrics, file.name, ordinaryActionsEnabled, std::move(choices)));
      }
    };

    const auto appendOffset = [&](const auto &offset) {
      skin::ConfigOffset configured{};
      if (const auto saved = row.settings.offsets.find(offset.name);
          saved != row.settings.offsets.end()) {
        configured = saved->second;
      }
      auto *offsetControls = new View();
      offsetControls->setFlexDirection(FlexDirection::Row);
      offsetControls->setFlexWrap(YGWrapWrap);
      offsetControls->setGap(metrics.compact ? 8.0f : 10.0f);
      offsetControls->addView(makeWrappedText(
          offset.name, metrics.smallTextSize, ui_theme::textSecondary()));
      auto addOffsetComponent =
          [this, &metrics, &row, ordinaryActionsEnabled, offsetControls,
           &offset,
           configured](const char *label, skin::OffsetPermissionMask permission,
                       int skin::ConfigOffset::*member) {
            if ((offset.permissions & permission) == 0) {
              return;
            }
            auto *group = new View();
            group->setFlexDirection(FlexDirection::Column);
            group->setGap(metrics.compact ? 4.0f : 6.0f);
            group->addView(makeWrappedText(label, metrics.smallTextSize,
                                           ui_theme::textSecondary()));
            auto *input =
                makeTextInput(metrics, metrics.compact ? 96 : 112);
            input->setEditingText(std::to_string(configured.*member));
            if (ordinaryActionsEnabled) {
              input->onEditingFinished(
                  [this, input, entry = row.entry, name = offset.name,
                   member](const std::string &) {
                    auto configured = gameplaySkinOffsetForEntry(entry, name);
                    configured.*member = sanitizeOffsetComponent(
                        input->getText(), configured.*member);
                    handleGameplaySkinActionResult(
                        gameplaySkinSettingsController->setOffset(entry, name,
                                                                  configured));
                  });
            }
            group->addView(input);
            offsetControls->addView(group);
          };
      addOffsetComponent("X", skin::kOffsetPermissionX, &skin::ConfigOffset::x);
      addOffsetComponent("Y", skin::kOffsetPermissionY, &skin::ConfigOffset::y);
      addOffsetComponent("W", skin::kOffsetPermissionW, &skin::ConfigOffset::w);
      addOffsetComponent("H", skin::kOffsetPermissionH, &skin::ConfigOffset::h);
      addOffsetComponent("R", skin::kOffsetPermissionR, &skin::ConfigOffset::r);
      addOffsetComponent("A", skin::kOffsetPermissionA, &skin::ConfigOffset::a);
      entryBody->addView(offsetControls);
    };

    for (const auto &catalogItem :
         skin::gameplaySkinSettingsCatalogItems(row.metadata)) {
      switch (catalogItem.kind) {
      case skin::GameplaySkinCatalogItemKind::CategoryHeading:
        entryBody->addView(makeWrappedText(
            catalogItem.label, metrics.bodyTextSize, ui_theme::cyan()));
        break;
      case skin::GameplaySkinCatalogItemKind::Separator: {
        auto *separator = new View();
        separator->setHeight(metrics.compact ? 8.0f : 10.0f);
        entryBody->addView(separator);
        break;
      }
      case skin::GameplaySkinCatalogItemKind::Option:
        if (catalogItem.declarationIndex < row.metadata.options.size()) {
          appendOption(row.metadata.options[catalogItem.declarationIndex]);
        }
        break;
      case skin::GameplaySkinCatalogItemKind::File:
        if (catalogItem.declarationIndex < row.metadata.files.size()) {
          appendFile(row.metadata.files[catalogItem.declarationIndex]);
        }
        break;
      case skin::GameplaySkinCatalogItemKind::Offset:
        if (catalogItem.declarationIndex < row.metadata.offsets.size()) {
          appendOffset(row.metadata.offsets[catalogItem.declarationIndex]);
        }
        break;
      }
    }

    auto *customViewport = new View();
    customViewport->setFlexDirection(FlexDirection::Row);
    customViewport->setFlexWrap(YGWrapWrap);
    customViewport->setGap(metrics.compact ? 8.0f : 10.0f);
    customViewport->addView(makeGameplaySkinAction(
        metrics, "Custom Base: Fit", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          const auto viewport = skin::gameplaySkinViewportWithCustomBase(
              gameplaySkinViewportForEntry(entry),
              skin::CustomViewportBase::Fit);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    customViewport->addView(makeGameplaySkinAction(
        metrics, "Custom Base: Stretch", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          const auto viewport = skin::gameplaySkinViewportWithCustomBase(
              gameplaySkinViewportForEntry(entry),
              skin::CustomViewportBase::Stretch);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    auto addViewportComponent = [this, &metrics, &row, ordinaryActionsEnabled,
                                 customViewport](const char *label, float value,
                                                 bool scale, bool horizontal) {
      auto *group = new View();
      group->setFlexDirection(FlexDirection::Column);
      group->setGap(metrics.compact ? 4.0f : 6.0f);
      group->addView(makeWrappedText(label, metrics.smallTextSize,
                                     ui_theme::textSecondary()));
      auto *input = makeTextInput(metrics, metrics.compact ? 132 : 156);
      input->setEditingText(formatViewportComponent(value));
      if (ordinaryActionsEnabled) {
        input->onEditingFinished([this, input, entry = row.entry, scale,
                                  horizontal](const std::string &) {
          const float minimum =
              scale ? skin::SkinProfileSettingsPolicy::minCustomScale
                    : skin::SkinProfileSettingsPolicy::minCustomTranslation;
          const float maximum =
              scale ? skin::SkinProfileSettingsPolicy::maxCustomScale
                    : skin::SkinProfileSettingsPolicy::maxCustomTranslation;
          auto viewport = gameplaySkinViewportForEntry(entry);
          float &component =
              scale ? (horizontal ? viewport.scaleX : viewport.scaleY)
                    : (horizontal ? viewport.translateX : viewport.translateY);
          component = sanitizeViewportComponent(input->getText(), component,
                                                minimum, maximum);
          viewport = skin::gameplaySkinViewportWithMode(
              viewport, skin::ViewportMode::Custom);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        });
      }
      group->addView(input);
      customViewport->addView(group);
    };
    addViewportComponent("Custom X", row.settings.viewport.translateX, false,
                         true);
    addViewportComponent("Custom Y", row.settings.viewport.translateY, false,
                         false);
    addViewportComponent("Custom Width", row.settings.viewport.scaleX, true,
                         true);
    addViewportComponent("Custom Height", row.settings.viewport.scaleY, true,
                         false);
    entryBody->addView(customViewport);

    auto *actions = new View();
    actions->setFlexDirection(FlexDirection::Row);
    actions->setFlexWrap(YGWrapWrap);
    actions->setGap(metrics.compact ? 8.0f : 10.0f);
    actions->addView(makeGameplaySkinAction(
        metrics, "Revalidate", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->requestRevalidation(entry));
        }));
    const bool confirmingRemoval =
        gameplaySkinRemovalConfirmationKey == row.entry.package.collisionKey;
    actions->addView(makeGameplaySkinAction(
        metrics, confirmingRemoval ? "Confirm Remove" : "Remove",
        ordinaryActionsEnabled,
        [this, package = row.entry.package, confirmingRemoval]() {
          if (!confirmingRemoval) {
            gameplaySkinRemovalConfirmationKey = package.collisionKey;
            gameplaySkinUiMessage =
                "Tap Confirm Remove to uninstall this package.";
            lastLayoutWidth = -1;
            return;
          }
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->requestRemoval(package));
          gameplaySkinRemovalConfirmationKey.clear();
        },
        ui_theme::coral()));
    actions->addView(makeGameplaySkinAction(
        metrics, "Fit", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          const auto viewport = skin::gameplaySkinViewportWithMode(
              gameplaySkinViewportForEntry(entry), skin::ViewportMode::Fit);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    actions->addView(makeGameplaySkinAction(
        metrics, "Stretch", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          const auto viewport = skin::gameplaySkinViewportWithMode(
              gameplaySkinViewportForEntry(entry),
              skin::ViewportMode::Stretch);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    actions->addView(makeGameplaySkinAction(
        metrics, "Custom", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          const auto viewport = skin::gameplaySkinViewportWithMode(
              gameplaySkinViewportForEntry(entry), skin::ViewportMode::Custom);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    actions->addView(makeGameplaySkinAction(
        metrics, "Reset Layout", ordinaryActionsEnabled,
        [this, entry = row.entry]() {
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->resetLayout(entry));
        },
        ui_theme::coral()));
    entryBody->addView(actions);
    traitPanel->addView(entryBody);
  } else if (selected == snapshot.selectedGameplayEntries.end()) {
    auto *builtInBody = new View();
    builtInBody->setFlexDirection(FlexDirection::Column);
    builtInBody->setGap(metrics.compact ? 10.0F : 12.0F);
    appendBuiltInGameplayTraitSettings(builtInBody, metrics,
                                       activeTrait->keyMode);
    traitPanel->addView(builtInBody);
  }

  traitWorkspace->addView(traitPanel);
  column->addView(makeCard(metrics, "Gameplay Skin Traits",
                           "Choose a keymode, then a skin and its settings.",
                           traitWorkspace, metrics.modeCardHeight,
                           metrics.cardsWidth));

  const auto managementRows = skin::gameplaySkinManagementEntries(snapshot);
  if (!managementRows.empty()) {
    auto *management = new View();
    management->setFlexDirection(FlexDirection::Column);
    management->setGap(metrics.compact ? 10.0f : 12.0f);
    for (const auto *row : managementRows) {
      auto *entryBody = new View();
      entryBody->setFlexDirection(FlexDirection::Column);
      entryBody->setGap(metrics.compact ? 6.0f : 8.0f);
      const std::string title = row->metadata.displayName.empty()
                                    ? row->entry.packageRelativePath
                                    : row->metadata.displayName;
      entryBody->addView(makeWrappedText(
          title + " — " + validationLabel(row->validation),
          metrics.bodyTextSize, ui_theme::coral()));
      entryBody->addView(makeWrappedText(
          "Package: " + row->entry.package.directoryName + " • Entry: " +
              row->entry.packageRelativePath,
          metrics.smallTextSize, ui_theme::textMuted()));
      for (const auto &diagnostic : row->diagnostics) {
        entryBody->addView(makeWrappedText(diagnosticPresentation(diagnostic),
                                           metrics.smallTextSize,
                                           ui_theme::textSecondary()));
      }
      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);
      actions->addView(makeGameplaySkinAction(
          metrics, "Revalidate", ordinaryActionsEnabled,
          [this, entry = row->entry]() {
            handleGameplaySkinActionResult(
                gameplaySkinSettingsController->requestRevalidation(entry));
          }));
      const bool confirmingRemoval =
          gameplaySkinRemovalConfirmationKey == row->entry.package.collisionKey;
      actions->addView(makeGameplaySkinAction(
          metrics, confirmingRemoval ? "Confirm Remove" : "Remove",
          ordinaryActionsEnabled,
          [this, package = row->entry.package, confirmingRemoval]() {
            if (!confirmingRemoval) {
              gameplaySkinRemovalConfirmationKey = package.collisionKey;
              gameplaySkinUiMessage =
                  "Tap Confirm Remove to uninstall this package.";
              lastLayoutWidth = -1;
              return;
            }
            handleGameplaySkinActionResult(
                gameplaySkinSettingsController->requestRemoval(package));
            gameplaySkinRemovalConfirmationKey.clear();
          },
          ui_theme::coral()));
      entryBody->addView(actions);
      management->addView(entryBody);
    }
    column->addView(makeCard(
        metrics, "Unavailable Installed Skins",
        "Review validation details, then revalidate or remove an entry.",
        management, metrics.modeCardHeight, metrics.cardsWidth));
  }

  if (snapshot.entries.empty()) {
    auto *empty = new View();
    empty->setFlexDirection(FlexDirection::Column);
    empty->setGap(static_cast<float>(metrics.cardGap));
    empty->addView(makeWrappedText("No installed gameplay skins were found.",
                                   metrics.bodyTextSize,
                                   ui_theme::textSecondary()));
    column->addView(makeCard(metrics, "Installed Skins", "Catalog", empty,
                             metrics.modeCardHeight, metrics.cardsWidth));
  }
  if (!snapshot.history.empty()) {
    auto *history = new View();
    history->setFlexDirection(FlexDirection::Column);
    history->setGap(static_cast<float>(metrics.cardGap));
    for (const auto &record : snapshot.history) {
      std::string recordText =
          "History #" + std::to_string(record.recordSerial) + " • " +
          record.entry.packageRelativePath +
          " • Revision: " + record.revisionDigest +
          " • Configuration: " + record.configurationDigest + " • " +
          diagnosticPresentation(record.diagnostic);
      if (record.luaLine) {
        recordText += " • Lua line " + std::to_string(*record.luaLine);
      }
      if (record.frameSerial) {
        recordText += " • Frame " + std::to_string(*record.frameSerial);
      }
      history->addView(makeWrappedText(recordText, metrics.smallTextSize,
                                       ui_theme::textSecondary()));
    }
    column->addView(makeCard(metrics, "Diagnostic History", "Recent records",
                             history, metrics.modeCardHeight,
                             metrics.cardsWidth));
  }
  return column;
}

void SettingsScene::buildGameplaySkinSafetyOverlay(
    const LayoutMetrics &metrics) {
  if (activeTab != SettingsTab::GameplaySkins ||
      gameplaySkinSettingsController == nullptr ||
      !gameplaySkinSettingsController->snapshot().pendingSafetyLevel) {
    return;
  }

  gameplaySkinSafetyOverlayRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  gameplaySkinSafetyOverlayRoot->setPositionType(YGPositionTypeAbsolute);
  gameplaySkinSafetyOverlayRoot->setPosition(Edge::Left, 0);
  gameplaySkinSafetyOverlayRoot->setPosition(Edge::Top, 0);
  gameplaySkinSafetyOverlayRoot->setZIndex(1050);
  gameplaySkinSafetyOverlayRoot->setFlexDirection(FlexDirection::Column);
  gameplaySkinSafetyOverlayRoot->setAlignItems(YGAlignCenter);
  gameplaySkinSafetyOverlayRoot->setJustifyContent(YGJustifyCenter);
  gameplaySkinSafetyOverlayRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(static_cast<float>(std::min(
      metrics.compact ? 620 : 760, std::max(280, metrics.contentWidth - 32))));
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setGap(metrics.compact ? 14.0F : 18.0F);
  panel->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setCornerRadius(ui_theme::panelRadius());
  panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);
  panel->setThemedBorderColor(ui_theme::hairline);
  panel->setBorderWidth(1);
  panel->addView(makeWrappedText("Enable unrestricted skins",
                                 metrics.sectionTitleSize,
                                 ui_theme::textPrimary()));
  panel->addView(makeWrappedText(
      "This removes every gameplay-skin safeguard. A skin may read or write "
      "outside its package, change process-wide state, or exhaust memory, "
      "storage, CPU, and file descriptors. Enable it only for skins you trust.",
      metrics.bodyTextSize, ui_theme::amber()));

  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(metrics.compact ? 10.0F : 14.0F);
  actions->setJustifyContent(YGJustifyCenter);
  auto *cancel = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Cancel", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE));
  cancel->setOnClickListener([this]() {
    gameplaySkinSettingsController->cancelSafetyLevelChange();
    lastLayoutWidth = -1;
  });
  actions->addView(cancel);
  auto *enable = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Enable Unrestricted", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::coral());
  enable->setOnClickListener([this]() {
    handleGameplaySkinActionResult(
        gameplaySkinSettingsController->confirmSafetyLevelChange());
    lastLayoutWidth = -1;
  });
  actions->addView(enable);
  panel->addView(actions);
  gameplaySkinSafetyOverlayRoot->addView(panel);
  rootLayout->addView(gameplaySkinSafetyOverlayRoot);
}

#endif
