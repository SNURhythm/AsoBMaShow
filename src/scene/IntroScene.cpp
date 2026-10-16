#include "../i18n/Localization.h"
#include "IntroScene.h"
#include "MainMenuScene.h"

#include "MusicSelectScene.h"
#include "MusicSelectSkinErrorScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "../music_select/MusicSelectLaunchPolicy.h"
#include "../rendering/common.h"
#include "../rendering/UiSafeArea.h"
#include "../rendering/ResponsiveUi.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"

#include <memory>
#include <utility>

namespace {
constexpr const char *kFontPath = app_theme::font;

TextView *text(std::string value, int size, View::ThemeColorProvider color) {
  auto *view = new TextView(size >= 26 ? app_theme::boldFont : kFontPath, size);
  view->setText(std::move(value));
  view->setThemedColor(std::move(color));
  view->setAlign(TextView::LEFT);
  view->setVAlign(TextView::MIDDLE);
  return view;
}

Button *button(std::string label) {
  auto *result = new Button();
  result->setWidthPercent(100)
      ->setHeight(80)
      ->setPadding(Edge::Left, 24)
      ->setPadding(Edge::Right, 24)
      ->setCornerRadius(app_theme::controlRadius());
  result->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  result->setThemedBorderColors(app_theme::hairlineStrong,
                                app_theme::accentBorder,
                                app_theme::accentBorderStrong);
  result->setStyledBorderWidth(0);
  auto *labelView = text(std::move(label), 34, app_theme::textPrimary);
  labelView->setPadding(Edge::Left, 24)->setPadding(Edge::Right, 24);
  result->setContentView(labelView);
  return result;
}
} // namespace

IntroScene::~IntroScene() { cleanup(); }

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
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(rootLayout_);

  content_ = new View();
  content_->setFlex(1)->setMinHeight(0)->setGap(48);
  rootLayout_->addView(content_);
  title_ = text("AsoBMaShow", 88, app_theme::textPrimary);
  title_->setMinWidth(0)->setFlex(1);
  title_->setWrap(true);
  content_->addView(title_);

  menu_ = new View();
  menu_->setFlexDirection(FlexDirection::Column)->setGap(1)
      ->setJustifyContent(YGJustifyCenter);
  content_->addView(menu_);
  startButton_ = button(i18n::tr("intro.start.label"));
  startButton_->setOnClickListener([this] { start(); });
  menu_->addView(startButton_);
  settingsButton_ = button(i18n::tr("intro.settings.label"));
  settingsButton_->setOnClickListener([this] { openSettings(); });
  menu_->addView(settingsButton_);
  tutorialButton_ = button(i18n::tr("tutorial.replay.label"));
  tutorialButton_->setOnClickListener([this] { startTutorial(); });
  menu_->addView(tutorialButton_);
  layoutView();
}

void IntroScene::layoutView() {
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
  const auto safe = rendering::uiSafeAreaInsets();
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  const bool portrait = layoutWidth_ < layoutHeight_;
  const int inset = portrait ? 28 : 48;
  rootLayout_->setSize(layoutWidth_, layoutHeight_);
  rootLayout_->setPadding(Edge::Left, safe.left + inset)
      ->setPadding(Edge::Right, safe.right + inset)
      ->setPadding(Edge::Top, safe.top + inset)
      ->setPadding(Edge::Bottom, safe.bottom + inset);
  content_->setFlexDirection(portrait ? FlexDirection::Column : FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)->setGap(portrait ? 24 : 48);
  menu_->setWidthPercent(portrait ? 100 : 38);
  menu_->setFlexShrink(0);
  title_->setVAlign(portrait ? TextView::BOTTOM : TextView::MIDDLE);
  title_->setAutoFitText(rendering::compactMenuCanvasActive);
  rootLayout_->applyYogaLayout();
}

