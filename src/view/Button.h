//
// Created by XF on 8/25/2024.
//

#pragma once
#include "View.h"
#include <functional>
#include <string>
class Button : public View {
private:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;

private:
  std::function<void()> onClickListener;
  View *contentView = nullptr;
  bool mousePressedInside = false;
  SDL_FingerID activeTouchId = -1;

public:
  Button() : View() {}
  Button(int x, int y, int width, int height) : View(x, y, width, height) {}
  ~Button() override;

  void onLayout() override;

  void setOnClickListener(std::function<void()> listener);
  void setContentView(View *view);
};
