#include "MusicSelectToolbarView.h"

#include "../input/SDLPointerEvent.h"
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
constexpr int kBorderWidth = 1;
constexpr float kDefaultPosition = 24.0F;

std::uint32_t codepointFor(MusicSelectToolbarControl control) {
  switch (control) {
  case MusicSelectToolbarControl::Drag:
    return ui_icons::kDrag;
  case MusicSelectToolbarControl::Collapse:
    return ui_icons::kCollapse;
  case MusicSelectToolbarControl::Expand:
    return ui_icons::kExpand;
  default:
    return 0;
  }
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
                MusicSelectToolbarControl::ChartMenu,
                MusicSelectToolbarControl::PlayOptions,
                MusicSelectToolbarControl::Rankings,
                MusicSelectToolbarControl::MoreMenu,
                MusicSelectToolbarControl::Collapse};

  // Yoga's declared size includes both padding and border.
  const float inset = kPadding + kBorderWidth;
  const float availableWidth =
      std::max(kControlSize, static_cast<float>(viewportWidth_) - inset * 2);
  setPadding(Edge::All, kPadding);
  setGap(kGap);
  setFlexDirection(FlexDirection::Row);
  setFlexWrap(YGWrapWrap);
  setAlignItems(YGAlignCenter);
  setThemedBackgroundColor(ui_theme::panelStrong);
  setThemedBorderColor(ui_theme::hairlineStrong);
  setBorderWidth(kBorderWidth);
  setCornerRadius(ui_theme::controlRadius());
  setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);

  std::vector<float> widths;
  for (const auto control : layout) {
    const auto codepoint = codepointFor(control);
    TextView *content = nullptr;
    TextView *label = nullptr;
    float width = kControlSize;
    if (codepoint != 0) {
      content = makeIcon(codepoint);
    } else {
      label = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
      std::string text;
      switch (control) {
      case MusicSelectToolbarControl::ChartMenu:
        text = std::string(i18n::tr("music_select.toolbar.chart.label")) + " ▾";
        break;
      case MusicSelectToolbarControl::MoreMenu:
        text = std::string(i18n::tr("music_select.toolbar.more.label")) + " ▾";
        break;
      case MusicSelectToolbarControl::Rankings:
        text = i18n::tr("menu.rankings.label");
        break;
      case MusicSelectToolbarControl::PlayOptions:
        text = i18n::tr("music_select.toolbar.play_options.label");
        break;
      default:
        break;
      }
      label->setText(text);
      label->setAlign(TextView::CENTER);
      label->setVAlign(TextView::MIDDLE);
      label->setOverflow(TextView::TextOverflow::Hidden);
      label->setThemedColor(ui_theme::textPrimary);
      width = std::min(availableWidth,
                       std::max(kControlSize,
                                static_cast<float>(label->measureTextWidth(text)) +
                                    24));
      content = label;
    }
    widths.push_back(width);
    controls_.push_back({.control = control,
                         .codepoint = codepoint,
                         .icon = label ? nullptr : content,
                         .label = label});
    if (control == MusicSelectToolbarControl::Drag) {
      addView(content);
      continue;
    }
    auto *button = new Button();
    controls_.back().button = button;
    button->setEnabled(isControlEnabled(control));
    button->setWidth(width);
    button->setFlexShrink(0.0F);
    button->setHeight(kControlSize);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setThemedBackgroundColors(ui_theme::control,
                                      ui_theme::controlHover,
                                      ui_theme::controlPressed);
    button->setThemedBorderColors(ui_theme::hairlineStrong,
                                  ui_theme::accentBorder,
                                  ui_theme::accentBorderStrong);
    button->setStyledBorderWidth(1);
    button->setContentView(content);
    button->setOnClickListener([this, control] { activateControl(control); });
    addView(button);
  }
  float rowWidth = 0;
  float widestRow = 0;
  int rows = 1;
  for (const float width : widths) {
    if (rowWidth > 0 && rowWidth + kGap + width > availableWidth) {
      widestRow = std::max(widestRow, rowWidth);
      rowWidth = 0;
      ++rows;
    }
    rowWidth += (rowWidth > 0 ? kGap : 0) + width;
  }
  setWidth(inset * 2 + std::max(widestRow, rowWidth));
  setHeight(inset * 2 + kControlSize * rows + kGap * (rows - 1));
}

void MusicSelectToolbarView::onLanguageChanged() {
  rebuild();
  place(state_.hasPosition ? state_.x : kDefaultPosition,
        state_.hasPosition ? state_.y : kDefaultPosition);
}

