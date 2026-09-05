#include "MusicSelectScene.h"

#include "../StartupTiming.h"
#include "../PlatformOpen.h"

#if defined(__ANDROID__)
#include "../AndroidNatives.h"
#endif

#include "IrUploadsScene.h"
#include "ChartViewerScene.h"
#include "MusicPlayerScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "CourseGameplaySessionBuilder.h"
#include "../ArchiveFile.h"
#include "../AssistOptionUtils.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../ReplayAutoPlay.h"
#include "../audio/Jukebox.h"
#include "play/Pacemaker.h"
#include "../music_select/MusicSelectRepositoryProjection.h"
#include "../music_select/MusicSelectReplaySlots.h"
#include "../music_select/MusicSelectFavorites.h"
#include "../music_select/MusicSelectExternalActions.h"
#include "../music_select/MusicSelectLaunchPolicy.h"
#include "../music_select/MusicSelectPropertyProjection.h"
#include "../library/ChartLibraryTaskService.h"
#include "../input/SDLPointerEvent.h"
#include "../ResultRecordSummary.h"
#include "../replay/ChartReplayConsumer.h"
#include "../replay/CourseReplayConsumer.h"
#include "../replay/ReplayFileActionService.h"
#include "../rendering/common.h"
#include "../repositories/LegacyResultSummary.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "../view/OverlayPortal.h"
#include "ChartPreloadWorker.h"
#include "DecideLoadingOverlay.h"
#include "../view/ResultRecordListView.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "play/GamePlayScene.h"
#include "MainMenuProfileSelections.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../skin/beatoraja/LuaSkinApplicationAudioBackend.h"
#include "play/PlayfieldChartVisualModel.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr const char *kSkinSoundAssetRoot = "assets";
constexpr std::int64_t kRankingDurationMillis = 5'000;
constexpr std::int64_t kRankingReloadDurationMillis = 10 * 60 * 1'000;

// Default playback for the select system-SE service: lazily loads each sound
// through AudioWrapper::{loadSkinSound,playSkinSound} (the same skin-sound path
// the preview service uses) and caches the loaded handle per path.
class MusicSelectSkinSoundPlayer {
public:
  explicit MusicSelectSkinSoundPlayer(AudioWrapper *audio) : audio_(audio) {}

  ~MusicSelectSkinSoundPlayer() {
    if (!audio_) return;
    for (const auto &[path, handle] : handles_) {
      audio_->stopSkinSound(audio::SkinSoundHandle{handle});
      audio_->disposeSkinSound(audio::SkinSoundHandle{handle});
    }
  }

  void operator()(const std::filesystem::path &path) {
    if (!audio_) return;
    const auto key = fspath_to_path_t(path);
    if (const auto found = handles_.find(key); found != handles_.end()) {
      audio_->playSkinSound(audio::SkinSoundHandle{found->second}, 1.0F, false);
      return;
    }
    std::atomic<bool> cancelled{false};
    const auto loaded = audio_->loadSkinSound(
        key, cancelled, std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max());
    if (!loaded.handle) return;
    if (!audio_->playSkinSound(*loaded.handle, 1.0F, false)) {
      audio_->disposeSkinSound(*loaded.handle);
      return;
    }
    handles_.emplace(key, loaded.handle->value);
  }

private:
  AudioWrapper *audio_;
  std::unordered_map<path_t, std::uint64_t> handles_;
};

skin::SkinSystemSoundService::Playback
musicSelectSkinSoundPlayback(AudioWrapper &audio) {
  auto player = std::make_shared<MusicSelectSkinSoundPlayer>(&audio);
  return [player](const std::filesystem::path &path) { (*player)(path); };
}

// Previews the chart and the looping select BGM through the same AudioWrapper
// skin-sound path as the system-SE player. The default handle stays loaded so
// returning to the select BGM is instant; preview sounds are disposed as soon
// as they stop so the session keeps only the default decoded
// (PreviewMusicProcessor.java:97-101, symmetric fade approximated).
class MusicSelectPreviewBgmPlayer {
public:
  explicit MusicSelectPreviewBgmPlayer(AudioWrapper *audio,
                                       std::filesystem::path defaultPath)
      : audio_(audio), defaultKey_(fspath_to_path_t(defaultPath)) {}

  ~MusicSelectPreviewBgmPlayer() {
    stopCurrent();
    if (audio_ && defaultHandle_) {
      audio_->disposeSkinSound(*defaultHandle_);
    }
  }

  bool play(const std::filesystem::path &path, bool loop,
            const std::shared_ptr<std::atomic_bool> &cancellation,
            std::stop_token stop) {
    if (!audio_) return false;
    stopCurrent();
    const auto key = fspath_to_path_t(path);
    const bool isDefault = (key == defaultKey_);
    audio::SkinSoundHandle handle;
    if (isDefault && defaultHandle_) {
      handle = *defaultHandle_;
    } else {
      const auto loaded = audio_->loadSkinSound(
          key, *cancellation, std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(), stop);
      if (!loaded.handle) return false;
      handle = *loaded.handle;
      if (isDefault) defaultHandle_ = handle;
    }
    if (cancellation->load(std::memory_order_acquire) ||
        stop.stop_requested()) {
      (void)audio_->disposeSkinSound(handle);
      if (isDefault) defaultHandle_.reset();
      return false;
    }
    if (!audio_->playSkinSound(handle, 1.0F, loop)) {
      (void)audio_->disposeSkinSound(handle);
      if (isDefault) defaultHandle_.reset();
      return false;
    }
    playingHandle_ = handle;
    playingKey_ = key;
    return true;
  }

  void stopCurrent() {
    if (!audio_ || !playingHandle_) return;
    (void)audio_->stopSkinSound(*playingHandle_);
    if (playingKey_ != defaultKey_) {
      (void)audio_->disposeSkinSound(*playingHandle_);
    }
    playingHandle_.reset();
    playingKey_ = {};
  }

private:
  AudioWrapper *audio_;
  path_t defaultKey_;
  std::optional<audio::SkinSoundHandle> defaultHandle_;
  std::optional<audio::SkinSoundHandle> playingHandle_;
  path_t playingKey_{};
};

MusicSelectPreviewAudioService::AudioPort
musicSelectPreviewAudioPort(AudioWrapper &audio,
                            const std::filesystem::path &defaultPath) {
  auto player =
      std::make_shared<MusicSelectPreviewBgmPlayer>(&audio, defaultPath);
  return {.play =
              [player](const std::filesystem::path &path, bool loop,
                       const std::shared_ptr<std::atomic_bool> &cancellation,
                       std::stop_token stop) {
                return player->play(path, loop, cancellation, std::move(stop));
              },
          .stop = [player]() { player->stopCurrent(); }};
}

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

const char *taskProgressStageText(ChartScanProgressStage stage) {
  switch (stage) {
  case ChartScanProgressStage::Preparing: return "Preparing library scan";
  case ChartScanProgressStage::ScanningRoots: return "Scanning folders";
  case ChartScanProgressStage::IndexingArchives: return "Indexing archives";
  case ChartScanProgressStage::PreparingUpdates:
    return "Preparing chart updates";
  case ChartScanProgressStage::RemovingDeleted: return "Removing deleted charts";
  case ChartScanProgressStage::ParsingCharts: return "Parsing charts";
  case ChartScanProgressStage::ReadingArchive: return "Reading archive entries";
  }
  return "Refreshing library";
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
  case SDLK_6: return MusicSelectControlKey::Num6;
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

std::vector<MusicSelectBar> projectionChildren(
    const MusicSelectProjection &projection, const MusicSelectBarId &parent) {
  std::vector<MusicSelectBar> result;
  const auto *directory = projection.find(parent);
  if (directory == nullptr) return result;
  result.reserve(directory->children.size());
  for (const auto &id : directory->children) {
    if (const auto *child = projection.find(id)) result.push_back(*child);
  }
  return result;
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
  // The looping SELECT system sound doubles as the preview fallback
  // (MusicSelector.java:173-174 setDefault(getSound(SELECT))).
  const auto selectBgm =
      std::filesystem::path{kSkinSoundAssetRoot} /
      std::string(
          skin::musicSelectSystemSoundFilename(skin::MusicSelectSystemSound::Select));
  previewAudio_ = std::make_unique<MusicSelectPreviewAudioService>(
      musicSelectPreviewAudioPort(context.jukebox.audioRuntime(), selectBgm));
  systemSound_ = std::make_unique<skin::SkinSystemSoundService>(
      kSkinSoundAssetRoot,
      musicSelectSkinSoundPlayback(context.jukebox.audioRuntime()));
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
  modalLayer_ = new View(0, 0, rendering::window_width,
                         rendering::window_height);
  modalLayer_->setPositionType(YGPositionTypeAbsolute);
  modalLayer_->setPosition(Edge::Left, 0);
  modalLayer_->setPosition(Edge::Top, 0);
  modalOverlayPortal_ = new OverlayPortal(
      0, 0, rendering::window_width, rendering::window_height);
  modalOverlayPortal_->setPositionType(YGPositionTypeAbsolute);
  modalOverlayPortal_->setPosition(Edge::Left, 0);
  modalOverlayPortal_->setPosition(Edge::Top, 0);
  modalOverlayPortal_->setZIndex(2000);
  modalLayer_->addView(modalOverlayPortal_);
  decideOverlay_ = new DecideLoadingOverlay(
      0, 0, rendering::window_width, rendering::window_height, {});
  decideOverlay_->setVisible(false);
  modalOverlayPortal_->present(decideOverlay_);
  preloadWorker_ = new ChartPreloadWorker();
  preloadWorker_->configure(
      [this](const ChartMetaRecord &request, std::atomic_bool &cancelled) {
        StartupTiming::instance().mark("preload worker start");
        auto chart = play_options::parseChart(request.meta, cancelled,
                                              "music-select preload");
        if (!chart || cancelled.load(std::memory_order_relaxed) ||
            preloadWorker_->superseded(
                fspath_to_utf8(request.meta.BmsPath))) {
          return;
        }
        StartupTiming::instance().mark("preload parse done, jukebox load begins");
        context.jukebox.stop();
        (void)context.jukebox.loadChart(*chart, true, cancelled);
        if (cancelled.load(std::memory_order_relaxed) ||
            preloadWorker_->superseded(
                fspath_to_utf8(request.meta.BmsPath))) {
          return;
        }
        StartupTiming::instance().mark("preload jukebox load done");
        std::lock_guard<std::mutex> lock(preloadMutex_);
        if (fspath_to_path_t(preloadedPath_) !=
            fspath_to_path_t(request.meta.BmsPath)) {
          return;  // selection moved on
        }
        preloadedChart_ = std::move(chart);
      });
  addView(modalLayer_);
  startInputListening();
}

void MusicSelectScene::onPause() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr) skinTextInput_->endEditing();
  skinTouchGesture_.cancel();
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
  hideDecideOverlay();
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

