#include "ir/IrSettingsPresentation.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

ir::IrOutboxMutationOutcome mutation(bool succeeds) {
  return {.status = succeeds ? ir::IrOutboxMutationStatus::Updated
                             : ir::IrOutboxMutationStatus::StorageFailure,
          .affectedRows = succeeds ? 2U : 0U,
          .diagnostic = succeeds ? std::string{} : "storage unavailable"};
}

void testBokutachiPresentationExposesWriteControlsWithoutSecrets() {
  ir::IrOutboxCounts counts;
  counts.storageAvailable = true;
  counts.pending = 2;
  counts.awaitingRemoteResult = 1;
  counts.failedPermanent = 3;
  counts.total = 6;
  const auto presentation = ir::makeIrSettingsPresentation(
      {.providerId = "tachi",
       .displayName = "Bokutachi",
       .capabilities = {.chartRankings = true,
                        .scoreSubmission = true,
                        .deferredSubmission = true},
       .settings = {.enabled = true,
                    .autoSubmit = true,
                    .serverOrigin = "http://local.example"},
       .hasCredential = true,
       .counts = counts});

  REQUIRE(presentation.providerId == "tachi");
  REQUIRE(presentation.displayName == "Bokutachi");
  REQUIRE(presentation.enabled);
  REQUIRE(presentation.autoSubmit);
  REQUIRE(presentation.hasCredential);
  REQUIRE(presentation.showAutoSubmit);
  REQUIRE(presentation.showQueueActions);
  REQUIRE(presentation.canRetryAll);
  REQUIRE(presentation.canDiscard);
  REQUIRE(presentation.insecureServerOrigin);
  REQUIRE(presentation.credentialLabel == "API key saved (••••••••)");
  REQUIRE(presentation.counts.failedPermanent == 3);

  counts.failedPermanent = 0;
  const auto pendingOnly = ir::makeIrSettingsPresentation(
      {.providerId = "tachi",
       .displayName = "Bokutachi",
       .capabilities = {.chartRankings = true,
                        .scoreSubmission = true,
                        .deferredSubmission = true},
       .settings = {},
       .counts = counts});
  REQUIRE(pendingOnly.canRetryAll);
}

void testReadOnlyPresentationHidesSubmissionControls() {
  const auto presentation = ir::makeIrSettingsPresentation(
      {.providerId = "lr2ir",
       .displayName = "LR2IR Archive",
       .capabilities = {.readOnly = true, .chartRankings = true},
       .settings = {.enabled = true,
                    .autoSubmit = true,
                    .serverOrigin = "https://archive.example"},
       .hasCredential = false});

  REQUIRE(presentation.enabled);
  REQUIRE(!presentation.autoSubmit);
  REQUIRE(!presentation.showAutoSubmit);
  REQUIRE(!presentation.showQueueActions);
  REQUIRE(!presentation.canRetryAll);
  REQUIRE(!presentation.canDiscard);
  REQUIRE(presentation.credentialLabel == "No API key saved");
}

void testRecordSyncRequiresCompleteActiveConfiguration() {
  ir::IrSettingsPresentationInput input{
      .providerId = "tachi",
      .displayName = "Bokutachi",
      .capabilities = {.scoreReconciliation = true},
      .settings = {.enabled = true, .serverOrigin = "https://boku.tachi.ac"},
      .hasCredential = true,
      .serviceActive = true,
  };

  const auto available = ir::makeIrSettingsPresentation(input);
  REQUIRE(available.showRecordSync);
  REQUIRE(available.canSyncRecords);
  REQUIRE(available.recordSyncButtonLabel == "Import & Reconcile");
  REQUIRE(available.recordSyncHelperText ==
          "Uses exactly two requests (7K, then 14K) to import remote score "
          "history and reconcile existing upload receipts. Local scores are "
          "not uploaded.");

  input.capabilities.scoreReconciliation = false;
  const auto unsupported = ir::makeIrSettingsPresentation(input);
  REQUIRE(!unsupported.showRecordSync);
  REQUIRE(!unsupported.canSyncRecords);

  input.capabilities.scoreReconciliation = true;
  input.settings.enabled = false;
  const auto disabled = ir::makeIrSettingsPresentation(input);
  REQUIRE(!disabled.showRecordSync);
  REQUIRE(!disabled.canSyncRecords);

  input.settings.enabled = true;
  input.hasCredential = false;
  const auto uncredentialed = ir::makeIrSettingsPresentation(input);
  REQUIRE(!uncredentialed.showRecordSync);
  REQUIRE(!uncredentialed.canSyncRecords);

  input.hasCredential = true;
  input.serviceActive = false;
  const auto inactive = ir::makeIrSettingsPresentation(input);
  REQUIRE(!inactive.showRecordSync);
  REQUIRE(!inactive.canSyncRecords);
}

