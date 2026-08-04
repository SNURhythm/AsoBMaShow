#include "skin/beatoraja/PlaySkinSession.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-play-skin-session-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class EmptyResources final : public SkinPreparedResourceView {
public:
  const PreparedSkinResource *find(SkinResourceId) const noexcept override {
    return nullptr;
  }
  const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId,
                     const SkinSourceRect &) const noexcept override {
    return nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId) const noexcept override {
    return nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept override {
    return nullptr;
  }
};

class SerialOnlyState final : public ISkinFrameState {
public:
  explicit SerialOnlyState(std::uint64_t serial) : serial_(serial) {}

  std::uint64_t frameSerial() const noexcept override { return serial_; }
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<ConfigOffset> offsetProperty(int) override { return {}; }
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override {
    return INT64_MIN;
  }
  std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override {
    return {};
  }
  SkinGaugeStateView gaugeState() const noexcept override { return {}; }
  SkinJudgeStateView judgeState(int) const noexcept override { return {}; }
  SkinNoteExpansionStateView noteExpansionState() const noexcept override {
    return {};
  }

private:
  std::uint64_t serial_ = 0;
};

PlayfieldVisualState stateAt(std::uint64_t serial) {
  PlayfieldVisualState state;
  state.clock.serial = serial;
  state.clock.visualTimeMicros = static_cast<long long>(serial) * 10'000;
  state.clock.gameplayTimeMicros = state.clock.visualTimeMicros;
  return state;
}

PlayfieldProjectionResult projectionAt(std::uint64_t serial) {
  PlayfieldProjectionResult projection;
  projection.frameSerial = serial;
  return projection;
}

bool hasDiagnostic(std::span<const SkinDiagnostic> diagnostics,
                   std::string_view code) {
  return std::ranges::any_of(
      diagnostics,
      [code](const SkinDiagnostic &diagnostic) { return diagnostic.code == code; });
}

bool hasDiagnostic(const PlaySkinFrameTransactionResult &result,
                   std::string_view code) {
  return hasDiagnostic(result.diagnostics, code) ||
         hasDiagnostic(result.evaluation.diagnostics, code);
}

class SessionTextureDevice final : public SkinTextureDevice {
public:
  SessionTextureDevice() : owner_(std::this_thread::get_id()) {}

  bgfx::TextureHandle
  create(const image_decode::DecodedImageData &) override {
    ++createCalls;
    if (!ownsCurrentThread()) {
      ++wrongThreadOperations;
    }
    if (stopAfterCreateCalls && createCalls == *stopAfterCreateCalls) {
      stopSource->request_stop();
    }
    return bgfx::TextureHandle{nextHandle_++};
  }

  void destroy(bgfx::TextureHandle) noexcept override {
    ++destroyCalls;
    if (!ownsCurrentThread()) {
      ++wrongThreadOperations;
    }
    if (observedRevision && !observedRevision->hasLiveLease()) {
      revisionLiveDuringDestroy = false;
    }
  }

  bool ownsCurrentThread() const noexcept override {
    return std::this_thread::get_id() == owner_;
  }

  void observeRevision(SkinRevisionWeakPin revision) {
    observedRevision = std::move(revision);
  }

  void requestStopAfter(std::size_t createCall, std::stop_source &source) {
    stopAfterCreateCalls = createCall;
    stopSource = &source;
  }

  std::size_t createCalls = 0;
  std::size_t destroyCalls = 0;
  std::size_t wrongThreadOperations = 0;
  bool revisionLiveDuringDestroy = true;

private:
  std::thread::id owner_;
  std::uint16_t nextHandle_ = 1;
  std::optional<SkinRevisionWeakPin> observedRevision;
  std::optional<std::size_t> stopAfterCreateCalls;
  std::stop_source *stopSource = nullptr;
};

struct ActivationFixtureOptions {
  bool resourceBearing = false;
};

