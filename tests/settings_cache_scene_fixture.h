// Only views are replaced. The generated methods below are complete, unchanged
// production methods; the controller, workers, and filesystem operations are real.
#include <iomanip>
#include <sstream>

namespace {
struct SDL_Color { unsigned char r, g, b, a; };
struct CacheTextView {
  std::thread::id owner = std::this_thread::get_id();
  std::string text;
  SDL_Color color{};
  void setText(const std::string &value) {
    assert(owner == std::this_thread::get_id()); text = value;
  }
  void setColor(SDL_Color value) {
    assert(owner == std::this_thread::get_id()); color = value;
  }
};
struct CacheLayout {
  int layouts = 0;
  int refreshes = 0;
  void applyYogaLayout() { ++layouts; }
  void refreshContentLayout() { ++refreshes; }
};
class SettingsScene {
public:
  SettingsScene(Maintenance::Cleanup cleanup, Maintenance::Measure measure)
      : archiveCacheMaintenance(std::move(cleanup), std::move(measure)) {}
  Maintenance archiveCacheMaintenance;
  CacheTextView status, button;
  CacheTextView *archiveCacheCleanupStatusText = &status;
  CacheTextView *archiveCacheCleanupButtonText = &button;
  CacheLayout layout;
  CacheLayout *rootLayout = &layout;
  CacheLayout *scrollView = &layout;
  std::string archiveCacheCleanupStatusMessage;
  SDL_Color archiveCacheCleanupStatusColor{};
  void cleanupTemporaryArchiveCache();
  void measureTemporaryArchiveCache();
  void applyPendingArchiveCacheCleanupStatus();
};

#include "settings_cache_scene_methods.inc"

void testSceneAppliesTypedResultsOnTheApplicationThread() {
  CacheFixture fixture;
  fixture.write("active.mov", "keep");
  fixture.write("unused.mov", "remove");
  Gate cleanup;
  SettingsScene scene([&](auto &result, auto &error) {
    cleanup.block(); return fixture.cleanup()(result, error);
  }, fixture.measure());
  scene.measureTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  assert(scene.status.text.find("Measuring") != std::string::npos);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.status.text.find("10 B") != std::string::npos);
  assert(scene.layout.layouts == 1 && scene.layout.refreshes == 1);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.layout.layouts == 1);

  scene.cleanupTemporaryArchiveCache();
  cleanup.wait();
  assert(scene.button.text == "Cleaning...");
  scene.measureTemporaryArchiveCache();
  scene.cleanupTemporaryArchiveCache();
  assert(scene.status.text.find("Cleaning") != std::string::npos);
  cleanup.release.set_value();
  waitIdle(scene.archiveCacheMaintenance);
  assert(scene.button.text == "Cleaning...");
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.button.text == "Clean Up");
  assert(scene.status.text.find("Removed 6 B") != std::string::npos);
  assert(scene.status.text.find("1 active file") != std::string::npos);
  assert(scene.status.color.g > scene.status.color.r);
  assert(scene.layout.layouts == 2 && scene.layout.refreshes == 2);
}

void testSceneShowsOperationErrorsAndHandlesAbsentViews() {
  CacheFixture fixture;
  // An oversized single path component fails without requiring filesystem privileges.
  const auto invalidRoot = fixture.root / std::string(4096, 'x');
  SettingsScene scene([&](auto &result, auto &error) {
    return fixture.cache.cleanup(invalidRoot, result, {},
        [](const auto &path) { return path.generic_string(); }, &error);
  }, fixture.measure());
  scene.cleanupTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.status.text.find("cleanup failed:") != std::string::npos);
  assert(scene.status.color.r > scene.status.color.g);
  assert(scene.button.text == "Clean Up");
  scene.archiveCacheCleanupStatusText = nullptr;
  scene.archiveCacheCleanupButtonText = nullptr;
  scene.rootLayout = nullptr;
  scene.scrollView = nullptr;
  scene.measureTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.archiveCacheCleanupStatusMessage.find("empty") != std::string::npos);
}

void testSceneShowsThrownOperationErrorsAndAllowsRetry() {
  CacheFixture fixture;
  bool fail = true;
  SettingsScene scene([&](auto &result, auto &error) {
    if (fail) throw std::runtime_error("cache cleanup exception");
    return fixture.cleanup()(result, error);
  }, [&](auto &, auto &, const auto &) -> bool {
    throw std::runtime_error("cache measurement exception");
  });
  scene.cleanupTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  assert(scene.button.text == "Cleaning...");
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.status.text == "Archive cache cleanup failed: cache cleanup exception");
  assert(scene.button.text == "Clean Up" && scene.status.color.r > scene.status.color.g);
  scene.measureTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.status.text == "Archive cache measurement failed: cache measurement exception");
  fail = false;
  scene.cleanupTemporaryArchiveCache();
  waitIdle(scene.archiveCacheMaintenance);
  scene.applyPendingArchiveCacheCleanupStatus();
  assert(scene.status.text.find("empty") != std::string::npos);
  assert(scene.status.color.g > scene.status.color.r);
  assert(scene.layout.layouts == 3 && scene.layout.refreshes == 3);
}
} // namespace
