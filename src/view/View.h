#pragma once

#include <SDL2/SDL.h>
#include <yoga/Yoga.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/Color.h"
#include "bgfx/bgfx.h"
#include "bgfx/defines.h"

namespace ui_theme {
struct ShadowSpec;
}

enum class Edge {
  Left = YGEdgeLeft,
  Top = YGEdgeTop,
  Right = YGEdgeRight,
  Bottom = YGEdgeBottom,
  Start = YGEdgeStart,
  End = YGEdgeEnd,
  All = YGEdgeAll
};

enum class FlexDirection {
  Row = YGFlexDirectionRow,
  Column = YGFlexDirectionColumn,
  RowReverse = YGFlexDirectionRowReverse,
  ColumnReverse = YGFlexDirectionColumnReverse
};
struct Scissor {
  int x, y, width, height;
};
struct RenderContext {
  RenderContext() { scissorStack.reserve(16); }
  Scissor scissor = {0, 0, -1, -1};
  std::vector<Scissor> scissorStack;

  inline void pushScissor(int x, int y, int width, int height) {
    scissorStack.push_back(scissor);
    if (scissor.width < 0 || scissor.height < 0) {
      scissor = {x, y, width, height};
      return;
    }
    int left = std::max(scissor.x, x);
    int top = std::max(scissor.y, y);
    int right = std::min(scissor.x + scissor.width, x + width);
    int bottom = std::min(scissor.y + scissor.height, y + height);
    scissor = {left, top, std::max(0, right - left), std::max(0, bottom - top)};
  }

  inline void popScissor() {
    if (scissorStack.empty()) {
      return;
    }
    scissor = scissorStack.back();
    scissorStack.pop_back();
  }
};

struct ScissorScope {
  explicit ScissorScope(RenderContext &context, int x, int y, int width,
                        int height)
      : context(context) {
    context.pushScissor(x, y, width, height);
  }
  ~ScissorScope() { context.popScissor(); }

private:
  RenderContext &context;
};

class View {
public:
  using ThemeColorProvider = std::function<Color()>;

  struct LayoutBatchScope {
    LayoutBatchScope() { View::beginLayoutBatch(); }
    ~LayoutBatchScope() { View::endLayoutBatch(); }
  };

  inline View(int x, int y, int width, int height) : isVisible(true) {
    dbgColor = {static_cast<uint8_t>(rand() % 256),
                static_cast<uint8_t>(rand() % 256),
                static_cast<uint8_t>(rand() % 256), 64};
    node = YGNodeNew();
    YGNodeStyleSetPosition(node, YGEdgeLeft, x);
    YGNodeStyleSetPosition(node, YGEdgeTop, y);
    YGNodeStyleSetWidth(node, width);
    YGNodeStyleSetHeight(node, height);
    YGNodeSetContext(node, this);
    applyYogaLayout();
  }
  inline View() : isVisible(true) {
    dbgColor = {static_cast<uint8_t>(rand() % 256),
                static_cast<uint8_t>(rand() % 256),
                static_cast<uint8_t>(rand() % 256), 64};
    node = YGNodeNew();
    YGNodeSetContext(node, this);
    applyYogaLayout();
  }

  View(const View &) = delete;
  View &operator=(const View &) = delete;
  View(View &&) = delete;
  View &operator=(View &&) = delete;

  virtual ~View() {
    dirtyRoots.erase(this);
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
    if (node != nullptr) {
      YGNodeFree(node);
      node = nullptr;
    }
  }