class ActivationFixture final {
public:
  explicit ActivationFixture(ActivationFixtureOptions options = {})
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("ActivationContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry),
        profile_(*makeSkinProfileId(
            "66666666-6666-4666-8666-666666666666")),
        device_(std::make_shared<SessionTextureDevice>()) {
    chart_.text.title = "Artist 日本 42";
    chart_.text.subtitle = "Session subtitle";
    chart_.text.artist = "Session artist";

    const fs::path source = temp_.root() / "source";
    if (options.resourceBearing) {
      fs::create_directories(source / "skin/resources");
      fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/resources/fixture.png",
                    source / "skin/resources/fixture.png");
      fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                    source / "skin/resources/fixture.ttf");
    }
    std::string script = R"lua(
local phase_count = (rawget(_G, "session_activation_phase_count") or 0) + 1
_G.session_activation_phase_count = phase_count
if skin_config then
  if phase_count ~= 2 then
    error("configured phase did not reuse exactly one fresh header state")
  end
  local marker = io.open("configured-phase-marker.txt", "w")
  if marker then
    marker:write("configured")
    marker:close()
  end
)lua";
    if (options.resourceBearing) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {{id = "fixture-image", path = "resources/fixture.png"}},
    font = {{id = "fixture-font", path = "resources/fixture.ttf", type = 0}},
    image = {{id = "fixture-object", src = "fixture-image", x = 0, y = 0, w = 40, h = 20}},
    text = {{id = "runtime-title", font = "fixture-font", size = 16, ref = 10}},
    destination = {
      {id = "fixture-object", dst = {{x = 0, y = 0, w = 40, h = 20}}},
      {id = "runtime-title", dst = {{x = 50, y = 50, w = 500, h = 30}}}
    }
  }
)lua";
    } else {
      script += R"lua(
  return { type = 0, w = 1280, h = 720 }
)lua";
    }
    script += R"lua(
end
if phase_count ~= 1 then
  error("header phase did not begin in a fresh state")
end
return { type = 0, name = "activation shell", w = 1280, h = 720 }
)lua";
    writeText(source / "skin/main.luaskin", script);

    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(),
           "activation fixture snapshots an immutable revision");
    if (!snapshot.prepared) {
      return;
    }
    std::string publishError;
    lease_ = std::move(*snapshot.prepared).publish(publishError);
    expect(lease_.has_value() && publishError.empty(),
           "activation fixture publishes the immutable revision");
    if (!lease_) {
      return;
    }

    GameplaySkinValidator validator(resources_);
    validation_ = validator.validate(lease_->readView(), entry_, &desired_, {});
    expect(validation_.disposition ==
               SkinValidationDisposition::Selectable7Key &&
               validation_.reconciledSettings.has_value() &&
               !validation_.configurationDigest.empty(),
           "activation fixture validates a selectable 7-key skin");
  }

  bool ready() const noexcept {
    return lease_.has_value() && validation_.reconciledSettings.has_value() &&
           !validation_.configurationDigest.empty();
  }

  ValidatedSkinActivation takeActivation() {
    return {.revision = std::move(*lease_),
            .entry = entry_,
            .reconciledSettings = *validation_.reconciledSettings,
            .configurationDigest = validation_.configurationDigest};
  }

  PlaySkinSessionContext context(
      ViewportSettings viewport = {}, std::stop_token stop = {}) {
    return {.sessionSerial = 73,
            .profileId = profile_,
            .chartModel = chart_,
            .viewport = viewport,
            .safeUiBounds = {.x = 0.0,
                             .y = 0.0,
                             .width = 1280.0,
                             .height = 720.0},
            .storageRoots = roots_,
            .resourcePreparation = resources_,
            .textureDevice = device_,
            .stop = stop};
  }

  const SkinEntryId &entry() const noexcept { return entry_; }
  const SkinProfileId &profile() const noexcept { return profile_; }
  const std::string &configurationDigest() const noexcept {
    return validation_.configurationDigest;
  }
  fs::path configuredMarkerPath() const {
    const auto overlay =
        deriveSkinPrivateOverlayRoot(roots_, profile_, entry_);
    return *overlay.root / "skin/configured-phase-marker.txt";
  }
  const std::shared_ptr<SessionTextureDevice> &device() const noexcept {
    return device_;
  }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  EntryProfileSettings desired_;
  SkinResourcePreparationService resources_;
  std::shared_ptr<SessionTextureDevice> device_;
  std::optional<SkinRevisionLease> lease_;
  SkinValidationResult validation_;
};

