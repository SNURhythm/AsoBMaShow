#pragma once

#include "SDL2/SDL_events.h"
#include "ScrollMomentum.h"
#include "View.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include <bx/math.h>
#include <string>
#include <vector>
#include <unordered_set>

template <typename T> class RecyclerView : public View {
private:
  inline void destroyAllViews() {
    std::unordered_set<View *> unique;
    for (auto &entry : viewEntries) {
      unique.insert(entry.first);
    }
    for (auto *view : recycledViewEntries) {
      unique.insert(view);
    }
    for (auto *view : unique) {
      delete view;
    }
    viewEntries.clear();
    recycledViewEntries.clear();
    idxToView.clear();
  }

private:
  void renderImpl(RenderContext &context) override {
    if (visibleItemsNeedLayout()) {
      updateVisibleItems();
    }
    if (!touchDragging) {
      float momentumDelta = 0.0f;
      if (touchMomentum.step(momentumDelta) && !scrollBy(momentumDelta)) {
        touchMomentum.stop();
      }
    }
    {
      ScissorScope scissor(context, this->getX(), this->getY(),
                           this->getWidth(), this->getHeight());
      for (const auto &entry : viewEntries) {
        entry.first->render(context);
      }
      renderScrollbar(context);
    }
  }

  inline bool handleEventsImpl(SDL_Event &event) override {
    switch (event.type) {
    case SDL_KEYDOWN: {
      bool changed = false;
      if (event.key.keysym.sym == SDLK_UP) {
        changed = true;
        bool isInitialSelection = selectedIndex == -1;
        int prevIndex = selectedIndex;
        if (selectedIndex > 0) {
          selectedIndex--;
        } else {
          selectedIndex = items.size() - 1;
        }
        if (!items.empty()) {
          if (onUnselected && !isInitialSelection) {
            onUnselected(items[prevIndex], prevIndex);
          }
          if (onSelected) {
            onSelected(items[selectedIndex], selectedIndex);
          }
        }

      } else if (event.key.keysym.sym == SDLK_DOWN) {
        changed = true;
        bool isInitialSelection = selectedIndex == -1;
        int prevIndex = selectedIndex;
        if (selectedIndex < items.size() - 1) {
          selectedIndex++;
        } else {
          selectedIndex = 0;
        }
        if (!items.empty()) {
          if (onUnselected && !isInitialSelection) {
            onUnselected(items[prevIndex], prevIndex);
          }
          if (onSelected) {
            onSelected(items[selectedIndex], selectedIndex);
          }
        }
      }
      // scroll to the selected item
      if (changed) {
        const float previousOffset = scrollOffset;
        int itemsSize =
            std::max(1, static_cast<int>(items.size())) * itemHeight;
        int selectedY = selectedIndex * itemHeight;
        if (selectedY < scrollOffset) {
          scrollOffset = selectedY;
        }
        if (selectedY > scrollOffset + this->getHeight() - itemHeight) {
          scrollOffset = selectedY - this->getHeight() + itemHeight;
        }
        clampScrollOffset();
        if (std::fabs(scrollOffset - previousOffset) > 0.001f) {
          revealScrollbar();
        }
        updateVisibleItems();
      }
      break;
    }
    case SDL_MOUSEWHEEL: {
      // check mouse position
      int x, y;
      SDL_GetMouseState(&x, &y);
      if (x < this->getX() || x > this->getX() + this->getWidth()) {
        return true;
      }
      if (y < this->getY() || y > this->getY() + this->getHeight()) {
        return true;
      }
      touchMomentum.stop();
      revealScrollbar();
      scrollBy(-event.wheel.y * 15.0f);
      break;
    }
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN: {
      if (touchDragging) {
        return true;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button != SDL_BUTTON_LEFT) {
        return true;
      }
      // ignore touch
      if (event.button.which == SDL_TOUCH_MOUSEID &&
          event.type == SDL_MOUSEBUTTONDOWN) {
        return true;
      }

      // ignore mouse up
      if (event.type == SDL_MOUSEBUTTONUP &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.which != SDL_TOUCH_MOUSEID) {
        return true;
      }

      int x, y;
      SDL_GetMouseState(&x, &y);
      x = static_cast<int>(x * rendering::widthScale);
      y = static_cast<int>(y * rendering::heightScale);
      int uiX = 0;
      int uiY = 0;
      rendering::screenToUi(x, y, uiX, uiY);
      if (uiX < this->getX() || uiX > this->getX() + this->getWidth()) {
        return true;
      }
      if (uiY < this->getY() || uiY > this->getY() + this->getHeight()) {
        return true;
      }
      touchMomentum.stop();
      int index = (uiY - this->getY() + scrollOffset) / itemHeight;
      if (index >= 0 && index < items.size()) {
        if (selectedIndex != -1 && onUnselected) {
          onUnselected(items[selectedIndex], selectedIndex);
        }
        selectedIndex = index;
        if (onSelected) {
          onSelected(items[selectedIndex], selectedIndex);
        }
      }
      break;
    }
    case SDL_FINGERDOWN: {
      // Get the normalized touch coordinates
      float normX = event.tfinger.x;
      float normY = event.tfinger.y;
      // Get the window size
      // Convert normalized coordinates to screen coordinates
      float touchX = 0.0f;
      float touchY = 0.0f;
      rendering::normalizedToUi(normX, normY, touchX, touchY);

      if (touchX < this->getX() || touchX > this->getX() + this->getWidth()) {
        return true;
      }
      if (touchY < this->getY() || touchY > this->getY() + this->getHeight()) {
        return true;
      }
      touchMomentum.stop();
      touchLastY = touchY;
      touchDragging = false;
      touchId = event.tfinger.fingerId;
      break;
    }
    case SDL_FINGERMOTION: {
      if (event.tfinger.fingerId != touchId) {
        return true;
      }
      // Get the normalized touch coordinates
      float normX = event.tfinger.x;
      float normY = event.tfinger.y;

      // Convert normalized coordinates to screen coordinates
      float touchX = 0.0f;
      float touchY = 0.0f;
      rendering::normalizedToUi(normX, normY, touchX, touchY);

      if (touchX < this->getX() || touchX > this->getX() + this->getWidth()) {
        return true;
      }
      if (touchY < this->getY() || touchY > this->getY() + this->getHeight()) {
        return true;
      }
      const float delta = touchLastY - touchY;
      revealScrollbar();
      scrollBy(delta);
      touchMomentum.recordDragDelta(delta);
      touchLastY = touchY;
      touchDragging = true;
      break;
    }
    case SDL_FINGERUP: {
      if (event.tfinger.fingerId != touchId) {
        return true;
      }
      const bool hadDrag = touchDragging;
      touchDragging = false;
      if (hadDrag) {
        touchMomentum.release();
      } else {
        touchMomentum.stop();
      }
      touchId = -1;
      break;
    }
    }
    return true;
  }

public:
  inline RecyclerView(std::function<bool(const T &, const T &)> itemComparator)
      : scrollOffset(0), itemHeight(100), topMargin(1), bottomMargin(1),
        itemComparator(itemComparator), View() {}
  inline ~RecyclerView() { destroyAllViews(); }

  // scroll offset in pixels
  float scrollOffset;

  // fixed height of all items in the list
  int itemHeight;
  int topMargin;    // Number of items to keep ready above the visible area
  int bottomMargin; // Number of items to keep ready below the visible area
  // Keep false for overlay scrollbars; set true when item layout should avoid
  // the scrollbar by shrinking content width.
  bool reserveScrollbarGutter = false;

  inline void setItems(std::vector<T> &&items) {
    this->items = std::move(items);
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void setItems(const std::vector<T> &items) {
    this->items = items;
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void push(T item) {
    items.push_back(item);
    updateVisibleItems();
  }

  inline void pop() {
    items.pop_back();
    updateVisibleItems();
  }

  inline void remove(int index) {
    items.erase(items.begin() + index);
    updateVisibleItems();
  }

  inline void clear() {
    items.clear();
    for (auto &entry : viewEntries) {
      recycleView(entry.first);
    }
    viewEntries.clear();
    idxToView.clear();
  }

  inline const T &get(int index) const { return items[index]; }

  inline int size() { return items.size(); }

  inline const std::vector<T> &getItems() const { return items; }

  // on bound to the view (delegate)
  std::function<void(View *, const T &, int, bool isSelected)> onBind;
  std::function<View *(const T &)> onCreateView;
  std::function<bool(const T &, const T &)> itemComparator;

  // on click
  std::function<void(const T &, int)> onSelected;
  std::function<void(const T &, int)> onUnselected;
  int selectedIndex = -1;

  inline View *getViewByIndex(int index) {
    if (idxToView.find(index) != idxToView.end()) {
      return idxToView[index];
    }
    return nullptr;
  }

private:
  std::vector<T> items;
  std::deque<std::pair<View *, T>> viewEntries; // Pair of view and item

  std::deque<View *> recycledViewEntries; // Pool of recycled views
  std::map<int, View *> idxToView;
  float touchLastY = 0;
  ScrollMomentum touchMomentum;
  SDL_FingerID touchId = -1;
  bool touchDragging = false;
  Uint64 scrollbarFadeInStartedAt = 0;
  Uint64 scrollbarLastActivityAt = 0;
  bool visibleItemsLayoutDirty = true;
  bool visibleItemsNeedRebind = false;
  int visibleItemsLayoutX = 0;
  int visibleItemsLayoutY = 0;
  int visibleItemsLayoutWidth = 0;
  int visibleItemsLayoutHeight = 0;
  int visibleItemsLayoutItemHeight = 0;
  float visibleItemsLayoutScrollOffset = 0.0f;

  static constexpr int kScrollbarContentInset = 14;
  static constexpr int kScrollbarWidth = 4;
  static constexpr int kScrollbarTrackWidth = 2;
  static constexpr int kScrollbarRightInset = 5;
  static constexpr int kScrollbarVerticalInset = 6;
  static constexpr int kScrollbarMinThumbHeight = 28;
  static constexpr Uint64 kScrollbarFadeInMs = 120;
  static constexpr Uint64 kScrollbarHoldMs = 650;
  static constexpr Uint64 kScrollbarFadeOutMs = 480;

  inline bool canScroll() const {
    return static_cast<int>(items.size()) * itemHeight > this->getHeight();
  }

  inline int visibleItemWidth() const {
    const int reservedWidth =
        reserveScrollbarGutter && canScroll() ? kScrollbarContentInset : 0;
    return std::max(0, this->getWidth() - reservedWidth);
  }

  inline bool visibleItemsNeedLayout() const {
    return visibleItemsLayoutDirty || visibleItemsLayoutX != this->getX() ||
           visibleItemsLayoutY != this->getY() ||
           visibleItemsLayoutWidth != visibleItemWidth() ||
           visibleItemsLayoutHeight != this->getHeight() ||
           visibleItemsLayoutItemHeight != itemHeight ||
           std::fabs(visibleItemsLayoutScrollOffset - scrollOffset) > 0.001f;
  }

  inline static float smoothStep(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
  }

  inline float currentScrollbarAlpha(Uint64 now) const {
    if (!canScroll() || scrollbarLastActivityAt == 0) {
      return 0.0f;
    }
    if (touchDragging) {
      return 1.0f;
    }

    const float fadeIn =
        scrollbarFadeInStartedAt == 0
            ? 1.0f
            : std::min(1.0f,
                       static_cast<float>(now - scrollbarFadeInStartedAt) /
                           static_cast<float>(kScrollbarFadeInMs));
    const Uint64 inactiveFor =
        now > scrollbarLastActivityAt ? now - scrollbarLastActivityAt : 0;
    float fadeOut = 1.0f;
    if (inactiveFor > kScrollbarHoldMs) {
      fadeOut =
          1.0f -
          std::min(1.0f, static_cast<float>(inactiveFor - kScrollbarHoldMs) /
                             static_cast<float>(kScrollbarFadeOutMs));
    }
    return smoothStep(std::min(fadeIn, fadeOut));
  }

  inline void revealScrollbar() {
    if (!canScroll()) {
      return;
    }
    const Uint64 now = SDL_GetTicks64();
    if (currentScrollbarAlpha(now) <= 0.01f) {
      scrollbarFadeInStartedAt = now;
    }
    scrollbarLastActivityAt = now;
  }

  inline static uint32_t scrollbarColor(uint8_t r, uint8_t g, uint8_t b,
                                        float alpha) {
    const auto a = static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::round(alpha * 255.0f)), 0, 255));
    return Color(r, g, b, a).toABGR();
  }

  inline void drawScrollbarRect(RenderContext &context, int x, int y, int width,
                                int height, uint32_t color) const {
    if (width <= 0 || height <= 0) {
      return;
    }
    if (bgfx::getAvailTransientVertexBuffer(
            4, rendering::PosColorVertex::ms_decl) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6) {
      return;
    }

    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    rendering::createRect(tvb, tib, x, y, width, height, color);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    bgfx::submit(rendering::ui_view, kProgram);
  }

  inline void renderScrollbar(RenderContext &context) const {
    const float alpha = currentScrollbarAlpha(SDL_GetTicks64());
    if (alpha <= 0.0f) {
      return;
    }

    const int itemsSize =
        std::max(1, static_cast<int>(items.size())) * itemHeight;
    const int trackHeight =
        std::max(0, this->getHeight() - (kScrollbarVerticalInset * 2));
    if (trackHeight <= 0) {
      return;
    }

    const int maxOffset = std::max(1, itemsSize - this->getHeight());
    const int thumbHeight =
        std::clamp(this->getHeight() * trackHeight / itemsSize,
                   kScrollbarMinThumbHeight, trackHeight);
    const float progress =
        std::clamp(scrollOffset / static_cast<float>(maxOffset), 0.0f, 1.0f);
    const int trackX = this->getX() + this->getWidth() - kScrollbarRightInset -
                       kScrollbarWidth;
    const int trackY = this->getY() + kScrollbarVerticalInset;
    const int thumbY =
        trackY +
        static_cast<int>(std::round((trackHeight - thumbHeight) * progress));

    const int trackCenterOffset = (kScrollbarWidth - kScrollbarTrackWidth) / 2;
    drawScrollbarRect(context, trackX + trackCenterOffset, trackY,
                      kScrollbarTrackWidth, trackHeight,
                      scrollbarColor(215, 226, 240, 0.12f * alpha));
    drawScrollbarRect(context, trackX, thumbY, kScrollbarWidth, thumbHeight,
                      scrollbarColor(226, 236, 247, 0.72f * alpha));
  }

  inline void clampScrollOffset() {
    const int itemsSize =
        std::max(1, static_cast<int>(items.size())) * itemHeight;
    const float maxOffset =
        std::max(0.0f, static_cast<float>(itemsSize - this->getHeight()));
    scrollOffset = std::clamp(scrollOffset, 0.0f, maxOffset);
  }

  inline bool scrollBy(float delta) {
    const float previousOffset = scrollOffset;
    scrollOffset += delta;
    clampScrollOffset();
    if (std::fabs(scrollOffset - previousOffset) <= 0.001f) {
      return false;
    }
    revealScrollbar();
    updateVisibleItems();
    return true;
  }

  inline void updateVisibleItems() {
    const int layoutX = this->getX();
    const int layoutY = this->getY();
    const int layoutWidth = visibleItemWidth();
    const int layoutHeight = this->getHeight();

    // Determine the range of visible items
    int startIndex = getStartIndex();
    int endIndex = getEndIndex();
    // if all items are visible
    if (items.size() * itemHeight < this->getHeight()) {
      startIndex = 0;
      endIndex = items.size() - 1;
      scrollOffset = 0;
    }

    // Temporary container for newly visible items
    std::deque<std::pair<View *, T>> newVisibleItems;
    idxToView.clear();
    LayoutBatchScope layoutBatch;
    // Iterate over the range of visible items
    for (int i = startIndex; i <= endIndex; ++i) {
      const T &item = items[i];
      View *view = nullptr;
      bool shouldBind = false;

      // Check if the item already has a corresponding view
      auto it = std::find_if(viewEntries.begin(), viewEntries.end(),
                             [&item, this](const std::pair<View *, T> &entry) {
                               return itemComparator(entry.second, item);
                             });

      if (it != viewEntries.end()) {
        // If the view is already visible, use it
        view = it->first;
        idxToView[i] = view;
        viewEntries.erase(it); // Remove from current visible items
        shouldBind = visibleItemsNeedRebind;
      } else {
        // Otherwise, get a recycled view or create a new one
        view = getViewForItem(item);
        idxToView[i] = view;
        shouldBind = true;
      }

      // Size before binding so row content is laid out against the recycler
      // width rather than the stale width of a created or recycled view.
      view->setPositionNoLayout(layoutX,
                                layoutY + (i * itemHeight) - scrollOffset,
                                YGPositionType::YGPositionTypeAbsolute);
      view->setWidth(layoutWidth)->setHeight(itemHeight)->applyYogaLayout();
      if (shouldBind && onBind) {
        onBind(view, item, i, selectedIndex == i);
      }

      newVisibleItems.emplace_back(view, item);
    }

    // Recycle any views that are no longer visible
    for (auto &entry : viewEntries) {
      recycleView(entry.first);
    }

    // Update the list of visible items
    viewEntries = std::move(newVisibleItems);
    visibleItemsLayoutDirty = false;
    visibleItemsNeedRebind = false;
    visibleItemsLayoutX = layoutX;
    visibleItemsLayoutY = layoutY;
    visibleItemsLayoutWidth = layoutWidth;
    visibleItemsLayoutHeight = layoutHeight;
    visibleItemsLayoutItemHeight = itemHeight;
    visibleItemsLayoutScrollOffset = scrollOffset;
  }

  inline int getStartIndex() {
    return std::max(0.0f, (scrollOffset / itemHeight) - topMargin);
  }

  inline int getEndIndex() {
    int viewportHeight =
        this->getHeight(); // Assuming RecyclerView has a getHeight method
    int lastPossibleIndex =
        (scrollOffset + viewportHeight) / itemHeight + bottomMargin;
    return std::min(static_cast<int>(items.size()) - 1, lastPossibleIndex);
  }

  inline View *getViewForItem(const T &item) {
    if (!recycledViewEntries.empty()) {
      View *view = recycledViewEntries.front();
      recycledViewEntries.pop_front();
      return view;
    } else {
      // Create a new view if no recycled view is available
      if (!onCreateView) {
        throw std::runtime_error("onCreateView is not set");
      }
      return onCreateView(item);
    }
  }

  inline void recycleView(View *view) { recycledViewEntries.push_back(view); }

protected:
  inline void onResize(int newWidth, int newHeight) override {
    View::onResize(newWidth, newHeight);
    updateVisibleItems();
  }
  void onMove(int newX, int newY) override {
    (void)newX;
    (void)newY;
    visibleItemsLayoutDirty = true;
  }
  void onLayout() override {
    View::onLayout();
    updateVisibleItems();
  }
};