  void render(RenderContext &context) {
    if (!isVisible)
      return;
    sortChildrenIfNeeded();
#if DEBUG
    if (drawBoundingBox) {
      float x = getX();
      float y = getY();
      float width = getWidth();
      float height = getHeight();
      bgfx::TransientVertexBuffer tvb{};
      bgfx::TransientIndexBuffer tib{};
      // Define the vertex layout
      bgfx::VertexLayout layout = rendering::PosColorVertex::ms_decl;

      bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
      bgfx::allocTransientIndexBuffer(&tib, 6);

      auto *vertices = (rendering::PosColorVertex *)tvb.data;
      auto *index = (uint16_t *)tib.data;

      uint32_t abgr = dbgColor.toABGR();
      vertices[0] = {x, y, 0.0f, abgr};
      vertices[1] = {x + width, y, 0.0f, abgr};
      vertices[2] = {x + width, y + height, 0.0f, abgr};
      vertices[3] = {x, y + height, 0.0f, abgr};

      // Set up indices for two triangles (quad)
      index[0] = 0;
      index[1] = 1;
      index[2] = 2;
      index[3] = 2;
      index[4] = 3;
      index[5] = 0;

      // Set up state (e.g., render state, texture, shaders)
      uint64_t state =
          BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
      bgfx::setState(state);

      // Set the vertex and index buffers
      bgfx::setVertexBuffer(0, &tvb);
      bgfx::setIndexBuffer(&tib);

      // Submit the draw call
      static const bgfx::ProgramHandle kSimpleProgram =
          rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
      bgfx::submit(rendering::ui_view, kSimpleProgram);
    }
#endif
    renderBoxDecoration(context);
    renderImpl(context);
    for (auto view : children) {
      view->render(context);
    }
  }
  bool handleEvents(SDL_Event &event) {
    if (!isVisible) {
      return true;
    }
    sortChildrenIfNeeded();
    // Let top-most children handle first.
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      if (!(*it)->handleEvents(event)) {
        return false;
      }
    }
    return handleEventsImpl(event);
  }

  using TemporaryEventListener = std::function<void(SDL_Event &)>;
  static uint64_t addTemporaryEventListener(TemporaryEventListener listener);
  static void removeTemporaryEventListener(uint64_t listenerId);
  static void dispatchTemporaryEventListeners(SDL_Event &event);
  static void deferAfterEvent(std::function<void()> callback);
  static void dispatchDeferredEventCallbacks();

  virtual inline void onLayout() {};

  inline void setSize(int newWidth, int newHeight) {
    const float width = YGNodeLayoutGetWidth(node);
    const float height = YGNodeLayoutGetHeight(node);
    const bool isResized =
        !std::isfinite(width) || !std::isfinite(height) ||
        std::abs(width - static_cast<float>(newWidth)) > 0.1f ||
        std::abs(height - static_cast<float>(newHeight)) > 0.1f;

    YGNodeStyleSetWidth(node, newWidth);
    YGNodeStyleSetHeight(node, newHeight);

    if (isResized || YGNodeIsDirty(node)) {
      requestLayout();
    }
  }

  inline void setVisible(bool visible) { isVisible = visible; }
  [[nodiscard]] inline bool getVisible() const { return isVisible; }
  inline void setZIndex(int zIndex) {
    this->zIndex = zIndex;
    if (parent != nullptr) {
      parent->markChildrenOrderDirty();
    }
  }
  [[nodiscard]] inline int getZIndex() const { return zIndex; }
  inline void setPosition(
      int newX, int newY,
      YGPositionType positionType = YGPositionType::YGPositionTypeRelative) {
    YGNodeStyleSetPositionType(node, positionType);
    YGNodeStyleSetPosition(node, YGEdgeLeft, newX);
    YGNodeStyleSetPosition(node, YGEdgeTop, newY);

    requestLayoutIfDirty();
  }
  // Use for absolute-positioned views to avoid full layout recalculation.
  inline void setPositionNoLayout(
      int newX, int newY,
      YGPositionType positionType = YGPositionType::YGPositionTypeAbsolute) {
    YGNodeStyleSetPositionType(node, positionType);
    YGNodeStyleSetPosition(node, YGEdgeLeft, newX);
    YGNodeStyleSetPosition(node, YGEdgeTop, newY);
    const int oldAbsX = absoluteX;
    const int oldAbsY = absoluteY;
    if (parent != nullptr) {
      absoluteX = parent->absoluteX + newX;
      absoluteY = parent->absoluteY + newY;
    } else {
      absoluteX = newX;
      absoluteY = newY;
    }
    const int dx = absoluteX - oldAbsX;
    const int dy = absoluteY - oldAbsY;
    updateChildrenAbsolute(dx, dy);
    onMove(newX, newY);
  }
  [[nodiscard]] inline int getX() const { return absoluteX; }
  [[nodiscard]] inline int getY() const { return absoluteY; }
  [[nodiscard]] inline int getWidth() const {
    return YGNodeLayoutGetWidth(node);
  }
  [[nodiscard]] inline int getHeight() const {
    return YGNodeLayoutGetHeight(node);
  }
  [[nodiscard]] inline int getContentX() const {
    return getX() + getLayoutInset(YGEdgeLeft);
  }
  [[nodiscard]] inline int getContentY() const {
    return getY() + getLayoutInset(YGEdgeTop);
  }
  [[nodiscard]] inline int getContentWidth() const {
    return std::max(0, getWidth() - getLayoutInset(YGEdgeLeft) -
                           getLayoutInset(YGEdgeRight));
  }
  [[nodiscard]] inline int getContentHeight() const {
    return std::max(0, getHeight() - getLayoutInset(YGEdgeTop) -
                           getLayoutInset(YGEdgeBottom));
  }

  virtual void onSelected() {}
  virtual void onUnselected() {}

  View *setWidth(float width);
  View *setWidthPercent(float widthPercent);
  View *setHeight(float height);
  View *setMinWidth(float minWidth);
  View *setMinHeight(float minHeight);
  View *setFlex(float flex);
  View *setFlexGrow(float flexGrow);
  View *setFlexBasis(float flexBasis);
  View *setFlexWrap(YGWrap flexWrap);
  View *setFlexShrink(float flexShrink);
  View *setMargin(Edge edge, float margin);
  View *setPadding(Edge edge, float padding);
  View *setPosition(Edge edge, float position);
  View *setPositionType(YGPositionType positionType);
  View *setAlignItems(YGAlign align);
  View *setAlignSelf(YGAlign align);
  View *setAlignContent(YGAlign align);
  View *setJustifyContent(YGJustify justify);
  View *setFlexDirection(FlexDirection direction);
  View *setGap(YGGutter gutter, float gap);
  View *setGap(float gap);
  View *setDirection(YGDirection direction);
  View *setDisplay(YGDisplay display);
  View *setBackgroundColor(const Color &color);
  View *setThemedBackgroundColor(ThemeColorProvider provider);
  View *setBackgroundGradient(const Color &topColor, const Color &bottomColor);
  View *clearBackgroundColor();
  View *setCornerRadius(float radius);
  [[nodiscard]] float getCornerRadius() const { return cornerRadius; }
  View *setRotationDegrees(float degrees) {
    rotationDegrees = std::isfinite(degrees) ? std::fmod(degrees, 360.0f)
                                              : 0.0f;
    return this;
  }
  [[nodiscard]] float getRotationDegrees() const { return rotationDegrees; }
  View *setShadow(const Color &color, int offsetX, int offsetY, int spread);
  View *setShadow(const Color &color, const ui_theme::ShadowSpec &shadow);
  View *setThemedShadow(ThemeColorProvider provider, int offsetX, int offsetY,
                        int spread);
  View *setThemedShadow(ThemeColorProvider provider,
                        const ui_theme::ShadowSpec &shadow);
  View *clearShadow();
  View *setBorderColor(const Color &color);
  View *setThemedBorderColor(ThemeColorProvider provider);
  View *clearBorderColor();
  View *setBorderWidth(int width);
  View *addView(View *view);
  View *insertViewBefore(View *view, const View *sibling);
  View *clearChildren();
  YGNodeRef getNode() const { return node; }
  // This collection may be z-sorted for render/event dispatch. It is not a
  // stable Yoga layout-order index; use sibling identity for layout insertion.
  std::vector<View *> &getChildren() { return children; }
  void setName(const std::string &name) { this->name = name; }
  const std::string &getName() const { return name; }
  View *findViewByName(const std::string &name);

  bool drawBoundingBox = false;
  void applyYogaLayout();
  void applyYogaLayoutFromRoot();
  virtual void propagateThemeChange();
  static void beginLayoutBatch() { ++layoutBatchDepth; }
  static void endLayoutBatch() {
    if (layoutBatchDepth == 0) {
      return;
    }
    --layoutBatchDepth;
    if (layoutBatchDepth == 0) {
      flushLayoutBatches();
    }
  }