void testActivationCreatesAnOwningFreshStateSession() {
  ActivationFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  const std::string revisionDigest =
      activation.revision.revision().lowercaseSha256;
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Stretch,
      .scaleX = 0.5F,
      .scaleY = 0.5F,
      .translateX = 10.0F,
      .translateY = -20.0F,
  };

  auto created = PlaySkinSession::create(
      std::move(activation), fixture.context(customViewport));
  expect(created.session != nullptr && !created.cancelled &&
             created.configurationDigest == fixture.configurationDigest() &&
             created.reconciledSettings.viewport == customViewport,
         "activation creates one fresh owning session and context viewport wins");
  if (!created.session) {
    return;
  }

  const auto &identity = created.session->identity();
  expect(identity.sessionSerial == 73 &&
             identity.profileId == fixture.profile() &&
             identity.entry == fixture.entry() &&
             identity.revisionDigest == revisionDigest &&
             identity.configurationDigest == fixture.configurationDigest(),
         "session identity retains the exact immutable activation identity");

  const auto frame = created.session->prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(frame.ready() && frame.evaluation.interactionLayout.has_value(),
         "fresh activation session enters render phase and prepares a frame");
  if (frame.evaluation.interactionLayout) {
    const auto &transform = frame.evaluation.interactionLayout->uiToAuthored;
    expect(transform.m00 == 2.0 && transform.m11 == -2.0 &&
               transform.tx == -660.0 && transform.ty == 1040.0,
           "context custom viewport is the frame's exact authored transform");
  }
  expect(weakRevision.hasLiveLease(),
         "session and uploaded catalog retain immutable revision pins");
  created.session.reset();
  expect(!weakRevision.hasLiveLease(),
         "final session and resource teardown releases every revision pin");
}

void testActivationRejectsAReconciledDigestMismatch() {
  ActivationFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  activation.configurationDigest.assign(64, '0');
  const auto expectedDigest = fixture.configurationDigest();

  auto created =
      PlaySkinSession::create(std::move(activation), fixture.context());
  expect(!created.session && !created.cancelled &&
             created.configurationDigest == expectedDigest &&
             hasDiagnostic(created.diagnostics,
                           "skin.session.configuration_digest_mismatch") &&
             !fs::exists(fixture.configuredMarkerPath()),
         "digest mismatch rejects before configured-phase sandbox writes");
}

void testResourceSessionOwnsUploadsAndExactRuntimeStringAtlas() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);

  auto created =
      PlaySkinSession::create(std::move(activation), fixture.context());
  expect(created.session && fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 0 &&
             fixture.device()->wrongThreadOperations == 0 &&
             weakRevision.hasLiveLease(),
         "session owns one image and one runtime-string glyph atlas upload");
  if (!created.session) {
    return;
  }
  const auto frame = created.session->prepareFrame(
      stateAt(1), projectionAt(1), {});
  const auto textCommand =
      frame.evaluation.submitReady
          ? std::ranges::find_if(
                frame.evaluation.submitReady->commands,
                [](const SkinDrawCommand &command) {
                  return std::holds_alternative<SkinGlyphRunCommand>(
                      command.payload);
                })
          : std::vector<SkinDrawCommand>::const_iterator{};
  const bool emittedTitle =
      frame.evaluation.submitReady &&
      textCommand != frame.evaluation.submitReady->commands.end() &&
      std::get<SkinGlyphRunCommand>(textCommand->payload).glyphs.size() == 12;
  expect(frame.ready() && emittedTitle,
         "title selector emits one runtime text command with every glyph");

  created.session.reset();
  expect(fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease(),
         "catalog textures tear down on owner thread before the final revision "
         "pin releases");
}

void testPostUploadCancellationRollsBackResourcesOnOwnerThread() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);
  std::stop_source stop;
  fixture.device()->requestStopAfter(2, stop);

  auto created = PlaySkinSession::create(
      std::move(activation), fixture.context({}, stop.get_token()));
  expect(created.cancelled && !created.session && fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease(),
         "post-upload cancellation destroys both owner-thread textures and "
         "releases the final revision pin");
}

