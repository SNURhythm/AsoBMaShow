#include "MusicSelectScene.h"

#include "IrUploadsScene.h"
#include "LibraryTasksScene.h"
#include "MusicPlayerScene.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#include "../ArchiveFile.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../PlayOptionUtils.h"
#include "../audio/Jukebox.h"
#include "../music_select/MusicSelectRepositoryProjection.h"
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
#include <atomic>
#include <cmath>
#include <ctime>
#include <memory>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr std::int64_t kBarMoveMillis = 120;

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
} // namespace

MusicSelectScene::MusicSelectScene(
    ApplicationContext &context,
    skin::GameplaySkinActivationRequest activationRequest)
    : Scene(context), activationRequest_(std::move(activationRequest)) {}

void MusicSelectScene::init() {
  started_ = std::chrono::steady_clock::now();
  chartSession_ = context.chartRepository.OpenSession(&context.scoreRepository);
  reloadLibrary();

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
}

void MusicSelectScene::onResume() { reloadLibrary(); }

void MusicSelectScene::reloadLibrary() {
  if (!chartSession_) return;
  std::vector<ChartMetaRecord> records;
  ChartMetaQuery query;
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(context.settings.selectedLnMode);
  chartSession_->QueryChartMeta(query, records);
  scoreCache_ = context.scoreRepository.LoadBestScores();
  libraryRevision_ = context.chartRepository.GetLibraryRevision();
  bars_.refresh(MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [this](const bms_parser::ChartMeta &meta, int mode) {
         return scoreCache_.bestFor(meta, mode);
       },
       .selectedLongNoteMode = query.selectedLongNoteMode,
       .repositoryRevision = libraryRevision_}));
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

  frame.properties.rates[1] = snapshot.rows.empty()
                                  ? 0.0
                                  : static_cast<double>(snapshot.selectedIndex) /
                                        snapshot.rows.size();
  frame.properties.rates[8] = 0.0;
  frame.properties.strings[30] = "";
  frame.properties.strings[60] = context.settings.skinModeFilterName;
  frame.properties.strings[61] = context.settings.skinSortId;
  frame.properties.strings[62] = context.settings.skinDifficultyFilterName;
  frame.properties.strings[86] = context.settings.skinChartReplicationMode;
  frame.properties.strings[1000] = snapshot.directoryText;
  frame.properties.booleans[21] = false;
  frame.properties.booleans[22] = false;
  frame.properties.booleans[23] = false;

  if (snapshot.selectedIndex < snapshot.rows.size()) {
    const auto &selected = snapshot.rows[snapshot.selectedIndex];
    frame.properties.booleans[1] =
        selected.kind == skin::MusicSelectBarKind::Folder;
    frame.properties.booleans[2] =
        selected.kind == skin::MusicSelectBarKind::Song;
    frame.properties.booleans[3] =
        selected.kind == skin::MusicSelectBarKind::Grade;
    frame.properties.booleans[5] = selected.selectable;
    const auto lamp = std::clamp(selected.presentation.lamp, 0, 10);
    frame.properties.booleans[100] = lamp == 0;
    frame.properties.booleans[101] = lamp == 1;
    frame.properties.booleans[1100] = lamp == 2;
    frame.properties.booleans[1101] = lamp == 3;
    frame.properties.booleans[102] = lamp == 4;
    frame.properties.booleans[103] = lamp == 5;
    frame.properties.booleans[104] = lamp == 6;
    frame.properties.booleans[1102] = lamp == 7;
    frame.properties.booleans[105] = lamp == 8;
    frame.properties.strings[10] = selected.title;
    frame.properties.strings[12] = selected.title;
    if (selected.chart) {
      const auto &record = *selected.chart;
      const auto &meta = record.meta;
      frame.properties.strings[10] = meta.Title;
      frame.properties.strings[11] = meta.SubTitle;
      frame.properties.strings[12] = selected.title;
      frame.properties.strings[13] = meta.Genre;
      frame.properties.strings[14] = meta.Artist;
      frame.properties.strings[15] = meta.SubArtist;
      frame.properties.strings[16] =
          meta.SubArtist.empty() ? meta.Artist
                                 : meta.Artist + " " + meta.SubArtist;
      frame.properties.strings[1030] = meta.MD5;
      frame.properties.strings[1031] = meta.SHA256;
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

  const auto now = unixMillis();
  if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
    switch (event.key.keysym.sym) {
    case SDLK_ESCAPE:
      context.quitFlag.store(true);
      return {.quit = true};
    case SDLK_DOWN:
      bars_.move(true, now, kBarMoveMillis);
      return {};
    case SDLK_UP:
      bars_.move(false, now, kBarMoveMillis);
      return {};
    case SDLK_LEFT:
      closeDirectory();
      return {};
    case SDLK_RIGHT:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      openSelected();
      return {};
    default:
      break;
    }
  }
  if (event.type == SDL_MOUSEWHEEL) {
    int movement = -event.wheel.y;
    while (movement > 0) {
      bars_.move(true, now, kBarMoveMillis);
      --movement;
    }
    while (movement < 0) {
      bars_.move(false, now, kBarMoveMillis);
      ++movement;
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
        bars_.setSelectedPosition(static_cast<float>(*pointer.selectIndex) /
                                  snapshot.rows.size());
        const auto selected = bars_.snapshot();
        if (selected.selectedIndex < selected.rows.size() &&
            !selected.rows[selected.selectedIndex].children.empty()) {
          (void)bars_.openSelected();
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
  } else if (selected.chart) {
    launchSelected();
  }
}

void MusicSelectScene::closeDirectory() { (void)bars_.close(); }

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
  const auto playInfo =
      play_options::applySelectedPlayOptions(*chart, selections.playOption);
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

void MusicSelectScene::executeEvent(
    const skin::MusicSelectSkinAction &action) {
  const auto id = numericSelector(action.selector);
  const auto name = selectorName(action.selector);
  if ((id && *id == 15) || name == "play") {
    launchSelected();
  } else if ((id && *id == 16) || name == "autoplay") {
    launchSelected(true, false);
  } else if ((id && *id == 315) || name == "practice") {
    launchSelected(false, true);
  } else if ((id && *id == 14) || name == "skinconfig" ||
             (id && *id == 13) || name == "keyconfig") {
    openSettings();
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
  }
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
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinSession_.reset();
#endif
  chartSession_.reset();
  toolbar_ = nullptr;
  diagnostics_.clear();
}
