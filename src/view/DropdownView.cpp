#include "DropdownView.h"

#include "Button.h"
#include "IconText.h"
#include "ScrollView.h"
#include "TextView.h"
#include "UiTheme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr uint32_t kIconCaretUp = 0xf0d8;
constexpr uint32_t kIconCaretDown = 0xf0d7;
constexpr float kTriggerHeight = 42.0f;
constexpr float kOptionHeight = 38.0f;
constexpr float kMenuPadding = 4.0f;
constexpr float kMenuGap = 4.0f;
constexpr float kWindowMargin = 10.0f;
constexpr float kIndicatorWidth = 5.0f;
constexpr float kTriggerIndicatorHeight = 26.0f;
constexpr float kOptionIndicatorHeight = 28.0f;

void styleButton(Button *button, TextView *text, bool selected, bool enabled) {
  if (button == nullptr || text == nullptr) {
    return;
  }
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  if (!enabled) {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
    return;
  }
  if (selected) {
    button->setThemedBackgroundColors(ui_theme::primaryAction,
                                      ui_theme::primaryActionHover,
                                      ui_theme::primaryActionPressed);
    button->setThemedBorderColors(
        [] { return ui_theme::withAlpha(ui_theme::accentBorderStrong(), 150); },
        [] { return ui_theme::withAlpha(ui_theme::accentBorderStrong(), 190); },
        [] {
          return ui_theme::withAlpha(ui_theme::accentBorderStrong(), 220);
        });
    text->setThemedColor(
        [] { return ui_theme::textOn(ui_theme::primaryAction()); });
    return;
  }

  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                ui_theme::hairlineStrong,
                                ui_theme::hairlineStrong);
  text->setThemedColor(ui_theme::textPrimary);
}

bool mouseEventToUi(const SDL_MouseButtonEvent &event, float &uiX, float &uiY) {
  if (event.which == SDL_TOUCH_MOUSEID) {
    return false;
  }
  const float screenX = static_cast<float>(event.x) * rendering::widthScale;
  const float screenY = static_cast<float>(event.y) * rendering::heightScale;
  rendering::screenToUi(screenX, screenY, uiX, uiY);
  return true;
}

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                     float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}

} // namespace

DropdownView::DropdownView(Callbacks callbacks)
    : View(), callbacks(std::move(callbacks)) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setWidth(kDefaultWidth);
  setHeight(kTriggerHeight);
  setMinHeight(kTriggerHeight);
  setFlexShrink(1.0f);
  setZIndex(0);
  buildView();
}

void DropdownView::buildView() {
  triggerButton = new Button(0, 0, 160, static_cast<int>(kTriggerHeight));
  triggerButton->setHeight(kTriggerHeight);
  triggerButton->setWidthPercent(100.0f);
  triggerButton->setFlexGrow(1.0f);
  triggerButton->setFlexBasis(0.0f);
  triggerButton->setFlexShrink(1.0f);
  triggerButton->setOnClickListener([this]() {
    if (!current.enabled) {
      return;
    }
    if (callbacks.onOpenChanged) {
      callbacks.onOpenChanged(!current.open);
    }
  });

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row);
  content->setAlignItems(YGAlignCenter);
  content->setPadding(Edge::Left, 14);
  content->setPadding(Edge::Right, 12);
  content->setGap(8);
  triggerIndicator = new View();
  triggerIndicator->setWidth(0);
  triggerIndicator->setHeight(kTriggerIndicatorHeight);
  triggerIndicator->setFlexShrink(0.0f);
  triggerIndicator->setCornerRadius(2.5f);
  content->addView(triggerIndicator);
  triggerText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  triggerText->setVAlign(TextView::MIDDLE);
  triggerText->setOverflow(TextView::TextOverflow::Hidden);
  triggerText->setMinWidth(0);
  triggerText->setFlex(1.0f);
  triggerIcon = new TextView(ui_icons::kFontAwesomeSolidPath, 13);
  triggerIcon->setAlign(TextView::CENTER);
  triggerIcon->setVAlign(TextView::MIDDLE);
  triggerIcon->setWidth(16);
  content->addView(triggerText);
  content->addView(triggerIcon);
  triggerButton->setContentView(content);
  addView(triggerButton);

  menuContent = new View();
  menuContent->setFlexDirection(FlexDirection::Column);
  menuContent->setAlignItems(YGAlignStretch);
  menuContent->setGap(4);

  menuScroll = new ScrollView(0, static_cast<int>(kTriggerHeight + kMenuGap),
                              160, static_cast<int>(kOptionHeight));
  menuScroll->setPositionType(YGPositionTypeAbsolute);
  menuScroll->setContentPadding(Edge::All, kMenuPadding);
  menuScroll->setThemedBackgroundColor(ui_theme::panelStrong);
  menuScroll->setThemedBorderColor(ui_theme::hairlineStrong);
  menuScroll->setBorderWidth(1);
  menuScroll->setCornerRadius(ui_theme::controlRadius());
  menuScroll->setThemedShadow(ui_theme::backdrop, ui_theme::kPanelShadow);
  menuScroll->setZIndex(100);
  menuScroll->setVisible(false);
  menuScroll->setContentView(menuContent);
  addView(menuScroll);
}

