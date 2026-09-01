#pragma once

#include "MusicSelectToolbarView.h"
#include "Scene.h"
#include "../music_select/MusicSelectBarManager.h"
#include "../repositories/ScoreRepositoryModels.h"
#include "../skin/GameplaySkinActivationRequest.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/beatoraja/MusicSelectSkinSession.h"
#endif

#include <chrono>
#include <optional>
#include <vector>

class MusicSelectScene final : public Scene {
public:
  MusicSelectScene(ApplicationContext &,
                   skin::GameplaySkinActivationRequest);

  void init() override;
  void onResume() override;
  EventHandleResult handleEvents(SDL_Event &) override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;

protected:
  bool renderViewBeforeScene(const View *) const override { return false; }

private:
  void reloadLibrary();
  skin::MusicSelectSkinFrame makeFrame() const;
  void consumeActions();
  void executeEvent(const skin::MusicSelectSkinAction &);
  void launchSelected(bool autoplay = false, bool practice = false);
  void openSelected();
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
  MusicSelectBarManager bars_;
  std::uint64_t libraryRevision_ = 0;
  std::uint64_t frameSerial_ = 0;
  std::chrono::steady_clock::time_point started_;
  std::vector<skin::SkinDiagnostic> diagnostics_;
  MusicSelectToolbarView *toolbar_ = nullptr;
  bool failed_ = false;
  bool launching_ = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  std::unique_ptr<skin::MusicSelectSkinSession> skinSession_;
#endif
};
