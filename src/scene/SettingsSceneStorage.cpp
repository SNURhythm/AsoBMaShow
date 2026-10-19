#include "SettingsSceneShared.h"
#include "StorageManagementScene.h"
#include "SceneManager.h"
#include "../view/StoragePolicyView.h"
#include "../view/StoragePolicyEditorState.h"
#include "../view/ActionButtonStyle.h"

using namespace settings_scene;
using namespace storage_management;
View *SettingsScene::buildStorageControls(const LayoutMetrics &metrics) {
  auto *controls = new View();
  controls->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(12);
  auto *openText = makeText(i18n::message("storage.ui.title"), metrics.bodyTextSize + 2,
                            app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE);
  auto *open = makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight, openText);
  action_button::applyStyle(open, openText, app_theme::ActionCategory::Primary);
  open->setOnClickListener([this] {
    defer([this] {
      context.sceneManager->changeScene(
          new StorageManagementScene(context, SceneReturnTarget::Retained(this)), true);
      return true; // Settings remains alive; retire this callback before it resumes.
    }, 0, true);
  });
  controls->addView(open);
  controls->addView(makeWrappedText(i18n::message("storage.ui.download_policy"),
                                   metrics.bodyTextSize, app_theme::textPrimary()));
  auto *toggleText = makeText("", metrics.bodyTextSize, app_theme::textPrimary(),
                              TextView::CENTER, TextView::MIDDLE);
  auto *toggle = makeControlButton(-1, metrics.actionButtonHeight, toggleText);
  auto editorState = std::make_shared<DownloadStoragePolicyEditorState>();
  auto refresh = [this, toggle, toggleText, editorState] {
    const auto &preferences = context.settings.storagePreferences;
    action_button::applyStyle(toggle, toggleText,
        editorState->AwaitingAcknowledgment() || (preferences.afterDownload && preferences.download.bga == BgaAction::Delete)
            ? app_theme::ActionCategory::Destructive
            : preferences.afterDownload ? app_theme::ActionCategory::Confirmation : app_theme::ActionCategory::Information);
    toggleText->setLocalizedText(i18n::message(editorState->AwaitingAcknowledgment() ? "storage.ui.confirm_auto_delete" :
        (context.settings.storagePreferences.afterDownload ? "storage.ui.auto_on" : "storage.ui.auto_off")));
  };
  toggle->setOnClickListener([this, editorState, refresh] {
    if (editorState->ToggleAutomation(context.settings.storagePreferences)) persistSettings();
    refresh();
  });
  controls->addView(toggle);
  auto *editor = new StoragePolicyView(context.settings.storagePreferences.download,
      [this, editorState, refresh](StoragePolicy policy) {
        editorState->ChangePolicy(context.settings.storagePreferences, policy);
        persistSettings();
        refresh();
      }, overlayPortal);
  controls->addView(editor);
  controls->addView(makeWrappedText(i18n::message("storage.ui.download_help"),
                                   metrics.smallTextSize, app_theme::textSecondary()));
  refresh();
  return makeCard(metrics, i18n::message("storage.ui.title"),
      i18n::message("storage.ui.settings_help"), controls, metrics.modeCardHeight, metrics.cardsWidth);
}
