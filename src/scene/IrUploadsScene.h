#pragma once

#include "IrUploadsController.h"
#include "IrUploadPreparationTask.h"
#include "Scene.h"
#include "SceneReturnTarget.h"

#include <cstddef>
#include <cstdint>
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
  explicit IrUploadsScene(
      ApplicationContext &context,
      SceneReturnTarget returnTarget =
          SceneReturnTarget::Registered("MainMenu"))
      : Scene(context), returnTarget_(std::move(returnTarget)) {}

  void init() override;
  void update(float dt) override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  SceneReturnTarget returnTarget_;
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
  std::string loadError;
  std::string loadDiagnostic;
  bool providerCanSubmit = false;
  bool reloadRequested = false;
  std::uint64_t observedAccountEvidenceRevision = 0;
  std::uint64_t observedAttemptStatusRevision = 0;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
};
