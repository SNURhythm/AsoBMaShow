#pragma once

#include "IntroSceneNavigation.h"
#include "Scene.h"

#include "../music_select/MusicSelectInputBindingAdapter.h"

#include <cstdint>
#include <memory>

class Button;
class View;
class TextView;

class IntroScene final : public Scene {
public:
  bool needsContinuousPresentation() const override { return false; }
  bool usesResponsiveMenuCanvas() const override { return true; }
  explicit IntroScene(ApplicationContext &context)
      : Scene(context),
        navigation_(musicSelectKeyLayoutForConfig(
            context.settings.skinMusicSelectInput)) {}
  ~IntroScene() override;

  void onInputQueueOverflow() override;
  void init() override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &) override;

private:
  void start();
  void startTutorial();
  void openSettings();
  void buildView();
  void layoutView();
  void startInputListening();
  void stopInputListening();
  void syncNavigationSelection();
  void processNavigationInput();

  View *rootLayout_ = nullptr;
  View *content_ = nullptr;
  View *menu_ = nullptr;
  TextView *title_ = nullptr;
  Button *startButton_ = nullptr;
  Button *settingsButton_ = nullptr;
  Button *tutorialButton_ = nullptr;
  IntroSceneNavigation navigation_;
  std::unique_ptr<MusicSelectInputBindingAdapter> inputBindingAdapter_;
  std::uint64_t inputSubscription_ = 0;
  std::uint64_t inputDeviceSubscription_ = 0;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;
  bool pendingStart_ = false;
};
