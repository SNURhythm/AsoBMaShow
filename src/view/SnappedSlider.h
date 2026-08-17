#pragma once

#include "View.h"

#include <functional>

class SnappedSlider final : public View {
public:
  struct State {
    int minimum = 0;
    int maximum = 100;
    int step = 1;
    int value = 0;
    bool enabled = true;
  };

  explicit SnappedSlider(std::function<void(int)> onValueChanged = {});

  void refresh(const State &state);
  [[nodiscard]] int value() const { return current.value; }

protected:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;

private:
  State current;
  std::function<void(int)> onValueChanged;
  bool mouseDragging = false;
  bool hovered = false;
  SDL_FingerID activeTouchId = -1;

  [[nodiscard]] bool contains(float x, float y) const;
  [[nodiscard]] int snappedValueForX(float x) const;
  void updateFromX(float x);
  void cancelInteraction();
};
