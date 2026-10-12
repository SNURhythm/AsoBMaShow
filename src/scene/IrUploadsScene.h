#pragma once

#include "IrUploadsController.h"
#include "IrUploadPreparationTask.h"
#include "Scene.h"
#include "SceneReturnTarget.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class Button;
class IrUploadCandidateListView;
class TextView;
class View;

class IrUploadsScene final : public Scene {
public:
  bool usesResponsiveMenuCanvas() const override { return true; }
  explicit IrUploadsScene(
      ApplicationContext &context,
      SceneReturnTarget returnTarget =
          SceneReturnTarget::Registered("MainMenu"))
      : Scene(context), returnTarget_(std::move(returnTarget)) {}

  // Configure before init; the shell owns the header and scene lifetime.
  void configureTab(float topInset, std::function<void()> onClose,
                    Scene *settingsReturnOwner);
  void setTabTopInset(float topInset);
  void onPause() override;
  void onResume() override;
  // Call before switching the active profile/repositories.
  void onProfileWillChange();

  void init() override;
  void update(float dt) override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  SceneReturnTarget returnTarget_;
  Scene *tabSettingsReturnOwner = nullptr;
  bool embeddedTab = false;
  float tabTopInset = 0;
  std::function<void()> tabClose;
  std::string displayedProfileId;
  void updateLayoutInsets();
  void refreshProfileIfNeeded();
  void buildView();
  void reloadCandidates();
  void refreshUi();
  void refreshProviderState();
  void observeRemoteRevisions();
  void applyPreparationUpdates();
  void startUpload();
  void stopPreparation();
  void goBack();
  void openIrSettings();
  [[nodiscard]] std::string serverOrigin() const;

  View *rootLayout = nullptr;
  View *headerLayout = nullptr;
  View *contentLayout = nullptr;
  TextView *candidateCountText = nullptr;
  TextView *providerStatusText = nullptr;
  TextView *selectionCountText = nullptr;
  TextView *stateText = nullptr;
  TextView *progressText = nullptr;
  TextView *uploadButtonText = nullptr;
  Button *refreshButton = nullptr;
  Button *openIrSettingsButton = nullptr;
  Button *selectAllButton = nullptr;
  Button *clearButton = nullptr;
  Button *uploadButton = nullptr;
  IrUploadCandidateListView *candidateList = nullptr;

  ir_uploads::Controller controller;
  ir_uploads::PreparationTask preparationTask;
  i18n::Text loadError;
  i18n::Text loadDiagnostic;
  bool providerCanSubmit = false;
  bool reloadRequested = false;
  bool candidatesLoaded = false;
  std::uint64_t observedScoreRevision = 0;
  std::uint64_t observedProfileCacheRevision = 0;
  std::string observedServerOrigin;
  std::uint64_t observedAccountEvidenceRevision = 0;
  std::uint64_t observedAttemptStatusRevision = 0;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
};