void MusicSelectScene::reloadLibrary(bool preserveDirectory) {
  if (!chartSession_) return;
  const MusicSelectBarManagerSnapshot previous =
      preserveDirectory ? bars_.snapshot() : MusicSelectBarManagerSnapshot{};
  std::optional<MusicSelectBarId> previousSelection;
  if (preserveDirectory && previous.selectedIndex < previous.rows.size()) {
    previousSelection = previous.rows[previous.selectedIndex].id;
  }
  const std::uint64_t loadedRevision =
      context.chartRepository.GetLibraryRevision();
  scoreCache_ = context.scoreRepository.LoadBestScores();
  playerHistory_ = context.scoreRepository.LoadPlayerScoreHistory();
  recentScoreImprovements_ = {};
  recentScoreImprovementsLoaded_ = false;
  libraryRevision_ = loadedRevision;
  repositoryMetadata_ = MusicSelectRepositoryProjection::loadMetadata(
      *chartSession_,
      long_note_mode::valueFromId(context.settings.selectedLnMode));
  bars_.configure({.modeFilter = context.settings.skinModeFilterName,
                   .difficultyFilter =
                       context.settings.skinDifficultyFilterName,
                   .sortId = context.settings.skinSortId});
  bars_.refresh(MusicSelectRepositoryProjection{}.projectRoot(
      repositoryMetadata_, searchHistory_.entries(), libraryRevision_));
  if (preserveDirectory) {
    for (const auto &directoryId : previous.directory) {
      const auto current = bars_.snapshot();
      const auto found = std::ranges::find(current.rows, directoryId,
                                           &MusicSelectBar::id);
      if (found == current.rows.end()) break;
      if (!found->childrenLoaded && !loadDirectoryChildren(*found)) break;
      if (!bars_.select(directoryId) || !bars_.open(directoryId)) break;
    }
    if (previousSelection) (void)bars_.select(*previousSelection);
  }
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  cancelSelectedChartAnalysis();
  selectedChartInformation_.reset();
  selectedChartAnalysisStarted_ = false;
#endif
  const auto snapshot = bars_.snapshot();
  // Preload the newly selected chart (parse + jukebox) in the background so a
  // heavy archive chart is ready by the time the user presses Start.
  startPreloadForSelection();
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
  rankingCacheKey_ = musicSelectRankingCacheKey(
      *rankingRequest_, irAccountEvidenceRevision_);

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
  // PlayerConfig.getLnmode(), consumed by SkinBar's LN label selection, is
  // zero-based: LN, CN, HCN. The gameplay model uses the distinct 1/2/3
  // representation internally.
  frame.songList.playerLnMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode) -
      long_note_mode::kLnValue;
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
  // Beatoraja property 30 is setter-only and always reads as an empty string.
  propertyRuntime.searchWord.clear();
  const auto tableContext = musicSelectTableContextForLaunch(snapshot);
  propertyRuntime.tableName = tableContext.name;
  propertyRuntime.tableLevel = tableContext.level;
  propertyRuntime.tableFullName = tableContext.fullName;
  propertyRuntime.irName = context.settings.irProviders.empty()
                               ? std::string{}
                               : context.settings.irProviders.begin()->first;
  propertyRuntime.irUserName = context.irAccountNameSnapshot();
  propertyRuntime.irOnline = !propertyRuntime.irUserName.empty();
  propertyRuntime.ranking = ranking_;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  propertyRuntime.selectedSongInformation = selectedChartInformation_.get();
  frame.gameplayGraph.chart = selectedChartInformation_;
#endif
  if (rankingLoadAtMicros_ != -1) {
    propertyRuntime.ranking.pendingDurationMillis =
        (rankingLoadAtMicros_ - elapsedMicros()) / 1'000;
  } else {
    propertyRuntime.ranking.pendingDurationMillis = -1;
  }
  propertyRuntime.playerHistory = playerHistory_;
  if (snapshot.selectedIndex < snapshot.rows.size()) {
    const auto &selected = snapshot.rows[snapshot.selectedIndex];
    if (selected.chart) {
      const auto &record = *selected.chart;
      const auto &meta = record.meta;
      frame.stageFile = resolveChartAsset(record, meta.StageFile);
      frame.banner = resolveChartAsset(record, meta.Banner);
    }
  }
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
  return frame;
}

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
void MusicSelectScene::cancelSelectedChartAnalysis() {
  ++selectedChartAnalysisGeneration_;
  if (selectedChartAnalysis_ != nullptr) {
    selectedChartAnalysis_->cancelled.store(true, std::memory_order_release);
  }
  selectedChartAnalysis_.reset();
}

void MusicSelectScene::updateSelectedChartAnalysis() {
  if (selectedChartAnalysis_ != nullptr &&
      selectedChartAnalysis_->finished.load(std::memory_order_acquire)) {
    const auto analysis = std::exchange(selectedChartAnalysis_, {});
    if (!analysis->cancelled.load(std::memory_order_acquire) &&
        analysis->generation == selectedChartAnalysisGeneration_) {
      std::scoped_lock lock(analysis->mutex);
      selectedChartInformation_ = std::move(analysis->result);
    }
  }
  if (selectedChartAnalysisStarted_ || selectedChartAnalysis_ != nullptr ||
      launching_ ||
      elapsedMicros() <= songBarChangeMicros_ + 350'000) {
    return;
  }

  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song ||
      !selected.presentation.exists || !selected.chart) {
    return;
  }

  // MusicSelector starts this BMS model load after notesGraphDuration and
  // does not retry it until another selected-bar transition.
  selectedChartAnalysisStarted_ = true;
  const auto path = selected.chart->meta.BmsPath;
  const int longNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  auto analysis = std::make_shared<SelectedChartAnalysis>();
  analysis->generation = selectedChartAnalysisGeneration_;
  selectedChartAnalysis_ = analysis;
  // MusicSelector starts a detached model-load thread after its 350 ms
  // debounce. A later bar move must not wait for that previous model before
  // it can render the next selection; publication is gated by this mailbox's
  // current selection generation instead.
  std::thread([path, longNoteMode, analysis] {
    std::shared_ptr<const SkinGameplayChartGraphState> result;
    try {
      auto chart = play_options::parseChart(
          path, analysis->cancelled, "music-select chart information");
      if (chart && !analysis->cancelled.load(std::memory_order_acquire)) {
        applyEffectiveLongNoteModeToChart(*chart, longNoteMode);
        auto model = buildPlayfieldChartVisualModel(*chart, longNoteMode);
        if (!analysis->cancelled.load(std::memory_order_acquire)) {
          result = std::make_shared<const SkinGameplayChartGraphState>(
              std::move(model.skinGameplayGraph));
        }
      }
    } catch (...) {
    }
    if (!analysis->cancelled.load(std::memory_order_acquire)) {
      std::scoped_lock lock(analysis->mutex);
      analysis->result = std::move(result);
    }
    analysis->finished.store(true, std::memory_order_release);
  }).detach();
}

TextInputBox *MusicSelectScene::skinTextInputForSize(int requestedSize) {
  const int size = std::max(1, requestedSize);
  const auto existing = skinTextInputs_.find(size);
  if (existing != skinTextInputs_.end()) return existing->second;
  auto *input = new TextInputBox(kFontPath, size);
  input->setPositionType(YGPositionTypeAbsolute);
  input->setZIndex(9000);
  input->setPadding(Edge::All, 0);
  input->setVAlign(TextView::MIDDLE);
  input->setVisible(false);
  input->onSubmit([input](const std::string &) { input->endEditing(); });
  input->onEditingFinished(
      [this](const std::string &value) { commitSkinTextEditing(value); });
  addView(input);
  skinTextInputs_.insert_or_assign(size, input);
  return input;
}

void MusicSelectScene::beginSkinTextEditing(
    const skin::MusicSelectSkinPointerResult::StringFocus &focus) {
  if (skinTextInput_ != nullptr && skinTextInput_->getSelected()) {
    skinTextInput_->endEditing();
  }
  skinTextInput_ = skinTextInputForSize(
      static_cast<int>(std::lround(focus.bounds.height)));
  skinTextInput_->setEditingText(focus.currentValue);
  activeSkinStringWriter_ = focus.writer;
  const auto colorChannel = [](float value) {
    return static_cast<Uint8>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
  };
  skinTextInput_->setColor(
      {colorChannel(focus.rgba[0]), colorChannel(focus.rgba[1]),
       colorChannel(focus.rgba[2]), colorChannel(focus.rgba[3])});
  skinTextInput_->setEditingText(focus.currentValue);
  skinTextInput_->setSize(static_cast<int>(std::lround(focus.bounds.width)),
                          static_cast<int>(std::lround(focus.bounds.height)));
  skinTextInput_->setPositionNoLayout(
      static_cast<int>(std::lround(focus.bounds.x)),
      static_cast<int>(std::lround(focus.bounds.y)),
      YGPositionTypeAbsolute);
  skinTextInput_->setVisible(true);
  skinTextInput_->beginEditing();
}

void MusicSelectScene::commitSkinTextEditing(const std::string &value) {
  const auto writer = std::exchange(activeSkinStringWriter_, std::nullopt);
  if (writer && skinSession_) {
    (void)skinSession_->queueStringWrite(*writer, value);
  }
  if (skinTextInput_ != nullptr) skinTextInput_->setVisible(false);
}

void MusicSelectScene::applySkinPointerResult(
    const skin::MusicSelectSkinPointerResult &pointer,
    MusicSelectPointerOrigin origin) {
  if (pointer.focusedStringWriter) {
    beginSkinTextEditing(*pointer.focusedStringWriter);
  }
  if (pointer.closeDirectory) closeDirectory();
  if (!pointer.selectIndex) return;
  const auto snapshot = bars_.snapshot();
  if (*pointer.selectIndex >= snapshot.rows.size()) return;
  const auto &clicked = snapshot.rows[*pointer.selectIndex];
  const bool activate = musicSelectPointerActivatesRow(origin);
  if (skin::musicSelectIsDirectoryBarKind(clicked.kind)) {
    if (activate) {
      (void)openDirectory(clicked);
      syncResolvedFilters();
    } else {
      (void)bars_.select(clicked.id);
    }
    selectedBarMoved();
  } else if (musicSelectPointerKeepsCenteredBar(clicked.kind) && activate) {
    launchSelected();
  }
}

