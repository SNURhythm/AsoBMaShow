#include "SettingsSceneShared.h"
#include "../GameplayKeyMode.h"
#include "../view/ScrollView.h"
#include "../view/ViewVisibility.h"
#include "../input/InputCaptureController.h"

using namespace settings_scene;

namespace {
Button *sectionButton(const LayoutMetrics &metrics, const i18n::Text &label,
                      std::function<void()> action, bool enabled = true) {
  auto *text = makeText(label, metrics.smallTextSize, app_theme::textPrimary(),
                        TextView::CENTER, TextView::MIDDLE);
  text->setAutoFitText(true);
  auto *button = makeControlButton(std::min(std::max(130, metrics.cardsWidth - metrics.cardPadding * 2),
                                           std::max(130, text->textureWidth() + 32)),
                                   metrics.actionButtonHeight, text);
  button->setEnabled(enabled);
  button->setOnClickListener(std::move(action));
  return button;
}
}

bool SettingsScene::selectSettingsTab(SettingsTab tab) {
    if (activeTab == tab) {
      return true;
    }
    if (activeTab == SettingsTab::Display) {
      cancelDisplayPreviewForTabExit();
    }
    if (activeTab == SettingsTab::Input &&
        inputCaptureController != nullptr) {
      inputCaptureController->cancel();
      inputCaptureAction.reset();
      inputViewRebuildGate.reset();
      inputLastViewSignature.clear();
    }
    if (activeTab == SettingsTab::Profile && profileController != nullptr) {
      const auto phase = profileController->phase();
      if (phase == ProfileSettingsPhase::PickingImport ||
          phase == ProfileSettingsPhase::Importing ||
          phase == ProfileSettingsPhase::PreparingExport ||
          phase == ProfileSettingsPhase::PickingExport) {
        return false;
      }
      profileInlineEditor.clear();
      profileController->cancelConfirmation();
    }
    pendingSectionAction.clear();
    activeTab = tab;
    if (activeTab == SettingsTab::GameplaySkins) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
      ensureGameplaySkinSettingsController();
#endif
    }
    settingsScrollResetRequested = true;
    lastLayoutWidth = -1;
    return true;
}

i18n::Text SettingsScene::orientationLabel(AppSettings::PresentationOrientation orientation) const {
  return i18n::message(orientation == AppSettings::PresentationOrientation::Portrait
      ? "settings.display.orientation_portrait.label" : "settings.display.orientation_landscape.label");
}

i18n::Text SettingsScene::appearanceScope(int mode, bool shared, const i18n::Text &skinName) const {
  const auto layout = orientationLabel(context.settings.activePresentationOrientation());
  return shared ? i18n::message("settings.clarity.builtin_scope", {{"layout", layout}})
      : i18n::message("settings.clarity.mode_scope", {{"skin", skinName},
          {"mode", gameplay::keyModeLabel(mode)}, {"layout", layout}});
}

void SettingsScene::appendSectionActions(View *body, const LayoutMetrics &metrics,
    const std::string &key, const i18n::Text &section, const i18n::Text &scope,
    bool enabled, std::function<void(bool)> apply, bool allowCopy) {
  const auto orientation = context.settings.activePresentationOrientation();
  const auto profile = context.profileManager.activeProfile().id;
  const auto identity = key + ":" + profile + ":" + std::to_string(static_cast<int>(orientation));
  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(8);
  if (pendingSectionAction == identity) {
    const auto source = orientationLabel(settings_edit::otherOrientation(orientation));
    body->addView(makeWrappedText(pendingSectionCopy
        ? i18n::message("settings.clarity.confirm_copy", {{"section", section}, {"source", source}, {"scope", scope}})
        : i18n::message("settings.clarity.confirm_reset", {{"section", section}, {"scope", scope}}),
        metrics.smallTextSize, app_theme::coral()));
    actions->addView(sectionButton(metrics, i18n::message("settings.clarity.confirm"),
        [this, profile, orientation, identity, apply, copy = pendingSectionCopy] {
          if (pendingSectionAction != identity || context.profileManager.activeProfile().id != profile ||
              context.settings.activePresentationOrientation() != orientation) return;
          pendingSectionAction.clear();
          apply(copy);
          lastLayoutWidth = -1;
        }, enabled));
    actions->addView(sectionButton(metrics, i18n::message("settings.clarity.cancel"), [this] {
      pendingSectionAction.clear();
      lastLayoutWidth = -1;
    }));
  } else {
    actions->addView(sectionButton(metrics, i18n::message("settings.clarity.reset"), [this, identity] {
      pendingSectionAction = identity;
      pendingSectionCopy = false;
      lastLayoutWidth = -1;
    }, enabled));
    if (allowCopy) actions->addView(sectionButton(metrics,
        i18n::message("settings.clarity.copy", {{"layout", orientationLabel(settings_edit::otherOrientation(orientation))}}),
        [this, identity] {
          pendingSectionAction = identity;
          pendingSectionCopy = true;
          lastLayoutWidth = -1;
        }, enabled));
  }
  body->addView(actions);
}

