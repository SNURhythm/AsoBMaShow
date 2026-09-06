#pragma once

#include "Scene.h"
#include "SceneReturnTarget.h"

#include <cstdint>

class View;
class ScrollView;

class LibraryTasksScene final : public Scene {
public:
  LibraryTasksScene(ApplicationContext &context, SceneReturnTarget returnTarget)
      : Scene(context), returnTarget_(std::move(returnTarget)) {}

  void init() override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &) override;

private:
  void buildView();
  void refreshTasks();
  void goBack();

  SceneReturnTarget returnTarget_;
  View *rootLayout_ = nullptr;
  ScrollView *taskScroll_ = nullptr;
  View *taskList_ = nullptr;
  std::uint64_t taskRevision_ = 0;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
};
