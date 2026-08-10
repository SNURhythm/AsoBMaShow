#include "scene/play/ReplayPlayfieldPresentation.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"
#include "view/View.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

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

  SelectedSkinFixture() {
    fs::create_directories(roots.visiblePackages);
    const fs::path packageRoot = roots.visiblePackages / entry.package.directoryName;
    fs::create_directories(packageRoot / "skin");
    std::ofstream(packageRoot / "skin/main.luaskin")
        << "return { type = 0, w = 1280, h = 720 }\n";
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
                 touchCount = receivedTouchCount](
                    skin::ValidatedSkinActivation activation,
                    skin::PlaySkinSessionContext context) {
                  if (context.initialState != nullptr) {
                    ++*initialStateCount;
                    *touchCount = static_cast<int>(context.initialState->touches.size());
                  }
                  if (context.initialProjection != nullptr) {
                    ++*initialProjectionCount;
                  }
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

void testSelectedReadySkinReceivesOneInitialSnapshotAndSubmitsSkinFrame() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  SelectedSkinFixture fixture;
  PlayfieldVisualState requestedInitial;
  requestedInitial.clock = {.serial = 7, .replayTouchTimeMicros = 120};
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
             *fixture.receivedTouchCount == 1,
         "selected ready skin receives one initial state/projection with replay touches");
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
  RenderContext context;
  const auto frame = created.presentation->renderFrame(
      context, {.serial = 1, .visualTimeMicros = 1'050}, {});
  expect(frame.outcome == PresentationFrameOutcome::Ready &&
             frame.submittedMode == PresentationMode::BuiltIn &&
             !bga.lastMissState.active,
         "classic long-note head remains lane-only and does not fan out a BGA miss");
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
  testNoSelectionKeepsOneAdapter();
  testSelectedFailureRetainsFactoryDiagnostic();
  testUnavailableSelectedSkinStopsBeforeAnyFrameWork();
  testSelectedReadySkinReceivesOneInitialSnapshotAndSubmitsSkinFrame();
  testSelectedSkinRuntimeFailureDoesNotSubmitBuiltIn();
  testClassicLongHeadSuppressesJudgeHudAndBgaMissClock();
  testAppliedJudgeCarriesTheProvidedBgaClockIntoSnapshot();
  testLongTailMissPreservesExporterEndpointSemantics();
  bgfx::shutdown();
  SDL_Quit();
  if (failures != 0) {
    std::cerr << failures << " replay playfield presentation test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playfield presentation tests passed\n";
  return 0;
}
