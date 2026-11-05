#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../recap/PlayRecap.h"
#include <future>
#include "../kombo/KomboModel.h"
class OverlayPortal;

class PlayRecapScene final : public Scene {
public:
  PlayRecapScene(ApplicationContext &context, SceneReturnTarget target)
      : Scene(context), returnTarget_(std::move(target)) {}
  void init() override;
  void update(float) override;
  void onPause() override;
  void onResume() override;
  void onLanguageChanged() override { build(); }
  void onPresentationOrientationChanged() override { build(); }
  bool usesResponsiveMenuCanvas() const override { return true; }
  bool needsContinuousPresentation() const override { return loading_ || exporting_; }
  bool locksOrientation() const override { return exporting_; }
  EventHandleResult handleEvents(SDL_Event &) override;
private:
  void renderScene() override {}
  void cleanupScene() override { root_ = nullptr; }
  void later(std::function<void()>);
  void reload();
  void build();
  void buildFilters(View *);
  void back();
  void movePeriod(int direction);
  void beginExport(bool share = false);
  void exportReceipt();
  void addUpgrade(View *, int kind, const recap::Upgrade &);
  std::string periodText() const;
  SceneReturnTarget returnTarget_;
  recap::Snapshot snapshot_;
  std::future<recap::Snapshot> load_;
  std::string profile_, loadedProfile_, profileName_, message_, from_, through_;
  int period_ = 0, dayOffset_ = 0;
  std::int64_t sessionBefore_ = 0;
  std::vector<std::int64_t> sessionForward_;
  std::array<std::size_t, 3> offsets_{};
  bool loading_ = false, reloadPending_ = false, exporting_ = false, sharing_ = false;
  std::filesystem::path exportDirectory_;
  kombo::Assignment difficultyFilter_;
  int tableId_ = 0;
  std::optional<std::string> rangeStart_, rangeEnd_;
  bool includeNew_ = true, filtersExpanded_ = false;
  OverlayPortal *portal_ = nullptr;
  View *root_ = nullptr;
};
