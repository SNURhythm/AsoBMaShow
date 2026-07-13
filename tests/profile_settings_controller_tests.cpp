#include "scene/ProfileSettingsController.h"
#include "scene/ProfileRuntimeReapply.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
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

PlayerProfile profile(std::string id, std::string name,
                      std::string lastUsed = "2026-07-10T00:00:00Z") {
  return {.id = std::move(id),
          .displayName = std::move(name),
          .createdAt = "2026-07-01T00:00:00Z",
          .lastUsedAt = std::move(lastUsed)};
}

ProfileResult profileSuccess(PlayerProfile value, std::string message = {}) {
  return {.error = ProfileError::None,
          .message = std::move(message),
          .profile = std::move(value)};
}

ProfileArchiveResult archiveSuccess(PlayerProfile value,
                                    std::string message = {}) {
  return {.error = ProfileError::None,
          .message = std::move(message),
          .profile = std::move(value)};
}

struct FakeServices {
  std::vector<PlayerProfile> profiles{
      profile("alpha", "Alpha", "2026-07-10T00:00:00Z"),
      profile("bravo", "Bravo", "2026-07-09T00:00:00Z")};
  std::string activeId = "alpha";
  ProfileError listError = ProfileError::None;
  std::string listMessage;
  int createCalls = 0;
  int renameCalls = 0;
  int duplicateCalls = 0;
  int deleteCalls = 0;
  int activateCalls = 0;
  int exportCalls = 0;
  int importCalls = 0;
  int listCalls = 0;
  int settingsFlushes = 0;
  int inputFlushes = 0;
  bool settingsFlushSucceeds = true;
  bool inputFlushSucceeds = true;
  bool failActivate = false;
  bool failActivateAfterCommit = false;
  bool failDelete = false;
  bool failImport = false;
  std::string throwAfterMutation;
  bool throwSettingsNonStd = false;
  bool throwInputNonStd = false;
  bool archivePipelineActive = false;
  bool archivePipelineStarts = true;
  int archivePipelineBeginCalls = 0;
  int archivePipelineEndCalls = 0;
  std::string exportSuccessMessage;
  std::string importSuccessMessage;

  PlayerProfile *find(std::string_view id) {
    for (auto &candidate : profiles) {
      if (candidate.id == id) {
        return &candidate;
      }
    }
    return nullptr;
  }

