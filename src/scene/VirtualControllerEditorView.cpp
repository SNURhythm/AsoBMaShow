#include "VirtualControllerEditorView.h"

#include "../rendering/SimpleBatchRenderer.h"
#include "../view/AppTheme.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr float kCanvasInset = 28.0F;
constexpr float kHandleSize = 44.0F;

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
    const float markerRadius = radius * 0.66F;
    const auto marker = element.pointAt(markerRadius, 0);
    const float markerX = marker.x, markerY = marker.y;
    batch.addLine(bounds.centerX(), bounds.centerY(), markerX, markerY,
                  std::max(2.0F, radius * 0.045F), border);
    batch.addCircle(bounds.centerX(), bounds.centerY(), radius * 0.14F, border);
    batch.addCircle(markerX, markerY, radius * 0.09F, border);
    return;
  }
  const float radius = std::min(bounds.height * 0.2F, 10.0F);
  batch.addRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height, radius,
                       border, element.rotationDegrees);
  batch.addRoundedRect(bounds.x + borderWidth, bounds.y + borderWidth,
                       std::max(0.0F, bounds.width - borderWidth * 2.0F),
                       std::max(0.0F, bounds.height - borderWidth * 2.0F),
                       std::max(0.0F, radius - borderWidth), fill, element.rotationDegrees);
}

} // namespace

VirtualControllerEditorView::VirtualControllerEditorView(
    input::VirtualControllerConfig config, int keyMode, CommitCallback onCommit,
    EditMode editMode, int selectedControl, SelectionCallback onSelection)
    : config_(config), keyMode_(keyMode), onCommit_(std::move(onCommit)),
      editMode_(editMode), selectedControl_(selectedControl), onSelection_(std::move(onSelection)) {
  std::vector<std::string> ignoredDiagnostics;
  config_.sanitize(ignoredDiagnostics);
}

void VirtualControllerEditorView::selectControl(int index) noexcept {
  if (index == selectedControl_) return;
  // A toolbar selection from another finger must not retarget the captured drag.
  onPointerInputCancelled();
  selectedControl_ = index;
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
  return gameplay::makeVirtualControllerLayout(config_, keyMode_, canvas());
}