void SettingsScene::appendBuiltInSectionActions(View *body, const LayoutMetrics &metrics,
    settings_edit::AppearanceSection section, int mode, const i18n::Text &label) {
  using Section = settings_edit::AppearanceSection;
  const auto orientation = context.settings.activePresentationOrientation();
  i18n::Text scope = appearanceScope(mode, section == Section::Hud, i18n::message("settings.clarity.builtin"));
  if (section == Section::SharedOverlays || section == Section::LaneCovers)
    scope = i18n::message("settings.clarity.shared_scope", {{"layout", orientationLabel(orientation)}});
  if (section == Section::Lane || section == Section::Effects)
    scope = scope.resolve() + " " + i18n::message(section == Section::Lane
        ? "settings.clarity.shared_geometry" : "settings.clarity.shared_effects").resolve();
  appendSectionActions(body, metrics, "appearance:" + std::to_string(static_cast<int>(section)) + ":" + std::to_string(mode),
      label, scope, true, [this, section, mode, orientation](bool copy) {
        auto &destination = context.settings.presentation(orientation);
        if (copy) settings_edit::copyAppearanceSection(destination,
            context.settings.presentation(settings_edit::otherOrientation(orientation)), section, mode, orientation);
        else settings_edit::resetAppearanceSection(destination, section, mode, orientation);
        closeAppearanceColorPopup();
        persistSettings();
      });
}

void SettingsScene::appendSharedOverlaySettings(View *body, const LayoutMetrics &metrics) {
  body->addView(makeWrappedText(i18n::message("settings.clarity.shared_scope",
      {{"layout", orientationLabel(context.settings.activePresentationOrientation())}}),
      metrics.smallTextSize, app_theme::textSecondary()));
  appendSelectedSkinHudSettings(body, metrics, false);
  appendBuiltInSectionActions(body, metrics, settings_edit::AppearanceSection::SharedOverlays,
      7, i18n::message("settings.clarity.overlays"));
}

void SettingsScene::appendAppearanceShortcut(View *body, const LayoutMetrics &metrics, AppearancePage page) {
  body->addView(sectionButton(metrics, i18n::message(page == AppearancePage::Overlays
      ? "settings.clarity.overlays" : "settings.clarity.builtin"), [this, page] {
    pendingSectionAction.clear();
    settingsScrollResetRequested = true;
    if (previewActive) {
      previewPanelPage = page == AppearancePage::Overlays ? 2 : 1;
    } else {
      appearancePage = page;
    }
    lastLayoutWidth = -1;
  }));
}

void SettingsScene::appendAppearanceNavigation(View *body, const LayoutMetrics &metrics) {
  auto *tabs = new View();
  tabs->setFlexDirection(FlexDirection::Row);
  tabs->setFlexWrap(YGWrapWrap);
  tabs->setGap(8);
  for (auto [page, label] : {std::pair{AppearancePage::Skin, "settings.clarity.skin_edit"},
       {AppearancePage::BuiltIn, "settings.clarity.builtin"},
       {AppearancePage::Overlays, "settings.clarity.overlays"},
       {AppearancePage::Manage, "settings.clarity.manage"}}) {
    auto *button = sectionButton(metrics, i18n::message(label), [this, page] {
      appearancePage = page;
      settingsScrollResetRequested = true;
      pendingSectionAction.clear();
      lastLayoutWidth = -1;
    });
    tab_button::style(button, appearancePage == page);
    tabs->addView(button);
  }
  body->addView(tabs);
}

