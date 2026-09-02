#pragma once

#include "IntroSceneNavigation.h"
#include "Scene.h"

#include "../music_select/MusicSelectInputBindingAdapter.h"

#include <cstdint>
#include <memory>

class Button;
class View;

class IntroScene final : public Scene {
public:
  explicit IntroScene(ApplicationContext &context)
      : Scene(context),
        navigation_(musicSelectKeyLayoutForConfig(
            context.settings.skinMusicSelectInput)) {}

  void init() override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &) override;

private:
  void start();
  void openSettings();
  void buildView();
  void startInputListening();
  void stopInputListening();
  void syncNavigationSelection();
  void processNavigationInput();

  View *rootLayout_ = nullptr;
  Button *startButton_ = nullptr;
  Button *settingsButton_ = nullptr;
  IntroSceneNavigation navigation_;
  std::unique_ptr<MusicSelectInputBindingAdapter> inputBindingAdapter_;
  std::uint64_t inputSubscription_ = 0;
  std::uint64_t inputDeviceSubscription_ = 0;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
};
