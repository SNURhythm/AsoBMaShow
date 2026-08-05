#include "SettingsSceneShared.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "GameplaySkinSettingsController.h"
#include "GameplaySkinSettingsPresentation.h"
#include "../skin/beatoraja/BeatorajaSkinConfiguration.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

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
  auto *button = makeAccentButton(
      metrics.compact ? 158 : 184, metrics.actionButtonHeight,
      makeText(label, metrics.smallTextSize, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      accent);
  button->setEnabled(enabled);
  if (enabled) {
    button->setOnClickListener(std::move(action));
  }
  return button;
}

const char *validationLabel(skin::SkinValidationDisposition disposition) {
  switch (disposition) {
  case skin::SkinValidationDisposition::Selectable7Key:
    return "7-key selectable";
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

std::string formatViewportComponent(float value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(2) << value;
  return std::move(stream).str();
}

} // namespace

bool SettingsScene::handleGameplaySkinActionResult(
    skin::ControllerActionResult result) {
  const bool accepted = result.accepted;
  if (!result.accepted && !result.message.empty()) {
    gameplaySkinUiMessage = result.message;
  } else if (!result.message.empty()) {
    gameplaySkinUiMessage = result.message;
  }
  lastLayoutWidth = -1;
  return accepted;
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
                        skin::SkinRescanReason::SettingsOpened);
                  }
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
  gameplaySkinSettingsPresentationKey.clear();
  gameplaySkinUiMessage.clear();
  gameplaySkinReplaceConfirmationArmed = false;
  gameplaySkinRemovalConfirmationKey.clear();
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
  const std::string next = skin::gameplaySkinSettingsPresentationKey(snapshot);
  if (next != gameplaySkinSettingsPresentationKey) {
    gameplaySkinSettingsPresentationKey = next;
    if (activeTab == SettingsTab::GameplaySkins) {
      lastLayoutWidth = -1;
    }
  }
}