void DropdownView::refresh(const State &state) {
  current = state;
  if (current.maxVisibleItems <= 0) {
    current.maxVisibleItems = 1;
  }
  rebuildOptions();
  refreshVisualState();
  updateMenuPlacement();
}

void DropdownView::rebuildOptions() {
  optionButtons.clear();
  if (menuContent == nullptr) {
    return;
  }
  menuContent->clearChildren();

  for (const auto &option : current.options) {
    auto *button = new Button(0, 0, 160, static_cast<int>(kOptionHeight));
    button->setHeight(kOptionHeight);
    button->setFlexGrow(0.0f);
    button->setFlexShrink(0.0f);

    auto *content = new View();
    content->setFlexDirection(FlexDirection::Row);
    content->setAlignItems(YGAlignCenter);
    content->setPadding(Edge::Left, 8);
    content->setPadding(Edge::Right, 10);
    content->setGap(8);
    auto *indicator = new View();
    indicator->setWidth(0);
    indicator->setHeight(kOptionIndicatorHeight);
    indicator->setFlexShrink(0.0f);
    indicator->setCornerRadius(2.5f);
    content->addView(indicator);

    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    text->setText(option.label);
    text->setAlign(TextView::LEFT);
    text->setVAlign(TextView::MIDDLE);
    text->setOverflow(TextView::TextOverflow::Hidden);
    text->setMinWidth(0);
    text->setFlex(1.0f);
    content->addView(text);
    button->setContentView(content);
    button->setOnClickListener([this, id = option.id]() {
      if (!current.enabled) {
        return;
      }
      if (callbacks.onOptionSelected) {
        callbacks.onOptionSelected(id);
      }
      if (callbacks.onOpenChanged) {
        callbacks.onOpenChanged(false);
      }
    });
    optionButtons.push_back({
        .button = button,
        .indicator = indicator,
        .text = text,
        .id = option.id,
        .leadingColor = option.leadingColor,
    });
    menuContent->addView(button);
  }

  if (menuScroll != nullptr) {
    menuScroll->setScrollOffset(0.0f);
    menuScroll->refreshContentLayout();
  }
}

void DropdownView::refreshVisualState() {
  const bool menuVisible =
      current.enabled && current.open && !current.options.empty();
  setZIndex(menuVisible ? 100 : 0);
  if (menuScroll != nullptr) {
    menuScroll->setVisible(menuVisible);
  }
  if (triggerText != nullptr) {
    const std::string selected = selectedLabel();
    triggerText->setText(
        current.label.empty() ? selected : current.label + ": " + selected);
  }
  refreshIndicator(triggerIndicator, selectedLeadingColor());
  if (triggerIcon != nullptr) {
    triggerIcon->setText(ui_icons::textForCodepoint(
        menuVisible ? kIconCaretUp : kIconCaretDown));
    triggerIcon->setThemedColor(
        !current.enabled ? ui_theme::textMuted
                         : (menuVisible ? ui_theme::textPrimary
                                        : ui_theme::textSecondary));
  }

  styleButton(triggerButton, triggerText, menuVisible, current.enabled);
  for (const auto &item : optionButtons) {
    styleButton(item.button, item.text, item.id == current.selectedId, true);
    refreshIndicator(item.indicator, item.leadingColor);
  }
}

