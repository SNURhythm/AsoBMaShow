#include "View.h"

#include "../rendering/UniformCache.h"
#include "UiTheme.h"

#include <cmath>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

bool prepareShadowRenderContext(const RenderContext &context,
                                RenderContext &shadowContext, int spread,
                                int offsetX, int offsetY) {
  shadowContext = context;
  if (context.scissor.width < 0 || context.scissor.height < 0) {
    return true;
  }

  if (context.scissor.width <= 0 || context.scissor.height <= 0) {
    return false;
  }

  const int bleedX = spread + std::abs(offsetX) + 2;
  (void)offsetY;
  // Vertical scrollers own the y clip; shadows only bleed horizontally.
  shadowContext.scissor.x -= bleedX;
  shadowContext.scissor.width += bleedX * 2;
  return shadowContext.scissor.width > 0 && shadowContext.scissor.height > 0;
}

float oneDrawablePixelInUi(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return 1.0f / scale;
}

float visibleUiBorderWidth(float width, float scale) {
  if (width <= 0.0f) {
    return 0.0f;
  }
  return std::max(width, oneDrawablePixelInUi(scale));
}

float visibleUiBorderInset(float width) {
  if (width <= 0.0f) {
    return 0.0f;
  }
  return std::max(width, std::max(oneDrawablePixelInUi(rendering::ui_scale_x),
                                  oneDrawablePixelInUi(rendering::ui_scale_y)));
}

std::optional<rendering::UiBatchScissor>
batchScissor(const RenderContext &context) {
  if (context.scissor.width < 0 || context.scissor.height < 0) {
    return std::nullopt;
  }
  return rendering::UiBatchScissor{.x = context.scissor.x,
                                   .y = context.scissor.y,
                                   .width = context.scissor.width,
                                   .height = context.scissor.height};
}

std::optional<std::array<float, 16>>
batchTransform(const RenderContext &context) {
  const float *transform = context.getTransformMatrix();
  if (transform == nullptr) {
    return std::nullopt;
  }
  std::array<float, 16> result;
  std::copy_n(transform, result.size(), result.begin());
  return result;
}

rendering::UiBatchState
batchState(const RenderContext &context, bgfx::ProgramHandle program,
           std::uint64_t state) {
  return {.program = program,
          .state = state,
          .scissor = batchScissor(context),
          .transform = batchTransform(context)};
}

void submitColoredRect(const RenderContext &context, float x, float y,
                       float width, float height, const Color &color) {
  if (width <= 0.0f || height <= 0.0f || color.a == 0) {
    return;
  }

  const uint32_t abgr = color.toABGR();
  const std::array vertices = {
      rendering::PosColorVertex{x, y, 0.0f, abgr},
      rendering::PosColorVertex{x + width, y, 0.0f, abgr},
      rendering::PosColorVertex{x + width, y + height, 0.0f, abgr},
      rendering::PosColorVertex{x, y + height, 0.0f, abgr}};
  constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(
      vertices, indices,
      batchState(context, kSimpleProgram,
                 BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA));
}

