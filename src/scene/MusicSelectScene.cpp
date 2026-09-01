#include "MusicSelectScene.h"

#include "IrUploadsScene.h"
#include "LibraryTasksScene.h"
#include "MusicPlayerScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "../ArchiveFile.h"
#include "../AssistOptionUtils.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../audio/Jukebox.h"
#include "../music_select/MusicSelectRepositoryProjection.h"
#include "../music_select/MusicSelectPropertyProjection.h"
#include "../rendering/common.h"
#include "../view/Button.h"
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
#include <cmath>
#include <ctime>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

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
    : Scene(context), activationRequest_(std::move(activationRequest)) {}

void MusicSelectScene::init() {
  started_ = std::chrono::steady_clock::now();
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

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (!context.skinStorageRoots || !context.skinResourcePreparationService ||
      !context.skinLiveResourceCounters) {
    enterError({skin::SkinDiagnostic{
        .code = "skin.music_select.session_services_unavailable",
        .message = "Music-select skin session services are unavailable."}});
    return;
  }
  auto initialFrame = makeFrame();
  const auto safetyLevel = activationRequest_.safetyLevel;
  auto created = skin::MusicSelectSkinSession::create(
      std::move(activationRequest_),
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
    return;
  }
  skinSession_ = std::move(created.session);
#endif

  MusicSelectToolbarState toolbarState;
  {
    std::lock_guard lock(context.applicationUiStateMutex);
    toolbarState = context.applicationUiState.musicSelectToolbar;
  }
  auto toolbar = MusicSelectToolbarView::Create(
      toolbarState,
      {.openMusicPlayer = [this] { openMusicPlayer(); },
       .openTasks = [this] { openTasks(); },
       .openIrUploads = [this] { openIrUploads(); },
       .openSettings = [this] { openSettings(); },
       .persist = [this](MusicSelectToolbarState state) {
         persistToolbar(state);
       }},
      rendering::window_width, rendering::window_height);
  if (toolbar) {
    toolbar_ = toolbar.get();
    addView(toolbar.release());
  }
  startInputListening();
}

void MusicSelectScene::onPause() { stopInputListening(); }

void MusicSelectScene::onResume() {
  reloadLibrary();
  if (!failed_) startInputListening();
}

