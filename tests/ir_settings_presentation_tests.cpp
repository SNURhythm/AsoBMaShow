#include "ir/IrSettingsPresentation.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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

struct FakeActions {
  ir::IrProviderSettings stored;
  ir::IrProviderSettings active;
  bool credentialPresent = false;
  bool settingsStoreSucceeds = true;
  bool credentialStoreSucceeds = true;
  bool mutationSucceeds = true;
  int settingsStores = 0;
  int settingsPublishes = 0;
  int credentialPublishes = 0;
  int retryCalls = 0;
  int discardCalls = 0;

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
        .replaceCredential =
            [this](std::string_view key, std::string &diagnostic) {
              if (!credentialStoreSucceeds) {
                diagnostic = "credential save failed";
                return false;
              }
              credentialPresent = !key.empty();
              return true;
            },
        .removeCredential =
            [this](std::string &diagnostic) {
              if (!credentialStoreSucceeds) {
                diagnostic = "credential remove failed";
                return false;
              }
              credentialPresent = false;
              return true;
            },
        .credentialCommitted = [this]() { ++credentialPublishes; },
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

  fake.credentialStoreSucceeds = false;
  const auto failedReplace = model.replaceCredential(secret);
  REQUIRE(!failedReplace.succeeded());
  REQUIRE(!model.hasCredential());
  REQUIRE(!fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 0);
  REQUIRE(failedReplace.diagnostic.find(secret) == std::string::npos);

  fake.credentialStoreSucceeds = true;
  REQUIRE(model.replaceCredential(secret).succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 1);

  fake.credentialStoreSucceeds = false;
  REQUIRE(!model.removeCredential().succeeded());
  REQUIRE(model.hasCredential());
  REQUIRE(fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 1);

  fake.credentialStoreSucceeds = true;
  REQUIRE(model.removeCredential().succeeded());
  REQUIRE(!model.hasCredential());
  REQUIRE(!fake.credentialPresent);
  REQUIRE(fake.credentialPublishes == 2);
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

} // namespace

int main() {
  testBokutachiPresentationExposesWriteControlsWithoutSecrets();
  testReadOnlyPresentationHidesSubmissionControls();
  testSettingsActionsPublishOnlyAfterDurableStore();
  testCredentialActionsNeverRetainKeyAndPublishAfterStore();
  testQueueActionsAreCapabilityGatedAndFailureSafe();
  return 0;
}
