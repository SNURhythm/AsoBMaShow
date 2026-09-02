#include "MusicSelectScene.h"

#include "../PlatformOpen.h"

#if defined(__ANDROID__)
#include "../AndroidNatives.h"
#endif

#include "IrUploadsScene.h"
#include "LibraryTasksScene.h"
#include "MusicPlayerScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "CourseGameplaySessionBuilder.h"
#include "../ArchiveFile.h"
#include "../AssistOptionUtils.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../audio/Jukebox.h"
#include "../music_select/MusicSelectRepositoryProjection.h"
#include "../music_select/MusicSelectReplaySlots.h"
#include "../music_select/MusicSelectFavorites.h"
#include "../music_select/MusicSelectExternalActions.h"
#include "../music_select/MusicSelectLaunchPolicy.h"
#include "../music_select/MusicSelectPropertyProjection.h"
#include "../replay/ChartReplayConsumer.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "play/GamePlayScene.h"
#include "MainMenuProfileSelections.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../skin/beatoraja/LuaSkinApplicationAudioBackend.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr std::int64_t kRankingDurationMillis = 5'000;
constexpr std::int64_t kRankingReloadDurationMillis = 10 * 60 * 1'000;

std::int64_t unixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

long long steadyMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::optional<int>
numericSelector(const skin::SkinBuiltinPropertySelector &selector) {
  if (const auto *id = std::get_if<int>(&selector.value)) return *id;
  return std::nullopt;
}

std::string selectorName(
    const skin::SkinBuiltinPropertySelector &selector) {
  if (const auto *name = std::get_if<std::string>(&selector.value)) {
    return *name;
  }
  return {};
}

TextView *makeText(std::string value, int size,
                   View::ThemeColorProvider color) {
  auto *result = new TextView(kFontPath, size);
  result->setText(std::move(value));
  result->setThemedColor(std::move(color));
  result->setWrap(true);
  result->setVAlign(TextView::MIDDLE);
  return result;
}

Button *makeButton(std::string label) {
  auto *result = new Button();
  result->setWidth(180)->setHeight(56)->setCornerRadius(
      ui_theme::controlRadius());
  result->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  result->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  result->setStyledBorderWidth(1);
  auto *labelView = makeText(std::move(label), 21, ui_theme::textPrimary);
  labelView->setAlign(TextView::CENTER);
  result->setContentView(labelView);
  return result;
}

std::filesystem::path resolveChartAsset(const ChartMetaRecord &record,
                                        const std::filesystem::path &asset) {
  if (asset.empty() || asset.is_absolute()) return asset;
  return record.meta.BmsPath.parent_path() / asset;
}

int sourceSortIndex(std::string_view id) {
  // BarSorter.defaultSorter in the pinned source.
  constexpr std::array<std::string_view, 8> values{
      "TITLE", "ARTIST", "BPM", "LENGTH",
      "LEVEL", "CLEAR", "SCORE", "MISSCOUNT"};
  const auto found = std::ranges::find(values, id);
  return found == values.end()
             ? 0
             : static_cast<int>(std::distance(values.begin(), found));
}

MusicSelectClockFields localClockFields(std::time_t value) {
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &value);
#else
  localtime_r(&value, &local);
#endif
  return {.year = local.tm_year + 1900,
          .month = local.tm_mon + 1,
          .day = local.tm_mday,
          .hour = local.tm_hour,
          .minute = local.tm_min,
          .second = local.tm_sec};
}

std::optional<MusicSelectPreviewSelection>
previewSelection(const MusicSelectBarManagerSnapshot &snapshot,
                 bool archivePreviewEnabled) {
  if (snapshot.selectedIndex >= snapshot.rows.size()) return std::nullopt;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart) {
    return std::nullopt;
  }
  const auto &meta = selected.chart->meta;
  bool archiveVirtualPath = archive_file::isVirtualPath(meta.BmsPath);
#if TARGET_OS_ANDROID
  if (archiveVirtualPath && IsAndroidTreePath(meta.BmsPath)) {
    archiveVirtualPath = false;
  }
#endif
  const bool suppressArchivePreview =
      archiveVirtualPath && !archivePreviewEnabled;
  return MusicSelectPreviewSelection{
      .id = selected.id.value,
      .folder = meta.BmsPath.parent_path(),
      .previewPath = meta.Preview.empty() || suppressArchivePreview
                         ? std::filesystem::path{}
                         : meta.BmsPath.parent_path() / meta.Preview,
  };
}

std::optional<std::vector<std::filesystem::path>>
materializedArchiveDocuments(const std::filesystem::path &chartPath) {
  std::filesystem::path archivePath;
  std::filesystem::path chartInnerPath;
  if (!archive_file::splitVirtualPath(chartPath, archivePath,
                                      chartInnerPath)) {
    return std::nullopt;
  }
  std::vector<archive_file::Entry> entries;
  std::string error;
  if (!archive_file::listEntries(archivePath, entries, &error)) {
    SDL_Log("Failed to enumerate selector archive documents: %s",
            error.c_str());
    return std::vector<std::filesystem::path>{};
  }
  std::vector<std::filesystem::path> documents;
  const auto folder = chartInnerPath.parent_path();
  for (const auto &entry : entries) {
    if (entry.directory || entry.path.parent_path() != folder) continue;
    std::string extension = entry.path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) {
                             return static_cast<char>(std::tolower(value));
                           });
    if (extension != ".txt") continue;
    const auto virtualPath =
        archive_file::makeVirtualPath(archivePath, entry.path);
    if (auto materialized = archive_file::materializeFile(virtualPath, &error)) {
      documents.push_back(std::move(*materialized));
    } else {
      SDL_Log("Failed to materialize selector archive document: %s",
              error.c_str());
    }
  }
  return documents;
}

std::optional<MusicSelectControlKey> controlKey(SDL_Keycode key) {
  switch (key) {
  case SDLK_0: return MusicSelectControlKey::Num0;
  case SDLK_1: return MusicSelectControlKey::Num1;
  case SDLK_2: return MusicSelectControlKey::Num2;
  case SDLK_3: return MusicSelectControlKey::Num3;
  case SDLK_4: return MusicSelectControlKey::Num4;
  case SDLK_5: return MusicSelectControlKey::Num5;
  case SDLK_7: return MusicSelectControlKey::Num7;
  case SDLK_8: return MusicSelectControlKey::Num8;
  case SDLK_9: return MusicSelectControlKey::Num9;
  case SDLK_UP: return MusicSelectControlKey::Up;
  case SDLK_DOWN: return MusicSelectControlKey::Down;
  case SDLK_LEFT: return MusicSelectControlKey::Left;
  case SDLK_RIGHT: return MusicSelectControlKey::Right;
  case SDLK_RETURN:
  case SDLK_KP_ENTER: return MusicSelectControlKey::Enter;
  case SDLK_ESCAPE: return MusicSelectControlKey::Escape;
  default: return std::nullopt;
  }
}

std::optional<MusicSelectCommandKey> commandKey(const SDL_KeyboardEvent &key) {
  const bool control = (key.keysym.mod & KMOD_CTRL) != 0;
  const bool shift = (key.keysym.mod & KMOD_SHIFT) != 0;
  switch (key.keysym.sym) {
  case SDLK_F2: return MusicSelectCommandKey::UpdateFolder;
  case SDLK_F3:
    if (control && shift) return MusicSelectCommandKey::CopySha256;
    if (control) return MusicSelectCommandKey::CopyMd5;
    return MusicSelectCommandKey::OpenExplorer;
  case SDLK_F8: return MusicSelectCommandKey::AddFavoriteSong;
  case SDLK_F9: return MusicSelectCommandKey::AddFavoriteChart;
  case SDLK_F10: return MusicSelectCommandKey::AutoplayFolder;
  case SDLK_F11: return MusicSelectCommandKey::OpenIr;
  default: return std::nullopt;
  }
}
} // namespace

MusicSelectScene::MusicSelectScene(
    ApplicationContext &context,
    skin::GameplaySkinActivationRequest activationRequest)
    : Scene(context), activationRequest_(std::move(activationRequest)),
      selectedSkinPath_(
          musicSelectSkinEntryPath(activationRequest_.activation.entry)) {}

