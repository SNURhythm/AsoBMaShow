#include "scene/play/ReplayPlayfieldPresentation.h"
#include "scene/play/ReplayVideoGameplayPreflight.h"
#include "PreparationPlan.h"
#include "ReplayGhostUtils.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"
#include "video/RendererAccessCoordinator.h"
#include "view/View.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
Camera *main_camera = nullptr;
Camera game_camera{main_view};
void updateUIScale(int renderWidth, int renderHeight) {
  render_width = renderWidth;
  render_height = renderHeight;
}
} // namespace rendering

namespace {

int failures = 0;
namespace fs = std::filesystem;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TestBga final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t, std::int64_t, const GameplayBgaMissState &miss) override {
    ++prepareCalls;
    lastMissState = miss;
    if (throwOnPrepare) {
      throw std::runtime_error("selected skin BGA preparation failure");
    }
    return {};
  }
  BgaPreflightResult preflight(const PreparedGameplayBgaFrame &,
                               std::span<const BgaDrawTarget>) override {
    ++preflightCalls;
    return {.ready = true};
  }
  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++commitCalls;
  }
  void submitPrepared(const PreparedGameplayBgaFrame &,
                      const BgaDrawTarget &) noexcept override {
    ++submitCalls;
  }
  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++finalizeCalls;
  }
  void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept override {
    ++fullscreenCalls;
  }

  bool throwOnPrepare = false;
  int prepareCalls = 0;
  int preflightCalls = 0;
  int commitCalls = 0;
  int submitCalls = 0;
  int finalizeCalls = 0;
  int fullscreenCalls = 0;
  GameplayBgaMissState lastMissState;
};

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    root_ = fs::temp_directory_path() /
            ("asobmashow-replay-presentation-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class FactoryTextureDevice final : public skin::SkinTextureDevice {
public:
  FactoryTextureDevice() : owner_(std::this_thread::get_id()) {}
  bgfx::TextureHandle create(const image_decode::DecodedImageData &) override {
    return bgfx::TextureHandle{nextHandle_++};
  }
  void destroy(bgfx::TextureHandle) noexcept override {}
  [[nodiscard]] bool ownsCurrentThread() const noexcept override {
    return std::this_thread::get_id() == owner_;
  }

private:
  std::thread::id owner_;
  std::uint16_t nextHandle_ = 1;
};

skin::SkinEntryId fixtureEntry() {
  const auto package = skin::normalizePackageId("ReplayAdapterSkin");
  expect(package.package.has_value(), "replay adapter fixture package is valid");
  if (!package.package) {
    std::abort();
  }
  const auto entry =
      skin::normalizeEntryPath(*package.package, "skin/main.luaskin");
  expect(entry.entry.has_value(), "replay adapter fixture entry is valid");
  if (!entry.entry) {
    std::abort();
  }
  return *entry.entry;
}

struct SelectedSkinFixture final {
  TempDirectory temp;
  skin::SkinStorageRoots roots{.visiblePackages = temp.root() / "visible",
                               .privateRevisions = temp.root() / "revisions",
                               .privateCatalog = temp.root() / "catalog",
                               .profileOverlays = temp.root() / "overlays"};
  skin::SkinPackageCatalog catalog{temp.root() / "catalog"};
  skin::SkinDiagnosticHistory history{catalog};
  skin::SkinResourcePreparationService resources;
  std::shared_ptr<skin::SkinLiveResourceCounters> counters =
      std::make_shared<skin::SkinLiveResourceCounters>();
  std::shared_ptr<FactoryTextureDevice> textureDevice =
      std::make_shared<FactoryTextureDevice>();
  skin::SkinConfigurationWriteQueue writes;
  skin::SkinEntryId entry = fixtureEntry();
  skin::SkinProfileId profile =
      *skin::makeSkinProfileId("77777777-7777-4777-8777-777777777777");
  std::optional<skin::SkinRevisionLease> lease;
  skin::SkinValidationResult validation;
  std::shared_ptr<int> receivedInitialStateCount = std::make_shared<int>(0);
  std::shared_ptr<int> receivedInitialProjectionCount =
      std::make_shared<int>(0);
  std::shared_ptr<int> receivedTouchCount = std::make_shared<int>(0);
  std::shared_ptr<std::optional<PlayfieldVisualState>> receivedInitialState =
      std::make_shared<std::optional<PlayfieldVisualState>>();
  std::shared_ptr<std::optional<PlayfieldProjectionResult>>
      receivedInitialProjection =
          std::make_shared<std::optional<PlayfieldProjectionResult>>();
  std::shared_ptr<std::optional<skin::UiLogicalRect>> receivedSafeUiBounds =
      std::make_shared<std::optional<skin::UiLogicalRect>>();
  std::shared_ptr<std::function<void()>> createObserver =
      std::make_shared<std::function<void()>>();

  SelectedSkinFixture() {
    fs::create_directories(roots.visiblePackages);
    const fs::path packageRoot = roots.visiblePackages / entry.package.directoryName;
    fs::create_directories(packageRoot / "skin");
    std::ofstream(packageRoot / "skin/main.luaskin")
        << "return { type = 0, w = 1280, h = 720, finishmargin = 701, "
           "fadeout = 702 }\n";
    lease = skin::SkinRevisionLease::fromLiveSource(
        {.package = entry.package, .lowercaseSha256 = std::string(64, 'c')},
        packageRoot);
    expect(lease.has_value(), "selected replay fixture acquires live revision");
    if (!lease) {
      return;
    }
    skin::GameplaySkinValidator validator(resources);
    validation = validator.validate(lease->readView(), entry, nullptr, {});
    expect(validation.reconciledSettings.has_value() &&
               !validation.configurationDigest.empty(),
           "selected replay fixture validates the skin");
  }