void testInvalidViewportRollsBackUploadedResourcesOnOwnerThread() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);
  auto context = fixture.context();
  context.safeUiBounds.width = 0.0;

  auto created =
      PlaySkinSession::create(std::move(activation), std::move(context));
  expect(!created.session &&
             hasDiagnostic(created.diagnostics,
                           "skin.session.viewport_invalid") &&
             fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease(),
         "post-upload viewport failure rolls back resources and releases all "
         "revision pins on the owner thread");
}

void testActivationCancellationAndZeroSerialDoNotPublishSessions() {
  {
    ActivationFixture fixture;
    if (fixture.ready()) {
      std::stop_source stop;
      stop.request_stop();
      auto created = PlaySkinSession::create(
          fixture.takeActivation(), fixture.context({}, stop.get_token()));
      expect(!created.session && created.cancelled,
             "pre-cancelled activation publishes no session");
    }
  }
  {
    ActivationFixture fixture;
    if (fixture.ready()) {
      auto context = fixture.context();
      context.sessionSerial = 0;
      auto created =
          PlaySkinSession::create(fixture.takeActivation(), std::move(context));
      expect(!created.session && !created.cancelled &&
                 hasDiagnostic(created.diagnostics,
                               "skin.session.serial_invalid"),
             "zero session serial is rejected before runtime publication");
    }
  }
}

const SessionPresentationWrite *presentationMutation(
    const SkinFrameMutation &mutation) {
  return std::get_if<SessionPresentationWrite>(&mutation);
}

class SessionFixture final {
public:
  explicit SessionFixture(std::uint64_t sessionSerial = 37)
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("SessionContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry),
        viewport_(evaluatePlaySkinViewport(
            {.width = 1280.0, .height = 720.0},
            {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0}, {})) {
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/main.luaskin", R"lua(
local captured_main_state = require("main_state")
return {
  type = 0,
  writer_a = function(value)
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  writer_b = function(value)
    captured_main_state.event_exec(901, math.floor(value * 100 + 0.5))
  end,
  writer_fail = function()
    captured_main_state.event_exec(900, 99)
    error("forced writer failure")
  end,
}
)lua");
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "session runtime fixture snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = prepared_->readView(),
         .entry = entry_,
         .storageRoots = roots_,
         .profileId =
             *makeSkinProfileId("55555555-5555-4555-8555-555555555555")});
    expect(fileSystem.fileSystem != nullptr,
           "session runtime filesystem creates");
    if (!fileSystem.fileSystem) {
      return;
    }
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    runtime_ = std::move(created.runtime);
    expect(runtime_ != nullptr, "session gameplay runtime creates");
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "session runtime header executes");
    if (!header.value) {
      return;
    }
    writerA_ = header.value->callbackNamed("writer_a");
    writerB_ = header.value->callbackNamed("writer_b");
    writerFail_ = header.value->callbackNamed("writer_fail");
    expect(writerA_ && writerB_ && writerFail_,
           "session writer callbacks are retained");
    header.value.reset();
    expect(runtime_->loadConfigured({}).value.has_value(),
           "session runtime configured phase executes");
    expect(runtime_->enterRenderPhase().ok,
           "session runtime enters render phase");

    model_.model.floatWriters = {
        {.id = SkinFloatWriterId{1}, .source = *writerA_},
        {.id = SkinFloatWriterId{2}, .source = *writerB_},
        {.id = SkinFloatWriterId{3}, .source = *writerFail_},
    };
    auto pinned = makePinnedSkinEventMutationTableV1();
    std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                              pinned.rules().end());
    rules.push_back({.builtInEventId = 900,
                     .kind = SkinEventMutationKind::SessionPresentation,
                     .maximumArguments = 2});
    rules.push_back({.builtInEventId = 901,
                     .kind = SkinEventMutationKind::SessionPresentation,
                     .maximumArguments = 2});
    mutations_ = SkinEventMutationTable(std::move(rules));
    bridge_ = std::make_unique<PlaySkinStateBridge>(PlaySkinStateBridgeContext{
        .chartModel = chart_,
        .model = model_,
        .configuration = configuration_,
        .runtime = *runtime_,
        .mutationTable = mutations_});
    session_ = std::make_unique<PlaySkinSession>(PlaySkinSessionFrameContext{
        .sessionSerial = sessionSerial,
        .model = model_,
        .configuration = configuration_,
        .resources = resources_,
        .viewport = viewport_,
        .runtime = *runtime_,
        .bridge = *bridge_,
        .renderer = renderer_});
  }

  bool ready() const noexcept { return session_ != nullptr; }
  PlaySkinSession &session() { return *session_; }
  PlaySkinStateBridge &bridge() { return *bridge_; }
  LuaSkinRuntime &runtime() { return *runtime_; }
  Skin2DRenderer &renderer() { return renderer_; }
  ValidatedBeatorajaSkinModel &model() { return model_; }
  const BeatorajaSkinConfiguration &configuration() const {
    return configuration_;
  }
  const EmptyResources &resources() const { return resources_; }
  const PlaySkinViewport &viewport() const { return viewport_; }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> writerA_;
  std::optional<LuaCallbackId> writerB_;
  std::optional<LuaCallbackId> writerFail_;
  PlayfieldChartVisualModel chart_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  SkinEventMutationTable mutations_;
  EmptyResources resources_;
  PlaySkinViewport viewport_;
  Skin2DRenderer renderer_;
  std::unique_ptr<PlaySkinStateBridge> bridge_;
  std::unique_ptr<PlaySkinSession> session_;
};

