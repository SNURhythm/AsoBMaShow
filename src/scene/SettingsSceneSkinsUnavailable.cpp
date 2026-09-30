#include "../i18n/Localization.h"
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
      available ? i18n::message("settings.skins.gameplay_skin_support_starting.message") :
                  i18n::message("settings.skins.gameplay_skins_unavailable_in_build.message"),
      metrics.bodyTextSize, ui_theme::textSecondary()));
  body->addView(makeWrappedText(
      i18n::message("settings.skins.built_in_gameplay_presentation_remains_active.message"),
      metrics.smallTextSize, ui_theme::textMuted()));
  column->addView(makeCard(metrics, i18n::message("settings.skins.skins.label"), i18n::message("settings.skins.availability.label"), body,
                           metrics.modeCardHeight, metrics.cardsWidth));
  return column;
}

#endif
