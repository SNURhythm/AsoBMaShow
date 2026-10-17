#include "StorageManagementScene.h"
#include "SceneManager.h"
#include "../ArchiveFile.h"
#include "../storage/StorageApplicationIntegration.h"
#include "../path.h"
#include "../rendering/common.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/MusicTrackRowView.h"
#include "../view/RecyclerView.h"
#include "../view/ScrollView.h"
#include "../view/StoragePolicyView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif
#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace storage_management;
namespace {
TextView *text(const std::string &value, int size = 22) {
  auto *view = new TextView(app_theme::font, size);
  view->setText(value);
  view->setWrap(true);
  view->setThemedColor(app_theme::textPrimary);
  view->setVAlign(TextView::MIDDLE);
  return view;
}
Button *button(const char *key, std::function<void()> click, TextView **label = nullptr) {
  auto *result = new Button();
  result->setHeight(66)->setMinWidth(130)->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)
      ->setCornerRadius(app_theme::controlRadius());
  result->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                   app_theme::controlPressed);
  auto *content = text(i18n::tr(key));
  content->setLocalizedText(i18n::message(key));
  content->setAlign(TextView::CENTER);
  result->setContentView(content);
  result->setOnClickListener(std::move(click));
  if (label) *label = content;
  return result;
}
std::string bytes(std::uint64_t value) {
  std::ostringstream out;
  if (value < 1024) out << value << " B";
  else if (value < 1024 * 1024) out << std::fixed << std::setprecision(1) << value / 1024.0 << " KiB";
  else if (value < 1024ULL * 1024 * 1024) out << std::fixed << std::setprecision(1) << value / (1024.0 * 1024) << " MiB";
  else out << std::fixed << std::setprecision(2) << value / (1024.0 * 1024 * 1024) << " GiB";
  return out.str();
}
std::string reason(StorageReason code) {
  switch (code) {
  case StorageReason::None: return {};
  case StorageReason::PolicyKeep: return i18n::tr("storage.ui.keep");
  case StorageReason::UnsupportedSource: return i18n::tr("storage.ui.local_required");
  case StorageReason::UnsupportedFormat: return i18n::tr("storage.ui.unsupported_format");
  case StorageReason::CodecUnavailable: return i18n::tr("storage.ui.codec_unavailable");
  case StorageReason::UnsafePath: return i18n::tr("storage.ui.unsafe_path");
  case StorageReason::IncompleteInventory: return i18n::tr("storage.ui.unavailable");
  case StorageReason::Collision: return i18n::tr("storage.ui.collision");
  case StorageReason::StaleInput: return i18n::tr("storage.ui.stale");
  case StorageReason::NotSmaller: return i18n::tr("storage.ui.not_smaller");
  case StorageReason::AlreadyOptimized: return i18n::tr("storage.ui.already_optimized");
  case StorageReason::IoError: return i18n::tr("storage.ui.io_error");
  case StorageReason::ValidationFailed: return i18n::tr("storage.ui.validation_failed");
  case StorageReason::InsufficientSpace: return i18n::tr("storage.ui.insufficient_space");
  case StorageReason::CleanupFailed: return i18n::tr("storage.ui.cleanup_pending");
  case StorageReason::RecoveryRequired: return i18n::tr("storage.ui.recovery_required");
  case StorageReason::Cancelled: return i18n::tr("storage.ui.cancelled");
  }
  return {};
}
const char *phase(StoragePhase value) {
  switch (value) {
  case StoragePhase::Inventory: return "storage.ui.scanning";
  case StoragePhase::Encode: return "storage.ui.encoding";
  case StoragePhase::Verify: return "storage.ui.verifying";
  case StoragePhase::Commit: return "storage.ui.committing";
  case StoragePhase::Complete: return "storage.ui.complete";
  }
  return "storage.ui.scanning";
}
}
StorageManagementScene::StorageManagementScene(ApplicationContext &context, SceneReturnTarget target)
    : Scene(context), returnTarget_(std::move(target)) {}
