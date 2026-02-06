#include "View.h"

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