bool MusicSelectScene::queueSkinPointerEvent(SDL_Event &event) {
  if (!skinSession_) return false;
  UiLogicalPoint point;
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.which == SDL_TOUCH_MOUSEID) return false;
    rendering::screenToUi(event.button.x * rendering::widthScale,
                          event.button.y * rendering::heightScale,
                          point.x, point.y);
    const auto pointer = skinSession_->queuePointerDown(
        point, static_cast<int>(event.button.button) - 1, steadyMicros());
    applySkinPointerResult(pointer, MusicSelectPointerOrigin::Mouse);
    return pointer.consumed;
  }
  case SDL_MOUSEMOTION:
    if (event.motion.which == SDL_TOUCH_MOUSEID || event.motion.state == 0) {
      return false;
    }
    rendering::screenToUi(event.motion.x * rendering::widthScale,
                          event.motion.y * rendering::heightScale,
                          point.x, point.y);
    return skinSession_->queuePointerDrag(point, steadyMicros());
  case SDL_FINGERDOWN: {
    if (sdl_pointer_event::isMouseSynthesizedTouch(event)) return false;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, point.x,
                              point.y);
    const auto target = skinSession_->pointerTargetAt(point);
    if (target.kind == skin::MusicSelectSkinPointerTargetKind::Bar) {
      return skinTouchGesture_.begin(event.tfinger.fingerId, event.tfinger.x,
                                     event.tfinger.y,
                                     MusicSelectTouchTarget::Bar);
    }
    if (target.kind == skin::MusicSelectSkinPointerTargetKind::None &&
        skinTouchGesture_.begin(event.tfinger.fingerId, event.tfinger.x,
                                event.tfinger.y,
                                MusicSelectTouchTarget::Navigation)) {
      return true;
    }
    const auto pointer =
        skinSession_->queuePointerDown(point, 0, steadyMicros());
    applySkinPointerResult(pointer, MusicSelectPointerOrigin::Touch);
    // The authored text target has just created and focused the native input.
    // Forward its initiating touch too, so platform text controls retain the
    // same touch lifecycle as a directly touched TextInputBox.
    if (pointer.focusedStringWriter && skinTextInput_ != nullptr) {
      (void)skinTextInput_->handleEvents(event);
    }
    if (target.kind == skin::MusicSelectSkinPointerTargetKind::Slider &&
        pointer.consumed) {
      (void)skinTouchGesture_.begin(event.tfinger.fingerId, event.tfinger.x,
                                    event.tfinger.y,
                                    MusicSelectTouchTarget::Slider);
    }
    return pointer.consumed;
  }
  case SDL_FINGERMOTION: {
    if (sdl_pointer_event::isMouseSynthesizedTouch(event)) return false;
    const auto motion =
        skinTouchGesture_.move(event.tfinger.fingerId, event.tfinger.x,
                               event.tfinger.y);
    if (!motion.accepted) return false;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, point.x,
                              point.y);
    if (motion.sliderDrag) {
      (void)skinSession_->queuePointerDrag(point, steadyMicros());
    }
    if (motion.rowDelta != 0) {
      const int duration = context.settings.skinMusicSelectScrollDurationLow;
      const bool increase = motion.rowDelta > 0;
      const int direction = increase ? duration : -duration;
      const auto deadline = unixMillis() + duration;
      for (int count = std::abs(motion.rowDelta); count > 0; --count) {
        bars_.move(increase, direction, deadline);
      }
      selectedBarMoved();
    }
    return true;
  }
  case SDL_FINGERUP: {
    if (sdl_pointer_event::isMouseSynthesizedTouch(event)) return false;
    const auto release = skinTouchGesture_.end(event.tfinger.fingerId);
    if (!release.accepted) return false;
    if (release.goBack) {
      closeDirectory();
      return true;
    }
    if (release.tap) {
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, point.x,
                                point.y);
      const auto pointer =
          skinSession_->queuePointerDown(point, 0, steadyMicros());
      applySkinPointerResult(pointer, MusicSelectPointerOrigin::Touch);
    }
    return true;
  }
  default:
    return false;
  }
}
#endif

EventHandleResult MusicSelectScene::handleEvents(SDL_Event &event) {
  if (failed_) return Scene::handleEvents(event);
  // While a chart is launching, the decide overlay blocks all input so the
  // user cannot scroll or change selection mid-launch.
  if (launching_ && decideOverlay_ != nullptr &&
      decideOverlay_->getVisible()) {
    (void)decideOverlay_->handleEvents(event);
    return {};
  }
  if (recordsModal_ != nullptr && recordsModal_->isVisible()) {
    (void)recordsModal_->handleEvents(event);
    return {};
  }
  if (tasksModal_ != nullptr && tasksModal_->getVisible()) {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        event.key.keysym.sym == SDLK_ESCAPE) {
      tasksModal_->setVisible(false);
      return {};
    }
    (void)tasksModal_->handleEvents(event);
    return {};
  }
  if (playOptionsModal_ != nullptr && playOptionsModal_->root() != nullptr &&
      playOptionsModal_->root()->getVisible()) {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        event.key.keysym.sym == SDLK_ESCAPE) {
      playOptionsModal_->hide();
      return {};
    }
    (void)playOptionsModal_->root()->handleEvents(event);
    return {};
  }
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr && skinTextInput_->getSelected() &&
      event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
      event.key.keysym.sym == SDLK_ESCAPE) {
    skinTextInput_->endEditing();
  }
  if (skinTextInput_ != nullptr && skinTextInput_->getSelected() &&
      !skinTextInput_->handleEvents(event)) {
    return {};
  }
  if (queueSkinPointerEvent(event)) return {};
#endif

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
  return {};
}

void MusicSelectScene::openSelected() {
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (skin::musicSelectIsDirectoryBarKind(selected.kind)) {
    (void)openDirectory(selected);
    syncResolvedFilters();
    selectedBarMoved();
  } else {
    launchSelected();
  }
}

bool MusicSelectScene::openDirectory(const MusicSelectBar &directory) {
  if (!skin::musicSelectIsDirectoryBarKind(directory.kind)) return false;
  if (!directory.childrenLoaded && !loadDirectoryChildren(directory)) {
    return false;
  }
  const bool opened = bars_.open(directory.id);
  if (opened && systemSound_) systemSound_->playFolderOpen();
  return opened;
}

bool MusicSelectScene::loadDirectoryChildren(
    const MusicSelectBar &directory) {
  if (!chartSession_) return false;
  const int selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  const auto inputFor = [this, selectedLongNoteMode](
                            std::span<const ChartMetaRecord> records,
                            const MusicSelectRepositoryMetadata *metadata = nullptr,
                            std::span<const MusicSelectSearchSource> searches = {},
                            const RecentScoreImprovements *improvements = nullptr) {
    return MusicSelectRepositoryProjectionInput{
        .records = records,
        .scoreFor = [this](const bms_parser::ChartMeta &meta, int mode) {
          return scoreCache_.bestFor(meta, mode);
        },
        .replayExistsFor = [this](const ChartMetaRecord &record, int mode) {
          return musicSelectExistingChartReplaySlots(
              record, mode, context.replayRepository.GetResolvedProfileRoot());
        },
        .courseScoresFor = [this](std::string_view courseKey, int courseId,
                                  int mode, bool doublePlay) {
          return context.scoreRepository.LoadCourseSelectorOptionScores(
              courseKey, courseId, mode, doublePlay);
        },
        .courseReplayExistsFor = [this](const MusicSelectBar &bar, int mode) {
          return musicSelectExistingCourseReplaySlots(
              bar, mode, context.replayRepository.GetResolvedProfileRoot());
        },
        .metadata = metadata,
        .searches = searches,
        .recentScoreImprovements = improvements,
        .modeFilter = context.settings.skinModeFilterName,
        .selectedLongNoteMode = selectedLongNoteMode,
        .repositoryRevision = libraryRevision_};
  };

  std::vector<MusicSelectBar> children;
  switch (directory.kind) {
  case skin::MusicSelectBarKind::Folder: {
    ChartMetaQuery query;
    query.exactFolder = directory.directoryPath;
    query.selectedLongNoteMode = selectedLongNoteMode;
    std::vector<ChartMetaRecord> records;
    chartSession_->QueryChartMeta(query, records);
    if (!records.empty()) {
      const auto projection =
          MusicSelectRepositoryProjection{}.project(inputFor(records));
      children = projectionChildren(projection, directory.id);
      break;
    }

    std::set<std::filesystem::path> paths;
    const auto parent = directory.directoryPath.lexically_normal();
    for (const auto &record : repositoryMetadata_.folders) {
      const auto candidate =
          std::filesystem::path(record.path).lexically_normal();
      const auto relative = candidate.lexically_relative(parent);
      if (relative.empty() || relative.is_absolute()) continue;
      const auto first = relative.begin();
      if (first == relative.end() || *first == "." || *first == "..") {
        continue;
      }
      paths.insert(parent / *first);
    }
    children.reserve(paths.size());
    for (const auto &path : paths) {
      std::int64_t addDateSeconds = 0;
      if (const auto found = std::ranges::find_if(
              repositoryMetadata_.folders, [&](const auto &record) {
                return std::filesystem::path(record.path).lexically_normal() ==
                       path;
              });
          found != repositoryMetadata_.folders.end()) {
        addDateSeconds = found->addDateSeconds;
      }
      const std::string title = path.filename().empty()
                                    ? fspath_to_utf8(path)
                                    : fspath_to_utf8(path.filename());
      children.push_back({
          .id = {"folder:" + fspath_to_utf8(path)},
          .kind = skin::MusicSelectBarKind::Folder,
          .title = title,
          .directoryPath = path,
          .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                           .title = title,
                           .exists = true,
                           .addDateSeconds = addDateSeconds},
          .selectable = true,
          .sortable = true,
          .childrenLoaded = false,
      });
    }
    break;
  }
  case skin::MusicSelectBarKind::Table: {
    if (directory.tableId == 0) break;
    const auto table = MusicSelectRepositoryProjection::loadTableMetadata(
        *chartSession_, directory.tableId, selectedLongNoteMode);
    if (!table) return false;
    MusicSelectRepositoryMetadata metadata;
    metadata.tables.push_back(*table);
    const auto projection = MusicSelectRepositoryProjection{}.project(
        inputFor({}, &metadata));
    children = projectionChildren(projection, directory.id);
    for (auto &child : children) {
      if (child.kind == skin::MusicSelectBarKind::Hash) {
        child.childrenLoaded = false;
      }
    }
    break;
  }
  case skin::MusicSelectBarKind::Hash: {
    MusicSelectDifficultyTableSource table{
        .info = {.id = directory.tableId}};
    table.levels.push_back(
        {.info = {.tableId = directory.tableId,
                  .level = directory.tableLevel}});
    ChartMetaQuery query;
    query.tableId = directory.tableId;
    query.tableLevel = directory.tableLevel;
    query.selectedLongNoteMode = selectedLongNoteMode;
    table.levels.front().records.clear();
    chartSession_->QueryChartMeta(query, table.levels.front().records);
    MusicSelectRepositoryMetadata metadata;
    metadata.tables.push_back(std::move(table));
    const auto projection = MusicSelectRepositoryProjection{}.project(
        inputFor({}, &metadata));
    children = projectionChildren(projection, directory.id);
    break;
  }
  case skin::MusicSelectBarKind::Container: {
    const bool lamp = directory.id.value == "container:lamp-update";
    for (int day = 0; day < 30; ++day) {
      const std::string title =
          day == 0 ? "TODAY" : std::to_string(day) + "DAYS AGO";
      children.push_back({
          .id = {"command:" +
                 std::string(lamp ? "lamp-update" : "score-update") +
                 ":" + std::to_string(day)},
          .kind = skin::MusicSelectBarKind::Command,
          .title = title,
          .presentation = {.kind = skin::MusicSelectBarKind::Command,
                           .title = title,
                           .exists = true},
          .selectable = true,
          .sortable = true,
          .childrenLoaded = false,
      });
    }
    break;
  }
  case skin::MusicSelectBarKind::Command: {
    if (!recentScoreImprovementsLoaded_) {
      recentScoreImprovements_ = context.scoreRepository.LoadRecentScoreImprovements(
          unixMillis() / 1'000, selectedLongNoteMode);
      recentScoreImprovementsLoaded_ = true;
    }
    std::vector<ChartMetaRecord> records;
    ChartMetaQuery query;
    query.selectedLongNoteMode = selectedLongNoteMode;
    chartSession_->QueryChartMeta(query, records);
    const auto projection = MusicSelectRepositoryProjection{}.project(
        inputFor(records, nullptr, {}, &recentScoreImprovements_));
    children = projectionChildren(projection, directory.id);
    break;
  }
  case skin::MusicSelectBarKind::SearchWord: {
    constexpr std::string_view prefix = "search:";
    if (!directory.id.value.starts_with(prefix)) return false;
    MusicSelectSearchSource source{
        .text = directory.id.value.substr(prefix.size())};
    ChartMetaQuery query;
    query.keyword = source.text;
    query.selectedLongNoteMode = selectedLongNoteMode;
    chartSession_->QueryChartMeta(query, source.records);
    const std::array<MusicSelectSearchSource, 1> searches{std::move(source)};
    const auto projection = MusicSelectRepositoryProjection{}.project(
        inputFor({}, nullptr, searches));
    children = projectionChildren(projection, directory.id);
    break;
  }
  case skin::MusicSelectBarKind::SameFolder:
  case skin::MusicSelectBarKind::Song:
  case skin::MusicSelectBarKind::Executable:
  case skin::MusicSelectBarKind::Grade:
  case skin::MusicSelectBarKind::RandomCourse: return false;
  }
  return bars_.installChildren(directory.id, std::move(children));
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr) skinTextInput_->endEditing();
  skinTouchGesture_.cancel();
