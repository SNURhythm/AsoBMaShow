#pragma once

#include "ScrollMomentum.h"
#include "View.h"

class ScrollView : public View {
public:
  ScrollView() = default;
  ScrollView(int x, int y, int width, int height) : View(x, y, width, height) {}
  ~ScrollView() override;

  void setContentView(View *view);
  void refreshContentLayout();
  float getScrollOffset() const { return scrollOffset; }

protected:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;
  void onLayout() override;
  void onMove(int newX, int newY) override;
  void onResize(int newWidth, int newHeight) override;

private:
  View *contentView = nullptr;
  float scrollOffset = 0.0f;
  bool mousePressedInside = false;
  bool mouseDragging = false;
  bool cancelMouseClick = false;
  int pressedMouseUiX = 0;
  int pressedMouseUiY = 0;
  int lastMouseUiY = 0;
  bool touchPressedInside = false;
  bool touchDragging = false;
  bool cancelTouchClick = false;
  float pressedTouchUiX = 0.0f;
  float pressedTouchUiY = 0.0f;
  float lastTouchUiY = 0.0f;
  SDL_FingerID activeTouchId = -1;
  ScrollMomentum touchMomentum;

  bool isInside(float uiX, float uiY) const;
  void clampScrollOffset();
  bool scrollBy(float delta);
  void updateContentPosition();
  bool eventToUi(const SDL_MouseButtonEvent &event, int &uiX, int &uiY) const;
  bool eventToUi(const SDL_MouseMotionEvent &event, int &uiX, int &uiY) const;
  void eventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                 float &uiY) const;
};
