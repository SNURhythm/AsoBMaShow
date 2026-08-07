#include "VirtualControllerEditorView.h"

#include "../rendering/SimpleBatchRenderer.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr float kCanvasInset = 10.0F;
constexpr float kHandleSize = 26.0F;

void renderElement(rendering::SimpleBatchRenderer &batch,
                   const gameplay::VirtualControllerElement &element,
                   uint32_t border, uint32_t fill) {
  const auto &bounds = element.bounds;
  constexpr float borderWidth = 2.5F;
  if (element.shape == gameplay::VirtualControllerShape::Circle) {
    const float radius = std::min(bounds.width, bounds.height) * 0.5F;
    batch.addCircle(bounds.centerX(), bounds.centerY(), radius, border);
    batch.addCircle(bounds.centerX(), bounds.centerY(),
                    std::max(0.0F, radius - borderWidth), fill);
    return;
  }
  const float radius = std::min(bounds.height * 0.2F, 10.0F);
  batch.addRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height, radius,
                       border);
  batch.addRoundedRect(bounds.x + borderWidth, bounds.y + borderWidth,
                       std::max(0.0F, bounds.width - borderWidth * 2.0F),
                       std::max(0.0F, bounds.height - borderWidth * 2.0F),
                       std::max(0.0F, radius - borderWidth), fill);
}

} // namespace

VirtualControllerEditorView::VirtualControllerEditorView(
    input::VirtualControllerConfig config, CommitCallback onCommit)
    : config_(config), onCommit_(std::move(onCommit)) {
  std::vector<std::string> ignoredDiagnostics;
  config_.sanitize(ignoredDiagnostics);
}

gameplay::VirtualControllerCanvas VirtualControllerEditorView::canvas() const
    noexcept {
  return {.x = static_cast<float>(getX()) + kCanvasInset,
          .y = static_cast<float>(getY()) + kCanvasInset,
          .width = std::max(0.0F,
                            static_cast<float>(getWidth()) - kCanvasInset * 2.0F),
          .height = std::max(0.0F,
                             static_cast<float>(getHeight()) - kCanvasInset * 2.0F)};
}

gameplay::VirtualControllerLayout VirtualControllerEditorView::layout() const {
  return gameplay::makeVirtualControllerLayout(config_, 7, canvas());
}

bool VirtualControllerEditorView::contains(
    const gameplay::VirtualControllerRect &rect, float x, float y) noexcept {
  return rect.valid() && x >= rect.x && x <= rect.x + rect.width &&
         y >= rect.y && y <= rect.y + rect.height;
}

gameplay::VirtualControllerRect
VirtualControllerEditorView::handleRect(float centerX, float centerY) noexcept {
  return {.x = centerX - kHandleSize * 0.5F,
          .y = centerY - kHandleSize * 0.5F,
          .width = kHandleSize,
          .height = kHandleSize};
}

VirtualControllerEditorView::DragMode
VirtualControllerEditorView::hitDragMode(float uiX, float uiY) const {
  const auto currentLayout = layout();
  if (!currentLayout.valid()) {
    return DragMode::None;
  }
  const auto &bounds = currentLayout.bounds;
  if (contains(handleRect(bounds.x, bounds.y), uiX, uiY)) {
    return DragMode::Move;
  }
  if (contains(handleRect(bounds.x + bounds.width, bounds.y + bounds.height),
               uiX, uiY)) {
    return DragMode::Resize;
  }
  if (contains(handleRect(bounds.x + bounds.width,
                          bounds.y + bounds.height * 0.5F),
               uiX, uiY)) {
    return DragMode::Spacing;
  }
  return contains(bounds, uiX, uiY) ? DragMode::Move : DragMode::None;
}

bool VirtualControllerEditorView::beginDrag(std::int64_t pointerId, float uiX,
                                             float uiY) noexcept {
  if (dragMode_ != DragMode::None) {
    return false;
  }
  dragMode_ = hitDragMode(uiX, uiY);
  if (dragMode_ == DragMode::None) {
    return false;
  }
  activePointerId_ = pointerId;
  dragStartConfig_ = config_;
  dragStartX_ = uiX;
  dragStartY_ = uiY;
  return true;
}

bool VirtualControllerEditorView::updateDrag(float uiX, float uiY) noexcept {
  if (dragMode_ == DragMode::None) {
    return false;
  }
  const auto editCanvas = canvas();
  if (!editCanvas.valid()) {
    return false;
  }
  const float dx = uiX - dragStartX_;
  const float dy = uiY - dragStartY_;
  config_ = dragStartConfig_;
  switch (dragMode_) {
  case DragMode::Move:
    config_.centerX += dx / editCanvas.width;
    config_.centerY += dy / editCanvas.height;
    break;
  case DragMode::Resize:
    config_.buttonSize +=
        (dx + dy) * 0.5F / std::min(editCanvas.width, editCanvas.height);
    break;
  case DragMode::Spacing:
    config_.keyGap += dx /
                      std::max(1.0F, std::min(editCanvas.width,
                                               editCanvas.height) *
                                        std::max(dragStartConfig_.buttonSize,
                                                 0.025F));
    break;
  case DragMode::None:
    return false;
  }
  std::vector<std::string> ignoredDiagnostics;
  config_.sanitize(ignoredDiagnostics);
  return true;
}

