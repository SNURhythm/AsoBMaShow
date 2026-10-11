#pragma once

#include "ColorPickerModel.h"
#include "View.h"

class ColorPickerView final : public View {
public:
  using Callback = std::function<void(color_picker::Hsv, bool finished)>;
  ColorPickerView(color_picker::Hsv initial, Callback onChanged);
  [[nodiscard]] color_picker::Hsv value() const { return current; }

protected:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;
  void onPointerEventConsumed(const SDL_Event &event) override;
  void onPointerInputCancelled() override;

private:
  enum class Area { None, SaturationValue, Hue };
  color_picker::Hsv current;
  Callback onChanged;
  Area dragging = Area::None;
  bool mouseDragging = false;
  std::optional<SDL_FingerID> touch;
  [[nodiscard]] Area areaAt(float x, float y) const;
  void updatePointer(float x, float y);
  void finish();
};