  ProfileSettingsControllerDependencies dependencies() {
    return {
        .listProfiles =
            [this]() {
              ++listCalls;
              return ProfileListResult{.error = listError,
                                       .message = listMessage,
                                       .profiles = profiles,
                                       .activeProfileId = activeId};
            },
        .create =
            [this](std::string name) {
              ++createCalls;
              PlayerProfile created = profile("charlie", std::move(name));
              profiles.push_back(created);
              if (throwAfterMutation == "create") {
                throw std::runtime_error("create callback failed after commit");
              }
              return profileSuccess(std::move(created));
            },
        .rename =
            [this](std::string_view id, std::string name) {
              ++renameCalls;
              if (auto *existing = find(id)) {
                existing->displayName = std::move(name);
                if (throwAfterMutation == "rename") {
                  throw std::runtime_error(
                      "rename callback failed after commit");
                }
                return profileSuccess(*existing);
              }
              return ProfileResult{.error = ProfileError::NotFound,
                                   .message = "profile disappeared"};
            },
        .duplicate =
            [this](std::string_view, std::string name) {
              ++duplicateCalls;
              PlayerProfile duplicated = profile("delta", std::move(name));
              profiles.push_back(duplicated);
              if (throwAfterMutation == "duplicate") {
                throw std::runtime_error(
                    "duplicate callback failed after commit");
              }
              return profileSuccess(std::move(duplicated));
            },
        .remove =
            [this](std::string_view id) {
              ++deleteCalls;
              if (failDelete) {
                return ProfileResult{.error = ProfileError::IoFailure,
                                     .message = "delete failed"};
              }
              const auto before = profiles.size();
              PlayerProfile removed;
              for (const auto &candidate : profiles) {
                if (candidate.id == id) {
                  removed = candidate;
                }
              }
              std::erase_if(profiles, [id](const PlayerProfile &candidate) {
                return candidate.id == id;
              });
              if (throwAfterMutation == "remove") {
                throw std::runtime_error("remove callback failed after commit");
              }
              return profiles.size() == before
                         ? ProfileResult{.error = ProfileError::NotFound,
                                         .message = "profile disappeared"}
                         : profileSuccess(std::move(removed));
            },
        .activate =
            [this](std::string_view id) {
              ++activateCalls;
              if (failActivate) {
                return ProfileSwitchResult{.error = ProfileError::IoFailure,
                                           .message = "switch failed"};
              }
              activeId = std::string(id);
              if (auto *existing = find(id)) {
                existing->lastUsedAt = "2026-07-11T00:00:00Z";
              }
              if (failActivateAfterCommit) {
                return ProfileSwitchResult{
                    .error = ProfileError::IoFailure,
                    .message = "switch reported failure after commit"};
              }
              if (throwAfterMutation == "activate") {
                throw std::runtime_error(
                    "activate callback failed after commit");
              }
              return ProfileSwitchResult{};
            },
        .exportProfile =
            [this](std::string_view id, const std::filesystem::path &) {
              ++exportCalls;
              const auto *existing = find(id);
              if (existing == nullptr) {
                return ProfileArchiveResult{.error = ProfileError::NotFound,
                                            .message = "profile disappeared"};
              }
              return archiveSuccess(*existing, exportSuccessMessage);
            },
        .importProfile =
            [this](const std::filesystem::path &,
                   const ProfileImportOptions &options) {
              ++importCalls;
              if (failImport) {
                return ProfileArchiveResult{.error = ProfileError::IoFailure,
                                            .message = "import failed"};
              }
              if (options.mode == ProfileImportMode::Overwrite) {
                auto *target = options.overwriteProfileId
                                   ? find(*options.overwriteProfileId)
                                   : nullptr;
                if (target == nullptr) {
                  return ProfileArchiveResult{.error = ProfileError::NotFound,
                                              .message = "target disappeared"};
                }
                target->displayName = "Imported over " + target->displayName;
                return archiveSuccess(*target, importSuccessMessage);
              }
              PlayerProfile imported = profile("echo", "Imported");
              profiles.push_back(imported);
              return archiveSuccess(std::move(imported), importSuccessMessage);
            },
        .flushSettings =
            [this](std::string &error) {
              ++settingsFlushes;
              if (throwSettingsNonStd) {
                throw 17;
              }
              if (!settingsFlushSucceeds) {
                error = "settings flush failed";
              }
              return settingsFlushSucceeds;
            },
        .flushInput =
            [this](std::string &error) {
              ++inputFlushes;
              if (throwInputNonStd) {
                throw 23;
              }
              if (!inputFlushSucceeds) {
                error = "input flush failed";
              }
              return inputFlushSucceeds;
            },
        .beginArchivePipeline =
            [this](std::string &error) {
              ++archivePipelineBeginCalls;
              if (!archivePipelineStarts || archivePipelineActive) {
                error = "archive pipeline unavailable";
                return false;
              }
              archivePipelineActive = true;
              return true;
            },
        .endArchivePipeline =
            [this]() {
              ++archivePipelineEndCalls;
              archivePipelineActive = false;
            }};
  }
};

ProfileRuntimeReapplyCallbacks countingRuntimeCallbacks(int &calls) {
  return {.sanitize = [&]() { ++calls; },
          .applyTheme = [&]() { ++calls; },
          .applyJukebox = [&]() { ++calls; },
          .applyMetadata =
              [&]() {
                ++calls;
                return std::string{};
              },
          .applyAudio =
              [&]() {
                ++calls;
                return std::string{};
              },
          .refreshDrafts = [&]() { ++calls; },
          .applyDisplay =
              [&]() {
                ++calls;
                return ProfileDisplayRuntimeResult{};
              }};
}

void testAuthoritativeRefreshKeepsStableUuidState() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());
  REQUIRE(controller.profiles().size() == 2);
  REQUIRE(controller.activeProfileId() == "alpha");
  REQUIRE(controller.selectedProfileId() == "alpha");
  REQUIRE(controller.select("bravo"));

  std::swap(fake.profiles[0], fake.profiles[1]);
  REQUIRE(controller.refresh());
  REQUIRE(controller.selectedProfileId() == "bravo");

  fake.profiles.clear();
  REQUIRE(!controller.refresh());
  REQUIRE(controller.profiles().size() == 2);
  REQUIRE(controller.selectedProfileId() == "bravo");
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);

  fake.profiles = {profile("alpha", "Changed Alpha")};
  fake.listError = ProfileError::IoFailure;
  fake.listMessage = "scan failed";
  REQUIRE(!controller.refresh());
  REQUIRE(controller.profiles().size() == 2);
  REQUIRE(controller.status().message == "scan failed");
}

