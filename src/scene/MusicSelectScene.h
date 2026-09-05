#pragma once

#include "MusicSelectToolbarView.h"
#include "MainMenuPlayOptionsModal.h"
#include "MainMenuProfileSelections.h"
#include "ReplayRecordsModal.h"
#include "../ReplayVideoExporter.h"
#include "Scene.h"
#include "../audio/SkinSystemSoundService.h"
#include "../music_select/MusicSelectBarManager.h"
#include "../music_select/MusicSelectEventController.h"
#include "../music_select/MusicSelectInputBindingAdapter.h"
#include "../music_select/MusicSelectInputProcessor.h"
#include "../music_select/MusicSelectExternalActions.h"
#include "../music_select/MusicSelectPreview.h"
#include "../music_select/MusicSelectRanking.h"
#include "../music_select/MusicSelectRepositoryProjection.h"
#include "../music_select/MusicSelectSearchHistory.h"
#include "../ir/IrRankingModels.h"
#include "../ir/IrExternalUrlService.h"
#include "../repositories/ScoreRepositoryModels.h"
#include "../skin/GameplaySkinActivationRequest.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/beatoraja/MusicSelectSkinSession.h"
#endif

#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

class BlockingOverlayView;
class OverlayPortal;
class DecideLoadingOverlay;
class ChartPreloadWorker;
class ResultRecordListView;
class TextInputBox;
class TextView;
struct SkinGameplayChartGraphState;
class MusicSelectScene final : public Scene {
public:
  MusicSelectScene(ApplicationContext &,
                   skin::GameplaySkinActivationRequest);

  void init() override;
  void onPause() override;
  void onResume() override;
  EventHandleResult handleEvents(SDL_Event &) override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;

protected:
  bool renderViewBeforeScene(const View *) const override { return false; }

private:
  void reloadLibrary(bool preserveDirectory = true);
  void syncResolvedFilters();
  [[nodiscard]] std::int64_t elapsedMicros() const;
  void selectedBarMoved();
  void updateRanking();
  void setRanking(MusicSelectRankingSnapshot);
  void setPanelState(int);
  skin::MusicSelectSkinFrame makeFrame() const;
  void consumeActions();
  void consumeLogicalInput();
  void applyInputAction(const MusicSelectInputAction &);
  void startInputListening();
  void stopInputListening();
  void executeEvent(const skin::MusicSelectSkinAction &);
  void launchSelected(bool autoplay = false, bool practice = false);
  void tryCompletePendingPreloadLaunch();
  void launchCourse(const MusicSelectBar &, bool autoplay);
  void launchSelectedDirectoryAutoplay();
  void startPreloadForSelection();
  void stopPreloadWorker();
  void showDecideOverlay(const ChartMetaRecord &record);
  void hideDecideOverlay();
  [[nodiscard]] bool reusePreloadedChart(
      const ChartMetaRecord &, bms_parser::Chart *&,
      play_options::PlayOptionReplayInfo &, int &lnMode);
  void launchSelectedReplay(int slot);
  void launchCourseReplay(const MusicSelectBar &, int slot,
                          const MusicSelectBarManagerSnapshot &);
  void changeSelectedFavorite(bool song, int direction);
  void openSelected();
  [[nodiscard]] bool openDirectory(const MusicSelectBar &);
  [[nodiscard]] bool loadDirectoryChildren(const MusicSelectBar &);
  void openSameFolder();
  void copySelectedHash(bool sha256);
  void closeDirectory();
  void buildSearchPrompt();
  void showSearchPrompt();
  void hideSearchPrompt();
  void search(std::string text);
  void enterError(std::vector<skin::SkinDiagnostic>);
  void buildErrorView();
  void openChartViewer();
  void openChartRecords();
  void revealChart();
  void openMusicPlayer();
  void openTasks();
  void openPlayOptions();
  void openIrUploads();
  void openSettings();
  void syncToolbar();
  void persistToolbar(MusicSelectToolbarState);
  [[nodiscard]] PlayOptionsPanelState playOptionsState() const;
  void updatePlayOptions(
      const std::function<void(main_menu_profile::Selections &)> &);
  void refreshPlayOptionsModal();
  ReplayRecordsModalCallbacks makeRecordsModalCallbacks();
  std::vector<ResultRecordSummary>
  loadRecordsForSelector(const ChartMetaRecord &);
  void launchChartReplay(const ChartMetaRecord &,
                         const ModernChartResultRecord &);
  void launchCourseReplay(const ChartMetaRecord &,
                          const ModernCourseResultRecord &);
  void launchAutoPlay(const ChartMetaRecord &);
  void launchChartReplayExport(const ChartMetaRecord &,
                               const ModernChartResultRecord &,
                               ReplayVideoExportOptions);
  void launchCourseReplayExport(const ModernCourseResultRecord &,
                                ReplayVideoExportOptions);
  void launchAutoPlayExport(const ChartMetaRecord &,
                            ReplayVideoExportOptions);
  void applyRecordsExportProgress();
  void applyRecordsExportResult();
  void showTasksModal();
  void refreshTasksModal();
  [[nodiscard]] std::string tasksModalTextSnapshot() const;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  [[nodiscard]] bool
  activateSkin(skin::GameplaySkinActivationRequest);
  [[nodiscard]] bool reactivateSkinAfterSettings();
  void buildSkinLoadingView();
  void finalizeSkinPreparationIfReady();
  void cancelSkinPreparation();
  void updateSelectedChartAnalysis();
  void cancelSelectedChartAnalysis();
  [[nodiscard]] TextInputBox *skinTextInputForSize(int);
  void beginSkinTextEditing(
      const skin::MusicSelectSkinPointerResult::StringFocus &);
  void commitSkinTextEditing(const std::string &);
  void applySkinPointerResult(
      const skin::MusicSelectSkinPointerResult &, MusicSelectPointerOrigin);
  [[nodiscard]] bool queueSkinPointerEvent(SDL_Event &);
#endif