#endif
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
  reloadLibrary(false);
  (void)bars_.select({"search:" + selectedText});
  selectedBarMoved();
}

void MusicSelectScene::closeDirectory() {
  if (bars_.close()) {
    if (systemSound_) systemSound_->playFolderClose();
    syncResolvedFilters();
    selectedBarMoved();
  } else {
    executeEvent({.kind = skin::MusicSelectSkinActionKind::Event,
                  .selector = {.value = 12},
                  .arguments = {0, 0}});
  }
}

void MusicSelectScene::startPreloadForSelection() {
  if (launching_) {
    return;
  }
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart ||
      selected.chart->solidArchive || selected.chart->unavailable ||
      selected.chart->meta.BmsPath.empty()) {
    return;
  }
  const auto record = *selected.chart;
  {
    std::lock_guard<std::mutex> lock(preloadMutex_);
    if (fspath_to_path_t(preloadedPath_) ==
            fspath_to_path_t(record.meta.BmsPath) &&
        preloadedChart_ != nullptr &&
        fspath_to_path_t(preloadedChart_->Meta.BmsPath) ==
            fspath_to_path_t(record.meta.BmsPath)) {
      return;  // this exact chart is already preloaded
    }
    // Selection changed to a different chart. Invalidate the stale result
    // immediately so reuse can never pair a previously-loaded chart with the
    // current selection.
    preloadedChart_.reset();
    preloadedPath_ = record.meta.BmsPath;
  }
  if (preloadWorker_ != nullptr) {
    preloadWorker_->request(record);
  }
}

void MusicSelectScene::stopPreloadWorker() {
  if (preloadWorker_ != nullptr) {
    preloadWorker_->stop();
  }
  std::lock_guard<std::mutex> lock(preloadMutex_);
  preloadedChart_.reset();
  preloadedPath_.clear();
}

void MusicSelectScene::showDecideOverlay(const ChartMetaRecord &record) {
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setChart(record);
    decideOverlay_->setVisible(true);
  }
}

void MusicSelectScene::hideDecideOverlay() {
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setVisible(false);
  }
}

void MusicSelectScene::tryCompletePendingPreloadLaunch() {
  if (!launching_ || !pendingLaunch_.has_value()) {
    return;
  }
  const PendingPreloadLaunch pending = std::move(*pendingLaunch_);
  pendingLaunch_.reset();
  const auto &record = pending.record;
  bms_parser::Chart *preparedChartRaw = nullptr;
  play_options::PlayOptionReplayInfo preloadedPlayInfo;
  int preloadedLnMode = 0;
  if (!reusePreloadedChart(record, preparedChartRaw, preloadedPlayInfo,
                          preloadedLnMode)) {
    // If the worker is still loading this chart, keep waiting (the decide
    // overlay stays up) instead of re-entering launchSelected, which would
    // begin a fresh startup session every frame.
    if (preloadWorker_ != nullptr &&
        preloadWorker_->isRequesting(fspath_to_utf8(record.meta.BmsPath))) {
      pendingLaunch_ = pending;
      return;
    }
    // The preload worker finished without publishing this chart (parse
    // failure, superseded, or cancelled). Fall back to the launch thread so
    // the launch is not dropped silently.
    launching_ = false;
    launchSelected(pending.autoplay, pending.practice);
    return;
  }
  // Stop the preload worker before gameplay starts so it cannot keep touching
  // the jukebox while GamePlayScene uses it.
  stopPreloadWorker();
  StartupTiming::instance().mark("reused preloaded chart (no parse/load at start)");
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  const auto snapshot = bars_.snapshot();
  const auto tableContext = musicSelectTableContextForLaunch(snapshot);
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const bool doublePlayFlip = context.settings.skinDoublePlayOption == 1;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode};
  const bool clubMode = context.settings.gameplayClubModeEnabled;
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = autoKeySound,
      .autoPlay = pending.autoplay,
      .gaugeType = selections.gaugeType,
      .gaugeAutoShift = selections.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
      .playOption = preloadedPlayInfo.option,
      .playOptionSeed = preloadedPlayInfo.seed,
      .playOption2 = preloadedPlayInfo.option2,
      .playOption2Seed = preloadedPlayInfo.seed2,
      .doublePlayFlip = doublePlayFlip,
      .longNoteMode = preloadedLnMode,
      .assistOption = selections.assistOption,
      .pacemakerTarget = selections.pacemakerTarget,
      .tableName = tableContext.name,
      .tableLevel = tableContext.level,
      .practiceMode = pending.practice,
      .playback = playback,
      .clubMode = clubMode,
      .returnScene = this,
      .ruleset = selections.ruleset};
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::unique_ptr<
                                                  bms_parser::Chart>(
                                                  preparedChartRaw),
                                      std::move(options)),
      true);
  launching_ = false;
}

bool MusicSelectScene::reusePreloadedChart(
    const ChartMetaRecord &record, bms_parser::Chart *&chart,
    play_options::PlayOptionReplayInfo &playInfo, int &lnMode) {
  std::unique_ptr<bms_parser::Chart> cached;
  {
    std::lock_guard<std::mutex> lock(preloadMutex_);
    // The optimistic preloadedPath_ is set as soon as a selection changes;
    // only reuse when the actual loaded chart matches the requested one so a
    // stale preload can never pair the wrong chart/audio with this selection.
    if (preloadedChart_ == nullptr ||
        fspath_to_path_t(preloadedPath_) !=
            fspath_to_path_t(record.meta.BmsPath) ||
        fspath_to_path_t(preloadedChart_->Meta.BmsPath) !=
            fspath_to_path_t(record.meta.BmsPath)) {
      return false;
    }
    cached = std::move(preloadedChart_);
    preloadedPath_.clear();
  }
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  if (!play_options::applyPlayOptionModifier(
          *cached, selections.playOption, std::nullopt, 0, playInfo.option,
          playInfo.seed, "music-select")) {
    return false;
  }
  if (cached->Meta.IsDP) {
    const auto player2 = replay::beatorajaReplayOptionName(
        context.settings.skinPlayer2RandomOption);
    if (!player2 || !play_options::applyPlayOptionModifier(
                        *cached, std::string(*player2), std::nullopt, 1,
                        playInfo.option2, playInfo.seed2, "music-select")) {
      return false;
    }
  }
  lnMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
  if (lnMode == 0) {
    lnMode = long_note_mode::valueFromId(selections.longNoteMode);
  }
  applyEffectiveLongNoteModeToChart(*cached, lnMode);
  chart = cached.release();
  return true;
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
  StartupTiming::instance().beginSession();
  StartupTiming::instance().mark("selector start press");
  const auto record = *selected.chart;
  if (record.unavailable || record.solidArchive ||
      record.meta.BmsPath.empty()) {
    return;
  }
  if (systemSound_) systemSound_->playDecide();
  showDecideOverlay(record);
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  const auto tableContext = musicSelectTableContextForLaunch(snapshot);
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const bool doublePlayFlip = context.settings.skinDoublePlayOption == 1;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode};
  const bool clubMode = context.settings.gameplayClubModeEnabled;

  // Reuse the background-preloaded chart (parse + jukebox already done) so
  // Start is near-instant for a chart the user had selected while browsing.
  bms_parser::Chart *preparedChartRaw = nullptr;
  play_options::PlayOptionReplayInfo preloadedPlayInfo;
  int preloadedLnMode = 0;
  if (reusePreloadedChart(record, preparedChartRaw, preloadedPlayInfo,
                          preloadedLnMode)) {
    // Stop the preload worker before gameplay starts so it cannot keep
    // touching the jukebox while GamePlayScene uses it.
    stopPreloadWorker();
    StartupTiming::instance().mark("reused preloaded chart (no parse/load at start)");
    if (!launching_) {
      launching_ = true;
      StartOptions options{
          .startPosition = 0,
          .autoKeySound = autoKeySound,
          .autoPlay = autoplay,
          .gaugeType = selections.gaugeType,
          .gaugeAutoShift = selections.gaugeAutoShift,
          .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
          .playOption = preloadedPlayInfo.option,
          .playOptionSeed = preloadedPlayInfo.seed,
          .playOption2 = preloadedPlayInfo.option2,
          .playOption2Seed = preloadedPlayInfo.seed2,
          .doublePlayFlip = doublePlayFlip,
          .longNoteMode = preloadedLnMode,
          .assistOption = selections.assistOption,
          .pacemakerTarget = selections.pacemakerTarget,
          .tableName = tableContext.name,
          .tableLevel = tableContext.level,
          .practiceMode = practice,
          .playback = playback,
          .clubMode = clubMode,
          .returnScene = this,
          .ruleset = selections.ruleset};
      context.sceneManager->changeScene(
          std::make_unique<GamePlayScene>(context, std::unique_ptr<
                                                  bms_parser::Chart>(
                                                  preparedChartRaw),
                                          std::move(options)),
          true);
      launching_ = false;
    }
    return;
  }

  // Start was pressed before the preload finished but the worker is still
  // loading this exact chart: keep it running (don't re-do the parse + jukebox
  // load) and launch from its result when it lands. The decide overlay stays
  // up while the load completes.
  if (!launching_ && preloadWorker_ != nullptr &&
      preloadWorker_->isRequesting(fspath_to_utf8(record.meta.BmsPath))) {
    launching_ = true;
    pendingLaunch_ = PendingPreloadLaunch{record, autoplay, practice};
    StartupTiming::instance().mark("waiting for in-flight preload to complete");
    return;
  }

  launching_ = true;
  stopPreloadWorker();
  if (launchThread_.joinable()) {
    launchThread_.join();
  }
  launchCancelled_.store(false, std::memory_order_release);
  launchThread_ = std::jthread(
      [this, record, selections, autoKeySound, doublePlayFlip, playback,
       clubMode, practice, autoplay, tableContext]() mutable {
        auto resetLaunching = [this]() {
          defer([this]() {
            launching_ = false;
            hideDecideOverlay();
            return true;
          }, 0, true);
        };
        std::atomic_bool cancelled = false;
        auto chart = play_options::parseChart(record.meta, cancelled,
                                              "music-select start");
        if (!chart || cancelled ||
            launchCancelled_.load(std::memory_order_acquire)) {
          resetLaunching();
          return;
        }
        play_options::PlayOptionReplayInfo playInfo;
        if (!play_options::applyPlayOptionModifier(
                *chart, selections.playOption, std::nullopt, 0,
                playInfo.option, playInfo.seed, "music-select")) {
          resetLaunching();
          return;
        }
        if (chart->Meta.IsDP) {
          const auto player2 = replay::beatorajaReplayOptionName(
              context.settings.skinPlayer2RandomOption);
          if (!player2 ||
              !play_options::applyPlayOptionModifier(
                  *chart, std::string(*player2), std::nullopt, 1,
                  playInfo.option2, playInfo.seed2, "music-select")) {
            resetLaunching();
            return;
          }
        }
        int lnMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
        if (lnMode == 0) {
          lnMode = long_note_mode::valueFromId(selections.longNoteMode);
        }
        applyEffectiveLongNoteModeToChart(*chart, lnMode);
        // The jukebox load (archive extraction + audio/BGA decode) is the
        // chart-dependent cost and can take seconds for a heavy chart. Run it
        // off the UI thread like the parse; the selector's only jukebox use
        // during launching_ is this load (preview uses its own audio
        // service), so it cannot race the UI thread.
        context.jukebox.stop();
        (void)context.jukebox.loadChart(*chart, true, cancelled);
        if (cancelled || launchCancelled_.load(std::memory_order_acquire)) {
          resetLaunching();
          return;
        }
        defer(
            [this,
             preparedChart =
                 std::make_shared<std::unique_ptr<bms_parser::Chart>>(
                     std::move(chart)),
             playInfo = std::move(playInfo), lnMode, selections, autoKeySound,
             doublePlayFlip, playback, clubMode, practice, autoplay,
             tableContext]() mutable {
              if (!launching_) {
                return true;
              }
              StartupTiming::instance().mark("parse + jukebox load done, changing scene");
              StartOptions options{
                  .startPosition = 0,
                  .autoKeySound = autoKeySound,
                  .autoPlay = autoplay,
                  .gaugeType = selections.gaugeType,
                  .gaugeAutoShift = selections.gaugeAutoShift,
                  .gaugeAutoShiftLowerBound =
                      selections.gaugeAutoShiftLowerBound,
                  .playOption = playInfo.option,
                  .playOptionSeed = playInfo.seed,
                  .playOption2 = playInfo.option2,
                  .playOption2Seed = playInfo.seed2,
                  .doublePlayFlip = doublePlayFlip,
                  .longNoteMode = lnMode,
                  .assistOption = selections.assistOption,
                  .pacemakerTarget = selections.pacemakerTarget,
                  .tableName = tableContext.name,
                  .tableLevel = tableContext.level,
                  .practiceMode = practice,
                  .playback = playback,
                  .clubMode = clubMode,
                  .returnScene = this,
                  .ruleset = selections.ruleset};
              context.sceneManager->changeScene(
                  std::make_unique<GamePlayScene>(context, std::move(*preparedChart),
                                                  std::move(options)),
                  true);
              launching_ = false;
              return true;
            },
            0, true);
      });
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
  const auto tableContext = musicSelectTableContextForLaunch(bars_.snapshot());
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
      .tableName = tableContext.name,
      .tableLevel = tableContext.level,
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
  auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto directory = snapshot.rows[snapshot.selectedIndex];
  if (!directory.childrenLoaded && !loadDirectoryChildren(directory)) return;
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