void submitRoundedRect(const RenderContext &context, float x, float y,
                       float width, float height, float radius,
                       const Color &color) {
  if (width <= 0.0f || height <= 0.0f || color.a == 0) {
    return;
  }

  radius = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
  if (radius <= 0.5f) {
    submitColoredRect(context, x, y, width, height, color);
    return;
  }

  const int segments =
      std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
  const uint16_t ringVertexCount = static_cast<uint16_t>((segments + 1) * 4);
  const uint16_t vertexCount = static_cast<uint16_t>(ringVertexCount + 1);
  const uint16_t indexCount = static_cast<uint16_t>(ringVertexCount * 3);

  std::array<rendering::PosColorVertex, 53> vertices;
  std::array<uint16_t, 156> indices;
  const uint32_t abgr = color.toABGR();
  uint16_t vertexIndex = 0;
  vertices[vertexIndex++] = {x + width * 0.5f, y + height * 0.5f, 0.0f,
                             abgr};

  const auto appendCorner = [&](float cx, float cy, float startAngle) {
    for (int i = 0; i <= segments; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(segments);
      const float angle = startAngle + t * (kPi * 0.5f);
      vertices[vertexIndex++] = {cx + std::cos(angle) * radius,
                                 cy + std::sin(angle) * radius, 0.0f, abgr};
    }
  };

  appendCorner(x + width - radius, y + radius, -kPi * 0.5f);
  appendCorner(x + width - radius, y + height - radius, 0.0f);
  appendCorner(x + radius, y + height - radius, kPi * 0.5f);
  appendCorner(x + radius, y + radius, kPi);

  uint16_t indexCountUsed = 0;
  for (uint16_t i = 0; i < ringVertexCount; ++i) {
    indices[indexCountUsed++] = 0;
    indices[indexCountUsed++] = static_cast<uint16_t>(i + 1);
    indices[indexCountUsed++] =
        static_cast<uint16_t>((i + 1) % ringVertexCount + 1);
  }
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(
      std::span(vertices).first(vertexCount),
      std::span(indices).first(indexCount),
      batchState(context, kSimpleProgram,
                 BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA |
                     BGFX_STATE_MSAA));
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

  constexpr uint32_t kVertexCount = 4;
  constexpr uint32_t kIndexCount = 6;
  const std::array vertices = {
      rendering::PosTexCoord0Vertex{static_cast<float>(shadowX),
                                     static_cast<float>(shadowY), 0.0f, 0.0f,
                                     0.0f},
      rendering::PosTexCoord0Vertex{static_cast<float>(shadowX + shadowWidth),
                                     static_cast<float>(shadowY), 0.0f, 1.0f,
                                     0.0f},
      rendering::PosTexCoord0Vertex{
          static_cast<float>(shadowX + shadowWidth),
          static_cast<float>(shadowY + shadowHeight), 0.0f, 1.0f, 1.0f},
      rendering::PosTexCoord0Vertex{static_cast<float>(shadowX),
                                     static_cast<float>(shadowY + shadowHeight),
                                     0.0f, 0.0f, 1.0f}};
  constexpr std::array<uint16_t, kIndexCount> indices = {0, 1, 2, 2, 3, 0};

  const float inv255 = 1.0f / 255.0f;
  const float shadowColor[4] = {static_cast<float>(color.r) * inv255,
                                static_cast<float>(color.g) * inv255,
                                static_cast<float>(color.b) * inv255,
                                static_cast<float>(color.a) * inv255};
  const float shadowParams[4] = {static_cast<float>(width),
                                 static_cast<float>(height), radius,
                                 static_cast<float>(spread)};
  static const bgfx::ProgramHandle kShadowProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_UI_SHADOW);
  auto state = batchState(context, kShadowProgram,
                          BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
  state.uniforms[0] = {
      .handle = rendering::UniformCache::getInstance().getVec4("u_shadowColor"),
      .value = {shadowColor[0], shadowColor[1], shadowColor[2], shadowColor[3]}};
  state.uniforms[1] = {
      .handle = rendering::UniformCache::getInstance().getVec4("u_shadowParams"),
      .value = {shadowParams[0], shadowParams[1], shadowParams[2], shadowParams[3]}};
  state.uniformCount = 2;
  context.appendUiTextured(vertices, indices, state);
}

