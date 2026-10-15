// The task, controller, batch outcome mapper, and scene methods are real.
// Candidate verification, draft construction, and durable storage are controlled
// effects; the fixture never submits scores or writes a real outbox.
#include "ir/IrProfileSettings.h"
#include "scene/SceneReturnTarget.h"

class Scene {};

namespace ir {
std::optional<IrSubmission> submissionForIrUploadCandidate(
    const IrUploadCandidate &value, std::string &) noexcept {
  return submissionFor(value);
}
} // namespace ir

namespace {
struct PreparationContext;
enum class SettingsDestination { Ir };
struct SettingsScene {
  SettingsScene(PreparationContext &, SettingsDestination,
                SceneReturnTarget target) : returnTarget(std::move(target)) {}
  SceneReturnTarget returnTarget;
};
struct PreparationSceneManager {
  bool retained = false;
  SceneReturnTarget target;
  void changeScene(std::unique_ptr<SettingsScene> scene, bool keepBackground) {
    retained = keepBackground;
    target = scene->returnTarget;
  }
};
struct PreparationDrivers {
  int drafts = 0;
  ir::BuildDraftOutcome buildDraft(std::string_view provider, const ir::IrSubmission &submission) {
    ++drafts;
    return {.status = ir::BuildDraftStatus::Built,
            .draft = ir::IrOutboxDraft{.providerId = std::string(provider),
                                       .attemptId = submission.attemptId}};
  }
};
struct PreparationService {
  int batches = 0;
  PreparationGate *gate = nullptr;
  ir::IrManualBatchEnqueueOutcome enqueueManualBatch(std::span<const ir::IrOutboxDraft> drafts) {
    ++batches;
    if (gate) { gate->block(); }
    ir::IrManualBatchEnqueueOutcome result{.storageAvailable = true};
    for (const auto &draft : drafts) {
      result.items.push_back({.attemptId = draft.attemptId,
                             .status = ir::IrManualBatchItemStatus::Inserted});
    }
    return result;
  }
};
struct PreparationContext {
  PreparationSceneManager *sceneManager = nullptr;
  PreparationDrivers irDrivers;
  std::unique_ptr<PreparationService> irSubmissionService = std::make_unique<PreparationService>();
};
struct PreparationCandidateList {
  float scrollOffset = 25;
  std::vector<ir::IrUploadCandidate> candidates;
  void setCandidates(const std::vector<ir::IrUploadCandidate> &values,
                     const std::unordered_set<std::string> &) {
    candidates = values;
  }
};
class IrUploadsScene {
public:
  explicit IrUploadsScene(PreparationContext &application) : context(application) {}
  PreparationContext &context;
  ir_uploads::Controller controller;
  ir_uploads::PreparationTask preparationTask;
  bool providerCanSubmit = true, reloadRequested = false;
  bool candidatesLoaded = false;
  std::thread::id applicationThread = std::this_thread::get_id();
  int refreshes = 0, providerRefreshes = 0;
  void refreshProviderState() { ++providerRefreshes; }
  void refreshUi() { assert(applicationThread == std::this_thread::get_id()); ++refreshes; }
  void startUpload();
  void applyPreparationUpdates();
  void stopPreparation();
  void onPause();
  void onProfileWillChange();
  void openIrSettings();
  void setTabTopInset(float topInset);
  float tabTopInset = 0;
  int layoutUpdates = 0;
  void updateLayoutInsets() { ++layoutUpdates; }
  bool embeddedTab = false;
  Scene *tabSettingsReturnOwner = nullptr;
  SceneReturnTarget returnTarget_ = SceneReturnTarget::Registered("CustomMenu");
  std::string displayedProfileId = "previous-profile";
  i18n::Text loadError, loadDiagnostic;
  PreparationCandidateList *candidateList = nullptr;
  int pointerCancellations = 0;
  void onInputQueueOverflow() { ++pointerCancellations; }
};

#include "ir_upload_scene_methods.inc"

void applyPreparationUntilUnlocked(IrUploadsScene &scene) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (scene.controller.selectionLocked() && std::chrono::steady_clock::now() < deadline) {
    scene.applyPreparationUpdates();
    std::this_thread::yield();
  }
  assert(!scene.controller.selectionLocked());
}

