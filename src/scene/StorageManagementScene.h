#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../library/StorageArchiveList.h"
#include "../storage/StorageInventoryTask.h"
#include "../storage/StorageTask.h"
#include "../storage/StorageMeasurementState.h"

class Button;
class OverlayPortal;
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
  void ReloadGroups(bool invalidateAll = false);
  void RefreshArchiveSizes();
  void MeasureGroups(const std::set<std::string> &);
  void ApplyReport(const storage_management::StorageReport &);
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
  StorageGroupArchives groupArchives_;
  std::map<std::filesystem::path, std::optional<std::uint64_t>> archiveSizes_;
  std::vector<Track> groups_;
  std::set<std::string> expanded_;
  std::set<std::filesystem::path> expandedArchives_;
  std::map<std::string, storage_management::GroupStorageMeasurement> measurements_;
  storage_management::StorageInventoryTask inventoryTask_;
  std::optional<storage_management::StoragePreflight> preflight_;
  std::map<std::string, storage_management::StorageGroupSource> measurementSources_;
  std::set<std::string> pendingMeasurements_;
  std::vector<std::filesystem::path> pathAccessRoots_;
  std::shared_ptr<const storage_management::StorageReport> appliedReport_;
  storage_management::StoragePolicy policy_;
  storage_management::StorageTaskSnapshot shownTask_;
  bool measuring_ = false, measuringSelection_ = false;
  bool sortBySize_ = false, returnWhenFinished_ = false;
  std::uint64_t libraryRevision_ = 0, measurementRevision_ = 0;
  View *root_ = nullptr, *body_ = nullptr, *browser_ = nullptr;
  ScrollView *controlsScroll_ = nullptr;
  StoragePolicyView *policyView_ = nullptr;
  OverlayPortal *overlayPortal_ = nullptr;
  RecyclerView<StorageArchiveRow> *list_ = nullptr;
  TextInputBox *search_ = nullptr;
  TextView *selectionText_ = nullptr, *statusText_ = nullptr, *preflightText_ = nullptr;
  TextView *archiveHint_ = nullptr;
  TextView *reportText_ = nullptr, *applyText_ = nullptr, *backText_ = nullptr, *sortText_ = nullptr;
  Button *apply_ = nullptr, *cancel_ = nullptr, *back_ = nullptr;
  int width_ = -1, height_ = -1;
};