void IntroScene::start() {
  pendingStart_ = false;
  if (!context.applicationUiState.newcomerTutorialCompleted) {
    startTutorial();
    return;
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (context.gameplaySkinLifecycle &&
      !context.gameplaySkinLifecycle->presentationReady()) {
    pendingStart_ = true;
    return;
  }
  skin::GameplaySkinAcquisition acquisition;
  if (context.gameplaySkinLifecycle) {
    acquisition =
        context.gameplaySkinLifecycle->acquireForSkinType(5, false);
  } else if (context.settings.presentation().skin.selectedSkinEntries.contains(5)) {
    acquisition.disposition =
        skin::GameplaySkinAcquisitionDisposition::Failed;
    acquisition.failure = skin::GameplaySkinAcquisitionFailure{
        .diagnostic = skin::SkinDiagnostic{
            .code = "skin.music_select.lifecycle_unavailable",
            .message = i18n::tr("intro.selected_music_select_skin_service_unavailable.message")}};
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
  pendingStart_ = false;
  context.sceneManager->changeScene(std::make_unique<SettingsScene>(
      context, SettingsDestination::Profile,
      SceneReturnTarget::Registered("Intro")));
}

void IntroScene::startTutorial() {
  pendingStart_ = false;
  // Gameplay and other menu screens return to the registered MainMenu.
  // Register the tutorial instance so those returns resume its retained state.
  context.sceneManager->registerScene(
      "MainMenu", std::make_unique<MainMenuScene>(context, true));
  context.sceneManager->changeScene("MainMenu");
}

EventHandleResult IntroScene::handleEvents(SDL_Event &event) {
  if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
      event.key.repeat == 0 && inputBindingAdapter_) {
    const bool down = event.type == SDL_EVENT_KEY_DOWN;
    std::optional<MusicSelectControlKey> key;
    switch (event.key.key) {
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (pendingStart_ &&
      (!context.gameplaySkinLifecycle ||
       context.gameplaySkinLifecycle->presentationReady())) {
    start();
    return;
  }
#endif
  processNavigationInput();
  const auto safe = rendering::uiSafeAreaInsets();
  if (rootLayout_ != nullptr &&
      (layoutWidth_ != rendering::window_width ||
       layoutHeight_ != rendering::window_height ||
       lastSafeTop != safe.top || lastSafeLeft != safe.left ||
       lastSafeBottom != safe.bottom || lastSafeRight != safe.right)) {
    layoutView();
  }
}

void IntroScene::renderScene() {}

void IntroScene::cleanupScene() {
  pendingStart_ = false;
  stopInputListening();
  rootLayout_ = nullptr;
  content_ = nullptr;
  menu_ = nullptr;
  title_ = nullptr;
  startButton_ = nullptr;
  settingsButton_ = nullptr;
  tutorialButton_ = nullptr;
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
  const std::array buttons{startButton_, settingsButton_, tutorialButton_};
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    if (buttons[i] == nullptr) continue;
    const bool selected = static_cast<std::size_t>(navigation_.choice()) == i;
    buttons[i]->setSelected(selected);
    buttons[i]->setThemedBackgroundColors(
        selected ? app_theme::primaryAction : app_theme::control,
        selected ? app_theme::primaryActionHover : app_theme::controlHover,
        selected ? app_theme::primaryActionPressed : app_theme::controlPressed);
    if (auto *label = dynamic_cast<TextView *>(buttons[i]->getContentView()))
      label->setThemedColor(selected ? app_theme::onAction : app_theme::textPrimary);
    buttons[i]->setThemedBorderColors(
        selected ? app_theme::accentBorderStrong : app_theme::hairlineStrong,
        app_theme::accentBorder, app_theme::accentBorderStrong);
  }
}

void IntroScene::processNavigationInput() {
  if (!inputBindingAdapter_) return;
  const auto result = navigation_.process(inputBindingAdapter_->state(),
                                          SDL_GetTicks());
  inputBindingAdapter_->clearFrameEdges();
  if (result.selectionChanged) syncNavigationSelection();
  if (!result.activated) return;
  if (*result.activated == IntroSceneChoice::Settings) {
    openSettings();
  } else if (*result.activated == IntroSceneChoice::Tutorial) {
    startTutorial();
  } else {
    start();
  }
}

void IntroScene::onInputQueueOverflow() {
  Scene::onInputQueueOverflow();
  if (inputBindingAdapter_) inputBindingAdapter_->reset();
  navigation_.cancelInput(musicSelectKeyLayoutForConfig(
      context.settings.skinMusicSelectInput));
}
