#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"

#include "../ir/IrRankingService.h"
#include "../ir/IrSubmissionService.h"

#include <algorithm>
#include <chrono>
#include <sstream>

using namespace settings_scene;

namespace {

constexpr std::string_view kProviderId = ir::kTachiProviderId;

View *makeIrColumn(const LayoutMetrics &metrics) {
  auto *column = new View();
  column->setFlexDirection(FlexDirection::Column);
  column->setGap(static_cast<float>(metrics.secondaryGap));
  column->setWidth(static_cast<float>(metrics.cardsWidth));
  return column;
}

View *makeActionRow(const LayoutMetrics &metrics) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setFlexWrap(YGWrapWrap);
  row->setAlignItems(YGAlignCenter);
  row->setGap(metrics.compact ? 8.0F : 12.0F);
  row->setWidthPercent(100.0F);
  return row;
}

Button *makeIrButton(const LayoutMetrics &metrics, i18n::Text label,
                     const Color &accent = ui_theme::cyan()) {
  return makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          makeText(label, metrics.bodyTextSize + 2,
                                   ui_theme::textPrimary(), TextView::CENTER,
                                   TextView::MIDDLE),
                          accent);
}

i18n::Text queueStateLabel(ir::IrOutboxState state) {
  switch (state) {
  case ir::IrOutboxState::Pending:
    return i18n::message("settings.ir.pending.label");
  case ir::IrOutboxState::Uploading:
    return i18n::message("settings.ir.uploading.label");
  case ir::IrOutboxState::AwaitingRemoteResult:
    return i18n::message("settings.ir.awaiting_remote_result.label");
  case ir::IrOutboxState::BlockedConfiguration:
    return i18n::message("settings.ir.blocked_by_configuration.label");
  case ir::IrOutboxState::FailedPermanent:
    return i18n::message("settings.ir.failed.label");
  case ir::IrOutboxState::Succeeded:
    return i18n::message("settings.ir.succeeded.label");
  }
  return i18n::message("settings.ir.unknown.label");
}

bool canDiscardStatus(ir::IrOutboxState state) {
  return state == ir::IrOutboxState::Pending ||
         state == ir::IrOutboxState::AwaitingRemoteResult ||
         state == ir::IrOutboxState::BlockedConfiguration ||
         state == ir::IrOutboxState::FailedPermanent;
}

} // namespace

