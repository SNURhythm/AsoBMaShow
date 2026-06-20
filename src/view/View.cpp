#include "View.h"

#include <utility>

namespace {
void submitColoredRect(const RenderContext &context, int x, int y, int width,
                       int height, const Color &color) {
  if (width <= 0 || height <= 0 || color.a == 0) {
    return;
  }

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  rendering::createRect(tvb, tib, x, y, width, height, color.toABGR());
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(rendering::ui_view, kSimpleProgram);
}

void submitGradientRect(const RenderContext &context, int x, int y, int width,
                        int height, const Color &topColor,
                        const Color &bottomColor) {
  if (width <= 0 || height <= 0 ||
      (topColor.a == 0 && bottomColor.a == 0)) {
    return;
  }

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  bgfx::allocTransientVertexBuffer(&tvb, 4,
                                   rendering::PosColorVertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);

  auto *vertices = reinterpret_cast<rendering::PosColorVertex *>(tvb.data);
  auto *indices = reinterpret_cast<uint16_t *>(tib.data);
  const uint32_t top = topColor.toABGR();
  const uint32_t bottom = bottomColor.toABGR();

  vertices[0] = {static_cast<float>(x), static_cast<float>(y), 0.0f, top};
  vertices[1] = {static_cast<float>(x + width), static_cast<float>(y), 0.0f,
                 top};
  vertices[2] = {static_cast<float>(x + width), static_cast<float>(y + height),
                 0.0f, bottom};
  vertices[3] = {static_cast<float>(x), static_cast<float>(y + height), 0.0f,
                 bottom};

  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 2;
  indices[4] = 3;
  indices[5] = 0;

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(rendering::ui_view, kSimpleProgram);
}
} // namespace

void View::eraseInactiveTemporaryEventListeners() {
  temporaryEventListeners.erase(
      std::remove_if(temporaryEventListeners.begin(),
                     temporaryEventListeners.end(),
                     [](const auto &entry) { return !entry.active; }),
      temporaryEventListeners.end());
}

uint64_t
View::addTemporaryEventListener(TemporaryEventListener listener) {
  if (!listener) {
    return 0;
  }
  const uint64_t listenerId = nextTemporaryEventListenerId++;
  temporaryEventListeners.push_back({listenerId, std::move(listener), true});
  return listenerId;
}

void View::removeTemporaryEventListener(uint64_t listenerId) {
  if (listenerId == 0) {
    return;
  }
  for (auto &entry : temporaryEventListeners) {
    if (entry.id == listenerId) {
      entry.active = false;
      break;
    }
  }
  if (!dispatchingTemporaryEventListeners) {
    eraseInactiveTemporaryEventListeners();
  }
}

void View::dispatchTemporaryEventListeners(SDL_Event &event) {
  dispatchingTemporaryEventListeners = true;
  const size_t listenerCount = temporaryEventListeners.size();
  for (size_t i = 0; i < listenerCount && i < temporaryEventListeners.size();
       ++i) {
    auto &entry = temporaryEventListeners[i];
    if (entry.active) {
      entry.listener(event);
    }
  }
  dispatchingTemporaryEventListeners = false;
  eraseInactiveTemporaryEventListeners();
}

View *View::setWidth(float width) {
  YGNodeStyleSetWidth(node, width);
  return this;
}

View *View::setWidthPercent(float widthPercent) {
  YGNodeStyleSetWidthPercent(node, widthPercent);
  return this;
}

View *View::setHeight(float height) {
  YGNodeStyleSetHeight(node, height);
  return this;
}

View *View::setMinWidth(float minWidth) {
  YGNodeStyleSetMinWidth(node, minWidth);
  return this;
}

View *View::setMinHeight(float minHeight) {
  YGNodeStyleSetMinHeight(node, minHeight);
  return this;
}

View *View::setFlex(float flex) {
  YGNodeStyleSetFlex(node, flex);
  return this;
}

View *View::setFlexGrow(float flexGrow) {
  YGNodeStyleSetFlexGrow(node, flexGrow);
  return this;
}

View *View::setFlexBasis(float flexBasis) {
  YGNodeStyleSetFlexBasis(node, flexBasis);
  return this;
}

View *View::setFlexWrap(YGWrap flexWrap) {
  YGNodeStyleSetFlexWrap(node, flexWrap);
  return this;
}

View *View::setFlexShrink(float flexShrink) {
  YGNodeStyleSetFlexShrink(node, flexShrink);
  return this;
}

View *View::setMargin(Edge edge, float margin) {
  YGNodeStyleSetMargin(node, static_cast<YGEdge>(edge), margin);
  return this;
}

View *View::setPadding(Edge edge, float padding) {
  YGNodeStyleSetPadding(node, static_cast<YGEdge>(edge), padding);
  return this;
}

View *View::setPosition(Edge edge, float position) {
  YGNodeStyleSetPosition(node, static_cast<YGEdge>(edge), position);
  return this;
}

View *View::setPositionType(YGPositionType positionType) {
  YGNodeStyleSetPositionType(node, positionType);
  return this;
}

View *View::setAlignItems(YGAlign align) {
  YGNodeStyleSetAlignItems(node, align);
  return this;
}

View *View::setAlignSelf(YGAlign align) {
  YGNodeStyleSetAlignSelf(node, align);
  return this;
}

View *View::setAlignContent(YGAlign align) {
  YGNodeStyleSetAlignContent(node, align);
  return this;
}

