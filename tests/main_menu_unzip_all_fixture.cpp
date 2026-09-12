#include "REPOSITORY_ROOT/src/repositories/ChartRepository.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace archive_file {
bool isVirtualPath(const std::filesystem::path &) { return false; }
}

struct Text {
  std::string text;
  void setText(const std::string &value) { text = value; }
};

struct Modal {
  int prompts = 0;
  int singleStarts = 0;
  bool active = false;
  bool visible = false;
  bool startAll() {
    if (visible) return false;
    ++prompts;
    visible = true;
    return true;
  }
  bool start(const ChartMetaRecord &) { ++singleStarts; return true; }
};

struct Rows {
  std::vector<ChartMetaRecord> records;
  int selectedIndex = -1;
  int size() const { return static_cast<int>(records.size()); }
  const ChartMetaRecord &get(int index) const { return records.at(index); }
};

struct MainMenuScene {
  std::atomic_bool willStart = false, replayExportInProgress = false;
  std::optional<std::filesystem::path> pendingSelectChartPath;
  struct {
    std::atomic_bool chartLibraryListReloadRequested = false;
    std::atomic_bool chartLibraryFoldersReloadRequested = false;
  } context;
  Rows rows;
  Rows *recyclerView = &rows;
  Modal modal;
  Modal *archiveUnzipModal_ = &modal;
  Text button, status;
  Text *unzipButtonText = &button, *replayStatusText = &status;
  struct Preview { void stop() {} };
  Preview *previewWorker_ = nullptr;
  std::mutex previewCleanupMutex;
  bool pendingStopAndClearSelectedChartAfterPreview = false;
  bool buttonVisible = false;
  void stopAndClearSelectedChart() {}
  bool archiveUnzipInProgress() const { return modal.active; }
  void setUnzipButtonVisible(bool visible) { buttonVisible = visible; }
  void refreshUnzipButtonForSelection(const ChartMetaRecord *record);
  void startUnzipSelectedArchiveFolder();
  void startUnzipArchiveFolder(const ChartMetaRecord &record);
};

SCENE_METHODS

int main() {
  MainMenuScene scene;
  ChartMetaRecord action;
  action.unzipAll = true;
  action.solidArchive = true;
  scene.refreshUnzipButtonForSelection(&action);
  assert(scene.buttonVisible && scene.button.text == "Unzip All");
  assert(scene.modal.prompts == 0 && scene.modal.singleStarts == 0);
  scene.rows.records = {action};
  scene.rows.selectedIndex = 0;
  scene.startUnzipSelectedArchiveFolder();
  assert(scene.modal.prompts == 1 && scene.modal.singleStarts == 0);
  scene.startUnzipSelectedArchiveFolder();
  assert(scene.modal.prompts == 1);
  scene.modal.visible = false;
  scene.modal.active = true;
  scene.startUnzipSelectedArchiveFolder();
  assert(scene.modal.prompts == 1);
  scene.modal.active = false;
  action.unavailable = true;
  scene.startUnzipArchiveFolder(action);
  assert(scene.modal.prompts == 1);
  ChartMetaRecord single;
  single.solidArchive = true;
  single.meta.BmsPath = "/songs/archive.7z";
  scene.refreshUnzipButtonForSelection(&single);
  assert(scene.buttonVisible && scene.button.text == "Unzip");
  scene.startUnzipArchiveFolder(single);
  assert(scene.modal.singleStarts == 1 && scene.modal.prompts == 1);
}
