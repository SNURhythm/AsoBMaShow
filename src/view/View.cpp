#include "View.h"

#include "../rendering/UniformCache.h"

#include <cmath>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

bool prepareShadowRenderContext(const RenderContext &context,
                                RenderContext &shadowContext, int x, int y,
                                int width, int height, int spread,
                                int offsetX, int offsetY) {
  shadowContext = context;
  if (context.scissor.width < 0 || context.scissor.height < 0) {
    return true;
  }

  if (context.scissor.width <= 0 || context.scissor.height <= 0) {
    return false;
  }

  const int clipLeft = context.scissor.x;
  const int clipTop = context.scissor.y;
  const int clipRight = context.scissor.x + context.scissor.width;
  const int clipBottom = context.scissor.y + context.scissor.height;
  const int ownerLeft = x;
  const int ownerTop = y;
  const int ownerRight = x + width;
  const int ownerBottom = y + height;
  if (ownerRight <= clipLeft || ownerLeft >= clipRight ||
      ownerBottom <= clipTop || ownerTop >= clipBottom) {
    return false;
  }

  const int bleedX = spread + std::abs(offsetX) + 2;
  const int bleedY = spread + std::abs(offsetY) + 2;
  const int leftBleed = ownerLeft >= clipLeft ? bleedX : 0;
  const int topBleed = ownerTop >= clipTop ? bleedY : 0;
  const int rightBleed = ownerRight <= clipRight ? bleedX : 0;
  const int bottomBleed = ownerBottom <= clipBottom ? bleedY : 0;

  shadowContext.scissor.x -= leftBleed;
  shadowContext.scissor.y -= topBleed;
  shadowContext.scissor.width += leftBleed + rightBleed;
  shadowContext.scissor.height += topBleed + bottomBleed;
  return shadowContext.scissor.width > 0 && shadowContext.scissor.height > 0;
}

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

void submitRoundedRect(const RenderContext &context, int x, int y, int width,
                       int height, float radius, const Color &color) {
  if (width <= 0 || height <= 0 || color.a == 0) {
    return;
  }

  radius = std::clamp(radius, 0.0f,
                      static_cast<float>(std::min(width, height)) * 0.5f);
  if (radius <= 0.5f) {
    submitColoredRect(context, x, y, width, height, color);
    return;
  }

  const int segments =
      std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
  const uint16_t ringVertexCount = static_cast<uint16_t>((segments + 1) * 4);
  const uint16_t vertexCount = static_cast<uint16_t>(ringVertexCount + 1);
  const uint16_t indexCount = static_cast<uint16_t>(ringVertexCount * 3);

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  if (bgfx::getAvailTransientVertexBuffer(
          vertexCount, rendering::PosColorVertex::ms_decl) < vertexCount ||
      bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount) {
    return;
  }
  bgfx::allocTransientVertexBuffer(&tvb, vertexCount,
                                   rendering::PosColorVertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, indexCount);

  auto *vertices = reinterpret_cast<rendering::PosColorVertex *>(tvb.data);
  auto *indices = reinterpret_cast<uint16_t *>(tib.data);
  const uint32_t abgr = color.toABGR();
  uint16_t vertexIndex = 0;
  vertices[vertexIndex++] = {
      static_cast<float>(x) + static_cast<float>(width) * 0.5f,
      static_cast<float>(y) + static_cast<float>(height) * 0.5f, 0.0f, abgr};

  const auto appendCorner = [&](float cx, float cy, float startAngle) {
    for (int i = 0; i <= segments; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(segments);
      const float angle = startAngle + t * (kPi * 0.5f);
      vertices[vertexIndex++] = {cx + std::cos(angle) * radius,
                                 cy + std::sin(angle) * radius, 0.0f, abgr};
    }
  };

  const float fx = static_cast<float>(x);
  const float fy = static_cast<float>(y);
  const float fw = static_cast<float>(width);
  const float fh = static_cast<float>(height);
  appendCorner(fx + fw - radius, fy + radius, -kPi * 0.5f);
  appendCorner(fx + fw - radius, fy + fh - radius, 0.0f);
  appendCorner(fx + radius, fy + fh - radius, kPi * 0.5f);
  appendCorner(fx + radius, fy + radius, kPi);

  uint16_t index = 0;
  for (uint16_t i = 0; i < ringVertexCount; ++i) {
    indices[index++] = 0;
    indices[index++] = static_cast<uint16_t>(i + 1);
    indices[index++] = static_cast<uint16_t>((i + 1) % ringVertexCount + 1);
  }

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA |
                 BGFX_STATE_MSAA);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(rendering::ui_view, kSimpleProgram);
}

