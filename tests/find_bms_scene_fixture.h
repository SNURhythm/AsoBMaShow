// Real service result types, task, dialog policy, and complete scene methods.
// Service definitions below replace network/filesystem effects with controlled
// calls. View and indexing doubles check the application-thread handoff.
#include "scene/FindBmsDialogPolicy.h"
#include "scene/FindBmsProgressPresentation.h"
#include <algorithm>

namespace {
struct ServiceCalls {
  int lookups = 0, downloads = 0, resolutions = 0;
  std::string sha256, md5, title, artist, candidateId;
  std::filesystem::path root;
  bool skipUnarchiving = false;
  bool cancellationObserved = false;
  BmsSearchPendingArtifactDecision decision = BmsSearchPendingArtifactDecision::Keep;
  std::optional<BmsSearchPendingArtifact> resolvedArtifact;
  BmsSearchResult result;
  Gate *gate = nullptr;
  std::thread::id applicationThread = std::this_thread::get_id();
};
ServiceCalls *serviceCalls = nullptr;
} // namespace

std::string BmsSearchService::patternUrlForSha256(const std::string &hash) { return "pattern/" + hash; }
std::string BmsSearchService::searchUrlForText(const std::string &text) { return "search/" + text; }
BmsSearchResult BmsSearchService::findAndDownload(
    const std::string &sha256, const std::string &md5,
    const std::filesystem::path &root, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progress, const std::string &title,
    const std::string &artist, BmsSearchDownloadOptions options) const {
  auto &calls = *serviceCalls;
  assert(calls.applicationThread != std::this_thread::get_id());
  ++calls.lookups;
  calls.sha256 = sha256; calls.md5 = md5; calls.root = root;
  calls.title = title; calls.artist = artist;
  calls.skipUnarchiving = options.skipUnarchivingForNonSolidArchives;
  progress({"Downloading archive", 20, 100});
  progress({"Downloading archive", 40, 100});
  if (calls.gate) { calls.gate->block(); }
  calls.cancellationObserved = cancelled.load();
  return calls.result;
}
BmsSearchResult BmsSearchService::downloadCandidate(
    const BmsSearchCandidate &candidate, const std::string &sha256,
    const std::string &md5, const std::filesystem::path &root,
    std::atomic_bool &cancelled, BmsSearchDownloadProgressCallback progress,
    BmsSearchDownloadOptions options) const {
  auto &calls = *serviceCalls;
  ++calls.downloads; calls.candidateId = candidate.id;
  // Reuse the controlled transport while recording which service entry ran.
  auto result = findAndDownload(sha256, md5, root, cancelled, std::move(progress), {}, {}, options);
  --calls.lookups;
  return result;
}
BmsSearchResult BmsSearchService::resolvePendingArtifact(
    BmsSearchResult result, BmsSearchPendingArtifactDecision decision) const {
  auto &calls = *serviceCalls;
  assert(calls.applicationThread != std::this_thread::get_id());
  ++calls.resolutions; calls.decision = decision; calls.resolvedArtifact = result.pendingArtifact;
  if (calls.gate) { calls.gate->block(); }
  return calls.result;
}