void testEligibilityAndConfirmationStayBoundToUuid() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());
  REQUIRE(!controller.deleteEligibility("alpha").enabled);
  REQUIRE(controller.deleteEligibility("alpha").reason ==
          "Activate another profile first.");
  REQUIRE(controller.deleteEligibility("bravo").enabled);
  REQUIRE(!controller.overwriteEligibility("alpha").enabled);
  REQUIRE(controller.overwriteEligibility("bravo").enabled);

  REQUIRE(controller.requestDelete("bravo").ok());
  REQUIRE(controller.phase() == ProfileSettingsPhase::ConfirmDelete);
  REQUIRE(controller.confirmationProfileId() == "bravo");
  REQUIRE(controller.select("alpha"));
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(controller.confirmationProfileId().empty());
  REQUIRE(!controller.confirmDelete().ok());

  REQUIRE(controller.requestDelete("bravo").ok());
  std::swap(fake.profiles[0], fake.profiles[1]);
  REQUIRE(controller.refresh());
  REQUIRE(controller.confirmationProfileId() == "bravo");
  REQUIRE(controller.confirmDelete().ok());
  REQUIRE(fake.deleteCalls == 1);
  REQUIRE(fake.profiles.size() == 1);
  REQUIRE(fake.profiles.front().id == "alpha");
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);

  const auto last = controller.deleteEligibility("alpha");
  REQUIRE(!last.enabled);
  REQUIRE(last.reason == "Activate another profile first." ||
          last.reason == "Keep at least one profile.");
}

void testMutationResultsRefreshAndSelectWithoutActivating() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());

  const auto created = controller.create("Charlie");
  REQUIRE(created.ok());
  REQUIRE(controller.selectedProfileId() == "charlie");
  REQUIRE(controller.activeProfileId() == "alpha");
  REQUIRE(fake.activateCalls == 0);

  REQUIRE(controller.select("bravo"));
  const auto renamed = controller.rename("bravo", "New Bravo");
  REQUIRE(renamed.ok());
  REQUIRE(controller.selectedProfileId() == "bravo");
  REQUIRE(controller.profiles()[1].displayName == "New Bravo" ||
          controller.profiles()[0].displayName == "New Bravo");

  REQUIRE(controller.select("alpha"));
  const auto duplicated = controller.duplicate("alpha", "Alpha Copy");
  REQUIRE(duplicated.ok());
  REQUIRE(fake.settingsFlushes == 1);
  REQUIRE(fake.inputFlushes == 1);
  REQUIRE(fake.duplicateCalls == 1);
  REQUIRE(controller.selectedProfileId() == "delta");
  REQUIRE(controller.activeProfileId() == "alpha");
}

void testFlushFailureStopsDuplicateAndExport() {
  FakeServices fake;
  fake.settingsFlushSucceeds = false;
  ProfileSettingsController controller(fake.dependencies());

  const auto duplicate = controller.duplicate("alpha", "Copy");
  REQUIRE(!duplicate.ok());
  REQUIRE(fake.duplicateCalls == 0);
  REQUIRE(fake.settingsFlushes == 1);
  REQUIRE(fake.inputFlushes == 0);

  const auto exported = controller.exportProfile(
      "alpha", std::filesystem::path("/tmp/alpha.asobprofile"));
  REQUIRE(!exported.ok());
  REQUIRE(fake.exportCalls == 0);
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
}

void testActivationRetainsSelectionOnFailureAndRefreshesOnSuccess() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());
  REQUIRE(controller.select("bravo"));
  fake.failActivate = true;
  const auto failed = controller.activate("bravo");
  REQUIRE(!failed.ok());
  REQUIRE(controller.activeProfileId() == "alpha");
  REQUIRE(controller.selectedProfileId() == "bravo");
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
  int failedRuntimeCalls = 0;
  const auto failedRuntime = ReapplyProfileRuntimeAfterSwitch(
      failed, countingRuntimeCallbacks(failedRuntimeCalls));
  REQUIRE(!failedRuntime.profileCommitted);
  REQUIRE(failedRuntimeCalls == 0);

  fake.failActivate = false;
  const auto activated = controller.activate("bravo");
  REQUIRE(activated.ok());
  REQUIRE(controller.activeProfileId() == "bravo");
  REQUIRE(controller.selectedProfileId() == "bravo");
}

