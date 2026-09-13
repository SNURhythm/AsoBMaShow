// Production resource acquisition/release with controlled media and UI effects.
#include <atomic>
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
using SDL_FingerID = long long;
namespace rendering { constexpr int window_width = 800, window_height = 600; }
struct MusicTrack { std::string id = "track"; };
struct Evidence { std::vector<std::string> events; int refreshes = 0, views = 0; };
struct Jukebox {
  Evidence &e;
  bool visualsEnabled = false;
  void stop() { e.events.push_back("stop"); }
  void unloadVisuals() { e.events.push_back("unload"); }
  bool getVisualsEnabled() const { return visualsEnabled; }
  void setVisualsEnabled(bool enabled) {
    e.events.push_back(enabled ? "visuals:on" : "visuals:off");
    visualsEnabled = enabled;
  }
};
struct NativeMusic {
  struct Playback { bool loaded = true; };
  Playback playback;
  std::optional<MusicTrack> current = MusicTrack{};
  Playback PlaybackState() const { return playback; }
  std::optional<MusicTrack> CurrentTrackSnapshot() const { return current; }
  void PlayCurrentAsync(std::string &, const char *) { playback.loaded = true; }
};
struct ApplicationContext {
  Evidence e;
  Jukebox jukebox{e};
  NativeMusic musicPlayer;
  std::atomic_bool ignoreBgaPostOptions{false};
};
struct View {
  explicit View(Evidence &e) : e(e) {}
  virtual ~View() { ++e.views; e.events.push_back("view"); }
  void setVisible(bool) {}
  void setSize(int, int) {}
  void applyYogaLayout() {}
  void freeImage() { e.events.push_back("free-image"); }
  Evidence &e;
};
struct Chart {
  explicit Chart(Evidence &e) : e(e) {}
  ~Chart() { e.events.push_back("chart"); }
  Evidence &e;
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
class MusicPlayerScene final : public Scene {
public:
  explicit MusicPlayerScene(ApplicationContext &context) : Scene(context) {}
  ~MusicPlayerScene() override;
  void watchVideo();
  void cleanupScene() override;
  void exitVideoFullscreen();
  std::optional<MusicTrack> displayTrack() const { return fallback; }
  std::string favoriteKeyForTrack(const MusicTrack &track) {
    if (failAfterVisualOverride) throw std::runtime_error("track key unavailable");
    return track.id;
  }
  void setStatus(std::string) {}
  void refreshActiveQueueList(bool) {}
  void playNowPlaying(std::vector<MusicTrack>, int, const char *, const char *) {}
  void showVideoControls(Uint64 = 4000) {}
  void updateVideoFullscreen() {}
  void refreshVideoOverlay() {}
  void refreshUi() { ++context.e.refreshes; }
  bool loadVideoVisualsForTrack(const MusicTrack &, bool) {
    if (failLoading) throw std::runtime_error("video load unavailable");
    if (hasBga) {
      videoChart = std::make_unique<Chart>(context.e);
      videoVisualsLoaded = true;
    }
    return hasBga;
  }
  void buildControlledViews() {
    rootLayout = new View(context.e);
    videoOverlayRoot = new View(context.e);
    videoArtworkImage = new View(context.e);
    views = {rootLayout, videoOverlayRoot, videoArtworkImage};
  }
  bool failAfterVisualOverride = false, failLoading = false, hasBga = true;
  std::optional<MusicTrack> fallback = MusicTrack{};
  VIEW_HANDLES
  std::string displayedLibraryArtworkPath, displayedFavoritesArtworkPath,
              displayedArtworkPath, displayedVideoArtworkPath, displayedQueueName;
  bool seekMouseDown = false, videoSeekMouseDown = false;
  SDL_FingerID activeSeekTouchId = -1, activeVideoSeekTouchId = -1;
  bool videoFullscreenActive = false, videoVisualsLoaded = false,
       videoShowingArtwork = false, videoRestoresVisualsEnabled = false,
       videoPreviousVisualsEnabled = true, videoControlsVisible = false,
       playbackModeDropdownOpen = false;
  Uint64 videoControlsVisibleUntil = 0;
  std::string videoTrackId;
  std::unique_ptr<Chart> videoChart;
};
PLAYER_METHODS

void testDirectDestruction(bool previousVisuals, bool hasBga, bool cleanupFirst) {
  ApplicationContext context;
  context.jukebox.visualsEnabled = previousVisuals;
  auto scene = std::make_unique<MusicPlayerScene>(context);
  scene->buildControlledViews();
  scene->hasBga = hasBga;
  scene->watchVideo();
  assert(scene->videoFullscreenActive && scene->videoRestoresVisualsEnabled);
  assert(scene->videoVisualsLoaded == hasBga);
  assert(context.jukebox.visualsEnabled && context.ignoreBgaPostOptions);
  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weakLifetime = lifetime;
  scene->deferred[0].second.push_back([lifetime] { assert(false); return false; });
  lifetime.reset();
  context.e.events.clear();
  if (cleanupFirst) {
    scene->cleanup();
    const auto events = context.e.events;
    scene->cleanup();
    assert(events == context.e.events);
  }
  scene.reset();
  std::vector<std::string> expected{"unload", previousVisuals ? "visuals:on" : "visuals:off"};
  if (hasBga) expected.push_back("chart");
  expected.insert(expected.end(), 3, "view");
  assert(context.e.events == expected);
  assert(!context.ignoreBgaPostOptions && context.jukebox.visualsEnabled == previousVisuals);
  assert(context.musicPlayer.playback.loaded && context.e.refreshes == 0);
  assert(weakLifetime.expired());
}
void testUnwindPartialAcquisition(bool beforeFullscreen) {
  ApplicationContext context;
  try {
    auto scene = std::make_unique<MusicPlayerScene>(context);
    scene->failAfterVisualOverride = beforeFullscreen;
    scene->failLoading = !beforeFullscreen;
    scene->watchVideo();
    assert(false);
  } catch (const std::runtime_error &) {
  }
  const std::vector<std::string> expected = beforeFullscreen
      ? std::vector<std::string>{"stop", "visuals:on", "visuals:off"}
      : std::vector<std::string>{"stop", "visuals:on", "unload", "visuals:off"};
  assert(context.e.events == expected);
  assert(!context.jukebox.visualsEnabled && !context.ignoreBgaPostOptions);
  assert(context.e.refreshes == 0);
}
void testUnownedDestructionPreservesOtherPlayback(int priorState) {
  ApplicationContext context;
  auto scene = std::make_unique<MusicPlayerScene>(context);
  if (priorState != 0) scene->buildControlledViews();
  if (priorState >= 2) {
    scene->watchVideo();
    if (priorState == 2) scene->exitVideoFullscreen();
    else scene->cleanup();
  } else if (priorState == 1) {
    context.musicPlayer.current.reset();
    scene->fallback.reset();
    scene->watchVideo(); // No selection: no video resources were acquired.
  }
  // Another playback owner changes shared policy after exit or before this
  // unused scene is destroyed. Destruction must leave that state untouched.
  context.ignoreBgaPostOptions = true;
  context.jukebox.visualsEnabled = false;
  const int refreshes = context.e.refreshes;
  context.e.events.clear();
  scene.reset();
  assert(context.ignoreBgaPostOptions && !context.jukebox.visualsEnabled);
  assert(context.e.refreshes == refreshes);
  const int remainingViews = priorState == 1 || priorState == 2 ? 3 : 0;
  assert(context.e.events == std::vector<std::string>(remainingViews, "view"));
}
int main() {
  for (const bool previous : {false, true}) {
    for (const bool bga : {false, true}) {
      testDirectDestruction(previous, bga, false);
      testDirectDestruction(previous, bga, true);
    }
  }
  testUnwindPartialAcquisition(false);
  testUnwindPartialAcquisition(true);
  for (int state = 0; state < 4; ++state) testUnownedDestructionPreservesOtherPlayback(state);
}