void submitShadowRect(const RenderContext &context, int x, int y, int width,
                      int height, float radius, int spread,
                      const Color &color) {
  if (width <= 0 || height <= 0 || spread <= 0 || color.a == 0) {
    return;
  }

  radius = std::clamp(radius, 0.0f,
                      static_cast<float>(std::min(width, height)) * 0.5f);
  const int shadowX = x - spread;
  const int shadowY = y - spread;
  const int shadowWidth = width + spread * 2;
  const int shadowHeight = height + spread * 2;

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  constexpr uint32_t kVertexCount = 4;
  constexpr uint32_t kIndexCount = 6;
  if (bgfx::getAvailTransientVertexBuffer(
          kVertexCount, rendering::PosTexCoord0Vertex::ms_decl) <
          kVertexCount ||
      bgfx::getAvailTransientIndexBuffer(kIndexCount) < kIndexCount) {
    return;
  }
  bgfx::allocTransientVertexBuffer(&tvb, kVertexCount,
                                   rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, kIndexCount);

  auto *vertices =
      reinterpret_cast<rendering::PosTexCoord0Vertex *>(tvb.data);
  auto *indices = reinterpret_cast<uint16_t *>(tib.data);
  vertices[0] = {static_cast<float>(shadowX), static_cast<float>(shadowY),
                 0.0f, 0.0f, 0.0f};
  vertices[1] = {static_cast<float>(shadowX + shadowWidth),
                 static_cast<float>(shadowY), 0.0f, 1.0f, 0.0f};
  vertices[2] = {static_cast<float>(shadowX + shadowWidth),
                 static_cast<float>(shadowY + shadowHeight), 0.0f, 1.0f,
                 1.0f};
  vertices[3] = {static_cast<float>(shadowX),
                 static_cast<float>(shadowY + shadowHeight), 0.0f, 0.0f,
                 1.0f};

  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 2;
  indices[4] = 3;
  indices[5] = 0;

  const float inv255 = 1.0f / 255.0f;
  const float shadowColor[4] = {static_cast<float>(color.r) * inv255,
                                static_cast<float>(color.g) * inv255,
                                static_cast<float>(color.b) * inv255,
                                static_cast<float>(color.a) * inv255};
  const float shadowParams[4] = {static_cast<float>(width),
                                 static_cast<float>(height), radius,
                                 static_cast<float>(spread)};
  bgfx::setUniform(
      rendering::UniformCache::getInstance().getVec4("u_shadowColor"),
      shadowColor);
  bgfx::setUniform(
      rendering::UniformCache::getInstance().getVec4("u_shadowParams"),
      shadowParams);
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kShadowProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_UI_SHADOW);
  bgfx::submit(rendering::ui_view, kShadowProgram);
}

