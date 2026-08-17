#pragma once
#include "../view/View.h"
#include "../context.h"
#include <SDL2/SDL.h>
#include <vector>
#include <set>
#include <memory>
struct EventHandleResult {
  bool quit = false;
};
class Scene {

public:
  Scene() = delete;
  Scene(ApplicationContext &context) : context(context) {}
  Scene(const Scene &) = delete;
  Scene &operator=(const Scene &) = delete;
  Scene(Scene &&) = delete;
  Scene &operator=(Scene &&) = delete;
  std::vector<View *> views;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>>
      deferred;
  virtual void init() = 0; // Initialize the scene
  virtual void onPause() {}
  virtual void onResume() {}
  virtual bool pausesBackgroundTasksForPerformance() const { return false; }
  virtual EventHandleResult handleEvents(SDL_Event &event) {
    for (auto view : views) {
      if (!view->handleEvents(event)) {
        return {};
      }
    }
    return {};
  }
  virtual void update(float dt) = 0; // Update the scene logic
  void defer(const std::function<bool()> &func, Uint64 delay,
             bool shouldWaitFrame = false) {
    Uint64 time = SDL_GetTicks64() + delay;
    if (deferred.find(time) == deferred.end()) {
      deferred[time] = {};
    }
    deferred[time].first =
        shouldWaitFrame ? context.currentFrame + 1 : context.currentFrame;
    deferred[time].second.push_back(func);
  }
  void handleDeferred() {
    if (deferred.empty()) {
      return;
    }

    Uint64 time = SDL_GetTicks64();
    auto it = deferred.begin();
    while (it != deferred.end()) {
      if (it->first <= time) {
        if (it->second.first <= context.currentFrame) {
          for (const auto &func : it->second.second) {
            if (!func())
              return;
            if (isDead) {
              return;
            }
          }
          it = deferred.erase(it);
        } else {
          ++it;
        }
      } else {
        ++it;
      }
    }
  }
  // Render the scene (non-virtual public method)
  void render() {
    RenderContext renderContext(context.uiBatchRenderer);
    {
      RenderContext::UiBatchScope uiBatchScope(renderContext);
      for (auto view : views) {
        if (renderViewBeforeScene(view)) {
          view->render(renderContext);
        }
      }
    }
    renderScene(); // Additional custom rendering
    {
      RenderContext::UiBatchScope uiBatchScope(renderContext);
      for (auto view : views) {
        if (!renderViewBeforeScene(view)) {
          view->render(renderContext);
        }
      }
    }
  }

  // Cleanup resources when exiting the scene (non-virtual public method)
  inline void cleanup() {
    if (isCleaned) {
      return;
    }
    isDead = true;
    cleanupScene(); // Additional custom cleanup
    destroyOwnedViews();
    isCleaned = true;
  }

  inline void prepareForUse() {
    isDead = false;
    isCleaned = false;
    deferred.clear();
  }

  inline void addView(View *view) {
    if (view == nullptr) {
      return;
    }
    std::unique_ptr<View> pending(view);
    views.push_back(view);
    pending.release();
  }

  virtual ~Scene() { destroyOwnedViews(); }

protected:
  // Protected virtual methods for customization by derived classes
  virtual bool renderViewBeforeScene(const View *view) const { return true; }

  virtual void renderScene() = 0;

  virtual void cleanupScene() = 0;

  ApplicationContext &context;

private:
  void destroyOwnedViews() {
    for (auto *view : views) {
      delete view;
    }
    views.clear();
    deferred.clear();
  }

  bool isDead = false;
  bool isCleaned = false;
};