void MusicSelectScene::init() {
  started_ = std::chrono::steady_clock::now();
  previewAudio_ = std::make_unique<MusicSelectPreviewAudioService>(
      context.jukebox.audioRuntime());
  sortIndex_ = sourceSortIndex(context.settings.skinSortId);
  inputProcessor_ = MusicSelectInputProcessor(
      {.layout = musicSelectKeyLayoutForConfig(
           context.settings.skinMusicSelectInput),
       .scrollDurationLowMillis =
           context.settings.skinMusicSelectScrollDurationLow,
       .scrollDurationHighMillis =
           context.settings.skinMusicSelectScrollDurationHigh,
       .analogTicksPerScroll =
           context.settings.skinMusicSelectAnalogTicksPerScroll});
  chartSession_ = context.chartRepository.OpenSession(&context.scoreRepository);
  reloadLibrary();
  selectedBarMoved();

  if (context.irHttpClient) {
    auto *application = &context;
    irExternalUrlService_ = std::make_unique<ir::IrExternalUrlService>(
        [application](const ir::IrExternalUrlRequest &request,
                      std::stop_token stopToken) {
          return ir::resolveFirstEnabledIrExternalUrl(
              request, stopToken,
              [application](std::string_view profileId,
                            std::string_view providerId) {
                return application->lookupActiveIrCredential(profileId,
                                                             providerId);
              },
              [application](std::string_view providerId,
                            const ir::IrProviderRuntimeConfig &runtime,
                            std::stop_token requestStop) {
                return application->irDrivers.fetchAuthenticatedAccount(
                    providerId, runtime, *application->irHttpClient,
                    requestStop);
              },
              [application](std::string_view providerId,
                            const ir::IrChartExternalUrlRequest &chart,
                            const ir::IrProviderRuntimeConfig &runtime,
                            std::stop_token requestStop) {
                return application->irDrivers.chartExternalUrl(
                    providerId, chart, runtime, *application->irHttpClient,
                    requestStop);
              },
              [application](std::string_view providerId,
                            const ir::IrCourseExternalUrlRequest &course,
                            const ir::IrProviderRuntimeConfig &runtime,
                            std::stop_token requestStop) {
                return application->irDrivers.courseExternalUrl(
                    providerId, course, runtime, *application->irHttpClient,
                    requestStop);
              });
        });
  }

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (!activateSkin(std::move(activationRequest_))) return;
#endif

  syncToolbar();
  buildSearchPrompt();
  startInputListening();
}

void MusicSelectScene::onPause() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinSession_) skinSession_->suspendAudio();
#endif
  stopInputListening();
  previewController_.reset();
  if (previewAudio_) previewAudio_->switchTo(std::nullopt);
  if (irExternalUrlGeneration_ != 0 && irExternalUrlService_) {
    irExternalUrlService_->close(irExternalUrlGeneration_);
  }
  irExternalUrlGeneration_ = 0;
}

void MusicSelectScene::onResume() {
  launching_ = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (reactivateSkinOnResume_) {
    reactivateSkinOnResume_ = false;
    if (!reactivateSkinAfterSettings()) return;
  }
  if (skinSession_) skinSession_->resumeAudio();
#endif
  syncToolbar();
  reloadLibrary();
  if (!failed_) {
    selectedBarMoved();
    startInputListening();
  }
}

void MusicSelectScene::reloadLibrary() {
  if (!chartSession_) return;
  const std::uint64_t loadedRevision =
      context.chartRepository.GetLibraryRevision();
  std::vector<ChartMetaRecord> records;
  ChartMetaQuery query;
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  chartSession_->QueryChartMeta(query, records);
  scoreCache_ = context.scoreRepository.LoadBestScores();
  clearRankCache_ = context.scoreRepository.LoadBestClearRanks();
  playerHistory_ = context.scoreRepository.LoadPlayerScoreHistory();
  recentScoreImprovements_ = context.scoreRepository.LoadRecentScoreImprovements(
      unixMillis() / 1'000);
  libraryRevision_ = loadedRevision;
  bars_.configure({.modeFilter = context.settings.skinModeFilterName,
                   .difficultyFilter =
                       context.settings.skinDifficultyFilterName,
                   .sortId = context.settings.skinSortId});
  const auto metadata = MusicSelectRepositoryProjection::loadMetadata(
      *chartSession_, query.selectedLongNoteMode);
  std::vector<MusicSelectSearchSource> searches;
  searches.reserve(searchHistory_.entries().size());
  for (const auto &text : searchHistory_.entries()) {
    ChartMetaQuery searchQuery;
    searchQuery.keyword = text;
    searchQuery.selectedLongNoteMode = query.selectedLongNoteMode;
    MusicSelectSearchSource source{.text = text};
    chartSession_->QueryChartMeta(searchQuery, source.records);
    searches.push_back(std::move(source));
  }
  bars_.refresh(MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [this](const bms_parser::ChartMeta &meta, int mode) {
         return scoreCache_.bestFor(meta, mode);
       },
       .replayExistsFor = [this](const ChartMetaRecord &record, int mode) {
         return musicSelectExistingChartReplaySlots(
             record, mode,
             context.replayRepository.GetResolvedProfileRoot());
       },
       .courseRankFor = [this](std::string_view courseKey, int courseId,
                               int mode) {
         return clearRankCache_.bestCourseRankFor(courseKey, courseId, mode);
       },
       .metadata = &metadata,
       .searches = searches,
       .recentScoreImprovements = &recentScoreImprovements_,
       .modeFilter = context.settings.skinModeFilterName,
       .selectedLongNoteMode = query.selectedLongNoteMode,
       .repositoryRevision = libraryRevision_}));
  syncResolvedFilters();
}

void MusicSelectScene::syncResolvedFilters() {
  const auto snapshot = bars_.snapshot();
  if (context.settings.skinModeFilterName == snapshot.resolvedModeFilter &&
      context.settings.skinDifficultyFilterName ==
          snapshot.resolvedDifficultyFilter) {
    return;
  }
  context.settings.skinModeFilterName = snapshot.resolvedModeFilter;
  context.settings.skinDifficultyFilterName =
      snapshot.resolvedDifficultyFilter;
  if (!context.saveSettings()) {
    SDL_Log("Unable to save music-select resolved filters");
  }
}

std::int64_t MusicSelectScene::elapsedMicros() const {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - started_)
      .count();
}

void MusicSelectScene::selectedBarMoved() {
  const auto snapshot = bars_.snapshot();
  // Pinned MusicSelector retains rankingOffset across bar changes; only its
  // ranking-position writer mutates that field.
  selectedReplay_ =
      snapshot.selectedIndex < snapshot.rows.size()
          ? musicSelectFirstExistingReplay(
                &snapshot.rows[snapshot.selectedIndex])
          : -1;
  songBarChangeMicros_ = elapsedMicros();

  const auto previewMove = previewController_.selectedBarMoved(
      previewSelection(snapshot, context.settings.archiveChartPreviewEnabled),
      songBarChangeMicros_);
  if (previewMove.stopAudio && previewAudio_) {
    previewAudio_->switchTo(std::nullopt);
  }

  if (rankingGeneration_ != 0 && context.irRankingService) {
    context.irRankingService->close(rankingGeneration_);
  }
  rankingGeneration_ = 0;
  rankingRevision_ = 0;
  rankingRequest_.reset();
  rankingCacheKey_.clear();
  rankingLoadAtMicros_ = -1;
  setRanking({});

  irAccountEvidenceRevision_ =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  if (!context.irRankingService || context.irAccountNameSnapshot().empty() ||
      snapshot.selectedIndex >= snapshot.rows.size()) {
    return;
  }
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart ||
      selected.chart->meta.BmsPath.empty()) {
    return;
  }

  const auto provider = std::ranges::find_if(
      context.settings.irProviders, [&](const auto &entry) {
        return entry.second.enabled && context.irDrivers.find(entry.first);
      });
  if (provider == context.settings.irProviders.end()) return;

  const auto &meta = selected.chart->meta;
  rankingRequest_ = ir::IrRankingRequest{
      .profileId = context.profileManager.activeProfile().id,
      .providerId = provider->first,
      .serverOrigin = provider->second.serverOrigin,
      .chart = {.keyMode = meta.KeyMode,
                .chartMd5 = meta.MD5,
                .chartSha256 = meta.SHA256,
                .totalNotes = meta.TotalNotes},
  };
  rankingCacheKey_ = rankingRequest_->profileId + "\n" +
                     rankingRequest_->providerId + "\n" +
                     rankingRequest_->serverOrigin + "\n" +
                     std::to_string(meta.KeyMode) + "\n" + meta.SHA256 +
                     "\n" + std::to_string(meta.TotalNotes);

  std::int64_t delayMillis = kRankingDurationMillis;
  if (const auto cached = rankingCache_.find(rankingCacheKey_);
      cached != rankingCache_.end()) {
    setRanking(cached->second.snapshot);
    delayMillis += std::max<std::int64_t>(
        kRankingReloadDurationMillis -
            (unixMillis() - cached->second.updatedUnixMillis),
        0);
  }
  rankingLoadAtMicros_ =
      songBarChangeMicros_ + delayMillis * 1'000;
}

