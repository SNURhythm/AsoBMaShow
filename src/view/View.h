#pragma once

#include <SDL2/SDL.h>
#include <yoga/Yoga.h>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/Color.h"
#include "bgfx/bgfx.h"
#include "bgfx/defines.h"
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

  virtual ~View() {
    if (node != nullptr) {
      YGNodeFree(node);
      node = nullptr;
    }
    for (auto view : children) {
      delete view;
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
      uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                       BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
      bgfx::setState(state);

      // Set the vertex and index buffers
      bgfx::setVertexBuffer(0, &tvb);
      bgfx::setIndexBuffer(&tib);

      // Submit the draw call
      bgfx::submit(
          rendering::ui_view,
          rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE));
    }
#endif
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

  virtual inline void onLayout() {};

  inline void setSize(int newWidth, int newHeight) {
    auto width = YGNodeLayoutGetWidth(node);
    auto height = YGNodeLayoutGetHeight(node);
    bool isResized = width != newWidth || height != newHeight;

    width = newWidth;
    height = newHeight;
    if (isResized) {
      SDL_Log("View::setSize: %d, %d", newWidth, newHeight);
      onResize(newWidth, newHeight);
      YGNodeStyleSetWidth(node, width);
      YGNodeStyleSetHeight(node, height);
      applyYogaLayout();
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

    applyYogaLayout();
    onMove(newX, newY);
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

  virtual void onSelected() {}
  virtual void onUnselected() {}

  View *setWidth(float width);
  View *setHeight(float height);
  View *setFlex(float flex);
  View *setFlexGrow(float flexGrow);
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
  View *addView(View *view);
  YGNodeRef getNode() const { return node; }
  std::vector<View *> &getChildren() { return children; }
  void setName(const std::string& name) { this->name = name; }
  const std::string& getName() const { return name; }
  View* findViewByName(const std::string& name);

  bool drawBoundingBox = false;
  void applyYogaLayout();
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
  // onResize
  virtual void onResize(int newWidth, int newHeight) {}
  // onMove
  virtual void onMove(int newX, int newY) {}

private:
  void markLayoutDirty() {
    View *root = this;
    while (root->parent != nullptr) {
      root = root->parent;
    }
    dirtyRoots.insert(root);
  }
  static void flushLayoutBatches() {
    for (auto *root : dirtyRoots) {
      root->applyYogaLayoutImmediate();
    }
    SDL_Log("flushLayoutBatches: %d", dirtyRoots.size());
    dirtyRoots.clear();
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

  Color dbgColor;
  int absoluteX;
  int absoluteY;
  bool isVisible; // Visibility of the view
  YGNodeRef node;
  View *parent = nullptr;

  std::vector<View *> children;
  bool childrenOrderDirty = false;
  int zIndex = 0;
  uint64_t insertionOrder = 0;
  inline static uint64_t nextInsertionOrder = 1;
  inline static int layoutBatchDepth = 0;
  inline static std::unordered_set<View *> dirtyRoots;
  std::string name;
};