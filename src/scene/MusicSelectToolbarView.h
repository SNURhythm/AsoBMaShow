#pragma once

#include "../ApplicationUiState.h"
#include "../view/View.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

class TextView;
class Button;
class GoalTargetBubble;

enum class MusicSelectToolbarControl {
  Drag,
  ChartMenu,
  MoreMenu,
  ChartViewer,
  ChartRecords,
  Rankings,
  RevealChart,
  MusicPlayer,
  Tasks,
  PlayOptions,
  IrUploads,
  Kombo,
  Goal,
  Settings,
  Collapse,
  Expand,
};

struct MusicSelectToolbarCallbacks {
  std::function<void()> openChartMenu;
  std::function<void()> openMoreMenu;
  std::function<void()> openChartViewer;
  std::function<void()> openChartRecords;
  std::function<void()> openRankings;
  std::function<void()> revealChart;
  std::function<void()> openMusicPlayer;
  std::function<void()> openTasks;
  std::function<void()> openPlayOptions;
  std::function<void()> openIrUploads;
  std::function<void()> openKombo;
  std::function<void()> openGoal;
  std::function<void()> openSettings;
  std::function<void(MusicSelectToolbarState)> persist;
};

struct MusicSelectToolbarRenderedControl {
  MusicSelectToolbarControl control = MusicSelectToolbarControl::Drag;
  std::uint32_t codepoint = 0;
  TextView *icon = nullptr;
  TextView *label = nullptr;
  Button *button = nullptr;
};

class MusicSelectToolbarView final : public View {
public:
  static std::unique_ptr<MusicSelectToolbarView>
  Create(MusicSelectToolbarState state,
         MusicSelectToolbarCallbacks callbacks, int viewportWidth,
         int viewportHeight);

  const MusicSelectToolbarState &state() const noexcept { return state_; }
  const std::vector<MusicSelectToolbarRenderedControl> &controls() const
      noexcept {
    return controls_;
  }

  void applyState(MusicSelectToolbarState state);
  void activateControl(MusicSelectToolbarControl control);
  void setControlEnabled(MusicSelectToolbarControl control, bool enabled);
  [[nodiscard]] bool isControlEnabled(MusicSelectToolbarControl control) const;
  void setViewportSize(int width, int height);
  void setGoalState(bool present, bool achieved, int lampRank, std::string lamp, std::string score,
                    bool recommended = false, std::string coverage = {});

private:
  MusicSelectToolbarView(MusicSelectToolbarState state,
                         MusicSelectToolbarCallbacks callbacks,
                         int viewportWidth, int viewportHeight);
  bool handleEventsImpl(SDL_Event &event) override;
  void onPointerInputCancelled() override;
  void onPointerEventConsumed(const SDL_Event &event) override;
  void rebuild();
  void onLanguageChanged() override;
  void onLayout() override;
  void onMove(int, int) override;
  void refreshGoalAppearance();
  void placeGoalBubble();
  void requestMode(MusicSelectToolbarMode mode);
  void persist();
  void place(float x, float y);
  bool insideDragHandle(float x, float y) const;

  MusicSelectToolbarState state_;
  MusicSelectToolbarCallbacks callbacks_;
  std::vector<MusicSelectToolbarRenderedControl> controls_;
  std::set<MusicSelectToolbarControl> disabledControls_;
  GoalTargetBubble *goalBubble_ = nullptr;
  View *goalRecommendationBadge_ = nullptr;
  bool goalPresent_ = false, goalAchieved_ = false;
  bool goalRecommended_ = false;
  int goalLampRank_ = -1;
  std::string goalLamp_, goalScore_, goalCoverage_;
  int viewportWidth_ = 0;
  int viewportHeight_ = 0;
  bool mouseDragging_ = false;
  SDL_FingerID touchDragging_ = -1;
  float dragPointerOffsetX_ = 0.0F;
  float dragPointerOffsetY_ = 0.0F;
};