  [[nodiscard]] GameplaySkinSessionServices services() {
    auto activation =
        std::make_shared<std::optional<skin::ValidatedSkinActivation>>(
            skin::ValidatedSkinActivation{
                .revision = std::move(*lease),
                .entry = entry,
                .reconciledSettings = *validation.reconciledSettings,
                .configurationDigest = validation.configurationDigest});
    return {.acquire = [activation, profile = profile](int) mutable {
              if (!activation->has_value()) {
                return skin::GameplaySkinAcquisition{
                    .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
                    .failure = skin::GameplaySkinAcquisitionFailure{
                        .diagnostic = {.code = "skin.test.activation_reused",
                                       .message = "activation was reused",
                                       .severity = skin::DiagnosticSeverity::Error}}};
              }
              skin::GameplaySkinAcquisition result;
              result.disposition = skin::GameplaySkinAcquisitionDisposition::Ready;
              result.request = skin::GameplaySkinActivationRequest{
                  .sessionSerial = 71,
                  .profileId = profile,
                  .activation = std::move(**activation),
                  .viewport = {}};
              activation->reset();
              return result;
            },
            .storageRoots = &roots,
            .resourcePreparation = &resources,
            .liveResourceCounters = counters,
            .configurationWrites = &writes,
            .diagnosticHistory = &history,
            .createSessionForTesting =
                [textureDevice = textureDevice,
                 initialStateCount = receivedInitialStateCount,
                 initialProjectionCount = receivedInitialProjectionCount,
                 touchCount = receivedTouchCount,
                 initialState = receivedInitialState,
                 initialProjection = receivedInitialProjection,
                 safeUiBounds = receivedSafeUiBounds,
                 createObserver = createObserver](
                    skin::ValidatedSkinActivation activation,
                    skin::PlaySkinSessionContext context) {
                  if (*createObserver) {
                    (*createObserver)();
                  }
                  if (context.initialState != nullptr) {
                    ++*initialStateCount;
                    *touchCount = static_cast<int>(context.initialState->touches.size());
                    *initialState = *context.initialState;
                  }
                  if (context.initialProjection != nullptr) {
                    ++*initialProjectionCount;
                    *initialProjection = *context.initialProjection;
                  }
                  *safeUiBounds = context.safeUiBounds;
                  context.textureDevice = textureDevice;
                  return skin::PlaySkinSession::create(std::move(activation),
                                                       std::move(context));
                }};
  }
};

ReplayPlayfieldPresentationCreateInfo
createInfo(bms_parser::Chart &chart, const AppSettings &settings,
           const PlayfieldPresentationConfig &configuration, TestBga &bga) {
  return {.chart = chart,
          .timingWindows = {},
          .configuration = configuration,
          .settings = settings,
          .playback = {},
          .bga = bga,
          .skinServices = {.acquire = [](int) {
            return skin::GameplaySkinAcquisition{
                .disposition = skin::GameplaySkinAcquisitionDisposition::BuiltIn};
          }},
          .skinInput = {},
          .recordFailure = {}};
}

void addLongNotePair(bms_parser::Chart &chart,
                     bms_parser::LongNoteType type, int lane,
                     long long headTimeMicros, long long tailTimeMicros) {
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  headTimeline->Timing = headTimeMicros;
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  tailTimeline->Timing = tailTimeMicros;
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
}

void configureTestGameplayCamera(int renderWidth, int renderHeight) {
  constexpr float cameraDepth = 2.1F;
  constexpr float laneLookAtY = AppSettings::kDefaultLaneLength * 0.25F;
  const float laneAngle = bx::toRad(AppSettings::kDefaultLaneAngleDegrees);
  const float uiScale = static_cast<float>(renderWidth) /
                        static_cast<float>(rendering::design_width);
  rendering::window_width = rendering::design_width;
  rendering::window_height = static_cast<int>(renderHeight / uiScale);
  rendering::render_width = renderWidth;
  rendering::render_height = renderHeight;
  rendering::ui_view_width = renderWidth;
  rendering::ui_view_height = renderHeight;
  rendering::game_camera.edit()
      .setPosition({4.0F, laneLookAtY - std::tan(laneAngle) * cameraDepth,
                    -cameraDepth})
      .setLookAt({4.0F, laneLookAtY, 0.0F})
      .setAspectRatio(static_cast<float>(rendering::window_width) /
                      static_cast<float>(rendering::window_height))
      .setNearClip(rendering::near_clip)
      .setFarClip(rendering::far_clip)
      .setViewRect(0, 0, static_cast<std::uint16_t>(renderWidth),
                   static_cast<std::uint16_t>(renderHeight))
      .commit();
}

void testModelReplayGhostsRetainRawLanesAndTimelinePositions() {
  PlayfieldChartVisualModel model;
  model.laneOrder = {4, 1};
  model.timelines = {
      {.id = 1,
       .timeMicros = 1'000,
       .scrollPosition = 1.0,
       .retainedForProjection = true},
      {.id = 2,
       .timeMicros = 2'000,
       .scrollPosition = 2.5,
       .retainedForProjection = true},
      {.id = 3,
       .timeMicros = 3'000,
       .scrollPosition = 4.0,
       .retainedForProjection = false},
  };
  ReplayData replay;
  replay.events = {
      {.action = ReplayEventAction::Press,
       .lane = 4,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 2'000,
       .judgement = Great},
      {.action = ReplayEventAction::Press,
       .lane = 9,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 2'000,
       .judgement = Great},
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'500,
       .judgeTimeMicros = 2'000,
       .judgement = Great},
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 3'000,
       .judgeTimeMicros = 3'000,
       .judgement = Great},
  };

  const auto ghosts = replay_ghost::buildReplayGhostEvents(replay, model);
  expect(ghosts.size() == 1 && ghosts.front().lane == 4 &&
             ghosts.front().noteTimeMicros == 1'000 &&
             ghosts.front().judgeScrollPosition == 2.5,
         "model-backed replay ghosts retain raw lanes and timeline positions");
}

void testExportPixelSizesMapToLogicalGameplayBounds() {
  const auto default4k =
      replay_video_export::replayGameplayLogicalUiBounds(3840, 2160);
  expect(default4k.x == 0.0 && default4k.y == 0.0 &&
             default4k.width == 1920.0 && default4k.height == 1080.0,
         "default 4K export uses the 1920x1080 logical gameplay viewport");
  const auto wide4k =
      replay_video_export::replayGameplayLogicalUiBounds(3840, 1600);
  expect(wide4k.x == 0.0 && wide4k.y == 0.0 && wide4k.width == 1920.0 &&
             wide4k.height == 800.0,
         "non-16:9 export preserves logical aspect ratio at design width");
}

