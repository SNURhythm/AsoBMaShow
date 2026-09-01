#pragma once

#include "MusicSelectToolbarView.h"
#include "Scene.h"
#include "../music_select/MusicSelectBarManager.h"
#include "../music_select/MusicSelectEventController.h"
#include "../music_select/MusicSelectInputBindingAdapter.h"
#include "../music_select/MusicSelectInputProcessor.h"
#include "../repositories/ScoreRepositoryModels.h"
#include "../skin/GameplaySkinActivationRequest.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/beatoraja/MusicSelectSkinSession.h"
#endif

#include <chrono>
#include <array>
#include <optional>
#include <vector>

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
  void setPanelState(int);
  skin::MusicSelectSkinFrame makeFrame() const;
  void consumeActions();
  void consumeLogicalInput();
  void applyInputAction(const MusicSelectInputAction &);
  void startInputListening();
  void stopInputListening();
  void executeEvent(const skin::MusicSelectSkinAction &);
  void launchSelected(bool autoplay = false, bool practice = false);
  void openSelected();
  void openSameFolder();
  void copySelectedHash(bool sha256);
  void closeDirectory();
  void enterError(std::vector<skin::SkinDiagnostic>);
  void buildErrorView();
  void openMusicPlayer();
  void openTasks();
  void openIrUploads();
  void openSettings();
  void persistToolbar(MusicSelectToolbarState);

  skin::GameplaySkinActivationRequest activationRequest_;
  std::optional<ChartRepository::Session> chartSession_;
  ScoreBestCache scoreCache_;
  ScoreClearRankCache clearRankCache_;
  PlayerScoreHistorySnapshot playerHistory_;
  MusicSelectBarManager bars_;
  MusicSelectInputProcessor inputProcessor_{{}};
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
  std::optional<std::int64_t> startInputMicros_;
  std::array<std::optional<std::int64_t>, 6> panelOnMicros_{};
  std::array<std::optional<std::int64_t>, 6> panelOffMicros_{};
  std::chrono::steady_clock::time_point started_;
  std::vector<skin::SkinDiagnostic> diagnostics_;
  MusicSelectToolbarView *toolbar_ = nullptr;
  bool failed_ = false;
  bool launching_ = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  std::unique_ptr<skin::MusicSelectSkinSession> skinSession_;
#endif
};
