// Complete production cleanup methods with controlled audio and view lifetimes.
#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using Uint64 = unsigned long long;
struct Jukebox {
  std::vector<std::string> events;
  bool playing = true;
  void stop() { events.push_back("stop"); playing = false; }
};
struct ApplicationContext { Jukebox jukebox; };
struct View {
  explicit View(Jukebox &audio) : audio(audio) {}
  virtual ~View() { audio.events.push_back("view"); }
  Jukebox &audio;
};
class Scene {
public:
  explicit Scene(ApplicationContext &context) : context(context) {}
  BASE_METHODS
  std::vector<View *> views;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
protected:
  virtual void cleanupScene() = 0;
  ApplicationContext &context;
private:
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false, isCleaned = false;
};
struct Chart {
  Chart(Jukebox &audio, bool requiresStop) : audio(audio), requiresStop(requiresStop) {}
  ~Chart() {
    assert(!requiresStop || !audio.playing);
    audio.events.push_back("chart");
  }
  Jukebox &audio;
  bool requiresStop;
};
class ChartViewerScene final : public Scene {
public:
  explicit ChartViewerScene(ApplicationContext &context) : Scene(context) {}
  ~ChartViewerScene() override;
  void cleanupScene() override;
  std::unique_ptr<Chart> chart;
  bool listenActive = false, listenAudioLoaded = false, retainedListenResourcesForReload = false;
  long long listenEndMicros = 0;
  std::vector<int> randomOptions, ghostReplaySummaries, practiceNamedPresets;
  std::optional<int> practiceGhostReplay, selectedPracticePresetId;
  std::unique_ptr<int> practicePresetStore;
  int loadedGhostReplayId = -1, selectedGhostReplayIndex = -1;
  long long practiceChartEndMicros = 0;
  View *rootLayout = nullptr, *canvasView = nullptr, *titleText = nullptr,
       *subtitleText = nullptr, *statusText = nullptr, *randomSummaryText = nullptr,
       *zoomText = nullptr, *selectionText = nullptr, *listenPauseText = nullptr,
       *listenPauseButton = nullptr, *listenStopButton = nullptr, *ghostLoadButton = nullptr,
       *ghostLoadButtonText = nullptr, *ghostClearButton = nullptr,
       *ghostClearButtonText = nullptr, *ghostModalRoot = nullptr,
       *ghostModalEmptyText = nullptr, *practiceGhostReplayButton = nullptr,
       *practiceGhostReplayItem = nullptr, *ghostReplayListView = nullptr,
       *optionsDrawerRoot = nullptr, *viewerOptionText = nullptr,
       *viewerPlayOptionsPanel = nullptr, *randomDrawerRoot = nullptr,
       *randomDrawerScroll = nullptr, *overlayPortal = nullptr, *practicePanel = nullptr;
};
constexpr int kNoGhostReplayId = -1;
VIEWER_METHODS

void checkLifetime(int resources, bool cleanupFirst, bool initializationFails) {
  ApplicationContext context;
  auto scene = std::make_unique<ChartViewerScene>(context);
  scene->chart = std::make_unique<Chart>(context.jukebox, resources != 0);
  scene->listenActive = resources & 1;
  scene->listenAudioLoaded = resources & 2;
  scene->retainedListenResourcesForReload = resources & 4;
  scene->listenEndMicros = 123;
  scene->rootLayout = new View(context.jukebox);
  scene->views.push_back(scene->rootLayout);
  auto callbackLifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weakCallback = callbackLifetime;
  scene->deferred[0].second.push_back([callbackLifetime] { assert(false); return false; });
  callbackLifetime.reset();
  if (cleanupFirst) {
    scene->cleanup();
    const auto events = context.jukebox.events;
    assert(!scene->chart && !scene->rootLayout && scene->views.empty());
    assert(!scene->listenActive && !scene->listenAudioLoaded && !scene->retainedListenResourcesForReload);
    scene->cleanup();
    assert(context.jukebox.events == events);
  }
  if (initializationFails) {
    try {
      auto pending = std::move(scene);
      throw std::runtime_error("viewer initialization failed");
    } catch (const std::runtime_error &) {
    }
  }
  scene.reset();
  assert(weakCallback.expired());
  const std::vector<std::string> expected = resources == 0
      ? std::vector<std::string>{"chart", "view"}
      : std::vector<std::string>{"stop", "chart", "view"};
  assert(context.jukebox.events == expected);
  assert(context.jukebox.playing == (resources == 0));
}
int main() {
  for (int resources = 0; resources < 8; ++resources) {
    checkLifetime(resources, false, false);
    checkLifetime(resources, true, false);
    checkLifetime(resources, false, true);
  }
  ApplicationContext context;
  { ChartViewerScene neverInitialized(context); }
  assert(context.jukebox.playing && context.jukebox.events.empty());
}