View *SettingsScene::buildIrTab(const LayoutMetrics &metrics) {
  irSettingsModel.reset();
  auto *column = makeIrColumn(metrics);
  const auto driver = context.irDrivers.find(kProviderId);
  if (!driver) {
    auto *body =
        makeWrappedText(i18n::message("settings.ir.bokutachi_driver_unavailable_in_build.message"),
                        metrics.bodyTextSize, ui_theme::textSecondary());
    column->addView(makeCard(metrics, i18n::message("settings.ir.internet_ranking.label"), "", body, 180,
                             metrics.cardsWidth));
    return column;
  }

  const ir::IrDriverCapabilities capabilities = driver->capabilities();
  ir::IrProviderSettings providerSettings;
  if (const auto found =
          context.settings.irProviders.find(std::string(kProviderId));
      found != context.settings.irProviders.end()) {
    providerSettings = found->second;
  }
  ir::sanitizeProviderSettings(providerSettings);

  bool hasCredential = false;
  if (context.profileReady()) {
    hasCredential =
        !context
             .lookupActiveIrCredential(
                 context.profileManager.activeProfile().id, kProviderId)
             .empty();
  }

  ir::IrOutboxCounts counts;
  ir::IrReconciliationStatusSnapshot reconciliationStatus;
  if (context.irSubmissionService) {
    counts = context.irSubmissionService->counts(kProviderId);
    reconciliationStatus =
        context.irSubmissionService->reconciliationStatus(kProviderId);
  }
  const bool serviceActive =
      context.irSubmissionService && context.profileReady() &&
      !context.appInBackground.load(std::memory_order_acquire);

  auto committedSettings = [this](const ir::IrProviderSettings &candidate) {
    context.settings.irProviders[std::string(kProviderId)] = candidate;
    std::string activationError;
    if (!context.activateIrProfileServices(
            context.profileManager.activeProfile().id, context.settings,
            activationError)) {
      SDL_Log("IR services could not apply saved settings");
    }
  };

  ir::IrSettingsActionDependencies dependencies{
      .storeSettings =
          [this](const ir::IrProviderSettings &candidate,
                 std::string &diagnostic) {
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            AppSettings next = context.settings;
            next.irProviders[std::string(kProviderId)] = candidate;
            next.sanitize();
            return context.saveSettingsCandidate(std::move(next), diagnostic);
          },
      .settingsCommitted = std::move(committedSettings),
      .quiesceRemoteWork =
          [this](std::string &diagnostic) {
            return context.pauseIrProfileServices(diagnostic);
          },
      .loadCredential =
          [this](std::optional<std::string> &apiKey, std::string &diagnostic) {
            apiKey.reset();
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            return context.loadIrCredential(
                context.profileManager.activeProfile().id, kProviderId, apiKey,
                diagnostic);
          },
      .invalidateProviderIdentity =
          [this](std::string_view providerId, std::string &diagnostic) {
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            const auto imported =
                context.scoreRepository.ClearImportedIrScores(providerId);
            if (imported.status != ImportedIrScoreProjectionStatus::Applied &&
                imported.status !=
                    ImportedIrScoreProjectionStatus::AlreadyCurrent) {
              diagnostic = imported.diagnostic.empty()
                               ? i18n::tr("settings.ir.imported_ir_scores_failed_cleared.message")
                               : imported.diagnostic;
              return false;
            }
            const auto cleared =
                context.replayRepository.ClearIrProviderAccountEvidence(
                    providerId);
            if (cleared.status != ir::IrOutboxMutationStatus::Updated &&
                cleared.status != ir::IrOutboxMutationStatus::NotFound) {
              diagnostic = cleared.diagnostic.empty()
                               ? i18n::tr("settings.ir.ir_account_evidence_failed_cleared.message")
                               : cleared.diagnostic;
              return false;
            }
            if (!context.bokutachiCacheStore ||
                !context.bokutachiCacheStore->clearUserIds(diagnostic)) {
              if (diagnostic.empty()) {
                diagnostic =
                    i18n::tr("settings.ir.cached_bokutachi_identity_failed_invalidated.message");
              }
              return false;
            }
            if (context.irRankingService) {
              context.irRankingService->invalidate(
                  {.profileId = context.profileManager.activeProfile().id,
                   .providerId = std::string(providerId)});
            }
            context.irAccountEvidenceRevision.fetch_add(
                1, std::memory_order_release);
            return true;
          },
      .replaceCredential =
          [this](std::string_view apiKey, std::string &diagnostic) {
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            return context.replaceIrCredential(
                context.profileManager.activeProfile().id, kProviderId, apiKey,
                diagnostic);
          },
      .removeCredential =
          [this](std::string &diagnostic) {
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            return context.removeIrCredential(
                context.profileManager.activeProfile().id, kProviderId,
                diagnostic);
          },
      .credentialCommitted =
          [this]() {
            if (context.irSubmissionService) {
              context.irSubmissionService->notifyConfigurationChanged();
            }
          },
      .reactivateRemoteWork =
          [this](std::string &diagnostic) {
            if (!context.profileReady()) {
              diagnostic = i18n::tr("settings.ir.active_profile_unavailable.message");
              return false;
            }
            return context.activateIrProfileServices(
                context.profileManager.activeProfile().id, context.settings,
                diagnostic);
          },
      .retryAll =
          [this]() {
            if (!context.irSubmissionService) {
              return ir::IrOutboxMutationOutcome{
                  .status = ir::IrOutboxMutationStatus::StorageFailure,
                  .diagnostic = i18n::tr("settings.ir.submission_service_unavailable.message")};
            }
            return context.irSubmissionService->retryAll(kProviderId);
          },
      .discard =
          [this](std::int64_t rowId) {
            if (!context.irSubmissionService) {
              return ir::IrOutboxMutationOutcome{
                  .status = ir::IrOutboxMutationStatus::StorageFailure,
                  .diagnostic = i18n::tr("settings.ir.submission_service_unavailable.message")};
            }
            return context.irSubmissionService->discard(rowId);
          },
  };
  irSettingsModel = std::make_unique<ir::IrSettingsActionModel>(
      std::string(kProviderId), capabilities, providerSettings, hasCredential,
      std::move(dependencies));
  const auto reconciliationNow = std::chrono::steady_clock::now();
  const bool reconciliationCooldownActive =
      reconciliationStatus.nextAllowedAt &&
      reconciliationNow < *reconciliationStatus.nextAllowedAt;
  (void)irSettingsModel->observeReconciliationRevision(
      reconciliationStatus.revision);
  (void)irSettingsModel->observeReconciliationCooldown(
      reconciliationCooldownActive);

  const auto presentation = ir::makeIrSettingsPresentation(
      {.providerId = std::string(kProviderId),
       .displayName = "Bokutachi",
       .capabilities = capabilities,
       .settings = irSettingsModel->settings(),
       .hasCredential = irSettingsModel->hasCredential(),
       .serviceActive = serviceActive,
       .counts = counts,
       .reconciliationStatus = reconciliationStatus,
       .now = reconciliationNow});

  auto publishResult = [this](const ir::IrSettingsActionResult &result,
                              std::string successMessage) {
    irStatusIsError = !result.succeeded();
    irStatusMessage = result.succeeded()
                          ? std::move(successMessage)
                          : (result.diagnostic.empty()
                                 ? i18n::tr("settings.ir.ir_setting_failed_changed.message")
                                 : result.diagnostic);
    lastLayoutWidth = -1;
  };

  auto *settingsBody = new View();
  settingsBody->setFlexDirection(FlexDirection::Column);
  settingsBody->setGap(metrics.compact ? 12.0F : 16.0F);

  auto *enableRow = makeActionRow(metrics);
  enableRow->addView(makeWrappedText(i18n::message("settings.ir.provider.label"), metrics.bodyTextSize,
                                     ui_theme::textSecondary()));
  auto *enabledButton =
      makeIrButton(metrics, presentation.enabled ? i18n::message("settings.ir.enabled.label") : i18n::message("settings.ir.disabled.label"),
                   presentation.enabled ? ui_theme::lime() : ui_theme::coral());
  enabledButton->setOnClickListener([this, publishResult]() {
    if (!irSettingsModel) {
      return;
    }
    publishResult(
        irSettingsModel->setEnabled(!irSettingsModel->settings().enabled),
        i18n::tr("settings.ir.bokutachi_enablement_saved.message"));
  });
  enableRow->addView(enabledButton);
  settingsBody->addView(enableRow);

  if (presentation.showAutoSubmit) {
    auto *autoRow = makeActionRow(metrics);
    autoRow->addView(makeWrappedText(i18n::message("settings.ir.automatic_score_submission.label"),
                                     metrics.bodyTextSize,
                                     ui_theme::textSecondary()));
    auto *autoButton = makeIrButton(
        metrics, presentation.autoSubmit ? i18n::message("settings.ir.auto_submit_on.label") : i18n::message("settings.ir.auto_submit_off.label"),
        presentation.autoSubmit ? ui_theme::lime() : ui_theme::amber());
    autoButton->setEnabled(presentation.authenticatedActionsAvailable);
    autoButton->setOnClickListener([this, publishResult]() {
      if (!irSettingsModel) {
        return;
      }
      publishResult(irSettingsModel->setAutoSubmit(
                        !irSettingsModel->settings().autoSubmit),
                    i18n::tr("settings.ir.automatic_submission_preference_saved.message"));
    });
    autoRow->addView(autoButton);
    settingsBody->addView(autoRow);
  }

  settingsBody->addView(makeWrappedText(i18n::message("settings.ir.server_origin.label"), metrics.bodyTextSize,
                                        ui_theme::textSecondary()));
  auto *originRow = makeActionRow(metrics);
  irServerOriginInput = makeTextInput(
      metrics, std::max(280, metrics.cardsWidth - metrics.actionButtonWidth -
                                 metrics.cardPadding * 2 - 24));
  irServerOriginInput->setEditingText(presentation.serverOrigin);
  originRow->addView(irServerOriginInput);
  auto *saveOrigin = makeIrButton(metrics, i18n::message("settings.ir.save_origin.label"));
  saveOrigin->setOnClickListener([this, publishResult]() {
    if (!irSettingsModel || !irServerOriginInput) {
      return;
    }
    publishResult(
        irSettingsModel->setServerOrigin(irServerOriginInput->getText()),
        i18n::tr("settings.ir.server_origin_saved.message"));
  });
  originRow->addView(saveOrigin);
  settingsBody->addView(originRow);
  if (presentation.insecureServerOrigin) {
    settingsBody->addView(makeWrappedText(
        i18n::message("settings.ir.server_origin.http_restrictions_notice"),
        metrics.smallTextSize, ui_theme::coral()));
  }

  settingsBody->addView(
      makeWrappedText(presentation.credentialLabel, metrics.bodyTextSize,
                      presentation.hasCredential ? ui_theme::lime()
                                                 : ui_theme::textSecondary()));
  if (irKeyEditorActive) {
    auto *keyRow = makeActionRow(metrics);
    irApiKeyInput =
        makeTextInput(metrics, std::max(280, metrics.cardsWidth -
                                                 metrics.actionButtonWidth * 2 -
                                                 metrics.cardPadding * 2 - 36));
    irApiKeyInput->setEditingText("");
    keyRow->addView(irApiKeyInput);
    auto *saveKey = makeIrButton(metrics, i18n::message("settings.ir.save_key.label"), ui_theme::lime());
    saveKey->setEnabled(presentation.authenticatedActionsAvailable);
    saveKey->setOnClickListener([this, publishResult]() {
      if (!irSettingsModel || !irApiKeyInput) {
        return;
      }
      std::string apiKey = irApiKeyInput->getText();
      const auto result = irSettingsModel->replaceCredential(apiKey);
      std::fill(apiKey.begin(), apiKey.end(), '\0');
      if (result.succeeded()) {
        irKeyEditorActive = false;
      }
      publishResult(result, i18n::tr("settings.ir.api_key_saved_device.message"));
    });
    keyRow->addView(saveKey);
    auto *cancelKey = makeIrButton(metrics, i18n::message("settings.ir.cancel.label"), ui_theme::amber());
    cancelKey->setOnClickListener([this]() {
      if (irApiKeyInput) {
        std::string apiKey = irApiKeyInput->getText();
        std::fill(apiKey.begin(), apiKey.end(), '\0');
        irApiKeyInput->setEditingText("");
      }
      irKeyEditorActive = false;
      irStatusMessage = i18n::tr("settings.ir.api_key_edit_cancelled.message");
      irStatusIsError = false;
      lastLayoutWidth = -1;
    });
    keyRow->addView(cancelKey);
    settingsBody->addView(keyRow);
  } else {
    auto *keyActions = makeActionRow(metrics);
    auto *replaceKey = makeIrButton(
        metrics, presentation.hasCredential ? i18n::message("settings.ir.replace_key.label") : i18n::message("settings.ir.add_key.label"));
    replaceKey->setEnabled(presentation.authenticatedActionsAvailable);
    replaceKey->setOnClickListener([this]() {
      irKeyEditorActive = true;
      irStatusMessage.clear();
      lastLayoutWidth = -1;
    });
    keyActions->addView(replaceKey);
    if (presentation.hasCredential) {
      auto *removeKey = makeIrButton(metrics, i18n::message("settings.ir.remove_key.label"), ui_theme::coral());
      removeKey->setOnClickListener([this, publishResult]() {
        if (!irSettingsModel) {
          return;
        }
        publishResult(irSettingsModel->removeCredential(),
                      i18n::tr("settings.ir.api_key_removed_from_device.message"));
      });
      keyActions->addView(removeKey);
    }
    settingsBody->addView(keyActions);
  }

  settingsBody->addView(makeWrappedText(
      i18n::message("settings.ir.credentials.device_storage_notice"),
      metrics.smallTextSize, ui_theme::textMuted()));
  column->addView(makeCard(
      metrics, "Bokutachi",
      presentation.readOnly
          ? i18n::message("settings.ir.read_only_chart_ranking_provider.message")
          : i18n::message("settings.ir.direct_manual_chart_rankings_durable_score_submission.message"),
      settingsBody, metrics.compact ? 620 : 680, metrics.cardsWidth));

  if (presentation.showQueueActions) {
    auto *queueBody = new View();
    queueBody->setFlexDirection(FlexDirection::Column);
    queueBody->setGap(metrics.compact ? 10.0F : 14.0F);
    irPendingCountText =
        makeWrappedText("", metrics.bodyTextSize, ui_theme::textPrimary());
    irAwaitingCountText =
        makeWrappedText("", metrics.bodyTextSize, ui_theme::textPrimary());
    irBlockedCountText =
        makeWrappedText("", metrics.bodyTextSize, ui_theme::textPrimary());
    irFailedCountText =
        makeWrappedText("", metrics.bodyTextSize, ui_theme::textPrimary());
    queueBody->addView(irPendingCountText);
    queueBody->addView(irAwaitingCountText);
    queueBody->addView(irBlockedCountText);
    queueBody->addView(irFailedCountText);

    auto *retryAll = makeIrButton(metrics, i18n::message("settings.ir.retry_all_now.label"), ui_theme::lime());
    retryAll->setEnabled(presentation.canRetryAll);
    retryAll->setOnClickListener([this, publishResult]() {
      if (!irSettingsModel) {
        return;
      }
      publishResult(irSettingsModel->retryAll(),
                    i18n::tr("settings.ir.queued_submissions_scheduled_retry.message"));
    });
    queueBody->addView(retryAll);

    std::vector<ir::IrAttemptStatusSnapshot> snapshots;
    if (context.irSubmissionService) {
      snapshots = context.irSubmissionService->statuses(kProviderId);
    }
    std::size_t shown = 0;
    for (const auto &snapshot : snapshots) {
      if (!snapshot.found || !canDiscardStatus(snapshot.state) || shown >= 8) {
        continue;
      }
      ++shown;
      auto *row = makeActionRow(metrics);
      const auto label = i18n::message(
          "settings.ir.outbox.entry",
          {{"state", queueStateLabel(snapshot.state)},
           {"id", std::to_string(snapshot.rowId)},
           {"diagnostic", snapshot.diagnostic.empty()
                              ? std::string{} : " · " + snapshot.diagnostic}});
      row->addView(makeWrappedText(label, metrics.smallTextSize,
                                   ui_theme::textSecondary()));
      if (irPendingDiscardRowId == snapshot.rowId) {
        auto *confirm =
            makeIrButton(metrics, i18n::message("settings.ir.confirm_discard.label"), ui_theme::coral());
        confirm->setOnClickListener(
            [this, publishResult, rowId = snapshot.rowId]() {
              if (!irSettingsModel) {
                return;
              }
              const auto result = irSettingsModel->discard(rowId);
              if (result.succeeded()) {
                irPendingDiscardRowId.reset();
              }
              publishResult(result, i18n::tr("settings.ir.queued_submission_discarded.message"));
            });
        row->addView(confirm);
        auto *cancel = makeIrButton(metrics, i18n::message("settings.ir.cancel.label"), ui_theme::amber());
        cancel->setOnClickListener([this]() {
          irPendingDiscardRowId.reset();
          lastLayoutWidth = -1;
        });
        row->addView(cancel);
      } else {
        auto *discard = makeIrButton(metrics, i18n::message("settings.ir.discard.label"), ui_theme::coral());
        discard->setOnClickListener([this, rowId = snapshot.rowId]() {
          irPendingDiscardRowId = rowId;
          irStatusMessage = i18n::tr("settings.ir.confirm_permanent_removal_queued_score.message");
          irStatusIsError = true;
          lastLayoutWidth = -1;
        });
        row->addView(discard);
      }
      queueBody->addView(row);
    }
    if (shown == 0) {
      queueBody->addView(
          makeWrappedText(i18n::message("settings.ir.no_discardable_submissions_currently_queued.message"),
                          metrics.smallTextSize, ui_theme::textMuted()));
    }
    column->addView(makeCard(
        metrics, i18n::message("settings.ir.submission_queue.label"),
        i18n::message("settings.ir.scores_remain_durable_across_offline_sessions_app_restarts.message"),
        queueBody, metrics.compact ? 360 : 420, metrics.cardsWidth));
  }

  if (presentation.showRecordSync) {
    auto *syncBody = new View();
    syncBody->setFlexDirection(FlexDirection::Column);
    syncBody->setGap(metrics.compact ? 10.0F : 14.0F);
    syncBody->addView(makeWrappedText(presentation.recordSyncHelperText,
                                      metrics.smallTextSize,
                                      ui_theme::textSecondary()));
    syncBody->addView(makeWrappedText(
        presentation.recordSyncStatusText, metrics.bodyTextSize,
        presentation.recordSyncStatusIsError ? ui_theme::coral()
                                             : ui_theme::textPrimary()));
    if (!presentation.recordSyncCooldownText.empty()) {
      syncBody->addView(makeWrappedText(presentation.recordSyncCooldownText,
                                        metrics.smallTextSize,
                                        ui_theme::amber()));
    }
    auto *syncRecords = makeIrButton(
        metrics, presentation.recordSyncButtonLabel, ui_theme::lime());
    syncRecords->setEnabled(presentation.canSyncRecords);
    syncRecords->setOnClickListener([this]() {
      if (!context.irSubmissionService) {
        return;
      }
      (void)context.irSubmissionService->requestUserScoreReconciliation(
          kProviderId);
      lastLayoutWidth = -1;
    });
    syncBody->addView(syncRecords);
    column->addView(makeCard(
        metrics, i18n::message("settings.ir.ir_record_import.label"),
        i18n::message("settings.ir.remote_history.description"),
        syncBody, metrics.compact ? 240 : 280, metrics.cardsWidth));
  }

  if (!irStatusMessage.empty()) {
    irStatusText =
        makeWrappedText(irStatusMessage, metrics.bodyTextSize,
                        irStatusIsError ? ui_theme::coral() : ui_theme::lime());
    auto *statusBody = new View();
    statusBody->addView(irStatusText);
    column->addView(makeCard(metrics, i18n::message("settings.ir.ir_status.label"), "", statusBody, 120,
                             metrics.cardsWidth));
  }

  refreshIrSettingsPresentation();
  return column;
}

