#pragma once

#include "OverlayPortal.h"
#include "View.h"

#include <functional>
#include <string>
#include <vector>

class ContextMenuView final : public View {
public:
  struct Action {
    std::string id;
    std::string label;
    bool enabled = true;
  };

  struct Callbacks {
    std::function<void(bool)> onOpenChanged;
    std::function<void(const std::string &)> onActionSelected;
  };

  ContextMenuView(OverlayPortal *portal, Callbacks callbacks);
  ~ContextMenuView() override;

  void show(OverlayAnchor anchor, std::vector<Action> actions,
            int menuWidth = 210);
  void dismiss();
  void setViewportSize(int width, int height);
  [[nodiscard]] bool isOpen() const noexcept { return open; }

private:
  OverlayPortal *portal = nullptr;
  Callbacks callbacks;
  OverlayAnchor anchor;
  std::vector<Action> actions;
  View *panel = nullptr;
  int viewportWidth = 0;
  int viewportHeight = 0;
  int requestedMenuWidth = 210;
  bool open = false;

  void rebuildActions();
  void updatePlacement();
  void dispatchAction(const std::string &id);
  void handlePointerDown(float x, float y);
  [[nodiscard]] bool pointInsideAnchor(float x, float y) const;
  [[nodiscard]] bool pointInsidePanel(float x, float y) const;
  bool handleEventsImpl(SDL_Event &event) override;
};
