#pragma once

#include "ScrollMomentum.h"
#include "View.h"
#include <memory>

class ScrollView : public View {
public:
  ScrollView() = default;
  ScrollView(int x, int y, int width, int height) : View(x, y, width, height) {}
  ~ScrollView() override;

  void setContentView(View *view);
  ScrollView *setContentPadding(Edge edge, float padding);
  void refreshContentLayout();
  void scrollToBottom();
  float getScrollOffset() const { return scrollOffset; }
  void setScrollOffset(float offset);

protected:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;
  void onLayout() override;
  void onMove(int newX, int newY) override;
  void onResize(int newWidth, int newHeight) override;

private:
  std::unique_ptr<View> contentView;
  float scrollOffset = 0.0f;
  bool mousePressedInside = false;
  bool mouseDragging = false;
  bool mouseCapturedByContent = false;
  bool cancelMouseClick = false;
  int pressedMouseUiX = 0;
  int pressedMouseUiY = 0;
  int lastMouseUiY = 0;
  bool touchPressedInside = false;
  bool touchDragging = false;
  bool touchCapturedByContent = false;
  bool cancelTouchClick = false;
  float pressedTouchUiX = 0.0f;
  float pressedTouchUiY = 0.0f;
  float lastTouchUiY = 0.0f;
  SDL_FingerID activeTouchId = -1;
  ScrollMomentum touchMomentum;
  int contentPaddingLeft = 0;
  int contentPaddingTop = 0;
  int contentPaddingRight = 0;
  int contentPaddingBottom = 0;

  bool isInside(float uiX, float uiY) const;
  int getViewportX() const;
  int getViewportY() const;
  int getViewportWidth() const;
  int getViewportHeight() const;
  int getScrollContentX() const;
  int getScrollContentY() const;
  int getScrollContentWidth() const;
  int getScrollContentHeight() const;
  void updateStoredContentPadding(Edge edge, float padding);
  bool hasScrollableOverflow() const;
  void renderPersistentScrollbar(RenderContext &context) const;
  void clampScrollOffset();
  bool scrollBy(float delta);
  void updateContentPosition();
  bool eventToUi(const SDL_MouseButtonEvent &event, int &uiX, int &uiY) const;
  bool eventToUi(const SDL_MouseMotionEvent &event, int &uiX, int &uiY) const;
  void eventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                 float &uiY) const;
};
