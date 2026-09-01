#pragma once

#include "Scene.h"

class View;

class IntroScene final : public Scene {
public:
  explicit IntroScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &) override;

private:
  void start();
  void openSettings();
  void buildView();

  View *rootLayout_ = nullptr;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
};