void DropdownView::updateMenuPlacement() {
  if (placementUpdating || menuScroll == nullptr || !current.enabled ||
      !current.open || current.options.empty()) {
    return;
  }

  placementUpdating = true;
  const int optionCount = static_cast<int>(current.options.size());
  const int visibleOptions =
      std::clamp(optionCount, 1, std::max(1, current.maxVisibleItems));
  const int desiredHeight = static_cast<int>(
      std::round(static_cast<float>(visibleOptions) * kOptionHeight +
                 kMenuPadding * 2.0f));
  const int minHeight =
      static_cast<int>(std::round(kOptionHeight + kMenuPadding * 2.0f));
  const int belowSpace = std::max(
      0, static_cast<int>(std::floor(
             static_cast<float>(rendering::window_height) -
             static_cast<float>(getY() + getHeight()) - kWindowMargin)));
  const int aboveSpace = std::max(
      0,
      static_cast<int>(std::floor(static_cast<float>(getY()) - kWindowMargin)));
  const bool placeBelow =
      belowSpace >= desiredHeight || belowSpace >= aboveSpace;
  const int availableSpace = placeBelow ? belowSpace : aboveSpace;
  const int menuHeight =
      std::min(desiredHeight, std::max(minHeight, availableSpace));

  int menuWidth = static_cast<int>(std::round(current.menuWidth));
  if (menuWidth <= 0) {
    menuWidth = getWidth() > 0 ? getWidth() : static_cast<int>(kDefaultWidth);
  }

  int left = 0;
  const int rightEdge = static_cast<int>(
      std::floor(static_cast<float>(rendering::window_width) - kWindowMargin));
  const int desiredRight = getX() + menuWidth;
  if (desiredRight > rightEdge) {
    left -= desiredRight - rightEdge;
  }
  const int leftEdge = static_cast<int>(std::ceil(kWindowMargin));
  if (getX() + left < leftEdge) {
    left += leftEdge - (getX() + left);
  }

  const int top = placeBelow ? getHeight() + static_cast<int>(kMenuGap)
                             : -menuHeight - static_cast<int>(kMenuGap);
  menuScroll->setPositionNoLayout(left, top, YGPositionTypeAbsolute);
  menuScroll->setSize(menuWidth, menuHeight);
  menuScroll->refreshContentLayout();
  placementUpdating = false;
}

std::string DropdownView::selectedLabel() const {
  for (const auto &option : current.options) {
    if (option.id == current.selectedId) {
      return option.label;
    }
  }
  return current.options.empty() ? std::string()
                                 : current.options.front().label;
}

std::optional<Color> DropdownView::selectedLeadingColor() const {
  for (const auto &option : current.options) {
    if (option.id == current.selectedId) {
      return option.leadingColor;
    }
  }
  return std::nullopt;
}

bool DropdownView::pointInsideOpenArea(float uiX, float uiY) const {
  const bool insideTrigger = uiX >= static_cast<float>(getX()) &&
                             uiX <= static_cast<float>(getX() + getWidth()) &&
                             uiY >= static_cast<float>(getY()) &&
                             uiY <= static_cast<float>(getY() + getHeight());
  if (insideTrigger) {
    return true;
  }
  if (menuScroll == nullptr || !menuScroll->getVisible()) {
    return false;
  }
  return uiX >= static_cast<float>(menuScroll->getX()) &&
         uiX <=
             static_cast<float>(menuScroll->getX() + menuScroll->getWidth()) &&
         uiY >= static_cast<float>(menuScroll->getY()) &&
         uiY <=
             static_cast<float>(menuScroll->getY() + menuScroll->getHeight());
}

bool DropdownView::refreshIndicator(View *indicator,
                                    const std::optional<Color> &color) {
  if (indicator == nullptr) {
    return false;
  }
  const float targetWidth = color.has_value() ? kIndicatorWidth : 0.0f;
  const bool layoutChanged =
      std::abs(static_cast<float>(indicator->getWidth()) - targetWidth) > 0.5f;
  indicator->setWidth(targetWidth);
  if (color.has_value()) {
    indicator->setBackgroundColor(*color);
  } else {
    indicator->clearBackgroundColor();
  }
  return layoutChanged;
}

bool DropdownView::handleEventsImpl(SDL_Event &event) {
  if (!current.open) {
    return true;
  }

  if (event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    float uiX = 0.0f;
    float uiY = 0.0f;
    if (mouseEventToUi(event.button, uiX, uiY) &&
        !pointInsideOpenArea(uiX, uiY)) {
      if (callbacks.onOpenChanged) {
        callbacks.onOpenChanged(false);
      }
    }
    return true;
  }

  if (event.type == SDL_FINGERDOWN) {
    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (!pointInsideOpenArea(uiX, uiY) && callbacks.onOpenChanged) {
      callbacks.onOpenChanged(false);
    }
    return true;
  }

  return true;
}

void DropdownView::onLayout() { updateMenuPlacement(); }