void testAuthoritativePostCommitFailureRunsRuntimeReapply() {
  FakeServices fake;
  fake.failActivateAfterCommit = true;
  ProfileSettingsController controller(fake.dependencies());
  REQUIRE(controller.select("bravo"));

  const auto activated = controller.activate("bravo");
  REQUIRE(activated.ok());
  REQUIRE(controller.activeProfileId() == "bravo");
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Warning);
  REQUIRE(controller.status().message.find("after commit") !=
          std::string::npos);

  int runtimeCalls = 0;
  const auto runtime = ReapplyProfileRuntimeAfterSwitch(
      activated, countingRuntimeCallbacks(runtimeCalls));
  REQUIRE(runtime.profileCommitted);
  REQUIRE(runtimeCalls == 7);
}

void testSplitArchiveTaskIsSerializedAndMapsWarnings() {
  FakeServices fake;
  fake.exportSuccessMessage = "exported; stale backup cleanup deferred";
  ProfileSettingsController controller(fake.dependencies());

  auto task = controller.beginExport(
      "alpha", std::filesystem::path("/tmp/alpha.asobprofile"));
  REQUIRE(task.has_value());
  REQUIRE(controller.phase() == ProfileSettingsPhase::PreparingExport);
  REQUIRE(fake.archivePipelineActive);
  REQUIRE(fake.settingsFlushes == 1);
  REQUIRE(fake.inputFlushes == 1);
  REQUIRE(!controller.beginExport(
      "bravo", std::filesystem::path("/tmp/bravo.asobprofile")));
  REQUIRE(fake.exportCalls == 0);

  const auto result = task->execute();
  REQUIRE(result.ok());
  REQUIRE(fake.exportCalls == 1);
  const auto repeated = task->execute();
  REQUIRE(!repeated.ok());
  REQUIRE(fake.exportCalls == 1);
  REQUIRE(controller.beginPreparedExportPicker(task->generation()));
  REQUIRE(controller.phase() == ProfileSettingsPhase::PickingExport);
  REQUIRE(fake.archivePipelineActive);
  REQUIRE(!controller.actionsEnabled());
  REQUIRE(controller.completeArchive(task->kind(), task->generation(), result));
  REQUIRE(!fake.archivePipelineActive);
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Warning);
  REQUIRE(controller.status().message == fake.exportSuccessMessage);

  REQUIRE(
      !controller.completeArchive(task->kind(), task->generation(), result));
}

void testImportCreateAndConfirmedOverwriteKeepUuidSemantics() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());

  auto createTask = controller.beginImport(
      "/tmp/new.asobprofile",
      ProfileImportOptions{.mode = ProfileImportMode::CreateWithNewId});
  REQUIRE(createTask.has_value());
  auto created = createTask->execute();
  REQUIRE(controller.completeArchive(createTask->kind(),
                                     createTask->generation(), created));
  REQUIRE(controller.selectedProfileId() == "echo");
  REQUIRE(controller.activeProfileId() == "alpha");

  const ProfileImportOptions overwrite{.mode = ProfileImportMode::Overwrite,
                                       .overwriteProfileId =
                                           std::string("bravo")};
  REQUIRE(!controller.beginImport("/tmp/overwrite.asobprofile", overwrite));
  REQUIRE(controller.requestOverwrite("bravo").ok());
  REQUIRE(controller.phase() == ProfileSettingsPhase::ConfirmOverwrite);
  REQUIRE(controller.confirmationProfileId() == "bravo");
  REQUIRE(controller.select("echo"));
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(controller.confirmationProfileId().empty());
  REQUIRE(!controller.beginImport("/tmp/overwrite.asobprofile", overwrite));
  REQUIRE(controller.requestOverwrite("bravo").ok());
  REQUIRE(controller.beginConfirmedOverwritePicker());
  REQUIRE(controller.phase() == ProfileSettingsPhase::PickingImport);
  REQUIRE(controller.confirmationProfileId() == "bravo");
  auto overwriteTask =
      controller.beginImport("/tmp/overwrite.asobprofile", overwrite);
  REQUIRE(overwriteTask.has_value());
  REQUIRE(controller.phase() == ProfileSettingsPhase::Importing);
  auto overwritten = overwriteTask->execute();
  REQUIRE(controller.completeArchive(overwriteTask->kind(),
                                     overwriteTask->generation(), overwritten));
  REQUIRE(controller.selectedProfileId() == "bravo");
  REQUIRE(controller.activeProfileId() == "alpha");

  const ProfileImportOptions activeOverwrite{
      .mode = ProfileImportMode::Overwrite,
      .overwriteProfileId = std::string("alpha")};
  REQUIRE(!controller.requestOverwrite("alpha").ok());
  REQUIRE(!controller.beginImport("/tmp/active.asobprofile", activeOverwrite));
}

