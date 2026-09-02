#include "IntroScene.h"

#include "MusicSelectScene.h"
#include "MusicSelectSkinErrorScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "../music_select/MusicSelectLaunchPolicy.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <memory>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

TextView *text(std::string value, int size, View::ThemeColorProvider color) {
  auto *view = new TextView(kFontPath, size);
  view->setText(std::move(value));
  view->setThemedColor(std::move(color));
  view->setAlign(TextView::CENTER);
  view->setVAlign(TextView::MIDDLE);
  return view;
}

Button *button(std::string label) {
  auto *result = new Button();
  result->setWidth(300)
      ->setHeight(64)
      ->setCornerRadius(ui_theme::controlRadius());
  result->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  result->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  result->setStyledBorderWidth(1);
  result->setContentView(text(std::move(label), 24, ui_theme::textPrimary));
  return result;
}
} // namespace

void IntroScene::init() {
  navigation_.reset(
      musicSelectKeyLayoutForConfig(context.settings.skinMusicSelectInput));
  buildView();
  startInputListening();
  syncNavigationSelection();
}

void IntroScene::buildView() {
  rootLayout_ =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(18)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout_);

  auto *title = text("AsoBMaShow", 58, ui_theme::textPrimary);
  title->setWidth(720)->setHeight(92);
  rootLayout_->addView(title);

  startButton_ = button("Start");
  startButton_->setOnClickListener([this] { start(); });
  rootLayout_->addView(startButton_);

  settingsButton_ = button("Settings");
  settingsButton_->setOnClickListener([this] { openSettings(); });
  rootLayout_->addView(settingsButton_);
  rootLayout_->applyYogaLayout();
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
}

void IntroScene::start() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skin::GameplaySkinAcquisition acquisition;
  if (context.gameplaySkinLifecycle) {
    acquisition =
        context.gameplaySkinLifecycle->acquireForSkinType(5, false);
  } else if (context.settings.skin.selectedSkinEntries.contains(5)) {
    acquisition.disposition =
        skin::GameplaySkinAcquisitionDisposition::Failed;
    acquisition.failure = skin::GameplaySkinAcquisitionFailure{
        .diagnostic = skin::SkinDiagnostic{
            .code = "skin.music_select.lifecycle_unavailable",
            .message = "The selected music-select skin service is unavailable."}};
  }
  auto decision = decideMusicSelectLaunch(std::move(acquisition));
  if (decision.kind == MusicSelectLaunchKind::SelectedSkin &&
      decision.request) {
    context.sceneManager->changeScene(std::make_unique<MusicSelectScene>(
        context, std::move(*decision.request)));
    return;
  }
  if (decision.kind == MusicSelectLaunchKind::Error) {
    context.sceneManager->changeScene(
        std::make_unique<MusicSelectSkinErrorScene>(
            context, std::move(decision.selectedSkinPath),
            std::move(decision.diagnostics)));
    return;
  }
#endif
  context.sceneManager->changeScene("MainMenu");
}

void IntroScene::openSettings() {
  context.sceneManager->changeScene(std::make_unique<SettingsScene>(
      context, SettingsDestination::Profile,
      SceneReturnTarget::Registered("Intro")));
}

EventHandleResult IntroScene::handleEvents(SDL_Event &event) {
  if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
      event.key.repeat == 0 && inputBindingAdapter_) {
    const bool down = event.type == SDL_KEYDOWN;
    std::optional<MusicSelectControlKey> key;
    switch (event.key.keysym.sym) {
    case SDLK_UP: key = MusicSelectControlKey::Up; break;
    case SDLK_DOWN: key = MusicSelectControlKey::Down; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: key = MusicSelectControlKey::Enter; break;
    default: break;
    }
    if (key) {
      auto &input = inputBindingAdapter_->state();
      if (down) {
        input.controlPressed.insert(*key);
        input.controlHeld.insert(*key);
      } else {
        input.controlHeld.erase(*key);
      }
      return {};
    }
  }
  return Scene::handleEvents(event);
}

void IntroScene::update(float) {
  processNavigationInput();
  if (rootLayout_ != nullptr &&
      (layoutWidth_ != rendering::window_width ||
       layoutHeight_ != rendering::window_height)) {
    layoutWidth_ = rendering::window_width;
    layoutHeight_ = rendering::window_height;
    rootLayout_->setSize(layoutWidth_, layoutHeight_);
    rootLayout_->applyYogaLayout();
  }
}

void IntroScene::renderScene() {}

void IntroScene::cleanupScene() {
  stopInputListening();
  rootLayout_ = nullptr;
  startButton_ = nullptr;
  settingsButton_ = nullptr;
  layoutWidth_ = -1;
  layoutHeight_ = -1;
}

void IntroScene::startInputListening() {
  if (inputSubscription_ != 0 || inputDeviceSubscription_ != 0) return;
  const auto layout =
      musicSelectKeyLayoutForConfig(context.settings.skinMusicSelectInput);
  inputBindingAdapter_ = std::make_unique<MusicSelectInputBindingAdapter>(
      context.inputProfile, layout);
  inputSubscription_ = context.inputDeviceRegistry.subscribeInput(
      [this](const input::PhysicalInputEvent &event) {
        if (inputBindingAdapter_) inputBindingAdapter_->consume(event);
      });
  inputDeviceSubscription_ = context.inputDeviceRegistry.subscribeDevices(
      [this](const input::InputDeviceSnapshot &device) {
        if (!device.connected && inputBindingAdapter_) {
          inputBindingAdapter_->disconnectDevice(device.stableId);
        }
      });
}

void IntroScene::stopInputListening() {
  if (inputSubscription_ != 0) {
    context.inputDeviceRegistry.unsubscribe(inputSubscription_);
    inputSubscription_ = 0;
  }
  if (inputDeviceSubscription_ != 0) {
    context.inputDeviceRegistry.unsubscribe(inputDeviceSubscription_);
    inputDeviceSubscription_ = 0;
  }
  if (inputBindingAdapter_) inputBindingAdapter_->reset();
  inputBindingAdapter_.reset();
}

void IntroScene::syncNavigationSelection() {
  if (startButton_ == nullptr || settingsButton_ == nullptr) return;
  const bool startSelected = navigation_.choice() == IntroSceneChoice::Start;
  startButton_->setSelected(startSelected);
  settingsButton_->setSelected(!startSelected);
  startButton_->setThemedBorderColors(
      startSelected ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong,
      ui_theme::accentBorder, ui_theme::accentBorderStrong);
  settingsButton_->setThemedBorderColors(
      startSelected ? ui_theme::hairlineStrong : ui_theme::accentBorderStrong,
      ui_theme::accentBorder, ui_theme::accentBorderStrong);
}

void IntroScene::processNavigationInput() {
  if (!inputBindingAdapter_) return;
  const auto result = navigation_.process(inputBindingAdapter_->state(),
                                          SDL_GetTicks64());
  inputBindingAdapter_->clearFrameEdges();
  if (result.selectionChanged) syncNavigationSelection();
  if (!result.activated) return;
  if (*result.activated == IntroSceneChoice::Settings) {
    openSettings();
  } else {
    start();
  }
}
