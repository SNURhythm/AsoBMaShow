#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../library/GroupedChartListModel.h"
#include "../storage/StorageInventoryTask.h"
#include "../storage/StorageTask.h"

class Button;
class TextView;
class TextInputBox;
class ScrollView;
class StoragePolicyView;
template <typename T> class RecyclerView;

class StorageManagementScene final : public Scene {
public:
  StorageManagementScene(ApplicationContext &, SceneReturnTarget);
  ~StorageManagementScene() override;
  bool needsContinuousPresentation() const override { return false; }
  bool usesResponsiveMenuCanvas() const override { return true; }
  void init() override;
  void update(float) override;
  void onLanguageChanged() override;
  void onPresentationOrientationChanged() override { if (root_) Layout(); }
  void onApplicationBackgroundChanged(bool) override;
  EventHandleResult handleEvents(SDL_Event &) override;
  bool prepareForStorageMaintenance(std::string &) override;
private:
  using Track = music_playlist::MusicTrack;
  void renderScene() override {}
  void cleanupScene() override;
  void BuildView();
  void Layout();
  void ReloadGroups();
  void RefreshRows();
  void RefreshState();
  void InvalidatePreflight();
  void Measure(bool selectedOnly);
  void ReviewOrApply();
  void ToggleGroup(const Track &);
  void ToggleExpanded(const Track &);
  void GoBack();
  storage_management::InventoryRequest RequestFor(const std::vector<Track> &) const;
  std::string MeasurementText(const Track &) const;

  SceneReturnTarget returnTarget_;
  std::string profileId_;
  GroupedChartListModel model_;
  StorageGroupSelection selection_;
  std::vector<Track> groups_;
  std::set<std::string> expanded_;
  std::map<std::string, storage_management::GroupStorageMeasurement> measurements_;
  storage_management::StorageInventoryTask inventoryTask_;
  std::optional<storage_management::StorageInventory> preflight_;
  storage_management::StoragePolicy policy_;
  storage_management::StorageTaskSnapshot shownTask_;
  bool measuring_ = false, measuringSelection_ = false;
  bool sortBySize_ = false, returnWhenFinished_ = false;
  std::uint64_t libraryRevision_ = 0, measurementRevision_ = 0;
  View *root_ = nullptr, *body_ = nullptr, *browser_ = nullptr;
  ScrollView *controlsScroll_ = nullptr;
  StoragePolicyView *policyView_ = nullptr;
  RecyclerView<Track> *list_ = nullptr;
  TextInputBox *search_ = nullptr;
  TextView *selectionText_ = nullptr, *statusText_ = nullptr, *preflightText_ = nullptr;
  TextView *reportText_ = nullptr, *applyText_ = nullptr, *backText_ = nullptr, *sortText_ = nullptr;
  Button *apply_ = nullptr, *cancel_ = nullptr;
  int width_ = -1, height_ = -1;
};
