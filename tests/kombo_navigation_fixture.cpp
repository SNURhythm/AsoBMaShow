#include <algorithm>
#include <cassert>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace i18n { std::string tr(const char *key) { return key; } }
struct View {
  bool visible=false;
  std::string value;
  void setVisible(bool v) { visible=v; }
  void setEditingText(const std::string &v) { value=v; }
  View *findViewByName(const char *) { return this; }
};
struct MainMenuScene {
  enum class Tab { Play, Course, Kombo } active=Tab::Kombo;
  struct LibraryFolderItem {
    enum class Type { AllSongs, Assignments };
    std::string key,label;
    Type type=Type::AllSongs;
  } activeFolder;
  std::optional<int> temporaryChartFolder=1,chartDifficultyRangeTableId=3;
  std::string searchText="unrelated",chartBpmMinText="200",chartBpmMaxText="220";
  int chartRecordFilters=7,reloads=0;
  bool chartKeyModeDropdownOpen=true,chartClearMarkDropdownOpen=true,chartScoreRankDropdownOpen=true;
  bool chartDifficultyMinDropdownOpen=true,chartDifficultyMaxDropdownOpen=true;
  std::set<std::string> expandedLibraryFolders;
  View search,root;
  View *searchBox=&search,*rootLayout=&root;
  void selectTab(Tab t) { active=t; }
  void reloadFolderItems() {
    assert(active==Tab::Play && expandedLibraryFolders.contains("container:assignments"));
    if(activeFolder.key=="container:assignment:deleted") activeFolder={.key="all"};
  }
  void reloadChartListForFolderSelection() {
    assert(!temporaryChartFolder && !chartDifficultyRangeTableId && searchText.empty());
    assert(chartRecordFilters==0 && search.value.empty() && root.visible);
    assert(chartBpmMinText.empty() && chartBpmMaxText.empty());
    assert(!chartKeyModeDropdownOpen && !chartClearMarkDropdownOpen && !chartScoreRankDropdownOpen);
    assert(!chartDifficultyMinDropdownOpen && !chartDifficultyMaxDropdownOpen);
    ++reloads;
  }
  void openKomboAssignment(const std::string &);
};
MAIN_NAVIGATION
struct Id { std::string value; };
struct MusicSelectBar { Id id; };
MusicSelectBar musicSelectAssignmentsRoot(std::string) { return {{"container:assignments"}}; }
struct Bars {
  struct Config { std::string modeFilter,difficultyFilter,sortId; } config;
  std::vector<std::string> selected;
  bool rootAvailable=true;
  void configure(Config c) { config=c; }
  bool select(const Id &id) {
    if(!rootAvailable)return false;
    selected.push_back(id.value);return true;
  }
  std::vector<MusicSelectBar> childrenOf(const Id &) {
    return {{{"container:assignment:first"}},{{"container:assignment:second"}}};
  }
};
struct MusicSelectScene {
  struct { struct { std::string skinSortId="TITLE"; } settings; } context;
  Bars bars_;
  std::vector<std::string> opened;
  bool synced=false;
  bool openDirectory(const MusicSelectBar &bar) { opened.push_back(bar.id.value);return true; }
  void syncResolvedFilters() { synced=true; }
  void openKomboAssignment(const std::string &);
};
CUSTOM_NAVIGATION
int main() {
  MainMenuScene native;
  native.openKomboAssignment("second");
  assert(native.activeFolder.key=="container:assignment:second" && native.reloads==1);
  native.openKomboAssignment("deleted");
  assert(native.activeFolder.key=="container:assignments" && native.reloads==2);
  MusicSelectScene custom;
  custom.openKomboAssignment("second");
  assert((custom.opened==std::vector<std::string>{"container:assignments","container:assignment:second"}));
  assert(custom.bars_.config.modeFilter=="ALL" && custom.bars_.config.difficultyFilter=="ALL" && custom.synced);
  custom.opened.clear();custom.openKomboAssignment("deleted");
  assert((custom.opened==std::vector<std::string>{"container:assignments"}));
  custom.opened.clear();custom.bars_.rootAvailable=false;custom.openKomboAssignment("second");
  assert(custom.opened.empty());
}
