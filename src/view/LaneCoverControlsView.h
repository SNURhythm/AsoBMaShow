#pragma once

#include "../LaneCover.h"
#include "../i18n/Localization.h"
#include "Button.h"
#include "CheckboxButtonContent.h"
#include "SnappedSlider.h"
#include "TextView.h"
#include "UiTheme.h"

#include <array>
#include <functional>
#include <utility>

// Shared by song options and the live lane/skin preview. Each cover can be
// combined with the others, as in Beatoraja's PlayConfig.
class LaneCoverControlsView final : public View {
public:
  explicit LaneCoverControlsView(std::function<void(const lane_cover::State &)> onChange)
      : onChange_(std::move(onChange)) {
    setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(10);
    constexpr std::array names{"sudden", "hidden", "lift"};
    constexpr std::array labels{"SUDDEN+", "HIDDEN+", "LIFT"};
    for (std::size_t i = 0; i < controls_.size(); ++i) {
      auto &control = controls_[i];
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(10)->setHeight(48)->setFlexShrink(0);
      control.button = new Button(0, 0, 140, 48);
      control.button->setName(std::string("lane-cover-") + names[i]);
      control.button->setCornerRadius(ui_theme::controlRadius());
      control.button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed);
      control.content = new CheckboxButtonContent(labels[i], 18, 20);
      control.content->setThemedColor(ui_theme::textPrimary);
      control.button->setContentView(control.content);
      control.button->setOnClickListener([this, i] {
        bool &value = enabled(i);
        value = !value;
        changed();
      });
      row->addView(control.button);
      control.slider = new SnappedSlider([this, i](int value) {
        if (i == 0) state_.laneCoverPercent = static_cast<float>(value) / 10;
        else if (i == 1) state_.hiddenRatio = static_cast<float>(value) / 1000;
        else state_.liftRatio = static_cast<float>(value) / 1000;
        changed();
      });
      control.slider->setName(std::string("lane-cover-") + names[i] + "-amount");
      control.slider->setFlex(1)->setMinWidth(50);
      row->addView(control.slider);
      control.number = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
      control.number->setWidth(48);
      control.number->setAlign(TextView::RIGHT);
      control.number->setVAlign(TextView::MIDDLE);
      control.number->setThemedColor(ui_theme::textPrimary);
      row->addView(control.number);
      addView(row);
    }
    auto *help = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
    help->setLocalizedText(i18n::message("settings.lane.covers.shortcuts"));
    help->setThemedColor(ui_theme::textSecondary);
    help->setWidthPercent(100);
    help->setWrap(true);
    addView(help);
    refresh({});
  }

  void refresh(const lane_cover::State &state) {
    state_ = state;
    const std::array amounts{state.laneCoverPercent * 10, state.hiddenRatio * 1000,
                              state.liftRatio * 1000};
    for (std::size_t i = 0; i < controls_.size(); ++i) {
      auto &control = controls_[i];
      control.content->setChecked(enabled(i));
      control.button->setSelected(enabled(i));
      const int value = static_cast<int>(std::lround(std::clamp(amounts[i], 0.0F, 1000.0F)));
      control.slider->refresh({.minimum = 0, .maximum = 1000, .step = 1, .value = value});
      control.number->setText(std::to_string(value));
    }
  }

private:
  struct Control {
    Button *button = nullptr;
    CheckboxButtonContent *content = nullptr;
    SnappedSlider *slider = nullptr;
    TextView *number = nullptr;
  };
  bool &enabled(std::size_t i) {
    return i == 0 ? state_.laneCoverEnabled : i == 1 ? state_.hiddenEnabled : state_.liftEnabled;
  }
  void changed() {
    refresh(state_);
    if (onChange_) onChange_(state_);
  }
  lane_cover::State state_;
  std::array<Control, 3> controls_;
  std::function<void(const lane_cover::State &)> onChange_;
};