void testPickerCancellationIsNeutralAndRuntimeWarningIsRetained() {
  FakeServices fake;
  ProfileSettingsController controller(fake.dependencies());
  const auto before = controller.status();
  REQUIRE(controller.beginImportPicker());
  REQUIRE(controller.phase() == ProfileSettingsPhase::PickingImport);
  REQUIRE(fake.archivePipelineActive);
  controller.cancelPicker();
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(!fake.archivePipelineActive);
  REQUIRE(controller.status() == before);

  auto exportTask =
      controller.beginExport("bravo", "/tmp/cancelled.asobprofile");
  REQUIRE(exportTask.has_value());
  const auto exportResult = exportTask->execute();
  REQUIRE(exportResult.ok());
  REQUIRE(controller.beginPreparedExportPicker(exportTask->generation()));
  controller.cancelPicker();
  REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  REQUIRE(!fake.archivePipelineActive);
  REQUIRE(controller.status() == before);

  controller.recordWarning("Profile switched, but audio restart failed.");
  REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Warning);
  REQUIRE(controller.status().message.find("audio restart failed") !=
          std::string::npos);
}

void testMutationExceptionsStillRefreshAuthoritativeState() {
  {
    FakeServices fake;
    fake.throwAfterMutation = "create";
    ProfileSettingsController controller(fake.dependencies());
    const int before = fake.listCalls;
    REQUIRE(!controller.create("Committed Charlie").ok());
    REQUIRE(fake.listCalls == before + 1);
    REQUIRE(controller.profiles().size() == 3);
    REQUIRE(controller.selectedProfileId() == "alpha");
  }
  {
    FakeServices fake;
    fake.throwAfterMutation = "rename";
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.select("bravo"));
    const int before = fake.listCalls;
    REQUIRE(!controller.rename("bravo", "Committed Rename").ok());
    REQUIRE(fake.listCalls == before + 1);
    REQUIRE(controller.selectedProfileId() == "bravo");
    REQUIRE(fake.find("bravo")->displayName == "Committed Rename");
  }
  {
    FakeServices fake;
    fake.throwAfterMutation = "duplicate";
    ProfileSettingsController controller(fake.dependencies());
    const int before = fake.listCalls;
    REQUIRE(!controller.duplicate("alpha", "Committed Copy").ok());
    REQUIRE(fake.listCalls == before + 1);
    REQUIRE(controller.profiles().size() == 3);
    REQUIRE(controller.selectedProfileId() == "alpha");
  }
  {
    FakeServices fake;
    fake.throwAfterMutation = "remove";
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.select("bravo"));
    const int before = fake.listCalls;
    REQUIRE(!controller.remove("bravo").ok());
    REQUIRE(fake.listCalls == before + 1);
    REQUIRE(controller.profiles().size() == 1);
    REQUIRE(controller.selectedProfileId() == "alpha");
  }
  {
    FakeServices fake;
    fake.throwAfterMutation = "activate";
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.select("bravo"));
    const int before = fake.listCalls;
    const auto activated = controller.activate("bravo");
    REQUIRE(activated.ok());
    REQUIRE(fake.listCalls == before + 1);
    REQUIRE(controller.activeProfileId() == "bravo");
    REQUIRE(controller.selectedProfileId() == "bravo");
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Warning);

    int runtimeCalls = 0;
    const auto runtime = ReapplyProfileRuntimeAfterSwitch(
        activated, countingRuntimeCallbacks(runtimeCalls));
    REQUIRE(runtime.profileCommitted);
    REQUIRE(runtimeCalls == 7);
  }
}

