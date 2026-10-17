"""Exercise the selector's production folder projection and tab restoration."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class MainMenuTabsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        production = (root / "src/scene/MainMenuScene.cpp").read_text()
        header = (root / "src/scene/MainMenuScene.h").read_text()
        repository = (root / "src/repositories/ChartRepository.h").read_text()
        library = (root / "src/scene/MainMenuLibrary.cpp").read_text()
        selection_helper = extract(library, "std::optional<ChartMetaRecord> chartSelectionRecordForReload(")
        # Run the real projection and active-folder reconciliation, stopping at
        # the UI refresh boundary. Database folder enumeration is fixture input.
        reload = extract(production, "void MainMenuScene::reloadFolderItems(")
        projection = reload[reload.index("  const auto isCourseFolder ="):
                            reload.index("  refreshChartFilterPanel();")]
        types = "\n".join(extract(repository, signature) + ";" for signature in (
            "enum class ChartRecordSortCriterion", "enum class ChartRecordSortDirection",
            "struct ChartMetaQuery"))
        methods = "\n".join(extract(production, signature) for signature in (
            "bool MainMenuScene::selectorTabActive() const",
            "Scene *MainMenuScene::activeUtilityTab() const",
            "void MainMenuScene::selectTab(Tab tab)",
            "void MainMenuScene::completeTabSwitch(Tab tab)",
            "bool MainMenuScene::applyPendingTabSwitch()",
            "void MainMenuScene::cancelPendingTabSwitch()",
            "void MainMenuScene::reselectCurrentChart()",
            "void MainMenuScene::applyPendingUiUpdates()",
            "void MainMenuScene::requestLibraryReload(bool includeFolders)",
            "EventHandleResult MainMenuScene::handleEvents(SDL_Event &event)",
            "void MainMenuScene::selectChartByPathAfterReload(",
            "ChartMetaQuery MainMenuScene::chartQueryForActiveFolder() const"))
        source = r'''
#include <algorithm>
#define TARGET_OS_ANDROID 0
#include <array>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <functional>
#include <future>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
constexpr int kNoClearTypeRank=-1, kMenuHeaderHeight=80;
using path_t=std::filesystem::path;
path_t fspath_to_path_t(const std::filesystem::path &path) { return path; }
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
namespace archive_file { void appendDebugLogLine(const std::string&) {} }
QUERY_TYPES
namespace bms_parser { struct ChartMeta {
  std::filesystem::path BmsPath;
  std::string Title, SubTitle, Artist, sha256, md5;
}; }
struct ChartMetaRecord {
  bms_parser::ChartMeta meta;
  bool courseStart=false, unzipAll=false, unavailable=false, solidArchive=false;
};
struct ChartRecordFilters {};
namespace i18n { std::string tr(const char *key) { return key; } }
namespace long_note_mode { int valueFromId(int mode) { return mode; } }
namespace chart_record_filters {
void applyToQuery(ChartMetaQuery&, const ChartRecordFilters&, bool) {}
}
namespace main_menu_library {
IDENTITY_TYPE;
FindBmsChartIdentity findBmsChartIdentity(const bms_parser::ChartMeta &meta) {
  return {meta.sha256, meta.md5}; // Fixture digests are already canonical.
}
IDENTITY_HELPER
GENERATION_HELPERS
HANDOFF_HELPER
SELECTION_HELPER
CENTERED_SCROLL_HELPER
std::optional<ChartMetaRecord> findBmsUnfilteredHandoffRecord(
    const std::optional<ChartMetaRecord> &record, const std::filesystem::path&) { return record; }
ChartMetaQuery chartQueryForSameFolder(const std::filesystem::path &folder,
    const std::string &keyword, const ChartRecordFilters&, int mode) {
  return {.keyword=keyword, .exactFolder=folder, .selectedLongNoteMode=mode};
}
}
namespace rendering { int window_width=1920; }
struct Insets { int top=0; };
Insets getSafeAreaInsetsUi() { return {}; }
using EventHandleResult = int;
constexpr int SDL_EVENT_KEY_DOWN=1, SDL_EVENT_KEY_UP=2;
constexpr int SDL_KMOD_CTRL=1, SDL_KMOD_GUI=2, SDLK_1=49, SDLK_4=52;
struct SDL_Event { int type=0; struct { int mod=0,key=0,repeat=0; } key; };
struct View {
  virtual ~View()=default;
  std::vector<View*> children;
  const std::vector<View*> &getChildren() const { return children; }
  std::string editing;
  void cancelPointerInput() {}
  void dismiss() {}
  bool visible=false;
  void setVisible(bool v) { visible=v; }
  bool getVisible() const { return visible; }
  bool isVisible() const { return visible; }
  bool isOpen() const { return visible; }
  bool handleEvents(SDL_Event&) { return true; }
  void applyYogaLayout() {}
  void setEditingText(const std::string &value) { editing=value; }
  View *findViewByName(const char*) { return nullptr; }
};
struct TextInputBox: View {
  bool editingActive=false;
  void endEditing() { editingActive=false; }
};
struct Scene {
  int initializations=0, resumes=0, digitEvents=0;
  EventHandleResult handleEvents(SDL_Event &event) { if(event.type==SDL_EVENT_KEY_DOWN) ++digitEvents; return {}; }
  bool coversTabHeader() const { return false; }
  void onPause() {} void onResume() { ++resumes; } void refreshLanguageIfNeeded() {}
  void prepareForUse() {} void init() { ++initializations; }
  template<class... Args> void configureTab(Args...) {}
};
struct Completion {
  std::filesystem::path chartPath;
  main_menu_library::FindBmsChartIdentity targetIdentity;
  std::uint64_t selectionGeneration=0;
};
struct Tasks {
  std::vector<Completion> completions;
  std::vector<Completion> takeDownloadedIndexCompletions() {
    auto result=std::move(completions); completions.clear(); return result;
  }
};
struct Context {
  std::atomic_bool chartLibraryFoldersReloadRequested=false, chartLibraryListReloadRequested=false;
  Tasks *chartLibraryTasks=nullptr;
  struct { std::uint64_t revision=1; std::uint64_t GetLibraryRevision() { return revision; } } chartRepository;
};
struct ImageView { static void dropAllCache() {} };
struct MusicPlayerScene: Scene { explicit MusicPlayerScene(Context&) {} };
struct IrUploadsScene: Scene { explicit IrUploadsScene(Context&) {} };
struct Preview { std::function<void()> stopping; void cancel() {} void stop() { if(stopping) stopping(); } };
template<class T> struct Recycler {
  std::vector<T> items;
  float scrollOffset=0;
  int selectedIndex=-1, itemHeight=100, height=100;
  std::function<void(const T&,int)> onSelected, onUnselected;
  int size() const { return int(items.size()); }
  int getHeight() const { return height; }
  T get(int index) const { return items.at(index); }
  void rebindVisibleItems() {}
};
struct Session {
  std::vector<ChartMetaRecord> rows;
  mutable int lookups=0;
  bool missFavorites=false;
  std::optional<ChartMetaRecord> SelectChartMetaByPaths(
      const std::array<std::filesystem::path,1>&) const { return std::nullopt; }
  int FindChartMetaIndex(const ChartMetaQuery &query, const std::filesystem::path &path) const {
    assert(++lookups<=16 && "lookup fallback must terminate, even when folder projection rejects All Songs");
    if(missFavorites && query.favoritesOnly)return -1;
    for (int i=0;i<rows.size();++i) if(rows[i].meta.BmsPath==path)return i;
    return -1;
  }
};
struct MainMenuScene : Scene {
  enum class Tab { Play, Course, Music, Ir };
  enum class AutoSelectionPreview { Suppress, Load };
  FOLDER_TYPE;
  TAB_STATE;
  Tab activeTab_=Tab::Play, selectorTab_=Tab::Play;
  std::array<SelectorTabState,2> selectorStates_;
  std::unique_ptr<MusicPlayerScene> musicTab_;
  std::unique_ptr<IrUploadsScene> irTab_;
  std::atomic_bool willStart=false;
  bool restoringTabSelection_=false;
  std::optional<ChartMetaRecord> selectedChartRecord;
  std::optional<std::filesystem::path> suppressPreviewForChartPath;
  int folderReloads=0, chartReloads=0, previewRequests=0;
  bool musicPlaying=false;
  std::optional<std::filesystem::path> temporaryChartFolder;
  LibraryFolderItem activeFolder;
  std::string searchText;
  struct { int longNoteMode=1; } profileSelections;
  ChartRecordFilters chartRecordFilters;
  struct { std::optional<ChartMetaRecord> leadingRecord; } chartListCache;
  Context context;
  std::mutex findBmsSelectionHandoffMutex;
  using PendingFindBmsSelectionHandoff=Completion;
  std::optional<PendingFindBmsSelectionHandoff> pendingFindBmsSelectionHandoff;
  std::optional<std::filesystem::path> pendingSelectChartPath;
  std::atomic_bool tasksModalOpenRequested=false;
  std::uint64_t chartSelectionGeneration=0, libraryRevision=1;
  int taskShows=0;
  bool archiveUnzipInProgress() const { return false; }
  void showTasksModal() { ++taskShows; }
  void reloadScoreClearRanks() {}
  std::optional<ChartMetaRecord> selectedRecordSnapshot() { return selectedChartRecord; }
  void applyPendingUiUpdates();
  void requestLibraryReload(bool includeFolders);
  View root;
  TextInputBox search;
  View *rootLayout=&root, *searchBox=&search, *revealContextMenu=nullptr;
  std::vector<View*> views;
  Preview *previewWorker_=nullptr;
  Recycler<ChartMetaRecord> songs;
  Recycler<LibraryFolderItem> sidebar;
  Recycler<ChartMetaRecord> *recyclerView=&songs;
  Recycler<LibraryFolderItem> *folderRecyclerView=&sidebar;
  std::optional<Session> chartSession=Session{};
  bool selectorTabActive() const;
  Scene *activeUtilityTab() const;
  EventHandleResult handleEvents(SDL_Event &);
  View *tutorial_=nullptr, *archiveUnzipModal_=nullptr, *findBmsModal_=nullptr, *decideOverlay_=nullptr, *recordsModal_=nullptr;
  View *playOptionsModalRoot=nullptr, *parseLogModalRoot=nullptr, *musicModalRoot=nullptr, *tasksModalRoot=nullptr, *rankingsModal=nullptr;
  std::optional<Tab> pendingTab_;
  std::future<void> tabPreviewStop_;
  View *tabLoadingOverlay_=nullptr;
  void selectTab(Tab);
  void completeTabSwitch(Tab);
  void reselectCurrentChart();
  bool applyPendingTabSwitch();
  void cancelPendingTabSwitch();
  void settleTab() {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!applyPendingTabSwitch() && std::chrono::steady_clock::now()<deadline)
      std::this_thread::yield();
    assert(!pendingTab_);
  }
  void switchTabAndWait(Tab tab) { selectTab(tab);settleTab(); }
  void selectChartByPathAfterReload(const std::filesystem::path&, AutoSelectionPreview);
  ChartMetaQuery chartQueryForActiveFolder() const;
  bool chartDifficultyRangeEnabled() const { return false; }
  int clearRankForFolder(const std::string&) const { return kNoClearTypeRank; }
  void updatePanelLayout() {} void refreshTabPresentation() {}
  // Production stops playback and clears the parsed chart, retaining metadata.
  void stopAndClearSelectedChart() {}
  void project(std::vector<LibraryFolderItem> &folders) {
PROJECTION
  }
  void reloadFolderItems(bool=false) {
    ++folderReloads;
    sidebar.items=fixtureFolders();
    project(sidebar.items);
  }
  void reloadChartList(bool=false) {
    ++chartReloads;
    const std::string prefix=selectorTab_==Tab::Course ? "course" : "song";
    chartSession->rows.clear();
    for(int i=0;i<5;++i) chartSession->rows.push_back({{prefix+std::to_string(i)+".bms"}});
    chartListCache.leadingRecord.reset();
    if(!temporaryChartFolder && activeFolder.type==LibraryFolderItem::Type::Course)
      chartListCache.leadingRecord=ChartMetaRecord{.courseStart=true};
    else if(!temporaryChartFolder && activeFolder.type==LibraryFolderItem::Type::SolidArchives)
      chartListCache.leadingRecord=ChartMetaRecord{.unzipAll=true};
    selectedChartRecord=main_menu_library::chartSelectionRecordForReload(
        selectedChartRecord,chartListCache.leadingRecord,false);
    songs.items=chartSession->rows;
    if(chartListCache.leadingRecord) songs.items.insert(songs.items.begin(),*chartListCache.leadingRecord);
    songs.selectedIndex=-1;
    // reload(false) only auto-selects the course-start row; ordinary folders
    // preserve metadata even when that record belongs to another tab.
    if(!temporaryChartFolder && !selectedChartRecord &&
        activeFolder.type==LibraryFolderItem::Type::Course && songs.size()>0) {
      songs.selectedIndex=0;
      songs.onSelected(songs.get(0),0);
    }
  }
  MainMenuScene() {
    root.children.push_back(&search);
    songs.onSelected=[this](const ChartMetaRecord &record,int) {
      chartSelectionGeneration=main_menu_library::chartSelectionGenerationAfter(
          chartSelectionGeneration, selectedChartRecord, record);
      selectedChartRecord=record;
      if(restoringTabSelection_ || !selectorTabActive() || record.meta.BmsPath.empty()) return;
      ++previewRequests;
      musicPlaying=false;
    };
  }
  static std::vector<LibraryFolderItem> fixtureFolders() {
    using T=LibraryFolderItem::Type;
    return {
      {.key="all",.type=T::AllSongs},
      {.key="favorites",.type=T::Favorites},
      {.key="archives",.type=T::SolidArchives},
      {.key="tables-heading",.sectionHeader=true},
      {.key="table",.type=T::DifficultyTable,.tableId=7},
      {.key="level",.type=T::DifficultyLevel,.depth=1,.tableId=7,.tableLevel="12"},
      {.key="clear",.type=T::DifficultyClearMark,.depth=2,.tableId=7,.clearMarkFolder=true},
      {.key="courses",.type=T::CoursesRoot},
      {.key="course-table",.type=T::CourseTable,.depth=1,.courseTableId=8},
      {.key="course-group",.type=T::CourseGroup,.depth=2,.courseTableId=8,.courseGroupName="段位"},
      {.key="course",.type=T::Course,.depth=3,.courseId=42,.courseTableId=8}
    };
  }
};
METHODS
int main(int argc,char **argv) {
  assert(argc==2);
  using T=MainMenuScene::LibraryFolderItem::Type;
  using Tab=MainMenuScene::Tab;
  MainMenuScene scene;
  auto folders=MainMenuScene::fixtureFolders();
  const std::string scenario=argv[1];
  if(scenario=="filtered-tab-selection") {
    for (auto tab : {Tab::Play, Tab::Course}) {
      MainMenuScene restored;
      restored.selectorTab_=restored.activeTab_=tab;
      if (tab==Tab::Course)
        restored.activeFolder={.key="course",.type=T::Course,.courseId=42};
      restored.reloadFolderItems(); restored.reloadChartList();
      ChartMetaRecord retained;
      retained.meta.BmsPath="filtered-out.bms";
      restored.songs.selectedIndex=-1;
      restored.songs.onSelected(retained,-1);
      restored.switchTabAndWait(tab==Tab::Play ? Tab::Course : Tab::Play);
      restored.switchTabAndWait(tab);
      assert(restored.songs.selectedIndex==-1 && "a retained chart must not highlight the course-start row");
      assert(restored.selectedChartRecord && restored.selectedChartRecord->meta.BmsPath=="filtered-out.bms" &&
             "tab restoration retains a chart even when the current query no longer includes it");
    }
    return 0;
  }
  if(scenario=="hidden-library-revision") {
    for(bool queuedHandoff : {false,true}) {
      MainMenuScene resume; Tasks tasks; resume.context.chartLibraryTasks=&tasks;
      resume.reloadFolderItems(); resume.reloadChartList();
      auto target=resume.songs.get(2);
      if(queuedHandoff) {
        target.unavailable=true; target.meta.BmsPath.clear(); target.meta.sha256=std::string(64,'a');
        resume.songs.items[2]=target;
      }
      resume.songs.selectedIndex=2; resume.songs.onSelected(target,2);
      const auto generation=resume.chartSelectionGeneration;
      resume.switchTabAndWait(Tab::Music);
      const auto foldersBefore=resume.folderReloads, chartsBefore=resume.chartReloads;
      // MUSIC favorite mutations increment the repository revision without setting reload flags.
      ++resume.context.chartRepository.revision;
      if(queuedHandoff) tasks.completions.push_back({"song3.bms", {std::string(64,'a'),{}}, generation});
      resume.applyPendingUiUpdates();
      assert(!resume.context.chartLibraryFoldersReloadRequested && !resume.context.chartLibraryListReloadRequested);
      assert(resume.folderReloads==foldersBefore && resume.chartReloads==chartsBefore);
      resume.switchTabAndWait(Tab::Play);
      assert(resume.chartSelectionGeneration==generation);
      resume.applyPendingUiUpdates();
      assert(resume.folderReloads==foldersBefore+1 && resume.chartReloads==chartsBefore+1);
      assert(resume.libraryRevision==resume.context.chartRepository.revision);
      assert(resume.selectedChartRecord->meta.BmsPath==(queuedHandoff ? "song3.bms" : "song2.bms"));
      assert(!resume.pendingFindBmsSelectionHandoff);
      resume.applyPendingUiUpdates();
      resume.switchTabAndWait(Tab::Music); resume.switchTabAndWait(Tab::Play);
      resume.applyPendingUiUpdates();
      assert(resume.folderReloads==foldersBefore+1 && resume.chartReloads==chartsBefore+1);
    }
  } else if(scenario=="hidden-completion-restore" || scenario=="hidden-completion-reject") {
    for(auto utility : {Tab::Music, Tab::Ir}) {
      for(bool pathless : {false,true}) {
        MainMenuScene handoff;
        Tasks tasks; handoff.context.chartLibraryTasks=&tasks;
        handoff.reloadFolderItems(); handoff.reloadChartList();
        auto target=handoff.songs.get(2);
        target.unavailable=true;
        target.meta.sha256=std::string(64, 'a');
        if(pathless) target.meta.BmsPath.clear();
        handoff.songs.items[2]=target;
        handoff.songs.selectedIndex=2;
        handoff.songs.onSelected(target,2);
        const auto capturedGeneration=handoff.chartSelectionGeneration;
        handoff.switchTabAndWait(utility);
        tasks.completions.push_back({"song3.bms", {std::string(64, 'a'), {}}, capturedGeneration});
        handoff.context.chartLibraryListReloadRequested=true;
        handoff.tasksModalOpenRequested=true;
        const auto reloads=handoff.chartReloads;
        handoff.applyPendingUiUpdates();
        assert(tasks.completions.empty() && handoff.taskShows==1);
        assert(handoff.pendingFindBmsSelectionHandoff && handoff.context.chartLibraryListReloadRequested);
        assert(handoff.chartReloads==reloads && handoff.selectedChartRecord->meta.BmsPath==target.meta.BmsPath);
        if(scenario=="hidden-completion-reject") {
          // Visiting another selector must invalidate the old selection generation.
          handoff.switchTabAndWait(Tab::Course);
          handoff.applyPendingUiUpdates();
          assert(!handoff.pendingFindBmsSelectionHandoff);
          assert(!handoff.selectedChartRecord || handoff.selectedChartRecord->meta.BmsPath!="song3.bms");
          continue;
        }
        handoff.switchTabAndWait(Tab::Play);
        assert(handoff.chartSelectionGeneration==capturedGeneration);
        assert(handoff.chartReloads==reloads);
        handoff.applyPendingUiUpdates();
        assert(!handoff.pendingFindBmsSelectionHandoff && !handoff.context.chartLibraryListReloadRequested);
        assert(handoff.selectedChartRecord && handoff.selectedChartRecord->meta.BmsPath=="song3.bms");
        assert(handoff.chartReloads==reloads+1);
        handoff.applyPendingUiUpdates();
        assert(handoff.chartReloads==reloads+1);
        handoff.switchTabAndWait(utility); handoff.switchTabAndWait(Tab::Play);
        assert(handoff.selectedChartRecord->meta.BmsPath=="song3.bms");
      }
    }
  } else if(scenario=="hidden-completion-stale") {
    for(bool changeGeneration : {false,true}) {
      MainMenuScene handoff; Tasks tasks; handoff.context.chartLibraryTasks=&tasks;
      handoff.reloadFolderItems(); handoff.reloadChartList();
      auto target=handoff.songs.get(2); target.unavailable=true; target.meta.sha256=std::string(64,'a');
      handoff.songs.onSelected(target,2);
      const auto generation=handoff.chartSelectionGeneration;
      handoff.switchTabAndWait(Tab::Music);
      tasks.completions.push_back({"song3.bms", {std::string(64,'a'),{}}, generation});
      handoff.context.chartLibraryListReloadRequested=true;
      handoff.applyPendingUiUpdates(); handoff.switchTabAndWait(Tab::Play);
      if(changeGeneration) ++handoff.chartSelectionGeneration;
      else handoff.selectedChartRecord->meta.sha256=std::string(64,'b');
      handoff.applyPendingUiUpdates();
      assert(!handoff.pendingFindBmsSelectionHandoff);
      assert(handoff.selectedChartRecord->meta.BmsPath=="song2.bms");
    }
  } else if(scenario=="slow-preview-switch" || scenario=="slow-preview-ir" || scenario=="slow-preview-course" || scenario=="slow-preview-play" || scenario=="cancel-preview-switch") {
    const Tab destination=scenario=="slow-preview-ir" ? Tab::Ir :
        scenario=="slow-preview-course" ? Tab::Course :
        scenario=="slow-preview-play" ? Tab::Play : Tab::Music;
    if(scenario=="slow-preview-play") scene.activeTab_=scene.selectorTab_=Tab::Course;
    const Tab origin=scene.activeTab_;
    std::promise<void> entered,release;
    auto releaseSignal=release.get_future().share();
    Preview preview{[&] { entered.set_value();releaseSignal.wait(); }};
    scene.previewWorker_=&preview;
    View overlay;
    scene.tabLoadingOverlay_=&overlay;
    auto switching=std::async(std::launch::async,[&] { scene.selectTab(destination); });
    entered.get_future().wait();
    const auto result=switching.wait_for(std::chrono::milliseconds(50));
    if(result!=std::future_status::ready) release.set_value();
    switching.get();
    assert(result==std::future_status::ready && "tab selection must not join slow preview/audio work on the UI thread");
    assert(scene.pendingTab_ && scene.activeTab_==origin && !scene.musicTab_ && !scene.irTab_);
    assert(overlay.visible && !scene.applyPendingTabSwitch());
    SDL_Event event{.type=SDL_EVENT_KEY_DOWN,.key={.mod=SDL_KMOD_GUI,.key=SDLK_4}};
    scene.handleEvents(event);
    scene.selectTab(Tab::Course);
    assert(*scene.pendingTab_==destination && scene.activeTab_==origin);
    if(scenario=="cancel-preview-switch") {
      std::promise<void> cancelEntered;
      auto cancelling=std::async(std::launch::async,[&] {
        cancelEntered.set_value();
        scene.cancelPendingTabSwitch();
      });
      cancelEntered.get_future().wait();
      const auto cancelResult=cancelling.wait_for(std::chrono::milliseconds(50));
      release.set_value();
      cancelling.get();
      assert(cancelResult==std::future_status::timeout);
      assert(!scene.pendingTab_ && !scene.tabPreviewStop_.valid());
      assert(scene.activeTab_==Tab::Play && !scene.musicTab_);
    } else {
      release.set_value();
      scene.settleTab();
      assert(scene.activeTab_==destination && (!scene.activeUtilityTab() || scene.activeUtilityTab()->initializations==1));
    }
    assert(!overlay.visible);
    scene.previewWorker_=nullptr;
  } else if(scenario=="restored-preview") {
    scene.reloadFolderItems();scene.reloadChartList();
    scene.songs.selectedIndex=2;
    scene.songs.onSelected(scene.songs.get(2),2);
    for(auto destination : {Tab::Course,Tab::Music,Tab::Ir}) {
      scene.switchTabAndWait(destination);
      scene.musicPlaying=destination==Tab::Music;
      const int requests=scene.previewRequests;
      scene.switchTabAndWait(Tab::Play);
      assert(scene.selectedChartRecord->meta.BmsPath=="song2.bms");
      assert(scene.previewRequests==requests+1 && "restored selection must restart its preview exactly once");
      assert(!scene.musicPlaying);
    }
  } else if(scenario=="shell-shortcuts") {
    for (int key=SDLK_1;key<=SDLK_4;++key) {
      scene.switchTabAndWait(Tab::Music);
      const int before=scene.musicTab_->digitEvents;
      SDL_Event event{.type=SDL_EVENT_KEY_DOWN,.key={.mod=SDL_KMOD_GUI,.key=key}};
      scene.handleEvents(event);
      assert(scene.activeTab_==static_cast<Tab>(key-SDLK_1));
      assert(scene.musicTab_->digitEvents==before);
      event.type=SDL_EVENT_KEY_UP;
      event.key.mod=0;
      scene.handleEvents(event);
      assert(scene.activeTab_==static_cast<Tab>(key-SDLK_1));
    }
  } else if(scenario=="utility-lifecycle") {
    for (auto tab : {Tab::Music, Tab::Ir}) {
      scene.switchTabAndWait(tab);
      auto *utility=scene.activeUtilityTab();
      assert(utility->initializations==1 && utility->resumes==0);
      scene.switchTabAndWait(Tab::Play);
      scene.switchTabAndWait(tab);
      assert(utility->initializations==1 && utility->resumes==1);
    }
  } else if(scenario=="play") {
    scene.activeFolder={.key="course",.type=T::Course,.courseId=42};
    scene.project(folders);
    const std::vector<std::string> expected{"all","favorites","archives","tables-heading","table","level","clear"};
    assert(folders.size()==expected.size());
    for(int i=0;i<folders.size();++i)assert(folders[i].key==expected[i]);
    assert(folders[5].depth==1 && folders[6].depth==2);
    assert(scene.activeFolder.key=="all");
    assert(!scene.chartQueryForActiveFolder().coursesOnly);
  } else if(scenario=="course") {
    scene.selectorTab_=Tab::Course;
    scene.activeFolder={.key="course-group",.type=T::CourseGroup};
    scene.project(folders);
    assert(folders.size()==3);
    assert(folders[0].key=="course-table" && folders[0].depth==0);
    assert(folders[1].key=="course-group" && folders[1].depth==1);
    assert(folders[2].key=="course" && folders[2].depth==2);
    assert(scene.activeFolder.key=="course-group" && scene.activeFolder.courseTableId==8);
    auto query=scene.chartQueryForActiveFolder();
    assert(query.courseTableId==8 && query.courseGroupName=="段位");
    scene.activeFolder=folders[2];
    assert(scene.chartQueryForActiveFolder().courseId==42);
  } else if(scenario=="empty") {
    scene.selectorTab_=Tab::Course;
    folders.resize(7); // A library with songs and tables but no course data.
    for(bool staleSelection : {false,true}) {
      auto emptyCourses=folders;
      scene.activeFolder=staleSelection
          ? MainMenuScene::LibraryFolderItem{.key="deleted-course",.type=T::Course,.courseId=99}
          : MainMenuScene::LibraryFolderItem{};
      scene.searchText="challenge";
      scene.project(emptyCourses);
      assert(emptyCourses.empty());
      assert(scene.activeFolder.type==T::CoursesRoot);
      const auto query=scene.chartQueryForActiveFolder();
      assert(query.coursesOnly && query.courseId==0 && query.tableId==0);
      assert(query.keyword=="challenge");
    }
  } else if(scenario=="restore") {
    scene.reloadFolderItems();scene.reloadChartList();
    scene.activeFolder={.key="favorites",.type=T::Favorites};
    scene.searchText="play search";
    scene.temporaryChartFolder="/charts/remember-me";
    scene.songs.selectedIndex=3;
    scene.selectedChartRecord=scene.songs.get(3);
    scene.songs.scrollOffset=175;
    scene.sidebar.scrollOffset=240;
    scene.switchTabAndWait(Tab::Course);
    assert(scene.selectorTab_==Tab::Course && scene.searchText.empty());
    assert(!scene.selectedChartRecord && scene.songs.selectedIndex==-1);
    assert(!scene.temporaryChartFolder && scene.activeFolder.key=="course-table");
    scene.activeFolder={.key="course",.type=T::Course,.courseId=42};
    scene.searchText="course search";
    scene.songs.selectedIndex=2;
    scene.selectedChartRecord=scene.songs.get(2);
    scene.songs.scrollOffset=950; // Library resizing must clamp a retained offset.
    scene.sidebar.scrollOffset=800;
    scene.switchTabAndWait(Tab::Play);
    assert(scene.activeFolder.key=="favorites" && scene.searchText=="play search");
    assert(scene.search.editing=="play search");
    assert(scene.temporaryChartFolder==std::filesystem::path("/charts/remember-me"));
    assert(scene.songs.selectedIndex==3 && scene.selectedChartRecord->meta.BmsPath=="song3.bms");
    assert(scene.songs.scrollOffset==175 && scene.sidebar.scrollOffset==240);
    // A leading synthetic record changes the visible index, not chart identity.
    scene.switchTabAndWait(Tab::Course);
    assert(scene.activeFolder.key=="course" && scene.searchText=="course search");
    assert(!scene.temporaryChartFolder);
    assert(scene.songs.selectedIndex==3 && scene.selectedChartRecord->meta.BmsPath=="course2.bms");
    assert(scene.songs.scrollOffset==500 && scene.sidebar.scrollOffset==200);
    assert(!scene.restoringTabSelection_);
    scene.switchTabAndWait(Tab::Music);
    scene.switchTabAndWait(Tab::Course);
    assert(scene.selectedChartRecord->meta.BmsPath=="course2.bms");
    assert(scene.searchText=="course search" && scene.songs.scrollOffset==500);
  } else if(scenario=="first-course") {
    scene.reloadFolderItems();scene.reloadChartList();
    scene.songs.selectedIndex=2;
    scene.selectedChartRecord=scene.songs.get(2);
    scene.search.editingActive=true;
    scene.switchTabAndWait(Tab::Course);
    assert(!scene.search.editingActive);
    assert(scene.activeFolder.type==T::CourseTable);
    assert(!scene.selectedChartRecord && "first COURSE entry must not retain PLAY chart details");
    assert(scene.songs.selectedIndex==-1);
  } else if(scenario=="special-rows") {
    scene.switchTabAndWait(Tab::Course);
    scene.activeFolder={.key="course",.type=T::Course,.courseId=42};
    scene.reloadChartList();
    assert(scene.selectedChartRecord && scene.selectedChartRecord->courseStart);
    assert(scene.selectedChartRecord->meta.BmsPath.empty());
    scene.switchTabAndWait(Tab::Play);
    scene.activeFolder={.key="archives",.type=T::SolidArchives};
    scene.reloadChartList();
    scene.songs.selectedIndex=0;
    scene.songs.onSelected(scene.songs.get(0),0);
    assert(scene.selectedChartRecord->unzipAll);
    scene.switchTabAndWait(Tab::Course);
    assert(scene.songs.selectedIndex==0 && scene.selectedChartRecord->courseStart);
    assert(!scene.selectedChartRecord->unzipAll && scene.selectedChartRecord->meta.BmsPath.empty());
    scene.switchTabAndWait(Tab::Play);
    assert(scene.songs.selectedIndex==0 && scene.selectedChartRecord && scene.selectedChartRecord->unzipAll);
    assert(!scene.selectedChartRecord->courseStart && scene.selectedChartRecord->meta.BmsPath.empty());
    scene.switchTabAndWait(Tab::Music);
    scene.switchTabAndWait(Tab::Play);
    assert(scene.songs.selectedIndex==0 && scene.selectedChartRecord->unzipAll);
  } else if(scenario=="failed-course-lookup") {
    for(auto preview : {MainMenuScene::AutoSelectionPreview::Suppress, MainMenuScene::AutoSelectionPreview::Load}) {
      MainMenuScene course;
      course.selectorTab_=course.activeTab_=Tab::Course;
      course.activeFolder={.key="course",.type=T::Course,.courseId=42};
      course.reloadFolderItems();course.reloadChartList();
      course.folderReloads=course.chartReloads=0;
      course.songs.scrollOffset=80;
      course.selectChartByPathAfterReload("absent.bms",preview);
      assert(course.chartSession->lookups==1);
      assert(course.folderReloads==0 && course.chartReloads==0);
      assert(course.activeFolder.key=="course" && course.activeFolder.courseId==42);
      assert(course.selectedChartRecord->courseStart && course.songs.selectedIndex==0);
      assert(course.songs.scrollOffset==80);
    }
  } else if(scenario=="play-lookup-fallback") {
    for(bool foundInAllSongs : {false,true}) {
      MainMenuScene play;
      play.activeFolder={.key="favorites",.type=T::Favorites};
      play.reloadFolderItems();play.reloadChartList();
      play.chartSession->missFavorites=true;
      play.folderReloads=play.chartReloads=0;
      const std::filesystem::path path=foundInAllSongs ? "song2.bms" : "absent.bms";
      play.selectChartByPathAfterReload(path,MainMenuScene::AutoSelectionPreview::Suppress);
      assert(play.activeFolder.type==T::AllSongs);
      assert(play.folderReloads==1 && play.chartReloads==1);
      assert(play.chartSession->lookups==2);
      if(foundInAllSongs) {
        assert(play.songs.selectedIndex==2 && play.selectedChartRecord->meta.BmsPath==path);
        assert(play.suppressPreviewForChartPath==path);
      } else assert(!play.selectedChartRecord);
    }
  } else assert(false);
}
'''
        source = (source.replace("QUERY_TYPES", types).replace("SELECTION_HELPER", selection_helper)
                  .replace("CENTERED_SCROLL_HELPER", extract(library, "float centeredScrollOffsetForItem("))
                  .replace("IDENTITY_TYPE", extract((root / "src/scene/MainMenuLibrary.h").read_text(), "struct FindBmsChartIdentity"))
                  .replace("IDENTITY_HELPER", extract(library, "bool identityMatches("))
                  .replace("GENERATION_HELPERS", extract(library, "bool sameChartSelection(") + "\n" + extract(library, "std::uint64_t chartSelectionGenerationAfter("))
                  .replace("HANDOFF_HELPER", extract(library, "bool findBmsSelectionHandoffAllowed("))
                  .replace("FOLDER_TYPE", extract(header, "struct LibraryFolderItem"))
                  .replace("TAB_STATE", extract(header, "struct SelectorTabState"))
                  .replace("PROJECTION", projection).replace("METHODS", methods))
        cls.temp = tempfile.TemporaryDirectory(prefix="asobmashow-menu-tabs-")
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name) / "test.cpp"
        cls.binary = Path(cls.temp.name) / "test"
        path.write_text(source)
        FixtureCompiler.from_environment(shared_headers=True).build([path], cls.binary, path.parent)

    def test_score_reorder_preserves_chart_after_page_release(self):
        root = Path(__file__).resolve().parents[1]
        production = (root / "src/scene/MainMenuScene.cpp").read_text()
        header = (root / "src/scene/MainMenuScene.h").read_text()
        library = (root / "src/scene/MainMenuLibrary.cpp").read_text()
        helpers = "\n".join(extract(library, signature) for signature in (
            "std::optional<ChartMetaRecord> chartSelectionRecordForReload(",
            "std::filesystem::path chartSelectionPathForReload("))
        methods = "\n".join(extract(production, signature) for signature in (
            "void MainMenuScene::ChartListPageCache::reset(",
            "void MainMenuScene::ChartListPageCache::releasePages()",
            "void MainMenuScene::ChartListPageCache::clear()",
            "const ChartMetaRecord &MainMenuScene::ChartListPageCache::get(",
            "void MainMenuScene::reloadChartList(",
            "std::optional<ChartMetaRecord> MainMenuScene::selectedRecordSnapshot() const",
            "void MainMenuScene::reselectCurrentChart()",
            "void MainMenuScene::openReplayRecordsForSelection()",
            "void MainMenuScene::startSelectedChart()",
            "void MainMenuScene::refreshSelectedChartDetails()"))
        fixture = (root / "tests/main_menu_chart_reload_fixture.cpp").read_text()
        fixture = fixture.replace("// SELECTION_HELPERS", helpers).replace(
            "// CACHE_TYPE", extract(header, "struct ChartListPageCache {") + ";").replace(
            "// PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-menu-reorder-") as temp:
            source, binary = Path(temp) / "test.cpp", Path(temp) / "test"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source], binary, source.parent, includes=[root / "src"])
            subprocess.run([str(binary)], check=True)

    def test_filtered_out_selection_survives_tab_restoration(self):
        subprocess.run([str(self.binary), "filtered-tab-selection"], check=True)

    def test_hidden_revision_only_library_change_refreshes_once_on_return(self):
        subprocess.run([str(self.binary), "hidden-library-revision"], check=True, timeout=5)

    def test_hidden_completion_survives_return_and_rejects_stale_selection(self):
        for scenario in ("hidden-completion-restore", "hidden-completion-reject", "hidden-completion-stale"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binary), scenario], check=True, timeout=5)

    def test_restored_chart_restarts_preview_and_stops_music(self):
        subprocess.run([str(self.binary), "restored-preview"], check=True)

    def test_shell_shortcuts_precede_music_digit_handlers(self):
        subprocess.run([str(self.binary), "shell-shortcuts"], check=True)

    def test_tab_selection_does_not_wait_for_preview_shutdown(self):
        for scenario in ("slow-preview-switch", "slow-preview-ir", "slow-preview-course", "slow-preview-play"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binary), scenario], check=True, timeout=5)

    def test_cancelling_switch_joins_shutdown_without_opening_destination(self):
        subprocess.run([str(self.binary), "cancel-preview-switch"], check=True, timeout=5)

    def test_utility_tabs_load_once_on_creation_and_resume_when_retained(self):
        subprocess.run([str(self.binary), "utility-lifecycle"], check=True)

    def test_play_excludes_every_course_folder_kind(self):
        subprocess.run([str(self.binary), "play"], check=True)

    def test_course_promotes_children_and_preserves_selected_group(self):
        subprocess.run([str(self.binary), "course"], check=True)

    def test_empty_course_tab_never_queries_all_songs(self):
        subprocess.run([str(self.binary), "empty"], check=True)

    def test_failed_course_lookup_never_falls_back_to_all_songs(self):
        subprocess.run([str(self.binary), "failed-course-lookup"], check=True, timeout=5)

    def test_play_lookup_falls_back_once_and_can_select_the_chart(self):
        subprocess.run([str(self.binary), "play-lookup-fallback"], check=True, timeout=5)

    def test_first_course_entry_clears_retained_play_metadata(self):
        subprocess.run([str(self.binary), "first-course"], check=True)

    def test_pathless_course_start_and_unzip_all_rows_restore(self):
        subprocess.run([str(self.binary), "special-rows"], check=True)

    def test_tabs_restore_independent_selection_search_folder_and_scroll(self):
        subprocess.run([str(self.binary), "restore"], check=True)


    def test_hidden_dialogs_do_not_relayout_the_idle_menu(self):
        root = Path(__file__).resolve().parents[1]
        render = extract((root / "src/scene/MainMenuScene.cpp").read_text(),
                         "void MainMenuScene::renderScene()")
        source = r'''
#include <cassert>
#include <cstdio>
#define TARGET_OS_ANDROID 0
namespace rendering { int window_width=1920, window_height=1080; }
struct SafeAreaInsets { int top=0, left=0, bottom=0, right=0; };
SafeAreaInsets safeArea;
SafeAreaInsets getSafeAreaInsetsUi() { return safeArea; }
enum class Edge { Top, Left, Right, Bottom };
struct View {
  bool visible=false;
  int resizes=0, renders=0;
  void setSize(int,int) {}
  void setPadding(Edge,int) {}
  void setViewportSize(int,int) {}
  void updateLayout(int,int) {}
  void applyYogaLayout() {}
  void dismiss() {}
  bool isOpen() { return false; }
  bool isVisible() { return visible; }
  bool getVisible() { return visible; }
  void resize(int,int) { ++resizes; }
  void render() { ++renders; }
};
constexpr int kRootPadding=8;
struct MainMenuScene {
  View root, records, finder, archive, utility;
  View *rootLayout=&root, *recordsModal_=&records,
       *findBmsModal_=&finder, *archiveUnzipModal_=&archive;
  View *tabLoadingOverlay_=nullptr, *playOptionsModal=nullptr,
       *overlayPortal=nullptr, *decideOverlay_=nullptr,
       *revealContextMenu=nullptr, *parseLogModalRoot=nullptr,
       *musicModalRoot=nullptr, *tasksModalRoot=nullptr, *tutorial_=nullptr;
  int lastLayoutWidth=-1,lastLayoutHeight=-1,lastSafeTop=-1,lastSafeLeft=-1,
      lastSafeBottom=-1,lastSafeRight=-1;
  bool musicTab=false;
  View *activeUtilityTab() { return musicTab ? &utility : nullptr; }
  void resizeTasksModal() {} void resizeParseLogModal() {}
  void updatePanelLayout() {} void refreshTabPresentation() {}
  void renderScene();
};
RENDER
int main() {
  MainMenuScene menu;
  menu.renderScene(); // Establish the viewport, including hidden dialogs.
  for (auto *modal : {&menu.records, &menu.finder, &menu.archive}) {
    assert(modal->resizes==1);
    modal->resizes=0;
  }
  for (int i=0; i<120; ++i) menu.renderScene();
  menu.musicTab=true;
  for (int i=0; i<120; ++i) menu.renderScene();
  assert(menu.utility.renders==120);
  for (auto *modal : {&menu.records, &menu.finder, &menu.archive}) {
    if (modal->resizes) std::fprintf(stderr,"hidden dialog resized %d times\n",modal->resizes);
    assert(modal->resizes==0);
  }
  ++rendering::window_width;
  menu.renderScene();
  ++safeArea.top;
  menu.renderScene();
  for (auto *modal : {&menu.records, &menu.finder, &menu.archive}) {
    assert(modal->resizes==2);
    modal->visible=true;
  }
  menu.renderScene();
  for (auto *modal : {&menu.records, &menu.finder, &menu.archive})
    assert(modal->resizes==3);
}
'''
        source = "#include <initializer_list>\n" + source.replace("RENDER", render)
        with tempfile.TemporaryDirectory(prefix="asobmashow-idle-menu-") as directory:
            cpp = Path(directory) / "idle.cpp"
            binary = Path(directory) / "idle"
            cpp.write_text(source)
            FixtureCompiler.from_environment(shared_headers=True).build([cpp], binary, cpp.parent)
            subprocess.run([str(binary)], check=True, timeout=5)


    def test_hidden_selector_coalesces_refreshes_and_delivers_handoffs(self):
        root = Path(__file__).resolve().parents[1]
        production = (root / "src/scene/MainMenuScene.cpp").read_text()
        fixture = (root / "tests/main_menu_hidden_selector_fixture.cpp").read_text()
        methods = "\n".join(extract(production, signature) for signature in (
            "void MainMenuScene::update(float dt)",
            "void MainMenuScene::applyPendingUiUpdates()",
            "void MainMenuScene::refreshScoreClearRanksIfNeeded()"))
        with tempfile.TemporaryDirectory(prefix="asobmashow-hidden-selector-") as directory:
            cpp = Path(directory) / "hidden.cpp"
            binary = Path(directory) / "hidden"
            cpp.write_text(fixture.replace("SCENE_METHODS", methods))
            FixtureCompiler.from_environment(shared_headers=True).build([cpp], binary, cpp.parent)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