void SettingsScene::refreshIrSettingsPresentation() {
  if (!context.irSubmissionService) {
    return;
  }
  const ir::IrOutboxCounts counts =
      context.irSubmissionService->counts(kProviderId);
  if (irPendingCountText) {
    irPendingCountText->setLocalizedText(i18n::message(
        "settings.ir.outbox.count",
        {{"label", i18n::message("settings.ir.pending.prefix")},
         {"count", std::to_string(counts.pending)}}));
  }
  if (irAwaitingCountText) {
    irAwaitingCountText->setLocalizedText(i18n::message(
        "settings.ir.outbox.count",
        {{"label", i18n::message("settings.ir.awaiting_remote_result.prefix")},
         {"count", std::to_string(counts.awaitingRemoteResult)}}));
  }
  if (irBlockedCountText) {
    irBlockedCountText->setLocalizedText(i18n::message(
        "settings.ir.outbox.count",
        {{"label", i18n::message("settings.ir.blocked_by_configuration.prefix")},
         {"count", std::to_string(counts.blockedConfiguration)}}));
  }
  if (irFailedCountText) {
    irFailedCountText->setLocalizedText(i18n::message(
        "settings.ir.outbox.count",
        {{"label", i18n::message("settings.ir.failed.prefix")},
         {"count", std::to_string(counts.failedPermanent)}}));
  }
  const auto status =
      context.irSubmissionService->reconciliationStatus(kProviderId);
  if (!irSettingsModel) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const bool cooldownActive =
      status.nextAllowedAt && now < *status.nextAllowedAt;
  const bool revisionChanged =
      irSettingsModel->observeReconciliationRevision(status.revision);
  const bool cooldownChanged =
      irSettingsModel->observeReconciliationCooldown(cooldownActive);
  if (revisionChanged || cooldownChanged) {
    lastLayoutWidth = -1;
  }
}