void testSuccessfulFrameCommitsWriterMutationsInInputOrder() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.2,
                           .eventMicros = 10},
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.7,
                           .eventMicros = 20},
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 4.0,
                           .eventMicros = 30},
  };
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(result.ready() && result.frameSerial == 1 &&
             result.evaluation.submitReady &&
             result.committed.frameSerial == 1,
         "matched empty-custom frame evaluates and commits exactly once");
  expect(result.committed.orderedMutations.size() == 3,
         "every successful queued writer contributes one ordered mutation");
  const std::array expectedEvents{900, 901, 900};
  const std::array expectedArguments{20, 70, 100};
  for (std::size_t index = 0;
       index < result.committed.orderedMutations.size() && index < 3; ++index) {
    const auto *mutation =
        presentationMutation(result.committed.orderedMutations[index]);
    expect(mutation && mutation->eventId == expectedEvents[index] &&
               mutation->argumentCount == 1 &&
               mutation->arguments[0] == expectedArguments[index],
           "queued writer mutation order and clamped values are preserved");
  }
  expect(fixture.bridge().frameSerial() == 0,
         "successful publication closes the bridge transaction");

  const auto duplicate = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(!duplicate.ready() && !duplicate.evaluation.submitReady &&
             duplicate.committed.frameSerial == 0 &&
             hasDiagnostic(duplicate,
                           "skin.play_state.frame_serial_not_increasing"),
         "one visual serial cannot begin or commit a second frame");
}

void testWriterFailureDiscardsEarlierAndFailedCallbackMutations() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.25},
      SkinWriterInvocation{.writer = SkinFloatWriterId{3},
                           .normalizedValue = 0.5},
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.75},
  };
  const auto failed = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(!failed.ready() && !failed.evaluation.submitReady &&
             failed.committed.orderedMutations.empty() &&
             hasDiagnostic(failed, "skin_lua_execution_failed") &&
             fixture.bridge().frameSerial() == 0,
         "writer failure discards the whole frame transaction");

  const std::array nextWriters{
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.4}};
  const auto next = fixture.session().prepareFrame(
      stateAt(2), projectionAt(2), nextWriters);
  expect(next.ready() && next.committed.orderedMutations.size() == 1,
         "writer failure does not poison the next visual frame budget");
  if (!next.committed.orderedMutations.empty()) {
    const auto *mutation =
        presentationMutation(next.committed.orderedMutations.front());
    expect(mutation && mutation->eventId == 901 &&
               mutation->arguments[0] == 40,
           "only the next frame mutation is published after rollback");
  }
}

void testEvaluatorFailureDiscardsWriterTransaction() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.model().model.destinations.push_back(
      {.object = 999, .presentation = {.authoredOrdinal = 1}});
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.3}};
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(!result.ready() && !result.evaluation.submitReady &&
             result.committed.orderedMutations.empty() &&
             hasDiagnostic(result, "skin.renderer.model.destination_object") &&
             fixture.bridge().frameSerial() == 0,
         "whole-buffer evaluation failure rolls back staged writers");
}