void SettingsScene::openSettingsSearchResult(settings_search::Destination destination) {
  using Destination = settings_search::Destination;
  SettingsTab tab = SettingsTab::GameplaySkins;
  switch (destination) {
  case Destination::Profile: tab = SettingsTab::Profile; break;
  case Destination::Calibration: tab = SettingsTab::Timing; break;
  case Destination::Scroll: tab = SettingsTab::Lane; break;
  case Destination::Input: tab = SettingsTab::Input; break;
  case Destination::Background: tab = SettingsTab::Background; break;
  case Destination::General: tab = SettingsTab::General; break;
  case Destination::Audio: tab = SettingsTab::Audio; break;
  case Destination::Display: tab = SettingsTab::Display; break;
  case Destination::Tables: tab = SettingsTab::DifficultyTables; break;
  case Destination::Library: tab = SettingsTab::BmsLibrary; break;
  case Destination::Ir: tab = SettingsTab::Ir; break;
  case Destination::Legal: tab = SettingsTab::Legal; break;
  default: break;
  }
  if (!selectSettingsTab(tab)) return;
  settingsSearchQuery.clear();
  settingsSearchOpen = false;
  settingsScrollResetRequested = true;
  pendingSectionAction.clear();
  switch (destination) {
  case Destination::Skin: appearancePage = AppearancePage::Skin; break;
  case Destination::Overlays: appearancePage = AppearancePage::Overlays; break;
  case Destination::Manage: appearancePage = AppearancePage::Manage; break;
  case Destination::BuiltInLane: case Destination::Notes: case Destination::Effects: case Destination::Hud:
    appearancePage = AppearancePage::BuiltIn;
    builtInSkinTab = destination == Destination::Notes ? BuiltInSkinTab::Notes
        : destination == Destination::Effects ? BuiltInSkinTab::Effects
        : destination == Destination::Hud ? BuiltInSkinTab::Hud : BuiltInSkinTab::Lane;
    break;
  default: break;
  }
  lastLayoutWidth = -1;
}

void SettingsScene::appendSettingsSearch(View *body, const LayoutMetrics &metrics) {
  auto *search = new View();
  search->setFlexDirection(FlexDirection::Column);
  search->setGap(12);
  search->setFlex(1);
  search->setMinHeight(0);
  search->setPadding(Edge::Bottom, metrics.safe.bottom + metrics.verticalPadding);
  search->setPadding(Edge::Left, metrics.safe.left + metrics.horizontalPadding);
  search->setPadding(Edge::Right, metrics.safe.right + metrics.horizontalPadding);
  search->setFlexShrink(1);
  search->addView(makeWrappedText(i18n::message("settings.clarity.search"),
      metrics.smallTextSize, app_theme::textSecondary()));
  auto *input = makeTextInput(metrics, metrics.contentWidth);
  input->setWidthPercent(100);
  input->setFlexShrink(0);
  input->setName("settingsSearch");
  input->setClearable(true);
  input->setEditingText(settingsSearchQuery);
  search->addView(input);
  auto *results = new View();
  results->setFlexDirection(FlexDirection::Column);
  results->setGap(4);
  std::vector<std::pair<std::string_view, Button *>> buttons;
  for (const auto &entry : settings_search::entries) {
    auto *button = sectionButton(metrics, i18n::message(entry.title.data()), [this, destination = entry.destination] {
      openSettingsSearchResult(destination);
    });
    button->setWidthPercent(100);
    button->setFlexShrink(0);
    results->addView(button);
    buttons.emplace_back(entry.title, button);
  }
  auto *empty = makeWrappedText(i18n::message("settings.clarity.no_results"),
      metrics.smallTextSize, app_theme::textSecondary());
  results->addView(empty);
  auto *resultScroll = new ScrollView();
  resultScroll->setFlex(1);
  resultScroll->setMinHeight(0);
  resultScroll->setWidthPercent(100);
  resultScroll->setContentView(results);
  search->addView(resultScroll);
  // Filter retained rows without rebuilding the input or ending composition.
  const auto filter = [this, buttons, empty, resultScroll](const std::string &query) {
    View::LayoutBatchScope layoutBatch;
    settingsSearchQuery = query;
    resultScroll->setScrollOffset(0);
    const auto matches = settings_search::search(query, [](std::string_view id) {
      return i18n::message(id.data()).resolve();
    });
    const bool hasQuery = query.find_first_not_of(" \t\r\n") != std::string::npos;
    view_visibility::setLayoutVisible(resultScroll, hasQuery);
    for (const auto &[title, button] : buttons) {
      const auto found = std::ranges::find(matches, title, &settings_search::Result::title);
      view_visibility::setLayoutVisible(button, found != matches.end());
    }
    view_visibility::setLayoutVisible(empty, hasQuery && matches.empty());
  };
  input->onTextChanged(filter);
  filter(settingsSearchQuery);
  body->addView(search);
  // Focus after the layout batch completes so native keyboards get the final
  // input rectangle. Resolve the current view instead of retaining a pointer
  // across a possible close or rebuild.
  defer([this] {
    if (settingsSearchOpen && rootLayout != nullptr) {
      if (auto *field = dynamic_cast<TextInputBox *>(rootLayout->findViewByName("settingsSearch")))
        field->beginEditing();
    }
    return true;
  }, 0, true);
}