namespace {
struct ChartMeta {
  std::string SHA256, MD5, Title, Artist;
};
struct ChartMetaRecord { ChartMeta meta; };
namespace main_menu_library {
std::string findBmsChartIdentity(const ChartMeta &meta) { return meta.SHA256; }
}
namespace rendering { constexpr int window_width = 1280, window_height = 720; }
constexpr std::size_t kFindBmsMaxLogLines = 120;
struct FindBmsModal {
  std::thread::id owner = std::this_thread::get_id();
  bool visible = false;
  void setSize(int, int) { assert(owner == std::this_thread::get_id()); }
  void setVisible(bool value) { assert(owner == std::this_thread::get_id()); visible = value; }
};
class MainMenuScene {
public:
  ~MainMenuScene() { findBmsTask.stopAndWait(); }
  FindBmsTask findBmsTask;
  FindBmsModal modal;
  FindBmsModal *findBmsModalRoot = &modal;
  struct { struct { bool findBmsSkipUnarchivingForNonSolidArchives = true; } settings; } context;
  ChartMetaRecord findBmsModalChart;
  BmsSearchResult findBmsResult;
  std::optional<BmsSearchPendingArtifactDecision> findBmsPendingDecision;
  std::string findBmsProgressMessage;
  std::uint64_t findBmsProgressCurrent = 0, findBmsProgressTotal = 0;
  double findBmsProgressFraction = 0;
  std::deque<std::string> findBmsProgressLog;
  std::uint64_t chartSelectionGeneration = 7, findBmsSelectionGenerationAtDownloadStart = 0;
  std::filesystem::path downloadRoot = "library-root";
  struct IndexRequest {
    std::filesystem::path path;
    std::string identity;
    std::uint64_t generation;
    std::vector<std::filesystem::path> removed;
  };
  std::vector<IndexRequest> indexed;
  int refreshes = 0;
  std::filesystem::path preferredBmsDownloadRoot() { return downloadRoot; }
  void refreshFindBmsModal() { assert(modal.owner == std::this_thread::get_id()); ++refreshes; }
  void enqueueDownloadedPathIndexTask(const std::filesystem::path &path,
      const std::string &identity, std::uint64_t generation,
      const std::vector<std::filesystem::path> &removed) {
    assert(modal.owner == std::this_thread::get_id());
    indexed.push_back({path, identity, generation, removed});
  }
  void showFindBmsModal(const ChartMetaRecord &record);
  void startFindBmsCandidateDownload(size_t index);
  void startFindBmsPendingArtifactResolution(BmsSearchPendingArtifactDecision decision);
  void hideFindBmsModal();
  void applyFindBmsUpdates();
  void cancelFindBms();
};

#include "find_bms_scene_methods.inc"

void applyUntilIdle(MainMenuScene &scene) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (scene.findBmsTask.running() && std::chrono::steady_clock::now() < deadline) {
    scene.applyFindBmsUpdates();
    std::this_thread::yield();
  }
  assert(!scene.findBmsTask.running());
}

void testImmediateArtifactCompletionKeepsActionsGatedUntilHandoff() {
  ServiceCalls calls;
  serviceCalls = &calls;
  MainMenuScene scene;
  scene.findBmsResult.pendingArtifact = BmsSearchPendingArtifact{.sourcePath = "staged.zip"};
  auto completed = startObserved(scene.findBmsTask, [](auto &, auto) {
    return BmsSearchResult{.status = BmsSearchResult::Status::HashMismatch,
                           .message = "Kept", .outputPath = "kept.zip"};
  });
  assert(completed.wait_for(5s) == std::future_status::ready);
  const auto policy = findBmsDialogPolicy(scene.findBmsTask.running(), scene.findBmsResult);
  assert(!policy.showPendingActions && !policy.canDismiss);
  scene.startFindBmsPendingArtifactResolution(BmsSearchPendingArtifactDecision::Delete);
  assert(calls.resolutions == 0);
  scene.applyFindBmsUpdates();
  assert(!scene.findBmsTask.running() && !scene.findBmsResult.pendingArtifact);
  assert(scene.indexed.size() == 1 && scene.indexed[0].path == "kept.zip");
  scene.applyFindBmsUpdates();
  assert(scene.indexed.size() == 1);
}

