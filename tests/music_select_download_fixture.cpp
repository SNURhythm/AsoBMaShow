#include "REPOSITORY_ROOT/src/music_select/MusicSelectTypes.h"
#include "REPOSITORY_ROOT/src/BmsSearchService.h"
#include "REPOSITORY_ROOT/src/library/ChartLibraryTaskTypes.h"
#include <atomic>
#include <cassert>

TOOLBAR_ENUM
MODAL_CALLBACKS
RECORDS_TARGET

struct View {};
struct FindBmsModal {
  FindBmsModalCallbacks callbacks;
  ChartMetaRecord shown;
  bool visible = false, confirmation = false;
  int shows = 0;
  static std::unique_ptr<FindBmsModal> Create(View *parent, FindBmsModalCallbacks callbacks) {
    assert(parent);
    auto result = std::make_unique<FindBmsModal>();
    result->callbacks = std::move(callbacks);
    return result;
  }
  void show(const ChartMetaRecord &record, bool requireConfirmation = false) {
    shown = record; confirmation = requireConfirmation; visible = true; ++shows;
  }
  bool isVisible() const { return visible; }
};
struct TaskService {
  std::vector<chart_library_tasks::TaskRequest> requests;
  void enqueue(chart_library_tasks::TaskRequest request) { requests.push_back(std::move(request)); }
};
static int rootCalls = 0;
std::filesystem::path findBmsDownloadRoot(void *) { ++rootCalls; return "download-root"; }
main_menu_library::FindBmsChartIdentity main_menu_library::findBmsChartIdentity(
    const bms_parser::ChartMeta &meta) { return {.sha256 = meta.SHA256, .md5 = meta.MD5}; }

struct Toolbar {
  struct Control { MusicSelectToolbarControl control; };
  std::vector<Control> entries{{MusicSelectToolbarControl::ChartMenu},
                              {MusicSelectToolbarControl::ChartViewer},
                              {MusicSelectToolbarControl::ChartRecords}};
  std::map<MusicSelectToolbarControl, bool> enabled;
  const auto &controls() const { return entries; }
  void setControlEnabled(MusicSelectToolbarControl control, bool value) { enabled[control] = value; }
};
struct MusicSelectScene {
  bool sceneActive_ = true, failed_ = false, blocked = false;
  struct Context {
    std::atomic_bool appInBackground = false;
    TaskService *chartLibraryTasks = nullptr;
    void *sceneManager = nullptr;
    struct { bool findBmsSkipUnarchivingForNonSolidArchives = false; } settings;
  } context;
  struct Bars {
    std::vector<MusicSelectBar> rows;
    size_t selectedIndex = 0;
    const Bars &readView() const { return *this; }
    size_t rowCount() const { return rows.size(); }
    const MusicSelectBar &rowAt(size_t index) const { return rows.at(index); }
  } bars_;
  std::optional<int> chartSession_;
  View *modalLayer_ = nullptr, *modalOverlayPortal_ = nullptr;
  Toolbar *toolbar_ = nullptr;
  std::unique_ptr<FindBmsModal> findBmsModal_;
  int resets = 0;
  bool selectorInputBlocked() const { return blocked || (findBmsModal_ && findBmsModal_->isVisible()); }
  void resetLogicalInput() { ++resets; }
  bool toolbarControlAvailable(MusicSelectToolbarControl control) const;
  void refreshToolbarAvailability();
  bool canDownloadSelectedChart() const;
  void openDownload();
};

SCENE_METHODS