void MusicSelectScene::reloadLibrary() {
  if (!chartSession_) return;
  std::vector<ChartMetaRecord> records;
  ChartMetaQuery query;
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  chartSession_->QueryChartMeta(query, records);
  scoreCache_ = context.scoreRepository.LoadBestScores();
  playerHistory_ = context.scoreRepository.LoadPlayerScoreHistory();
  libraryRevision_ = context.chartRepository.GetLibraryRevision();
  bars_.configure({.modeFilter = context.settings.skinModeFilterName,
                   .difficultyFilter =
                       context.settings.skinDifficultyFilterName,
                   .sortId = context.settings.skinSortId});
  bars_.refresh(MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [this](const bms_parser::ChartMeta &meta, int mode) {
         return scoreCache_.bestFor(meta, mode);
       },
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
  selectedReplay_ =
      snapshot.selectedIndex < snapshot.rows.size()
          ? musicSelectFirstExistingReplay(
                &snapshot.rows[snapshot.selectedIndex])
          : -1;
  songBarChangeMicros_ = elapsedMicros();
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
      if (!snapshot.rows.empty()) {
        selectedBarMoved();
        bars_.setSelectedPosition(static_cast<float>(*pointer.selectIndex) /
                                  snapshot.rows.size());
        const auto selected = bars_.snapshot();
        if (selected.selectedIndex < selected.rows.size() &&
            !selected.rows[selected.selectedIndex].children.empty()) {
          (void)bars_.openSelected();
          syncResolvedFilters();
          selectedBarMoved();
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
  if (!selected.children.empty()) {
    (void)bars_.openSelected();
    syncResolvedFilters();
    selectedBarMoved();
  } else if (selected.chart) {
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

void MusicSelectScene::closeDirectory() {
  if (bars_.close()) {
    syncResolvedFilters();
    selectedBarMoved();
  }
}

void MusicSelectScene::launchSelected(bool autoplay, bool practice) {
  if (launching_) return;
  const auto snapshot = bars_.snapshot();
  if (snapshot.selectedIndex >= snapshot.rows.size() ||
      !snapshot.rows[snapshot.selectedIndex].chart) {
    return;
  }
  const auto record = *snapshot.rows[snapshot.selectedIndex].chart;
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
      .practiceMode = practice,
      .playback = {.percent = context.settings.selectedPlaybackRatePercent,
                   .mode = context.settings.selectedPlaybackMode},
      .clubMode = context.settings.gameplayClubModeEnabled,
      .ruleset = selections.ruleset};
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(chart),
                                      std::move(options)),
      true);
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
        selectedBarMoved();
        bars_.setSelectedPosition(static_cast<float>(action.floatValue));
      }
      break;
    }
    case skin::MusicSelectSkinActionKind::StringWriter:
      break;
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
    logicalInput.currentBar = !selected.children.empty()
                                   ? MusicSelectInputBarKind::Directory
                                   : selected.selectable
                                         ? MusicSelectInputBarKind::Selectable
                                         : MusicSelectInputBarKind::Other;
  }
  logicalInput.selectedReplay = selectedReplay_;
  for (const auto &action :
       inputProcessor_.process(logicalInput, unixMillis())) {
    applyInputAction(action);
  }
  inputBindingAdapter_->clearFrameEdges();
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
  case MusicSelectInputActionKind::OpenFolder:
    if (bars_.openSelected()) {
      syncResolvedFilters();
      selectedBarMoved();
    }
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
      .hasSelectedPlayConfig = selected != nullptr && selected->chart.has_value(),
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
      break;
    case MusicSelectEventEffectKind::UpdateFolder: {
      std::filesystem::path path;
      if (selected != nullptr &&
          selected->kind == skin::MusicSelectBarKind::Folder) {
        path = selected->directoryPath;
      } else if (selected != nullptr &&
                 selected->kind == skin::MusicSelectBarKind::Song &&
                 selected->chart &&
                 !selected->chart->meta.BmsPath.empty()) {
        path = selected->chart->meta.BmsPath.parent_path();
      }
      if (!path.empty() && context.chartLibraryTasks) {
        context.chartLibraryTasks->enqueue({
            .kind = chart_library_tasks::TaskKind::RefreshPath,
            .title = "Update Folder",
            .refreshPath = std::move(path),
        });
      }
      break;
    }
    default:
      break;
    }
  }
}

void MusicSelectScene::update(float) {
  if (failed_) return;
  if (toolbar_) {
    toolbar_->setViewportSize(rendering::window_width,
                              rendering::window_height);
  }
  if (context.chartRepository.GetLibraryRevision() != libraryRevision_) {
    reloadLibrary();
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
  consumeLogicalInput();
  consumeActions();
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
  diagnostics_ = std::move(diagnostics);
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
  if (diagnostics_.empty()) {
    auto *reason = makeText("No diagnostic was reported.", 20,
                            ui_theme::textSecondary);
    reason->setHeight(44);
    root->addView(reason);
  } else {
    for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
      const auto &diagnostic = diagnostics_[index];
      std::string reason = std::to_string(index + 1) + ". ";
      if (!diagnostic.code.empty()) reason += diagnostic.code + ": ";
      reason += diagnostic.message;
      auto *reasonView = makeText(std::move(reason), 19,
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
  context.sceneManager->changeScene(std::make_unique<SettingsScene>(
      context, SettingsDestination::Profile,
      SceneReturnTarget::Retained(this)), true);
}

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
  stopInputListening();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinSession_.reset();
#endif
  chartSession_.reset();
  toolbar_ = nullptr;
  diagnostics_.clear();
}