void MusicSelectScene::setRanking(MusicSelectRankingSnapshot next) {
  if (ranking_.state != next.state) {
    rankingTimerMicros_.fill(std::nullopt);
    int timer = -1;
    switch (next.state) {
    case MusicSelectRankingState::Access: timer = 0; break;
    case MusicSelectRankingState::Finish: timer = 1; break;
    case MusicSelectRankingState::Fail: timer = 2; break;
    case MusicSelectRankingState::None: break;
    }
    if (timer >= 0) {
      rankingTimerMicros_[static_cast<std::size_t>(timer)] = elapsedMicros();
    }
  }
  ranking_ = std::move(next);
}

void MusicSelectScene::updateRanking() {
  if (!context.irRankingService || !rankingRequest_) return;
  const auto now = elapsedMicros();
  if (rankingLoadAtMicros_ != -1 && now > rankingLoadAtMicros_) {
    rankingLoadAtMicros_ = -1;
    rankingGeneration_ = context.irRankingService->open(*rankingRequest_);
    rankingRevision_ = 0;
  }
  if (rankingGeneration_ == 0) return;

  auto service = context.irRankingService->snapshot();
  if (service.generation != rankingGeneration_ ||
      service.revision == rankingRevision_) {
    return;
  }
  rankingRevision_ = service.revision;
  if (service.state == ir::IrRankingSnapshotState::Succeeded &&
      service.ranking && service.ranking->nextPageToken &&
      !service.loadingNextPage && !service.paginationBlocked) {
    if (context.irRankingService->loadNextPage(rankingGeneration_)) {
      service = context.irRankingService->snapshot();
      rankingRevision_ = service.revision;
    }
  }

  auto projected = projectMusicSelectRanking(service, rankingOffset_);
  projected.pendingDurationMillis = -1;
  setRanking(std::move(projected));
  if (ranking_.state == MusicSelectRankingState::Finish ||
      ranking_.state == MusicSelectRankingState::Fail) {
    rankingCache_[rankingCacheKey_] = {
        .snapshot = ranking_, .updatedUnixMillis = unixMillis()};
  }
}

void MusicSelectScene::setPanelState(int next) {
  if (panelState_ == next) return;
  const auto now = elapsedMicros();
  if (panelState_ > 0 && panelState_ <= 6) {
    const auto index = static_cast<std::size_t>(panelState_ - 1);
    panelOffMicros_[index] = now;
    panelOnMicros_[index].reset();
  }
  if (next > 0 && next <= 6) {
    const auto index = static_cast<std::size_t>(next - 1);
    panelOnMicros_[index] = now;
    panelOffMicros_[index].reset();
  }
  panelState_ = next;
}

skin::MusicSelectSkinFrame MusicSelectScene::makeFrame() const {
  const auto snapshot = bars_.snapshot();
  skin::MusicSelectSkinFrame frame;
  frame.serial = std::max<std::uint64_t>(1, frameSerial_);
  frame.elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_)
                            .count();
  frame.songList.selectedIndex = snapshot.selectedIndex;
  frame.songList.elapsedMillis = frame.elapsedMillis;
  const auto wall = unixMillis();
  frame.songList.wallClockMillis = wall;
  frame.songList.wallClockSeconds = wall / 1000;
  frame.songList.playerLnMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  frame.songList.movementDirection = snapshot.movementDirection;
  frame.songList.movementEndMillis = snapshot.movementEndMillis;
  frame.songList.bars.reserve(snapshot.rows.size());
  for (const auto &row : snapshot.rows) {
    frame.songList.bars.push_back(row.presentation);
  }

  const std::time_t wallSeconds = static_cast<std::time_t>(wall / 1000);
  MusicSelectPropertyRuntimeSnapshot propertyRuntime;
  propertyRuntime.wallClock = localClockFields(wallSeconds);
  propertyRuntime.applicationUptimeMillis =
      context.applicationUptimeMillis.load(std::memory_order_acquire);
  propertyRuntime.framesPerSecond =
      context.currentFramesPerSecond.load(std::memory_order_acquire);
  propertyRuntime.panelState = panelState_;
  propertyRuntime.selectedReplay = selectedReplay_;
  propertyRuntime.sortIndex = sortIndex_;
  propertyRuntime.playerName =
      context.profileManager.activeProfile().displayName;
  propertyRuntime.targetName = context.settings.skinTargetId == "MAX"
                                   ? "MAX"
                                   : std::string{};
  // Beatoraja property 30 is setter-only and always reads as an empty string.
  propertyRuntime.searchWord.clear();
  propertyRuntime.tableName = tableContext_.name;
  propertyRuntime.tableLevel = tableContext_.level;
  propertyRuntime.tableFullName = tableContext_.fullName;
  propertyRuntime.version = ASOBMASHOW_APPLICATION_VERSION;
  propertyRuntime.irName = context.settings.irProviders.empty()
                               ? std::string{}
                               : context.settings.irProviders.begin()->first;
  propertyRuntime.irUserName = context.irAccountNameSnapshot();
  propertyRuntime.irOnline = !propertyRuntime.irUserName.empty();
  propertyRuntime.ranking = ranking_;
  if (rankingLoadAtMicros_ != -1) {
    propertyRuntime.ranking.pendingDurationMillis =
        (rankingLoadAtMicros_ - elapsedMicros()) / 1'000;
  } else {
    propertyRuntime.ranking.pendingDurationMillis = -1;
  }
  propertyRuntime.playerHistory = playerHistory_;
  frame.properties =
      projectMusicSelectProperties(context.settings, snapshot, propertyRuntime);
  if (startInputMicros_) frame.properties.timers[1] = *startInputMicros_;
  frame.properties.timers[11] = songBarChangeMicros_;
  for (std::size_t index = 0; index < panelOnMicros_.size(); ++index) {
    if (panelOnMicros_[index]) {
      frame.properties.timers[21 + static_cast<int>(index)] =
          *panelOnMicros_[index];
    }
    if (panelOffMicros_[index]) {
      frame.properties.timers[31 + static_cast<int>(index)] =
          *panelOffMicros_[index];
    }
  }
  for (std::size_t index = 0; index < rankingTimerMicros_.size(); ++index) {
    if (rankingTimerMicros_[index]) {
      frame.properties.timers[172 + static_cast<int>(index)] =
          *rankingTimerMicros_[index];
    }
  }

  if (snapshot.selectedIndex < snapshot.rows.size()) {
    const auto &selected = snapshot.rows[snapshot.selectedIndex];
    if (selected.chart) {
      const auto &record = *selected.chart;
      const auto &meta = record.meta;
      frame.stageFile = resolveChartAsset(record, meta.StageFile);
      frame.backBmp = resolveChartAsset(record, meta.BackBmp);
      frame.banner = resolveChartAsset(record, meta.Banner);
    }
  }
  return frame;
}

EventHandleResult MusicSelectScene::handleEvents(SDL_Event &event) {
  if (failed_) return Scene::handleEvents(event);
  if (searchOverlay_ != nullptr && searchOverlay_->getVisible()) {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        event.key.keysym.sym == SDLK_ESCAPE) {
      hideSearchPrompt();
      return {};
    }
    (void)searchOverlay_->handleEvents(event);
    return {};
  }
  if (toolbar_ != nullptr && !toolbar_->handleEvents(event)) return {};

  if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
      event.key.repeat == 0) {
    const bool down = event.type == SDL_KEYDOWN;
    if (inputBindingAdapter_) {
      auto &logicalInput = inputBindingAdapter_->state();
      if (const auto key = controlKey(event.key.keysym.sym)) {
        if (down) {
          logicalInput.controlPressed.insert(*key);
          logicalInput.controlHeld.insert(*key);
        } else {
          logicalInput.controlHeld.erase(*key);
        }
      }
      if (down) {
        if (const auto command = commandKey(event.key)) {
          logicalInput.commands.insert(*command);
        }
      }
    }
    return {};
  }
  if (event.type == SDL_MOUSEWHEEL) {
    if (inputBindingAdapter_) {
      inputBindingAdapter_->state().wheel += event.wheel.y;
    }
    return {};
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  // Pinned MainController forwards selector mouse callbacks independently of
  // TIMER_STARTINPUT, matching the logical-input path in update().
  if (skinSession_ && event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.which != SDL_TOUCH_MOUSEID) {
    float x = 0.0F;
    float y = 0.0F;
    rendering::screenToUi(event.button.x * rendering::widthScale,
                          event.button.y * rendering::heightScale, x, y);
    const auto pointer = skinSession_->queuePointerDown(
        {.x = x, .y = y}, static_cast<int>(event.button.button) - 1,
        steadyMicros());
    if (pointer.closeDirectory) closeDirectory();
    if (pointer.selectIndex) {
      const auto snapshot = bars_.snapshot();
      if (*pointer.selectIndex < snapshot.rows.size()) {
        const auto &clicked = snapshot.rows[*pointer.selectIndex];
        if (skin::musicSelectIsDirectoryBarKind(clicked.kind)) {
          (void)bars_.open(clicked.id);
          syncResolvedFilters();
          selectedBarMoved();
        } else {
          (void)bars_.select(clicked.id);
          selectedBarMoved();
          launchSelected();
        }
      }
    }
    if (pointer.consumed) return {};
  }
#endif
  return {};
}