void testSceneSettingsRouteRetainsEmbeddedShell() {
  PreparationContext application;
  PreparationSceneManager manager;
  application.sceneManager = &manager;
  IrUploadsScene scene(application);
  scene.openIrSettings();
  assert(!manager.retained);
  assert(manager.target.kind == SceneReturnTarget::Kind::Registered);
  assert(manager.target.registeredName == "CustomMenu");
  Scene shell;
  scene.embeddedTab = true;
  scene.tabSettingsReturnOwner = &shell;
  scene.openIrSettings();
  assert(manager.retained);
  assert(manager.target.kind == SceneReturnTarget::Kind::Retained);
  assert(manager.target.retained == &shell);
  scene.setTabTopInset(160);
  scene.setTabTopInset(160);
  assert(scene.layoutUpdates == 1 && scene.tabTopInset == 160);
  scene.setTabTopInset(-1);
  scene.setTabTopInset(0);
  assert(scene.layoutUpdates == 2 && scene.tabTopInset == 0);
}

void testScenePreparationLaunchGatesAndApplicationThreadCompletion() {
  PreparationContext application;
  IrUploadsScene scene(application);
  scene.startUpload();
  assert(!scene.preparationTask.hasWorker());
  scene.controller.replaceCandidates({candidate(20), candidate(21)});
  scene.controller.selectAll();
  scene.providerCanSubmit = false;
  scene.startUpload();
  assert(!scene.preparationTask.hasWorker() && scene.controller.selectedCount() == 2);
  scene.providerCanSubmit = true;
  PreparationGate gate;
  application.irSubmissionService->gate = &gate;
  scene.startUpload();
  gate.wait();
  scene.onPause();
  assert(scene.pointerCancellations == 1 && scene.preparationTask.hasWorker());
  assert(scene.controller.selectionLocked() && scene.controller.selectedCount() == 2);
  assert(application.irDrivers.drafts == 2 && application.irSubmissionService->batches == 1);
  scene.startUpload();
  assert(application.irSubmissionService->batches == 1);
  scene.applyPreparationUpdates();
  assert(scene.controller.statusText() == "Preparing 2 of 2...");
  assert(!scene.reloadRequested);
  gate.release.set_value();
  applyPreparationUntilUnlocked(scene);
  assert(!scene.preparationTask.hasWorker() && scene.reloadRequested);
  assert(scene.controller.selectedCount() == 0);
  const auto refreshes = scene.refreshes;
  scene.applyPreparationUpdates();
  scene.stopPreparation();
  assert(scene.refreshes == refreshes);
}

void testSceneStopRetainsCancelledSelectionUntilCompletion() {
  PreparationContext application;
  IrUploadsScene scene(application);
  scene.controller.replaceCandidates({candidate(22)});
  scene.controller.selectAll();
  auto candidates = scene.controller.beginPreparation();
  std::promise<void> entered;
  assert(scene.preparationTask.start(std::move(candidates), {
      .verify = [&](const auto &, const auto &token) {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        assert(token.stop_requested());
        return ir_uploads::VerificationOutcome{};
      }}));
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  scene.stopPreparation();
  assert(!scene.preparationTask.hasWorker() && scene.controller.selectionLocked());
  assert(scene.controller.statusText() == "Cancelling...");
  scene.applyPreparationUpdates();
  assert(!scene.controller.selectionLocked() && scene.controller.selectedCount() == 1);
  assert(scene.controller.statusText() == "Upload cancelled." && scene.reloadRequested);

  // A profile handoff retires any worker and its old-profile completion before
  // the caller replaces the repositories used by durable enqueue dependencies.
  PreparationCandidateList list;
  list.candidates = scene.controller.candidates();
  scene.candidateList = &list;
  candidates = scene.controller.beginPreparation();
  std::promise<void> profileWorkEntered;
  assert(scene.preparationTask.start(std::move(candidates), {
      .verify = [&](const auto &, const auto &token) {
        profileWorkEntered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        assert(token.stop_requested());
        return ir_uploads::VerificationOutcome{};
      }}));
  assert(profileWorkEntered.get_future().wait_for(5s) == std::future_status::ready);
  scene.candidatesLoaded = true;
  scene.onProfileWillChange();
  assert(!scene.preparationTask.hasWorker() && !scene.controller.selectionLocked());
  assert(scene.controller.candidates().empty() && scene.controller.selectedCount() == 0);
  assert(scene.displayedProfileId.empty() && list.candidates.empty() && list.scrollOffset == 0);
  assert(!scene.candidatesLoaded && "profile handoff must invalidate the loaded candidate snapshot");
  const auto refreshes = scene.refreshes;
  scene.applyPreparationUpdates();
  assert(!scene.reloadRequested && scene.refreshes == refreshes);
}
} // namespace