void testReplayExportConfigPreservesGameplayPresentationSettings() {
  AppSettings settings;
  settings.visibleTimeGreenNumber = 777;
  settings.gameplayHispeedMultiplier = 1.75F;
  settings.visibleTimeUseMilliseconds = true;
  settings.visibleTimeBpmStrategy =
      AppSettings::VisibleTimeBpmStrategy::MostPrevalent;
  settings.laneBeamLengthPercent = 71;
  settings.noteStartPositionPercent = 40;
  settings.showInvisibleNotes = true;
  settings.markProcessedNotes = true;
  settings.judgementIndicatorEnabled = false;
  settings.judgementIndicatorY = 0.25F;
  settings.judgementIndicatorWidthScale = 0.75F;
  settings.judgementIndicatorRenderMode =
      AppSettings::JudgementIndicatorRenderMode::Hud2D;
  settings.judgementIndicatorRangeMilliseconds = 123;
  settings.judgementTextY = 0.6F;
  settings.judgementCounterEnabled = false;
  settings.judgementCounterPosition = AppSettings::JudgementCounterPosition::Top;
  settings.judgementTimingFastSlowCriteria =
      AppSettings::JudgementTimingDisplayCriteria::Off;
  settings.judgementTimingMillisecondsCriteria =
      AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow;
  settings.gaugeBarPosition = AppSettings::GaugeBarPosition::Left;

  const auto configuration =
      replay_video_export::replayGameplayPresentationConfig(
          settings, 9.5F, false, false);
  expect(configuration.visibleTimeGreenNumber == 777 &&
             configuration.hispeedMultiplier == 1.75F &&
             configuration.visibleTimeUseMilliseconds &&
             configuration.visibleTimeBpmStrategy ==
                 AppSettings::VisibleTimeBpmStrategy::MostPrevalent &&
             configuration.playAreaWidth == 9.5F &&
             configuration.laneBeamLengthPercent == 71 &&
             configuration.noteStartPositionPercent == 40 &&
             configuration.showInvisibleNotes &&
             configuration.markProcessedNotes &&
             !configuration.judgementIndicatorEnabled &&
             configuration.judgementIndicatorY == 0.25F &&
             configuration.judgementIndicatorWidthScale == 0.75F &&
             configuration.judgementIndicatorHudMode &&
             configuration.judgementIndicatorRangeMilliseconds == 123 &&
             configuration.judgementTextY == 0.6F &&
             !configuration.judgementCounterEnabled &&
             configuration.judgementCounterPosition ==
                 AppSettings::JudgementCounterPosition::Top &&
             configuration.fastSlowCriteria ==
                 AppSettings::JudgementTimingDisplayCriteria::Off &&
             configuration.millisecondsCriteria ==
                 AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow &&
             configuration.gaugeBarPosition == AppSettings::GaugeBarPosition::Left &&
             !configuration.touchVisualizationEnabled &&
             !configuration.replayGhostRenderingEnabled,
         "replay export configuration retains all gameplay presentation settings");
}

void testFirstExportFrameRefreshesPreparedRendererGeometry() {
  configureTestGameplayCamera(1920, 1080);
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "export-geometry replay adapter is created");
    return;
  }
  const float primaryUpperBound =
      created.presentation->builtInRenderer().projectionTraversal().upperBound;
  const std::uint64_t primaryGeometryRevision =
      created.presentation->builtInRenderer().touchLayoutRevision();

  configureTestGameplayCamera(3840, 1600);
  RenderContext context;
  (void)created.presentation->renderFrame(context, {.serial = 1}, {});
  const float exportUpperBound =
      created.presentation->builtInRenderer().projectionTraversal().upperBound;
  const std::uint64_t exportGeometryRevision =
      created.presentation->builtInRenderer().touchLayoutRevision();
  expect(exportUpperBound != primaryUpperBound &&
             exportGeometryRevision != primaryGeometryRevision,
         std::string("first non-primary-aspect export frame refreshes prepared "
                     "BMSRenderer geometry (primary=") +
             std::to_string(primaryUpperBound) +
             ", export=" + std::to_string(exportUpperBound) + ")");

  configureTestGameplayCamera(1920, 1080);
}

void testReplayGameplayFrameStateMirrorsLiveTimerAndStartClocks() {
  bms_parser::Chart chart;
  chart.Meta.PlayLength = 120'000'000;
  chart.Meta.TotalLength = 130'000'000;
  ReplayData replay;
  AppSettings settings;
  settings.audioOffsetMs = 20;
  settings.visualOffsetMs = 10;
  preparation::Plan plan;
  plan.playback = {.percent = 100};
  plan.playbackStartTimeMicros = -2'000'000;
  plan.metronome.enabled = true;
  plan.metronome.startTimeMicros = -1'000'000;

  const auto initial = replay_video_export::replayGameplayFrameState(
      plan, chart, replay, settings, 1, 0);
  expect(initial.clock.serial == 1 &&
             initial.clock.visualTimeMicros == -1'990'000 &&
             initial.clock.gameplayTimeMicros == -1'980'000 &&
             !initial.clock.playTimer.active &&
             initial.clock.playTimer.startMicros == 0 &&
             initial.clock.playTimer.elapsedMillisExact &&
             initial.clock.playTimer.playtimeMillis == 125'000 &&
             initial.sceneStartMicros == -990'000 &&
             initial.playStartMicros == 0,
         "initial export frame mirrors live scene/play/timer authority");

  const auto gameplay = replay_video_export::replayGameplayFrameState(
      plan, chart, replay, settings, 2, 2'000'000);
  expect(gameplay.clock.serial == 2 &&
             gameplay.clock.visualTimeMicros == 10'000 &&
             gameplay.clock.gameplayTimeMicros == 20'000 &&
             gameplay.clock.replayTouchTimeMicros == 20'000 &&
             gameplay.clock.bgaTimeMicros == 20'000 &&
             gameplay.clock.playTimer.active &&
             gameplay.clock.playTimer.startMicros == 0 &&
             gameplay.clock.playTimer.elapsedMillisExact &&
             gameplay.clock.playTimer.playtimeMillis == 125'000,
         "gameplay export frame keeps Timer 41 and progress authority live");
}

void testReplayGameplayStatePlayDeadlineMatchesPinnedBmsPlayer() {
  bms_parser::Chart chart;
  chart.Meta.PlayLength = 10'000'000;
  chart.Meta.TotalLength = 25'000'000;
  ReplayData replay;

  expect(replay_video_export::replayGameplayStatePlayDeadlineMicros(
             chart, replay) == 15'000'000,
         "manual replay export retains BMSPlayer's final five-second state-play margin");
  // A recorded autoplay is still BMSPlayerMode.REPLAY. Only its separately
  // launched AUTOPLAY mode selects model.getLastTime().
  replay.autoPlay = true;
  expect(replay_video_export::replayGameplayStatePlayDeadlineMicros(
             chart, replay) == 15'000'000,
         "recorded autoplay replay retains BMSPlayer REPLAY mode's last-note deadline");
  AppSettings settings;
  preparation::Plan plan;
  const auto autoplayReplayFrame = replay_video_export::replayGameplayFrameState(
      plan, chart, replay, settings, 1, 0);
  expect(autoplayReplayFrame.clock.playTimer.playtimeMillis == 15'000,
         "recorded autoplay replay exposes the same last-note TIMER_PLAY deadline");
}