int main() {
  using Control = MusicSelectToolbarControl;
  MusicSelectScene scene;
  TaskService tasks;
  View layer, portal;
  Toolbar toolbar;
  scene.context.chartLibraryTasks = &tasks;
  scene.context.sceneManager = &scene;
  scene.modalLayer_ = &layer;
  scene.modalOverlayPortal_ = &portal;
  scene.toolbar_ = &toolbar;
  assert(!scene.canDownloadSelectedChart());
  scene.openDownload();
  assert(!scene.findBmsModal_);
  ChartMetaRecord missing;
  missing.unavailable = true;
  missing.meta.MD5 = "missing-md5";
  missing.meta.Title = "Missing chart";
  scene.bars_.rows = {{.kind = skin::MusicSelectBarKind::Song, .chart = missing}};
  scene.refreshToolbarAvailability();
  assert(scene.canDownloadSelectedChart());
  assert(!toolbar.enabled[Control::ChartViewer] && !toolbar.enabled[Control::ChartRecords]);
  assert(!scene.toolbarControlAvailable(Control::RevealChart));
  for (bool *guard : {&scene.failed_, &scene.blocked}) {
    *guard = true;
    scene.openDownload();
    assert(!scene.findBmsModal_);
    *guard = false;
  }
  scene.sceneActive_ = false;
  scene.openDownload();
  scene.sceneActive_ = true;
  scene.context.appInBackground = true;
  scene.openDownload();
  scene.context.appInBackground = false;
  assert(!scene.findBmsModal_);
  scene.context.settings.findBmsSkipUnarchivingForNonSolidArchives = true;
  scene.openDownload();
  auto *modal = scene.findBmsModal_.get();
  assert(modal && modal->confirmation);
  assert(modal && modal->shown.meta.MD5 == missing.meta.MD5 && scene.resets == 1);
  assert(!scene.canDownloadSelectedChart());
  scene.openDownload();
  assert(modal->shows == 1);
  assert(modal->callbacks.downloadRoot() == "download-root" && rootCalls == 1);
  assert(modal->callbacks.downloadOptions().skipUnarchivingForNonSolidArchives);
  BmsSearchResult result;
  result.outputPath = "download-root/chart.zip";
  result.removedPaths = {"download-root/old.zip"};
  modal->callbacks.filesReady(missing, result, true);
  assert(tasks.requests.size() == 1);
  const auto &indexed = tasks.requests.back();
  assert(indexed.kind == chart_library_tasks::TaskKind::IndexDownloadedPath);
  assert(indexed.downloadedPath == result.outputPath);
  assert(indexed.downloadedRemovedPaths == result.removedPaths);
  assert(indexed.downloadedTargetIdentity.md5 == missing.meta.MD5);
  modal->callbacks.filesReady(missing, result, false);
  assert(!tasks.requests.back().downloadedTargetIdentity.valid());
  assert(tasks.requests.back().downloadedRemovedPaths == result.removedPaths);
  modal->callbacks.refreshLibrary();
  assert(tasks.requests.back().kind == chart_library_tasks::TaskKind::RefreshLibrary);
  modal->visible = false;
  scene.openDownload();
  assert(scene.findBmsModal_.get() == modal && modal->shows == 2);
  modal->visible = false;
  auto &chart = *scene.bars_.rows.front().chart;
  chart.unavailable = false;
  chart.meta.BmsPath = "local/chart.bms";
  scene.refreshToolbarAvailability();
  assert(!scene.canDownloadSelectedChart());
  assert(toolbar.enabled[Control::ChartViewer] && toolbar.enabled[Control::ChartRecords]);
  assert(scene.toolbarControlAvailable(Control::RevealChart));
  chart.solidArchive = true;
  assert(!scene.toolbarControlAvailable(Control::ChartViewer));
  assert(!scene.toolbarControlAvailable(Control::ChartRecords));
  chart.unavailable = true;
  assert(!scene.canDownloadSelectedChart());
  scene.bars_.rows = {{.kind = skin::MusicSelectBarKind::Grade, .courseKey = "course"}};
  assert(scene.toolbarControlAvailable(Control::ChartRecords));
  assert(!scene.canDownloadSelectedChart());
  scene.bars_.rows = {{.kind = skin::MusicSelectBarKind::Folder}};
  assert(!scene.toolbarControlAvailable(Control::ChartViewer));
  assert(!scene.toolbarControlAvailable(Control::ChartRecords));
  scene.context.chartLibraryTasks = nullptr;
  assert(!scene.toolbarControlAvailable(Control::Tasks));
}
