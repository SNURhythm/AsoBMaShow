#pragma once

#include "../view/PlayOptionsPanelView.h"

#include <memory>

class OverlayPortal;
class View;

// The Main Menu and skinned selector use the same retained modal rather than
// routing through a separate settings scene.
class MainMenuPlayOptionsModal final {
public:
  static std::unique_ptr<MainMenuPlayOptionsModal>
  Create(View *parent, PlayOptionsPanelCallbacks callbacks,
         OverlayPortal *overlayPortal);

  [[nodiscard]] View *root() const noexcept { return root_; }
  [[nodiscard]] PlayOptionsPanelView *panel() const noexcept { return panel_; }

  void refresh(const PlayOptionsPanelState &);
  void show();
  void hide();
  void resize(int width, int height);

private:
  MainMenuPlayOptionsModal() = default;

  View *root_ = nullptr;
  PlayOptionsPanelView *panel_ = nullptr;
};
