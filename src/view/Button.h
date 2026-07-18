//
// Created by XF on 8/25/2024.
//

#pragma once
#include "View.h"
#include <functional>
#include <memory>
#include <string>
class Button : public View {
private:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;
  void onThemeChanged() override;

private:
  std::function<void()> onClickListener;
  std::unique_ptr<View> contentView;
  bool mousePressedInside = false;
  bool isHovered = false;
  bool enabled = true;
  bool selected = false;
  SDL_FingerID activeTouchId = -1;
  Color normalBackgroundColor = Color(0, 0, 0, 0);
  Color hoverBackgroundColor = Color(0, 0, 0, 0);
  Color pressedBackgroundColor = Color(0, 0, 0, 0);
  Color normalBorderColor = Color(0, 0, 0, 0);
  Color hoverBorderColor = Color(0, 0, 0, 0);
  Color pressedBorderColor = Color(0, 0, 0, 0);
  ThemeColorProvider normalBackgroundColorProvider;
  ThemeColorProvider hoverBackgroundColorProvider;
  ThemeColorProvider pressedBackgroundColorProvider;
  ThemeColorProvider normalBorderColorProvider;
  ThemeColorProvider hoverBorderColorProvider;
  ThemeColorProvider pressedBorderColorProvider;
  int styleBorderWidth = 0;
  bool hasStyledBackground = false;
  bool hasStyledBorder = false;

public:
  Button() : View() {}
  Button(int x, int y, int width, int height) : View(x, y, width, height) {}
  ~Button() override;

  void onLayout() override;
  void onMove(int newX, int newY) override;
  void onResize(int newWidth, int newHeight) override;
  void propagateThemeChange() override;

  void setOnClickListener(std::function<void()> listener);
  void setEnabled(bool enabled);
  [[nodiscard]] bool isEnabled() const { return enabled; }
  void setSelected(bool selected) { this->selected = selected; }
  [[nodiscard]] bool isSelected() const { return selected; }
  void setContentView(View *view);
  [[nodiscard]] View *getContentView() { return contentView.get(); }
  [[nodiscard]] const View *getContentView() const { return contentView.get(); }
  Button *setBackgroundColors(const Color &normal, const Color &hover,
                              const Color &pressed);
  Button *setThemedBackgroundColors(ThemeColorProvider normal,
                                    ThemeColorProvider hover,
                                    ThemeColorProvider pressed);
  Button *setBorderColors(const Color &normal, const Color &hover,
                          const Color &pressed);
  Button *setThemedBorderColors(ThemeColorProvider normal,
                                ThemeColorProvider hover,
                                ThemeColorProvider pressed);
  Button *setStyledBorderWidth(int width);
};