  skin::GameplaySkinActivationRequest activationRequest_;
  std::string selectedSkinPath_;
  std::optional<ChartRepository::Session> chartSession_;
  ScoreBestCache scoreCache_;
  ScoreClearRankCache clearRankCache_;
  PlayerScoreHistorySnapshot playerHistory_;
  RecentScoreImprovements recentScoreImprovements_;
  bool recentScoreImprovementsLoaded_ = false;
  MusicSelectRepositoryMetadata repositoryMetadata_;
  MusicSelectBarManager bars_;
  MusicSelectInputProcessor inputProcessor_{{}};
  std::atomic_bool recordsExportInProgress_{false};
  struct PendingRecordsExportProgress {
    double fraction = 0.0;
    std::string message;
  };
  std::mutex recordsExportProgressMutex_;
  std::optional<PendingRecordsExportProgress> pendingRecordsExportProgress_;
  std::mutex recordsExportResultMutex_;
  std::optional<ReplayVideoExportResult> pendingRecordsExportResult_;
  // Declared after the mutexes and result state it guards so reverse-order
  // member destruction joins the worker before those guards are torn down.
  std::jthread recordsExportThread_;
  MusicSelectPreviewController previewController_;
  std::unique_ptr<MusicSelectPreviewAudioService> previewAudio_;
  std::unique_ptr<skin::SkinSystemSoundService> systemSound_;
  // The launch parse runs off the UI thread so a large archive chart does not
  // freeze the selector between Start and the gameplay scene. Completion is
  // published on the next deferred pass.
  std::jthread launchThread_;
  std::atomic_bool launchCancelled_{false};
  // Background preload of the selected chart's parse + jukebox load so Start
  // is near-instant even for a heavy archive chart. A single worker thread
  // lives for the scene and processes the latest selection request (debounced
  // and latest-wins), so scrolling never joins a previous in-flight load on
  // the UI thread.
  ChartPreloadWorker *preloadWorker_ = nullptr;
  mutable std::mutex preloadMutex_;
  std::unique_ptr<bms_parser::Chart> preloadedChart_;
  std::filesystem::path preloadedPath_;
  // When Start is pressed while the preload worker is still loading the same
  // chart, keep the worker running and launch from its result instead of
  // stopping it and re-doing parse + jukebox from scratch.
  struct PendingPreloadLaunch {
    ChartMetaRecord record;
    bool autoplay = false;
    bool practice = false;
  };
  std::optional<PendingPreloadLaunch> pendingLaunch_;
  MusicSelectSearchHistory searchHistory_;
  std::unique_ptr<MusicSelectInputBindingAdapter> inputBindingAdapter_;
  std::uint64_t inputSubscription_ = 0;
  std::uint64_t inputDeviceSubscription_ = 0;
  std::uint64_t libraryRevision_ = 0;
  std::uint64_t frameSerial_ = 0;
  int sortIndex_ = 0;
  int panelState_ = 0;
  int selectedReplay_ = -1;
  int currentRivalIndex_ = -1;
  std::int64_t songBarChangeMicros_ = 0;
  struct CachedRanking {
    MusicSelectRankingSnapshot snapshot;
    std::int64_t updatedUnixMillis = 0;
  };
  MusicSelectRankingSnapshot ranking_;
  std::optional<ir::IrRankingRequest> rankingRequest_;
  std::map<std::string, CachedRanking, std::less<>> rankingCache_;
  std::string rankingCacheKey_;
  std::uint64_t rankingGeneration_ = 0;
  std::unique_ptr<ir::IrExternalUrlService> irExternalUrlService_;
  std::uint64_t irExternalUrlGeneration_ = 0;
  std::uint64_t rankingRevision_ = 0;
  std::uint64_t irAccountEvidenceRevision_ = 0;
  std::int64_t rankingLoadAtMicros_ = -1;
  int rankingOffset_ = 0;
  std::array<std::optional<std::int64_t>, 3> rankingTimerMicros_{};
  std::optional<std::int64_t> startInputMicros_;
  std::array<std::optional<std::int64_t>, 6> panelOnMicros_{};
  std::array<std::optional<std::int64_t>, 6> panelOffMicros_{};
  std::chrono::steady_clock::time_point started_;
  std::vector<skin::SkinDiagnostic> diagnostics_;
  MusicSelectToolbarView *toolbar_ = nullptr;
  BlockingOverlayView *searchOverlay_ = nullptr;
  TextInputBox *searchInput_ = nullptr;
  View *modalLayer_ = nullptr;
  OverlayPortal *modalOverlayPortal_ = nullptr;
  std::unique_ptr<MainMenuPlayOptionsModal> playOptionsModal_;
  std::unique_ptr<ReplayRecordsModal> recordsModal_;
  BlockingOverlayView *tasksModal_ = nullptr;
  TextView *tasksModalText_ = nullptr;
  DecideLoadingOverlay *decideOverlay_ = nullptr;
  std::uint64_t displayedTasksRevision_ = 0;
  std::uint64_t displayedTaskProgressRevision_ = 0;
  View *errorView_ = nullptr;
  bool failed_ = false;
  bool launching_ = false;
  bool reactivateSkinOnResume_ = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  std::unique_ptr<skin::MusicSelectSkinSession> skinSession_;
  std::future<skin::MusicSelectSkinSessionPreparationResult>
      skinPreparation_;
  std::stop_source skinPreparationStop_;
  View *skinLoadingView_ = nullptr;
  struct SelectedChartAnalysis {
    std::uint64_t generation = 0;
    std::atomic_bool cancelled{false};
    std::atomic_bool finished{false};
    std::mutex mutex;
    std::shared_ptr<const SkinGameplayChartGraphState> result;
  };
  std::shared_ptr<SelectedChartAnalysis> selectedChartAnalysis_;
  std::uint64_t selectedChartAnalysisGeneration_ = 0;
  std::shared_ptr<const SkinGameplayChartGraphState>
      selectedChartInformation_;
  bool selectedChartAnalysisStarted_ = false;
  TextInputBox *skinTextInput_ = nullptr;
  std::map<int, TextInputBox *> skinTextInputs_;
  std::optional<skin::SkinStringWriterId> activeSkinStringWriter_;
  MusicSelectTouchGesture skinTouchGesture_;
#endif
};