void submitGradientRect(const RenderContext &context, float x, float y,
                        float width, float height, const Color &topColor,
                        const Color &bottomColor) {
  if (width <= 0.0f || height <= 0.0f ||
      (topColor.a == 0 && bottomColor.a == 0)) {
    return;
  }

  const uint32_t top = topColor.toABGR();
  const uint32_t bottom = bottomColor.toABGR();
  const std::array vertices = {
      rendering::PosColorVertex{x, y, 0.0f, top},
      rendering::PosColorVertex{x + width, y, 0.0f, top},
      rendering::PosColorVertex{x + width, y + height, 0.0f, bottom},
      rendering::PosColorVertex{x, y + height, 0.0f, bottom}};
  constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  context.appendUiColor(
      vertices, indices,
      batchState(context, kSimpleProgram,
                 BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA));
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

void View::deferAfterEvent(std::function<void()> callback) {
  if (callback) {
    deferredEventCallbacks.push_back(std::move(callback));
  }
}

void View::dispatchDeferredEventCallbacks() {
  if (deferredEventCallbacks.empty()) {
    return;
  }
  auto callbacks = std::move(deferredEventCallbacks);
  deferredEventCallbacks.clear();
  for (auto &callback : callbacks) {
    if (callback) {
      callback();
    }
  }
}

View *View::setWidth(float width) {
  YGNodeStyleSetWidth(node, width);
  requestLayoutIfDirty();
  return this;
}

View *View::setWidthPercent(float widthPercent) {
  YGNodeStyleSetWidthPercent(node, widthPercent);
  requestLayoutIfDirty();
  return this;
}

View *View::setHeight(float height) {
  YGNodeStyleSetHeight(node, height);
  requestLayoutIfDirty();
  return this;
}

View *View::setMinWidth(float minWidth) {
  YGNodeStyleSetMinWidth(node, minWidth);
  requestLayoutIfDirty();
  return this;
}

View *View::setMinHeight(float minHeight) {
  YGNodeStyleSetMinHeight(node, minHeight);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlex(float flex) {
  YGNodeStyleSetFlex(node, flex);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlexGrow(float flexGrow) {
  YGNodeStyleSetFlexGrow(node, flexGrow);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlexBasis(float flexBasis) {
  YGNodeStyleSetFlexBasis(node, flexBasis);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlexWrap(YGWrap flexWrap) {
  YGNodeStyleSetFlexWrap(node, flexWrap);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlexShrink(float flexShrink) {
  YGNodeStyleSetFlexShrink(node, flexShrink);
  requestLayoutIfDirty();
  return this;
}

View *View::setMargin(Edge edge, float margin) {
  YGNodeStyleSetMargin(node, static_cast<YGEdge>(edge), margin);
  requestLayoutIfDirty();
  return this;
}

View *View::setPadding(Edge edge, float padding) {
  YGNodeStyleSetPadding(node, static_cast<YGEdge>(edge), padding);
  updateStoredPadding(edge, padding);
  requestLayoutIfDirty();
  return this;
}

View *View::setPosition(Edge edge, float position) {
  YGNodeStyleSetPosition(node, static_cast<YGEdge>(edge), position);
  requestLayoutIfDirty();
  return this;
}

View *View::setPositionType(YGPositionType positionType) {
  YGNodeStyleSetPositionType(node, positionType);
  requestLayoutIfDirty();
  return this;
}

View *View::setAlignItems(YGAlign align) {
  YGNodeStyleSetAlignItems(node, align);
  requestLayoutIfDirty();
  return this;
}

View *View::setAlignSelf(YGAlign align) {
  YGNodeStyleSetAlignSelf(node, align);
  requestLayoutIfDirty();
  return this;
}

View *View::setAlignContent(YGAlign align) {
  YGNodeStyleSetAlignContent(node, align);
  requestLayoutIfDirty();
  return this;
}

View *View::setJustifyContent(YGJustify justify) {
  YGNodeStyleSetJustifyContent(node, justify);
  requestLayoutIfDirty();
  return this;
}

View *View::setFlexDirection(FlexDirection direction) {
  YGNodeStyleSetFlexDirection(node, static_cast<YGFlexDirection>(direction));
  requestLayoutIfDirty();
  return this;
}

View *View::setGap(YGGutter gutter, float gap) {
  YGNodeStyleSetGap(node, gutter, gap);
  requestLayoutIfDirty();
  return this;
}

View *View::setGap(float gap) {
  YGNodeStyleSetGap(node, YGGutterAll, gap);
  requestLayoutIfDirty();
  return this;
}

View *View::setDirection(YGDirection direction) {
  YGNodeStyleSetDirection(node, direction);
  requestLayoutIfDirty();
  return this;
}

View *View::setDisplay(YGDisplay display) {
  YGNodeStyleSetDisplay(node, display);
  requestLayoutIfDirty();
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
  shadowRadiusInset = 0.0f;
  hasShadow = color.a > 0 && shadowSpread > 0;
  return this;
}

View *View::setShadow(const Color &color, const ui_theme::ShadowSpec &shadow) {
  setShadow(color, shadow.offsetX, shadow.offsetY, shadow.spread);
  shadowRadiusInset = std::max(0.0f, shadow.radiusInset);
  return this;
}

View *View::setThemedShadow(ThemeColorProvider provider, int offsetX,
                            int offsetY, int spread) {
  themedShadowColorProvider = std::move(provider);
  shadowOffsetX = offsetX;
  shadowOffsetY = offsetY;
  shadowSpread = std::max(0, spread);
  shadowRadiusInset = 0.0f;
  if (themedShadowColorProvider) {
    shadowColor = themedShadowColorProvider();
    hasShadow = shadowColor.a > 0 && shadowSpread > 0;
  }
  return this;
}

View *View::setThemedShadow(ThemeColorProvider provider,
                            const ui_theme::ShadowSpec &shadow) {
  setThemedShadow(std::move(provider), shadow.offsetX, shadow.offsetY,
                  shadow.spread);
  shadowRadiusInset = std::max(0.0f, shadow.radiusInset);
  return this;
}

View *View::clearShadow() {
  themedShadowColorProvider = nullptr;
  hasShadow = false;
  shadowSpread = 0;
  shadowRadiusInset = 0.0f;
  return this;
}

View *View::setBorderColor(const Color &color) {
  themedBorderColorProvider = nullptr;
  borderColor = color;
  hasBorder = true;
  syncYogaBorderWidth();
  return this;
}

View *View::setThemedBorderColor(ThemeColorProvider provider) {
  themedBorderColorProvider = std::move(provider);
  if (themedBorderColorProvider) {
    borderColor = themedBorderColorProvider();
    hasBorder = true;
  }
  syncYogaBorderWidth();
  return this;
}

View *View::clearBorderColor() {
  themedBorderColorProvider = nullptr;
  hasBorder = false;
  syncYogaBorderWidth();
  return this;
}

View *View::setBorderWidth(int width) {
  borderWidth = std::max(0, width);
  syncYogaBorderWidth();
  return this;
}

View *View::addView(View *view) {
  return insertViewAtLayoutIndex(view, YGNodeGetChildCount(node));
}

View *View::insertViewBefore(View *view, const View *sibling) {
  if (sibling == nullptr || sibling->parent != this) {
    return this;
  }
  const std::size_t childCount = YGNodeGetChildCount(node);
  for (std::size_t index = 0; index < childCount; ++index) {
    if (YGNodeGetChild(node, index) == sibling->getNode()) {
      return insertViewAtLayoutIndex(view, index);
    }
  }
  return this;
}

View *View::insertViewAtLayoutIndex(View *view, std::size_t layoutIndex) {
  if (view == nullptr || view->parent != nullptr) {
    return this;
  }
  std::unique_ptr<View> pending(view);
  layoutIndex = std::min(layoutIndex,
                         static_cast<std::size_t>(YGNodeGetChildCount(node)));
  children.push_back(view);
  pending.release();
  YGNodeInsertChild(node, view->getNode(), layoutIndex);
  view->parent = this;
  refreshInsertionOrderFromLayout();
  childrenOrderDirty = true;
  applyYogaLayout();
  return this;
}

void View::refreshInsertionOrderFromLayout() {
  const std::size_t childCount = YGNodeGetChildCount(node);
  for (std::size_t index = 0; index < childCount; ++index) {
    auto *child = static_cast<View *>(
        YGNodeGetContext(YGNodeGetChild(node, index)));
    if (child != nullptr) {
      child->insertionOrder = nextInsertionOrder++;
    }
  }
}

View *View::clearChildren() {
  for (auto *view : children) {
    dirtyRoots.erase(view);
    if (node != nullptr && view != nullptr && view->node != nullptr) {
      YGNodeRemoveChild(node, view->node);
    }
    if (view != nullptr) {
      view->parent = nullptr;
      delete view;
    }
  }
  children.clear();
  childrenOrderDirty = false;
  applyYogaLayout();
  return this;
}

void View::applyYogaLayout() { requestLayout(); }

void View::applyYogaLayoutFromRoot() { requestLayout(); }

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

  int layoutInset = hasBorder ? borderWidth : 0;
  layoutInset = std::min(layoutInset, std::min(width / 2, height / 2));
  const float borderWidthX =
      hasBorder ? std::min(visibleUiBorderWidth(static_cast<float>(layoutInset),
                                                rendering::ui_scale_x),
                           static_cast<float>(width) * 0.5f)
                : 0.0f;
  const float borderWidthY =
      hasBorder ? std::min(visibleUiBorderWidth(static_cast<float>(layoutInset),
                                                rendering::ui_scale_y),
                           static_cast<float>(height) * 0.5f)
                : 0.0f;
  const float borderInset =
      hasBorder
          ? std::min(visibleUiBorderInset(static_cast<float>(layoutInset)),
                     static_cast<float>(std::min(width, height)) * 0.5f)
          : 0.0f;
  const bool rounded = cornerRadius > 0.5f;

  if (hasShadow) {
    RenderContext shadowContext;
    if (prepareShadowRenderContext(context, shadowContext, shadowSpread,
                                   shadowOffsetX, shadowOffsetY)) {
      const float shadowRadius =
          std::max(0.0f, cornerRadius - shadowRadiusInset);
      submitShadowRect(shadowContext, x + shadowOffsetX, y + shadowOffsetY,
                       width, height, shadowRadius, shadowSpread, shadowColor);
    }
  }

  if (rounded && hasBorder && layoutInset > 0 && hasBackground) {
    submitRoundedRect(context, x, y, width, height, cornerRadius, borderColor);

    const float backgroundX = static_cast<float>(x) + borderInset;
    const float backgroundY = static_cast<float>(y) + borderInset;
    const float backgroundWidth =
        static_cast<float>(width) - borderInset * 2.0f;
    const float backgroundHeight =
        static_cast<float>(height) - borderInset * 2.0f;
    const float backgroundRadius = std::max(0.0f, cornerRadius - borderInset);
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

  if (hasBorder && layoutInset > 0) {
    submitColoredRect(context, x, y, width, borderWidthY, borderColor);
    submitColoredRect(context, x, static_cast<float>(y + height) - borderWidthY,
                      width, borderWidthY, borderColor);

    const float middleY = static_cast<float>(y) + borderWidthY;
    const float middleHeight = static_cast<float>(height) - borderWidthY * 2.0f;
    if (middleHeight > 0.0f) {
      submitColoredRect(context, x, middleY, borderWidthX, middleHeight,
                        borderColor);
      submitColoredRect(context, static_cast<float>(x + width) - borderWidthX,
                        middleY, borderWidthX, middleHeight, borderColor);
    }
  }

  if (hasBackground) {
    const float backgroundX = static_cast<float>(x) + borderWidthX;
    const float backgroundY = static_cast<float>(y) + borderWidthY;
    const float backgroundWidth =
        static_cast<float>(width) - borderWidthX * 2.0f;
    const float backgroundHeight =
        static_cast<float>(height) - borderWidthY * 2.0f;
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
  const bool outermostLayout = layoutApplyDepth == 0;
  ++layoutApplyDepth;

  auto prevX = absoluteX;
  auto prevY = absoluteY;
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
  if (prevX != absoluteX || prevY != absoluteY) {
    onMove(YGNodeLayoutGetLeft(node), YGNodeLayoutGetTop(node));
  }
  auto newWidth = YGNodeLayoutGetWidth(node);
  auto newHeight = YGNodeLayoutGetHeight(node);
  if (prevWidth != newWidth || prevHeight != newHeight) {
    onResize(newWidth, newHeight);
  }

  --layoutApplyDepth;
  if (outermostLayout && layoutBatchDepth == 0) {
    flushLayoutBatches();
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
