#pragma once

#include "SDL2/SDL_events.h"
#include "View.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include <bx/math.h>
#include <string>
#include <vector>
#include <algorithm>
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
    if (!touchDragging && touchDragged) {
      touchScrollSpeed *= 0.98;
      if (touchScrollSpeedReal > 0.01f || touchScrollSpeedReal < -0.01f) {
        scrollOffset += touchScrollSpeedReal;
        touchScrollSpeedReal *= 0.95;
        int itemsSize =
            std::max(1, static_cast<int>(items.size())) * itemHeight;
        if (scrollOffset < 0) {
          scrollOffset = 0;
        }
        if (scrollOffset > itemsSize - this->getHeight()) {
          scrollOffset = itemsSize - this->getHeight();
        }
        updateVisibleItems();
      }
    }
    // clip the rendering area

    ScissorScope scissor(context, this->getX(), this->getY(), this->getWidth(),
                         this->getHeight());
    for (const auto &entry : viewEntries) {
      entry.first->render(context);
    }
    if (items.size() * itemHeight < this->getHeight()) {
      return;
    }
    rendering::PosColorVertex vertices[] = {
        {-0.5f, -0.5f, 0.0f, 0xffffffff}, // Bottom-left
        {0.5f, -0.5f, 0.0f, 0xffffffff},  // Bottom-right
        {0.5f, 0.5f, 0.0f, 0xffffffff},   // Top-right
        {-0.5f, 0.5f, 0.0f, 0xffffffff}   // Top-left
    };
    rendering::PosColorVertex thumbVertices[] = {
        {-0.5f, -0.5f, 0.0f, 0xFF3333FF}, // Bottom-left
        {0.5f, -0.5f, 0.0f, 0xFF3333FF},  // Bottom-right
        {0.5f, 0.5f, 0.0f, 0xFF3333FF},   // Top-right
        {-0.5f, 0.5f, 0.0f, 0xFF3333FF}   // Top-left
    };
    uint16_t indices[] = {0, 1, 2, 2, 3, 0};

    // Create vertex and index buffers
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientVertexBuffer thumbVbh;
    bgfx::TransientIndexBuffer ibh;
    if (bgfx::getAvailTransientVertexBuffer(
            4, rendering::PosColorVertex::ms_decl) < 4 ||
        bgfx::getAvailTransientVertexBuffer(
            4, rendering::PosColorVertex::ms_decl) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6) {
      return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, 4,
                                     rendering::PosColorVertex::ms_decl);
    bgfx::allocTransientVertexBuffer(&thumbVbh, 4,
                                     rendering::PosColorVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&ibh, 6);

    // Copy data to the vertex buffer
    bx::memCopy(tvb.data, vertices, sizeof(vertices));
    bx::memCopy(thumbVbh.data, thumbVertices, sizeof(thumbVertices));
    bx::memCopy(ibh.data, indices, sizeof(indices));

    // scroll bar area
    // scale
    float mtx[16];
    bx::mtxSRT(mtx, 10.0f, this->getHeight(), 1.0f, 0.0f, 0.0f, 0.0f,
               this->getX() + this->getWidth() - 5,
               this->getY() + this->getHeight() / 2, 0.0f);
    bgfx::setTransform(mtx);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&ibh);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    bgfx::submit(rendering::ui_view, kProgram);

    // // scroll bar thumb
    int itemsSize = std::max(1, static_cast<int>(items.size())) * itemHeight;
    int thumbHeight = this->getHeight() * this->getHeight() / itemsSize;
    int thumbY = this->getY() + scrollOffset * this->getHeight() / itemsSize +
                 thumbHeight / 2;
    bx::mtxSRT(mtx, 10.0f, thumbHeight, 1.0f, 0.0f, 0.0f, 0.0f,
               this->getX() + this->getWidth() - 5, thumbY, 0.0f);
    bgfx::setTransform(mtx);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setVertexBuffer(0, &thumbVbh);
    bgfx::setIndexBuffer(&ibh);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    bgfx::submit(rendering::ui_view, kProgram);

    // int itemsSize = std::max(6, static_cast<int>(items.size())) * itemHeight;
    // int thumbHeight = this->getHeight() * this->getHeight() / itemsSize;
    // int thumbY = this->getY() + scrollOffset * this->getHeight() / itemsSize;
    // SDL_Rect scrollBarThumb = {this->getX() + this->getWidth() - 10, thumbY,
    // 10,
    //                            thumbHeight};
    // SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);
    // SDL_RenderFillRect(renderer, &scrollBarThumb);

    // SDL_RenderSetClipRect(renderer, nullptr);
    // bgfx::setScissor();
    // bgfx::setScissor(this->getX() + 100 + this->getWidth() - 10,
    // this->getY(),
    //                  10, this->getHeight());
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
        int itemsSize =
            std::max(1, static_cast<int>(items.size())) * itemHeight;
        int selectedY = selectedIndex * itemHeight;
        if (selectedY < scrollOffset) {
          scrollOffset = selectedY;
        }
        if (selectedY > scrollOffset + this->getHeight() - itemHeight) {
          scrollOffset = selectedY - this->getHeight() + itemHeight;
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
      scrollOffset -= event.wheel.y * 15.0f;
      if (scrollOffset < 0) {
        scrollOffset = 0;
      }
      int itemsSize = std::max(1, static_cast<int>(items.size())) * itemHeight;
      if (scrollOffset > itemsSize - this->getHeight()) {
        scrollOffset = itemsSize - this->getHeight();
      }
      updateVisibleItems();
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
      touchLastY = touchY;
      touchScrollSpeedReal = 0;
      touchScrollInertia = 0;
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
      scrollOffset += static_cast<int>(touchLastY - touchY);
      touchScrollInertia = 1.2f * (touchLastY - touchY);
      touchLastY = touchY;
      touchDragging = true;

      int itemsSize = std::max(1, static_cast<int>(items.size())) * itemHeight;
      if (scrollOffset < 0) {
        scrollOffset = 0;
      }
      if (scrollOffset > itemsSize - this->getHeight()) {
        scrollOffset = itemsSize - this->getHeight();
      }
      updateVisibleItems();
      break;
    }
    case SDL_FINGERUP: {
      touchDragging = false;
      touchDragged = true;
      if (touchScrollInertia < 2.0f && touchScrollInertia > -2.0f) {
        touchScrollInertia = 0;
        touchScrollSpeed = 0;
      }
      if (touchScrollSpeed < 0 && touchScrollInertia > 0 ||
          touchScrollSpeed > 0 && touchScrollInertia < 0) {
        touchScrollSpeed = touchScrollInertia;
      } else {
        touchScrollSpeed += touchScrollInertia;
      }
      touchScrollSpeedReal = touchScrollSpeed;

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

  inline void setItems(std::vector<T> &&items) {
    this->items = std::move(items);
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
    updateVisibleItems();
  }

  inline void setItems(const std::vector<T> &items) {
    this->items = items;
    // reset selected index
    selectedIndex = -1;
    // reset scroll offset
    scrollOffset = 0;
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
  float touchScrollInertia = 0;
  float touchScrollSpeed = 0;
  float touchScrollSpeedReal = 0;
  SDL_FingerID touchId = -1;
  bool touchDragging = false;
  bool touchDragged = false;
  inline void updateVisibleItems() {
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
      } else {
        // Otherwise, get a recycled view or create a new one
        view = getViewForItem(item);
        idxToView[i] = view;
        if (onBind) {
          onBind(view, item, i,
                 selectedIndex == i); // Bind the item to the view
        }
      }
      // update the position of the view

      view->setPositionNoLayout(this->getX(),
                                this->getY() + (i * itemHeight) - scrollOffset,
                                YGPositionType::YGPositionTypeAbsolute);
      view->setSize(this->getWidth(), itemHeight);

      newVisibleItems.emplace_back(view, item);
    }

    // Recycle any views that are no longer visible
    for (auto &entry : viewEntries) {
      recycleView(entry.first);
    }

    // Update the list of visible items
    viewEntries = std::move(newVisibleItems);
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
  void onLayout() override {
    View::onLayout();
    updateVisibleItems();
  }
};