ir::IrSettingsPresentationInput recordSyncInput(
    ir::IrReconciliationPhase phase,
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::time_point{std::chrono::seconds{100}}) {
  return {
      .providerId = "tachi",
      .displayName = "Bokutachi",
      .capabilities = {.scoreReconciliation = true},
      .settings = {.enabled = true, .serverOrigin = "https://boku.tachi.ac"},
      .hasCredential = true,
      .serviceActive = true,
      .reconciliationStatus = {.revision = 7, .phase = phase},
      .now = now,
  };
}

void testRecordSyncProjectsEveryPhaseAndBoundedMutationSummary() {
  const auto idle = ir::makeIrSettingsPresentation(
      recordSyncInput(ir::IrReconciliationPhase::Idle));
  REQUIRE(idle.canSyncRecords);
  REQUIRE(idle.recordSyncStatusText ==
          "Ready to import remote records and reconcile upload receipts.");

  const auto queued = ir::makeIrSettingsPresentation(
      recordSyncInput(ir::IrReconciliationPhase::Queued));
  REQUIRE(!queued.canSyncRecords);
  REQUIRE(queued.recordSyncStatusText ==
          "Sync queued. Waiting for active uploads to finish.");

  const auto fetching7K = ir::makeIrSettingsPresentation(
      recordSyncInput(ir::IrReconciliationPhase::Fetching7K));
  REQUIRE(!fetching7K.canSyncRecords);
  REQUIRE(fetching7K.recordSyncStatusText ==
          "Request 1 of 2: fetching 7K records.");

  const auto fetching14K = ir::makeIrSettingsPresentation(
      recordSyncInput(ir::IrReconciliationPhase::Fetching14K));
  REQUIRE(!fetching14K.canSyncRecords);
  REQUIRE(fetching14K.recordSyncStatusText ==
          "Request 2 of 2: fetching 14K records.");

  const auto applying = ir::makeIrSettingsPresentation(
      recordSyncInput(ir::IrReconciliationPhase::Applying));
  REQUIRE(!applying.canSyncRecords);
  REQUIRE(applying.recordSyncStatusText ==
          "Both requests validated. Applying records and receipts atomically.");

  auto succeededInput = recordSyncInput(ir::IrReconciliationPhase::Succeeded);
  succeededInput.reconciliationStatus.remoteScores = 20;
  succeededInput.reconciliationStatus.remoteScoresAdded = 3;
  succeededInput.reconciliationStatus.remoteScoresRemoved = 2;
  succeededInput.reconciliationStatus.receiptsUpserted = 4;
  succeededInput.reconciliationStatus.receiptsDeleted = 1;
  succeededInput.reconciliationStatus.ambiguousReceiptsPreserved = 5;
  succeededInput.reconciliationStatus.outboxRowsSettled = 6;
  succeededInput.reconciliationStatus.nextAllowedAt =
      succeededInput.now + std::chrono::seconds{42};
  const auto succeeded = ir::makeIrSettingsPresentation(succeededInput);
  REQUIRE(!succeeded.canSyncRecords);
  REQUIRE(succeeded.recordSyncStatusText ==
          "Sync complete. Records: 20 total, 3 added, 2 removed. Receipts: 4 "
          "confirmed, 1 removed, 5 ambiguous. Settled outbox rows: 6.");
  REQUIRE(succeeded.recordSyncCooldownText == "Available again in 42 seconds.");

  auto failedInput = recordSyncInput(ir::IrReconciliationPhase::Failed);
  failedInput.reconciliationStatus.diagnostic = std::string(2048, 'x');
  const auto failed = ir::makeIrSettingsPresentation(failedInput);
  REQUIRE(failed.recordSyncStatusIsError);
  REQUIRE(failed.recordSyncStatusText.starts_with("Sync failed: "));
  REQUIRE(failed.recordSyncStatusText.ends_with(
      " Existing records and receipts were left unchanged."));
  REQUIRE(failed.recordSyncStatusText.size() <=
          ir::kMaximumRecordSyncStatusBytes);
  REQUIRE(failed.recordSyncStatusText.find("private-api-key") ==
          std::string::npos);

  auto cooldownInput = recordSyncInput(ir::IrReconciliationPhase::Cooldown);
  cooldownInput.reconciliationStatus.nextAllowedAt =
      cooldownInput.now + std::chrono::milliseconds{1501};
  const auto cooldown = ir::makeIrSettingsPresentation(cooldownInput);
  REQUIRE(!cooldown.canSyncRecords);
  REQUIRE(cooldown.recordSyncStatusText == "Sync cooldown is active.");
  REQUIRE(cooldown.recordSyncCooldownText == "Available again in 2 seconds.");

  cooldownInput.now = *cooldownInput.reconciliationStatus.nextAllowedAt;
  const auto cooldownExpired = ir::makeIrSettingsPresentation(cooldownInput);
  REQUIRE(cooldownExpired.canSyncRecords);
  REQUIRE(cooldownExpired.recordSyncStatusText ==
          "Sync cooldown complete. Record sync is available.");
  REQUIRE(cooldownExpired.recordSyncCooldownText.empty());
}