void MusicSelectScene::openSelected() {
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (skin::musicSelectIsDirectoryBarKind(selected.kind)) {
    (void)bars_.openSelected();
    syncResolvedFilters();
    selectedBarMoved();
  } else {
    launchSelected();
  }
}

void MusicSelectScene::openSameFolder() {
  if (!chartSession_) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart ||
      selected.chart->unavailable || selected.chart->meta.BmsPath.empty()) {
    return;
  }

  const auto folder = selected.chart->meta.Folder.empty()
                          ? selected.chart->meta.BmsPath.parent_path()
                          : selected.chart->meta.Folder;
  std::vector<ChartMetaRecord> records;
  ChartMetaQuery query;
  query.exactFolder = folder;
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  chartSession_->QueryChartMeta(query, records);
  auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [this](const bms_parser::ChartMeta &meta, int mode) {
         return scoreCache_.bestFor(meta, mode);
       },
       .replayExistsFor = [this](const ChartMetaRecord &record, int mode) {
         return musicSelectExistingChartReplaySlots(
             record, mode,
             context.replayRepository.GetResolvedProfileRoot());
       },
       .selectedLongNoteMode = query.selectedLongNoteMode,
       .repositoryRevision = libraryRevision_});
  std::vector<MusicSelectBar> children;
  std::vector<MusicSelectBarId> childIds;
  for (auto &bar : projection.bars) {
    if (bar.kind == skin::MusicSelectBarKind::Song) {
      childIds.push_back(bar.id);
      children.push_back(std::move(bar));
    }
  }
  MusicSelectBar directory{
      .id = {"same-folder:" + selected.id.value},
      .kind = skin::MusicSelectBarKind::SameFolder,
      .title = selected.title,
      .children = std::move(childIds),
      .presentation = {.kind = skin::MusicSelectBarKind::SameFolder,
                       .title = selected.title,
                       .exists = true},
  };
  if (bars_.openTransient(std::move(directory), std::move(children))) {
    syncResolvedFilters();
    selectedBarMoved();
  }
}

void MusicSelectScene::copySelectedHash(bool sha256) {
  const auto snapshot = bars_.snapshot();
  const MusicSelectBar *selected =
      snapshot.selectedIndex < snapshot.rows.size()
          ? &snapshot.rows[snapshot.selectedIndex]
          : nullptr;
  const std::string hash = musicSelectSelectedHash(selected, sha256);
  if (!hash.empty() && SDL_SetClipboardText(hash.c_str()) != 0) {
    SDL_Log("Unable to copy selected chart hash: %s", SDL_GetError());
  }
}

void MusicSelectScene::buildSearchPrompt() {
  searchOverlay_ = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  searchOverlay_->setPositionType(YGPositionTypeAbsolute);
  searchOverlay_->setPosition(Edge::Left, 0);
  searchOverlay_->setPosition(Edge::Top, 0);
  searchOverlay_->setZIndex(3000);
  searchOverlay_->setVisible(false);
  searchOverlay_->setFlexDirection(FlexDirection::Column);
  searchOverlay_->setAlignItems(YGAlignCenter);
  searchOverlay_->setJustifyContent(YGJustifyCenter);
  searchOverlay_->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(600)
      ->setHeight(250)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16)
      ->setPadding(Edge::All, 24)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(ui_theme::hairlineStrong)
      ->setBorderWidth(1);

  auto *title = makeText("Search", 28, ui_theme::textPrimary);
  title->setHeight(40);
  panel->addView(title);

  searchInput_ = new TextInputBox(kFontPath, 22);
  searchInput_->setHeight(58);
  searchInput_->setClearable(true);
  searchInput_->setThemedBackgroundColor(ui_theme::insetSurface);
  searchInput_->setThemedBorderColor(ui_theme::hairlineStrong);
  searchInput_->setBorderWidth(1);
  searchInput_->setCornerRadius(ui_theme::controlRadius());
  searchInput_->setThemedColor(ui_theme::textPrimary);
  searchInput_->setVAlign(TextView::MIDDLE);
  searchInput_->onSubmit([this](const std::string &text) {
    hideSearchPrompt();
    search(text);
  });
  panel->addView(searchInput_);

  auto *actions = new View();
  actions->setHeight(58)
      ->setFlexDirection(FlexDirection::Row)
      ->setJustifyContent(YGJustifyFlexEnd)
      ->setGap(12);
  auto *cancel = makeButton("Cancel");
  cancel->setOnClickListener([this] { hideSearchPrompt(); });
  actions->addView(cancel);
  auto *submit = makeButton("Search");
  submit->setOnClickListener([this] {
    const std::string text = searchInput_ ? searchInput_->getText() : "";
    hideSearchPrompt();
    search(text);
  });
  actions->addView(submit);
  panel->addView(actions);

  searchOverlay_->addView(panel);
  addView(searchOverlay_);
}

void MusicSelectScene::showSearchPrompt() {
  if (searchOverlay_ == nullptr || searchInput_ == nullptr) return;
  stopInputListening();
  searchInput_->setEditingText("");
  searchOverlay_->setSize(rendering::window_width, rendering::window_height);
  searchOverlay_->setVisible(true);
  searchOverlay_->applyYogaLayout();
  searchInput_->beginEditing();
}

void MusicSelectScene::hideSearchPrompt() {
  if (searchOverlay_ == nullptr || !searchOverlay_->getVisible()) return;
  if (searchInput_ != nullptr) searchInput_->endEditing();
  searchOverlay_->setVisible(false);
  if (!failed_) startInputListening();
}

void MusicSelectScene::search(std::string text) {
  if (!chartSession_ || !MusicSelectSearchHistory::acceptsText(text)) return;
  ChartMetaQuery query;
  query.keyword = text;
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  std::vector<ChartMetaRecord> records;
  chartSession_->QueryChartMeta(query, records);
  if (!searchHistory_.remember(
          std::move(text), !records.empty(),
          static_cast<std::size_t>(
              context.settings.skinMusicSelectMaxSearchBarCount))) {
    return;
  }
  const auto &selectedText = searchHistory_.entries().back();
  reloadLibrary();
  (void)bars_.select({"search:" + selectedText});
  selectedBarMoved();
}

void MusicSelectScene::closeDirectory() {
  if (bars_.close()) {
    syncResolvedFilters();
    selectedBarMoved();
  } else {
    executeEvent({.kind = skin::MusicSelectSkinActionKind::Event,
                  .selector = {.value = 12},
                  .arguments = {0, 0}});
  }
}

