// The task, controller, batch outcome mapper, and scene methods are real.
// Candidate verification, draft construction, and durable storage are controlled
// effects; the fixture never submits scores or writes a real outbox.
#include "ir/IrProfileSettings.h"

namespace ir {
std::optional<IrSubmission> submissionForIrUploadCandidate(
    const IrUploadCandidate &value, std::string &) noexcept {
  return submissionFor(value);
}
} // namespace ir

namespace {
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
  PreparationDrivers irDrivers;
  std::unique_ptr<PreparationService> irSubmissionService = std::make_unique<PreparationService>();
};
class IrUploadsScene {
public:
  explicit IrUploadsScene(PreparationContext &application) : context(application) {}
  PreparationContext &context;
  ir_uploads::Controller controller;
  ir_uploads::PreparationTask preparationTask;
  bool providerCanSubmit = true, reloadRequested = false;
  std::thread::id applicationThread = std::this_thread::get_id();
  int refreshes = 0, providerRefreshes = 0;
  void refreshProviderState() { ++providerRefreshes; }
  void refreshUi() { assert(applicationThread == std::this_thread::get_id()); ++refreshes; }
  void startUpload();
  void applyPreparationUpdates();
  void stopPreparation();
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
}
} // namespace
