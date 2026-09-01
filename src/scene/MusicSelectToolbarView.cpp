#include "MusicSelectToolbarView.h"

#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/IconText.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr float kControlSize = 48.0F;
constexpr float kGap = 6.0F;
constexpr float kPadding = 8.0F;
constexpr float kToolbarHeight = kControlSize + kPadding * 2.0F;
constexpr float kDefaultPosition = 24.0F;

std::uint32_t codepointFor(MusicSelectToolbarControl control) {
  switch (control) {
  case MusicSelectToolbarControl::Drag:
    return ui_icons::kDrag;
  case MusicSelectToolbarControl::MusicPlayer:
    return ui_icons::kMusic;
  case MusicSelectToolbarControl::Tasks:
    return ui_icons::kTasks;
  case MusicSelectToolbarControl::IrUploads:
    return ui_icons::kIrUploads;
  case MusicSelectToolbarControl::Settings:
    return ui_icons::kSettings;
  case MusicSelectToolbarControl::Collapse:
    return ui_icons::kCollapse;
  case MusicSelectToolbarControl::Expand:
    return ui_icons::kExpand;
  case MusicSelectToolbarControl::Hide:
    return ui_icons::kHide;
  }
  return ui_icons::kDrag;
}

TextView *makeIcon(std::uint32_t codepoint) {
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 22);
  icon->setText(ui_icons::textForCodepoint(codepoint));
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(ui_theme::textPrimary);
  icon->setWidth(kControlSize);
  icon->setHeight(kControlSize);
  return icon;
}

void mousePosition(const SDL_MouseButtonEvent &event, float &x, float &y) {
  rendering::screenToUi(event.x * rendering::widthScale,
                        event.y * rendering::heightScale, x, y);
}

void mousePosition(const SDL_MouseMotionEvent &event, float &x, float &y) {
  rendering::screenToUi(event.x * rendering::widthScale,
                        event.y * rendering::heightScale, x, y);
}

void touchPosition(const SDL_TouchFingerEvent &event, float &x, float &y) {
  rendering::normalizedToUi(event.x, event.y, x, y);
}
} // namespace

std::unique_ptr<MusicSelectToolbarView> MusicSelectToolbarView::Create(
    MusicSelectToolbarState state, MusicSelectToolbarCallbacks callbacks,
    int viewportWidth, int viewportHeight) {
  if (state.mode == MusicSelectToolbarMode::Hidden) {
    return nullptr;
  }
  return std::unique_ptr<MusicSelectToolbarView>(new MusicSelectToolbarView(
      state, std::move(callbacks), viewportWidth, viewportHeight));
}

MusicSelectToolbarView::MusicSelectToolbarView(
    MusicSelectToolbarState state, MusicSelectToolbarCallbacks callbacks,
    int viewportWidth, int viewportHeight)
    : state_(state), callbacks_(std::move(callbacks)),
      viewportWidth_(viewportWidth), viewportHeight_(viewportHeight) {
  setPositionType(YGPositionTypeAbsolute);
  setZIndex(10000);
  rebuild();
  place(state_.hasPosition ? state_.x : kDefaultPosition,
        state_.hasPosition ? state_.y : kDefaultPosition);
}

void MusicSelectToolbarView::applyState(MusicSelectToolbarState state) {
  state_ = state;
  rebuild();
  if (state_.mode != MusicSelectToolbarMode::Hidden) {
    place(state_.hasPosition ? state_.x : kDefaultPosition,
          state_.hasPosition ? state_.y : kDefaultPosition);
  }
}

void MusicSelectToolbarView::rebuild() {
  clearChildren();
  controls_.clear();
  if (state_.mode == MusicSelectToolbarMode::Hidden) {
    setVisible(false);
    return;
  }
  setVisible(true);
  const std::vector<MusicSelectToolbarControl> layout =
      state_.mode == MusicSelectToolbarMode::Collapsed
          ? std::vector<MusicSelectToolbarControl>{
                MusicSelectToolbarControl::Drag,
                MusicSelectToolbarControl::Expand}
          : std::vector<MusicSelectToolbarControl>{
                MusicSelectToolbarControl::Drag,
                MusicSelectToolbarControl::MusicPlayer,
                MusicSelectToolbarControl::Tasks,
                MusicSelectToolbarControl::IrUploads,
                MusicSelectToolbarControl::Settings,
                MusicSelectToolbarControl::Collapse,
                MusicSelectToolbarControl::Hide};

  setWidth(kPadding * 2.0F + kControlSize * layout.size() +
           kGap * (layout.size() - 1));
  setHeight(kToolbarHeight);
  setPadding(Edge::All, kPadding);
  setGap(kGap);
  setFlexDirection(FlexDirection::Row);
  setAlignItems(YGAlignCenter);
  setThemedBackgroundColor(ui_theme::panelStrong);
  setThemedBorderColor(ui_theme::hairlineStrong);
  setBorderWidth(1);
  setCornerRadius(ui_theme::controlRadius());
  setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);

  for (const auto control : layout) {
    const auto codepoint = codepointFor(control);
    auto *icon = makeIcon(codepoint);
    controls_.push_back(
        {.control = control, .codepoint = codepoint, .icon = icon});
    if (control == MusicSelectToolbarControl::Drag) {
      addView(icon);
      continue;
    }
    auto *button = new Button();
    button->setWidth(kControlSize);
    button->setHeight(kControlSize);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setThemedBackgroundColors(ui_theme::control,
                                      ui_theme::controlHover,
                                      ui_theme::controlPressed);
    button->setThemedBorderColors(ui_theme::hairlineStrong,
                                  ui_theme::accentBorder,
                                  ui_theme::accentBorderStrong);
    button->setStyledBorderWidth(1);
    button->setContentView(icon);
    button->setOnClickListener([this, control] { activateControl(control); });
    addView(button);
  }
}