void MusicSelectScene::launchSelected(bool autoplay, bool practice) {
  if (launching_) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind == skin::MusicSelectBarKind::Grade ||
      selected.kind == skin::MusicSelectBarKind::RandomCourse) {
    launchCourse(selected, autoplay);
    return;
  }
  if (!selected.chart) return;
  const auto record = *selected.chart;
  if (record.unavailable || record.solidArchive ||
      record.meta.BmsPath.empty()) {
    return;
  }
  launching_ = true;
  std::atomic_bool cancelled = false;
  auto chart = play_options::parseChart(record.meta, cancelled,
                                        "music-select start");
  if (!chart || cancelled) {
    launching_ = false;
    return;
  }
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  play_options::PlayOptionReplayInfo playInfo;
  if (!play_options::applyPlayOptionModifier(
          *chart, selections.playOption, std::nullopt, 0, playInfo.option,
          playInfo.seed, "music-select")) {
    launching_ = false;
    return;
  }
  if (chart->Meta.IsDP) {
    const auto player2 = replay::beatorajaReplayOptionName(
        context.settings.skinPlayer2RandomOption);
    if (!player2 || !play_options::applyPlayOptionModifier(
                        *chart, std::string(*player2), std::nullopt, 1,
                        playInfo.option2, playInfo.seed2, "music-select")) {
      launching_ = false;
      return;
    }
  }
  int lnMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
  if (lnMode == 0) lnMode = long_note_mode::valueFromId(selections.longNoteMode);
  applyEffectiveLongNoteModeToChart(*chart, lnMode);
  context.jukebox.stop();
  context.jukebox.loadChart(*chart, true, cancelled);
  if (cancelled) {
    launching_ = false;
    return;
  }
  tableContext_ = musicSelectTableContextForLaunch(snapshot);
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = !context.settings.inputKeysoundEnabled,
      .autoPlay = autoplay,
      .gaugeType = selections.gaugeType,
      .gaugeAutoShift = selections.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
      .playOption = playInfo.option,
      .playOptionSeed = playInfo.seed,
      .playOption2 = playInfo.option2,
      .playOption2Seed = playInfo.seed2,
      .doublePlayFlip = context.settings.skinDoublePlayOption == 1,
      .longNoteMode = lnMode,
      .assistOption = selections.assistOption,
      .pacemakerTarget = selections.pacemakerTarget,
      .tableName = tableContext_.name,
      .tableLevel = tableContext_.level,
      .practiceMode = practice,
      .playback = {.percent = context.settings.selectedPlaybackRatePercent,
                   .mode = context.settings.selectedPlaybackMode},
      .clubMode = context.settings.gameplayClubModeEnabled,
      .returnScene = this,
      .ruleset = selections.ruleset};
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      true);
}

void MusicSelectScene::launchCourse(const MusicSelectBar &bar,
                                    bool autoplay) {
  if (launching_ || bar.courseCharts.empty() || !bar.presentation.exists) {
    return;
  }
  auto session = buildCourseGameplaySession(
      {.courseId = bar.courseId,
       .courseKey = bar.courseKey,
       .courseName = bar.courseGroupName.empty()
                         ? bar.title
                         : bar.courseGroupName + " " + bar.title,
       .courseGroupName = bar.courseGroupName,
       .constraintJson = bar.courseConstraintJson,
       .records = bar.courseCharts,
       .selections =
           main_menu_profile::Selections::fromSettings(context.settings),
       .player2PlayOption = std::string(
           replay::beatorajaReplayOptionName(
               context.settings.skinPlayer2RandomOption)
               .value_or("NORMAL")),
       .doublePlayFlip = context.settings.skinDoublePlayOption == 1,
       .inputKeysoundEnabled = context.settings.inputKeysoundEnabled});
  session->autoPlay = autoplay;
  const auto *firstMeta = session->currentMeta();
  if (firstMeta == nullptr || firstMeta->BmsPath.empty()) return;

  launching_ = true;
  std::atomic_bool cancelled = false;
  auto chart = play_options::parseChart(firstMeta->BmsPath, cancelled,
                                        "music-select course");
  if (!chart || cancelled) {
    launching_ = false;
    return;
  }
  applyCourseConstraintsToChart(*chart, session->constraints);
  const auto playInfo = play_options::applySelectedPlayOptions(
      *chart, session->requestedPlayOption, session->requestedPlayOption2);
  applyEffectiveLongNoteModeToChart(*chart, session->longNoteMode);
  session->playOption = playInfo.option;
  session->playOptionSeed = playInfo.seed;
  session->playOption2 = playInfo.option2;
  session->playOption2Seed = playInfo.seed2;

  context.jukebox.stop();
  context.jukebox.loadChart(*chart, true, cancelled);
  if (cancelled) {
    launching_ = false;
    return;
  }
  tableContext_ = musicSelectTableContextForLaunch(bars_.snapshot());
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = session->autoKeySound,
      .autoPlay = autoplay,
      .gaugeType = session->gaugeType,
      .gaugeProfile = session->gaugeProfile,
      .gaugeAutoShift = session->gaugeAutoShift,
      .gaugeAutoShiftLowerBound = session->gaugeAutoShiftLowerBound,
      .playOption = playInfo.option,
      .playOptionSeed = playInfo.seed,
      .playOption2 = playInfo.option2,
      .playOption2Seed = playInfo.seed2,
      .doublePlayFlip = session->doublePlayFlip,
      .longNoteMode = session->longNoteMode,
      .assistOption = session->assistOption,
      .tableName = tableContext_.name,
      .tableLevel = tableContext_.level,
      .playback = course_rules::kRequiredPlaybackRate,
      .clubMode = context.settings.gameplayClubModeEnabled,
      .courseSession = session,
      .courseConstraints = session->constraints,
      .ruleset = session->ruleset,
      .requiredRulesetDescriptor = session->rulesetDescriptor,
      .ownsChart = true,
      .returnScene = this,
  };
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      true);
}

void MusicSelectScene::launchSelectedDirectoryAutoplay() {
  if (launching_) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &directory = snapshot.rows[snapshot.selectedIndex];
  MusicSelectBar playlist;
  playlist.title = directory.title;
  for (const auto &child : bars_.childrenOf(directory.id)) {
    if (child.kind == skin::MusicSelectBarKind::Song && child.chart &&
        !child.chart->meta.BmsPath.empty()) {
      playlist.courseCharts.push_back(*child.chart);
    }
  }
  playlist.presentation.exists = !playlist.courseCharts.empty();
  launchCourse(playlist, true);
}

void MusicSelectScene::launchSelectedReplay(int slot) {
  if (launching_ || slot < 0 || slot >= 4) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size() ||
      !snapshot.rows[snapshot.selectedIndex].chart) {
    return;
  }
  const auto record = *snapshot.rows[snapshot.selectedIndex].chart;
  const int lnMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  const auto paths = musicSelectChartReplaySlotPaths(record, lnMode);
  if (!paths) return;
  const auto &slotPath = (*paths)[static_cast<std::size_t>(slot)];
  std::error_code existsError;
  if (!std::filesystem::exists(
          context.replayRepository.GetResolvedProfileRoot() /
              slotPath.relativePath,
          existsError)) {
    return;
  }

  const auto inventory =
      context.replayRepository.ListModernReplayFileReferences();
  if (inventory.status != ModernReplayFileInventoryStatus::Loaded) {
    SDL_Log("Unable to read replay slot inventory: %s",
            inventory.diagnostic.c_str());
    return;
  }
  const auto resultId =
      musicSelectChartReplayResultId(inventory.entries, slotPath);
  if (!resultId) return;
  auto stored = context.replayRepository.LoadModernChartResult(*resultId);
  if (stored.status != ModernChartResultReadStatus::Loaded || !stored.record) {
    SDL_Log("Unable to read replay slot result: %s", stored.diagnostic.c_str());
    return;
  }

  launching_ = true;
  std::atomic_bool cancelled = false;
  auto consumer =
      replay::makeRuntimeChartReplayConsumer(context.replayRepository);
  auto loaded = consumer.load(*stored.record, record.meta.BmsPath, cancelled);
  if (!loaded.ready() || cancelled) {
    SDL_Log("Unable to prepare replay slot: %s", loaded.diagnostic.c_str());
    launching_ = false;
    return;
  }
  context.jukebox.stop();
  context.jukebox.loadChart(*loaded.chart, true, cancelled);
  if (cancelled) {
    launching_ = false;
    return;
  }
  tableContext_ = musicSelectTableContextForLaunch(snapshot);
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = false,
      .autoPlay = false,
      .gaugeType = loaded.replayData->initialGaugeType,
      .gaugeAutoShift = loaded.replayData->gaugeAutoShift,
      .replayData = loaded.replayData,
      .pacemakerTarget = selections.pacemakerTarget,
      .tableName = tableContext_.name,
      .tableLevel = tableContext_.level,
      .returnScene = this,
  };
  applyReplayProvenanceToStartOptions(options, *loaded.replayData);
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(loaded.chart),
                                      std::move(options)),
      true);
}

