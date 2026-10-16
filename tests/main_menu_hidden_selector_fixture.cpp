// Production update/invalidations with controlled repository, media and view boundaries.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 0
namespace rendering { int window_width=1920,window_height=1080; bool compactMenuCanvasActive=false; }
struct Safe { int top=0,left=0,bottom=0,right=0; };
Safe getSafeAreaInsetsUi(){return {};}
constexpr int kRootPadding=0,kMenuHeaderHeight=80;
namespace i18n { using Text=std::string; }
struct ChartMetaRecord { struct {std::filesystem::path BmsPath;} meta; };
struct Parsed {struct {std::filesystem::path BmsPath;} Meta;};
namespace main_menu_library {bool findBmsSelectionHandoffAllowed(int,int,int,const ChartMetaRecord&){return true;}}
namespace archive_file {void appendDebugLogLine(const char*){}}
struct View {
 void setVisible(bool){} bool getVisible(){return false;} bool isVisible(){return false;}
 bool coversTabHeader(){return false;} void refreshLanguageIfNeeded(){} void update(float=0){} void handleDeferred(){}
 View* findViewByName(const char*){return nullptr;} int getHeight() const {return 0;}
 void updatePreviewGlow(float,float,std::function<int()>){}
};
struct ImageView { static inline int drops=0; static void dropAllCache(){++drops;} };
struct Completion {std::filesystem::path chartPath;int targetIdentity=0,selectionGeneration=0;};
struct Tasks {std::vector<Completion> completions; std::vector<Completion> takeDownloadedIndexCompletions(){auto r=std::move(completions);completions.clear();return r;}};
struct Context {
 std::atomic_bool chartLibraryFoldersReloadRequested=false,chartLibraryListReloadRequested=false;
 struct Repository {std::uint64_t revision=1;std::uint64_t GetRevision(){return revision;}std::uint64_t GetLibraryRevision(){return revision;}} scoreRepository,chartRepository;
 Tasks *chartLibraryTasks=nullptr;
 struct Player {bool ProcessNativeControlEvents(std::string&){return false;}bool ConsumeNativeControlStatus(std::string&){return false;}} musicPlayer;
};
struct MainMenuScene {
 enum class Tab {Play,Course,Music,Ir}; Tab activeTab_=Tab::Music;
 enum class AutoSelectionPreview {Suppress,Load};
 Context context;std::mutex selectedChartMutex,findBmsSelectionHandoffMutex;
 std::shared_ptr<Parsed> selectedChart;std::optional<ChartMetaRecord> selectedChartRecord;
 struct Player {float GetOutputLevel(){return 0;}int GetOutputSpectrum(){return 0;}}previewPlayer_;
 View *chartDetailsView_=nullptr,*irTab_=nullptr,*musicTab_=nullptr,*rootLayout=nullptr;
 View *archiveUnzipModal_=nullptr,*recordsModal_=nullptr,*parseLogModalRoot=nullptr,*musicModalRoot=nullptr,*tasksModalRoot=nullptr,*rankingsModal=nullptr;
 std::atomic_bool tasksModalOpenRequested=false;
 struct PendingFindBmsSelectionHandoff {std::filesystem::path chartPath;int targetIdentity,selectionGeneration;};
 std::optional<PendingFindBmsSelectionHandoff> pendingFindBmsSelectionHandoff;
 std::optional<std::filesystem::path> pendingSelectChartPath;
 int chartSelectionGeneration=0,scoreReads=0,folderReloads=0,chartReloads=0,cacheViewRefreshes=0,taskShows=0,handoffs=0;
 std::uint64_t libraryRevision=1,scoreClearRanksRevision=1;
 int scoreClearRanks=0,scoreBestScores=0,folderClearData=0;
 std::string musicStatusMessage;
 void update(float);void applyPendingUiUpdates();void refreshScoreClearRanksIfNeeded();
 bool applyPendingTabSwitch(){return true;}bool selectorTabActive(){return activeTab_==Tab::Play||activeTab_==Tab::Course;}
 bool archiveUnzipInProgress(){return false;}
 std::optional<ChartMetaRecord> selectedRecordSnapshot(){return selectedChartRecord;}
 void showTasksModal(){++taskShows;}
 void selectChartByPathAfterReload(const std::filesystem::path&,AutoSelectionPreview){++handoffs;}
 std::optional<std::string> reloadScoreClearRanks(){++scoreReads;scoreClearRanksRevision=context.scoreRepository.GetRevision();return {};}
 void reloadFolderItems(bool){++folderReloads;}void reloadChartList(bool){++chartReloads;}
 void refreshLongNoteModeClearRankViews(){++cacheViewRefreshes;}
 std::optional<std::string> refreshScoreClearRankViews(){reloadScoreClearRanks();refreshLongNoteModeClearRankViews();return {};}
 float portraitDetailsHeight(float){return 0;}
 void updatePreviewVisuals(){}void refreshIrRecordListIfNeeded(){}void refreshTasksButton(){}void applyFindBmsUpdates(){}
 void applyReplayLoadCompletion(){}void applyReplayExportProgress(){}void applyReplayExportResult(){}void applyReplayFileDocumentHandoff(){}
 void applyParseLogDocumentHandoff(){}void observeReplayIrServiceRevisions(){}void refreshParseLogModal(){}void refreshMusicModal(){}
 void refreshTasksModal(){}void updatePanelLayout(){}
};
SCENE_METHODS
int main(){
 for(auto tab:{MainMenuScene::Tab::Music,MainMenuScene::Tab::Ir}){
  MainMenuScene scene;scene.activeTab_=tab;ImageView::drops=0;
  for(int frame=0;frame<600;++frame){
   if(frame%60==0){scene.context.chartLibraryFoldersReloadRequested=true;scene.context.chartLibraryListReloadRequested=true;}
   scene.update(0);
  }
  std::cout<<(tab==MainMenuScene::Tab::Music?"MUSIC":"IR")<<" hidden 10 folder/import completions over 600 frames: scoreReload="<<scene.scoreReads<<" folderReload="<<scene.folderReloads<<" chartReload="<<scene.chartReloads<<" imageCacheDrop="<<ImageView::drops<<'\n';
  assert(scene.scoreReads==0&&scene.folderReloads==0&&scene.chartReloads==0&&ImageView::drops==0);
  assert(scene.context.chartLibraryFoldersReloadRequested && scene.context.chartLibraryListReloadRequested);
  scene.activeTab_=MainMenuScene::Tab::Play;scene.update(0);
  assert(scene.scoreReads==1&&scene.folderReloads==1&&scene.chartReloads==1&&ImageView::drops==1);
  scene.update(0);assert(scene.scoreReads==1&&scene.chartReloads==1);
  MainMenuScene score;score.activeTab_=tab;
  for(int frame=0;frame<600;++frame){if(frame%60==0)++score.context.scoreRepository.revision;score.update(0);}
  std::cout<<(tab==MainMenuScene::Tab::Music?"MUSIC":"IR")<<" hidden 10 score revisions: scoreReload="<<score.scoreReads<<" viewRefresh="<<score.cacheViewRefreshes<<'\n';
  assert(score.scoreReads==0&&score.cacheViewRefreshes==0);
  score.activeTab_=MainMenuScene::Tab::Play;score.update(0);
  assert(score.scoreReads==1&&score.cacheViewRefreshes==1);
  score.update(0);assert(score.scoreReads==1);
 }
 MainMenuScene modal;modal.tasksModalOpenRequested=true;modal.update(0);
 assert(modal.taskShows==1&&modal.scoreReads==0&&modal.chartReloads==0);
 MainMenuScene completion;Tasks tasks;completion.context.chartLibraryTasks=&tasks;
 completion.selectedChartRecord=ChartMetaRecord{};tasks.completions.push_back({"download.bms",1,1});
 completion.pendingSelectChartPath="unzipped.bms";
 completion.tasksModalOpenRequested=true;completion.context.chartLibraryListReloadRequested=true;
 completion.update(0);
 assert(completion.handoffs==0&&completion.taskShows==1&&tasks.completions.empty());
 assert(completion.pendingSelectChartPath&&completion.pendingFindBmsSelectionHandoff);
 assert(completion.context.chartLibraryListReloadRequested);
 completion.activeTab_=MainMenuScene::Tab::Play;completion.update(0);
 assert(completion.handoffs==2&&!completion.pendingSelectChartPath&&!completion.pendingFindBmsSelectionHandoff);
 std::cout<<"Background completion obligations: taskModal="<<completion.taskShows<<" selectionHandoffs="<<completion.handoffs<<'\n';
}