struct FakeActions {
  ir::IrProviderSettings stored;
  ir::IrProviderSettings active;
  bool credentialPresent = false;
  bool settingsStoreSucceeds = true;
  bool quiesceSucceeds = true;
  bool invalidationSucceeds = true;
  bool credentialStoreSucceeds = true;
  bool reactivationSucceeds = true;
  bool mutationSucceeds = true;
  int settingsStores = 0;
  int settingsPublishes = 0;
  int quiesceCalls = 0;
  int invalidationCalls = 0;
  int replaceCredentialCalls = 0;
  int removeCredentialCalls = 0;
  int credentialPublishes = 0;
  int reactivationCalls = 0;
  int retryCalls = 0;
  int discardCalls = 0;
  std::vector<std::string> credentialActionOrder;

  ir::IrSettingsActionDependencies dependencies() {
    return {
        .storeSettings =
            [this](const ir::IrProviderSettings &candidate,
                   std::string &diagnostic) {
              ++settingsStores;
              if (!settingsStoreSucceeds) {
                diagnostic = "settings save failed";
                return false;
              }
              stored = candidate;
              return true;
            },
        .settingsCommitted =
            [this](const ir::IrProviderSettings &candidate) {
              ++settingsPublishes;
              active = candidate;
            },
        .quiesceRemoteWork =
            [this](std::string &diagnostic) {
              ++quiesceCalls;
              credentialActionOrder.emplace_back("quiesce");
              if (!quiesceSucceeds) {
                diagnostic = "quiesce failed";
                return false;
              }
              return true;
            },
        .invalidateProviderIdentity =
            [this](std::string_view providerId, std::string &diagnostic) {
              ++invalidationCalls;
              credentialActionOrder.push_back("invalidate:" +
                                              std::string(providerId));
              if (!invalidationSucceeds) {
                diagnostic = "identity invalidation failed";
                return false;
              }
              return true;
            },
        .replaceCredential =
            [this](std::string_view key, std::string &diagnostic) {
              ++replaceCredentialCalls;
              credentialActionOrder.emplace_back("replace");
              if (!credentialStoreSucceeds) {
                diagnostic = "credential save failed";
                return false;
              }
              credentialPresent = !key.empty();
              return true;
            },
        .removeCredential =
            [this](std::string &diagnostic) {
              ++removeCredentialCalls;
              credentialActionOrder.emplace_back("remove");
              if (!credentialStoreSucceeds) {
                diagnostic = "credential remove failed";
                return false;
              }
              credentialPresent = false;
              return true;
            },
        .credentialCommitted =
            [this]() {
              ++credentialPublishes;
              credentialActionOrder.emplace_back("committed");
            },
        .reactivateRemoteWork =
            [this](std::string &diagnostic) {
              ++reactivationCalls;
              credentialActionOrder.emplace_back("reactivate");
              if (!reactivationSucceeds) {
                diagnostic = "reactivation failed";
                return false;
              }
              return true;
            },
        .retryAll =
            [this]() {
              ++retryCalls;
              return mutation(mutationSucceeds);
            },
        .discard =
            [this](std::int64_t) {
              ++discardCalls;
              return mutation(mutationSucceeds);
            },
    };
  }
};

