// Complete production destruction/cleanup methods with controlled resources.
#include "REPOSITORY_ROOT/src/scene/ReplayRecordTask.h"
#include "REPOSITORY_ROOT/src/replay/ReplayExportJob.h"
#include <algorithm>
#include <cassert>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
#define TARGET_OS_IOS 1
#define TARGET_OS_SIMULATOR 0
using Uint64 = unsigned long long;
void SDL_Log(const char *, ...) {}
struct Evidence {
  std::vector<std::string> events;
  std::atomic_bool recordsEntered{false}, launchEntered{false}, exportEntered{false};
  std::atomic_bool preloadEntered{false}, skinEntered{false};
  std::atomic_bool recordsStopped{false}, launchStopped{false}, exportStopped{false};
  std::atomic_bool preloadStopped{false}, skinStopped{false};
  int views = 0, preloads = 0, scopedAccess = 0;
  void event(std::string name) { events.push_back(std::move(name)); }
};
struct Resource {
  Evidence &e;
  std::string name;
  Resource(Evidence &e, std::string name) : e(e), name(std::move(name)) {}
  virtual ~Resource() { e.event(name); }
  void close() { e.event(name + ":close"); }
  void stop() { e.event(name + ":stop"); }
  void cancel() { e.event(name + ":cancel"); }
  void reset() { e.event(name + ":reset"); }
};
struct View : Resource {
  using Resource::Resource;
  ~View() override { ++e.views; }
  void endEditing() { e.event(name + ":end"); }
  void setText(const std::string &) {}
  void setVisible(bool) {}
  void dismiss(View *) { e.event(name + ":dismiss"); }
};
struct Registry {
  Evidence &e;
  void unsubscribe(std::uint64_t id) { e.event("input:" + std::to_string(id)); }
};
struct Ranking : Resource {
  using Resource::Resource;
  void close(std::uint64_t id) { e.event("ranking:" + std::to_string(id)); }
};
struct ApplicationContext {
  Registry inputDeviceRegistry;
  std::unique_ptr<Ranking> irRankingService;
};
void StopIOSSecurityScopedResource(void *handle) {
  ++static_cast<Evidence *>(handle)->scopedAccess;
}
class Scene {
public:
  explicit Scene(ApplicationContext &context) : context(context) {}
  virtual ~Scene() BASE_DESTRUCTOR
  void cleanup() BASE_CLEANUP
  std::vector<View *> views;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
protected:
  virtual void cleanupScene() = 0;
  ApplicationContext &context;
private:
  void destroyOwnedViews() BASE_DESTROY_VIEWS
  void clearPostedDeferred() BASE_CLEAR_POSTED
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false, isCleaned = false;
};
struct Chart : Resource { using Resource::Resource; };
class ChartPreloadWorker {
public:
  ChartPreloadWorker(Evidence &e, std::function<void(std::stop_token)> work)
      : e(e), worker(std::move(work)) {}
  void stop() {
    if (worker.joinable()) { worker.request_stop(); worker.join(); }
  }
  ~ChartPreloadWorker() { stop(); ++e.preloads; e.event("preload:delete"); }
private:
  Evidence &e;
  std::jthread worker;
};
class MusicSelectScene final : public Scene {
public:
  explicit MusicSelectScene(ApplicationContext &context, Evidence &e)
      : Scene(context), e(e), previewController_(e, "preview-controller"),
        skinTouchGesture_(e, "gesture") {}
  ~MusicSelectScene() override;
  void cleanupScene() override;
  void cancelDirectoryLoad();
  void showDirectoryStatus(std::string message) { directoryStatusMessage_ = std::move(message); }
  void stopPreloadWorker();
  void stopInputListening();
  void cancelSelectedChartAnalysis();
  void cancelSkinPreparation();
  Evidence &e;
  ReplayRecordTask recordsTask_;
  std::unique_ptr<Resource> recordFileActions_, archiveUnzipModal_, directoryLoader_, folderStatusLoader_;
  bool sceneActive_ = false;
  std::uint64_t launchGeneration_ = 0;
  std::atomic_bool launchCancelled_{false};
  std::jthread launchThread_;
  ChartPreloadWorker *preloadWorker_ = nullptr;
  std::mutex preloadMutex_;
  std::unique_ptr<Chart> preloadedChart_;
  std::string preloadedPath_;
  std::optional<int> directoryRequest_, restoreSelection_;
  std::vector<int> restoreDirectories_, restoreDirectoryBars_;
  View *searchInput_ = nullptr, *decideOverlay_ = nullptr, *modalOverlayPortal_ = nullptr;
  View *skinTextInput_ = nullptr;
  Resource previewController_, skinTouchGesture_;
  std::unique_ptr<Resource> inputBindingAdapter_;
  std::uint64_t inputSubscription_ = 0, inputDeviceSubscription_ = 0, rankingGeneration_ = 0;
  std::unique_ptr<Resource> irExternalUrlService_;
  std::uint64_t irExternalUrlGeneration_ = 0;
  std::unique_ptr<Resource> previewAudio_, systemSound_, skinSession_, selectedChartInformation_;
  void *soundSetFolderAccessHandle_ = nullptr;
  View *skinLoadingView_ = nullptr;
  std::optional<int> activeSkinStringWriter_, chartSession_;
  std::map<int, View *> skinTextInputs_;
  std::unique_ptr<Resource> playOptionsModal_, recordsModal_;
  View *toolbar_ = nullptr, *searchOverlay_ = nullptr, *modalLayer_ = nullptr;
  replay::ReplayExportJob recordsExportJob_;
  View *tasksModal_ = nullptr, *tasksModalText_ = nullptr, *errorView_ = nullptr, *directoryStatus_ = nullptr;
  std::string directoryStatusMessage_;
  std::vector<std::string> diagnostics_;
  std::future<int> skinPreparation_;
  std::stop_source skinPreparationStop_;
  struct Analysis { std::atomic_bool cancelled{false}; };
  std::shared_ptr<Analysis> selectedChartAnalysis_;
  std::uint64_t selectedChartAnalysisGeneration_ = 0;
};
SCENE_METHODS