void testReplayLaneCoverResetIsOneFramePulseForNormalAndCoursePlayback() {
  const std::vector<ReplayLaneCoverEvent> events = {
      {.songTimeMicros = 1'000,
       .noteStartPositionPercent = 35,
       .laneCoverEnabled = false,
       .resetVisibleTimeReference = true}};
  for (int path = 0; path < 2; ++path) {
    replay_video_export::ReplayLaneCoverPlayback playback(20, true);
    const auto before = playback.advance(events, 999);
    const auto eventFrame = playback.advance(events, 1'000);
    const auto nextFrame = playback.advance(events, 1'001);
    expect(before.percent == 20 && before.enabled &&
               !before.resetVisibleTimeReference && eventFrame.percent == 35 &&
               !eventFrame.enabled &&
               eventFrame.resetVisibleTimeReference && nextFrame.percent == 35 &&
               !nextFrame.enabled && !nextFrame.resetVisibleTimeReference,
           path == 0 ? "normal lane-cover reset is a one-frame pulse"
                     : "course lane-cover reset is a one-frame pulse");
  }
}

void testUnsubmittedReplayFrameReleasesItsPreparedBga() {
  TestBga bga;
  PresentationFrameResult frame{
      .frameSerial = 17,
      .outcome = PresentationFrameOutcome::CriticalFailure,
      .submittedMode = PresentationMode::Skin,
      .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin,
      .preparedBga = PreparedGameplayBgaFrame{.sequence = 71},
  };

  replay_video_export::releaseUnsubmittedReplayGameplayBga(bga, frame);
  expect(bga.finalizeCalls == 1,
         "an export-aborted presentation frame releases its BGA lease");
}

void testSelectedNormalPreflightAndDestructionUseRendererOwnership() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.PlayLength = 120'000'000;
  ReplayData replay;
  AppSettings settings;
  preparation::Plan plan;
  plan.playbackStartTimeMicros = -2'000'000;
  plan.metronome.enabled = true;
  plan.metronome.startTimeMicros = -1'000'000;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  SelectedSkinFixture fixture;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator rendererAccess(rendererMutex,
                                                    exportActive);
  bool creationBlockedDisplay = false;
  *fixture.createObserver = [&]() {
    std::string error;
    creationBlockedDisplay =
        !rendererAccess.tryAcquireDisplay(error).has_value();
  };

  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
  const auto failure = replay_video_export::preflightReplayGameplayPresentation(
      chart, replay, settings, plan, configuration, 3840, 2160, bga,
      fixture.services(), rendererAccess, presentation);
  expect(!failure && presentation != nullptr && creationBlockedDisplay &&
             fixture.receivedInitialState->has_value() &&
             (*fixture.receivedInitialState)->clock.serial == 1 &&
             (*fixture.receivedInitialState)->sceneStartMicros == -1'000'000 &&
             (*fixture.receivedInitialState)->playStartMicros == 0 &&
             fixture.receivedInitialProjection->has_value() &&
             (*fixture.receivedInitialProjection)->frameSerial == 1 &&
             fixture.receivedSafeUiBounds->has_value() &&
             (*fixture.receivedSafeUiBounds)->width == 1920.0 &&
             (*fixture.receivedSafeUiBounds)->height == 1080.0,
         "selected production preflight has valid state/projection/logical bounds under renderer ownership");
  const auto timing = presentation == nullptr
                          ? std::optional<skin::SkinGameplayTiming>{}
                          : presentation->selectedSkinGameplayTiming();
  expect(timing.has_value() && timing->finishMarginMillis == 701 &&
             timing->fadeoutMillis == 702,
         "selected replay presentation retains its authored end-animation timing");
  constexpr int exportFps = 60;
  const long long selectedGameplayDuration =
      replay_video_export::replayGameplayDurationWithSelectedSkinAnimation(
          chart, replay, plan, 0, exportFps, chart.Meta.PlayLength, false,
          presentation.get());
  const long long expectedSkinDeadlineMicros =
      chart.Meta.PlayLength + 5'000'000 + 701'000 + 702'000;
  const long long exportFrameMicros =
      (1'000'000LL + exportFps - 1) / exportFps;
  expect(selectedGameplayDuration ==
             plan.realTimeAtGameplayTime(expectedSkinDeadlineMicros, 0) +
                 3 * exportFrameMicros,
         "selected replay export retains the end-of-notes, finish-margin, and fadeout frames");
  if (!presentation) {
    return;
  }

  bool destructionBlockedDisplay = false;
  presentation->setDestructionObserverForTesting([&]() {
    std::string error;
    destructionBlockedDisplay =
        !rendererAccess.tryAcquireDisplay(error).has_value();
  });
  replay_video_export::destroyReplayGameplayPresentation(rendererAccess,
                                                         presentation);
  std::string error;
  expect(destructionBlockedDisplay && presentation == nullptr &&
             rendererAccess.tryAcquireDisplay(error).has_value(),
         "presentation destruction stays inside renderer ownership and releases it afterward");
}

void testNoSelectionKeepsOneAdapter() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  auto info = createInfo(chart, settings, configuration, bga);
  info.replayTouchSamples = {{.action = ReplayTouchAction::Down,
                              .fingerId = 9,
                              .songTimeMicros = 120,
                              .x = 0.25F,
                              .y = 0.75F}};
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation != nullptr && !created.failure,
         "no selected skin creates one coordinator-backed replay adapter");
}

void testSelectedFailureRetainsFactoryDiagnostic() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  auto info = createInfo(chart, settings, configuration, bga);
  info.skinServices.acquire = [](int) {
    return skin::GameplaySkinAcquisition{
        .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
        .failure = skin::GameplaySkinAcquisitionFailure{
            .diagnostic = {.code = "skin.test.selected_failure",
                           .message = "selected skin failed",
                           .severity = skin::DiagnosticSeverity::Error}}};
  };
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation == nullptr && created.failure &&
             created.failure->diagnostic.code == "skin.test.selected_failure",
         "selected-skin factory failure has no replay adapter and retains its diagnostic");
}