void MusicSelectScene::launchCourseReplay(
    const MusicSelectBar &course, int slot,
    const MusicSelectBarManagerSnapshot &snapshot) {
  const int lnMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  const auto paths = musicSelectCourseReplaySlotPaths(course, lnMode);
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
    SDL_Log("Unable to read course replay slot inventory: %s",
            inventory.diagnostic.c_str());
    return;
  }
  const auto resultId =
      musicSelectCourseReplayResultId(inventory.entries, slotPath);
  if (!resultId) return;
  auto stored = context.replayRepository.LoadModernCourseResult(*resultId);
  if (stored.status != ModernCourseResultReadStatus::Loaded ||
      !stored.record) {
    SDL_Log("Unable to read course replay slot result: %s",
            stored.diagnostic.c_str());
    return;
  }

  std::vector<std::filesystem::path> chartPaths;
  chartPaths.reserve(stored.record->result.stages.size());
  for (std::size_t index = 0;
       index < stored.record->result.stages.size(); ++index) {
    if (index >= course.courseCharts.size()) return;
    chartPaths.push_back(course.courseCharts[index].meta.BmsPath);
  }

  launching_ = true;
  std::atomic_bool cancelled = false;
  auto consumer =
      replay::makeRuntimeCourseReplayConsumer(context.replayRepository);
  auto loaded = consumer.load(*stored.record, chartPaths, cancelled);
  if (!loaded.ready() || cancelled) {
    SDL_Log("Unable to prepare course replay slot: %s",
            loaded.diagnostic.c_str());
    launching_ = false;
    return;
  }
  auto session = replay::makeCourseReplayLaunchSession(
      std::move(loaded), replay::CourseReplayLaunchMode::Watch);
  if (session == nullptr ||
      !session->hasCourseReplayStage(session->currentIndex)) {
    launching_ = false;
    return;
  }
  auto stageReplay = session->currentCourseReplayStageReplay();
  if (stageReplay == nullptr) {
    launching_ = false;
    return;
  }
  session->applyReplayStagePlayOptions(*stageReplay);
  auto chart = session->takePreparedCourseChart(session->currentIndex);
  if (chart == nullptr) {
    launching_ = false;
    return;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*chart, true, cancelled);
  if (cancelled) {
    launching_ = false;
    return;
  }
  const auto tableContext = musicSelectTableContextForLaunch(snapshot);
  StartOptions options =
      makeCourseReplayStageStartOptions(session, stageReplay);
  options.tableName = tableContext.name;
  options.tableLevel = tableContext.level;
  options.returnScene = this;
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      true);
}

void MusicSelectScene::launchSelectedReplay(int slot) {
  if (launching_ || slot < 0 || slot >= 4) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind == skin::MusicSelectBarKind::Grade) {
    launchCourseReplay(selected, slot, snapshot);
    return;
  }
  if (!selected.chart) return;
  const auto record = *selected.chart;
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
  const auto tableContext = musicSelectTableContextForLaunch(snapshot);
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
      .tableName = tableContext.name,
      .tableLevel = tableContext.level,
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
  bool audioSettingsChanged = false;
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
      } else if ((id && *id == 17) || name == "mastervolume") {
        audioSettingsChanged =
            audioSettingsChanged ||
            context.settings.audioVideo.audio.masterVolume !=
                action.floatValue;
        context.settings.audioVideo.audio.masterVolume = action.floatValue;
      } else if ((id && *id == 18) || name == "keyvolume") {
        audioSettingsChanged =
            audioSettingsChanged ||
            context.settings.audioVideo.audio.keysoundVolume !=
                action.floatValue;
        context.settings.audioVideo.audio.keysoundVolume = action.floatValue;
      } else if ((id && *id == 19) || name == "bgmvolume") {
        audioSettingsChanged =
            audioSettingsChanged ||
            context.settings.audioVideo.audio.bgmVolume != action.floatValue;
        context.settings.audioVideo.audio.bgmVolume = action.floatValue;
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
  if (audioSettingsChanged) {
    (void)context.audioDeviceManager.apply(context.settings.audioVideo.audio);
    if (!context.saveSettings()) {
      SDL_Log("Failed to save music-select skin audio settings");
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
  case MusicSelectInputActionKind::ScratchSound:
    if (systemSound_) systemSound_->playScratch();
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
    if (const auto snapshot = bars_.snapshot();
        snapshot.selectedIndex < snapshot.rows.size()) {
      (void)openDirectory(snapshot.rows[snapshot.selectedIndex]);
    }
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
    // Pinned MusicSelectInputProcessor toggles PlayerConfig.scrollMode here;
    // this shortcut is distinct from event 400's enableConstant setting.
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
  case MusicSelectInputActionKind::OpenSettings:
    openSettings();
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
    case MusicSelectEventEffectKind::OptionChangeSound:
      if (systemSound_) systemSound_->playOptionChange();
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
        const auto isArchiveFile = [](const std::filesystem::path &path) {
          std::error_code error;
          return archive_file::hasSupportedArchiveExtension(path) &&
                 std::filesystem::is_regular_file(path, error) && !error;
        };
        if (const auto path = musicSelectExplorerPath(
                *selected, lookups, archive_file::splitVirtualPath,
                isArchiveFile)) {
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
      const auto tableId = selected != nullptr
                               ? musicSelectDifficultyTableUpdateId(*selected)
                               : std::nullopt;
      if (tableId && context.chartLibraryTasks) {
        context.chartLibraryTasks->enqueue({
            .kind = chart_library_tasks::TaskKind::UpdateDifficultyTable,
            .title = "Update Difficulty Table",
            .tableId = *tableId,
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
    case MusicSelectEventEffectKind::ApplyBgaEnabled:
      context.jukebox.setVisualsEnabled(effect.value != 0);
      break;
    default:
      break;
    }
  }
}

void MusicSelectScene::update(float) {
  if (failed_) return;
  tryCompletePendingPreloadLaunch();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  updateSelectedChartAnalysis();
#endif
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
  if (modalLayer_ != nullptr) {
    modalLayer_->setSize(rendering::window_width, rendering::window_height);
  }
  if (modalOverlayPortal_ != nullptr) {
    modalOverlayPortal_->setSize(rendering::window_width,
                                 rendering::window_height);
  }
  if (playOptionsModal_ != nullptr) {
    playOptionsModal_->resize(rendering::window_width,
                              rendering::window_height);
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->resize(rendering::window_width,
                          rendering::window_height);
    recordsModal_->update();
    applyRecordsExportProgress();
    applyRecordsExportResult();
  }
  if (tasksModal_ != nullptr) {
    tasksModal_->setSize(rendering::window_width, rendering::window_height);
    if (tasksModal_->getVisible()) refreshTasksModal();
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
  finalizeSkinPreparationIfReady();
  if (failed_ || !skinSession_) return;
  ++frameSerial_;
  auto frame = makeFrame();
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr) skinTextInput_->endEditing();
  skinTouchGesture_.cancel();
#endif
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
  if (skinLoadingView_ != nullptr) skinLoadingView_->setVisible(false);
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
  auto *settings = makeButton("Settings");
  settings->setHeight(64);
  settings->setOnClickListener([this] { openSettings(); });
  root->addView(settings);
  root->applyYogaLayout();
  addView(root);
  errorView_ = root;
}

void MusicSelectScene::openMusicPlayer() {
  context.sceneManager->changeScene(std::make_unique<MusicPlayerScene>(
      context, SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::openChartViewer() {
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart ||
      selected.chart->solidArchive || selected.chart->unavailable ||
      selected.chart->meta.BmsPath.empty()) {
    return;
  }
  context.jukebox.stop();
  context.sceneManager->changeScene(std::make_unique<ChartViewerScene>(
      context, *selected.chart, std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, SceneReturnTarget::Retained(this)), true);
}

void MusicSelectScene::openChartRecords() {
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (selected.kind != skin::MusicSelectBarKind::Song || !selected.chart ||
      selected.chart->solidArchive || selected.chart->unavailable ||
      selected.chart->meta.BmsPath.empty()) {
    return;
  }
  if (recordsModal_ == nullptr) {
    recordsModal_ = ReplayRecordsModal::Create(modalLayer_,
                                               makeRecordsModalCallbacks());
  }
  if (playOptionsModal_ != nullptr) playOptionsModal_->hide();
  if (tasksModal_ != nullptr) tasksModal_->setVisible(false);
  recordsModal_->setTouchVisualizationEnabled(
      context.settings.touchVisualizationEnabled);
  recordsModal_->showChart(*selected.chart);
}

std::vector<ResultRecordSummary>
MusicSelectScene::loadRecordsForSelector(const ChartMetaRecord &record) {
  std::vector<ResultRecordSummary> projected;
  const auto legacy = context.replayRepository.ListLegacyChartSummaries(
      record.meta, kMaximumLegacyResultSummaryRows);
  projected.reserve(legacy.size());
  for (const LegacyChartResultSummary &summary : legacy) {
    projected.push_back(makeLegacyChartResultRecord(summary));
  }
  if (!record.meta.SHA256.empty()) {
    const auto history = context.replayRepository.ListModernChartResults(
        record.meta.SHA256, kMaximumModernChartHistoryRows);
    if (history.status == ModernChartHistoryReadStatus::Loaded) {
      replay::ReplayFileActionService replayActions(context.replayRepository);
      projected.reserve(projected.size() + history.records.size());
      for (const ModernChartResultRecord &modern : history.records) {
        const auto inspected = replayActions.probe(modern.replayFile);
        projected.push_back(makeModernChartResultRecord(
            modern, replay::replayStateForFileAction(inspected.state),
            ir::IrRecordState::Hidden));
      }
    }
  }
  return mergeResultRecords(std::span<const ReplaySummary>{}, projected,
                            std::span<const ir::IrRemoteScore>{},
                            std::string_view{}, std::string_view{});
}

void MusicSelectScene::launchChartReplay(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
  if (launching_ || record.unavailable || record.solidArchive ||
      record.meta.BmsPath.empty()) {
    return;
  }
  launching_ = true;
  if (recordsModal_ != nullptr) {
    recordsModal_->setLoadInProgress(true);
  }
  std::atomic_bool cancelled = false;
  auto consumer =
      replay::makeRuntimeChartReplayConsumer(context.replayRepository);
  auto loaded = consumer.load(modern, record.meta.BmsPath, cancelled);
  if (!loaded.ready() || loaded.chart == nullptr || cancelled) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setLoadInProgress(false);
      recordsModal_->setStatus("Replay playback could not be prepared.");
    }
    launching_ = false;
    return;
  }
  context.jukebox.stop();
  context.jukebox.loadChart(*loaded.chart, true, cancelled);
  if (cancelled) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setLoadInProgress(false);
    }
    launching_ = false;
    return;
  }
  const bool renderTouchPoints =
      recordsModal_ != nullptr ? recordsModal_->renderTouchPoints() : false;
  const bool renderGhosts =
      recordsModal_ != nullptr ? recordsModal_->renderReplayGhosts() : true;
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = false,
      .autoPlay = false,
      .gaugeType = loaded.replayData->initialGaugeType,
      .gaugeAutoShift = loaded.replayData->gaugeAutoShift,
      .replayData = loaded.replayData,
      .pacemakerTarget = pacemaker::kTargetOff,
      .touchVisualizationEnabled = renderTouchPoints,
      .replayGhostRenderingEnabled = renderGhosts,
      .returnScene = this,
  };
  applyReplayProvenanceToStartOptions(options, *loaded.replayData);
  if (recordsModal_ != nullptr) {
    recordsModal_->setLoadInProgress(false);
    recordsModal_->hide();
  }
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(loaded.chart),
                                      std::move(options)),
      true);
  launching_ = false;
}

