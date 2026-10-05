#pragma once

#include "BlockingOverlayView.h"
#include "ColorPickerModel.h"
#include "TextView.h"

class ColorPickerView;
class Button;

class ColorPickerPopup final : public BlockingOverlayView {
public:
  enum class SampleStyle { Solid, Scratch, Outline };
  struct Sample {
    std::string label;
    float width = 160;
    float height = 25;
    SampleStyle style = SampleStyle::Solid;
    std::uint8_t alpha = 255;
  };
  struct Result { bool confirmed; color_picker::Hsv color; };

  ColorPickerPopup(color_picker::Hsv initial, std::vector<Sample> samples);
  void fitToViewport(int width, int height, int left = 0, int top = 0,
                     int right = 0, int bottom = 0);
  [[nodiscard]] const std::optional<Result> &result() const { return outcome; }

private:
  class SampleView;
  color_picker::Hsv draft;
  std::optional<Result> outcome;
  View *panel;
  TextView *title;
  TextView *hex;
  ColorPickerView *picker;
  Button *confirm;
  Button *cancel;
  std::vector<Sample> samples;
  std::vector<View *> sampleFrames;
  std::vector<TextView *> sampleLabels;
  std::vector<SampleView *> sampleViews;
  void refreshDraft();
  bool handleEventsImpl(SDL_Event &event) override;
};