protected:
  virtual void renderImpl(RenderContext &context) {};
  virtual inline bool handleEventsImpl(SDL_Event &event) { return true; };
  virtual void onThemeChanged();
  // onResize
  virtual void onResize(int newWidth, int newHeight) {}
  // onMove
  virtual void onMove(int newX, int newY) {}

private:
  View *insertViewAtLayoutIndex(View *view, std::size_t layoutIndex);
  void refreshInsertionOrderFromLayout();
  void renderBoxDecoration(RenderContext &context) const;
  [[nodiscard]] int getLayoutInset(YGEdge edge) const {
    return (hasBorder ? std::max(0, borderWidth) : 0) + getStoredPadding(edge);
  }
  [[nodiscard]] int getStoredPadding(YGEdge edge) const {
    switch (edge) {
    case YGEdgeLeft:
    case YGEdgeStart:
      return paddingLeft;
    case YGEdgeTop:
      return paddingTop;
    case YGEdgeRight:
    case YGEdgeEnd:
      return paddingRight;
    case YGEdgeBottom:
      return paddingBottom;
    default:
      return 0;
    }
  }
  void updateStoredPadding(Edge edge, float padding) {
    const int value =
        std::max(0, static_cast<int>(std::round(std::max(0.0f, padding))));
    switch (edge) {
    case Edge::Left:
    case Edge::Start:
      paddingLeft = value;
      break;
    case Edge::Top:
      paddingTop = value;
      break;
    case Edge::Right:
    case Edge::End:
      paddingRight = value;
      break;
    case Edge::Bottom:
      paddingBottom = value;
      break;
    case Edge::All:
      paddingLeft = value;
      paddingTop = value;
      paddingRight = value;
      paddingBottom = value;
      break;
    }
  }
  void syncYogaBorderWidth() {
    YGNodeStyleSetBorder(
        node, YGEdgeAll,
        hasBorder ? static_cast<float>(std::max(0, borderWidth)) : 0.0f);
    requestLayoutIfDirty();
  }
  void markLayoutDirty() {
    View *root = this;
    while (root->parent != nullptr) {
      root = root->parent;
    }
    dirtyRoots.insert(root);
  }
  static void flushLayoutBatches() {
    if (dirtyRoots.empty()) {
      return;
    }
    while (!dirtyRoots.empty()) {
      auto roots = std::move(dirtyRoots);
      dirtyRoots.clear();
      for (auto *root : roots) {
        if (root != nullptr) {
          root->applyYogaLayoutImmediate();
        }
      }
    }
  }
  void requestLayout() {
    if (layoutBatchDepth > 0 || layoutApplyDepth > 0) {
      markLayoutDirty();
      return;
    }
    View *root = this;
    while (root->parent != nullptr) {
      root = root->parent;
    }
    root->applyYogaLayoutImmediate();
  }
  void requestLayoutIfDirty() {
    if (YGNodeIsDirty(node)) {
      requestLayout();
    }
  }
  void applyYogaLayoutImmediate();

  void updateChildrenAbsolute(int dx, int dy) {
    if (dx == 0 && dy == 0) {
      return;
    }
    for (auto *child : children) {
      child->absoluteX += dx;
      child->absoluteY += dy;
      child->updateChildrenAbsolute(dx, dy);
      child->onMove(YGNodeLayoutGetLeft(child->node),
                    YGNodeLayoutGetTop(child->node));
    }
  }
  void markChildrenOrderDirty() { childrenOrderDirty = true; }
  void sortChildrenIfNeeded() {
    if (!childrenOrderDirty) {
      return;
    }
    std::sort(children.begin(), children.end(),
              [](const View *a, const View *b) {
                if (a->zIndex != b->zIndex) {
                  return a->zIndex < b->zIndex;
                }
                return a->insertionOrder < b->insertionOrder;
              });
    childrenOrderDirty = false;
  }
  struct TemporaryEventListenerEntry {
    uint64_t id = 0;
    TemporaryEventListener listener;
    bool active = false;
  };
  static void eraseInactiveTemporaryEventListeners();

  Color dbgColor;
  Color backgroundColor;
  Color backgroundGradientTopColor;
  Color backgroundGradientBottomColor;
  Color borderColor;
  Color shadowColor;
  ThemeColorProvider themedBackgroundColorProvider;
  ThemeColorProvider themedBorderColorProvider;
  ThemeColorProvider themedShadowColorProvider;
  int absoluteX = 0;
  int absoluteY = 0;
  bool isVisible; // Visibility of the view
  bool hasBackground = false;
  bool hasGradientBackground = false;
  bool hasBorder = false;
  bool hasShadow = false;
  int borderWidth = 0;
  int paddingLeft = 0;
  int paddingTop = 0;
  int paddingRight = 0;
  int paddingBottom = 0;
  float cornerRadius = 0.0f;
  float rotationDegrees = 0.0f;
  int shadowOffsetX = 0;
  int shadowOffsetY = 0;
  int shadowSpread = 0;
  float shadowRadiusInset = 0.0f;
  YGNodeRef node;
  View *parent = nullptr;

  std::vector<View *> children;
  bool childrenOrderDirty = false;
  int zIndex = 0;
  uint64_t insertionOrder = 0;
  inline static uint64_t nextInsertionOrder = 1;
  inline static int layoutBatchDepth = 0;
  inline static int layoutApplyDepth = 0;
  inline static std::unordered_set<View *> dirtyRoots;
  inline static std::vector<TemporaryEventListenerEntry>
      temporaryEventListeners;
  inline static std::vector<std::function<void()>> deferredEventCallbacks;
  inline static uint64_t nextTemporaryEventListenerId = 1;
  inline static bool dispatchingTemporaryEventListeners = false;
  std::string name;
};
