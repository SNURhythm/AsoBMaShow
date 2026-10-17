#include "scene/MainMenuPreviewController.h"
#include "targets.h"

#include <future>
#include <iostream>

namespace main_menu_library {
int chartSelectionGenerationAfter(int generation,
    const std::optional<ChartMetaRecord> &, const ChartMetaRecord &) {
  return generation + 1;
}
}

namespace archive_file {
bool isVirtualPath(const std::filesystem::path &path) {
  return path.string().starts_with("archive:");
}
void appendDebugLogLine(const std::string &) {}
}

std::string formatFindBmsBytes(std::uint64_t) { return {}; }

struct View {
  void onSelected() {}
  void freeImage() {}
  void setImageAsync(const std::filesystem::path &, bool) {}
  void setText(const std::string &) {}
  void setLocalizedText(const i18n::Text &) {}
};

struct RecyclerView {
  std::function<void(const ChartMetaRecord &, int)> onSelected;
  std::function<void(const ChartMetaRecord &, int)> onSelectedByPointer;
  View *getViewByIndex(int) { return nullptr; }
};

struct Context {
  struct Settings { bool archiveChartPreviewEnabled = false; } settings;
  struct MusicPlayer { int stops = 0; void Stop(std::string &) { ++stops; } } musicPlayer;
};

struct MainMenuScene {
  Context context;
  bool restoringTabSelection_ = false;
  bool pendingTab_ = false;
  bool selectorActive = true;
  bool selectorTabActive() const { return selectorActive; }
  std::atomic_bool willStart = false;
  int chartSelectionGeneration = 0;
  std::optional<ChartMetaRecord> selectedChartRecord;
  std::optional<std::filesystem::path> suppressPreviewForChartPath;
  struct { bool inProgress() const { return false; } } replayExportJob_;
  View view;
  View *jacketView = &view;
  RecyclerView recycler;
  RecyclerView *recyclerView = &recycler;
  MainMenuPreviewController *previewWorker_ = nullptr;
  PREVIEW_STATE_FIELDS
  std::atomic_bool active = false;
  std::atomic_bool playing = true;
  std::atomic_bool selected = true;
  std::atomic_bool concurrentCleanup = false;
  std::atomic_bool cleanupOnSelectionThread = false;
  std::thread::id selectionThread;

  void refreshRankingsButton() {}
  void refreshReplayAvailability(const ChartMetaRecord *) {}
  void refreshPlayOptionButtons() {}
  void refreshLongNoteModeButtons() {}
  void refreshAssistOptionButtons() {}
  void setPlayableChartActionsVisible(bool, bool = true) {}
  void refreshUnzipButtonForSelection(const ChartMetaRecord *) {}
  void setFindBmsButtonVisible(bool) {}
  void refreshStartButtonForActiveFolder() {}
  void clearSelectedChart() { selected = false; }
  void stopAndClearSelectedChart() {
    concurrentCleanup = active.load();
    cleanupOnSelectionThread = std::this_thread::get_id() == selectionThread;
    playing = false;
    clearSelectedChart();
  }

  MainMenuScene() {
    auto &context = this->context;
    const auto selectChart = [this, &context](const ChartMetaRecord &record, int idx,
                                            bool bypassDebounce)
        SELECTION_CALLBACK;
    SELECTION_WIRING
  }
};

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

void exerciseSelection(int kind, bool blocked) {
  MainMenuScene scene;
  std::mutex gate;
  std::condition_variable cv;
  bool entered = false;
  bool release = !blocked;
  bool nextLoaded = false;
  MainMenuPreviewController worker([&](const ChartMetaRecord &record, std::atomic_bool &) {
    std::lock_guard loadLock(scene.previewJukeboxLoadMutex);
    scene.active = true;
    std::unique_lock lock(gate);
    if (record.meta.Title == "old") {
      entered = true;
      cv.notify_all();
      cv.wait(lock, [&] { return release; });
    } else {
      nextLoaded = true;
      scene.playing = true;
      scene.selected = true;
    }
    scene.active = false;
    cv.notify_all();
  }, [&] {
    scene.stopAndClearSelectedChart();
    std::lock_guard lock(gate);
    cv.notify_all();
  }, std::chrono::milliseconds(0));
  scene.previewWorker_ = &worker;
  ChartMetaRecord old;
  old.meta.Title = "old";
  old.meta.BmsPath = "/songs/old.bms";
  worker.request(old);
  {
    std::unique_lock lock(gate);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return entered;
    }), "initial preview reaches the test barrier");
  }
  ChartMetaRecord selected;
  selected.meta.BmsPath = "/songs/new.bms";
  selected.unavailable = kind == 0;
  selected.solidArchive = kind == 1;
  selected.courseStart = kind == 2;
  if (kind == 3) selected.meta.BmsPath.clear();
  if (kind == 4) selected.meta.BmsPath = "archive:/pack.zip/chart.bms";
  if (kind == 5) scene.suppressPreviewForChartPath = selected.meta.BmsPath;
  scene.restoringTabSelection_ = kind == 7;
  scene.selectorActive = kind != 8;
  auto selection = std::async(std::launch::async, [&] {
    scene.selectionThread = std::this_thread::get_id();
    scene.recycler.onSelected(selected, 0);
  });
  const bool prompt = selection.wait_for(std::chrono::milliseconds(500)) ==
                      std::future_status::ready;
  {
    std::lock_guard lock(gate);
    release = true;
  }
  cv.notify_all();
  selection.get();
  expect(prompt, "selection must not wait for archive loading, kind=" +
                     std::to_string(kind));
  {
    std::unique_lock lock(gate);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return kind == 6 ? nextLoaded : !scene.playing.load();
    }), "selection eventually loads the new preview or cleans the old one");
  }
  expect(!scene.concurrentCleanup, "cleanup must not overlap a jukebox load");
  expect(!scene.cleanupOnSelectionThread, "cleanup stays off the selection thread");
  if (kind != 6) expect(!scene.selected, "skipped selections discard the old chart");
  if (kind == 7 || kind == 8)
    expect(scene.context.musicPlayer.stops == 0, "restoring or hidden selections preserve music playback");
  worker.stop();
}

void exerciseDirectSelectionTiming() {
  MainMenuScene scene;
  std::promise<void> loaded;
  auto ready = loaded.get_future();
  MainMenuPreviewController worker(
      [&](const auto &, auto &) { loaded.set_value(); }, [] {}, std::chrono::seconds(2));
  scene.previewWorker_ = &worker;
  ChartMetaRecord record;
  record.meta.BmsPath = "/songs/direct.bms";
  scene.recycler.onSelected(record, 0);
  expect(ready.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
         "ordinary selection keeps the preview debounce");
  scene.recycler.onSelectedByPointer(record, 0);
  expect(ready.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready,
         "the actual pointer selection callback bypasses the preview debounce");
  worker.stop();
}

int main() {
  exerciseDirectSelectionTiming();
  for (int kind = 0; kind <= 8; ++kind) {
    exerciseSelection(kind, false);
    exerciseSelection(kind, true);
  }
  return failures == 0 ? 0 : 1;
}