void testSceneLookupProgressAndIndexHandoff() {
  ServiceCalls calls;
  serviceCalls = &calls;
  Gate gate;
  calls.gate = &gate;
  calls.result = {.status = BmsSearchResult::Status::Downloaded,
                  .message = "Downloaded", .outputPath = "new.bms", .removedPaths = {"old.bms"}};
  MainMenuScene scene;
  scene.showFindBmsModal({{"sha256", "md5", "Title", "Artist"}});
  gate.wait();
  assert(calls.lookups == 1 && calls.sha256 == "sha256" && calls.md5 == "md5");
  assert(calls.title == "Title" && calls.artist == "Artist");
  assert(calls.root == "library-root" && calls.skipUnarchiving);
  assert(scene.modal.visible && scene.findBmsProgressCurrent == 0);
  scene.applyFindBmsUpdates();
  assert(scene.findBmsProgressCurrent == 40 && scene.findBmsProgressTotal == 100);
  assert(scene.findBmsProgressLog.size() == 2); // Repeated download lines coalesce.
  assert(scene.indexed.empty());
  scene.hideFindBmsModal();
  assert(scene.modal.visible);
  gate.release.set_value();
  assert(scene.indexed.empty());
  applyUntilIdle(scene);
  assert(scene.findBmsProgressFraction == 1 && scene.indexed.size() == 1);
  assert(scene.indexed[0].identity == "sha256" && scene.indexed[0].generation == 7);
  assert(scene.indexed[0].path == "new.bms" && scene.indexed[0].removed == calls.result.removedPaths);
  scene.applyFindBmsUpdates();
  assert(scene.indexed.size() == 1);
  scene.hideFindBmsModal();
  assert(!scene.modal.visible);
}

void testSceneCancellationKeepsPendingArtifactVisible() {
  ServiceCalls calls;
  serviceCalls = &calls;
  Gate gate;
  calls.gate = &gate;
  calls.result = {.status = BmsSearchResult::Status::HashMismatch,
                  .message = "Review downloaded files", .pendingArtifact = BmsSearchPendingArtifact{}};
  MainMenuScene scene;
  scene.showFindBmsModal({{"", "md5", "Title", "Artist"}});
  gate.wait();
  assert(scene.findBmsResult.fallbackUrl == "search/Title Artist");
  scene.cancelFindBms();
  assert(scene.findBmsTask.running());
  gate.release.set_value();
  applyUntilIdle(scene);
  assert(calls.cancellationObserved && scene.findBmsResult.pendingArtifact);
  scene.hideFindBmsModal();
  assert(scene.modal.visible && scene.indexed.empty());
}

void testSceneCandidateAndPendingArtifactDecisions() {
  ServiceCalls calls;
  serviceCalls = &calls;
  MainMenuScene scene;
  scene.findBmsModalChart = {{"sha256", "md5", "Title", "Artist"}};
  scene.findBmsResult.candidates = {{.id = "candidate-1"}};
  scene.startFindBmsCandidateDownload(1);
  assert(calls.downloads == 0);
  scene.startFindBmsCandidateDownload(0);
  applyUntilIdle(scene);
  assert(calls.downloads == 1 && calls.candidateId == "candidate-1");
  assert(calls.sha256 == "sha256" && calls.md5 == "md5" && calls.skipUnarchiving);

  scene.startFindBmsPendingArtifactResolution(BmsSearchPendingArtifactDecision::Keep);
  assert(calls.resolutions == 0);
  for (auto decision : {BmsSearchPendingArtifactDecision::Keep, BmsSearchPendingArtifactDecision::Delete}) {
    scene.findBmsResult.pendingArtifact = BmsSearchPendingArtifact{.sourcePath = "staged.zip"};
    const bool keep = decision == BmsSearchPendingArtifactDecision::Keep;
    calls.result = {.status = BmsSearchResult::Status::HashMismatch,
                    .message = keep ? "Kept" : "Deleted", .outputPath = keep ? "kept.zip" : ""};
    scene.startFindBmsPendingArtifactResolution(decision);
    applyUntilIdle(scene);
    assert(calls.decision == decision && calls.resolvedArtifact->sourcePath == "staged.zip");
    assert(!scene.findBmsPendingDecision && !scene.findBmsResult.pendingArtifact);
  }
  assert(calls.resolutions == 2 && scene.indexed.size() == 1);
  assert(scene.indexed[0].path == "kept.zip" && scene.indexed[0].identity.empty());
  assert(scene.indexed[0].generation == 0);
}
} // namespace