View *SettingsScene::buildGameplaySkinsTab(const LayoutMetrics &metrics) {
  auto *column = makeGameplaySkinsColumn(metrics);
  if (!skin::luaGameplaySkinsAvailable() ||
      gameplaySkinSettingsController == nullptr) {
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
        availabilityMessage,
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
  auto rerender = [this]() { lastLayoutWidth = -1; };

  auto *overview = new View();
  overview->setFlexDirection(FlexDirection::Column);
  overview->setGap(static_cast<float>(metrics.cardGap));
  overview->addView(makeWrappedText(
      "Install and configure trusted Beatoraja gameplay skins. Lua is never "
      "executed in this settings screen.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  overview->addView(
      makeWrappedText("Files location: On My iPad/AsoBMaShow/Skins",
                      metrics.smallTextSize, ui_theme::textMuted()));
  auto *compatibility = makeGameplaySkinAction(
      metrics,
      snapshot.compatibilityEnabled ? "Use Beatoraja Gameplay Skin: On"
                                    : "Use Beatoraja Gameplay Skin: Off",
      ordinaryActionsEnabled,
      [this, enabled = snapshot.compatibilityEnabled]() {
        handleGameplaySkinActionResult(
            gameplaySkinSettingsController->setCompatibilityEnabled(!enabled));
      },
      snapshot.compatibilityEnabled ? ui_theme::lime() : ui_theme::coral());
  overview->addView(compatibility);
  if (!snapshot.statusMessage.empty()) {
    overview->addView(makeWrappedText(snapshot.statusMessage,
                                      metrics.smallTextSize,
                                      ui_theme::textSecondary()));
  }
  if (!gameplaySkinUiMessage.empty() &&
      gameplaySkinUiMessage != snapshot.statusMessage) {
    overview->addView(makeWrappedText(gameplaySkinUiMessage,
                                      metrics.smallTextSize,
                                      ui_theme::textSecondary()));
  }
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
  if (snapshot.preparedName) {
    auto *nameRow = new View();
    nameRow->setFlexDirection(FlexDirection::Row);
    nameRow->setFlexWrap(YGWrapWrap);
    nameRow->setGap(metrics.compact ? 8.0f : 10.0f);
    TextInputBox *nameInput = nullptr;
    if (actionAvailability.canEditPreparedName) {
      nameInput = makeTextInput(metrics, metrics.compact ? 240 : 320);
      nameInput->setText(snapshot.preparedName->suggestedPackageName);
      nameInput->onEditingFinished([this, nameInput](const std::string &) {
        gameplaySkinReplaceConfirmationArmed = false;
        handleGameplaySkinActionResult(
            gameplaySkinSettingsController->setSuggestedPackageName(
                nameInput->getText()));
      });
      nameRow->addView(nameInput);
    } else {
      nameRow->addView(
          makeWrappedText(snapshot.preparedName->suggestedPackageName,
                          metrics.bodyTextSize, ui_theme::textSecondary()));
    }
    const bool collision = snapshot.collisionPackage.has_value();
    const std::string installLabel =
        collision ? (gameplaySkinReplaceConfirmationArmed ? "Confirm Replace"
                                                          : "Replace Existing")
                  : "Install";
    nameRow->addView(makeGameplaySkinAction(
        metrics, installLabel, actionAvailability.canInstallPrepared,
        [this, nameInput, collision]() {
          if (nameInput == nullptr) {
            return;
          }
          if (collision && !gameplaySkinReplaceConfirmationArmed) {
            gameplaySkinReplaceConfirmationArmed = true;
            gameplaySkinUiMessage =
                "Tap Confirm Replace to replace the installed package.";
            lastLayoutWidth = -1;
            return;
          }
          if (!handleGameplaySkinActionResult(
                  gameplaySkinSettingsController->setSuggestedPackageName(
                      nameInput->getText()))) {
            gameplaySkinReplaceConfirmationArmed = false;
            return;
          }
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->confirmPreparedImport(
                  collision ? skin::PackageCollisionPolicy::Replace
                            : skin::PackageCollisionPolicy::Reject));
          gameplaySkinReplaceConfirmationArmed = false;
        },
        ui_theme::lime()));
    imports->addView(makeWrappedText("Package name", metrics.smallTextSize,
                                     ui_theme::textSecondary()));
    if (!snapshot.preparedName->validationError.empty()) {
      imports->addView(makeWrappedText(snapshot.preparedName->validationError,
                                       metrics.smallTextSize,
                                       ui_theme::coral()));
    }
    imports->addView(nameRow);
  }
  column->addView(makeCard(metrics, "Install",
                           "Import a .zip or an unpacked folder.", imports,
                           metrics.modeCardHeight, metrics.cardsWidth));

  for (const auto &row : snapshot.entries) {
    auto *entryBody = new View();
    entryBody->setFlexDirection(FlexDirection::Column);
    entryBody->setGap(metrics.compact ? 10.0f : 12.0f);
    const std::string title = row.metadata.displayName.empty()
                                  ? row.entry.packageRelativePath
                                  : row.metadata.displayName;
    const bool selectedEntry =
        snapshot.selected7KeyEntry && *snapshot.selected7KeyEntry == row.entry;
    entryBody->addView(makeWrappedText(
        title + " — " + validationLabel(row.validation) +
            (selectedEntry ? " — Selected" : ""),
        metrics.bodyTextSize,
        row.validation == skin::SkinValidationDisposition::Selectable7Key
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
    entryBody->addView(
        makeWrappedText("Revision: " + row.revisionDigest +
                            " • Configuration: " + row.configurationDigest,
                        metrics.smallTextSize, ui_theme::textMuted()));
    for (const auto &category : row.metadata.categories) {
      std::string categoryText = "Category: " + category.name;
      if (!category.items.empty()) {
        categoryText += " • ";
        for (std::size_t index = 0; index < category.items.size(); ++index) {
          if (index != 0) {
            categoryText += ", ";
          }
          categoryText += category.items[index];
        }
      }
      entryBody->addView(makeWrappedText(categoryText, metrics.smallTextSize,
                                         ui_theme::textSecondary()));
    }
    for (const auto &diagnostic : row.diagnostics) {
      entryBody->addView(makeWrappedText(diagnosticPresentation(diagnostic),
                                         metrics.smallTextSize,
                                         ui_theme::textSecondary()));
    }

    for (const auto &option : row.metadata.options) {
      if (option.choices.empty()) {
        continue;
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
      const auto next = option.choices.begin() +
                        ((std::distance(option.choices.begin(), selected) + 1) %
                         static_cast<std::ptrdiff_t>(option.choices.size()));
      entryBody->addView(makeGameplaySkinAction(
          metrics, option.name + ": " + selected->label, ordinaryActionsEnabled,
          [this, entry = row.entry, name = option.name, value = next->value]() {
            handleGameplaySkinActionResult(
                gameplaySkinSettingsController->setOption(entry, name, value));
          }));
    }

    for (const auto &file : row.metadata.files) {
      if (file.choices.empty()) {
        continue;
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
      const auto next = file.choices.begin() +
                        ((std::distance(file.choices.begin(), current) + 1) %
                         static_cast<std::ptrdiff_t>(file.choices.size()));
      entryBody->addView(makeGameplaySkinAction(
          metrics, file.name + ": " + *current, ordinaryActionsEnabled,
          [this, entry = row.entry, name = file.name, value = *next]() {
            handleGameplaySkinActionResult(
                gameplaySkinSettingsController->setFileChoice(entry, name,
                                                              value));
          }));
    }

    for (const auto &offset : row.metadata.offsets) {
      skin::ConfigOffset configured;
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
            for (const int delta : {-1, 1}) {
              const int value = configured.*member;
              const std::string actionLabel = std::string(label) + " " +
                                              std::to_string(value) +
                                              (delta < 0 ? " −" : " +");
              offsetControls->addView(makeGameplaySkinAction(
                  metrics, actionLabel, ordinaryActionsEnabled,
                  [this, entry = row.entry, name = offset.name, configured,
                   member, delta]() mutable {
                    if (delta < 0 &&
                        configured.*member > std::numeric_limits<int>::min()) {
                      --configured.*member;
                    } else if (delta > 0 &&
                               configured.*member <
                                   std::numeric_limits<int>::max()) {
                      ++configured.*member;
                    }
                    handleGameplaySkinActionResult(
                        gameplaySkinSettingsController->setOffset(entry, name,
                                                                  configured));
                  }));
            }
          };
      addOffsetComponent("X", skin::kOffsetPermissionX, &skin::ConfigOffset::x);
      addOffsetComponent("Y", skin::kOffsetPermissionY, &skin::ConfigOffset::y);
      addOffsetComponent("W", skin::kOffsetPermissionW, &skin::ConfigOffset::w);
      addOffsetComponent("H", skin::kOffsetPermissionH, &skin::ConfigOffset::h);
      addOffsetComponent("R", skin::kOffsetPermissionR, &skin::ConfigOffset::r);
      addOffsetComponent("A", skin::kOffsetPermissionA, &skin::ConfigOffset::a);
      entryBody->addView(offsetControls);
    }

    auto *customViewport = new View();
    customViewport->setFlexDirection(FlexDirection::Row);
    customViewport->setFlexWrap(YGWrapWrap);
    customViewport->setGap(metrics.compact ? 8.0f : 10.0f);
    customViewport->addView(makeGameplaySkinAction(
        metrics, "Custom Base: Fit", ordinaryActionsEnabled,
        [this, entry = row.entry, viewport = row.settings.viewport]() mutable {
          viewport = skin::gameplaySkinViewportWithCustomBase(
              viewport, skin::CustomViewportBase::Fit);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    customViewport->addView(makeGameplaySkinAction(
        metrics, "Custom Base: Stretch", ordinaryActionsEnabled,
        [this, entry = row.entry, viewport = row.settings.viewport]() mutable {
          viewport = skin::gameplaySkinViewportWithCustomBase(
              viewport, skin::CustomViewportBase::Stretch);
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
      input->setText(formatViewportComponent(value));
      if (ordinaryActionsEnabled) {
        input->onEditingFinished([this, input, entry = row.entry,
                                  viewport = row.settings.viewport, scale,
                                  horizontal](const std::string &) mutable {
          const float minimum =
              scale ? skin::SkinProfileSettingsPolicy::minCustomScale
                    : skin::SkinProfileSettingsPolicy::minCustomTranslation;
          const float maximum =
              scale ? skin::SkinProfileSettingsPolicy::maxCustomScale
                    : skin::SkinProfileSettingsPolicy::maxCustomTranslation;
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
    actions->addView(makeGameplaySkinAction(
        metrics, "Select",
        ordinaryActionsEnabled &&
            row.validation == skin::SkinValidationDisposition::Selectable7Key,
        [this, entry = row.entry]() {
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->select(entry));
        },
        ui_theme::lime()));
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
        [this, entry = row.entry, viewport = row.settings.viewport]() mutable {
          viewport = skin::gameplaySkinViewportWithMode(
              viewport, skin::ViewportMode::Fit);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    actions->addView(makeGameplaySkinAction(
        metrics, "Stretch", ordinaryActionsEnabled,
        [this, entry = row.entry, viewport = row.settings.viewport]() mutable {
          viewport = skin::gameplaySkinViewportWithMode(
              viewport, skin::ViewportMode::Stretch);
          handleGameplaySkinActionResult(
              gameplaySkinSettingsController->setViewport(entry, viewport));
        }));
    actions->addView(makeGameplaySkinAction(
        metrics, "Custom", ordinaryActionsEnabled,
        [this, entry = row.entry, viewport = row.settings.viewport]() mutable {
          viewport = skin::gameplaySkinViewportWithMode(
              viewport, skin::ViewportMode::Custom);
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
    column->addView(makeCard(metrics, title, row.entry.packageRelativePath,
                             entryBody, metrics.modeCardHeight,
                             metrics.cardsWidth));
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

#endif
