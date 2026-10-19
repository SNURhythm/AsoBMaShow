// Real scene ownership and Settings event dispatch; graphics and display
// services are substituted. Run with ASan to detect reads after Back deletes
// the dynamically allocated Settings scene, as Intro and selectors do.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace i18n {
std::uint64_t revision() { return 0; }
std::string tr(const char *key) { return key; }
}
using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks() { return 1; }
enum {
  SDL_EVENT_WILL_ENTER_BACKGROUND = 1, SDL_EVENT_DID_ENTER_BACKGROUND,
  SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_MINIMIZED,
  SDL_EVENT_WINDOW_HIDDEN, SDL_EVENT_MOUSE_BUTTON_UP
};
constexpr int SDL_EVENT_WINDOW_FIRST = SDL_EVENT_WINDOW_FOCUS_LOST;
constexpr int SDL_EVENT_WINDOW_LAST = SDL_EVENT_WINDOW_HIDDEN;
struct SDL_Event { int type = SDL_EVENT_MOUSE_BUTTON_UP; struct { int event = 0; } window; };
class SceneManager;
namespace audio::playback { struct BackendOperationResult { bool success = true; std::string diagnostic; }; }
namespace player_settings { enum class PresentationOrientation { Landscape, Portrait }; }
struct BackgroundTasks { void setGameplayPaused(bool) {} };
struct FolderEntry { std::filesystem::path path; std::string iosBookmark; };
struct ChartRepositoryFixture {
  std::vector<FolderEntry> entries;
  auto *OpenSession() { return this; }
  const auto &SelectAllEntries() const { return entries; }
};
struct ApplicationContext {
  ChartRepositoryFixture chartRepository;
  Uint64 currentFrame = 0;
  int uiBatchRenderer = 0;
  SceneManager *sceneManager = nullptr;
  std::atomic_bool backgroundTasksPausedForForegroundScene = false;
  std::atomic_bool storageMaintenanceActive = false;
  std::atomic_bool quitFlag = false;
  BackgroundTasks *chartLibraryTasks = nullptr;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;
  std::function<void(bool)> setGameplayOrientationLocked;
  std::function<void(bool)> setResponsiveMenuCanvas;
  int gameplayBgaCompositeState = 0;
  struct {
    int stops = 0;
    bool fail = false;
    bool SetStorageMaintenance(bool active, std::string &error) {
      if (active) { if (fail) { error = "native unload failed"; return false; } ++stops; } return true;
    }
  } musicPlayer;
  struct {
    int stops = 0;
    using Result = audio::playback::BackendOperationResult;
    bool fail = false;
    Result discardLoadedMedia(const std::function<Result()> &beforeDiscard) {
      if (fail) return {false, "backend busy"};
      auto result = beforeDiscard();
      if (result.success) ++stops;
      return result;
    }
  } jukebox;
  struct {
    struct { int video = 0; } audioVideo;
    player_settings::PresentationOrientation orientation = player_settings::PresentationOrientation::Landscape;
    auto activePresentationOrientation() const { return orientation; }
    void setActivePresentationOrientation(player_settings::PresentationOrientation value) { orientation = value; }
  } settings;
};
// Exercise real iOS storage admission with only repository and OS services
// substituted. Paths and symlink validation use the production implementations.
PRODUCTION_STORAGE_PATH_INDEX
namespace storage_management {
namespace fs = std::filesystem;
PRODUCTION_STORAGE_PLATFORM
PRODUCTION_SAFE_STORAGE_PATH
}
#undef TARGET_OS_IOS
#undef TARGET_OS_SIMULATOR
#undef TARGET_OS_ANDROID
#define TARGET_OS_IOS 1
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_ANDROID 0
namespace chart_library_platform {
std::filesystem::path resolveFolderEntryPath(const FolderEntry &entry) { return entry.path; }
}
namespace chart_storage_identity {
std::optional<std::filesystem::path> RebaseLegacyIOSDocumentsPath(
    const std::filesystem::path &, const std::filesystem::path &) { return {}; }
}
namespace fs = std::filesystem;
struct BookmarkResponse { fs::path resolved; bool handle = true; bool accessing = true; };
std::map<std::string, BookmarkResponse> bookmarkResponses;
int outstandingBookmarkHandles = 0;
fs::path privateStorageRoot;
std::string fspath_to_utf8(const fs::path &path) { return path.string(); }
std::string GetIOSDocumentsPath() { return (privateStorageRoot / "Documents").string(); }
void *StartIOSSecurityScopedResource(const std::string &path, const std::string &bookmark,
                                    std::string &resolved, std::string &error) {
  const auto &response = bookmarkResponses.at(bookmark);
  resolved = response.resolved.empty() ? path : response.resolved.string();
  error = response.accessing ? "" : "Failed to start folder access";
  if (!response.handle) { error = "Failed to resolve folder access"; return nullptr; }
  ++outstandingBookmarkHandles;
  return new int;
}
void StopIOSSecurityScopedResource(void *handle) {
  --outstandingBookmarkHandles;
  delete static_cast<int *>(handle);
}
namespace storage_management {
fs::path StoragePrivateRoot(ApplicationContext &) { return privateStorageRoot; }
PRODUCTION_STORAGE_ACCESS
std::function<bool(Checkpoint)> cleanupScans;
bool CleanupAbandonedStorageScans(const StoragePlatformPaths &, std::string &, Checkpoint check) {
  return cleanupScans(std::move(check));
}
void SDL_Log(const char *, ...) {}
PRODUCTION_STORAGE_STARTUP_CHECKPOINTS
bool admitStorageRoots(ApplicationContext &context, const std::vector<fs::path> &roots) {
  std::string error;
  auto access = accessFolders(context, error);
  if (!access) return false;
  PRODUCTION_STORAGE_ROOT_ADMISSION
  return true;
}
void testRecoveryCheckpointConsumesQueuedQuit() {
  storage_test::TempDirectory temp;
  privateStorageRoot = temp.root;
  ApplicationContext context;
  bool queuedQuit = false, stopped = false;
  unsigned notifications = 0;
  storage_management::cleanupScans = [&](storage_management::Checkpoint check) {
    queuedQuit = true;
    // A recovery phase can checkpoint repeatedly without publishing progress.
    stopped = !check();
    return !stopped;
  };
  const bool completed = storage_management::InitializeStorageServices(context,
      [&](const auto &) { ++notifications; }, [&] {
        if (queuedQuit) context.quitFlag = true;
      });
  assert(stopped && !completed && context.quitFlag && notifications == 1);
  storage_management::cleanupScans = {};
}
}
void testBookmarkScopeAdmission() {
  storage_test::TempDirectory temp;
  privateStorageRoot = temp.root / "private";
  fs::create_directory(privateStorageRoot);
  temp.write("library/child/song.wav");
  temp.write("library/sibling/song.wav");
  temp.write("library/child/archive.zip");
  temp.write("library-other/song.wav");
  const auto parent = temp.root / "library", child = parent / "child";
  bookmarkResponses = {{"active", {}}, {"stale", {{}, false, false}},
                       {"denied", {{}, true, false}}};
  ApplicationContext context;
  auto check = [&](std::vector<FolderEntry> entries, const fs::path &selected,
                   bool expected, const char *message) {
    context.chartRepository.entries = std::move(entries);
    storage_test::require(storage_management::admitStorageRoots(context, {selected}) == expected, message);
    storage_test::require(outstandingBookmarkHandles == 0, "all bookmark wrappers released after admission");
  };
  check({{parent, "stale"}, {child, "active"}}, child, true,
        "active child bookmark survives an unavailable parent registration");
  check({{parent, "stale"}, {child, "active"}}, child / "archive.zip", true,
        "local archive inherits its active child folder grant");
  check({{parent, "stale"}, {child, "active"}}, parent / "sibling", false,
        "child grant never authorizes an unavailable parent's sibling subtree");
  check({{parent, "active"}, {child, "stale"}}, child, true,
        "failed child bookmark does not revoke an active parent directory grant");
  for (bool reversed : {false, true}) {
    std::vector<FolderEntry> duplicate{{child, "active"}, {child / "", "stale"}};
    if (reversed) std::reverse(duplicate.begin(), duplicate.end());
    check(std::move(duplicate), child, true, "duplicate active grant wins independently of registration order");
  }
  check({{child, "active"}}, temp.root / "library-other", false,
        "component-prefix sibling is outside the active grant");
  check({{child, "denied"}}, child, false,
        "resolved wrapper with failed native access is not an active grant");
  check({{parent, "active"}, {child, "denied"}}, child, true,
        "active parent still covers a resolved child with failed access");
  check({{child, ""}}, child, true, "ordinary registered sandbox folder needs no bookmark");
  bookmarkResponses["moved"] = {child, true, true};
  check({{temp.root / "old", "moved"}, {parent, "stale"}}, child, true,
        "active scope is classified at its resolved current location");
  bookmarkResponses["moved-denied"] = {child, true, false};
  check({{temp.root / "old", "moved-denied"}}, child, false,
        "failed access is classified at its resolved current location");
  fs::create_directory_symlink(parent / "sibling", child / "link");
  check({{child, "active"}}, child / "link", false, "active grant does not bypass symlink validation");
}
#undef TARGET_OS_IOS
#undef TARGET_OS_SIMULATOR
#undef TARGET_OS_ANDROID
struct RenderContext {
  explicit RenderContext(int) {}
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct View {
  void cancelPointerInput() {}
  std::function<bool()> eventCallback;
  virtual ~View() = default;
  bool handleEvents(SDL_Event &) {
    auto callback = eventCallback;
    return callback ? callback() : true;
  }
  void propagateLanguageChange() {}
  void render(RenderContext &) {}
  static void dispatchTemporaryEventListeners(SDL_Event &) {}
  static void dispatchDeferredEventCallbacks() {}
};

PRODUCTION_SCENE_HEADER
PRODUCTION_MANAGER_HEADER
PRODUCTION_MANAGER_METHODS
PRODUCTION_RETURN_TARGET
PRODUCTION_RETURN_METHOD

struct DisplaySession {
  struct Result { std::string message; };
  int focusLosses = 0;
  bool hasDisplayPreview() const { return true; }
  std::optional<Result> onFocusLost() {
    ++focusLosses;
    return Result{"Display restored"};
  }
};
struct Color { int r, g, b, a; };
struct SettingsScene : Scene {
  using Scene::Scene;
  DisplaySession *audioVideoSession = nullptr;
  View *appearanceColorPopup = nullptr;
  int displayDraft = 0;
  bool previewActive = false;
  int previewEvents = 0;
  int previewInputCleanups = 0;
  int *destructions = nullptr;
  std::string displayStatus;
  int displayPreviewUpdates = 0;
  ~SettingsScene() override { if (destructions) ++*destructions; }
  void setDisplayStatus(const std::string &status, Color) { displayStatus = status; }
  void updateDisplayPreviewUi() { ++displayPreviewUpdates; }
  void forwardPreviewInputEvent(SDL_Event &) { ++previewEvents; }
  void destroyPreviewInputHandler() { ++previewInputCleanups; }
  void init() override { addView(new View); }
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {}
  EventHandleResult handleEvents(SDL_Event &event) override;
  void openStorage();
};

PRODUCTION_SETTINGS_EVENTS

struct StorageManagementScene : SettingsScene {
  SceneReturnTarget target;
  StorageManagementScene(ApplicationContext &context, SceneReturnTarget value)
      : SettingsScene(context), target(std::move(value)) {}
};
void SettingsScene::openStorage() {
  defer(PRODUCTION_STORAGE_OPEN, 0, true);
}

struct MaintenanceAccess { std::vector<std::shared_ptr<void>> scopes; };
auto environment = std::make_shared<MaintenanceAccess>();
bool imageReadsPaused = false;
std::shared_ptr<void> pauseImageReadsForStorageMaintenance() {
  imageReadsPaused = true;
  return std::shared_ptr<void>(&imageReadsPaused, [](void *) { imageReadsPaused = false; });
}

bool admitMedia(ApplicationContext &context) {
  std::string error;
  bool guard = true;
  PRODUCTION_MEDIA_ADMISSION
}
struct MaintenanceScene : SettingsScene {
  using SettingsScene::SettingsScene;
  bool busy = false;
  int releases = 0;
  bool canPrepareForStorageMaintenance(std::string &error) override {
    if (busy) error = "busy";
    return !busy;
  }
  bool prepareForStorageMaintenance(std::string &error) override {
    if (!canPrepareForStorageMaintenance(error)) return false;
    ++releases;
    return true;
  }
};
void testMaintenanceAdmission() {
  ApplicationContext context;
  SceneManager manager(context);
  auto first = std::make_unique<MaintenanceScene>(context);
  auto second = std::make_unique<MaintenanceScene>(context);
  auto *a = first.get();
  auto *b = second.get();
  manager.changeScene(std::move(first));
  manager.changeScene(std::move(second), true);
  a->busy = true; // retained Settings/library work rejects before any media is stopped
  assert(!admitMedia(context));
  assert(context.musicPlayer.stops == 0 && context.jukebox.stops == 0);
  assert(a->releases == 0 && b->releases == 0);
  assert(!imageReadsPaused && environment->scopes.empty());
  // Exercise both traversal orders and direct SceneManager callers too.
  std::string error;
  for (auto *busy : {a, b}) {
    a->busy = busy == a;
    b->busy = busy == b;
    assert(!manager.prepareForStorageMaintenance(error));
    assert(a->releases == 0 && b->releases == 0);
  }
  a->busy = b->busy = false;
  context.jukebox.fail = true;
  assert(!admitMedia(context));
  assert(context.musicPlayer.stops == 0);
  assert(a->releases == 0 && b->releases == 0);
  assert(!imageReadsPaused && environment->scopes.empty());
  context.jukebox.fail = false;
  context.jukebox.stops = 0;
  context.musicPlayer.fail = true;
  assert(!admitMedia(context));
  assert(context.jukebox.stops == 0 && context.musicPlayer.stops == 0);
  assert(a->releases == 0 && b->releases == 0);
  assert(!imageReadsPaused && environment->scopes.empty());
  context.musicPlayer.fail = false;
  assert(admitMedia(context));
  assert(context.musicPlayer.stops == 1 && context.jukebox.stops == 1);
  assert(a->releases == 1 && b->releases == 1);
  assert(imageReadsPaused && environment->scopes.size() == 1);
  environment->scopes.clear(); // The maintenance guard releases its access scopes.
  assert(!imageReadsPaused);
}
int main() {
  storage_management::testRecoveryCheckpointConsumesQueuedQuit();
  testBookmarkScopeAdmission();
  testMaintenanceAdmission();
  ApplicationContext context;
  int destructions = 0;
  SceneManager manager(context);
  manager.registerScene("Intro", std::make_unique<SettingsScene>(context));
  for (bool previewActive : {false, true}) {
    auto settings = std::make_unique<SettingsScene>(context);
    auto *active = settings.get();
    active->destructions = &destructions;
    active->previewActive = previewActive;
    manager.changeScene(std::move(settings));
    SDL_Event event;
    manager.handleEvents(event);
    assert(active->previewEvents == (previewActive ? 1 : 0));
    active->views.front()->eventCallback = [] { return false; };
    manager.handleEvents(event);
    assert(active->previewEvents == (previewActive ? 1 : 0) &&
           "consumed events do not reach the gameplay preview");
    DisplaySession display;
    active->audioVideoSession = &display;
    context.settings.audioVideo.video = 42;
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    manager.handleEvents(event);
    assert(display.focusLosses == 1 && active->displayDraft == 42);
    assert(active->displayStatus == "Display restored");
    assert(active->displayPreviewUpdates == 1);
    assert(active->previewInputCleanups == (previewActive ? 1 : 0));
    assert(active->previewEvents == (previewActive ? 1 : 0));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    active->views.front()->eventCallback = [&manager] {
      manager.changeScene("Intro");
      return false;
    };
    manager.handleEvents(event);
    assert(manager.currentScene == manager.registeredScenes.at("Intro").get());
  }
  assert(destructions == 2 && "Back deletes each dynamic Settings scene");
  auto settings = std::make_unique<SettingsScene>(context);
  auto *retained = settings.get();
  manager.changeScene(std::move(settings));
  retained->openStorage();
  ++context.currentFrame;
  manager.handleDeferred();
  assert(dynamic_cast<StorageManagementScene *>(manager.currentScene));
  assert(manager.hasBackgroundScene(retained));
  assert(returnToScene(manager, SceneReturnTarget::Retained(retained)));
  manager.handleDeferred();
  assert(manager.currentScene == retained && "Back must not replay the storage-opening callback");
}
