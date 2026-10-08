#include "MainMenuScene.h"
#include "NewcomerTutorialView.h"

#include "../i18n/PlatformLocale.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"

#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

void MainMenuScene::buildTutorial() {
  bool folderImportCopies = false;
#if TARGET_OS_ANDROID
  folderImportCopies = !AndroidBuildHasManageExternalStorage();
#endif
  const float originalScroll = tutorialRightScroll_->getScrollOffset();
  tutorial_ = new NewcomerTutorialView({
      .saveLanguage = [this](const std::string &language) {
        auto &preference = context.applicationUiState.language;
        std::string previous;
        {
          std::lock_guard lock(context.applicationUiStateMutex);
          previous = preference;
          preference = language;
        }
        if (!context.saveApplicationUiState()) {
          std::lock_guard lock(context.applicationUiStateMutex);
          preference = previous;
          return false;
        }
        i18n::initializePlatformLanguage(language);
        onLanguageChanged();
        return true;
      },
      .complete = [this] {
        auto &completed = context.applicationUiState.newcomerTutorialCompleted;
        bool previous;
        {
          std::lock_guard lock(context.applicationUiStateMutex);
          previous = completed;
          completed = true;
        }
        if (!context.saveApplicationUiState()) {
          std::lock_guard lock(context.applicationUiStateMutex);
          completed = previous;
          return false;
        }
        return true;
      },
      .target = [this](NewcomerTutorialStep step) -> View * {
        switch (step) {
        case NewcomerTutorialStep::Tables: return folderRecyclerView;
        case NewcomerTutorialStep::Folder: return addFolderButton_;
        case NewcomerTutorialStep::Language: return nullptr;
        default: break;
        }
        View *target = step == NewcomerTutorialStep::Download
                           ? findBmsButton : readyPlayOptionsButton;
        // Play options scroll on short screens; Download stays in the fixed
        // primary action area. Only scroll a target inside the scroll view.
        if (step != NewcomerTutorialStep::Download && target && tutorialRightScroll_) {
          const int top = tutorialRightScroll_->getY();
          const int bottom = top + tutorialRightScroll_->getHeight();
          if (target->getY() < top || target->getY() + target->getHeight() > bottom) {
            tutorialRightScroll_->setScrollOffset(
                tutorialRightScroll_->getScrollOffset() + target->getY() - top);
          }
        }
        return target;
      },
      .stepChanged = [this, originalScroll](NewcomerTutorialStep) {
        setFindBmsButtonVisible(findBmsAvailableWithoutTutorial_);
        if (tutorial_ && !tutorial_->getVisible()) {
          tutorialRightScroll_->setScrollOffset(originalScroll);
        }
      },
  }, folderImportCopies);
  rootLayout->addView(tutorial_);
  rootLayout->applyYogaLayout();
}