void MusicSelectScene::launchCourseReplay(
    const ChartMetaRecord &record, const ModernCourseResultRecord &modern) {
  if (launching_) {
    return;
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setStatus(
        "Course replays are available from the Main Menu.");
  }
}

void MusicSelectScene::launchAutoPlay(const ChartMetaRecord &record) {
  if (launching_ || record.courseStart || record.unavailable ||
      record.solidArchive || record.meta.BmsPath.empty()) {
    return;
  }
  launching_ = true;
  std::atomic_bool cancelled = false;
  auto chart = play_options::parseChart(record.meta, cancelled, "autoplay");
  if (!chart || cancelled) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setLoadInProgress(false);
      recordsModal_->setStatus("Autoplay chart could not be prepared.");
    }
    launching_ = false;
    return;
  }
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  const auto playInfo = play_options::applySelectedPlayOptions(
      *chart, selections.playOption);
  applyEffectiveLongNoteModeToChart(
      *chart, long_note_mode::valueFromId(selections.longNoteMode));
  context.jukebox.stop();
  context.jukebox.loadChart(*chart, true, cancelled);
  if (cancelled) {
    launching_ = false;
    return;
  }
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = true,
      .autoPlay = true,
      .gaugeType = selections.gaugeType,
      .gaugeAutoShift = selections.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
      .playOption = playInfo.option,
      .playOptionSeed = playInfo.seed,
      .playOption2 = playInfo.option2,
      .playOption2Seed = playInfo.seed2,
      .longNoteMode = long_note_mode::valueFromId(selections.longNoteMode),
      .assistOption = selections.assistOption,
      .pacemakerTarget = pacemaker::kTargetOff,
      .playback = {.percent = context.settings.selectedPlaybackRatePercent,
                   .mode = context.settings.selectedPlaybackMode},
      .ruleset = selections.ruleset,
      .returnScene = this,
  };
  if (recordsModal_ != nullptr) recordsModal_->hide();
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      true);
  launching_ = false;
}

void MusicSelectScene::launchChartReplayExport(
    const ChartMetaRecord &record, const ModernChartResultRecord &modern,
    ReplayVideoExportOptions options) {
  if (recordsExportInProgress_.exchange(true)) {
    return;
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(true);
    recordsModal_->showExportProgress("Exporting Replay", "Preparing export");
  }
  if (recordsExportThread_.joinable()) {
    recordsExportThread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(recordsExportProgressMutex_);
    pendingRecordsExportProgress_.reset();
  }
  options.progressCallback =
      [this](const ReplayVideoExportProgress &progress) {
        std::lock_guard<std::mutex> lock(recordsExportProgressMutex_);
        pendingRecordsExportProgress_ = PendingRecordsExportProgress{
            .fraction = progress.fraction,
            .message = progress.message,
        };
      };
  recordsExportThread_ = std::jthread(
      [this, record, modern = std::move(modern),
       options](const std::stop_token &stop) mutable {
        std::atomic_bool cancelled = false;
        auto consumer = replay::makeRuntimeChartReplayConsumer(
            context.replayRepository);
        auto loaded = consumer.load(modern, record.meta.BmsPath, cancelled);
        if (!loaded.ready() || loaded.chart == nullptr || cancelled) {
          std::lock_guard<std::mutex> lock(recordsExportResultMutex_);
          pendingRecordsExportResult_ = ReplayVideoExportResult{
              .success = false,
              .message = "Replay export could not be prepared.",
          };
          return;
        }
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.stop = stop;
        auto result = ReplayVideoExporter::Export(
            context, loaded.chart.get(), *loaded.replayData, exportOptions);
        std::lock_guard<std::mutex> lock(recordsExportResultMutex_);
        pendingRecordsExportResult_ = result;
      });
}

void MusicSelectScene::launchCourseReplayExport(
    const ModernCourseResultRecord &modern, ReplayVideoExportOptions options) {
  if (recordsModal_ != nullptr) {
    recordsModal_->setStatus(
        "Course replay export is available from the Main Menu.");
  }
}

void MusicSelectScene::launchAutoPlayExport(const ChartMetaRecord &record,
                                            ReplayVideoExportOptions options) {
  if (recordsExportInProgress_.exchange(true)) {
    return;
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(true);
    recordsModal_->showExportProgress("Exporting Replay", "Preparing export");
  }
  if (recordsExportThread_.joinable()) {
    recordsExportThread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(recordsExportProgressMutex_);
    pendingRecordsExportProgress_.reset();
  }
  options.progressCallback =
      [this](const ReplayVideoExportProgress &progress) {
        std::lock_guard<std::mutex> lock(recordsExportProgressMutex_);
        pendingRecordsExportProgress_ = PendingRecordsExportProgress{
            .fraction = progress.fraction,
            .message = progress.message,
        };
      };
  const auto selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  recordsExportThread_ = std::jthread(
      [this, record, options,
       selections](const std::stop_token &stop) mutable {
        std::atomic_bool cancelled = false;
        auto chart = play_options::parseChart(record.meta, cancelled,
                                              "autoplay export");
        if (!chart || cancelled) {
          std::lock_guard<std::mutex> lock(recordsExportResultMutex_);
          pendingRecordsExportResult_ = ReplayVideoExportResult{
              .success = false, .message = "Autoplay export failed."};
          return;
        }
        const auto playInfo = play_options::applySelectedPlayOptions(
            *chart, selections.playOption);
        applyEffectiveLongNoteModeToChart(
            *chart, long_note_mode::valueFromId(selections.longNoteMode));
        ReplayData replay = replay_autoplay::BuildReplayData(
            *chart, selections.gaugeType, selections.gaugeAutoShift,
            {.percent = context.settings.selectedPlaybackRatePercent,
             .mode = context.settings.selectedPlaybackMode},
            playInfo.option, playInfo.seed, playInfo.option2, playInfo.seed2,
            selections.assistOption, context.settings.gameplayClubModeEnabled,
            selections.gaugeAutoShiftLowerBound, selections.ruleset);
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.stop = stop;
        exportOptions.renderTouchPoints = false;
        exportOptions.renderReplayGhosts = false;
        auto result = ReplayVideoExporter::Export(
            context, chart.get(), replay, exportOptions);
        std::lock_guard<std::mutex> lock(recordsExportResultMutex_);
        pendingRecordsExportResult_ = result;
      });
}

void MusicSelectScene::applyRecordsExportProgress() {
  std::optional<PendingRecordsExportProgress> progress;
  {
    std::lock_guard<std::mutex> lock(recordsExportProgressMutex_);
    if (!pendingRecordsExportProgress_.has_value()) {
      return;
    }
    progress = std::move(pendingRecordsExportProgress_);
    pendingRecordsExportProgress_.reset();
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->updateExportProgress(progress->fraction, progress->message);
  }
}