View *View::setJustifyContent(YGJustify justify) {
  YGNodeStyleSetJustifyContent(node, justify);
  return this;
}

View *View::setFlexDirection(FlexDirection direction) {
  YGNodeStyleSetFlexDirection(node, static_cast<YGFlexDirection>(direction));
  return this;
}

View *View::setGap(YGGutter gutter, float gap) {
  YGNodeStyleSetGap(node, gutter, gap);
  return this;
}

View *View::setGap(float gap) {
  YGNodeStyleSetGap(node, YGGutterAll, gap);
  return this;
}

View *View::setDirection(YGDirection direction) {
  YGNodeStyleSetDirection(node, direction);
  return this;
}

View *View::setBackgroundColor(const Color &color) {
  backgroundColor = color;
  hasBackground = true;
  hasGradientBackground = false;
  return this;
}

View *View::setBackgroundGradient(const Color &topColor,
                                  const Color &bottomColor) {
  backgroundGradientTopColor = topColor;
  backgroundGradientBottomColor = bottomColor;
  hasBackground = true;
  hasGradientBackground = true;
  return this;
}

View *View::clearBackgroundColor() {
  hasBackground = false;
  hasGradientBackground = false;
  return this;
}

View *View::setBorderColor(const Color &color) {
  borderColor = color;
  hasBorder = true;
  return this;
}

View *View::clearBorderColor() {
  hasBorder = false;
  return this;
}

View *View::setBorderWidth(int width) {
  borderWidth = std::max(0, width);
  return this;
}

View *View::addView(View *view) {
  if (view == nullptr) {
    return this;
  }
  std::unique_ptr<View> pending(view);
  children.push_back(view);
  pending.release();
  YGNodeInsertChild(node, view->getNode(), YGNodeGetChildCount(node));
  view->parent = this;
  view->insertionOrder = nextInsertionOrder++;
  childrenOrderDirty = true;
  applyYogaLayout();
  return this;
}

void View::applyYogaLayout() {
  if (layoutBatchDepth > 0) {
    markLayoutDirty();
    return;
  }
  applyYogaLayoutImmediate();
}

void View::applyYogaLayoutFromRoot() {
  if (layoutBatchDepth > 0) {
    markLayoutDirty();
    return;
  }
  View *root = this;
  while (root->parent != nullptr) {
    root = root->parent;
  }
  root->applyYogaLayoutImmediate();
}

void View::renderBoxDecoration(RenderContext &context) const {
  if ((!hasBackground && (!hasBorder || borderWidth <= 0)) || getWidth() <= 0 ||
      getHeight() <= 0) {
    return;
  }

  const int x = getX();
  const int y = getY();
  const int width = getWidth();
  const int height = getHeight();

  int inset = hasBorder ? borderWidth : 0;
  inset = std::min(inset, std::min(width / 2, height / 2));

  if (hasBorder && inset > 0) {
    submitColoredRect(context, x, y, width, inset, borderColor);
    submitColoredRect(context, x, y + height - inset, width, inset,
                      borderColor);

    const int middleHeight = height - inset * 2;
    if (middleHeight > 0) {
      submitColoredRect(context, x, y + inset, inset, middleHeight,
                        borderColor);
      submitColoredRect(context, x + width - inset, y + inset, inset,
                        middleHeight, borderColor);
    }
  }

  if (hasBackground) {
    const int backgroundX = x + inset;
    const int backgroundY = y + inset;
    const int backgroundWidth = width - inset * 2;
    const int backgroundHeight = height - inset * 2;
    if (hasGradientBackground) {
      submitGradientRect(context, backgroundX, backgroundY, backgroundWidth,
                         backgroundHeight, backgroundGradientTopColor,
                         backgroundGradientBottomColor);
    } else {
      submitColoredRect(context, backgroundX, backgroundY, backgroundWidth,
                        backgroundHeight, backgroundColor);
    }
  }
}

void View::applyYogaLayoutImmediate() {
  auto prevWidth = YGNodeLayoutGetWidth(node);
  auto prevHeight = YGNodeLayoutGetHeight(node);
  // Only calculate layout from root node
  if (YGNodeGetParent(node) == nullptr) {
    YGNodeCalculateLayout(node, YGUndefined, YGUndefined, YGDirectionLTR);
  }

  // Update position and dimensions
  absoluteX = YGNodeLayoutGetLeft(node);
  absoluteY = YGNodeLayoutGetTop(node);

  // Accumulate parent positions
  YGNodeRef parent = YGNodeGetParent(node);
  while (parent != nullptr) {
    absoluteX += YGNodeLayoutGetLeft(parent);
    absoluteY += YGNodeLayoutGetTop(parent);
    parent = YGNodeGetParent(parent);
  }

  // Recursively update children positions
  for (auto child : children) {
    child->applyYogaLayoutImmediate();
  }

  // Call onLayout to notify derived classes
  onLayout();
  auto newWidth = YGNodeLayoutGetWidth(node);
  auto newHeight = YGNodeLayoutGetHeight(node);
  if (prevWidth != newWidth || prevHeight != newHeight) {
    onResize(newWidth, newHeight);
  }
}

View* View::findViewByName(const std::string& targetName) {
    if (this->name == targetName) {
        return this;
    }
    for (auto* child : children) {
        View* found = child->findViewByName(targetName);
        if (found) {
            return found;
        }
    }
    return nullptr;
}