void MusicSelectToolbarView::activateControl(
    MusicSelectToolbarControl control) {
  switch (control) {
  case MusicSelectToolbarControl::Drag:
    break;
  case MusicSelectToolbarControl::MusicPlayer:
    if (callbacks_.openMusicPlayer) {
      callbacks_.openMusicPlayer();
    }
    break;
  case MusicSelectToolbarControl::Tasks:
    if (callbacks_.openTasks) {
      callbacks_.openTasks();
    }
    break;
  case MusicSelectToolbarControl::IrUploads:
    if (callbacks_.openIrUploads) {
      callbacks_.openIrUploads();
    }
    break;
  case MusicSelectToolbarControl::Settings:
    if (callbacks_.openSettings) {
      callbacks_.openSettings();
    }
    break;
  case MusicSelectToolbarControl::Collapse:
    requestMode(MusicSelectToolbarMode::Collapsed);
    break;
  case MusicSelectToolbarControl::Expand:
    requestMode(MusicSelectToolbarMode::Expanded);
    break;
  case MusicSelectToolbarControl::Hide:
    requestMode(MusicSelectToolbarMode::Hidden);
    break;
  }
}

void MusicSelectToolbarView::requestMode(MusicSelectToolbarMode mode) {
  state_.mode = mode;
  persist();
  if (mode == MusicSelectToolbarMode::Hidden) {
    setVisible(false);
  }
  View::deferAfterEvent([this] {
    rebuild();
    if (state_.mode != MusicSelectToolbarMode::Hidden) {
      place(state_.hasPosition ? state_.x : kDefaultPosition,
            state_.hasPosition ? state_.y : kDefaultPosition);
    }
  });
}

void MusicSelectToolbarView::persist() {
  if (callbacks_.persist) {
    callbacks_.persist(state_);
  }
}

void MusicSelectToolbarView::setViewportSize(int width, int height) {
  viewportWidth_ = width;
  viewportHeight_ = height;
  place(state_.hasPosition ? state_.x : kDefaultPosition,
        state_.hasPosition ? state_.y : kDefaultPosition);
}

void MusicSelectToolbarView::place(float x, float y) {
  const float maxX = std::max(0, viewportWidth_ - getWidth());
  const float maxY = std::max(0, viewportHeight_ - getHeight());
  const float visibleX = std::clamp(x, 0.0F, maxX);
  const float visibleY = std::clamp(y, 0.0F, maxY);
  setPositionNoLayout(static_cast<int>(visibleX), static_cast<int>(visibleY),
                      YGPositionTypeAbsolute);
}

bool MusicSelectToolbarView::insideDragHandle(float x, float y) const {
  return x >= getX() + kPadding &&
         x <= getX() + kPadding + kControlSize && y >= getY() + kPadding &&
         y <= getY() + kPadding + kControlSize;
}

bool MusicSelectToolbarView::handleEventsImpl(SDL_Event &event) {
  float x = 0.0F;
  float y = 0.0F;
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN:
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return true;
    }
    mousePosition(event.button, x, y);
    if (!insideDragHandle(x, y)) {
      return true;
    }
    mouseDragging_ = true;
    dragPointerOffsetX_ = x - getX();
    dragPointerOffsetY_ = y - getY();
    return false;
  case SDL_MOUSEMOTION:
    if (!mouseDragging_) {
      return true;
    }
    mousePosition(event.motion, x, y);
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    return false;
  case SDL_MOUSEBUTTONUP:
    if (!mouseDragging_ || event.button.button != SDL_BUTTON_LEFT) {
      return true;
    }
    mousePosition(event.button, x, y);
    mouseDragging_ = false;
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    state_.x = static_cast<float>(getX());
    state_.y = static_cast<float>(getY());
    state_.hasPosition = true;
    persist();
    return false;
  case SDL_FINGERDOWN:
    if (touchDragging_ != -1) {
      return true;
    }
    touchPosition(event.tfinger, x, y);
    if (!insideDragHandle(x, y)) {
      return true;
    }
    touchDragging_ = event.tfinger.fingerId;
    dragPointerOffsetX_ = x - getX();
    dragPointerOffsetY_ = y - getY();
    return false;
  case SDL_FINGERMOTION:
    if (touchDragging_ != event.tfinger.fingerId) {
      return true;
    }
    touchPosition(event.tfinger, x, y);
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    return false;
  case SDL_FINGERUP:
    if (touchDragging_ != event.tfinger.fingerId) {
      return true;
    }
    touchPosition(event.tfinger, x, y);
    touchDragging_ = -1;
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    state_.x = static_cast<float>(getX());
    state_.y = static_cast<float>(getY());
    state_.hasPosition = true;
    persist();
    return false;
  default:
    return true;
  }
}
