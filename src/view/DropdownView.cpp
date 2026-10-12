#include "DropdownView.h"

#include "Button.h"
#include "IconText.h"
#include "OverlayPortal.h"
#include "ScrollView.h"
#include "TextView.h"
#include "AppTheme.h"
#include "../rendering/ResponsiveUi.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr uint32_t kIconCaretUp = 0xf0d8;
constexpr uint32_t kIconCaretDown = 0xf0d7;
float triggerHeight() { return rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 42.0f; }
float optionHeight() { return rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 38.0f; }
constexpr float kMenuPadding = 4.0f;
constexpr float kMenuGap = 4.0f;
constexpr float kWindowMargin = 10.0f;
constexpr float kIndicatorWidth = 5.0f;
constexpr float kTriggerIndicatorHeight = 26.0f;
constexpr float kOptionIndicatorHeight = 28.0f;
constexpr float kTriggerHorizontalChrome = 50.0f;
constexpr float kIndicatorHorizontalChrome = 13.0f;

class ScopedBoolFlag {
public:
  explicit ScopedBoolFlag(bool &flag) : flag(flag) { flag = true; }
  ~ScopedBoolFlag() { flag = false; }

private:
  bool &flag;
};

bool colorsEqual(const std::optional<Color> &lhs,
                 const std::optional<Color> &rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  if (!lhs.has_value()) {
    return true;
  }
  return lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
         lhs->a == rhs->a;
}

void styleButton(Button *button, TextView *text, bool selected, bool enabled) {
  if (button == nullptr || text == nullptr) {
    return;
  }
  button->setCornerRadius(app_theme::controlRadius());
  button->setStyledBorderWidth(1);
  if (!enabled) {
    button->setThemedBackgroundColors(
        app_theme::panelSubtle, app_theme::panelSubtle, app_theme::panelSubtle);
    button->setThemedBorderColors(app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle);
    text->setThemedColor(app_theme::textMuted);
    return;
  }
  if (selected) {
    button->setThemedBackgroundColors(app_theme::primaryAction,
                                      app_theme::primaryActionHover,
                                      app_theme::primaryActionPressed);
    button->setThemedBorderColors(
        [] { return app_theme::withAlpha(app_theme::accentBorderStrong(), 150); },
        [] { return app_theme::withAlpha(app_theme::accentBorderStrong(), 190); },
        [] {
          return app_theme::withAlpha(app_theme::accentBorderStrong(), 220);
        });
    text->setThemedColor(
        [] { return app_theme::textOn(app_theme::primaryAction()); });
    return;
  }

  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairlineSubtle,
                                app_theme::hairlineStrong,
                                app_theme::hairlineStrong);
  text->setThemedColor(app_theme::textPrimary);
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

DropdownView::DropdownView(Callbacks callbacks, OverlayPortal *portal)
    : View(), callbacks(std::move(callbacks)), overlayPortal(portal) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setWidth(kDefaultWidth);
  lastTriggerHeight = triggerHeight();
  setHeight(lastTriggerHeight);
  setMinHeight(triggerHeight());
  setFlexShrink(1.0f);
  setZIndex(0);
  buildView();
}

DropdownView::~DropdownView() {
  if (lifetimeToken) {
    *lifetimeToken = false;
  }
  if (menuOwnedByPortal && menuScroll != nullptr) {
    overlayPortal->dismiss(menuScroll);
    delete menuScroll;
    menuScroll = nullptr;
    menuContent = nullptr;
  }
  pendingRefresh.reset();
}

