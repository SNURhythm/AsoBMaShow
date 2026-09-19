namespace profile_archive_scene_fixture {
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
void SDL_LogWarn(int, const char *, ...) {}
constexpr std::string_view kProfileArchiveMimeType = "application/zip";
constexpr std::string_view kProfileArchiveExportName = "profile.asobprofile";
enum class SettingsProfileDocumentHandoffKind { None, Import, Export };
struct TemporaryEvidence { std::atomic<int> attempts{0}, releases{0}; bool succeeds = true; };
struct TemporaryOwner {
  TemporaryEvidence &e;
  ~TemporaryOwner() { ++e.releases; }
};
struct PlatformDocumentHandoffResult {
  std::string message;
  std::filesystem::path localPath;
  std::shared_ptr<TemporaryOwner> owner;
  bool success = true, wasCancelled = false;
  bool ok() const { return success; }
  bool cancelled() const { return wasCancelled; }
};
struct PlatformDocumentExportRequest {
  std::filesystem::path localPath;
  std::string mimeType, suggestedName;
  std::uint64_t maxBytes = 0;
  std::shared_ptr<void> sourceLifetime;
};
struct Handoff {
  bool active = false;
  std::optional<PlatformDocumentHandoffResult> result;
  explicit operator bool() const { return active; }
  void close() { active = false; }
  bool ready() const { return result.has_value(); }
  std::optional<PlatformDocumentHandoffResult> takeResult() { return std::exchange(result, {}); }
};
namespace platform_document_handoff {
std::optional<PlatformDocumentExportRequest> nativeRequest;
bool rejectPicker = false;
bool CleanupTemporaryDocument(PlatformDocumentHandoffResult &document) {
  if (!document.owner) return true;
  auto &e = document.owner->e;
  ++e.attempts;
  if (!e.succeeds) return false;
  document.owner.reset();
  return true;
}
Handoff ExportDocumentAsync(PlatformDocumentExportRequest request) {
  if (rejectPicker) return {};
  nativeRequest = std::move(request);
  return {.active = true};
}
} // namespace platform_document_handoff

// A controlled thread-launch failure exercises the production scene catch path.
// Normal runs delegate all lifecycle behavior to the real asynchronous owner.
struct Worker {
  using AfterExecute = ProfileArchiveWorker::AfterExecute;
  ProfileArchiveWorker owner;
  bool hasWorker() const { return owner.hasWorker(); }
  auto takeCompletion() { return owner.takeCompletion(); }
  void stopAndWait() { owner.stopAndWait(); }
  int launchFailure = 0;
  void start(ProfileArchiveTask task, AfterExecute after) {
    if (launchFailure == 1) throw std::runtime_error("thread unavailable");
    if (launchFailure == 2) throw 42;
    owner.start(std::move(task), std::move(after));
  }
};
class SettingsScene {
public:
  explicit SettingsScene(FakeServices &fake) : fake(fake) { ensureProfileController(); }
  ~SettingsScene() { stopProfileArchiveWork(); }
  void ensureProfileController() {
    if (!profileController) profileController = std::make_unique<ProfileSettingsController>(fake.dependencies());
  }
  void invalidateProfileLayout() { ++layouts; }
  bool startProfileArchiveTask(ProfileArchiveTask task,
      std::optional<PlatformDocumentHandoffResult> temporaryDocument = {});
  void applyPendingProfileArchiveCompletion();
  void applyPendingProfileDocumentHandoff();
  void stopProfileArchiveWork();
  FakeServices &fake;
  Worker profileArchiveWorker;
  std::unique_ptr<ProfileSettingsController> profileController;
  std::uint64_t profileArchiveGeneration = 0;
  std::filesystem::path profileExportStagingFile;
  std::shared_ptr<void> profileExportSourceLifetime;
  std::optional<ProfileArchiveResult> preparedProfileExportResult;
  Handoff profileDocumentHandoff;
  SettingsProfileDocumentHandoffKind profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
  ProfileImportOptions pendingProfileImportOptions;
  int layouts = 0;
};
#include "profile_archive_scene_methods.inc"

void consumeArchive(SettingsScene &scene) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (scene.profileArchiveWorker.hasWorker()) {
    scene.applyPendingProfileArchiveCompletion();
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}
PlatformDocumentHandoffResult temporary(TemporaryEvidence &e) {
  PlatformDocumentHandoffResult result{.localPath = "private.zip"};
  result.owner = std::shared_ptr<TemporaryOwner>(new TemporaryOwner{e});
  return result;
}
void testImportCleanupAndFailure() {
  for (const bool importFails : {false, true}) {
    for (const bool cleanupSucceeds : {false, true}) {
      TemporaryEvidence evidence;
      evidence.succeeds = cleanupSucceeds;
      FakeServices fake;
      fake.failImport = importFails;
      fake.importSuccessMessage = "Imported.";
      SettingsScene scene(fake);
      auto task = scene.profileController->beginImport("private.zip");
      REQUIRE(task.has_value());
      REQUIRE(scene.startProfileArchiveTask(std::move(*task), temporary(evidence)));
      consumeArchive(scene);
      REQUIRE(evidence.attempts == 1);
      REQUIRE(evidence.releases == 1);
      REQUIRE(fake.importCalls == 1);
      REQUIRE(!fake.archivePipelineActive);
      REQUIRE(scene.profileArchiveGeneration == 0);
      const auto &status = scene.profileController->status();
      if (importFails) {
        REQUIRE(status.kind == ProfileSettingsStatusKind::Error);
        REQUIRE(status.message.find("temporary cleanup") == std::string::npos);
      } else if (!cleanupSucceeds) {
        REQUIRE(status.message == "Imported.; Profile imported; temporary cleanup is pending.");
      } else {
        REQUIRE(status.message == "Imported.");
      }
      const int layouts = scene.layouts;
      scene.applyPendingProfileArchiveCompletion();
      REQUIRE(scene.layouts == layouts);
    }
  }
}
void testLaunchFailureAndRejectedAdmission() {
  for (const int failure : {0, 1, 2}) {
    TemporaryEvidence evidence;
    FakeServices fake;
    SettingsScene scene(fake);
    auto task = scene.profileController->beginImport("private.zip");
    REQUIRE(task.has_value());
    if (failure == 0) scene.profileArchiveGeneration = task->generation();
    else scene.profileArchiveWorker.launchFailure = failure;
    REQUIRE(!scene.startProfileArchiveTask(std::move(*task), temporary(evidence)));
    REQUIRE(evidence.attempts == 1 && evidence.releases == 1);
    REQUIRE(fake.importCalls == 0);
    REQUIRE(!fake.archivePipelineActive);
    REQUIRE(!scene.profileArchiveWorker.hasWorker());
    REQUIRE(scene.profileController->status().message == (failure == 0
        ? "A profile task is already running." : "Could not start the profile task."));
  }
}
void testExportPickerOwnsStagingUntilNativeRelease() {
  for (const bool rejectPicker : {false, true}) {
    platform_document_handoff::rejectPicker = rejectPicker;
    platform_document_handoff::nativeRequest.reset();
    FakeServices fake;
    SettingsScene scene(fake);
    auto source = std::make_shared<int>(1);
    std::weak_ptr<int> weakSource = source;
    scene.profileExportSourceLifetime = std::move(source);
    scene.profileExportStagingFile = "staged-profile.zip";
    auto task = scene.profileController->beginExport("bravo", scene.profileExportStagingFile);
    REQUIRE(task.has_value());
    const auto generation = task->generation();
    REQUIRE(scene.startProfileArchiveTask(std::move(*task)));
    consumeArchive(scene);
    REQUIRE(scene.profileExportStagingFile.empty());
    REQUIRE(!scene.profileExportSourceLifetime);
    if (rejectPicker) {
      REQUIRE(weakSource.expired());
      REQUIRE(scene.profileArchiveGeneration == 0);
      REQUIRE(!fake.archivePipelineActive);
      REQUIRE(scene.profileController->status().message == "Could not open the save picker.");
    } else {
      REQUIRE(!weakSource.expired());
      REQUIRE(scene.profileArchiveGeneration == generation);
      REQUIRE(scene.profileController->phase() == ProfileSettingsPhase::PickingExport);
      REQUIRE(fake.archivePipelineActive);
      const auto &request = *platform_document_handoff::nativeRequest;
      REQUIRE(request.localPath == "staged-profile.zip");
      REQUIRE(request.mimeType == "application/zip");
      REQUIRE(request.maxBytes == ProfileArchiveSizePolicy::kMaximumExistingArchiveBytes);
      scene.profileDocumentHandoff.result = PlatformDocumentHandoffResult{};
      scene.applyPendingProfileDocumentHandoff();
      REQUIRE(scene.profileController->phase() == ProfileSettingsPhase::Idle);
      REQUIRE(!fake.archivePipelineActive);
      REQUIRE(scene.profileArchiveGeneration == 0);
      REQUIRE(!scene.preparedProfileExportResult);
      scene.stopProfileArchiveWork();
      REQUIRE(!weakSource.expired());
      platform_document_handoff::nativeRequest.reset();
      REQUIRE(weakSource.expired());
    }
  }
  platform_document_handoff::rejectPicker = false;
}
void testStopJoinsBeforeControllerAndTemporaryRelease() {
  TemporaryEvidence evidence;
  FakeServices fake;
  SettingsScene scene(fake);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto dependencies = fake.dependencies();
  dependencies.importProfile = [&](const auto &, const auto &) {
    entered.set_value();
    released.wait();
    REQUIRE(fake.archivePipelineActive);
    REQUIRE(evidence.releases == 0);
    return archiveSuccess(profile("charlie", "Charlie"));
  };
  scene.profileController = std::make_unique<ProfileSettingsController>(std::move(dependencies));
  auto task = scene.profileController->beginImport("private.zip");
  REQUIRE(task.has_value());
  REQUIRE(scene.startProfileArchiveTask(std::move(*task), temporary(evidence)));
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  std::promise<void> stopping;
  auto stopped = std::async(std::launch::async, [&] {
    stopping.set_value();
    scene.stopProfileArchiveWork();
  });
  stopping.get_future().wait();
  REQUIRE(stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  release.set_value();
  REQUIRE(stopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  stopped.get();
  REQUIRE(evidence.attempts == 1 && evidence.releases == 1);
  REQUIRE(!fake.archivePipelineActive);
  REQUIRE(!scene.profileController && scene.profileArchiveGeneration == 0);
  REQUIRE(!scene.profileArchiveWorker.hasWorker());
  REQUIRE(!scene.profileArchiveWorker.takeCompletion());
  scene.applyPendingProfileArchiveCompletion();
  scene.stopProfileArchiveWork();
}
void run() {
  testImportCleanupAndFailure();
  testLaunchFailureAndRejectedAdmission();
  testExportPickerOwnsStagingUntilNativeRelease();
  testStopJoinsBeforeControllerAndTemporaryRelease();
}
} // namespace profile_archive_scene_fixture