void MusicSelectScene::applyRecordsExportResult() {
  std::optional<ReplayVideoExportResult> result;
  {
    std::lock_guard<std::mutex> lock(recordsExportResultMutex_);
    if (!pendingRecordsExportResult_.has_value()) {
      return;
    }
    result = std::move(pendingRecordsExportResult_);
    pendingRecordsExportResult_.reset();
  }
  if (recordsExportThread_.joinable()) {
    recordsExportThread_.join();
  }
  recordsExportInProgress_ = false;
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(false);
    if (result->success) {
      recordsModal_->returnToList(
          result->message == "Saved to Photos" ? "Saved" : "Exported");
    } else {
      recordsModal_->returnToList(
          result->message == "No Chart"
              ? "No Chart"
              : (result->message.empty() ? "Replay export failed."
                                         : result->message));
    }
  }
}

ReplayRecordsModalCallbacks MusicSelectScene::makeRecordsModalCallbacks() {
  ReplayRecordsModalCallbacks callbacks;
  callbacks.loadRecords = [this](const ChartMetaRecord &record) {
    return loadRecordsForSelector(record);
  };
  callbacks.watchModernChart =
      [this](const ChartMetaRecord &record,
             const ModernChartResultRecord &modern) {
        launchChartReplay(record, modern);
      };
  callbacks.watchModernCourse =
      [this](const ChartMetaRecord &record,
             const ModernCourseResultRecord &modern) {
        launchCourseReplay(record, modern);
      };
  callbacks.watchAutoPlay = [this](const ChartMetaRecord &record) {
    launchAutoPlay(record);
  };
  callbacks.gbattle = [this](const ChartMetaRecord &record,
                             const ModernChartResultRecord &modern) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setStatus("G-BATTLE is available from the Main Menu.");
    }
  };
  callbacks.recallModernChart =
      [this](const ChartMetaRecord &, const ModernChartResultRecord &) {
        if (recordsModal_ != nullptr) {
          recordsModal_->setStatus(
              "Result recall is available from the Main Menu.");
        }
      };
  callbacks.recallModernCourse =
      [this](const ModernCourseResultRecord &, bool) {
        if (recordsModal_ != nullptr) {
          recordsModal_->setStatus(
              "Result recall is available from the Main Menu.");
        }
      };
  callbacks.recallRemote =
      [this](const IrRemoteRecordId &, const std::string &) {
        if (recordsModal_ != nullptr) {
          recordsModal_->setStatus(
              "Result recall is available from the Main Menu.");
        }
      };
  callbacks.exportModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern,
             ReplayVideoExportOptions options) {
        launchChartReplayExport(record, modern, options);
      };
  callbacks.exportModernCourse =
      [this](const ModernCourseResultRecord &modern,
             ReplayVideoExportOptions options) {
        launchCourseReplayExport(modern, options);
      };
  callbacks.exportAutoPlay =
      [this](const ChartMetaRecord &record, ReplayVideoExportOptions options) {
        launchAutoPlayExport(record, options);
      };
  callbacks.irUpload = [this](const ModernChartResultRecord &modern) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setStatus(
          "IR upload is available from the Main Menu.");
    }
  };
  callbacks.irStatusFeedback = [this](ir::IrRecordState state) {
    const char *message = nullptr;
    switch (state) {
    case ir::IrRecordState::Queued:
      message = "IR upload is queued.";
      break;
    case ir::IrRecordState::Uploading:
      message = "IR upload is in progress.";
      break;
    case ir::IrRecordState::AwaitingRemote:
      message = "IR is awaiting the remote result.";
      break;
    case ir::IrRecordState::Blocked:
      message = "IR upload is blocked. Check Settings > IR.";
      break;
    case ir::IrRecordState::Uploaded:
      message = "IR upload is complete.";
      break;
    case ir::IrRecordState::Hidden:
    case ir::IrRecordState::Eligible:
    case ir::IrRecordState::Failed:
      return;
    }
    if (recordsModal_ != nullptr) {
      recordsModal_->showIrFeedback(message);
    }
  };
  return callbacks;
}

void MusicSelectScene::revealChart() {
  executeEvent({.kind = skin::MusicSelectSkinActionKind::Event,
                .selector = {.value = 212},
                .arguments = {0, 0}});
}

void MusicSelectScene::openTasks() {
  showTasksModal();
}

std::string MusicSelectScene::tasksModalTextSnapshot() const {
  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  const auto &progress = snapshot.progress;
  std::vector<chart_library_tasks::TaskInfo> active;
  std::vector<chart_library_tasks::TaskInfo> recent;
  active.reserve(snapshot.tasks.size());
  recent.reserve(snapshot.tasks.size());
  for (const auto &task : snapshot.tasks) {
    if (task.status == chart_library_tasks::TaskStatus::Queued ||
        task.status == chart_library_tasks::TaskStatus::Running ||
        task.status == chart_library_tasks::TaskStatus::Paused) {
      active.push_back(task);
    } else {
      recent.push_back(task);
    }
  }
  if (active.empty() && recent.empty()) return "No parsing tasks.";

  std::ostringstream text;
  if (active.empty()) {
    text << "No active tasks.\n\nRecent tasks\n\n";
  } else {
    text << active.size()
         << (active.size() == 1 ? " active task" : " active tasks")
         << "\n\n";
  }
  const auto appendTask = [&text, &progress](
                              const chart_library_tasks::TaskInfo &task) {
    std::string status;
    switch (task.status) {
    case chart_library_tasks::TaskStatus::Queued: status = "Queued"; break;
    case chart_library_tasks::TaskStatus::Running: status = "Running"; break;
    case chart_library_tasks::TaskStatus::Complete: status = "Complete"; break;
    case chart_library_tasks::TaskStatus::Failed: status = "Failed"; break;
    case chart_library_tasks::TaskStatus::Paused: status = "Paused"; break;
    }
    text << task.title << "\n" << status;
    if (task.status == chart_library_tasks::TaskStatus::Running) {
      if (progress.valid && progress.taskId == task.id) {
        text << " - " << (progress.basisPoints / 100) << "%";
        if (progress.total > 0) {
          text << " (" << progress.current << " / " << progress.total << ")";
        }
        text << "\n" << taskProgressStageText(progress.stage);
      } else {
        text << " - " << static_cast<int>(std::round(task.fraction * 100.0))
             << "%";
        if (task.total > 0) {
          text << " (" << task.current << " / " << task.total << ")";
        }
        if (!task.detail.empty()) text << "\n" << task.detail;
      }
    } else if (!task.detail.empty() && task.detail != status) {
      text << "\n" << task.detail;
    }
    text << "\n\n";
  };
  for (const auto &task : active) appendTask(task);
  if (!active.empty() && !recent.empty()) text << "Recent tasks\n\n";
  constexpr std::size_t kMaxRecentTasksShown = 8;
  for (std::size_t index = 0;
       index < recent.size() && index < kMaxRecentTasksShown; ++index) {
    appendTask(recent[recent.size() - index - 1]);
  }
  return text.str();
}

void MusicSelectScene::showTasksModal() {
  if (modalLayer_ == nullptr) return;
  if (tasksModal_ == nullptr) {
    tasksModal_ = new BlockingOverlayView(
        0, 0, rendering::window_width, rendering::window_height);
    tasksModal_->setPositionType(YGPositionTypeAbsolute);
    tasksModal_->setPosition(Edge::Left, 0);
    tasksModal_->setPosition(Edge::Top, 0);
    tasksModal_->setZIndex(1000);
    tasksModal_->setVisible(false);
    tasksModal_->setFlexDirection(FlexDirection::Column);
    tasksModal_->setAlignItems(YGAlignCenter);
    tasksModal_->setJustifyContent(YGJustifyCenter);
    tasksModal_->setThemedBackgroundColor(ui_theme::scrim);

    auto *panel = new View();
    panel->setWidth(760)
        ->setHeight(460)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setGap(14)
        ->setPadding(Edge::All, 22)
        ->setThemedBackgroundColor(ui_theme::panelStrong)
        ->setCornerRadius(ui_theme::panelRadius())
        ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
        ->setThemedBorderColor(ui_theme::hairlineStrong)
        ->setBorderWidth(1);
    auto *title = makeText("Tasks", 30, ui_theme::textPrimary);
    title->setHeight(42);
    panel->addView(title);

    auto *scroll = new ScrollView(0, 0, 716, 400);
    scroll->setWidth(716)
        ->setFlex(1)
        ->setThemedBackgroundColor(ui_theme::insetSurface)
        ->setCornerRadius(ui_theme::controlRadius())
        ->setThemedBorderColor(ui_theme::hairline)
        ->setBorderWidth(1);
    scroll->setContentPadding(Edge::All, 12);
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch);
    tasksModalText_ = makeText({}, 18, ui_theme::textSecondary);
    tasksModalText_->setWrap(true);
    tasksModalText_->setOverflow(TextView::TextOverflow::Visible);
    content->addView(tasksModalText_);
    scroll->setContentView(content);
    panel->addView(scroll);

    auto *footer = new View();
    footer->setFlexDirection(FlexDirection::Row)
        ->setJustifyContent(YGJustifyFlexEnd)
        ->setGap(12)
        ->setHeight(56);
    auto *refresh = makeButton("Refresh List");
    refresh->setOnClickListener([this] {
      if (context.chartLibraryTasks &&
          context.chartLibraryTasks->snapshot().activeCount > 0) {
        context.chartLibraryScanFlushRequested.fetch_add(1,
                                                          std::memory_order_release);
      }
      reloadLibrary();
      selectedBarMoved();
      if (tasksModal_ != nullptr) tasksModal_->setVisible(false);
    });
    auto *close = makeButton("Close");
    close->setOnClickListener([this] {
      if (tasksModal_ != nullptr) tasksModal_->setVisible(false);
    });
    footer->addView(refresh);
    footer->addView(close);
    panel->addView(footer);
    tasksModal_->addView(panel);
    modalLayer_->addView(tasksModal_);
  }
  if (playOptionsModal_ != nullptr) playOptionsModal_->hide();
  if (recordsModal_ != nullptr) recordsModal_->hide();
  tasksModal_->setSize(rendering::window_width, rendering::window_height);
  tasksModal_->setVisible(true);
  displayedTasksRevision_ = 0;
  displayedTaskProgressRevision_ = 0;
  refreshTasksModal();
  tasksModal_->applyYogaLayout();
}

void MusicSelectScene::refreshTasksModal() {
  if (tasksModal_ == nullptr || tasksModalText_ == nullptr) return;
  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  if (snapshot.revision == displayedTasksRevision_ &&
      snapshot.progress.revision == displayedTaskProgressRevision_) {
    return;
  }
  displayedTasksRevision_ = snapshot.revision;
  displayedTaskProgressRevision_ = snapshot.progress.revision;
  tasksModalText_->setText(tasksModalTextSnapshot());
}

