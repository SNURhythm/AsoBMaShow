#include "AppSettings.h"
#include "scene/play/GameplayRuleset.h"
#include "scene/play/RhythmState.h"
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <utility>

PANEL_CONTRACTS
struct View {};
struct PlayOptionsPanelView {
  PlayOptionsPanelState state;
  void refresh(const PlayOptionsPanelState &value) { state = value; }
};
struct MainMenuPlayOptionsModal {
  PlayOptionsPanelView panelView;
  PlayOptionsPanelCallbacks callbacks;
  View rootView;
  static std::unique_ptr<MainMenuPlayOptionsModal> Create(View *, PlayOptionsPanelCallbacks callbacks, View *) {
    auto modal = std::make_unique<MainMenuPlayOptionsModal>();
    modal->callbacks = std::move(callbacks);
    return modal;
  }
  View *root() { return &rootView; }
  PlayOptionsPanelView *panel() { return &panelView; }
  void show() {}
  void hide() {}
};
namespace course_rules { struct { int percent = 100; } kRequiredPlaybackRate; }
inline void SDL_Log(const char *, ...) {}
struct MainMenuScene {
  struct Context {
    AppSettings settings, persisted;
    int saves = 0;
    struct ProfileManager {
      struct Profile { std::string id = "test"; } profile;
      const Profile &activeProfile() const { return profile; }
    } profileManager;
    bool saveSettings() { persisted = settings; ++saves; return true; }
  } context;
  View rootView;
  View *rootLayout = &rootView, *overlayPortal = nullptr, *playOptionsModalRoot = nullptr;
  std::unique_ptr<MainMenuPlayOptionsModal> playOptionsModal;
  PlayOptionsPanelView *playOptionsPanel = nullptr;
  PlayOptionsPanelState profileSelections;
  using EffectivePlayOptionSelection = PlayOptionsPanelState;
  EffectivePlayOptionSelection currentEffectivePlayOptionSelection() const { return profileSelections; }
  bool playbackSelectionLockedForCourse() const { return false; }
  bool currentPlayOptionSelectionAllowed(const std::string &) const { return true; }
  void setGameplayRulesetSelection(GameplayRuleset) {}
  void setGaugeSelection(GaugeType, GaugeAutoShiftMode) {}
  void setGaugeAutoShiftLowerBound(GaugeType) {}
  void setPlayOptionSelection(const std::string &) {}
  void setLongNoteModeSelection(const std::string &) {}
  void setAssistOptionSelection(const std::string &) {}
  void setPlaybackRateSelection(int) {}
  void setPlaybackModeSelection(const std::string &) {}
  void toggleGameplayClubMode() {}
  void setPacemakerTargetSelection(const std::string &) {}
  void refreshGaugeSelectionButtons() { refreshPlayOptionsPanel(); }
  void refreshPlayOptionButtons() { refreshPlayOptionsPanel(); }
  void refreshLongNoteModeButtons() { refreshPlayOptionsPanel(); }
  void refreshAssistOptionButtons() { refreshPlayOptionsPanel(); }
  void refreshPlaybackSelectionControls() { refreshPlayOptionsPanel(); }
  void refreshPacemakerTargetButtons() { refreshPlayOptionsPanel(); }
  void buildPlayOptionsModal();
  void refreshPlayOptionsPanel();
  void showPlayOptionsModal();
  void hidePlayOptionsModal();
};
PRODUCTION_METHODS
int main() {
  MainMenuScene scene;
  const lane_cover::State saved{.laneCoverPercent = 23.4F, .laneCoverEnabled = true,
      .liftEnabled = false, .liftRatio = 0.125F, .hiddenEnabled = true, .hiddenRatio = 0.456F};
  scene.context.settings.presentation().setLaneCoverState(saved);
  scene.buildPlayOptionsModal();
  assert(scene.playOptionsPanel->state.laneCovers == saved && "opening Play Options loads personal lane covers");
  assert(scene.context.saves == 0);
  auto &change = scene.playOptionsModal->callbacks.onLaneCoversChanged;
  assert(change && "main-menu lane cover changes must reach settings persistence");
  const lane_cover::State edited{.laneCoverPercent = 67.8F, .laneCoverEnabled = false,
      .liftEnabled = true, .liftRatio = 0.321F, .hiddenEnabled = false, .hiddenRatio = 0.789F};
  change(edited);
  assert(scene.context.saves == 1);
  assert(scene.context.settings.presentation().laneCoverState() == edited);
  assert(scene.context.persisted.presentation().laneCoverState() == edited);
  scene.refreshPlaybackSelectionControls();
  assert(scene.playOptionsPanel->state.laneCovers == edited && "other option refreshes retain lane covers");
  scene.hidePlayOptionsModal();
  scene.showPlayOptionsModal();
  assert(scene.playOptionsPanel->state.laneCovers == edited);

  MainMenuScene reopened;
  reopened.context.settings = scene.context.persisted;
  reopened.buildPlayOptionsModal();
  assert(reopened.playOptionsPanel->state.laneCovers == edited && "recreated scene uses saved values");
  // Changes from Settings (or another profile's settings) replace the visible values on open.
  reopened.context.settings.presentation().setLaneCoverState(saved);
  reopened.showPlayOptionsModal();
  assert(reopened.playOptionsPanel->state.laneCovers == saved);
}