void testUnavailableSelectedSkinStopsBeforeAnyFrameWork() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  auto info = createInfo(chart, settings, configuration, bga);
  info.skinServices.acquire = [](int) {
    return skin::GameplaySkinAcquisition{
        .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
        .failure = skin::GameplaySkinAcquisitionFailure{
            .diagnostic = {.code = "skin.lifecycle.activation_unavailable",
                           .message = "The selected gameplay skin is unavailable.",
                           .severity = skin::DiagnosticSeverity::Error}}};
  };
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation == nullptr && created.failure &&
             created.failure->diagnostic.code ==
                 "skin.lifecycle.activation_unavailable" &&
             bga.prepareCalls == 0 && bga.submitCalls == 0 &&
             bga.fullscreenCalls == 0,
         "unavailable selected skin retains its diagnostic before any BGA frame work");
}

void testRealNormalExportPreflightStopsAudioAndMp4Work() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  ReplayData replay;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  GameplaySkinSessionServices skinServices{
      .acquire = [](int) {
        return skin::GameplaySkinAcquisition{
            .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
            .failure = skin::GameplaySkinAcquisitionFailure{
                .diagnostic = {
                    .code = "skin.lifecycle.activation_unavailable",
                    .message = "The selected gameplay skin is unavailable.",
                    .severity = skin::DiagnosticSeverity::Error}}};
      }};
  preparation::Plan plan;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator rendererAccess(rendererMutex,
                                                    exportActive);
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
  const auto preflight = replay_video_export::preflightReplayGameplayPresentation(
      chart, replay, settings, plan, configuration, 1280, 720, bga,
      std::move(skinServices), rendererAccess, presentation);
  int fakeAudioWork = 0;
  int fakeMp4Work = 0;
  const auto result = replay_video_export::runPreflightGatedNormalExport(
      [&]() { return preflight; }, [&]() -> ReplayVideoExportResult {
        ++fakeAudioWork;
        ++fakeMp4Work;
        return {.success = true};
      });
  expect(!result.success &&
             result.message.contains("skin.lifecycle.activation_unavailable") &&
             presentation == nullptr && fakeAudioWork == 0 &&
             fakeMp4Work == 0,
         "real selected-skin preflight stops normal audio and MP4 work");
}

struct CoursePreflightStage {
  bms_parser::Chart chart;
  ReplayData replay;
  GameplaySkinSessionServices skinServices;
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
};

struct FakeMediaWork {
  int calls = 0;
};

CoursePreflightStage readyStage(int keyMode) {
  CoursePreflightStage stage{
          .skinServices = {.acquire = [](int) {
            return skin::GameplaySkinAcquisition{
                .disposition = skin::GameplaySkinAcquisitionDisposition::BuiltIn};
          }}};
  stage.chart.Meta.KeyMode = keyMode;
  return stage;
}

CoursePreflightStage unavailableStage(int keyMode) {
  CoursePreflightStage stage{
          .skinServices = {.acquire = [](int) {
            return skin::GameplaySkinAcquisition{
                .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
                .failure = skin::GameplaySkinAcquisitionFailure{
                    .diagnostic = {
                        .code = "skin.lifecycle.activation_unavailable",
                        .message = "The selected gameplay skin is unavailable.",
                        .severity = skin::DiagnosticSeverity::Error}}};
          }}};
  stage.chart.Meta.KeyMode = keyMode;
  return stage;
}

std::optional<ReplayVideoExportResult>
preflightCourseFor(CoursePreflightStage ready, CoursePreflightStage unavailable) {
  AppSettings settings;
  TestBga bga;
  preparation::Plan plan;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator rendererAccess(rendererMutex,
                                                    exportActive);
  std::vector<replay_video_export::CourseReplayGameplayPreflightStage> stages;
  stages.reserve(2);
  stages.push_back({.chart = ready.chart,
                    .replay = ready.replay,
                    .preparationPlan = plan,
                    .configuration = {},
                    .exportWidth = 1280,
                    .exportHeight = 720,
                    .skinServices = std::move(ready.skinServices),
                    .presentation = ready.presentation});
  stages.push_back({.chart = unavailable.chart,
                    .replay = unavailable.replay,
                    .preparationPlan = plan,
                    .configuration = {},
                    .exportWidth = 1280,
                    .exportHeight = 720,
                    .skinServices = std::move(unavailable.skinServices),
                    .presentation = unavailable.presentation});
  return replay_video_export::preflightCourseReplayGameplayPresentations(
      stages, bga, settings, rendererAccess);
}

void testRealCoursePreflightStopsAllMediaWorkForLaterSkinFailure() {
  FakeMediaWork fakeAudioWork;
  FakeMediaWork fakeEncoderWork;
  const auto result = replay_video_export::runPreflightGatedNormalExport(
      [&]() { return preflightCourseFor(readyStage(7), unavailableStage(14)); },
      [&]() -> ReplayVideoExportResult {
        ++fakeAudioWork.calls;
        ++fakeEncoderWork.calls;
        return {.success = true};
      });
  expect(!result.success &&
             result.message ==
                 "The selected gameplay skin is unavailable.\n\n"
                 "[skin.lifecycle.activation_unavailable]",
         "a later course stage preserves the complete selected-skin diagnostic");
  expect(fakeAudioWork.calls == 0 && fakeEncoderWork.calls == 0,
         "course failure has no partial media output");
}

void testSelectedCoursePreflightUsesNonWidescreenLogicalBounds() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.PlayLength = 90'000'000;
  ReplayData replay;
  AppSettings settings;
  preparation::Plan plan;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  SelectedSkinFixture fixture;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator rendererAccess(rendererMutex,
                                                    exportActive);
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
  std::vector<replay_video_export::CourseReplayGameplayPreflightStage> stages;
  stages.push_back({.chart = chart,
                    .replay = replay,
                    .preparationPlan = plan,
                    .configuration = configuration,
                    .exportWidth = 3840,
                    .exportHeight = 1600,
                    .skinServices = fixture.services(),
                    .presentation = presentation});
  const auto failure =
      replay_video_export::preflightCourseReplayGameplayPresentations(
          stages, bga, settings, rendererAccess);
  expect(!failure && presentation != nullptr &&
             fixture.receivedSafeUiBounds->has_value() &&
             (*fixture.receivedSafeUiBounds)->width == 1920.0 &&
             (*fixture.receivedSafeUiBounds)->height == 800.0 &&
             fixture.receivedInitialState->has_value() &&
             (*fixture.receivedInitialState)->clock.serial == 1,
         "course selected-skin preflight maps non-widescreen pixels to logical bounds");
  replay_video_export::destroyReplayGameplayPresentation(rendererAccess,
                                                         presentation);
}