void testNonStandardFlushExceptionsAreMappedToFailures() {
  {
    FakeServices fake;
    fake.throwSettingsNonStd = true;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(!controller.duplicate("alpha", "Copy").ok());
    REQUIRE(fake.duplicateCalls == 0);
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
  }
  {
    FakeServices fake;
    fake.throwInputNonStd = true;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(!controller.exportProfile("alpha", "/tmp/never-created.asobprofile")
                 .ok());
    REQUIRE(fake.exportCalls == 0);
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
  }
}

void testArchivePipelineFlagClearsOnEveryTerminalPath() {
  {
    FakeServices fake;
    {
      ProfileSettingsController controller(fake.dependencies());
      REQUIRE(controller.beginImportPicker());
      REQUIRE(fake.archivePipelineActive);
    }
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(fake.archivePipelineEndCalls == 1);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.requestOverwrite("bravo").ok());
    REQUIRE(controller.beginConfirmedOverwritePicker());
    REQUIRE(fake.archivePipelineActive);
    controller.cancelPicker();
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(controller.confirmationProfileId().empty());
    REQUIRE(controller.status().message.find("Choose Overwrite") ==
            std::string::npos);
    REQUIRE(!fake.archivePipelineActive);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.requestOverwrite("bravo").ok());
    REQUIRE(controller.beginConfirmedOverwritePicker());
    REQUIRE(fake.archivePipelineActive);
    REQUIRE(controller.select("alpha"));
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(controller.confirmationProfileId().empty());
    REQUIRE(!fake.archivePipelineActive);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.requestOverwrite("bravo").ok());
    REQUIRE(controller.beginConfirmedOverwritePicker());
    const ProfileImportOptions mismatched{.mode = ProfileImportMode::Overwrite,
                                          .overwriteProfileId =
                                              std::string("alpha")};
    REQUIRE(!controller.beginImport("/tmp/mismatch.asobprofile", mismatched));
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(fake.importCalls == 0);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(!controller.beginExport("alpha", {}));
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(fake.archivePipelineEndCalls == 1);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    auto task = controller.beginExport("alpha", "/tmp/abandoned.asobprofile");
    REQUIRE(task.has_value());
    REQUIRE(fake.archivePipelineActive);
    controller.abandonArchive(task->generation());
    REQUIRE(!fake.archivePipelineActive);
  }
  {
    FakeServices fake;
    fake.archivePipelineStarts = false;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(!controller.beginImportPicker());
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
    REQUIRE(fake.archivePipelineEndCalls == 0);
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    REQUIRE(controller.beginImportPicker());
    REQUIRE(fake.archivePipelineActive);
    REQUIRE(controller.failPicker("The document picker could not open."));
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
    REQUIRE(controller.status().message ==
            "The document picker could not open.");
    REQUIRE(!controller.failPicker("stale picker failure"));
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    auto task = controller.beginExport("alpha", "/tmp/prepared.asobprofile");
    REQUIRE(task.has_value());
    REQUIRE(task->execute().ok());
    REQUIRE(controller.beginPreparedExportPicker(task->generation()));
    REQUIRE(controller.failPicker({}));
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
    REQUIRE(controller.status().message ==
            "Unable to access the selected profile archive document.");
  }
  {
    FakeServices fake;
    ProfileSettingsController controller(fake.dependencies());
    controller.recordError("Private profile archive storage is unavailable.");
    REQUIRE(controller.phase() == ProfileSettingsPhase::Idle);
    REQUIRE(controller.status().kind == ProfileSettingsStatusKind::Error);
    REQUIRE(controller.status().message ==
            "Private profile archive storage is unavailable.");
  }
}
} // namespace

int main() {
  testAuthoritativeRefreshKeepsStableUuidState();
  testEligibilityAndConfirmationStayBoundToUuid();
  testMutationResultsRefreshAndSelectWithoutActivating();
  testFlushFailureStopsDuplicateAndExport();
  testActivationRetainsSelectionOnFailureAndRefreshesOnSuccess();
  testAuthoritativePostCommitFailureRunsRuntimeReapply();
  testSplitArchiveTaskIsSerializedAndMapsWarnings();
  testImportCreateAndConfirmedOverwriteKeepUuidSemantics();
  testPickerCancellationIsNeutralAndRuntimeWarningIsRetained();
  testMutationExceptionsStillRefreshAuthoritativeState();
  testNonStandardFlushExceptionsAreMappedToFailures();
  testArchivePipelineFlagClearsOnEveryTerminalPath();
  std::cout << "profile settings controller tests passed\n";
  return 0;
}