void submitGradientRect(const RenderContext &context, int x, int y, int width,
                        int height, const Color &topColor,
                        const Color &bottomColor) {
  if (width <= 0 || height <= 0 || (topColor.a == 0 && bottomColor.a == 0)) {
    return;
  }

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  bgfx::allocTransientVertexBuffer(&tvb, 4, rendering::PosColorVertex::ms_decl);
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

uint64_t View::addTemporaryEventListener(TemporaryEventListener listener) {
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
  themedBackgroundColorProvider = nullptr;
  backgroundColor = color;
  hasBackground = true;
  hasGradientBackground = false;
  return this;
}

View *View::setThemedBackgroundColor(ThemeColorProvider provider) {
  themedBackgroundColorProvider = std::move(provider);
  if (themedBackgroundColorProvider) {
    backgroundColor = themedBackgroundColorProvider();
    hasBackground = true;
    hasGradientBackground = false;
  }
  return this;
}

View *View::setBackgroundGradient(const Color &topColor,
                                  const Color &bottomColor) {
  themedBackgroundColorProvider = nullptr;
  backgroundGradientTopColor = topColor;
  backgroundGradientBottomColor = bottomColor;
  hasBackground = true;
  hasGradientBackground = true;
  return this;
}

View *View::clearBackgroundColor() {
  themedBackgroundColorProvider = nullptr;
  hasBackground = false;
  hasGradientBackground = false;
  return this;
}

View *View::setCornerRadius(float radius) {
  cornerRadius = std::max(0.0f, radius);
  return this;
}

View *View::setShadow(const Color &color, int offsetX, int offsetY,
                      int spread) {
  themedShadowColorProvider = nullptr;
  shadowColor = color;
  shadowOffsetX = offsetX;
  shadowOffsetY = offsetY;
  shadowSpread = std::max(0, spread);
  hasShadow = color.a > 0 && shadowSpread > 0;
  return this;
}

View *View::setThemedShadow(ThemeColorProvider provider, int offsetX,
                            int offsetY, int spread) {
  themedShadowColorProvider = std::move(provider);
  shadowOffsetX = offsetX;
  shadowOffsetY = offsetY;
  shadowSpread = std::max(0, spread);
  if (themedShadowColorProvider) {
    shadowColor = themedShadowColorProvider();
    hasShadow = shadowColor.a > 0 && shadowSpread > 0;
  }
  return this;
}

View *View::clearShadow() {
  themedShadowColorProvider = nullptr;
  hasShadow = false;
  shadowSpread = 0;
  return this;
}

View *View::setBorderColor(const Color &color) {
  themedBorderColorProvider = nullptr;
  borderColor = color;
  hasBorder = true;
  return this;
}

View *View::setThemedBorderColor(ThemeColorProvider provider) {
  themedBorderColorProvider = std::move(provider);
  if (themedBorderColorProvider) {
    borderColor = themedBorderColorProvider();
    hasBorder = true;
  }
  return this;
}

View *View::clearBorderColor() {
  themedBorderColorProvider = nullptr;
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

void View::onThemeChanged() {
  if (themedBackgroundColorProvider) {
    backgroundColor = themedBackgroundColorProvider();
    hasBackground = true;
    hasGradientBackground = false;
  }
  if (themedBorderColorProvider) {
    borderColor = themedBorderColorProvider();
    hasBorder = true;
  }
  if (themedShadowColorProvider) {
    shadowColor = themedShadowColorProvider();
    hasShadow = shadowColor.a > 0 && shadowSpread > 0;
  }
}

void View::propagateThemeChange() {
  onThemeChanged();
  for (auto *child : children) {
    if (child != nullptr) {
      child->propagateThemeChange();
    }
  }
}

void View::renderBoxDecoration(RenderContext &context) const {
  if ((!hasBackground && (!hasBorder || borderWidth <= 0) && !hasShadow) ||
      getWidth() <= 0 || getHeight() <= 0) {
    return;
  }

  const int x = getX();
  const int y = getY();
  const int width = getWidth();
  const int height = getHeight();

  int inset = hasBorder ? borderWidth : 0;
  inset = std::min(inset, std::min(width / 2, height / 2));
  const bool rounded = cornerRadius > 0.5f;

  if (hasShadow) {
    RenderContext shadowContext;
    if (prepareShadowRenderContext(context, shadowContext, x, y, width, height,
                                   shadowSpread, shadowOffsetX,
                                   shadowOffsetY)) {
      submitShadowRect(shadowContext, x + shadowOffsetX, y + shadowOffsetY,
                       width, height, cornerRadius, shadowSpread, shadowColor);
    }
  }

  if (rounded && hasBorder && inset > 0 && hasBackground) {
    submitRoundedRect(context, x, y, width, height, cornerRadius, borderColor);

    const int backgroundX = x + inset;
    const int backgroundY = y + inset;
    const int backgroundWidth = width - inset * 2;
    const int backgroundHeight = height - inset * 2;
    const float backgroundRadius = std::max(0.0f, cornerRadius - inset);
    if (hasGradientBackground) {
      submitRoundedRect(context, backgroundX, backgroundY, backgroundWidth,
                        backgroundHeight, backgroundRadius,
                        backgroundGradientTopColor);
    } else {
      submitRoundedRect(context, backgroundX, backgroundY, backgroundWidth,
                        backgroundHeight, backgroundRadius, backgroundColor);
    }
    return;
  }

  if (rounded && hasBackground) {
    const Color fillColor =
        hasGradientBackground ? backgroundGradientTopColor : backgroundColor;
    submitRoundedRect(context, x, y, width, height, cornerRadius, fillColor);
    return;
  }

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

View *View::findViewByName(const std::string &targetName) {
  if (this->name == targetName) {
    return this;
  }
  for (auto *child : children) {
    View *found = child->findViewByName(targetName);
    if (found) {
      return found;
    }
  }
  return nullptr;
}