void MusicSelectScene::changeSelectedFavorite(bool song, int direction) {
  if (!chartSession_) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (!selected.chart || selected.chart->meta.BmsPath.empty()) return;

  const MusicSelectFavoriteBits bits =
      song ? MusicSelectFavoriteBits{.favorite = 1, .invisible = 4}
           : MusicSelectFavoriteBits{.favorite = 2, .invisible = 8};
  const auto state = musicSelectNextFavoriteState(
      selected.chart->songReviewFavorite, bits, direction);
  if (!song) {
    const int flags = musicSelectApplyFavoriteState(
        selected.chart->songReviewFavorite, bits, state);
    const bool favorite = (flags & bits.favorite) != 0;
    if (!chartSession_->SetFavorite(selected.chart->meta, favorite) ||
        !chartSession_->SetSongReviewFavorite(selected.chart->meta.SHA256,
                                              flags)) {
      SDL_Log("Unable to update selected chart favorite state");
    }
    return;
  }

  ChartMetaQuery query;
  query.exactFolder = !selected.chart->meta.Folder.empty()
                          ? selected.chart->meta.Folder
                          : selected.chart->meta.BmsPath.parent_path();
  std::vector<ChartMetaRecord> records;
  chartSession_->QueryChartMeta(query, records);
  for (const auto &record : records) {
    const int flags = musicSelectApplyFavoriteState(
        record.songReviewFavorite, bits, state);
    if (!chartSession_->SetSongReviewFavorite(record.meta.SHA256, flags)) {
      SDL_Log("Unable to update song favorite state");
    }
  }
}

void MusicSelectScene::consumeActions() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (!skinSession_) return;
  for (const auto &action : skinSession_->takePublishedActions()) {
    switch (action.kind) {
    case skin::MusicSelectSkinActionKind::Event:
      executeEvent(action);
      break;
    case skin::MusicSelectSkinActionKind::FloatWriter: {
      const auto id = numericSelector(action.selector);
      const auto name = selectorName(action.selector);
      if ((id && *id == 1) || name == "musicselect_position") {
        bars_.setSelectedPosition(static_cast<float>(action.floatValue));
        selectedBarMoved();
      } else if (((id && *id == 8) || name == "ranking_position") &&
                 action.floatValue >= 0.0 && action.floatValue < 1.0) {
        rankingOffset_ = static_cast<int>(
            std::max(1, ranking_.totalPlayers) * action.floatValue);
        ranking_.offset = rankingOffset_;
      }
      break;
    }
    case skin::MusicSelectSkinActionKind::StringWriter: {
      const auto id = numericSelector(action.selector);
      if ((id && *id == 30) || selectorName(action.selector) == "searchword") {
        search(action.stringValue);
      }
      break;
    }
    }
  }
#endif
}

void MusicSelectScene::consumeLogicalInput() {
  if (!inputBindingAdapter_) return;
  auto &logicalInput = inputBindingAdapter_->state();
  const auto snapshot = bars_.snapshot();
  logicalInput.currentBar = MusicSelectInputBarKind::Other;
  if (snapshot.selectedIndex < snapshot.rows.size()) {
    const auto &selected = snapshot.rows[snapshot.selectedIndex];
    logicalInput.currentBar =
        skin::musicSelectIsDirectoryBarKind(selected.kind)
            ? MusicSelectInputBarKind::Directory
            : skin::musicSelectIsSelectableBarKind(selected.kind)
                  ? MusicSelectInputBarKind::Selectable
                  : MusicSelectInputBarKind::Other;
  }
  logicalInput.selectedReplay = selectedReplay_;
  for (const auto &action :
       inputProcessor_.process(logicalInput, unixMillis())) {
    applyInputAction(action);
  }
  if (inputBindingAdapter_) inputBindingAdapter_->clearFrameEdges();
}

void MusicSelectScene::startInputListening() {
  if (inputSubscription_ != 0 || inputDeviceSubscription_ != 0) return;
  const auto layout =
      musicSelectKeyLayoutForConfig(context.settings.skinMusicSelectInput);
  inputProcessor_.setLayout(layout);
  inputBindingAdapter_ = std::make_unique<MusicSelectInputBindingAdapter>(
      context.inputProfile, layout);
  inputSubscription_ = context.inputDeviceRegistry.subscribeInput(
      [this](const input::PhysicalInputEvent &event) {
        if (inputBindingAdapter_) inputBindingAdapter_->consume(event);
      });
  inputDeviceSubscription_ = context.inputDeviceRegistry.subscribeDevices(
      [this](const input::InputDeviceSnapshot &device) {
        if (!device.connected && inputBindingAdapter_) {
          inputBindingAdapter_->disconnectDevice(device.stableId);
        }
      });
}

void MusicSelectScene::stopInputListening() {
  if (inputSubscription_ != 0) {
    context.inputDeviceRegistry.unsubscribe(inputSubscription_);
    inputSubscription_ = 0;
  }
  if (inputDeviceSubscription_ != 0) {
    context.inputDeviceRegistry.unsubscribe(inputDeviceSubscription_);
    inputDeviceSubscription_ = 0;
  }
  if (inputBindingAdapter_) inputBindingAdapter_->reset();
  inputBindingAdapter_.reset();
}

void MusicSelectScene::applyInputAction(
    const MusicSelectInputAction &action) {
  bool saveSettings = false;
  switch (action.kind) {
  case MusicSelectInputActionKind::Event:
    executeEvent({.kind = skin::MusicSelectSkinActionKind::Event,
                  .selector = {.value = action.value},
                  .arguments = {action.argument1, action.argument2}});
    break;
  case MusicSelectInputActionKind::SetPanel:
    setPanelState(action.value);
    break;
  case MusicSelectInputActionKind::MoveNext:
    bars_.move(true, action.value, action.deadlineMillis);
    selectedBarMoved();
    break;
  case MusicSelectInputActionKind::MovePrevious:
    bars_.move(false, action.value, action.deadlineMillis);
    selectedBarMoved();
    break;
  case MusicSelectInputActionKind::Play:
    launchSelected();
    break;
  case MusicSelectInputActionKind::Practice:
    launchSelected(false, true);
    break;
  case MusicSelectInputActionKind::Autoplay:
    launchSelected(true, false);
    break;
  case MusicSelectInputActionKind::AutoplayFolder:
    launchSelectedDirectoryAutoplay();
    break;
  case MusicSelectInputActionKind::Replay:
    launchSelectedReplay(action.value);
    break;
  case MusicSelectInputActionKind::OpenFolder:
    (void)bars_.openSelected();
    syncResolvedFilters();
    selectedBarMoved();
    break;
  case MusicSelectInputActionKind::CloseFolder:
    closeDirectory();
    break;
  case MusicSelectInputActionKind::CommandNextReplay: {
    const auto snapshot = bars_.snapshot();
    if (snapshot.selectedIndex < snapshot.rows.size()) {
      selectedReplay_ = musicSelectNextExistingReplay(
          &snapshot.rows[snapshot.selectedIndex], selectedReplay_);
    }
    break;
  }
  case MusicSelectInputActionKind::CommandSameFolder:
    openSameFolder();
    break;
  case MusicSelectInputActionKind::CopyMd5:
    copySelectedHash(false);
    break;
  case MusicSelectInputActionKind::CopySha256:
    copySelectedHash(true);
    break;
  case MusicSelectInputActionKind::SearchPrompt:
    showSearchPrompt();
    break;
  case MusicSelectInputActionKind::SelectedBarMoved:
    selectedBarMoved();
    break;
  case MusicSelectInputActionKind::ToggleCustomJudge:
    context.settings.customJudge = !context.settings.customJudge;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleConstant:
    context.settings.scrollMode = context.settings.scrollMode == 1 ? 0 : 1;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleShowJudgeArea:
    context.settings.showJudgeArea = !context.settings.showJudgeArea;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleLegacyNote:
    context.settings.longNoteModifierMode =
        context.settings.longNoteModifierMode == 1 ? 0 : 1;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleMarkProcessedNote:
    context.settings.markProcessedNotes = !context.settings.markProcessedNotes;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleBpmGuide:
    context.settings.selectedAssistOption =
        assist_options::isBpmGuide(context.settings.selectedAssistOption)
            ? assist_options::kOff
            : assist_options::kBpmGuide;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ToggleNoMine:
    context.settings.mineMode = context.settings.mineMode == 1 ? 0 : 1;
    saveSettings = true;
    break;
  case MusicSelectInputActionKind::ExitApplication:
    context.quitFlag.store(true);
    break;
  default:
    break;
  }
  if (saveSettings && !context.saveSettings()) {
    SDL_Log("Unable to save music-select input option change");
  }
}

