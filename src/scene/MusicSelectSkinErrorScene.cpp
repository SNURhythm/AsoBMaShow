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

TextView *makeText(std::string value, int size,
                   View::ThemeColorProvider color) {
  auto *result = new TextView(kFontPath, size);
  result->setText(std::move(value));
  result->setThemedColor(std::move(color));
  result->setWrap(true);
  result->setVAlign(TextView::MIDDLE);
  return result;
}

Button *makeButton(std::string label) {
  auto *result = new Button();
  result->setWidth(180)->setHeight(56)->setCornerRadius(
      ui_theme::controlRadius());
  result->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  result->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  result->setStyledBorderWidth(1);
  auto *labelView = makeText(std::move(label), 21, ui_theme::textPrimary);
  labelView->setAlign(TextView::CENTER);
  result->setContentView(labelView);
  return result;
}
} // namespace

MusicSelectSkinErrorScene::MusicSelectSkinErrorScene(
    ApplicationContext &context, std::string selectedSkinPath,
    std::vector<skin::SkinDiagnostic> diagnostics)
    : Scene(context), selectedSkinPath_(std::move(selectedSkinPath)),
      diagnostics_(std::move(diagnostics)) {}

void MusicSelectSkinErrorScene::init() {
  rootLayout_ =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(12)
      ->setPadding(Edge::All, 32)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout_);

  auto *title = makeText("Music-select skin failed", 38, ui_theme::coral);
  title->setHeight(58);
  rootLayout_->addView(title);
  if (!selectedSkinPath_.empty()) {
    auto *path = makeText("Selected skin: " + selectedSkinPath_, 20,
                          ui_theme::textSecondary);
    path->setHeight(44);
    rootLayout_->addView(path);
  }
  if (diagnostics_.empty()) {
    auto *reason = makeText("No diagnostic was reported.", 20,
                            ui_theme::textSecondary);
    reason->setHeight(46);
    rootLayout_->addView(reason);
  } else {
    for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
      const auto &diagnostic = diagnostics_[index];
      auto *reasonView = makeText(
          musicSelectSkinFailureReason(index, diagnostic), 19,
                                  ui_theme::textPrimary);
      reasonView->setMinHeight(42);
      rootLayout_->addView(reasonView);
    }
  }

  auto *actions = new View();
  actions->setHeight(64)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(12);
  auto *settings = makeButton("Settings");
  settings->setOnClickListener([this] { openSettings(); });
  actions->addView(settings);
  rootLayout_->addView(actions);
  rootLayout_->applyYogaLayout();
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
}

void MusicSelectSkinErrorScene::openSettings() {
  context.sceneManager->changeScene(std::make_unique<SettingsScene>(
      context, SettingsDestination::Profile,
      SceneReturnTarget::Retained(this)), true);
}

void MusicSelectSkinErrorScene::update(float) {
  if (rootLayout_ != nullptr &&
      (layoutWidth_ != rendering::window_width ||
       layoutHeight_ != rendering::window_height)) {
    layoutWidth_ = rendering::window_width;
    layoutHeight_ = rendering::window_height;
    rootLayout_->setSize(layoutWidth_, layoutHeight_);
    rootLayout_->applyYogaLayout();
  }
}

void MusicSelectSkinErrorScene::renderScene() {}

void MusicSelectSkinErrorScene::cleanupScene() {
  rootLayout_ = nullptr;
  layoutWidth_ = -1;
  layoutHeight_ = -1;
}