void testSelectedReadySkinReceivesOneInitialSnapshotAndSubmitsSkinFrame() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  SelectedSkinFixture fixture;
  PlayfieldVisualState requestedInitial;
  requestedInitial.clock = {
      .serial = 7,
      .visualTimeMicros = 55'000,
      .gameplayTimeMicros = 44'000,
      .replayTouchTimeMicros = 120,
      .bgaTimeMicros = 33'000,
      .playTimer = {.active = true,
                    .startMicros = 0,
                    .elapsedMillisExact = true,
                    .playtimeMillis = 125'000}};
  requestedInitial.sceneStartMicros = -500'000;
  requestedInitial.playStartMicros = 0;
  auto info = createInfo(chart, settings, configuration, bga);
  info.skinServices = fixture.services();
  info.skinInput.initialState = &requestedInitial;
  info.skinInput.safeUiBounds =
      {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0};
  info.replayTouchSamples = {{.action = ReplayTouchAction::Down,
                              .fingerId = 4,
                              .songTimeMicros = 120,
                              .x = 0.2F,
                              .y = 0.8F}};
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation != nullptr && !created.failure &&
             *fixture.receivedInitialStateCount == 1 &&
             *fixture.receivedInitialProjectionCount == 1 &&
             *fixture.receivedTouchCount == 1 &&
             fixture.receivedInitialState->has_value() &&
             (*fixture.receivedInitialState)->clock == requestedInitial.clock &&
             (*fixture.receivedInitialState)->sceneStartMicros == -500'000 &&
             (*fixture.receivedInitialState)->playStartMicros == 0 &&
             fixture.receivedInitialProjection->has_value() &&
             (*fixture.receivedInitialProjection)->frameSerial == 7,
         "selected ready skin receives one complete initial state/projection with replay touches");
  if (!created.presentation) {
    return;
  }

  RenderContext context;
  const auto frame = created.presentation->renderFrame(
      context, {.serial = 8, .replayTouchTimeMicros = 120}, {});
  expect(frame.frameSerial == 8 &&
             frame.outcome == PresentationFrameOutcome::Ready &&
             frame.submittedMode == PresentationMode::Skin &&
             bga.prepareCalls == 1 && bga.fullscreenCalls == 0,
         "selected ready skin renders one coordinator frame without built-in submission");
}

void testSelectedSkinRuntimeFailureDoesNotSubmitBuiltIn() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  SelectedSkinFixture fixture;
  auto info = createInfo(chart, settings, configuration, bga);
  PlayfieldVisualState requestedInitial;
  requestedInitial.clock.serial = 1;
  info.skinServices = fixture.services();
  info.skinInput.initialState = &requestedInitial;
  info.skinInput.safeUiBounds =
      {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0};
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation != nullptr && !created.failure,
         "selected skin is ready before its frame-time failure");
  if (!created.presentation) {
    return;
  }

  bga.throwOnPrepare = true;
  RenderContext context;
  const auto frame = created.presentation->renderFrame(context, {.serial = 1}, {});
  expect(frame.outcome == PresentationFrameOutcome::CriticalFailure &&
             frame.failure &&
             frame.failure->diagnostic.code ==
                 "skin.presentation.bga_prepare_failed" &&
             bga.prepareCalls == 1 && bga.fullscreenCalls == 0,
         "selected runtime failure returns its diagnostic without built-in submission");
}

void testClassicLongHeadSuppressesJudgeHudAndBgaMissClock() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  headTimeline->Timing = 1'000;
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  tailTimeline->Timing = 2'000;
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav,
                                         bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav,
                                         bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(1, head);
  tailTimeline->SetNote(1, tail);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  expect(created.presentation != nullptr, "classic long-note adapter is created");
  if (!created.presentation) {
    return;
  }
  const ReplayEvent event{.action = ReplayEventAction::Press,
                          .lane = 1,
                          .noteTimeMicros = 1'000,
                          .judgeTimeMicros = 1'010,
                          .judgement = PGreat,
                          .gauge = 0.73F};
  const PlayfieldJudgeEventClock clock{.songTimeMicros = 1'100,
                                       .visualTimeMicros = 1'050,
                                       .bgaTimeMicros = 1'025};
  expect(!created.presentation->applyReplayEvent(event, clock, true),
         "classic long-note head returns the exporter-equivalent suppressed HUD result");
  created.presentation->releaseDueClassicLongNoteTails(1'999);
  const auto beforeTail =
      created.presentation->captureVisualStateForTesting({});
  expect(beforeTail.notes.size() == 2 && beforeTail.notes[0].longActive &&
             beforeTail.notes[1].longActive && !beforeTail.notes[1].judged,
         "classic long-note tail stays held before its authored release time");
  created.presentation->releaseDueClassicLongNoteTails(2'000);
  const auto afterTail = created.presentation->captureVisualStateForTesting({});
  expect(afterTail.notes.size() == 2 && !afterTail.notes[0].longActive &&
             !afterTail.notes[1].longActive && afterTail.notes[1].judged &&
             afterTail.notes[1].playedTimeMicros == 2'000,
         "adapter releases a due classic long-note tail at its authored time");
  RenderContext context;
  const auto frame = created.presentation->renderFrame(
      context, {.serial = 1, .visualTimeMicros = 1'050}, {});
  expect(frame.outcome == PresentationFrameOutcome::Ready &&
             frame.submittedMode == PresentationMode::BuiltIn &&
             !bga.lastMissState.active,
         "classic long-note head remains lane-only and does not fan out a BGA miss");
}

void testBuiltInReplayPresentationPreprocessesGhostsAndMisses() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 1'000;
  measure->TimeLines.push_back(timeline);
  timeline->SetNote(1, new bms_parser::Note(bms_parser::Parser::NoWav));

  ReplayData replay;
  replay.events = {
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 1'010,
       .judgement = PGreat},
      {.action = ReplayEventAction::Miss,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 1'020,
       .judgement = Bad},
  };
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  auto info = createInfo(chart, settings, configuration, bga);
  info.replayData = &replay;
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation != nullptr &&
             created.presentation->builtInRenderer()
                     .replayGhostEventCountForTesting() == 1 &&
             created.presentation->builtInRenderer()
                     .replayMissMarkerCountForTesting() == 1,
         "built-in replay presentation receives ghost and miss-marker replay data");
}

void testAppliedJudgeCarriesTheProvidedBgaClockIntoSnapshot() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "judge clock adapter is created");
    return;
  }
  const ReplayEvent event{.action = ReplayEventAction::Press,
                          .lane = 1,
                          .judgement = PGreat,
                          .combo = 0};
  expect(created.presentation->applyReplayEvent(
             event, {.songTimeMicros = 900,
                     .visualTimeMicros = 800,
                     .bgaTimeMicros = 700},
             true),
         "non-classic replay judge reports exporter-equivalent HUD application");
  RenderContext context;
  (void)created.presentation->renderFrame(context, {.serial = 1}, {});
  expect(bga.lastMissState.active &&
             bga.lastMissState.startedBgaMicros == 700,
         "the adapter stores the supplied judge BGA clock before frame snapshot");
}

