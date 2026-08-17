#pragma once

#include "SDL2/SDL_events.h"
#include "../input/SDLPointerEvent.h"
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
      ScissorScope scissor(context, this->getContentX(), this->getContentY(),
                           this->getContentWidth(),
                           this->getContentHeight());
      for (const auto &entry : viewEntries) {
        entry.first->render(context);
      }
      renderScrollbar(context);
    }
  }

  inline bool handleEventsImpl(SDL_Event &event) override {
    if (sdl_pointer_event::isMouseSynthesizedTouch(event)) {
      return true;
    }
    if (shouldForwardEventToVisibleItems(event)) {
      for (auto it = viewEntries.rbegin(); it != viewEntries.rend(); ++it) {
        if (it->first != nullptr && !it->first->handleEvents(event)) {
          return false;
        }
      }
    }

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
          selectedIndex = itemCount() - 1;
        }
        if (itemCount() > 0) {
          if (onUnselected && !isInitialSelection) {
            onUnselected(itemAt(prevIndex), prevIndex);
          }
          if (onSelected) {
            onSelected(itemAt(selectedIndex), selectedIndex);
          }
        }

      } else if (event.key.keysym.sym == SDLK_DOWN) {
        changed = true;
        bool isInitialSelection = selectedIndex == -1;
        int prevIndex = selectedIndex;
        if (selectedIndex < itemCount() - 1) {
          selectedIndex++;
        } else {
          selectedIndex = 0;
        }
        if (itemCount() > 0) {
          if (onUnselected && !isInitialSelection) {
            onUnselected(itemAt(prevIndex), prevIndex);
          }
          if (onSelected) {
            onSelected(itemAt(selectedIndex), selectedIndex);
          }
        }
      }
      // scroll to the selected item
      if (changed) {
        const float previousOffset = scrollOffset;
        int itemsSize = std::max(1, itemCount()) * itemHeight;
        int selectedY = selectedIndex * itemHeight;
        if (selectedY < scrollOffset) {
          scrollOffset = selectedY;
        }
        if (selectedY >
            scrollOffset + this->getContentHeight() - itemHeight) {
          scrollOffset = selectedY - this->getContentHeight() + itemHeight;
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
      x = static_cast<int>(x * rendering::widthScale);
      y = static_cast<int>(y * rendering::heightScale);
      int uiX = 0;
      int uiY = 0;
      rendering::screenToUi(x, y, uiX, uiY);
      if (uiX < this->getContentX() ||
          uiX > this->getContentX() + this->getContentWidth()) {
        return true;
      }
      if (uiY < this->getContentY() ||
          uiY > this->getContentY() + this->getContentHeight()) {
        return true;
      }
      touchMomentum.stop();
      revealScrollbar();
      scrollBy(sdl_pointer_event::verticalWheelScrollDelta(event.wheel,
                                                           15.0F));
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
      // Touch selection is handled by FINGERDOWN/FINGERUP so release cannot
      // select a row unless this recycler accepted the matching press.
      if (event.button.which == SDL_TOUCH_MOUSEID) {
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
      if (!isInsideContent(uiX, uiY)) {
        return true;
      }
      touchMomentum.stop();
      selectIndex(indexAtUiY(uiY));
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

      if (!isInsideContent(touchX, touchY)) {
        return true;
      }
      touchMomentum.stop();
      touchLastY = touchY;
      touchDragging = false;
      touchId = event.tfinger.fingerId;
      touchPressIndex = indexAtUiY(touchY);
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

      if (!isInsideContent(touchX, touchY)) {
        touchDragging = true;
        touchPressIndex = -1;
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
        float touchX = 0.0f;
        float touchY = 0.0f;
        rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, touchX,
                                  touchY);
        const int releaseIndex = indexAtUiY(touchY);
        if (touchPressIndex >= 0 && touchPressIndex == releaseIndex &&
            isInsideContent(touchX, touchY)) {
          selectIndex(releaseIndex);
        }
      }
      touchId = -1;
      touchPressIndex = -1;
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

  [[nodiscard]] inline int getVisibleItemWidth() const {
    return visibleItemWidth();
  }

  inline void setItems(std::vector<T> &&items) {
    itemProvider = nullptr;
    externalItemCount = 0;
    this->items = std::move(items);
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void setItems(const std::vector<T> &items) {
    itemProvider = nullptr;
    externalItemCount = 0;
    this->items = items;
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void setItemProvider(int count,
                              std::function<const T &(int)> provider) {
    items.clear();
    externalItemCount = std::max(0, count);
    itemProvider = std::move(provider);
    selectedIndex = -1;
    scrollOffset = 0;
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  // Replace an externally owned provider after an append without moving the
  // viewport back to the first row.
  inline void updateItemProvider(int count,
                                 std::function<const T &(int)> provider) {
    items.clear();
    externalItemCount = std::max(0, count);
    itemProvider = std::move(provider);
    if (selectedIndex >= externalItemCount) {
      selectedIndex = -1;
    }
    clampScrollOffset();
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void push(T item) {
    itemProvider = nullptr;
    externalItemCount = 0;
    items.push_back(item);
    updateVisibleItems();
  }

  inline void pop() {
    itemProvider = nullptr;
    externalItemCount = 0;
    items.pop_back();
    updateVisibleItems();
  }

  inline void remove(int index) {
    itemProvider = nullptr;
    externalItemCount = 0;
    items.erase(items.begin() + index);
    updateVisibleItems();
  }

  inline void clear() {
    items.clear();
    itemProvider = nullptr;
    externalItemCount = 0;
    for (auto &entry : viewEntries) {
      recycleView(entry.first);
    }
    viewEntries.clear();
    idxToView.clear();
  }

  inline const T &get(int index) const { return itemAt(index); }

  inline int size() const { return itemCount(); }

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

  inline void rebindVisibleItems() {
    visibleItemsNeedRebind = true;
    updateVisibleItems();
  }

  inline void propagateThemeChange() override {
    View::propagateThemeChange();
    std::unordered_set<View *> themedViews;
    for (auto &entry : viewEntries) {
      themedViews.insert(entry.first);
    }
    for (auto *view : recycledViewEntries) {
      themedViews.insert(view);
    }
    for (auto *view : themedViews) {
      if (view != nullptr) {
        view->propagateThemeChange();
      }
    }
  }

private:
  std::vector<T> items;
  int externalItemCount = 0;
  std::function<const T &(int)> itemProvider;
  std::deque<std::pair<View *, T>> viewEntries; // Pair of view and item

  std::deque<View *> recycledViewEntries; // Pool of recycled views
  std::map<int, View *> idxToView;
  float touchLastY = 0;
  ScrollMomentum touchMomentum;
  SDL_FingerID touchId = -1;
  int touchPressIndex = -1;
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
    return itemCount() * itemHeight > this->getContentHeight();
  }

  inline int itemCount() const {
    return itemProvider ? externalItemCount : static_cast<int>(items.size());
  }

  inline const T &itemAt(int index) const {
    return itemProvider ? itemProvider(index) : items[index];
  }

  inline bool isInsideContent(float uiX, float uiY) const {
    return uiX >= this->getContentX() &&
           uiX <= this->getContentX() + this->getContentWidth() &&
           uiY >= this->getContentY() &&
           uiY <= this->getContentY() + this->getContentHeight();
  }

  inline int indexAtUiY(float uiY) const {
    return static_cast<int>((uiY - this->getContentY() + scrollOffset) /
                            itemHeight);
  }

  inline void selectIndex(int index) {
    if (index < 0 || index >= itemCount()) {
      return;
    }
    if (selectedIndex != -1 && onUnselected) {
      onUnselected(itemAt(selectedIndex), selectedIndex);
    }
    selectedIndex = index;
    if (onSelected) {
      onSelected(itemAt(selectedIndex), selectedIndex);
    }
  }

  inline int visibleItemWidth() const {
    const int reservedWidth =
        reserveScrollbarGutter && canScroll() ? kScrollbarContentInset : 0;
    return std::max(0, this->getContentWidth() - reservedWidth);
  }

  inline bool visibleItemsNeedLayout() const {
    return visibleItemsLayoutDirty ||
           visibleItemsLayoutX != this->getContentX() ||
           visibleItemsLayoutY != this->getContentY() ||
           visibleItemsLayoutWidth != visibleItemWidth() ||
           visibleItemsLayoutHeight != this->getContentHeight() ||
           visibleItemsLayoutItemHeight != itemHeight ||
           std::fabs(visibleItemsLayoutScrollOffset - scrollOffset) > 0.001f;
  }

  inline static bool shouldForwardEventToVisibleItems(const SDL_Event &event) {
    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
      return true;
    default:
      return false;
    }
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
    const std::array vertices = {
        rendering::PosColorVertex{static_cast<float>(x), static_cast<float>(y),
                                  0.0f, color},
        rendering::PosColorVertex{static_cast<float>(x + width),
                                  static_cast<float>(y), 0.0f, color},
        rendering::PosColorVertex{static_cast<float>(x + width),
                                  static_cast<float>(y + height), 0.0f, color},
        rendering::PosColorVertex{static_cast<float>(x),
                                  static_cast<float>(y + height), 0.0f, color}};
    constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    context.appendUiColor(
        vertices, indices,
        context.makeUiBatchState(
            kProgram, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA));
  }

  inline void renderScrollbar(RenderContext &context) const {
    const float alpha = currentScrollbarAlpha(SDL_GetTicks64());
    if (alpha <= 0.0f) {
      return;
    }

    const int itemsSize = std::max(1, itemCount()) * itemHeight;
    const int trackHeight =
        std::max(0, this->getContentHeight() - (kScrollbarVerticalInset * 2));
    if (trackHeight <= 0) {
      return;
    }

    const int maxOffset = std::max(1, itemsSize - this->getContentHeight());
    const int thumbHeight =
        std::clamp(this->getContentHeight() * trackHeight / itemsSize,
                   kScrollbarMinThumbHeight, trackHeight);
    const float progress =
        std::clamp(scrollOffset / static_cast<float>(maxOffset), 0.0f, 1.0f);
    const int trackX = this->getContentX() + this->getContentWidth() -
                       kScrollbarRightInset - kScrollbarWidth;
    const int trackY = this->getContentY() + kScrollbarVerticalInset;
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
    const int itemsSize = std::max(1, itemCount()) * itemHeight;
    const float maxOffset =
        std::max(0.0f,
                 static_cast<float>(itemsSize - this->getContentHeight()));
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
    const int layoutX = this->getContentX();
    const int layoutY = this->getContentY();
    const int layoutWidth = visibleItemWidth();
    const int layoutHeight = this->getContentHeight();

    // Determine the range of visible items
    int startIndex = getStartIndex();
    int endIndex = getEndIndex();
    // if all items are visible
    if (itemCount() * itemHeight < this->getContentHeight()) {
      startIndex = 0;
      endIndex = itemCount() - 1;
      scrollOffset = 0;
    }

    // Temporary container for newly visible items
    std::deque<std::pair<View *, T>> newVisibleItems;
    std::vector<std::pair<View *, int>> pendingBindings;
    idxToView.clear();
    {
      LayoutBatchScope sizingBatch;
      // Iterate over the range of visible items
      for (int i = startIndex; i <= endIndex; ++i) {
        const T &item = itemAt(i);
        View *view = nullptr;
        bool shouldBind = false;

        // Check if the item already has a corresponding view
        auto it =
            std::find_if(viewEntries.begin(), viewEntries.end(),
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

        view->setPositionNoLayout(layoutX,
                                  layoutY + (i * itemHeight) - scrollOffset,
                                  YGPositionType::YGPositionTypeAbsolute);
        view->setWidth(layoutWidth)->setHeight(itemHeight)->applyYogaLayout();
        if (shouldBind && onBind) {
          pendingBindings.emplace_back(view, i);
        }

        newVisibleItems.emplace_back(view, item);
      }
    }

    // Bind only after the sizing batch has flushed so responsive row content
    // observes the current recycler width instead of stale recycled geometry.
    {
      LayoutBatchScope bindingBatch;
      for (const auto &[view, index] : pendingBindings) {
        onBind(view, itemAt(index), index, selectedIndex == index);
      }
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
    int viewportHeight = this->getContentHeight();
    int lastPossibleIndex =
        (scrollOffset + viewportHeight) / itemHeight + bottomMargin;
    return std::min(itemCount() - 1, lastPossibleIndex);
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