void MusicSelectToolbarView::activateControl(
    MusicSelectToolbarControl control) {
  if (!isControlEnabled(control)) return;
  switch (control) {
  case MusicSelectToolbarControl::Drag:
    break;
  case MusicSelectToolbarControl::ChartMenu:
    if (callbacks_.openChartMenu) callbacks_.openChartMenu();
    break;
  case MusicSelectToolbarControl::MoreMenu:
    if (callbacks_.openMoreMenu) callbacks_.openMoreMenu();
    break;
  case MusicSelectToolbarControl::ChartViewer:
    if (callbacks_.openChartViewer) {
      callbacks_.openChartViewer();
    }
    break;
  case MusicSelectToolbarControl::ChartRecords:
    if (callbacks_.openChartRecords) {
      callbacks_.openChartRecords();
    }
    break;
  case MusicSelectToolbarControl::Rankings:
    if (callbacks_.openRankings) callbacks_.openRankings();
    break;
  case MusicSelectToolbarControl::RevealChart:
    if (callbacks_.revealChart) {
      callbacks_.revealChart();
    }
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
  case MusicSelectToolbarControl::PlayOptions:
    if (callbacks_.openPlayOptions) {
      callbacks_.openPlayOptions();
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
  }
}

bool MusicSelectToolbarView::isControlEnabled(MusicSelectToolbarControl control) const {
  if (disabledControls_.contains(control)) return false;
  switch (control) {
  case MusicSelectToolbarControl::ChartMenu: return bool(callbacks_.openChartMenu);
  case MusicSelectToolbarControl::MoreMenu: return bool(callbacks_.openMoreMenu);
  case MusicSelectToolbarControl::ChartViewer: return bool(callbacks_.openChartViewer);
  case MusicSelectToolbarControl::ChartRecords: return bool(callbacks_.openChartRecords);
  case MusicSelectToolbarControl::Rankings: return bool(callbacks_.openRankings);
  case MusicSelectToolbarControl::RevealChart: return bool(callbacks_.revealChart);
  case MusicSelectToolbarControl::MusicPlayer: return bool(callbacks_.openMusicPlayer);
  case MusicSelectToolbarControl::Tasks: return bool(callbacks_.openTasks);
  case MusicSelectToolbarControl::PlayOptions: return bool(callbacks_.openPlayOptions);
  case MusicSelectToolbarControl::IrUploads: return bool(callbacks_.openIrUploads);
  case MusicSelectToolbarControl::Settings: return bool(callbacks_.openSettings);
  default: return true;
  }
}

void MusicSelectToolbarView::setControlEnabled(MusicSelectToolbarControl control,
                                               bool enabled) {
  if (enabled) disabledControls_.erase(control);
  else disabledControls_.insert(control);
  for (const auto &rendered : controls_) {
    if (rendered.control == control && rendered.button) {
      rendered.button->setEnabled(isControlEnabled(control));
    }
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
  const bool layoutChanged =
      viewportWidth_ != width || viewportHeight_ != height;
  viewportWidth_ = width;
  viewportHeight_ = height;
  if (mouseDragging_ || touchDragging_ != -1) {
    place(static_cast<float>(getX()), static_cast<float>(getY()));
    return;
  }
  if (layoutChanged) {
    rebuild();
  }
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

void MusicSelectToolbarView::onPointerEventConsumed(const SDL_Event &event) {
  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && mouseDragging_ &&
      event.button.button == SDL_BUTTON_LEFT &&
      event.button.which != SDL_TOUCH_MOUSEID) {
    mouseDragging_ = false;
  } else if ((event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) && touchDragging_ != -1 &&
             !sdl_pointer_event::isMouseSynthesizedTouch(event) &&
             event.tfinger.fingerID == touchDragging_) {
    touchDragging_ = -1;
  } else {
    return;
  }
  state_.x = static_cast<float>(getX());
  state_.y = static_cast<float>(getY());
  state_.hasPosition = true;
  persist();
}

bool MusicSelectToolbarView::handleEventsImpl(SDL_Event &event) {
  float x = 0.0F;
  float y = 0.0F;
  switch (event.type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
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
  case SDL_EVENT_MOUSE_MOTION:
    if (!mouseDragging_) {
      return true;
    }
    mousePosition(event.motion, x, y);
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    return false;
  case SDL_EVENT_MOUSE_BUTTON_UP:
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
  case SDL_EVENT_FINGER_DOWN:
    if (touchDragging_ != -1) {
      return true;
    }
    touchPosition(event.tfinger, x, y);
    if (!insideDragHandle(x, y)) {
      return true;
    }
    touchDragging_ = event.tfinger.fingerID;
    dragPointerOffsetX_ = x - getX();
    dragPointerOffsetY_ = y - getY();
    return false;
  case SDL_EVENT_FINGER_MOTION:
    if (touchDragging_ != event.tfinger.fingerID) {
      return true;
    }
    touchPosition(event.tfinger, x, y);
    place(x - dragPointerOffsetX_, y - dragPointerOffsetY_);
    return false;
  case SDL_EVENT_FINGER_UP:
    if (touchDragging_ != event.tfinger.fingerID) {
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

void MusicSelectToolbarView::onPointerInputCancelled() {
  mouseDragging_ = false;
  touchDragging_ = -1;
}
