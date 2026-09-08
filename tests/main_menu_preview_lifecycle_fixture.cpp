#include "scene/ChartPreloadWorker.h"
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
};

struct RecyclerView {
  std::function<void(const ChartMetaRecord &, int)> onSelected;
  View *getViewByIndex(int) { return nullptr; }
};

struct Context {
  struct Settings { bool archiveChartPreviewEnabled = false; } settings;
  struct MusicPlayer { void Stop(std::string &) {} } musicPlayer;
};

struct MainMenuScene {
  Context context;
  std::atomic_bool willStart = false;
  int chartSelectionGeneration = 0;
  std::optional<ChartMetaRecord> selectedChartRecord;
  std::optional<std::filesystem::path> suppressPreviewForChartPath;
  std::atomic_bool replayExportInProgress = false;
  View view;
  View *jacketView = &view;
  View *replayStatusText = nullptr;
  RecyclerView recycler;
  RecyclerView *recyclerView = &recycler;
  ChartPreloadWorker *previewWorker_;
  PREVIEW_STATE_FIELDS
  std::atomic_bool active = false;
  std::atomic_bool playing = true;
  std::atomic_bool selected = true;
  std::atomic_bool concurrentCleanup = false;
  std::atomic_bool cleanupOnSelectionThread = false;
  std::thread::id selectionThread;
  std::function<void()> onIdle;

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

  explicit MainMenuScene(ChartPreloadWorker &worker) : previewWorker_(&worker) {
    auto &context = this->context;
    recyclerView->onSelected = [this, &context](const ChartMetaRecord &item, int idx)
        SELECTION_CALLBACK;
    onIdle = [this]() IDLE_CALLBACK;
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
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  MainMenuScene scene(worker);
  std::mutex gate;
  std::condition_variable cv;
  bool entered = false;
  bool release = !blocked;
  bool nextLoaded = false;
  int idleCount = 0;
  worker.configure([&](const ChartMetaRecord &record, std::atomic_bool &) {
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
  });
  worker.setOnIdle([&] {
    scene.onIdle();
    std::lock_guard lock(gate);
    ++idleCount;
    cv.notify_all();
  });
  ChartMetaRecord old;
  old.meta.Title = "old";
  old.meta.BmsPath = "/songs/old.bms";
  worker.request(old);
  {
    std::unique_lock lock(gate);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return blocked ? entered : idleCount > 0;
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
  worker.stop();
}

int main() {
  for (int kind = 0; kind <= 6; ++kind) {
    exerciseSelection(kind, false);
    exerciseSelection(kind, true);
  }
  return failures == 0 ? 0 : 1;
}