ir::IrProviderSettings initialSettings() {
  return {.enabled = false,
          .autoSubmit = false,
          .serverOrigin = "https://boku.tachi.ac"};
}

void testSettingsActionsPublishOnlyAfterDurableStore() {
  FakeActions fake;
  fake.stored = initialSettings();
  fake.active = initialSettings();
  ir::IrSettingsActionModel model("tachi",
                                  {.chartRankings = true,
                                   .scoreSubmission = true,
                                   .deferredSubmission = true},
                                  initialSettings(), false,
                                  fake.dependencies());

  REQUIRE(model.setEnabled(true).succeeded());
  REQUIRE(model.settings().enabled);
  REQUIRE(fake.stored.enabled);
  REQUIRE(fake.active.enabled);
  REQUIRE(fake.settingsPublishes == 1);

  fake.settingsStoreSucceeds = false;
  REQUIRE(!model.setAutoSubmit(true).succeeded());
  REQUIRE(!model.settings().autoSubmit);
  REQUIRE(!fake.stored.autoSubmit);
  REQUIRE(!fake.active.autoSubmit);
  REQUIRE(fake.settingsPublishes == 1);

  REQUIRE(!model.setServerOrigin("https://other.example").succeeded());
  REQUIRE(model.settings().serverOrigin == "https://boku.tachi.ac");
  REQUIRE(fake.active.serverOrigin == "https://boku.tachi.ac");

  fake.settingsStoreSucceeds = true;
  REQUIRE(model.setAutoSubmit(true).succeeded());
  REQUIRE(model.setServerOrigin("HTTPS://Other.Example/").succeeded());
  REQUIRE(model.settings().serverOrigin == "https://other.example");
  REQUIRE(fake.active == model.settings());
}

