#pragma once

#include "../ApplicationUiState.h"
#include "../view/View.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class TextView;

enum class MusicSelectToolbarControl {
  Drag,
  MusicPlayer,
  Tasks,
  IrUploads,
  Settings,
  Collapse,
  Expand,
  Hide,
};

struct MusicSelectToolbarCallbacks {
  std::function<void()> openMusicPlayer;
  std::function<void()> openTasks;
  std::function<void()> openIrUploads;
  std::function<void()> openSettings;
  std::function<void(MusicSelectToolbarState)> persist;
};

struct MusicSelectToolbarRenderedControl {
  MusicSelectToolbarControl control = MusicSelectToolbarControl::Drag;
  std::uint32_t codepoint = 0;
  TextView *icon = nullptr;
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
  void setViewportSize(int width, int height);

private:
  MusicSelectToolbarView(MusicSelectToolbarState state,
                         MusicSelectToolbarCallbacks callbacks,
                         int viewportWidth, int viewportHeight);
  bool handleEventsImpl(SDL_Event &event) override;
  void rebuild();
  void requestMode(MusicSelectToolbarMode mode);
  void persist();
  void place(float x, float y);
  bool insideDragHandle(float x, float y) const;

  MusicSelectToolbarState state_;
  MusicSelectToolbarCallbacks callbacks_;
  std::vector<MusicSelectToolbarRenderedControl> controls_;
  int viewportWidth_ = 0;
  int viewportHeight_ = 0;
  bool mouseDragging_ = false;
  SDL_FingerID touchDragging_ = -1;
  float dragPointerOffsetX_ = 0.0F;
  float dragPointerOffsetY_ = 0.0F;
};