static void waitFor(const std::atomic_bool &flag) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!flag.load()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}
static void before(const Evidence &e, const std::string &a, const std::string &b) {
  const auto first = std::find(e.events.begin(), e.events.end(), a);
  const auto second = std::find(e.events.begin(), e.events.end(), b);
  assert(first != e.events.end() && second != e.events.end() && first < second);
}
static void activeDestruction(bool cleanupFirst, bool initializationFails = false) {
  Evidence e;
  ApplicationContext context{{e}, std::make_unique<Ranking>(e, "ranking-service")};
  auto scene = std::make_unique<MusicSelectScene>(context, e);
  auto &s = *scene;
  auto resource = [&](const char *name) { return std::make_unique<Resource>(e, name); };
  s.recordFileActions_ = resource("file-actions");
  s.directoryLoader_ = resource("directory");
  s.folderStatusLoader_ = resource("folder-status");
  s.archiveUnzipModal_ = resource("archive-modal");
  s.previewAudio_ = resource("preview-audio");
  s.systemSound_ = resource("system-sound");
  s.irExternalUrlService_ = resource("external-url");
  s.inputBindingAdapter_ = resource("input-adapter");
  s.skinSession_ = resource("skin");
  s.recordsModal_ = resource("records-modal");
  s.inputSubscription_ = 17;
  s.inputDeviceSubscription_ = 18;
  s.rankingGeneration_ = 19;
  s.soundSetFolderAccessHandle_ = &e;
  s.preloadedChart_ = std::make_unique<Chart>(e, "chart");
  s.preloadedPath_ = "selected.bms";
  s.diagnostics_ = {"alive"};
  s.searchInput_ = new View(e, "search");
  s.skinTextInput_ = new View(e, "skin-input");
  s.modalOverlayPortal_ = new View(e, "portal");
  s.decideOverlay_ = new View(e, "decide");
  s.views = {s.searchInput_, s.skinTextInput_, s.modalOverlayPortal_};
  s.selectedChartAnalysis_ = std::make_shared<MusicSelectScene::Analysis>();
  auto analysis = s.selectedChartAnalysis_;
  auto completionLifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weakCompletion = completionLifetime;
  s.deferred[0].second.push_back([completionLifetime] { assert(false); return false; });
  completionLifetime.reset();
  s.recordsTask_.start([&](auto cancelled) {
    e.recordsEntered = true;
    while (!cancelled->load()) std::this_thread::yield();
    assert(s.recordFileActions_ && s.previewAudio_);
    s.recordsTask_.publish([] { assert(false); });
    e.recordsStopped = true;
  });
  s.launchThread_ = std::jthread([&](std::stop_token stop) {
    e.launchEntered = true;
    while (!stop.stop_requested()) std::this_thread::yield();
    assert(s.launchCancelled_ && s.preloadedChart_);
    e.launchStopped = true;
  });
  s.preloadWorker_ = new ChartPreloadWorker(e, [&](std::stop_token stop) {
    e.preloadEntered = true;
    while (!stop.stop_requested()) std::this_thread::yield();
    std::lock_guard lock(s.preloadMutex_);
    assert(s.preloadedChart_ && s.preloadedPath_ == "selected.bms");
    e.preloadStopped = true;
  });
  s.skinPreparation_ = std::async(std::launch::async, [&] {
    e.skinEntered = true;
    while (!s.skinPreparationStop_.stop_requested()) std::this_thread::yield();
    assert(s.skinSession_ && s.inputBindingAdapter_);
    e.skinStopped = true;
    return 1;
  });
  assert(s.recordsExportJob_.tryBegin());
  s.recordsExportJob_.start({}, [&](const auto &, auto &cancelled) {
    e.exportEntered = true;
    while (!cancelled.load()) std::this_thread::yield();
    assert(s.recordsModal_ && s.diagnostics_.at(0) == "alive");
    e.exportStopped = true;
    return ReplayVideoExportResult{};
  });
  for (auto *flag : {&e.recordsEntered, &e.launchEntered, &e.preloadEntered,
                     &e.skinEntered, &e.exportEntered}) waitFor(*flag);
  if (cleanupFirst) {
    s.cleanup();
    const auto events = e.events;
    s.cleanup();
    assert(e.events == events);
  }
  if (initializationFails) {
    try {
      auto pendingScene = std::move(scene);
      throw std::runtime_error("initialization failed after starting work");
    } catch (const std::runtime_error &) {
    }
  }
  scene.reset();
  assert(e.recordsStopped && e.launchStopped && e.preloadStopped && e.skinStopped && e.exportStopped);
  assert(e.views == 4 && e.preloads == 1 && e.scopedAccess == 1);
  assert(analysis->cancelled && weakCompletion.expired());
  assert(std::count(e.events.begin(), e.events.end(), "input:17") == 1);
  assert(std::count(e.events.begin(), e.events.end(), "ranking:19") == 1);
  before(e, "directory:cancel", "directory");
  before(e, "chart", "preload:delete");
  before(e, "search:end", "search");
  before(e, "portal:dismiss", "decide");
  before(e, "skin-input:end", "skin-input");
  before(e, "input:17", "input-adapter");
  before(e, "input:18", "input-adapter");
  before(e, "external-url:stop", "external-url");
  before(e, "preview-controller:reset", "preview-audio");
  before(e, "skin", "skin-input");
  before(e, "records-modal", "search");
}
int main() {
  activeDestruction(false);
  activeDestruction(true);
  activeDestruction(false, true);
  Evidence e;
  ApplicationContext context{{e}, {}};
  { MusicSelectScene neverInitialized(context, e); }
  assert(e.views == 0 && e.preloads == 0 && e.scopedAccess == 0);
}