void testCredentialActionsNeverRetainKeyAndPublishAfterStore() {
  FakeActions fake;
  ir::IrSettingsActionModel model("tachi",
                                  {.chartRankings = true,
                                   .scoreSubmission = true,
                                   .deferredSubmission = true},
                                  initialSettings(), false,
                                  fake.dependencies());
  const std::string secret = "private-api-key-should-not-be-retained";

  fake.quiesceSucceeds = false;
  const auto failedQuiesce = model.replaceCredential(secret);
  REQUIRE(!failedQuiesce.succeeded());
  REQUIRE(!model.hasCredential());
  REQUIRE(fake.invalidationCalls == 0);
  REQUIRE(fake.replaceCredentialCalls == 0);
  REQUIRE(fake.reactivationCalls == 1);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "reactivate"}));

  fake.quiesceSucceeds = true;
  fake.credentialActionOrder.clear();
  fake.invalidationSucceeds = false;
  const auto failedInvalidation = model.replaceCredential(secret);
  REQUIRE(!failedInvalidation.succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.invalidationCalls == 1);
  REQUIRE(fake.replaceCredentialCalls == 1);
  REQUIRE(fake.credentialPublishes == 0);
  REQUIRE(fake.reactivationCalls == 2);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "replace", "invalidate:tachi",
                                    "reactivate"}));
  REQUIRE(failedInvalidation.diagnostic.find(secret) == std::string::npos);

  fake.invalidationSucceeds = true;
  fake.credentialActionOrder.clear();
  fake.credentialStoreSucceeds = false;
  const auto failedReplace = model.replaceCredential(secret);
  REQUIRE(!failedReplace.succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.invalidationCalls == 1);
  REQUIRE(fake.replaceCredentialCalls == 2);
  REQUIRE(fake.credentialPublishes == 0);
  REQUIRE(fake.reactivationCalls == 3);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "replace", "reactivate"}));
  REQUIRE(failedReplace.diagnostic.find(secret) == std::string::npos);

  fake.credentialActionOrder.clear();
  fake.credentialStoreSucceeds = true;
  REQUIRE(model.replaceCredential(secret).succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 1);
  REQUIRE(fake.reactivationCalls == 4);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "replace", "invalidate:tachi",
                                    "committed", "reactivate"}));

  fake.credentialActionOrder.clear();
  fake.invalidationSucceeds = false;
  REQUIRE(!model.removeCredential().succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.removeCredentialCalls == 0);
  REQUIRE(fake.credentialPublishes == 1);
  REQUIRE(fake.reactivationCalls == 5);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{
               "quiesce", "invalidate:tachi",
               "reactivate"}));

  fake.credentialActionOrder.clear();
  fake.invalidationSucceeds = true;
  fake.credentialStoreSucceeds = false;
  REQUIRE(!model.removeCredential().succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.removeCredentialCalls == 1);
  REQUIRE(fake.credentialPublishes == 1);
  REQUIRE(fake.reactivationCalls == 6);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{
               "quiesce", "invalidate:tachi", "remove",
               "reactivate"}));

  fake.credentialActionOrder.clear();
  fake.credentialStoreSucceeds = true;
  REQUIRE(model.removeCredential().succeeded());
  REQUIRE(!model.hasCredential());
  REQUIRE(!fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 2);
  REQUIRE(fake.reactivationCalls == 7);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{
               "quiesce", "invalidate:tachi", "remove",
               "committed", "reactivate"}));

  fake.credentialActionOrder.clear();
  fake.reactivationSucceeds = false;
  const auto failedReactivation = model.replaceCredential(secret);
  REQUIRE(!failedReactivation.succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 3);
  REQUIRE(fake.reactivationCalls == 8);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "replace", "invalidate:tachi",
                                    "committed", "reactivate"}));
}

void testFailedCredentialReplacementPreservesExistingAccountEvidence() {
  FakeActions fake;
  fake.credentialPresent = true;
  fake.credentialStoreSucceeds = false;
  ir::IrSettingsActionModel model("tachi",
                                  {.chartRankings = true,
                                   .scoreSubmission = true,
                                   .deferredSubmission = true},
                                  initialSettings(), true, fake.dependencies());

  const auto result = model.replaceCredential("replacement-api-key");

  REQUIRE(!result.succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.replaceCredentialCalls == 1);
  REQUIRE(fake.invalidationCalls == 0);
  REQUIRE(fake.credentialPublishes == 0);
  REQUIRE(fake.reactivationCalls == 1);
  REQUIRE((fake.credentialActionOrder ==
           std::vector<std::string>{"quiesce", "replace", "reactivate"}));
}