void testLongTailMissPreservesExporterEndpointSemantics() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  headTimeline->Timing = 1'000;
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  tailTimeline->Timing = 2'000;
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav,
                                         bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav,
                                         bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(1, head);
  tailTimeline->SetNote(1, tail);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "long-tail miss adapter is created");
    return;
  }
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat},
      {}, true);
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Miss,
       .lane = 1,
       .noteTimeMicros = 2'000,
       .judgeTimeMicros = 1'500,
       .judgement = Bad},
      {}, true);
  const auto state = created.presentation->captureVisualStateForTesting({});
  expect(state.notes.size() == 2 && state.notes[0].judged &&
             state.notes[1].judged && !state.notes[0].longActive &&
             !state.notes[1].longActive && !state.notes[1].dead,
         "tail miss before its timing clears both holds without marking the tail dead");
}

void testChargeLongReleaseClearsBothEndpointsWithoutReactivation() {
  for (const auto type : {bms_parser::LongNoteType::ChargeNote,
                          bms_parser::LongNoteType::HellChargeNote}) {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure;
    chart.Measures.push_back(measure);
    auto *headTimeline = new bms_parser::TimeLine(8, false);
    headTimeline->Timing = 1'000;
    auto *tailTimeline = new bms_parser::TimeLine(8, false);
    tailTimeline->Timing = 2'000;
    measure->TimeLines.push_back(headTimeline);
    measure->TimeLines.push_back(tailTimeline);
    auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
    auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
    head->Tail = tail;
    tail->Head = head;
    headTimeline->SetNote(1, head);
    tailTimeline->SetNote(1, tail);

    AppSettings settings;
    PlayfieldPresentationConfig configuration;
    TestBga bga;
    const auto created = ReplayPlayfieldPresentation::create(
        createInfo(chart, settings, configuration, bga));
    if (!created.presentation) {
      expect(false, "charge long-note adapter is created");
      continue;
    }
    (void)created.presentation->applyReplayEvent(
        {.action = ReplayEventAction::Press,
         .lane = 1,
         .noteTimeMicros = 1'000,
         .judgeTimeMicros = 1'000,
         .judgement = PGreat},
        {}, true);
    (void)created.presentation->applyReplayEvent(
        {.action = ReplayEventAction::Release,
         .lane = 1,
         .noteTimeMicros = 2'000,
         .judgeTimeMicros = 2'010,
         .judgement = PGreat},
        {}, true);
    const auto released =
        created.presentation->captureVisualStateForTesting({});
    expect(released.notes.size() == 2 && !released.notes[0].longActive &&
               !released.notes[1].longActive && released.notes[1].judged,
           "CN/HCN release clears both endpoints without reactivation");
  }
}

void testHcnBodyInputReprojectsBothEndpointStates() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  addLongNotePair(chart, bms_parser::LongNoteType::HellChargeNote, 1, 1'000,
                  3'000);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "HCN body-input replay adapter is created");
    return;
  }

  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat},
      {}, true);
  const auto held = created.presentation->captureVisualStateForTesting(
      {.visualTimeMicros = 1'500});
  expect(held.notes.size() == 2 && held.notes[0].longReactive &&
             held.notes[1].longReactive && held.notes[0].longActive &&
             held.notes[1].longActive && !held.notes[0].longDamaged &&
             !held.notes[1].longDamaged,
         "accepted HCN head projects active reactive state to both endpoints");

  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Release,
       .lane = 1,
       .noteTimeMicros = 3'000,
       .songTimeMicros = 1'500,
       .judgeTimeMicros = 1'500,
       .judgement = None},
      {}, true);
  const auto released = created.presentation->captureVisualStateForTesting(
      {.visualTimeMicros = 1'500});
  expect(released.notes.size() == 2 && !released.notes[0].longReactive &&
             !released.notes[1].longReactive && !released.notes[0].longActive &&
             !released.notes[1].longActive && released.notes[0].longDamaged &&
             released.notes[1].longDamaged,
         "judgement-less HCN body release projects damage to both endpoints");

  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = -1,
       .songTimeMicros = 2'000,
       .judgeTimeMicros = 2'000,
       .judgement = None},
      {}, true);
  const auto recovered = created.presentation->captureVisualStateForTesting(
      {.visualTimeMicros = 2'000});
  expect(recovered.notes.size() == 2 && recovered.notes[0].longReactive &&
             recovered.notes[1].longReactive && recovered.notes[0].longActive &&
             recovered.notes[1].longActive && !recovered.notes[0].longDamaged &&
             !recovered.notes[1].longDamaged,
         "HCN body re-press projects reactive recovery to both endpoints");
}

void testMissedHcnHeadRecoversFromRecordedLanePress() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  addLongNotePair(chart, bms_parser::LongNoteType::HellChargeNote, 1, 1'000,
                  3'000);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "missed-head HCN replay adapter is created");
    return;
  }

  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Miss,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'100,
       .judgeTimeMicros = 1'100,
       .judgement = Poor},
      {}, true);
  const auto missed = created.presentation->captureVisualStateForTesting(
      {.visualTimeMicros = 1'100});
  expect(missed.notes.size() == 2 && !missed.notes[0].longReactive &&
             !missed.notes[1].longReactive && !missed.notes[0].longActive &&
             !missed.notes[1].longActive && missed.notes[0].longDamaged &&
             missed.notes[1].longDamaged,
         "missed HCN head projects damage to both endpoints");

  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = -1,
       .songTimeMicros = 1'500,
       .judgeTimeMicros = 1'500,
       .judgement = None},
      {}, true);
  const auto recovered = created.presentation->captureVisualStateForTesting(
      {.visualTimeMicros = 1'500});
  expect(recovered.notes.size() == 2 && recovered.notes[0].longReactive &&
             recovered.notes[1].longReactive && recovered.notes[0].longActive &&
             recovered.notes[1].longActive && !recovered.notes[0].longDamaged &&
             !recovered.notes[1].longDamaged,
         "recorded lane press recovers both endpoints after a missed HCN head");
}

void testMineReplayEventFindsLandmineSource() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 3'000;
  timeline->SetLandmineNote(2, new bms_parser::LandmineNote(5.0F));
  measure->TimeLines.push_back(timeline);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "mine replay adapter is created");
    return;
  }
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Mine,
       .lane = 2,
       .noteTimeMicros = 3'000,
       .judgeTimeMicros = 3'010,
       .judgement = Bad},
      {}, true);
  const auto state = created.presentation->captureVisualStateForTesting({});
  expect(state.notes.size() == 1 && state.notes[0].judged &&
             state.notes[0].dead && state.notes[0].playedTimeMicros == 3'010,
         "mine replay event resolves the authored landmine visual source");
}