void MusicSelectScene::executeEvent(
    const skin::MusicSelectSkinAction &action) {
  const auto id = numericSelector(action.selector);
  const auto name = selectorName(action.selector);
  const auto snapshot = bars_.snapshot();
  const MusicSelectBar *selected =
      snapshot.selectedIndex < snapshot.rows.size()
          ? &snapshot.rows[snapshot.selectedIndex]
          : nullptr;
  MusicSelectEventContext eventContext{
      .settings = context.settings,
      .sortIndex = sortIndex_,
      .hasSelectedPlayConfig = selected != nullptr,
      .selectedSongHasPath =
          selected != nullptr && selected->chart.has_value() &&
          !selected->chart->meta.BmsPath.empty(),
      .rivalCount = 0,
      .currentRivalIndex = currentRivalIndex_,
  };
  const int argument1 = action.arguments.empty() ? 0 : action.arguments[0];
  const int argument2 =
      action.arguments.size() < 2 ? 0 : action.arguments[1];
  auto outcome = MusicSelectEventController::execute(
      eventContext, id.value_or(-1), name, argument1, argument2);
  sortIndex_ = eventContext.sortIndex;
  currentRivalIndex_ = eventContext.currentRivalIndex;
  if (!outcome.failure.empty()) {
    enterError({skin::SkinDiagnostic{
        .code = "skin.music_select.event_failed",
        .message = std::move(outcome.failure)}});
    return;
  }
  if (outcome.settingsChanged && !context.saveSettings()) {
    enterError({skin::SkinDiagnostic{
        .code = "skin.music_select.settings_save_failed",
        .message = "The music-select event changed configuration, but the "
                   "profile settings could not be saved."}});
    return;
  }
  for (const auto &effect : outcome.effects) {
    switch (effect.kind) {
    case MusicSelectEventEffectKind::RefreshBars:
      reloadLibrary();
      selectedBarMoved();
      break;
    case MusicSelectEventEffectKind::OpenSettings:
      openSettings();
      return;
    case MusicSelectEventEffectKind::Play:
      launchSelected();
      return;
    case MusicSelectEventEffectKind::Autoplay:
      launchSelected(true, false);
      return;
    case MusicSelectEventEffectKind::Practice:
      launchSelected(false, true);
      return;
    case MusicSelectEventEffectKind::Replay:
      selectedReplay_ = effect.value;
      launchSelectedReplay(effect.value);
      return;
    case MusicSelectEventEffectKind::OpenDocument:
      if (selected != nullptr && platform_open::desktopOpenSupported()) {
        for (const auto &path : musicSelectDocumentPaths(
                 *selected, materializedArchiveDocuments)) {
          std::string error;
          if (!platform_open::openPath(path, error)) {
            SDL_Log("Failed to open chart document %s: %s",
                    path.string().c_str(), error.c_str());
          }
        }
      }
      break;
    case MusicSelectEventEffectKind::OpenExplorer:
      if (selected != nullptr && platform_open::desktopOpenSupported()) {
        MusicSelectExplorerLookups lookups;
        lookups.originalMd5Paths = [this](
                                            std::span<const std::string> md5s) {
          std::vector<std::filesystem::path> paths;
          if (!chartSession_) return paths;
          for (auto hash = md5s.rbegin(); hash != md5s.rend(); ++hash) {
            for (const auto &meta :
                 chartSession_->SelectChartMetaByHash({}, *hash)) {
              paths.push_back(meta.BmsPath);
            }
          }
          return paths;
        };
        lookups.textPaths = [this](std::string_view text) {
          std::vector<std::filesystem::path> paths;
          if (!chartSession_) return paths;
          ChartMetaQuery query;
          query.keyword = std::string(text);
          std::vector<ChartMetaRecord> records;
          chartSession_->QueryChartMeta(query, records);
          paths.reserve(records.size());
          for (const auto &record : records) {
            paths.push_back(record.meta.BmsPath);
          }
          return paths;
        };
        if (const auto path = musicSelectExplorerPath(
                *selected, lookups, archive_file::splitVirtualPath)) {
          std::string error;
          if (!platform_open::openPath(*path, error)) {
            SDL_Log("Failed to open selector path %s: %s",
                    path->string().c_str(), error.c_str());
          }
        }
      }
      break;
    case MusicSelectEventEffectKind::OpenDownloadSite:
      if (selected != nullptr) {
        for (const auto &url : musicSelectDownloadUrls(*selected)) {
          std::string error;
          if (!platform_open::openExternalUrl(url, error)) {
            SDL_Log("Failed to open selector download URL %s: %s",
                    url.c_str(), error.c_str());
          }
        }
      }
      break;
    case MusicSelectEventEffectKind::OpenIr:
      if (selected != nullptr && irExternalUrlService_) {
        ir::IrExternalUrlRequest request;
        request.profile.profileId = context.profileManager.activeProfile().id;
        request.profile.providers.insert(context.settings.irProviders.begin(),
                                         context.settings.irProviders.end());
        if (selected->kind == skin::MusicSelectBarKind::Song &&
            selected->chart) {
          const auto &meta = selected->chart->meta;
          request.target = ir::IrExternalUrlTarget::Chart;
          request.chart = {.keyMode = meta.KeyMode,
                           .isDoublePlay = meta.IsDP,
                           .chartSha256 = meta.SHA256};
        } else if (selected->kind == skin::MusicSelectBarKind::Grade) {
          request.target = ir::IrExternalUrlTarget::Course;
        }
        irExternalUrlGeneration_ =
            irExternalUrlService_->open(std::move(request));
      }
      break;
    case MusicSelectEventEffectKind::UpdateFolder: {
      if (selected != nullptr &&
          selected->kind == skin::MusicSelectBarKind::Table &&
          context.chartLibraryTasks) {
        context.chartLibraryTasks->enqueue({
            .kind = chart_library_tasks::TaskKind::UpdateDifficultyTable,
            .title = "Update Difficulty Table",
            .tableId = selected->tableId,
        });
        break;
      }
      const auto path = selected != nullptr
                            ? musicSelectRefreshPath(
                                  *selected, archive_file::splitVirtualPath)
                            : std::nullopt;
      if (path && context.chartLibraryTasks) {
        context.chartLibraryTasks->enqueue({
            .kind = chart_library_tasks::TaskKind::RefreshPath,
            .title = "Update Folder",
            .refreshPath = *path,
        });
      }
      break;
    }
    case MusicSelectEventEffectKind::ChangeFavoriteSong:
      changeSelectedFavorite(true, effect.value);
      break;
    case MusicSelectEventEffectKind::ChangeFavoriteChart:
      changeSelectedFavorite(false, effect.value);
      break;
    default:
      break;
    }
  }
}

void MusicSelectScene::update(float) {
  if (failed_) return;
  if (irExternalUrlGeneration_ != 0 && irExternalUrlService_) {
    const auto snapshot = irExternalUrlService_->snapshot();
    if (snapshot.generation == irExternalUrlGeneration_ &&
        snapshot.finished) {
      irExternalUrlService_->close(irExternalUrlGeneration_);
      irExternalUrlGeneration_ = 0;
      if (snapshot.url) {
        std::string error;
        if (!platform_open::openExternalUrl(*snapshot.url, error)) {
          SDL_Log("Failed to open selector IR URL %s: %s",
                  snapshot.url->c_str(), error.c_str());
        }
      }
    }
  }
  if (toolbar_) {
    toolbar_->setViewportSize(rendering::window_width,
                              rendering::window_height);
  }
  if (searchOverlay_ != nullptr && searchOverlay_->getVisible()) {
    searchOverlay_->setSize(rendering::window_width,
                            rendering::window_height);
  }
  if (context.chartRepository.GetLibraryRevision() != libraryRevision_) {
    reloadLibrary();
    selectedBarMoved();
  }
  const auto irEvidenceRevision =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  if (irEvidenceRevision != irAccountEvidenceRevision_) {
    selectedBarMoved();
  }
  if (!startInputMicros_) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
    const int inputMillis = skinSession_ ? skinSession_->inputDelayMillis() : 0;
#else
    const int inputMillis = 0;
#endif
    const auto now = elapsedMicros();
    if (now > static_cast<std::int64_t>(inputMillis) * 1'000) {
      startInputMicros_ = now;
    }
  }
  // Pinned Beatoraja exposes the authored selector delay through timer 1 but
  // MainController still calls MusicSelector.input() before that timer turns
  // on. Keep controller and keyboard input live across the same interval.
  consumeLogicalInput();
  consumeActions();
  previewController_.observeSelection(
      previewSelection(bars_.snapshot(),
                       context.settings.archiveChartPreviewEnabled),
      songBarChangeMicros_);
  if (auto preview = previewController_.update(elapsedMicros(), launching_);
      preview && previewAudio_) {
    previewAudio_->switchTo(std::move(preview->path));
  }
  updateRanking();
}