void testSerialMismatchDoesNotConsumeRuntimeFrame() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const auto mismatch = fixture.session().prepareFrame(
      stateAt(10), projectionAt(11), {});
  expect(!mismatch.ready() &&
             hasDiagnostic(mismatch, "skin.play_state.frame_serial_invalid"),
         "mismatched state and projection fail before runtime frame begin");
  const auto corrected = fixture.session().prepareFrame(
      stateAt(10), projectionAt(10), {});
  expect(corrected.ready() && corrected.committed.frameSerial == 10,
         "corrected matched serial can still begin exactly once");
}

void testInvalidSessionSerialDoesNotConsumeFrameOwners() {
  SessionFixture fixture(0);
  if (!fixture.ready()) {
    return;
  }
  const auto rejected = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(!rejected.ready() &&
             hasDiagnostic(rejected, "skin.session.serial_invalid") &&
             fixture.bridge().frameSerial() == 0,
         "zero session serial fails before opening the bridge frame");

  SerialOnlyState state(1);
  const auto evaluation = fixture.renderer().evaluateFrame({
      .frameSerial = 1,
      .sessionSerial = 37,
      .visualTimeMicros = 10'000,
      .model = fixture.model(),
      .configuration = fixture.configuration(),
      .resources = fixture.resources(),
      .viewport = fixture.viewport(),
      .runtime = fixture.runtime(),
      .state = state,
  });
  expect(evaluation.submitReady.has_value(),
         "zero session serial does not consume the Lua frame serial");
}

void testPassiveCustomTimerUsesTheSharedSessionFrame() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.model().model.customTimers.push_back(
      {.id = 10'001, .timer = std::nullopt});
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(result.ready() && result.committed.frameSerial == 1 &&
             result.committed.orderedMutations.empty(),
         "nonempty passive custom timers execute within the shared session frame");
}

void testLegacyRendererAdapterBeginsInternallyAndRejectsDoubleBegin() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const auto sessionFrame = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(sessionFrame.ready(),
         "session-owned renderer evaluation consumes external ownership");

  SerialOnlyState state(2);
  const auto inputs = SkinFrameInputs{
      .frameSerial = 2,
      .sessionSerial = 37,
      .visualTimeMicros = 20'000,
      .model = fixture.model(),
      .configuration = fixture.configuration(),
      .resources = fixture.resources(),
      .viewport = fixture.viewport(),
      .runtime = fixture.runtime(),
      .state = state};
  const auto first = fixture.renderer().evaluateFrame(inputs);
  const auto second = fixture.renderer().evaluateFrame(inputs);
  expect(first.submitReady.has_value(),
         "legacy renderer adapter still owns an internal runtime begin");
  expect(!second.submitReady &&
             hasDiagnostic(second.diagnostics, "skin_lua_frame_invalid"),
         "legacy double begin is rejected deterministically");
}

} // namespace

int main() {
  testActivationCreatesAnOwningFreshStateSession();
  testActivationRejectsAReconciledDigestMismatch();
  testResourceSessionOwnsUploadsAndExactRuntimeStringAtlas();
  testPostUploadCancellationRollsBackResourcesOnOwnerThread();
  testInvalidViewportRollsBackUploadedResourcesOnOwnerThread();
  testActivationCancellationAndZeroSerialDoNotPublishSessions();
  testSuccessfulFrameCommitsWriterMutationsInInputOrder();
  testWriterFailureDiscardsEarlierAndFailedCallbackMutations();
  testEvaluatorFailureDiscardsWriterTransaction();
  testSerialMismatchDoesNotConsumeRuntimeFrame();
  testInvalidSessionSerialDoesNotConsumeFrameOwners();
  testPassiveCustomTimerUsesTheSharedSessionFrame();
  testLegacyRendererAdapterBeginsInternallyAndRejectsDoubleBegin();
  if (failures != 0) {
    std::cerr << failures << " play skin session test(s) failed\n";
    return 1;
  }
  std::cout << "play skin session tests passed\n";
  return 0;
}