void testMaximumComboAdvancesOnlyWithAppliedReplayEvents() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "maximum-combo replay adapter is created");
    return;
  }
  replay_video_export::ReplayCourseMaximumComboPlayback courseMaximumCombo;
  expect(created.presentation->progressiveMaximumCombo() == 0 &&
             courseMaximumCombo.observe(*created.presentation) == 0,
         "course maximum combo starts at zero rather than the final result");
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .judgement = PGreat,
       .combo = 2},
      {}, true);
  expect(created.presentation->progressiveMaximumCombo() == 2 &&
             courseMaximumCombo.observe(*created.presentation) == 2,
         "maximum combo advances after the first applied judgement");
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .judgement = Great,
       .combo = 5},
      {}, true);
  expect(created.presentation->progressiveMaximumCombo() == 5 &&
             courseMaximumCombo.observe(*created.presentation) == 5,
         "maximum combo grows progressively per replay event");

  const auto nextStage = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!nextStage.presentation) {
    expect(false, "next course-stage replay adapter is created");
    return;
  }
  expect(courseMaximumCombo.observe(*nextStage.presentation) == 5,
         "course maximum combo carries into a new stage before its first event");
  (void)nextStage.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .judgement = Great,
       .combo = 3},
      {}, true);
  expect(courseMaximumCombo.observe(*nextStage.presentation) == 5,
         "a lower next-stage combo cannot reduce the progressive course maximum");
  (void)nextStage.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .judgement = Great,
       .combo = 8},
      {}, true);
  expect(courseMaximumCombo.observe(*nextStage.presentation) == 8,
         "the progressive course maximum advances in the next stage");
}

void testReplayAdapterCarriesStageFullComboAuthority() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  chart.Measures.push_back(measure);
  auto *first = new bms_parser::TimeLine(8, false);
  first->Timing = 1'000;
  first->SetNote(1, new bms_parser::Note(bms_parser::Parser::NoWav));
  measure->TimeLines.push_back(first);
  auto *second = new bms_parser::TimeLine(8, false);
  second->Timing = 2'000;
  second->SetNote(2, new bms_parser::Note(bms_parser::Parser::NoWav));
  measure->TimeLines.push_back(second);

  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  if (!created.presentation) {
    expect(false, "full-combo replay adapter is created");
    return;
  }
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat,
       .combo = 38},
      {.songTimeMicros = 1'000,
       .visualTimeMicros = 1'000,
       .bgaTimeMicros = 1'000},
      true);
  (void)created.presentation->applyReplayEvent(
      {.action = ReplayEventAction::Press,
       .lane = 2,
       .noteTimeMicros = 2'000,
       .judgeTimeMicros = 2'000,
       .judgement = PGreat,
       .combo = 39},
      {.songTimeMicros = 2'000,
       .visualTimeMicros = 2'000,
       .bgaTimeMicros = 2'000},
      true);
  created.presentation->applyAuthorityUpdate({.comboBreak = 0});
  const auto state = created.presentation->captureVisualStateForTesting({});
  expect(state.authority.stagePassedNotes == 2 &&
             state.authority.stageCombo == 2,
         "replay reducer retains stage-local full-combo authority instead of course combo");
}

} // namespace

int main() {
  if (SDL_Init(SDL_INIT_TIMER) != 0) {
    std::cerr << "FAIL: SDL timer initialization failed: " << SDL_GetError()
              << '\n';
    return 1;
  }
  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless Metal initialization failed\n";
    SDL_Quit();
    return 1;
  }
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  rendering::PosTexCoord0Vertex::init();
  testModelReplayGhostsRetainRawLanesAndTimelinePositions();
  testExportPixelSizesMapToLogicalGameplayBounds();
  testReplayExportConfigPreservesGameplayPresentationSettings();
  testFirstExportFrameRefreshesPreparedRendererGeometry();
  testReplayGameplayFrameStateMirrorsLiveTimerAndStartClocks();
  testReplayGameplayStatePlayDeadlineMatchesPinnedBmsPlayer();
  testReplayLaneCoverResetIsOneFramePulseForNormalAndCoursePlayback();
  testUnsubmittedReplayFrameReleasesItsPreparedBga();
  testSelectedNormalPreflightAndDestructionUseRendererOwnership();
  testNoSelectionKeepsOneAdapter();
  testSelectedFailureRetainsFactoryDiagnostic();
  testUnavailableSelectedSkinStopsBeforeAnyFrameWork();
  testRealNormalExportPreflightStopsAudioAndMp4Work();
  testRealCoursePreflightStopsAllMediaWorkForLaterSkinFailure();
  testSelectedCoursePreflightUsesNonWidescreenLogicalBounds();
  testSelectedReadySkinReceivesOneInitialSnapshotAndSubmitsSkinFrame();
  testSelectedSkinRuntimeFailureDoesNotSubmitBuiltIn();
  testClassicLongHeadSuppressesJudgeHudAndBgaMissClock();
  testBuiltInReplayPresentationPreprocessesGhostsAndMisses();
  testAppliedJudgeCarriesTheProvidedBgaClockIntoSnapshot();
  testLongTailMissPreservesExporterEndpointSemantics();
  testChargeLongReleaseClearsBothEndpointsWithoutReactivation();
  testHcnBodyInputReprojectsBothEndpointStates();
  testMissedHcnHeadRecoversFromRecordedLanePress();
  testMineReplayEventFindsLandmineSource();
  testMaximumComboAdvancesOnlyWithAppliedReplayEvents();
  testReplayAdapterCarriesStageFullComboAuthority();
  bgfx::shutdown();
  SDL_Quit();
  if (failures != 0) {
    std::cerr << failures << " replay playfield presentation test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playfield presentation tests passed\n";
  return 0;
}