StorageManagementScene::~StorageManagementScene() { inventoryTask_.Cancel(); }
void StorageManagementScene::init() {
  profileId_ = context.profileManager.activeProfile().id;
  policy_ = context.settings.storagePreferences.manual;
  BuildView();
  if (context.storageTask) appliedReport_ = context.storageTask->Snapshot().lastReport;
  ReloadGroups();
}
void StorageManagementScene::BuildView() {
  root_ = new View(0, 0, rendering::window_width, rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::All, 16)->setGap(10)->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(root_);
  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(12);
  auto *title = text(i18n::tr("storage.ui.title"), 34);
  title->setLocalizedText(i18n::message("storage.ui.title"));
  title->setFlex(1)->setMinHeight(66);
  header->addView(title);
  header->addView(button("library.tasks.back.label", [this] { GoBack(); }, &backText_));
  root_->addView(header);
  body_ = new View();
  body_->setFlex(1)->setMinHeight(0)->setGap(16)->setAlignItems(YGAlignStretch);
  root_->addView(body_);
  browser_ = new View();
  browser_->setFlex(1)->setMinWidth(0)->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(8);
  auto *searchLabel = text(i18n::tr("storage.ui.search"));
  searchLabel->setLocalizedText(i18n::message("storage.ui.search"));
  browser_->addView(searchLabel);
  search_ = new TextInputBox(app_theme::font, 20);
  search_->setHeight(66)->setThemedBackgroundColor(app_theme::insetSurface);
  search_->setThemedColor(app_theme::textPrimary);
  search_->setClearable(true);
  search_->onTextChanged([this](const std::string &query) {
    model_.SetQuery(query); RefreshRows();
  });
  browser_->addView(search_);
  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row)->setFlexWrap(YGWrapWrap)->setGap(6);
  actions->addView(button("storage.ui.select_filtered", [this] {
    if (shownTask_.running) return;
    for (const auto &row : model_.Rows())
      if (!selection_.Contains(row.groupId)) selection_.Toggle(row);
    InvalidatePreflight(); RefreshRows();
  }));
  actions->addView(button("storage.ui.clear", [this] {
    if (shownTask_.running) return;
    selection_.Clear(); InvalidatePreflight(); RefreshRows();
  }));
  actions->addView(button("storage.ui.sort_title", [this] {
    sortBySize_ = !sortBySize_; RefreshRows();
  }, &sortText_));
  actions->addView(button("storage.ui.refresh", [this] {
    if (!shownTask_.running) ReloadGroups(true);
  }));
  browser_->addView(actions);
  selectionText_ = text("");
  browser_->addView(selectionText_);
  list_ = new RecyclerView<Track>(music_playlist::SameTrackIdentity);
  list_->setFlex(1)->setMinHeight(100);
  list_->itemHeight = 82;
  list_->onCreateView = [](const Track &) {
    auto *row = new MusicTrackRowView();
    row->setHeight(82);
    row->setTrailingActionSize(66);
    return row;
  };
  list_->onBind = [this](View *view, const Track &track, int, bool) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view))
      row->setTrack(track, false,
          {selection_.Contains(track.groupId) ? "☑" : "☐", selection_.Contains(track.groupId),
           [this, track] { ToggleGroup(track); }},
          trackDetail(track) + " · " + MeasurementText(track));
  };
  list_->onSelectedByPointer = [this](const Track &track, int) { ToggleExpanded(track); };
  browser_->addView(list_);
  body_->addView(browser_);
  controlsScroll_ = new ScrollView();
  auto *controls = new View();
  controls->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(12);
  policyView_ = new StoragePolicyView(policy_, [this](StoragePolicy policy) {
    if (shownTask_.running) return;
    policy_ = policy;
    context.settings.storagePreferences.manual = policy;
    if (!context.saveSettings()) SDL_Log("Failed to save storage policy");
    InvalidatePreflight(); RefreshState();
  });
  controls->addView(policyView_);
  auto *help = text(i18n::tr("storage.ui.logical_bytes"), 20);
  help->setLocalizedText(i18n::message("storage.ui.logical_bytes"));
  help->setThemedColor(app_theme::textSecondary);
  controls->addView(help);
  preflightText_ = text("");
  controls->addView(preflightText_);
  apply_ = button("storage.ui.review", [this] { ReviewOrApply(); }, &applyText_);
  controls->addView(apply_);
  cancel_ = button("storage.ui.cancel", [this] {
    if (context.storageTask && shownTask_.running) context.storageTask->RequestCancel();
    else { inventoryTask_.Cancel(); measuring_ = false; pendingMeasurements_.clear(); InvalidatePreflight(); RefreshState(); }
  });
  controls->addView(cancel_);
  reportText_ = text("", 20);
  controls->addView(reportText_);
  controlsScroll_->setContentView(controls);
  body_->addView(controlsScroll_);
  statusText_ = text("", 20);
  root_->addView(statusText_);
  Layout();
}
void StorageManagementScene::Layout() {
  width_ = rendering::window_width; height_ = rendering::window_height;
  const bool compact = width_ < 850;
  root_->setSize(width_, height_);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto safe = GetIOSSafeAreaInsetsNormalized();
  root_->setPadding(Edge::Top, 16 + safe.top * height_)
      ->setPadding(Edge::Bottom, 16 + safe.bottom * height_)
      ->setPadding(Edge::Left, 16 + safe.left * width_)
      ->setPadding(Edge::Right, 16 + safe.right * width_);
#endif
  body_->setFlexDirection(compact ? FlexDirection::Column : FlexDirection::Row);
  controlsScroll_->setWidth(compact ? YGUndefined : 440);
  controlsScroll_->setHeight(compact ? std::min(300, height_ / 3) : YGUndefined);
  controlsScroll_->setFlexShrink(0);
  root_->applyYogaLayout();
  controlsScroll_->refreshContentLayout();
}
InventoryRequest StorageManagementScene::RequestFor(const std::vector<Track> &tracks) const {
  InventoryRequest request;
  request.policy = policy_;
  request.pathAccessRoots = pathAccessRoots_;
  request.scanScratchRoot = StoragePrivateRoot(context) / "storage-management/scan";
  for (const auto &track : tracks) {
    const auto &meta = track.representativeChart;
    const bool fallback = meta.Folder.empty();
    const auto path = fallback ? meta.BmsPath : meta.Folder;
    const auto name = fspath_to_utf8(path);
    const bool local = path.is_absolute() && !archive_file::isVirtualPath(path) &&
        !name.starts_with("@androidtree@") && !name.starts_with("content:");
    request.groups.push_back({track.groupId, path, local, fallback, local ? StorageSourceKind::Local :
        archive_file::isVirtualPath(path) ? StorageSourceKind::Archive : StorageSourceKind::Unsupported});
  }
  return request;
}
void StorageManagementScene::ReloadGroups(bool invalidateAll) {
  inventoryTask_.Cancel();
  preflight_.reset(); measuring_ = false;
  const auto changes = context.chartRepository.GetLibraryChangesSince(libraryRevision_);
  libraryRevision_ = changes.revision;
  measurementRevision_ = context.storageMeasurementRevision;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  pathAccessRoots_.clear();
  if (auto session = context.chartRepository.OpenSession()) {
    for (const auto &entry : session->SelectAllEntries()) {
      const auto root = chart_library_platform::resolveFolderEntryPath(entry);
      if (root.is_absolute()) pathAccessRoots_.push_back(root.lexically_normal());
    }
  }
#endif
  groups_ = music_playlist::MakeTracks(context.musicPlaylistRepository.SelectLibraryTracks());
  const auto request = RequestFor(groups_);
  std::vector<StorageGroupSource> sources;
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    const auto &group = groups_[i]; const auto &meta = group.representativeChart;
    sources.push_back({request.groups[i], std::to_string(group.chartCount) + ':' + meta.MD5 + ':' + meta.SHA256 + ':' +
        fspath_to_utf8(meta.BmsPath)});
  }
  const auto changed = ReconcileStorageMeasurements(measurements_, measurementSources_, sources, invalidateAll || changes.unknown, changes.roots);
  std::erase_if(pendingMeasurements_, [&](const auto &id) { return measurements_.contains(id) || !measurementSources_.contains(id); });
  pendingMeasurements_.insert(changed.begin(), changed.end());
  selection_.SetGroups(groups_);
  model_ = GroupedChartListModel{};
  model_.SetGroups(groups_);
  model_.SetQuery(search_->getText());
  std::vector<bms_parser::ChartMeta> expandedGroups;
  std::vector<std::string> expandedIds;
  for (const auto &group : groups_) if (expanded_.contains(group.groupId)) {
    expandedGroups.push_back(group.representativeChart); expandedIds.push_back(group.groupId);
  }
  const auto expandedTracks = context.musicPlaylistRepository.SelectLibraryGroups(expandedGroups);
  for (std::size_t i = 0; i < expandedIds.size(); ++i) {
    auto children = music_playlist::MakeTracks(expandedTracks[i]);
    for (auto &child : children) { child.groupId = expandedIds[i]; child.expandedChart = true; child.groupRepresentative = false; }
    model_.SetChildren(expandedIds[i], std::move(children)); model_.SetExpanded(expandedIds[i], true);
  }
  RefreshRows();
  MeasureGroups(pendingMeasurements_);
}
void StorageManagementScene::MeasureGroups(const std::set<std::string> &ids) {
  std::vector<Track> groups;
  for (const auto &group : groups_) if (ids.contains(group.groupId)) groups.push_back(group);
  if (groups.empty()) return;
  measuring_ = true; measuringSelection_ = false;
  inventoryTask_.StartMeasurements(RequestFor(groups));
  RefreshState();
}
void StorageManagementScene::Measure(bool selectedOnly) {
  measuring_ = true; measuringSelection_ = selectedOnly;
  pendingMeasurements_.clear();
  if (selectedOnly) inventoryTask_.StartPreflight(RequestFor(selection_.SelectedGroups()));
  else {
    for (const auto &group : groups_) pendingMeasurements_.insert(group.groupId);
    inventoryTask_.StartMeasurements(RequestFor(groups_));
  }
  RefreshState();
}
void StorageManagementScene::ApplyReport(const StorageReport &report) {
  preflight_.reset();
  inventoryTask_.Cancel(); measuring_ = false;
  pendingMeasurements_.clear();
  const auto changed = ApplyStorageReportMeasurements(measurements_, RequestFor(groups_).groups, report);
  pendingMeasurements_.insert(changed.begin(), changed.end());
  RefreshRows(); MeasureGroups(pendingMeasurements_);
}
void StorageManagementScene::InvalidatePreflight() {
  preflight_.reset();
  if (measuring_ && measuringSelection_) { inventoryTask_.Cancel(); measuring_ = false; }
}
void StorageManagementScene::ToggleGroup(const Track &track) {
  if (shownTask_.running) return;
  selection_.Toggle(track); InvalidatePreflight(); RefreshRows();
}
void StorageManagementScene::ToggleExpanded(const Track &track) {
  if (expanded_.erase(track.groupId)) model_.SetExpanded(track.groupId, false);
  else {
    const auto found = std::find_if(groups_.begin(), groups_.end(),
        [&](const auto &group) { return group.groupId == track.groupId; });
    if (found == groups_.end() || found->chartCount <= 1) return;
    auto children = music_playlist::MakeTracks(
        context.musicPlaylistRepository.SelectLibraryGroupTracks(found->representativeChart));
    for (auto &child : children) { child.groupId = track.groupId; child.expandedChart = true; child.groupRepresentative = false; }
    model_.SetChildren(track.groupId, std::move(children));
    expanded_.insert(track.groupId);
    model_.SetExpanded(track.groupId, true);
  }
  RefreshRows();
}
std::string StorageManagementScene::MeasurementText(const Track &track) const {
  const auto found = measurements_.find(track.groupId);
  if (found == measurements_.end())
    return i18n::tr(measuring_ ? "storage.ui.pending" : "storage.ui.unavailable");
  const auto &value = found->second;
  if (!value.totalBytes) return reason(value.reason.code).empty() ? i18n::tr("storage.ui.unavailable") : reason(value.reason.code);
  const auto prefix = value.sourceKind == StorageSourceKind::Archive
      ? std::string(i18n::tr("storage.ui.archive_uncompressed")) + " · " : std::string{};
  return prefix + bytes(*value.totalBytes) + " · " + i18n::tr("storage.ui.audio") + " " + bytes(value.audioBytes) +
      " · " + i18n::tr("storage.ui.video") + " " + bytes(value.videoBytes) +
      " · " + i18n::tr("storage.ui.images") + " " + bytes(value.imageBytes);
}
void StorageManagementScene::RefreshRows() {
  std::stable_sort(groups_.begin(), groups_.end(), [this](const Track &a, const Track &b) {
    if (sortBySize_) {
      const auto x = measurements_.find(a.groupId), y = measurements_.find(b.groupId);
      const auto ax = x == measurements_.end() ? std::optional<std::uint64_t>{} : x->second.totalBytes;
      const auto by = y == measurements_.end() ? std::optional<std::uint64_t>{} : y->second.totalBytes;
      if (ax != by) return ax > by;
    }
    return trackTitle(a) < trackTitle(b);
  });
  model_.SetGroups(groups_);
  const float scroll = list_->scrollOffset;
  list_->setItems(model_.Rows());
  list_->scrollOffset = std::clamp(scroll, 0.0f, std::max(0.0f,
      static_cast<float>(list_->size() * list_->itemHeight - list_->getContentHeight())));
  list_->rebindVisibleItems();
  RefreshState();
}
void StorageManagementScene::ReviewOrApply() {
  if (shownTask_.running || (measuring_ && measuringSelection_) || selection_.Count() == 0 || IsNoOp(policy_)) return;
  if (!preflight_) { Measure(true); return; }
  if (!preflight_->complete || preflight_->eligibleCount == 0 || !context.storageTask) return;
  auto request = OptimizationRequest{.inventory = preflight_->request, .policy = policy_, .preflight = std::move(preflight_)};
  for (const auto &[id, row] : measurements_) request.measurementBaselines.push_back(row);
  preflight_.reset();
  inventoryTask_.Cancel();
  (void)context.storageTask->Start(std::move(request));
  shownTask_ = context.storageTask->Snapshot();
  RefreshState();
}
void StorageManagementScene::RefreshState() {
  if (!root_) return;
  shownTask_ = context.storageTask ? context.storageTask->Snapshot() : StorageTaskSnapshot{};
  selectionText_->setText(i18n::format("storage.ui.selected", {
      {"count", std::to_string(selection_.Count())},
      {"hidden", std::to_string(selection_.HiddenCount(model_.Rows()))}}));
  sortText_->setText(i18n::tr(sortBySize_ ? "storage.ui.sort_size" : "storage.ui.sort_title"));
  backText_->setText(i18n::tr(shownTask_.running ? "storage.ui.cancel_back" : "library.tasks.back.label"));
  policyView_->SetEnabled(!shownTask_.running && !returnWhenFinished_);
  apply_->setEnabled(context.storageTask && !shownTask_.running && !(measuring_ && measuringSelection_) &&
      !shownTask_.recoveryRequired && !IsNoOp(policy_) && selection_.Count() > 0 &&
      (!preflight_ || (preflight_->complete && preflight_->eligibleCount != 0)));
  applyText_->setText(i18n::tr(preflight_ ? "storage.ui.confirm_apply" : "storage.ui.review"));
  cancel_->setEnabled(shownTask_.running || measuring_);
  if (preflight_) {
    auto summary = i18n::format("storage.ui.preflight", {{"count", std::to_string(preflight_->eligibleCount)},
        {"bytes", bytes(preflight_->eligibleBytes)}, {"skipped", std::to_string(preflight_->skippedCount)}});
    for (const auto &group : preflight_->groups)
      if (!group.totalBytes) summary += "\n" + fspath_to_utf8(group.root) + ": " + reason(group.reason.code);
    summary += std::string("\n\n") + i18n::tr("storage.ui.confirm_warning");
    preflightText_->setText(summary);
  } else preflightText_->setText(i18n::tr("storage.ui.folder_scope"));
  std::string status;
  if (shownTask_.recoveryRequired) status = i18n::tr("storage.ui.recovery_required");
  else if (shownTask_.running) {
    status = std::string(i18n::tr(shownTask_.cancelling ? "storage.ui.cancelling" : phase(shownTask_.progress.phase))) +
        " · " + std::to_string(shownTask_.progress.filesDone) + "/" + std::to_string(shownTask_.progress.filesTotal) +
        " · " + bytes(shownTask_.progress.reclaimedBytes) + "\n" + fspath_to_utf8(shownTask_.progress.currentPath.filename());
  } else if (measuring_) status = i18n::tr("storage.ui.scanning");
  else if (!shownTask_.admissionError.empty()) status = std::string(i18n::tr("storage.ui.busy")) + " " + shownTask_.admissionError;
  else if (shownTask_.lastReport) status = i18n::tr(shownTask_.lastReport->cancelled ? "storage.ui.cancelled" : "storage.ui.complete");
  else if (groups_.empty()) status = i18n::tr("storage.ui.empty");
  statusText_->setText(status);
  if (const auto &report = shownTask_.lastReport) {
    auto value = i18n::format("storage.ui.report", {{"converted", std::to_string(report->converted)},
        {"deleted", std::to_string(report->deleted)}, {"skipped", std::to_string(report->skipped)},
        {"failed", std::to_string(report->failed)}, {"bytes", bytes(report->reclaimedBytes)}});
    if (report->cleanupPending) value += std::string("\n") + i18n::tr("storage.ui.cleanup_pending");
    for (const auto &file : report->files)
      if (file.reason.code != StorageReason::None)
        value += "\n" + fspath_to_utf8(file.source.filename()) + ": " + reason(file.reason.code);
    reportText_->setText(value);
  }
  root_->applyYogaLayout();
  controlsScroll_->refreshContentLayout();
}
void StorageManagementScene::GoBack() {
  if (context.storageTask && context.storageTask->Snapshot().running) {
    returnWhenFinished_ = true;
    context.storageTask->RequestCancel();
    RefreshState();
    return;
  }
  inventoryTask_.Cancel();
  defer([this] { (void)returnToScene(*context.sceneManager, returnTarget_); return false; }, 0, true);
}
void StorageManagementScene::update(float) {
  if (!root_) return;
  const auto task = context.storageTask ? context.storageTask->Snapshot() : StorageTaskSnapshot{};
  if (returnWhenFinished_ && !task.running) { returnWhenFinished_ = false; GoBack(); return; }
  if (profileId_ != context.profileManager.activeProfile().id) {
    profileId_ = context.profileManager.activeProfile().id;
    policy_ = context.settings.storagePreferences.manual;
    policyView_->SetPolicy(policy_);
    selection_.Clear(); expanded_.clear(); ReloadGroups();
  }
  if (!task.running && task.lastReport != appliedReport_) {
    appliedReport_ = task.lastReport;
    measurementRevision_ = context.storageMeasurementRevision;
    if (appliedReport_) ApplyReport(*appliedReport_);
  }
  if (!task.running && libraryRevision_ != context.chartRepository.GetLibraryRevision()) ReloadGroups();
  measurementRevision_ = context.storageMeasurementRevision;
  bool rowsChanged = false;
  const auto publish = [&](std::vector<GroupStorageMeasurement> rows) {
    for (auto &group : rows) {
      pendingMeasurements_.erase(group.groupId);
      measurements_[group.groupId] = std::move(group); rowsChanged = true;
    }
  };
  publish(inventoryTask_.TakeMeasurements());
  if (auto result = inventoryTask_.TakeResult()) {
    publish(inventoryTask_.TakeMeasurements()); // Completion flushes the last throttled batch.
    measuring_ = false; pendingMeasurements_.clear();
    if (result->mode == StorageInventoryMode::Preflight) preflight_ = std::move(result->preflight);
    rowsChanged = true;
  }
  if (rowsChanged) RefreshRows();
  if (task.running != shownTask_.running || task.cancelling != shownTask_.cancelling ||
      task.lastReport != shownTask_.lastReport || task.progress.phase != shownTask_.progress.phase ||
      task.progress.filesDone != shownTask_.progress.filesDone || task.progress.filesTotal != shownTask_.progress.filesTotal ||
      task.progress.reclaimedBytes != shownTask_.progress.reclaimedBytes ||
      task.progress.currentPath != shownTask_.progress.currentPath)
    RefreshState();
  if (width_ != rendering::window_width || height_ != rendering::window_height) Layout();
}
void StorageManagementScene::onLanguageChanged() { Scene::onLanguageChanged(); RefreshRows(); }
void StorageManagementScene::onApplicationBackgroundChanged(bool background) {
  if (background) { inventoryTask_.Cancel(); measuring_ = false; InvalidatePreflight(); }
  else if (!shownTask_.running) {
    for (const auto &group : groups_) if (!measurements_.contains(group.groupId)) pendingMeasurements_.insert(group.groupId);
    MeasureGroups(pendingMeasurements_);
  }
}
bool StorageManagementScene::prepareForStorageMaintenance(std::string &) {
  inventoryTask_.Cancel(); measuring_ = false;
  return true;
}
EventHandleResult StorageManagementScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) { GoBack(); return {}; }
  return Scene::handleEvents(event);
}
void StorageManagementScene::cleanupScene() {
  inventoryTask_.Cancel();
  if (list_) { list_->onBind = {}; list_->onCreateView = {}; list_->onSelectedByPointer = {}; }
  root_ = body_ = browser_ = nullptr;
  controlsScroll_ = nullptr; policyView_ = nullptr; list_ = nullptr; search_ = nullptr;
  selectionText_ = statusText_ = preflightText_ = reportText_ = applyText_ = backText_ = sortText_ = nullptr;
  apply_ = cancel_ = nullptr;
}