void MusicSelectScene::renderScene() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (failed_ || !skinSession_) return;
  ++frameSerial_;
  auto frame = makeFrame();
  if (skinSession_->requiresResourceRefresh(frame) &&
      !skinSession_->refreshResources(frame)) {
    enterError(skinSession_->takeLastDiagnostics());
    return;
  }
  RenderContext renderContext(context.uiBatchRenderer);
  RenderContext::UiBatchScope batch(renderContext);
  if (!skinSession_->render(renderContext, frame)) {
    enterError(skinSession_->takeLastDiagnostics());
  }
#endif
}

void MusicSelectScene::enterError(
    std::vector<skin::SkinDiagnostic> diagnostics) {
  if (failed_) return;
  failed_ = true;
  stopInputListening();
  previewController_.reset();
  if (previewAudio_) previewAudio_->switchTo(std::nullopt);
  if (searchInput_ != nullptr) searchInput_->endEditing();
  if (searchOverlay_ != nullptr) searchOverlay_->setVisible(false);
  diagnostics_ = std::move(diagnostics);
  if (diagnostics_.empty()) {
    diagnostics_.push_back(skin::SkinDiagnostic{
        .code = "skin.music_select.failure_without_diagnostic",
        .message =
            "The music-select skin failed without reporting a diagnostic."});
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinSession_.reset();
#endif
  if (toolbar_) toolbar_->setVisible(false);
  buildErrorView();
}

void MusicSelectScene::buildErrorView() {
  auto *root = new View(0, 0, rendering::window_width,
                        rendering::window_height);
  root->setPositionType(YGPositionTypeAbsolute);
  root->setZIndex(20000);
  root->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(12)
      ->setPadding(Edge::All, 32)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  auto *title = makeText("Music-select skin failed", 38, ui_theme::coral);
  title->setHeight(58);
  root->addView(title);
  if (!selectedSkinPath_.empty()) {
    auto *path = makeText("Selected skin: " + selectedSkinPath_, 20,
                          ui_theme::textSecondary);
    path->setHeight(44);
    root->addView(path);
  }
  if (diagnostics_.empty()) {
    auto *reason = makeText("No diagnostic was reported.", 20,
                            ui_theme::textSecondary);
    reason->setHeight(44);
    root->addView(reason);
  } else {
    for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
      const auto &diagnostic = diagnostics_[index];
      auto *reasonView = makeText(
          musicSelectSkinFailureReason(index, diagnostic), 19,
                                  ui_theme::textPrimary);
      reasonView->setMinHeight(42);
      root->addView(reasonView);
    }
  }
  auto *actions = new View();
  actions->setHeight(64)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(12);
  auto *back = makeButton("Back");
  back->setOnClickListener(
      [this] { context.sceneManager->changeScene("Intro"); });
  actions->addView(back);
  auto *settings = makeButton("Settings");
  settings->setOnClickListener([this] {
    context.sceneManager->changeScene(std::make_unique<SettingsScene>(
        context, SettingsDestination::Profile,
        SceneReturnTarget::Registered("Intro")));
  });
  actions->addView(settings);
  root->addView(actions);
  root->applyYogaLayout();
  addView(root);
}

void MusicSelectScene::openMusicPlayer() {
  context.sceneManager->changeScene(std::make_unique<MusicPlayerScene>(
      context, SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::openTasks() {
  context.sceneManager->changeScene(std::make_unique<LibraryTasksScene>(
      context, SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::openIrUploads() {
  context.sceneManager->changeScene(std::make_unique<IrUploadsScene>(
      context, SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::openSettings() {
  reactivateSkinOnResume_ = true;
  context.sceneManager->changeScene(std::make_unique<SettingsScene>(
      context, SettingsDestination::Profile,
      SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::syncToolbar() {
  MusicSelectToolbarState state;
  {
    std::lock_guard lock(context.applicationUiStateMutex);
    state = context.applicationUiState.musicSelectToolbar;
  }
  if (toolbar_ != nullptr) {
    toolbar_->applyState(state);
    return;
  }
  auto toolbar = MusicSelectToolbarView::Create(
      state,
      {.openMusicPlayer = [this] { openMusicPlayer(); },
       .openTasks = [this] { openTasks(); },
       .openIrUploads = [this] { openIrUploads(); },
       .openSettings = [this] { openSettings(); },
       .persist = [this](MusicSelectToolbarState updated) {
         persistToolbar(updated);
       }},
      rendering::window_width, rendering::window_height);
  if (toolbar) {
    toolbar_ = toolbar.get();
    addView(toolbar.release());
  }
}

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
bool MusicSelectScene::activateSkin(
    skin::GameplaySkinActivationRequest request) {
  selectedSkinPath_ = musicSelectSkinEntryPath(request.activation.entry);
  if (!context.skinStorageRoots || !context.skinResourcePreparationService ||
      !context.skinLiveResourceCounters) {
    enterError({skin::SkinDiagnostic{
        .code = "skin.music_select.session_services_unavailable",
        .message = "Music-select skin session services are unavailable."}});
    return false;
  }
  auto initialFrame = makeFrame();
  const auto safetyLevel = request.safetyLevel;
  auto created = skin::MusicSelectSkinSession::create(
      std::move(request),
      {.storageRoots = *context.skinStorageRoots,
       .resourcePreparation = *context.skinResourcePreparationService,
       .initialFrame = std::move(initialFrame),
       .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
       .builtinImageReader = archive_file::readFileBounded,
       .audioBackend = skin::createLuaSkinApplicationAudioBackend(
           context.jukebox.audioRuntime(),
           [this] { return context.settings.audioVideo.audio.masterVolume; },
           {}, context.skinLiveResourceCounters),
       .liveResourceCounters = context.skinLiveResourceCounters,
       .safetyPolicy = skin::SkinSafetyPolicy(safetyLevel)});
  if (!created.session) {
    enterError(std::move(created.diagnostics));
    return false;
  }
  skinSession_ = std::move(created.session);
  return true;
}

bool MusicSelectScene::reactivateSkinAfterSettings() {
  skin::GameplaySkinAcquisition acquisition;
  if (context.gameplaySkinLifecycle) {
    acquisition =
        context.gameplaySkinLifecycle->acquireForSkinType(5, false);
  } else if (context.settings.skin.selectedSkinEntries.contains(5)) {
    acquisition.disposition =
        skin::GameplaySkinAcquisitionDisposition::Failed;
    acquisition.failure = skin::GameplaySkinAcquisitionFailure{
        .diagnostic = skin::SkinDiagnostic{
            .code = "skin.music_select.lifecycle_unavailable",
            .message =
                "The selected music-select skin service is unavailable."}};
  }
  auto decision = decideMusicSelectLaunch(std::move(acquisition));
  if (decision.kind == MusicSelectLaunchKind::BuiltIn) {
    context.sceneManager->changeScene("MainMenu");
    return false;
  }
  if (decision.kind == MusicSelectLaunchKind::Error) {
    selectedSkinPath_ = std::move(decision.selectedSkinPath);
    enterError(std::move(decision.diagnostics));
    return false;
  }
  return decision.request && activateSkin(std::move(*decision.request));
}
#endif

void MusicSelectScene::persistToolbar(MusicSelectToolbarState state) {
  {
    std::lock_guard lock(context.applicationUiStateMutex);
    context.applicationUiState.musicSelectToolbar = state;
  }
  std::string diagnostic;
  if (!context.saveApplicationUiState(&diagnostic)) {
    SDL_Log("Unable to save music-select toolbar state: %s",
            diagnostic.c_str());
  }
}

void MusicSelectScene::cleanupScene() {
  if (searchInput_ != nullptr) searchInput_->endEditing();
  stopInputListening();
  if (rankingGeneration_ != 0 && context.irRankingService) {
    context.irRankingService->close(rankingGeneration_);
  }
  rankingGeneration_ = 0;
  if (irExternalUrlService_) {
    irExternalUrlService_->stop();
  }
  irExternalUrlService_.reset();
  irExternalUrlGeneration_ = 0;
  previewController_.reset();
  previewAudio_.reset();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinSession_.reset();
#endif
  chartSession_.reset();
  toolbar_ = nullptr;
  searchOverlay_ = nullptr;
  searchInput_ = nullptr;
  diagnostics_.clear();
}