void VirtualControllerEditorView::endDrag(std::int64_t pointerId) {
  if (dragMode_ == DragMode::None || pointerId != activePointerId_) {
    return;
  }
  dragMode_ = DragMode::None;
  activePointerId_ = -1;
  if (onCommit_) {
    onCommit_(config_);
  }
}

bool VirtualControllerEditorView::mousePoint(const SDL_MouseButtonEvent &event,
                                              float &x, float &y) noexcept {
  if (event.which == SDL_TOUCH_MOUSEID) {
    return false;
  }
  const float screenX = static_cast<float>(event.x) * rendering::widthScale;
  const float screenY = static_cast<float>(event.y) * rendering::heightScale;
  rendering::screenToUi(screenX, screenY, x, y);
  return true;
}

bool VirtualControllerEditorView::mousePoint(const SDL_MouseMotionEvent &event,
                                              float &x, float &y) noexcept {
  const float screenX = static_cast<float>(event.x) * rendering::widthScale;
  const float screenY = static_cast<float>(event.y) * rendering::heightScale;
  rendering::screenToUi(screenX, screenY, x, y);
  return true;
}

void VirtualControllerEditorView::fingerPoint(const SDL_TouchFingerEvent &event,
                                               float &x, float &y) noexcept {
  rendering::normalizedToUi(event.x, event.y, x, y);
}

bool VirtualControllerEditorView::handleEventsImpl(SDL_Event &event) {
  float uiX = 0.0F;
  float uiY = 0.0F;
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN:
    if (event.button.button != SDL_BUTTON_LEFT ||
        !mousePoint(event.button, uiX, uiY)) {
      return true;
    }
    return !beginDrag(0, uiX, uiY);
  case SDL_MOUSEMOTION:
    if (activePointerId_ != 0 || !mousePoint(event.motion, uiX, uiY)) {
      return true;
    }
    return !updateDrag(uiX, uiY);
  case SDL_MOUSEBUTTONUP:
    if (event.button.button != SDL_BUTTON_LEFT || activePointerId_ != 0 ||
        !mousePoint(event.button, uiX, uiY)) {
      return true;
    }
    (void)updateDrag(uiX, uiY);
    endDrag(0);
    return false;
  case SDL_FINGERDOWN:
    fingerPoint(event.tfinger, uiX, uiY);
    return !beginDrag(event.tfinger.fingerId, uiX, uiY);
  case SDL_FINGERMOTION:
    if (activePointerId_ != event.tfinger.fingerId) {
      return true;
    }
    fingerPoint(event.tfinger, uiX, uiY);
    return !updateDrag(uiX, uiY);
  case SDL_FINGERUP:
    if (activePointerId_ != event.tfinger.fingerId) {
      return true;
    }
    fingerPoint(event.tfinger, uiX, uiY);
    (void)updateDrag(uiX, uiY);
    endDrag(event.tfinger.fingerId);
    return false;
  default:
    return true;
  }
}

void VirtualControllerEditorView::renderImpl(RenderContext &context) {
  const auto editCanvas = canvas();
  if (!editCanvas.valid()) {
    return;
  }
  const auto currentLayout = layout();
  rendering::SimpleBatchRenderer batch;
  batch.setSubmitView(rendering::ui_view);
  batch.begin(context.getTransformMatrix());
  batch.addRoundedRect(editCanvas.x, editCanvas.y, editCanvas.width,
                       editCanvas.height, 14.0F,
                       ui_theme::withAlpha(ui_theme::panelSubtle(), 190)
                           .toABGR());
  if (currentLayout.valid()) {
    const uint32_t border = ui_theme::withAlpha(ui_theme::cyan(), 218).toABGR();
    const uint32_t fill = ui_theme::withAlpha(ui_theme::cyan(), 66).toABGR();
    for (const auto &element : currentLayout.elements) {
      renderElement(batch, element, border, fill);
    }
    const auto &bounds = currentLayout.bounds;
    const auto addHandle = [&](float x, float y, const Color &color) {
      const auto handle = handleRect(x, y);
      batch.addRoundedRect(handle.x, handle.y, handle.width, handle.height,
                           5.0F, color.toABGR());
    };
    addHandle(bounds.x, bounds.y, ui_theme::lime());
    addHandle(bounds.x + bounds.width, bounds.y + bounds.height,
              ui_theme::amber());
    addHandle(bounds.x + bounds.width, bounds.y + bounds.height * 0.5F,
              ui_theme::coral());
  }
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  batch.end();
}