void testStoreInvalidCredentialFormatsHaveNoSideEffects() {
  const std::vector<std::string> invalidKeys{
      {},
      " leading",
      "trailing ",
      "line\nbreak",
      std::string("key") + static_cast<char>(0x01) + "value",
      std::string("key") + static_cast<char>(0x7f) + "value",
      std::string(4U * 1024U + 1U, 'x'),
  };

  for (const auto &apiKey : invalidKeys) {
    FakeActions fake;
    fake.credentialPresent = true;
    ir::IrSettingsActionModel model(
        "tachi",
        {.chartRankings = true,
         .scoreSubmission = true,
         .deferredSubmission = true},
        initialSettings(), true, fake.dependencies());

    const auto result = model.replaceCredential(apiKey);

    REQUIRE(result.status == ir::IrSettingsActionResult::Status::Invalid);
    REQUIRE(model.hasCredential());
    REQUIRE(fake.credentialPresent);
    REQUIRE(fake.settingsStores == 0);
    REQUIRE(fake.settingsPublishes == 0);
    REQUIRE(fake.quiesceCalls == 0);
    REQUIRE(fake.invalidationCalls == 0);
    REQUIRE(fake.replaceCredentialCalls == 0);
    REQUIRE(fake.removeCredentialCalls == 0);
    REQUIRE(fake.credentialPublishes == 0);
    REQUIRE(fake.reactivationCalls == 0);
    REQUIRE(fake.credentialActionOrder.empty());
    REQUIRE(apiKey.empty() || result.diagnostic.find(apiKey) ==
                                  std::string::npos);
  }
}

void testQueueActionsAreCapabilityGatedAndFailureSafe() {
  FakeActions writable;
  ir::IrSettingsActionModel writableModel("tachi",
                                          {.chartRankings = true,
                                           .scoreSubmission = true,
                                           .deferredSubmission = true},
                                          initialSettings(), true,
                                          writable.dependencies());
  writable.mutationSucceeds = false;
  REQUIRE(!writableModel.retryAll().succeeded());
  REQUIRE(!writableModel.discard(42).succeeded());
  REQUIRE(writable.retryCalls == 1);
  REQUIRE(writable.discardCalls == 1);

  FakeActions readOnly;
  ir::IrSettingsActionModel readOnlyModel(
      "lr2ir", {.readOnly = true, .chartRankings = true}, initialSettings(),
      false, readOnly.dependencies());
  REQUIRE(!readOnlyModel.setAutoSubmit(true).succeeded());
  REQUIRE(!readOnlyModel.retryAll().succeeded());
  REQUIRE(!readOnlyModel.discard(42).succeeded());
  REQUIRE(readOnly.settingsStores == 0);
  REQUIRE(readOnly.retryCalls == 0);
  REQUIRE(readOnly.discardCalls == 0);
}

void testActionModelObservesReconciliationRevisionAndCooldownChanges() {
  FakeActions fake;
  ir::IrSettingsActionModel model("tachi", {.scoreReconciliation = true},
                                  initialSettings(), true, fake.dependencies());

  REQUIRE(model.observeReconciliationRevision(12));
  REQUIRE(!model.observeReconciliationRevision(12));
  REQUIRE(model.observeReconciliationRevision(13));

  REQUIRE(model.observeReconciliationCooldown(true));
  REQUIRE(!model.observeReconciliationCooldown(true));
  REQUIRE(model.observeReconciliationCooldown(false));
  REQUIRE(!model.observeReconciliationCooldown(false));
}

} // namespace

int main() {
  testBokutachiPresentationExposesWriteControlsWithoutSecrets();
  testReadOnlyPresentationHidesSubmissionControls();
  testRecordSyncRequiresCompleteActiveConfiguration();
  testRecordSyncProjectsEveryPhaseAndBoundedMutationSummary();
  testSettingsActionsPublishOnlyAfterDurableStore();
  testCredentialActionsNeverRetainKeyAndPublishAfterStore();
  testFailedCredentialReplacementPreservesExistingAccountEvidence();
  testStoreInvalidCredentialFormatsHaveNoSideEffects();
  testQueueActionsAreCapabilityGatedAndFailureSafe();
  testActionModelObservesReconciliationRevisionAndCooldownChanges();
  return 0;
}
