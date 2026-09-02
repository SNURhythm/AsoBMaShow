#pragma once

#include "MusicSelectToolbarView.h"
#include "Scene.h"
#include "../music_select/MusicSelectBarManager.h"
#include "../music_select/MusicSelectEventController.h"
#include "../music_select/MusicSelectInputBindingAdapter.h"
#include "../music_select/MusicSelectInputProcessor.h"
#include "../music_select/MusicSelectPreview.h"
#include "../music_select/MusicSelectRanking.h"
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
#include <map>
#include <optional>
#include <vector>

class BlockingOverlayView;
class TextInputBox;

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
  void reloadLibrary();
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
  void launchCourse(const MusicSelectBar &, bool autoplay);
  void launchSelectedDirectoryAutoplay();
  void launchSelectedReplay(int slot);
  void launchCourseReplay(const MusicSelectBar &, int slot,
                          const MusicSelectBarManagerSnapshot &);
  void changeSelectedFavorite(bool song, int direction);
  void openSelected();
  void openSameFolder();
  void copySelectedHash(bool sha256);
  void closeDirectory();
  void buildSearchPrompt();
  void showSearchPrompt();
  void hideSearchPrompt();
  void search(std::string text);
  void enterError(std::vector<skin::SkinDiagnostic>);
  void buildErrorView();
  void openMusicPlayer();
  void openTasks();
  void openIrUploads();
  void openSettings();
  void syncToolbar();
  void persistToolbar(MusicSelectToolbarState);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  [[nodiscard]] bool
  activateSkin(skin::GameplaySkinActivationRequest);
  [[nodiscard]] bool reactivateSkinAfterSettings();
  [[nodiscard]] TextInputBox *skinTextInputForSize(int);
  void beginSkinTextEditing(
      const skin::MusicSelectSkinPointerResult::StringFocus &);
  void commitSkinTextEditing(const std::string &);
  void applySkinPointerResult(
      const skin::MusicSelectSkinPointerResult &);
  [[nodiscard]] bool queueSkinPointerEvent(SDL_Event &);
#endif

  skin::GameplaySkinActivationRequest activationRequest_;
  std::string selectedSkinPath_;
  std::optional<ChartRepository::Session> chartSession_;
  ScoreBestCache scoreCache_;
  ScoreClearRankCache clearRankCache_;
  PlayerScoreHistorySnapshot playerHistory_;
  RecentScoreImprovements recentScoreImprovements_;
  MusicSelectBarManager bars_;
  MusicSelectInputProcessor inputProcessor_{{}};
  MusicSelectPreviewController previewController_;
  std::unique_ptr<MusicSelectPreviewAudioService> previewAudio_;
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
  MusicSelectTableContext tableContext_;
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
  bool failed_ = false;
  bool launching_ = false;
  bool reactivateSkinOnResume_ = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  std::unique_ptr<skin::MusicSelectSkinSession> skinSession_;
  TextInputBox *skinTextInput_ = nullptr;
  std::map<int, TextInputBox *> skinTextInputs_;
  std::optional<skin::SkinStringWriterId> activeSkinStringWriter_;
#endif
};