PlayOptionsPanelState MusicSelectScene::playOptionsState() const {
  const main_menu_profile::Selections selections =
      main_menu_profile::Selections::fromSettings(context.settings);
  PlayOptionsPanelState state{
      .ruleset = selections.ruleset,
      .gaugeType = selections.gaugeType,
      .gaugeAutoShift = selections.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
      .playOption = selections.playOption,
      .longNoteMode = selections.longNoteMode,
      .assistOption = selections.assistOption,
      .playbackRatePercent = context.settings.selectedPlaybackRatePercent,
      .clubMode = context.settings.gameplayClubModeEnabled,
      .pacemakerTarget = selections.pacemakerTarget};
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size()) return state;
  const auto &selected = snapshot.rows[snapshot.selectedIndex];
  if (!selected.chart) return state;
  const int chartLnMode = normalizeChartLongNoteModeValue(
      selected.chart->meta.LnMode);
  if (chartLnMode > 0) {
    state.longNoteMode = long_note_mode::idFromValue(
        chartLnMode, AppSettings::kDefaultLnMode);
    state.longNoteModeLocked = true;
  }
  return state;
}

void MusicSelectScene::updatePlayOptions(
    const std::function<void(main_menu_profile::Selections &)> &update) {
  auto selections = main_menu_profile::Selections::fromSettings(context.settings);
  update(selections);
  selections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save music-select play options");
  }
  refreshPlayOptionsModal();
}

void MusicSelectScene::refreshPlayOptionsModal() {
  if (playOptionsModal_ != nullptr) {
    playOptionsModal_->refresh(playOptionsState());
  }
}

void MusicSelectScene::openPlayOptions() {
  if (modalLayer_ == nullptr) return;
  if (recordsModal_ != nullptr) recordsModal_->hide();
  if (tasksModal_ != nullptr) tasksModal_->setVisible(false);
  if (!playOptionsModal_) {
    playOptionsModal_ = MainMenuPlayOptionsModal::Create(
        modalLayer_,
        {.onRulesetSelected = [this](GameplayRuleset ruleset) {
           updatePlayOptions([ruleset](auto &selections) {
             selections.ruleset = ruleset;
           });
         },
         .onGaugeSelected = [this](GaugeType gauge,
                                   GaugeAutoShiftMode autoShift) {
           updatePlayOptions([gauge, autoShift](auto &selections) {
             selections.gaugeType = gauge;
             selections.gaugeAutoShift = autoShift;
           });
         },
         .onGaugeLowerBoundSelected = [this](GaugeType gauge) {
           updatePlayOptions([gauge](auto &selections) {
             selections.gaugeAutoShiftLowerBound = gauge;
           });
         },
         .onPlayOptionSelected = [this](const std::string &option) {
           updatePlayOptions([&option](auto &selections) {
             selections.playOption = play_options::normalizePlayOption(option);
           });
         },
         .onLongNoteModeSelected = [this](const std::string &mode) {
           const auto state = playOptionsState();
           if (state.longNoteModeLocked &&
               long_note_mode::parseId(mode, AppSettings::kDefaultLnMode) !=
                   state.longNoteMode) {
             return;
           }
           updatePlayOptions([&mode](auto &selections) {
             selections.longNoteMode = long_note_mode::parseId(
                 mode, AppSettings::kDefaultLnMode);
           });
         },
         .onAssistOptionSelected = [this](const std::string &option) {
           updatePlayOptions([&option](auto &selections) {
             selections.assistOption = assist_options::normalize(option);
           });
         },
         .onPlaybackRateSelected = [this](int percent) {
           context.settings.selectedPlaybackRatePercent = percent;
           context.settings.sanitize();
           if (!context.saveSettings()) {
             SDL_Log("Failed to save music-select playback rate");
           }
           refreshPlayOptionsModal();
         },
         .onPlaybackModeSelected = [this](const std::string &mode) {
           if (mode != "pitch-shift") return;
           context.settings.selectedPlaybackMode = audio::PlaybackMode::PitchShift;
           context.settings.sanitize();
           if (!context.saveSettings()) {
             SDL_Log("Failed to save music-select playback mode");
           }
           refreshPlayOptionsModal();
         },
         .onClubModeToggled = [this] {
           context.settings.gameplayClubModeEnabled =
               !context.settings.gameplayClubModeEnabled;
           if (!context.saveSettings()) {
             SDL_Log("Failed to save music-select Club mode");
           }
           refreshPlayOptionsModal();
         },
         .onPacemakerSelected = [this](const std::string &target) {
           updatePlayOptions([&target](auto &selections) {
             selections.pacemakerTarget = pacemaker::normalizeTargetId(target);
           });
         }},
        modalOverlayPortal_);
  }
  if (playOptionsModal_ == nullptr) return;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr) skinTextInput_->endEditing();
#endif
  refreshPlayOptionsModal();
  playOptionsModal_->show();
}

void MusicSelectScene::openIrUploads() {
  context.sceneManager->changeScene(std::make_unique<IrUploadsScene>(
      context,
      SceneReturnTarget::Retained(
          this, [this] { reactivateSkinOnResume_ = true; })),
      true);
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
      {.openChartViewer = [this] { openChartViewer(); },
       .openChartRecords = [this] { openChartRecords(); },
       .revealChart = [this] { revealChart(); },
       .openMusicPlayer = [this] { openMusicPlayer(); },
       .openTasks = [this] { openTasks(); },
       .openPlayOptions = [this] { openPlayOptions(); },
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
void MusicSelectScene::buildSkinLoadingView() {
  if (skinLoadingView_ != nullptr) {
    skinLoadingView_->setVisible(true);
    return;
  }
  auto *root = new View(0, 0, rendering::window_width,
                        rendering::window_height);
  root->setPositionType(YGPositionTypeAbsolute);
  root->setZIndex(10000);
  root->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setPadding(Edge::All, 32)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  auto *label = makeText("Loading music-select skin…", 28,
                         ui_theme::textPrimary);
  label->setHeight(56);
  label->setAlign(TextView::CENTER);
  root->addView(label);
  root->applyYogaLayout();
  addView(root);
  skinLoadingView_ = root;
}

void MusicSelectScene::cancelSkinPreparation() {
  skinPreparationStop_.request_stop();
  if (!skinPreparation_.valid()) return;
  try {
    (void)skinPreparation_.get();
  } catch (...) {
  }
}

void MusicSelectScene::finalizeSkinPreparationIfReady() {
  if (!skinPreparation_.valid() ||
      skinPreparation_.wait_for(std::chrono::milliseconds::zero()) !=
          std::future_status::ready) {
    return;
  }
  auto prepared = skinPreparation_.get();
  if (prepared.cancelled || skinPreparationStop_.stop_requested()) return;
  auto diagnostics = std::move(prepared.diagnostics);
  if (!prepared.prepared) {
    enterError(std::move(diagnostics));
    return;
  }
  auto finalized = skin::MusicSelectSkinSession::finalize(
      std::move(*prepared.prepared),
      {.resourcePreparation = *context.skinResourcePreparationService,
       .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
       .liveResourceCounters = context.skinLiveResourceCounters,
       .quadBackend = nullptr,
       .captureLegacyInputGeneration = [this] {
         return context.inputDeviceRegistry.legacyInputGeneration(
             rendering::render_width, rendering::render_height);
       }});
  diagnostics.insert(
      diagnostics.end(),
      std::make_move_iterator(finalized.diagnostics.begin()),
      std::make_move_iterator(finalized.diagnostics.end()));
  if (!finalized.session) {
    enterError(std::move(diagnostics));
    return;
  }
  skinSession_ = std::move(finalized.session);
  if (skinLoadingView_ != nullptr) skinLoadingView_->setVisible(false);
}

bool MusicSelectScene::activateSkin(
    skin::GameplaySkinActivationRequest request) {
  cancelSkinPreparation();
  skinSession_.reset();
  failed_ = false;
  if (errorView_ != nullptr) errorView_->setVisible(false);
  selectedSkinPath_ = musicSelectSkinEntryPath(request.activation.entry);
  if (!context.skinStorageRoots || !context.skinResourcePreparationService ||
      !context.skinLiveResourceCounters) {
    enterError({skin::SkinDiagnostic{
        .code = "skin.music_select.session_services_unavailable",
        .message = "Music-select skin session services are unavailable."}});
    return false;
  }
  skinPreparationStop_ = std::stop_source{};
  const auto initialGeneration =
      context.inputDeviceRegistry.legacyInputGeneration(
          rendering::render_width, rendering::render_height);
  auto preparationContext = skin::MusicSelectSkinSessionPreparationContext{
      .storageRoots = *context.skinStorageRoots,
      .resourcePreparation = *context.skinResourcePreparationService,
      .initialFrame = makeFrame(),
      .builtinImageReader = archive_file::readFileBounded,
      .audioBackend = skin::createLuaSkinApplicationAudioBackend(
          context.jukebox.audioRuntime(),
          [this] { return context.settings.audioVideo.audio.masterVolume; },
          {.maximumIdentities = std::numeric_limits<std::size_t>::max(),
           .maximumEncodedBytes = std::numeric_limits<std::size_t>::max(),
           .maximumDecodedBytes = std::numeric_limits<std::size_t>::max()},
          context.skinLiveResourceCounters),
      .initialLegacyInputGeneration = initialGeneration,
      .stop = skinPreparationStop_.get_token()};
  skinPreparation_ = std::async(
      std::launch::async,
      [request = std::move(request),
       preparationContext = std::move(preparationContext)]() mutable {
        return skin::MusicSelectSkinSession::prepare(
            std::move(request), std::move(preparationContext));
      });
  buildSkinLoadingView();
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
  launchCancelled_.store(true, std::memory_order_release);
  if (launchThread_.joinable()) {
    launchThread_.request_stop();
    launchThread_.join();
  }
  stopPreloadWorker();
  {
    std::lock_guard<std::mutex> lock(preloadMutex_);
    preloadedChart_.reset();
    preloadedPath_.clear();
  }
  if (preloadWorker_ != nullptr) {
    delete preloadWorker_;
    preloadWorker_ = nullptr;
  }
  if (searchInput_ != nullptr) searchInput_->endEditing();
  if (decideOverlay_ != nullptr) {
    modalOverlayPortal_->dismiss(decideOverlay_);
    delete decideOverlay_;
    decideOverlay_ = nullptr;
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinTextInput_ != nullptr) skinTextInput_->endEditing();
  skinTouchGesture_.cancel();
  cancelSelectedChartAnalysis();
  cancelSkinPreparation();
#endif
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
  skinLoadingView_ = nullptr;
  selectedChartInformation_.reset();
  activeSkinStringWriter_.reset();
  skinTextInput_ = nullptr;
  skinTextInputs_.clear();
#endif
  chartSession_.reset();
  playOptionsModal_.reset();
  toolbar_ = nullptr;
  searchOverlay_ = nullptr;
  searchInput_ = nullptr;
  modalOverlayPortal_ = nullptr;
  modalLayer_ = nullptr;
  if (recordsExportThread_.joinable()) {
    SDL_Log("Joining recordsExportThread");
    recordsExportThread_.request_stop();
    recordsExportThread_.join();
  }
  recordsModal_.reset();
  tasksModal_ = nullptr;
  tasksModalText_ = nullptr;
  errorView_ = nullptr;
  diagnostics_.clear();
}
