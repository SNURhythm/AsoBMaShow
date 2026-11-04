#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../recap/PlayRecap.h"
#include <future>

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
  void back();
  void movePeriod(int direction);
  void beginExport();
  void exportNext();
  void addUpgrade(View *, int kind, const recap::Upgrade &, bool image = false);
  std::string periodText() const;
  std::string exportSummary() const;
  SceneReturnTarget returnTarget_;
  recap::Snapshot snapshot_;
  std::future<recap::Snapshot> load_;
  std::string profile_, loadedProfile_, profileName_, message_, from_, through_;
  int period_ = 0, dayOffset_ = 0;
  std::int64_t sessionBefore_ = 0;
  std::vector<std::int64_t> sessionForward_;
  std::array<std::size_t, 3> offsets_{};
  bool loading_ = false, reloadPending_ = false, exporting_ = false;
  struct ExportPage { int kind; std::size_t start, count; };
  std::vector<ExportPage> exportPages_;
  std::size_t exportPage_ = 0;
  bool exportedToPhotos_ = false;
  int exportWidth_ = 0, exportHeight_ = 0;
  std::filesystem::path exportDirectory_;
  View *root_ = nullptr;
};
