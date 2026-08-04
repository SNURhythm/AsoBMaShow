#include "SettingsSceneShared.h"

#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

using namespace settings_scene;

View *SettingsScene::buildGameplaySkinsTab(const LayoutMetrics &metrics) {
  auto *column = new View();
  column->setFlexDirection(FlexDirection::Column);
  column->setGap(static_cast<float>(metrics.secondaryGap));
  column->setWidth(static_cast<float>(metrics.cardsWidth));

  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setGap(static_cast<float>(metrics.cardGap));
  const bool available = skin::luaGameplaySkinsAvailable();
  body->addView(makeWrappedText(
      available ? "Gameplay skin support is starting." :
                  "Gameplay skins are unavailable in this build.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  body->addView(makeWrappedText(
      "The built-in gameplay presentation remains active.",
      metrics.smallTextSize, ui_theme::textMuted()));
  column->addView(makeCard(metrics, "Gameplay Skins", "Availability", body,
                           metrics.modeCardHeight, metrics.cardsWidth));
  return column;
}

#endif
