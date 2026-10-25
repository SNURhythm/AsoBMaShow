// Production page-cache and reload methods are inserted by main_menu_tabs_tests.py.
#include "repositories/ChartMetaPageCache.h"
#include <cassert>
#include <atomic>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
constexpr int kNoClearTypeRank=-1;
using path_t = std::filesystem::path;
path_t fspath_to_path_t(const std::filesystem::path &path) { return path; }
struct ChartMetaRecord {
  struct { std::filesystem::path BmsPath; std::string Title, Artist; } meta;
  std::string difficultyTableLabels;
  bool unzipAll=false, courseStart=false, unavailable=false, solidArchive=false;
  std::optional<int> randomSelect;
};
struct ChartMetaQuery { int limit=0, offset=0; };
enum class ChartRecordSortCriterion { Default, ClearMark, Rate };
namespace i18n { std::string tr(const char *key) { return key; } }
void SDL_Log(const char *, ...) {}
namespace main_menu_library {
// SELECTION_HELPERS
ChartMetaRecord unzipAllRecord(int) { return {.unzipAll=true}; }
constexpr std::array kChartSelectorRandomFilters{0, 1, 2};
// RANDOM_RECORDS
}
struct ChartRepository {
  struct Session {
    std::vector<ChartMetaRecord> rows;
    std::array<int, 3> randomIndices{-1, -1, -1};
    std::array<int, 3> RandomChartMetaIndices(const ChartMetaQuery &, const auto &, const auto &) {
      return randomIndices;
    }
    int CountChartMeta(const ChartMetaQuery &) const { return int(rows.size()); }
    void QueryChartMeta(const ChartMetaQuery &query, std::vector<ChartMetaRecord> &out) const {
      for (int i=query.offset; i<int(rows.size()) && i<query.offset+query.limit; ++i)
        out.push_back(rows[i]);
    }
    int FindChartMetaIndex(const ChartMetaQuery &, const std::filesystem::path &path) const {
      for (int i=0; i<int(rows.size()); ++i)
        if (rows[i].meta.BmsPath==path) return i;
      return -1;
    }
  };
};
struct Recycler {
  int count=0, selectedIndex=-1, itemHeight=100;
  float scrollOffset=0;
  std::function<const ChartMetaRecord &(int)> provider;
  std::function<void(const ChartMetaRecord &, int)> onSelected;
  int size() const { return count; }
  int getHeight() const { return 300; }
  const ChartMetaRecord &get(int i) const { return provider(i); }
  void setItemProvider(int size, decltype(provider) value) {
    count=size; provider=std::move(value); selectedIndex=-1; scrollOffset=0;
  }
  void rebindVisibleItems() {}
};
namespace long_note_mode { int valueFromId(int mode) { return mode; } }
std::string formatGaugeTotal(const auto &, int) { return ""; }
struct MainMenuScene {
  enum class Tab { Play, Course };
  Tab selectorTab_=Tab::Play;
  // CACHE_TYPE
  ChartListPageCache chartListCache;
  Recycler list;
  Recycler *recyclerView=&list;
  Recycler *folderRecyclerView=nullptr;
  std::optional<ChartRepository::Session> chartSession{std::in_place};
  std::optional<ChartMetaRecord> selectedChartRecord;
  std::optional<std::filesystem::path> temporaryChartFolder;
  struct LibraryFolderItem {
    enum class Type { AllSongs, Course, SolidArchives, DifficultyClearMark };
    Type type=Type::AllSongs;
    int courseId=0;
    std::string label, courseGroupName;
  } activeFolder;
  struct { int solidArchiveCount=0; } folderMetadataCache;
  ChartMetaQuery chartQueryForActiveFolder() const { return {}; }
  std::atomic_bool willStart=false;
  std::optional<std::filesystem::path> pendingSelectChartPath;
  struct {
    std::atomic_bool chartLibraryListReloadRequested=false,
                     chartLibraryFoldersReloadRequested=false;
    struct Repository {
      unsigned revision=1;
      unsigned GetLibraryRevision() const { return revision; }
      unsigned GetRevision() const { return revision; }
    } chartRepository, scoreRepository;
  } context;
  unsigned libraryRevision=1, scoreClearRanksRevision=1;
  struct { bool inProgress() const { return false; } } replayExportJob_;
  bool archiveUnzipInProgress() const { return false; }
  std::filesystem::path playedPath, previewPath;
  struct Records { std::filesystem::path path;
    void showChart(const ChartMetaRecord &record) { path=record.meta.BmsPath; }
  } records;
  Records *recordsModal_=&records;
  std::vector<int> replayIrObservedRevisions;
  void setReplayButtonVisible(bool) {}
  void startChartDirect(const ChartMetaRecord &record) { playedPath=record.meta.BmsPath; }
  struct Details { std::filesystem::path path;
    void setChart(const ChartMetaRecord *record, const auto &, int, const std::string &) {
      path=record ? record->meta.BmsPath : std::filesystem::path{};
    }
  } details;
  Details *chartDetailsView_=&details;
  struct { std::optional<int> bestFor(const auto &, int) { return {}; } } scoreBestScores;
  struct { int longNoteMode=1, ruleset=0; } profileSelections;
  int scoreClearRanks=0;
  struct {
    std::optional<int> clearMarkRank, scoreRank;
    struct { ChartRecordSortCriterion criterion=ChartRecordSortCriterion::Default; } sort;
  } chartRecordFilters;
  int clearRankForChart(const ChartMetaRecord &) { return 0; }
  std::optional<ChartMetaRecord> selectedRecordSnapshot() const;
  void reselectCurrentChart();
  void openReplayRecordsForSelection();
  void startSelectedChart();
  void startRandomChart(const ChartMetaRecord &);
  void refreshSelectedChartDetails();
  void refreshPlayOptionButtons() {}
  void refreshLongNoteModeButtons() {}
  void refreshAssistOptionButtons() {}
  void refreshSelectedChartActionState() {}
  void reloadFolderItems(bool) {}
  void refreshLongNoteModeClearRankViews();
  void reloadChartList(bool preserveViewState=false);
};
// PRODUCTION_METHODS
int main() {
  {
    MainMenuScene scene;
    scene.chartSession->rows = {{{"first.bms"}}, {{"second.bms"}}};
    scene.chartSession->randomIndices = {0, 0, 1};
    scene.reloadChartList();
    assert(scene.list.size()==5 && "three random rows precede the scoped charts");
    for (int i=0; i<3; ++i) assert(scene.list.get(i).randomSelect==i);
    assert(scene.list.get(3).meta.BmsPath=="first.bms");
    scene.list.selectedIndex=1;
    scene.selectedChartRecord=scene.list.get(1);
    scene.context.chartRepository.revision=2;
    scene.chartSession->rows[0].meta.BmsPath="updated.bms";
    scene.startSelectedChart();
    assert(scene.playedPath.empty() && "do not use stale library or score caches");
    scene.libraryRevision=2;
    scene.context.scoreRepository.revision=2;
    scene.startSelectedChart();
    assert(scene.playedPath.empty());
    scene.scoreClearRanksRevision=2;
    scene.startSelectedChart();
    assert(scene.playedPath=="updated.bms" && scene.previewPath.empty());
    assert(scene.list.selectedIndex==1 && scene.selectedChartRecord->randomSelect==1);
    scene.chartListCache.releasePages();
    scene.reloadChartList(true);
    assert(scene.list.selectedIndex==1 && scene.selectedChartRecord->randomSelect==1);
    scene.chartSession->randomIndices={0, -1, 1};
    scene.refreshLongNoteModeClearRankViews();
    assert(!scene.selectedChartRecord && scene.list.selectedIndex==-1);
    assert(scene.list.size()==4 && scene.list.get(1).randomSelect==2);
    scene.chartSession->randomIndices={0, 0, 1};
    scene.refreshLongNoteModeClearRankViews();
    assert(scene.list.size()==5 && scene.list.get(1).randomSelect==1);
  }
  for (bool releasePages : {true, false}) {
    for (bool selectionWasVisible : {false, true}) {
      MainMenuScene scene;
      for (int i=0; i<20; ++i) {
        ChartMetaRecord chart;
        chart.meta.BmsPath=std::to_string(i)+".bms";
        scene.chartSession->rows.push_back(chart);
      }
      scene.reloadChartList();
      scene.list.selectedIndex=2;
      scene.selectedChartRecord=scene.list.get(2);
      scene.list.scrollOffset=selectionWasVisible ? 150.0F : 1500.0F;
      // MainMenuScene::onPause drops pages before gameplay writes a new score.
      if (releasePages) scene.chartListCache.releasePages();
      auto &rows=scene.chartSession->rows;
      const auto played=rows[2];
      rows.erase(rows.begin()+2);
      rows.insert(rows.begin()+12, played);
      scene.reloadChartList(true);
      assert(scene.list.selectedIndex==12 &&
             "a score reorder must retain the played chart, not its old row index");
      assert(scene.list.get(scene.list.selectedIndex).meta.BmsPath=="2.bms");
      assert(scene.list.scrollOffset==(selectionWasVisible ? 1150.0F : 1500.0F) &&
             "visible selection follows its new row; offscreen selection does not steal scroll");
      rows[12].meta.Title="Updated chart metadata";
      scene.chartListCache.releasePages();
      scene.list.onSelected=[&](const ChartMetaRecord &record, int) {
        scene.selectedChartRecord=record;
      };
      scene.reselectCurrentChart();
      assert(scene.selectedChartRecord->meta.Title=="Updated chart metadata" &&
             "a matching visible row refreshes retained metadata after library changes");
    }
  }
  for (bool emptyList : {false, true}) {
    MainMenuScene scene;
    ChartMetaRecord played, other;
    played.meta.BmsPath="played.bms";
    other.meta.BmsPath="other.bms";
    scene.chartSession->rows={played, other};
    scene.reloadChartList();
    scene.list.selectedIndex=0;
    scene.selectedChartRecord=played;
    scene.list.onSelected=[&](const ChartMetaRecord &record, int) {
      scene.previewPath=record.meta.BmsPath;
      scene.selectedChartRecord=record;
    };
    scene.chartListCache.releasePages();
    // The updated clear/score rank removes the played chart from this query.
    scene.chartSession->rows=emptyList ? std::vector<ChartMetaRecord>{}
                                       : std::vector<ChartMetaRecord>{other};
    scene.reloadChartList(true);
    assert(scene.list.selectedIndex==-1);
    scene.refreshSelectedChartDetails();
    assert(scene.details.path=="played.bms" && "filtered-out selection keeps its details");
    scene.reselectCurrentChart();
    assert(scene.previewPath=="played.bms" && "resume restores the retained chart preview");
    scene.openReplayRecordsForSelection();
    assert(scene.records.path=="played.bms" && "Records targets the retained chart outside the filter");
    scene.startSelectedChart();
    assert(scene.playedPath=="played.bms" && "Play targets the retained chart outside the filter");
  }
}
