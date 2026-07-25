#include "ContextMenuView.h"

#include "Button.h"
#include "TextView.h"
#include "UiTheme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kActionHeight = 44;
constexpr int kPanelPadding = 4;
constexpr int kActionGap = 2;
constexpr int kViewportMargin = 10;
constexpr int kAnchorGap = 4;

bool mouseEventToUi(const SDL_MouseButtonEvent &event, float &uiX,
                    float &uiY) {
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

bool isBlockedInputEvent(Uint32 type) {
  switch (type) {
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
  case SDL_MOUSEMOTION:
  case SDL_MOUSEWHEEL:
  case SDL_FINGERDOWN:
  case SDL_FINGERUP:
  case SDL_FINGERMOTION:
  case SDL_KEYDOWN:
  case SDL_KEYUP:
  case SDL_TEXTINPUT:
  case SDL_TEXTEDITING:
  case SDL_TEXTEDITING_EXT:
    return true;
  default:
    return false;
  }
}

} // namespace

ContextMenuView::ContextMenuView(OverlayPortal *portalValue,
                                 Callbacks callbackValue)
    : portal(portalValue), callbacks(std::move(callbackValue)) {
  setPosition(0, 0, YGPositionTypeAbsolute);
  setSize(rendering::window_width, rendering::window_height);
  viewportWidth = rendering::window_width;
  viewportHeight = rendering::window_height;

  panel = new View();
  panel->setPositionType(YGPositionTypeAbsolute);
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setPadding(Edge::All, kPanelPadding);
  panel->setGap(kActionGap);
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setThemedBorderColor(ui_theme::hairlineStrong);
  panel->setBorderWidth(1);
  panel->setCornerRadius(ui_theme::controlRadius());
  panel->setThemedShadow(ui_theme::backdrop, ui_theme::kPanelShadow);
  addView(panel);
}

ContextMenuView::~ContextMenuView() {
  if (portal != nullptr) {
    portal->dismiss(this);
  }
  open = false;
}

void ContextMenuView::show(OverlayAnchor anchorValue,
                           std::vector<Action> actionValues, int menuWidth) {
  if (actionValues.empty()) {
    dismiss();
    return;
  }

  anchor = anchorValue;
  actions = std::move(actionValues);
  requestedMenuWidth = std::max(1, menuWidth);
  rebuildActions();
  updatePlacement();

  const bool wasOpen = open;
  open = true;
  if (portal != nullptr) {
    portal->present(this);
  }
  if (!wasOpen && callbacks.onOpenChanged) {
    callbacks.onOpenChanged(true);
  }
}

void ContextMenuView::dismiss() {
  if (!open) {
    return;
  }
  if (portal != nullptr) {
    portal->dismiss(this);
  }
  open = false;
  if (callbacks.onOpenChanged) {
    callbacks.onOpenChanged(false);
  }
}

void ContextMenuView::setViewportSize(int width, int height) {
  viewportWidth = std::max(0, width);
  viewportHeight = std::max(0, height);
  setSize(viewportWidth, viewportHeight);
  updatePlacement();
}

void ContextMenuView::rebuildActions() {
  panel->clearChildren();
  for (const auto &action : actions) {
    auto *button = new Button(0, 0, requestedMenuWidth, kActionHeight);
    button->setWidthPercent(100.0F);
    button->setHeight(kActionHeight);
    button->setFlexShrink(0.0F);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setStyledBorderWidth(1);
    button->setThemedBackgroundColors(ui_theme::control,
                                      ui_theme::controlHover,
                                      ui_theme::controlPressed);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineStrong,
                                  ui_theme::hairlineStrong);
    button->setEnabled(action.enabled);

    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
    text->setText(action.label);
    text->setAlign(TextView::LEFT);
    text->setVAlign(TextView::MIDDLE);
    text->setPadding(Edge::Left, 14);
    text->setPadding(Edge::Right, 14);
    text->setThemedColor(action.enabled ? ui_theme::textPrimary
                                        : ui_theme::textMuted);
    button->setContentView(text);
    button->setOnClickListener(
        [this, id = action.id]() { dispatchAction(id); });
    panel->addView(button);
  }
}

void ContextMenuView::updatePlacement() {
  if (panel == nullptr || actions.empty() || viewportWidth <= 0 ||
      viewportHeight <= 0) {
    return;
  }
  const int actionCount = static_cast<int>(actions.size());
  const int desiredHeight = kPanelPadding * 2 + actionCount * kActionHeight +
                            std::max(0, actionCount - 1) * kActionGap;
  const OverlayPlacement placement = placeAnchoredOverlay(
      anchor, requestedMenuWidth, desiredHeight,
      kPanelPadding * 2 + kActionHeight, viewportWidth, viewportHeight,
      kViewportMargin, kAnchorGap);
  panel->setPositionNoLayout(placement.x, placement.y,
                             YGPositionTypeAbsolute);
  panel->setSize(placement.width, placement.height);
}

void ContextMenuView::dispatchAction(const std::string &id) {
  const auto selected =
      std::ranges::find_if(actions, [&](const Action &candidate) {
        return candidate.id == id;
      });
  if (selected == actions.end() || !selected->enabled) {
    return;
  }
  if (callbacks.onActionSelected) {
    callbacks.onActionSelected(id);
  }
  dismiss();
}

void ContextMenuView::handlePointerDown(float x, float y) {
  if (!pointInsidePanel(x, y) || pointInsideAnchor(x, y)) {
    dismiss();
  }
}

bool ContextMenuView::pointInsideAnchor(float x, float y) const {
  return x >= static_cast<float>(anchor.x) &&
         x <= static_cast<float>(anchor.x + anchor.width) &&
         y >= static_cast<float>(anchor.y) &&
         y <= static_cast<float>(anchor.y + anchor.height);
}

bool ContextMenuView::pointInsidePanel(float x, float y) const {
  if (panel == nullptr) {
    return false;
  }
  return x >= static_cast<float>(panel->getX()) &&
         x <= static_cast<float>(panel->getX() + panel->getWidth()) &&
         y >= static_cast<float>(panel->getY()) &&
         y <= static_cast<float>(panel->getY() + panel->getHeight());
}

bool ContextMenuView::handleEventsImpl(SDL_Event &event) {
  if (!open) {
    return true;
  }

  if (event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    float uiX = 0.0F;
    float uiY = 0.0F;
    if (mouseEventToUi(event.button, uiX, uiY)) {
      handlePointerDown(uiX, uiY);
    }
    return false;
  }

  if (event.type == SDL_FINGERDOWN) {
    float uiX = 0.0F;
    float uiY = 0.0F;
    fingerEventToUi(event.tfinger, uiX, uiY);
    handlePointerDown(uiX, uiY);
    return false;
  }

  if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
      (event.key.keysym.sym == SDLK_ESCAPE ||
       event.key.keysym.sym == SDLK_AC_BACK)) {
    dismiss();
    return false;
  }

  return !isBlockedInputEvent(event.type);
}