VirtualControllerEditorView::HandleCenters
VirtualControllerEditorView::handleCenters(
    const gameplay::VirtualControllerLayout &layout) noexcept {
  HandleCenters centers;
  if (!layout.valid()) {
    return centers;
  }
  const gameplay::VirtualControllerElement *scratch = nullptr;
  const gameplay::VirtualControllerElement *firstKey = nullptr;
  const gameplay::VirtualControllerElement *secondRowKey = nullptr;
  float keyplateLeft = std::numeric_limits<float>::infinity();
  float keyplateRight = -std::numeric_limits<float>::infinity();
  for (const auto &element : layout.elements) {
    if (element.control == gameplay::VirtualControllerControl::Scratch) {
      scratch = &element;
    }
    if (element.control != gameplay::VirtualControllerControl::Key) {
      continue;
    }
    keyplateLeft = std::min(keyplateLeft, element.bounds.x);
    keyplateRight = std::max(keyplateRight,
                             element.bounds.x + element.bounds.width);
    if (element.keyPosition == 0) {
      firstKey = &element;
    } else if (element.keyPosition == 1) {
      secondRowKey = &element;
    }
  }
  const auto &bounds = layout.bounds;
  centers.moveX = bounds.x;
  centers.moveY = bounds.y;
  centers.resizeX = bounds.x + bounds.width;
  centers.resizeY = bounds.y + bounds.height;
  if (firstKey != nullptr && secondRowKey != nullptr &&
      std::isfinite(keyplateLeft) && std::isfinite(keyplateRight)) {
    centers.keySpacingXX = keyplateRight + kHandleSize * 0.75F;
    centers.keySpacingXY =
        (firstKey->bounds.centerY() + secondRowKey->bounds.centerY()) * 0.5F;
    centers.keySpacingYX = (keyplateLeft + keyplateRight) * 0.5F;
    centers.keySpacingYY = centers.keySpacingXY;
  }
  if (scratch != nullptr && std::isfinite(keyplateLeft)) {
    centers.scratchSpacingX =
        (scratch->bounds.x + scratch->bounds.width + keyplateLeft) * 0.5F;
    centers.scratchSpacingY = scratch->bounds.centerY();
  }
  return centers;
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

const gameplay::VirtualControllerElement *VirtualControllerEditorView::selectedElement(
    const gameplay::VirtualControllerLayout &layout) const noexcept {
  for (const auto &element : layout.elements) {
    if (element.configIndex() == selectedControl_) return &element;
  }
  return nullptr;
}

gameplay::VirtualControllerRect VirtualControllerEditorView::resizeHandle(
    const gameplay::VirtualControllerElement &element) const noexcept {
  const auto area = canvas();
  // Keep a full finger-sized handle reachable even at a canvas edge.
  const auto corner = element.pointAt((element.bounds.width + kHandleSize) / 2,
                                      (element.bounds.height + kHandleSize) / 2);
  const float x = std::clamp(corner.x,
                             area.x, area.x + area.width);
  const float y = std::clamp(corner.y,
                             area.y, area.y + area.height);
  return handleRect(x, y);
}

VirtualControllerEditorView::DragMode
VirtualControllerEditorView::hitDragMode(float uiX, float uiY) const {
  const auto currentLayout = layout();
  if (!currentLayout.valid()) {
    return DragMode::None;
  }
  const auto &bounds = currentLayout.bounds;
  const auto handles = handleCenters(currentLayout);
  if (contains(handleRect(handles.resizeX, handles.resizeY), uiX, uiY)) {
    return DragMode::Resize;
  }
  if (contains(handleRect(handles.keySpacingXX, handles.keySpacingXY), uiX,
               uiY)) {
    return DragMode::KeySpacingX;
  }
  if ((keyMode_ % 2 != 0 || keyMode_ == 10 || keyMode_ == 14) &&
      contains(handleRect(handles.keySpacingYX, handles.keySpacingYY), uiX, uiY)) {
    return DragMode::KeySpacingY;
  }
  if (config_.scratchEnabled &&
      contains(handleRect(handles.scratchSpacingX, handles.scratchSpacingY), uiX, uiY)) {
    return DragMode::ScratchKeyplateSpacing;
  }
  if (contains(handleRect(handles.moveX, handles.moveY), uiX, uiY)) {
    return DragMode::Move;
  }
  return contains(bounds, uiX, uiY) ? DragMode::Move : DragMode::None;
}

bool VirtualControllerEditorView::beginDrag(std::int64_t pointerId, float uiX,
                                             float uiY) noexcept {
  if (dragMode_ != DragMode::None) {
    return false;
  }
  const auto currentLayout = layout();
  if (editMode_ == EditMode::Controls) {
    const auto *selected = selectedElement(currentLayout);
    const auto hit = [&](const gameplay::VirtualControllerElement &element) {
      const auto local = gameplay::rotateControllerOffset(uiX - element.bounds.centerX(),
          uiY - element.bounds.centerY(), -element.rotationDegrees);
      return std::abs(local.x) <= element.bounds.width / 2 &&
             std::abs(local.y) <= element.bounds.height / 2 &&
             (element.shape != gameplay::VirtualControllerShape::Circle ||
              std::hypot(uiX - element.bounds.centerX(), uiY - element.bounds.centerY()) <=
                  element.bounds.width * 0.5F);
    };
    if (selected && contains(resizeHandle(*selected), uiX, uiY)) {
      dragMode_ = DragMode::ControlResize;
    } else if (selected && hit(*selected)) {
      // Arrow selection must make a covered control draggable out of an overlap.
      if (onSelection_) onSelection_(selectedControl_);
      dragMode_ = DragMode::ControlMove;
    } else {
      const gameplay::VirtualControllerElement *nearest = nullptr;
      float nearestDistance = std::numeric_limits<float>::infinity();
      for (const auto &element : currentLayout.elements) {
        if (!hit(element)) continue;
        const float distance = std::hypot(uiX - element.bounds.centerX(), uiY - element.bounds.centerY());
        if (distance < nearestDistance) {
          nearest = &element;
          nearestDistance = distance;
        }
      }
      if (nearest) {
        selectedControl_ = nearest->configIndex();
        if (onSelection_) onSelection_(selectedControl_);
        dragMode_ = DragMode::ControlMove;
      }
    }
  } else if (editMode_ == EditMode::Compensation) {
    const auto area = canvas();
    if (contains({area.x, area.y, area.width, area.height}, uiX, uiY))
      dragMode_ = DragMode::Compensation;
  } else {
    dragMode_ = hitDragMode(uiX, uiY);
  }
  if (dragMode_ == DragMode::None) {
    return false;
  }
  activePointerId_ = pointerId;
  dragOriginalConfig_ = config_;
  dragStartConfig_ = config_;
  dragStartLayout_ = layout();
  if (dragMode_ == DragMode::ControlMove || dragMode_ == DragMode::ControlResize) {
    auto baseConfig = config_;
    baseConfig.controls[selectedControl_] = {};
    const auto baseLayout = gameplay::makeVirtualControllerLayout(baseConfig, keyMode_, canvas());
    const auto *base = selectedElement(baseLayout);
    const auto *visible = selectedElement(dragStartLayout_);
    if (base && visible && dragStartLayout_.unit > 0.0F) {
      // Start from the visible position, not an off-screen numeric offset that
      // was clamped by the layout (also possible after rotation or resizing).
      auto &custom = dragStartConfig_.controls[selectedControl_];
      const auto delta = gameplay::rotateControllerOffset(visible->bounds.centerX() - base->bounds.centerX(),
          visible->bounds.centerY() - base->bounds.centerY(), -config_.plateRotationDegrees);
      custom.offsetX = delta.x / dragStartLayout_.unit;
      custom.offsetY = delta.y / dragStartLayout_.unit;
      if (dragMode_ == DragMode::ControlResize)
        custom.sizeScale = visible->bounds.width / base->bounds.width;
    }
  }
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
  const float keyWidth = dragStartLayout_.unit;
  const float keyHeight = keyWidth * 2.0F;
  switch (dragMode_) {
  case DragMode::Move:
    config_.centerX += dx / editCanvas.width;
    config_.centerY += dy / editCanvas.height;
    break;
  case DragMode::Resize:
    config_.buttonSize +=
        (dx + dy) * 0.5F / std::min(editCanvas.width, editCanvas.height);
    break;
  case DragMode::KeySpacingX:
    if (keyWidth <= 0.0F) {
      return false;
    }
    config_.keySpacingX += dx / keyWidth;
    break;
  case DragMode::KeySpacingY:
    if (keyHeight <= 0.0F) {
      return false;
    }
    config_.keySpacingY += dy / keyHeight;
    break;
  case DragMode::ScratchKeyplateSpacing:
    if (keyWidth <= 0.0F) {
      return false;
    }
    config_.scratchKeyplateSpacing += dx / keyWidth;
    break;
  case DragMode::ControlMove: {
    const auto *element = selectedElement(dragStartLayout_);
    if (!element || dragStartLayout_.unit <= 0.0F) return false;
    // Keep the saved offset within the visible canvas.
    auto &custom = config_.controls[selectedControl_];
    const auto axis = gameplay::rotateControllerOffset(1, 0,
        element->scratch ? 0 : element->rotationDegrees);
    const auto &bounds = element->bounds;
    const float halfWidth = (std::abs(axis.x) * bounds.width + std::abs(axis.y) * bounds.height) / 2;
    const float halfHeight = (std::abs(axis.y) * bounds.width + std::abs(axis.x) * bounds.height) / 2;
    const float left = editCanvas.x + halfWidth - bounds.centerX();
    const float top = editCanvas.y + halfHeight - bounds.centerY();
    const float right = editCanvas.x + editCanvas.width - halfWidth - bounds.centerX();
    const float bottom = editCanvas.y + editCanvas.height - halfHeight - bounds.centerY();
    const auto delta = gameplay::rotateControllerOffset(std::clamp(dx, left, std::max(left, right)),
        std::clamp(dy, top, std::max(top, bottom)), -config_.plateRotationDegrees);
    custom.offsetX += delta.x / dragStartLayout_.unit;
    custom.offsetY += delta.y / dragStartLayout_.unit;
    break;
  }
  case DragMode::ControlResize: {
    const auto *element = selectedElement(dragStartLayout_);
    if (!element) return false;
    const auto &bounds = element->bounds;
    // Project the drag onto the diagonal: either axis works and aspect is preserved.
    const auto local = gameplay::rotateControllerOffset(dx, dy, -element->rotationDegrees);
    const float factor = 1.0F + 2.0F * (local.x * bounds.width + local.y * bounds.height) /
        (bounds.width * bounds.width + bounds.height * bounds.height);
    config_.controls[selectedControl_].sizeScale *= factor;
    break;
  }
  case DragMode::Compensation:
    config_.touchCompensation += dx / std::max(1.0F, editCanvas.width * 0.5F);
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
  dragStartLayout_ = {};
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
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.button != SDL_BUTTON_LEFT ||
        !mousePoint(event.button, uiX, uiY)) {
      return true;
    }
    return !beginDrag(0, uiX, uiY);
  case SDL_EVENT_MOUSE_MOTION:
    if (activePointerId_ != 0 || !mousePoint(event.motion, uiX, uiY)) {
      return true;
    }
    return !updateDrag(uiX, uiY);
  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (event.button.button != SDL_BUTTON_LEFT || activePointerId_ != 0 ||
        !mousePoint(event.button, uiX, uiY)) {
      return true;
    }
    (void)updateDrag(uiX, uiY);
    endDrag(0);
    return false;
  case SDL_EVENT_FINGER_DOWN:
    fingerPoint(event.tfinger, uiX, uiY);
    return !beginDrag(event.tfinger.fingerID, uiX, uiY);
  case SDL_EVENT_FINGER_MOTION:
    if (activePointerId_ != event.tfinger.fingerID) {
      return true;
    }
    fingerPoint(event.tfinger, uiX, uiY);
    return !updateDrag(uiX, uiY);
  case SDL_EVENT_FINGER_CANCELED:
    if (activePointerId_ == event.tfinger.fingerID) onPointerInputCancelled();
    return false;
  case SDL_EVENT_FINGER_UP:
    if (activePointerId_ != event.tfinger.fingerID) {
      return true;
    }
    fingerPoint(event.tfinger, uiX, uiY);
    (void)updateDrag(uiX, uiY);
    endDrag(event.tfinger.fingerID);
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
                       app_theme::withAlpha(app_theme::panelSubtle(), 190)
                           .toABGR());
  if (currentLayout.valid()) {
    const uint32_t border = ui_theme::withAlpha(ui_theme::cyan(), 218).toABGR();
    const uint32_t fill = ui_theme::withAlpha(ui_theme::cyan(), 66).toABGR();
    for (const auto &element : currentLayout.elements) {
      if (editMode_ == EditMode::Compensation) {
        auto expanded = element;
        const float extra = std::min(element.bounds.width, element.bounds.height) *
                            currentLayout.touchCompensation;
        expanded.bounds.x -= extra * 0.5F;
        expanded.bounds.y -= extra * 0.5F;
        expanded.bounds.width += extra;
        expanded.bounds.height += extra;
        renderElement(batch, expanded, ui_theme::withAlpha(ui_theme::coral(), 180).toABGR(),
                       ui_theme::withAlpha(ui_theme::coral(), 30).toABGR());
      }
      const bool selected = editMode_ == EditMode::Controls && element.configIndex() == selectedControl_;
      renderElement(batch, element, selected ? ui_theme::amber().toABGR() : border, fill);
    }
    if (editMode_ == EditMode::Controls) {
      if (const auto *element = selectedElement(currentLayout)) {
        const auto handle = resizeHandle(*element);
        batch.addRoundedRect(handle.x, handle.y, handle.width, handle.height,
                             8.0F, ui_theme::amber().toABGR());
        batch.addLine(handle.x + 13, handle.y + 31, handle.x + 31, handle.y + 13,
                       3.0F, app_theme::backdrop().toABGR());
      }
    }
    if (editMode_ == EditMode::Whole) {
      const auto handles = handleCenters(currentLayout);
      const auto addHandle = [&](float x, float y, const Color &color) {
        const auto handle = handleRect(x, y);
        batch.addRoundedRect(handle.x, handle.y, handle.width, handle.height,
                             5.0F, color.toABGR());
      };
      addHandle(handles.moveX, handles.moveY, ui_theme::lime());
      addHandle(handles.resizeX, handles.resizeY, ui_theme::amber());
      addHandle(handles.keySpacingXX, handles.keySpacingXY, ui_theme::cyan());
      if (keyMode_ % 2 != 0 || keyMode_ == 10 || keyMode_ == 14) {
        addHandle(handles.keySpacingYX, handles.keySpacingYY, ui_theme::violetAction());
      }
      if (config_.scratchEnabled) {
        addHandle(handles.scratchSpacingX, handles.scratchSpacingY, ui_theme::coral());
      }
    }
  }
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  batch.end();
}

void VirtualControllerEditorView::onPointerInputCancelled() {
  // The movement baseline may normalize clamped values; rollback must not.
  if (dragMode_ != DragMode::None) config_ = dragOriginalConfig_;
  dragMode_ = DragMode::None;
  activePointerId_ = -1;
  dragStartLayout_ = {};
}

void VirtualControllerEditorView::onPointerEventConsumed(const SDL_Event &event) {
  if ((event.type == SDL_EVENT_MOUSE_BUTTON_UP && activePointerId_ == 0 &&
       event.button.button == SDL_BUTTON_LEFT) ||
      ((event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) &&
       activePointerId_ == event.tfinger.fingerID)) {
    onPointerInputCancelled();
  }
}
