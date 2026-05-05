#include "View.h"

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
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(rendering::ui_view, kSimpleProgram);
}
} // namespace

View *View::setWidth(float width) {
  YGNodeStyleSetWidth(node, width);
  return this;
}

View *View::setHeight(float height) {
  YGNodeStyleSetHeight(node, height);
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
  return this;
}

View *View::clearBackgroundColor() {
  hasBackground = false;
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
  YGNodeInsertChild(node, view->getNode(), YGNodeGetChildCount(node));
  view->parent = this;
  view->insertionOrder = nextInsertionOrder++;
  children.push_back(view);
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

void View::renderBoxDecoration(RenderContext &context) const {
  if ((!hasBackground && (!hasBorder || borderWidth <= 0)) || getWidth() <= 0 ||
      getHeight() <= 0) {
    return;
  }

  const int x = getX();
  const int y = getY();
  const int width = getWidth();
  const int height = getHeight();

  if (hasBorder && borderWidth > 0) {
    submitColoredRect(context, x, y, width, height, borderColor);
  }

  if (hasBackground) {
    int inset = hasBorder ? borderWidth : 0;
    inset = std::min(inset, std::min(width / 2, height / 2));
    submitColoredRect(context, x + inset, y + inset, width - inset * 2,
                      height - inset * 2, backgroundColor);
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
    child->applyYogaLayout();
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
