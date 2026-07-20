#pragma once

#include "IrUploadsController.h"
#include "Scene.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

class Button;
class IrUploadCandidateListView;
class TextView;
class View;

struct IrUploadsSceneMailbox {
  std::mutex mutex;
  std::optional<std::pair<std::size_t, std::size_t>> progress;
  std::optional<ir_uploads::PreparationOutcome> completion;
};

class IrUploadsScene final : public Scene {
public:
  explicit IrUploadsScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  void update(float dt) override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void buildView();
  void reloadCandidates();
  void refreshUi();
  void refreshProviderState();
  void observeRemoteRevisions();
  void applyMailbox();
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
  std::shared_ptr<ir_uploads::DurableEnqueueGate> enqueueGate;
  std::shared_ptr<IrUploadsSceneMailbox> mailbox;
  std::jthread preparationThread;
  std::string loadError;
  std::string loadDiagnostic;
  bool providerCanSubmit = false;
  bool reloadRequested = false;
  std::uint64_t observedAccountEvidenceRevision = 0;
  std::uint64_t observedAttemptStatusRevision = 0;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
};