void DropdownView::buildView() {
  triggerButton = new Button(0, 0, 160, static_cast<int>(triggerHeight()));
  triggerButton->setHeight(triggerHeight());
  triggerButton->setWidthPercent(100.0f);
  triggerButton->setFlexGrow(1.0f);
  triggerButton->setFlexBasis(0.0f);
  triggerButton->setFlexShrink(1.0f);
  triggerButton->setOnClickListener([this]() {
    if (!current.enabled) {
      return;
    }
    const bool open = !current.open;
    setOpen(open);
    if (callbacks.onOpenChanged) {
      callbacks.onOpenChanged(open);
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
  triggerText = new TextView(app_theme::font, 16);
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

  menuScroll = new ScrollView(0, static_cast<int>(triggerHeight() + kMenuGap),
                              160, static_cast<int>(optionHeight()));
  menuScroll->setPositionType(YGPositionTypeAbsolute);
  menuScroll->setContentPadding(Edge::All, kMenuPadding);
  menuScroll->setThemedBackgroundColor(app_theme::panelStrong);
  menuScroll->setThemedBorderColor(app_theme::hairlineStrong);
  menuScroll->setBorderWidth(1);
  menuScroll->setCornerRadius(app_theme::controlRadius());
  menuScroll->setThemedShadow(app_theme::shadow, app_theme::kPanelShadow);
  menuScroll->setZIndex(100);
  menuScroll->setVisible(false);
  menuScroll->setContentView(menuContent);
  if (overlayPortal != nullptr) {
    menuOwnedByPortal = true;
  } else {
    addView(menuScroll);
  }
}

void DropdownView::refresh(const State &state) {
  State next = state;
  if (next.maxVisibleItems <= 0) {
    next.maxVisibleItems = 1;
  }

  if (dispatchingOptionCallback && refreshRequiresOptionRebuild(next)) {
    pendingRefresh = std::move(next);
    scheduleDeferredRefresh();
    return;
  }

  applyRefresh(std::move(next));
}

void DropdownView::applyRefresh(State state) {
  refreshDensity();
  const bool rebuild = !optionsMatch(state.options);
  const bool wasOpen = current.open;
  if (rebuild || current.label != state.label) cachedPreferredWidth.reset();
  current = std::move(state);
  if (current.open && (rebuild || optionButtons.empty())) {
    rebuildOptions();
  } else if (wasOpen) {
    scheduleOptionViewClear();
  }
  refreshVisualState();
  refreshWidths();
  updateMenuPlacement();
}

void DropdownView::setTriggerWidth(float width) {
  triggerWidth = std::isfinite(width) ? std::max(0.0f, width) : 0.0f;
  refreshWidths();
  updateMenuPlacement();
}

void DropdownView::refreshWidths() {
  resolvedWidth = std::max({triggerWidth, current.menuWidth, preferredWidth()});
  setWidth(triggerWidth > 0.0f ? triggerWidth : resolvedWidth);
  setMinWidth(triggerWidth > 0.0f ? 0.0f : resolvedWidth);
}

void DropdownView::rebuildOptions() {
  clearOptionViews();
  if (menuContent == nullptr) {
    return;
  }

  for (const auto &option : current.options) {
    auto *button = new Button(0, 0, 160, static_cast<int>(optionHeight()));
    button->setHeight(optionHeight());
    button->setWidthPercent(100.0f);
    button->setMinWidth(0);
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

    auto *text = new TextView(app_theme::font, 15);
    text->setLocalizedText(option.label);
    text->setAlign(TextView::LEFT);
    text->setVAlign(TextView::MIDDLE);
    text->setOverflow(TextView::TextOverflow::Hidden);
    text->setMinWidth(0);
    text->setFlex(1.0f);
    content->addView(text);
    button->setContentView(content);
    button->setOnClickListener([this, id = option.id]() {
      const auto option =
          std::ranges::find_if(current.options, [&](const Option &candidate) {
            return candidate.id == id;
          });
      if (option == current.options.end() ||
          !optionSelectable(current, *option)) {
        return;
      }
      const std::string previousId = current.selectedId;
      current.selectedId = id;
      setOpen(false);
      refreshVisualState();
      ScopedBoolFlag dispatching(dispatchingOptionCallback);
      const bool accepted = callbacks.onOptionSelectedResult
                                ? callbacks.onOptionSelectedResult(id)
                                : true;
      if (!callbacks.onOptionSelectedResult && callbacks.onOptionSelected) {
        callbacks.onOptionSelected(id);
      }
      if (!accepted) {
        current.selectedId = previousId;
        refreshVisualState();
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
        .available = option.available,
        .leadingColor = option.leadingColor,
    });
    menuContent->addView(button);
  }

  if (menuScroll != nullptr) {
    menuScroll->setScrollOffset(0.0f);
    menuScroll->refreshContentLayout();
  }
}

void DropdownView::clearOptionViews() {
  optionButtons.clear();
  if (menuContent != nullptr) {
    menuContent->clearChildren();
  }
  if (menuScroll != nullptr) {
    menuScroll->setScrollOffset(0.0f);
    menuScroll->refreshContentLayout();
  }
}

void DropdownView::refreshVisualState() {
  const bool menuVisible =
      current.enabled && current.open && !current.options.empty();
  setZIndex(!menuOwnedByPortal && menuVisible ? 100 : 0);
  if (menuScroll != nullptr) {
    menuScroll->setVisible(menuVisible);
  }
  if (menuOwnedByPortal) {
    if (menuVisible) {
      overlayPortal->present(menuScroll);
    } else {
      overlayPortal->dismiss(menuScroll);
    }
  }
  if (triggerText != nullptr) {
    const std::string selected = selectedLabel();
    const std::string label = current.label.resolve();
    triggerText->setText(label.empty() ? selected : label + ": " + selected);
  }
  refreshIndicator(triggerIndicator, selectedLeadingColor());
  if (triggerIcon != nullptr) {
    triggerIcon->setText(ui_icons::textForCodepoint(
        menuVisible ? kIconCaretUp : kIconCaretDown));
    triggerIcon->setThemedColor(
        !current.enabled
            ? app_theme::textMuted
            : (menuVisible ? app_theme::onAction : app_theme::textSecondary));
  }

  styleButton(triggerButton, triggerText, menuVisible, current.enabled);
  for (const auto &item : optionButtons) {
    styleButton(item.button, item.text, item.id == current.selectedId,
                item.available);
    refreshIndicator(item.indicator, item.leadingColor);
  }
}

void DropdownView::setOpen(bool open) {
  if (current.open == open) {
    return;
  }
  current.open = open;
  if (open) {
    rebuildOptions();
  }
  refreshVisualState();
  if (!open) {
    scheduleOptionViewClear();
  }
  updateMenuPlacement();
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
      std::round(static_cast<float>(visibleOptions) * optionHeight() +
                 kMenuPadding * 2.0f));
  const int minimumHeight =
      static_cast<int>(std::round(optionHeight() + kMenuPadding * 2.0f));

  const int menuWidth =
      static_cast<int>(std::round(std::max(1.0f, resolvedWidth)));

  const OverlayPlacement placement = placeAnchoredOverlay(
      {.x = getX(), .y = getY(), .width = getWidth(), .height = getHeight()},
      menuWidth, desiredHeight, minimumHeight, rendering::window_width,
      rendering::window_height, static_cast<int>(kWindowMargin),
      static_cast<int>(kMenuGap));
  const int left = menuOwnedByPortal ? placement.x : placement.x - getX();
  const int top = menuOwnedByPortal ? placement.y : placement.y - getY();
  menuScroll->setPositionNoLayout(left, top, YGPositionTypeAbsolute);
  menuScroll->setSize(placement.width, placement.height);
  menuScroll->refreshContentLayout();
  placementUpdating = false;
}

float DropdownView::preferredWidth() const {
  if (triggerText == nullptr) {
    return kDefaultWidth;
  }
  const auto languageRevision = i18n::revision();
  if (cachedPreferredWidth && preferredWidthLanguageRevision == languageRevision) {
    return *cachedPreferredWidth;
  }

  int widestValue = 0;
  bool hasLeadingIndicator = false;
  const std::string label = current.label.resolve();
  for (const auto &option : current.options) {
    const std::string text = label.empty()
                                 ? option.label.resolve()
                                 : label + ": " + option.label.resolve();
    widestValue = std::max(widestValue, triggerText->measureTextWidth(text));
    hasLeadingIndicator = hasLeadingIndicator || option.leadingColor.has_value();
  }

  const float horizontalChrome =
      kTriggerHorizontalChrome +
      (hasLeadingIndicator ? kIndicatorHorizontalChrome : 0.0f);
  cachedPreferredWidth = std::max(kDefaultWidth,
                                  static_cast<float>(widestValue) + horizontalChrome);
  preferredWidthLanguageRevision = languageRevision;
  return *cachedPreferredWidth;
}

void DropdownView::scheduleDeferredRefresh() {
  if (deferredRefreshScheduled) {
    return;
  }
  deferredRefreshScheduled = true;
  std::weak_ptr<bool> aliveToken = lifetimeToken;
  View::deferAfterEvent([this, aliveToken]() {
    const auto alive = aliveToken.lock();
    if (!alive || !*alive) {
      return;
    }
    deferredRefreshScheduled = false;
    if (!pendingRefresh.has_value()) {
      return;
    }
    State next = std::move(*pendingRefresh);
    pendingRefresh.reset();
    applyRefresh(std::move(next));
  });
}

void DropdownView::scheduleOptionViewClear() {
  if (deferredOptionViewClearScheduled) {
    return;
  }
  deferredOptionViewClearScheduled = true;
  std::weak_ptr<bool> aliveToken = lifetimeToken;
  View::deferAfterEvent([this, aliveToken]() {
    const auto alive = aliveToken.lock();
    if (!alive || !*alive) {
      return;
    }
    deferredOptionViewClearScheduled = false;
    if (!current.open) {
      clearOptionViews();
    }
  });
}

std::string DropdownView::selectedLabel() const {
  for (const auto &option : current.options) {
    if (option.id == current.selectedId) {
      return option.label.resolve();
    }
  }
  return current.options.empty() ? std::string()
                                 : current.options.front().label.resolve();
}

std::optional<Color> DropdownView::selectedLeadingColor() const {
  for (const auto &option : current.options) {
    if (option.id == current.selectedId) {
      return option.leadingColor;
    }
  }
  return std::nullopt;
}

bool DropdownView::optionsMatch(const std::vector<Option> &options) const {
  if (current.options.size() != options.size()) {
    return false;
  }
  for (size_t i = 0; i < options.size(); ++i) {
    const auto &currentOption = current.options[i];
    const auto &nextOption = options[i];
    if (currentOption.id != nextOption.id ||
        currentOption.label != nextOption.label ||
        currentOption.available != nextOption.available ||
        !colorsEqual(currentOption.leadingColor, nextOption.leadingColor)) {
      return false;
    }
  }
  return true;
}

bool DropdownView::refreshRequiresOptionRebuild(const State &state) const {
  return !optionsMatch(state.options);
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

  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    float uiX = 0.0f;
    float uiY = 0.0f;
    if (mouseEventToUi(event.button, uiX, uiY) &&
        !pointInsideOpenArea(uiX, uiY)) {
      setOpen(false);
      if (callbacks.onOpenChanged) {
        callbacks.onOpenChanged(false);
      }
    }
    return true;
  }

  if (event.type == SDL_EVENT_FINGER_DOWN) {
    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (!pointInsideOpenArea(uiX, uiY)) {
      setOpen(false);
      if (callbacks.onOpenChanged) {
        callbacks.onOpenChanged(false);
      }
    }
    return true;
  }

  return true;
}

void DropdownView::refreshDensity() {
  if (triggerButton == nullptr) return;
  const float height = triggerHeight();
  if (height == lastTriggerHeight) return;
  lastTriggerHeight = height;
  View::LayoutBatchScope batch;
  setHeight(height)->setMinHeight(height);
  triggerButton->setHeight(height);
  for (auto &option : optionButtons) option.button->setHeight(optionHeight());
}

void DropdownView::onLayout() {
  refreshDensity();
  updateMenuPlacement();
}

void DropdownView::onMove(int, int) { updateMenuPlacement(); }

void DropdownView::onThemeChanged() {
  View::onThemeChanged();
  if (menuOwnedByPortal && menuScroll != nullptr) {
    menuScroll->propagateThemeChange();
  }
}

void DropdownView::onLanguageChanged() {
  View::onLanguageChanged();
  if (menuOwnedByPortal && menuScroll != nullptr) {
    menuScroll->propagateLanguageChange();
  }
  refreshVisualState();
  refreshWidths();
  updateMenuPlacement();
}
