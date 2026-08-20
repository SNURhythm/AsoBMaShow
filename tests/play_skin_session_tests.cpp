#include "skin/beatoraja/PlaySkinSession.h"

#include "rendering/SkinQuadBatchRenderer.h"
#include "scene/play/PlayfieldPresentation.h"
#include "skin/SkinStoragePaths.h"
#include "skin/SkinConfigurationWriteQueue.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/SyntheticReplayGhostOverlay.h"
#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "view/View.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace session_test_allocation_fault {
thread_local bool failNext = false;
}

void *operator new(std::size_t size) {
  if (session_test_allocation_fault::failNext) {
    session_test_allocation_fault::failNext = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace rendering {
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
} // namespace rendering

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

class SessionResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id) {
    const SkinSourceRect region{.x = 0, .y = 0, .w = 10, .h = 10};
    PreparedSkinResource resource;
    resource.id = id;
    resource.texture = bgfx::TextureHandle{
        static_cast<std::uint16_t>(id == 0 ? 1 : id)};
    resource.width = 10;
    resource.height = 10;
    resource.regions = {region};
    resource.regionMappings = {{.authored = region, .resolved = region}};
    resources_.emplace(id, std::move(resource));
  }

  const PreparedSkinResource *find(SkinResourceId id) const noexcept override {
    const auto found = resources_.find(id);
    return found == resources_.end() ? nullptr : &found->second;
  }
  const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId id,
                     const SkinSourceRect &region) const noexcept override {
    const auto *resource = find(id);
    if (resource == nullptr || resource->regionMappings.empty()) {
      return nullptr;
    }
    const auto &resolved = resource->regionMappings.front();
    return resolved.authored.x == region.x && resolved.authored.y == region.y &&
                   resolved.authored.w == region.w &&
                   resolved.authored.h == region.h
               ? &resolved
               : nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId) const noexcept override {
    return nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept override {
    return nullptr;
  }

private:
  std::map<SkinResourceId, PreparedSkinResource> resources_;
};

class SessionQuadBackend final : public rendering::SkinQuadBatchBackend {
public:
  bool preflightVertexLayouts(
      std::span<const bgfx::VertexLayout *const> layouts) override {
    ++layoutPreflightCalls;
    layoutCount = layouts.size();
    return preflightReady;
  }

  bool preflightSamplers(std::span<const SkinFilterMode> samplers) override {
    ++samplerPreflightCalls;
    samplerCount = samplers.size();
    return preflightReady;
  }

  bool reserve(std::size_t vertexCount, std::size_t indexCount,
               std::size_t, const GameplayBgaTransientRequirements &) override {
    ++reserveCalls;
    reservedVertices = vertexCount;
    reservedIndices = indexCount;
    return preflightReady;
  }

  void submit(const rendering::SkinQuadBackendBatch &) override {
    ++submitCalls;
    if (failNextAllocationAfterSubmit) {
      session_test_allocation_fault::failNext = true;
    }
  }

  bool preflightReady = true;
  bool failNextAllocationAfterSubmit = false;
  std::size_t layoutPreflightCalls = 0;
  std::size_t samplerPreflightCalls = 0;
  std::size_t reserveCalls = 0;
  std::size_t submitCalls = 0;
  std::size_t layoutCount = 0;
  std::size_t samplerCount = 0;
  std::size_t reservedVertices = 0;
  std::size_t reservedIndices = 0;
};

bool sameBgaFrame(const PreparedGameplayBgaFrame &left,
                  const PreparedGameplayBgaFrame &right) {
  const auto sameSurface = [](const auto &first, const auto &second) {
    if (first.has_value() != second.has_value()) {
      return false;
    }
    return !first ||
           (first->role == second->role &&
            first->mediaKind == second->mediaKind &&
            first->surfaceToken == second->surfaceToken &&
            first->sourceWidth == second->sourceWidth &&
            first->sourceHeight == second->sourceHeight);
  };
  return left.sequence == right.sequence &&
         left.composition == right.composition &&
         sameSurface(left.base, right.base) &&
         sameSurface(left.layer, right.layer) &&
         sameSurface(left.miss, right.miss);
}

class SessionBgaSubmitter final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t, std::int64_t,
      const GameplayBgaMissState &) override {
    return {};
  }

  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override {
    ++preflightCalls;
    preflightFrame = frame;
    targetCount = targets.size();
    return {.ready = preflightReady,
            .failure = preflightReady
                           ? std::nullopt
                           : std::optional<SkinDiagnostic>{SkinDiagnostic{
                                 .code = "session.bga.preflight",
                                 .message = "forced preflight failure"}},
            .requirements = preflightReady
                                ? GameplayBgaTransientRequirements{
                                      .vertexBytes = 64,
                                      .vertexAlignmentPadding = 8,
                                      .indexCount = 6}
                                : GameplayBgaTransientRequirements{}};
  }

  void commitPrepared(const PreparedGameplayBgaFrame &frame) noexcept override {
    ++commitCalls;
    committedFrame = frame;
  }

  void submitPrepared(const PreparedGameplayBgaFrame &frame,
                      const BgaDrawTarget &) noexcept override {
    ++submitCalls;
    submittedFrame = frame;
  }

  void finalizePrepared(
      const PreparedGameplayBgaFrame &frame) noexcept override {
    ++finalizeCalls;
    finalizedFrame = frame;
  }

  void submitFullscreen(
      const PreparedGameplayBgaFrame &) noexcept override {
    ++fullscreenCalls;
  }

  bool preflightReady = true;
  std::size_t preflightCalls = 0;
  std::size_t commitCalls = 0;
  std::size_t submitCalls = 0;
  std::size_t finalizeCalls = 0;
  std::size_t fullscreenCalls = 0;
  std::size_t targetCount = 0;
  PreparedGameplayBgaFrame preflightFrame;
  PreparedGameplayBgaFrame committedFrame;
  PreparedGameplayBgaFrame submittedFrame;
  PreparedGameplayBgaFrame finalizedFrame;
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
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override {
    return {};
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override {
    return {};
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override {
    return {};
  }
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

PreparedGameplayBgaFrame bgaFrame(std::uint64_t sequence) {
  return {.sequence = sequence,
          .composition = GameplayBgaComposition::BaseThenLayer,
          .base = PreparedGameplayBgaSurface{
              .role = GameplayBgaRole::Base,
              .mediaKind = GameplayBgaMediaKind::Image,
              .surfaceToken = 1000 + sequence,
              .sourceWidth = 640,
              .sourceHeight = 360}};
}

SkinFrameMutation persisted(PersistedSkinConfigurationWrite write) {
  return SkinFrameMutation{std::move(write)};
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
  bool requireConfiguredState = false;
  bool repeatedPomyu = false;
  bool oversizedPomyuWithSibling = false;
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
    if (options.repeatedPomyu || options.oversizedPomyuWithSibling) {
      if (options.oversizedPomyuWithSibling) {
        writeText(source / "skin/characters/alpha.chp",
                  std::string(SkinResourcePolicy::maximumEncodedBytes + 1U,
                              'x'));
        writeText(source / "skin/characters/beta.chp",
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else {
        writeText(source / "skin/characters/alpha.chp",
                  "#Anime\t100\n#Pattern\t1\t00\n");
      }
    }
    std::string script = R"lua(
local phase_count = (rawget(_G, "session_activation_phase_count") or 0) + 1
_G.session_activation_phase_count = phase_count
if skin_config then
  if phase_count ~= 2 then
    error("configured phase did not reuse exactly one fresh header state")
  end
 )lua";
    if (options.requireConfiguredState) {
      script += R"lua(
  if main_state.option(81) ~= true then
    error("configured state did not expose the initialized loaded option")
  end
)lua";
    }
    script += R"lua(
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
    } else if (options.repeatedPomyu || options.oversizedPomyuWithSibling) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {{id = "shared-chara", path = "characters/alpha.chp"}},
    pmchara = {
      {id = "pomyu-one", src = "shared-chara", type = 0, side = 1},
      {id = "pomyu-two", src = "shared-chara", type = 0, side = 1}
    },
    destination = {
      {id = "pomyu-one", dst = {{x = 0, y = 0, w = 64, h = 64}}},
      {id = "pomyu-two", dst = {{x = 64, y = 0, w = 64, h = 64}}}
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

    // Runtime execution follows the installed, Files-visible package exactly
    // as Beatoraja follows its selected skin directory.  Keep the immutable
    // revision for activation identity, but make this fixture exercise the
    // writable visible copy used by a real installation.
    fs::create_directories(roots_.visiblePackages);
    fs::copy(source, roots_.visiblePackages / package_.directoryName,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);

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
    initialState_ = stateAt(1);
    initialState_.authority.loadingState = PlayfieldLoadingState::Loaded;
    initialProjection_ = projectionAt(initialState_.clock.serial);
    return {.sessionSerial = 73,
            .profileId = profile_,
            .chartModel = chart_,
            .initialState = &initialState_,
            .initialProjection = &initialProjection_,
            .viewport = viewport,
            .safeUiBounds = {.x = 0.0,
                             .y = 0.0,
                             .width = 1280.0,
                             .height = 720.0},
            .storageRoots = roots_,
            .resourcePreparation = resources_,
            .textureDevice = device_,
            .liveResourceCounters = liveResourceCounters_,
            .configurationWrites = configurationWrites_,
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
  const std::shared_ptr<SkinLiveResourceCounters> &liveCounters() const
      noexcept {
    return liveResourceCounters_;
  }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  PlayfieldVisualState initialState_;
  PlayfieldProjectionResult initialProjection_;
  EntryProfileSettings desired_;
  SkinResourcePreparationService resources_;
  std::shared_ptr<SessionTextureDevice> device_;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters_ =
      std::make_shared<SkinLiveResourceCounters>();
  SkinConfigurationWriteQueue configurationWrites_;
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
  expect(fixture.liveCounters()->snapshot() ==
             SkinLiveResourceSnapshot{.liveTextures = 0,
                                      .liveResources = 1},
         "a successful resource-free session owns one published catalog graph");
  created.session.reset();
  expect(!weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
         "final session teardown releases every revision pin and counter");
}

void testConfiguredLoadUsesTheInitializedAuthoritativeState() {
  ActivationFixture fixture({.requireConfiguredState = true});
  if (!fixture.ready()) {
    return;
  }
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr && created.diagnostics.empty(),
         "configured Lua load receives the initialized authoritative state");
}

void testRepeatedPomyuObjectsShareCyclePreparation() {
  ActivationFixture fixture({.repeatedPomyu = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr &&
             pomyuCycleFileReadsForTesting() == 1 &&
             pomyuCycleParsesForTesting() == 1,
         "repeated Pomyu objects share one bounded CHP read and parse");
}

void testExplicitOversizedPomyuDoesNotFallBackToSibling() {
  ActivationFixture fixture({.oversizedPomyuWithSibling = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  (void)PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(pomyuCycleFileReadsForTesting() == 1 &&
             pomyuCycleParsesForTesting() == 0,
         "an oversized explicit CHP does not silently borrow a sibling's "
         "Pomyu timing metadata");
}

void testRequestedExternalGameplaySkinCreatesARealSession() {
  const char *configuredRoot =
      std::getenv("ASOBMASHOW_EXTERNAL_GAMEPLAY_SKIN_ROOT");
  if (configuredRoot == nullptr || *configuredRoot == '\0') {
    return;
  }
  const fs::path source(configuredRoot);
  expect(fs::is_directory(source),
         "requested external gameplay skin root is a readable directory");
  if (!fs::is_directory(source)) {
    return;
  }
  const char *configuredEntry =
      std::getenv("ASOBMASHOW_EXTERNAL_GAMEPLAY_SKIN_ENTRY");
  const std::string entryPath =
      configuredEntry != nullptr && *configuredEntry != '\0'
          ? configuredEntry
          : "play7.luaskin";

  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = normalizePackageId("ExternalGameplaySkin").package;
  const auto entry = package ? normalizeEntryPath(*package, entryPath).entry
                             : std::nullopt;
  const auto profile =
      makeSkinProfileId("77777777-7777-4777-8777-777777777777");
  expect(package && entry && profile,
         "requested external gameplay source has portable activation IDs");
  if (!package || !entry || !profile) {
    return;
  }

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, *package, {}, {});
  expect(snapshot.prepared.has_value(),
         "requested external gameplay package snapshots for a real session");
  if (!snapshot.prepared) {
    return;
  }
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease.has_value() && publishError.empty(),
         "requested external gameplay revision publishes for a real session");
  if (!lease) {
    return;
  }

  SkinResourcePreparationService resources;
  GameplaySkinValidator validator(resources);
  const auto validation = validator.validate(lease->readView(), *entry, nullptr, {});
  expect(validation.disposition == SkinValidationDisposition::Selectable7Key &&
             validation.reconciledSettings.has_value() &&
             !validation.configurationDigest.empty(),
         "requested external gameplay skin is selectable before session creation");
  if (validation.disposition != SkinValidationDisposition::Selectable7Key ||
      !validation.reconciledSettings || validation.configurationDigest.empty()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.keyCount = 7;
  chart.text = {.title = "external title",
                .artist = "external artist",
                .fullArtist = "external artist"};
  PlayfieldVisualState initialState = stateAt(1);
  initialState.authority.loadingState = PlayfieldLoadingState::Loaded;
  const PlayfieldProjectionResult initialProjection = projectionAt(1);
  SkinConfigurationWriteQueue configurationWrites;
  auto device = std::make_shared<SessionTextureDevice>();
  auto counters = std::make_shared<SkinLiveResourceCounters>();
  auto created = PlaySkinSession::create(
      {.revision = std::move(*lease),
       .entry = *entry,
       .reconciledSettings = *validation.reconciledSettings,
       .configurationDigest = validation.configurationDigest},
      {.sessionSerial = 92,
       .profileId = *profile,
       .chartModel = chart,
       .initialState = &initialState,
       .initialProjection = &initialProjection,
       .safeUiBounds = {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
       .storageRoots = roots,
       .resourcePreparation = resources,
       .textureDevice = std::move(device),
       .liveResourceCounters = std::move(counters),
       .configurationWrites = configurationWrites});
  if (!created.session) {
    for (const auto &diagnostic : created.diagnostics) {
      std::cerr << "external gameplay session diagnostic: " << diagnostic.code
                << ": " << diagnostic.message << " • "
                << diagnostic.virtualPath << '\n';
    }
  }
  const bool hasError = std::ranges::any_of(
      created.diagnostics, [](const SkinDiagnostic &diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
      });
  expect(created.session != nullptr && !created.cancelled && !hasError,
         "requested external gameplay skin creates a full configured session; "
         "unsupported optional visuals may remain visible as warnings");
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
  expect(fixture.liveCounters()->snapshot() ==
             SkinLiveResourceSnapshot{.liveTextures = 2,
                                      .liveResources = 1},
         "session ownership exposes only its two unique physical textures");
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
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
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
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
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
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
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
  {
    ActivationFixture fixture({.resourceBearing = true});
    if (fixture.ready()) {
      auto context = fixture.context();
      context.liveResourceCounters.reset();
      auto created = PlaySkinSession::create(fixture.takeActivation(),
                                              std::move(context));
      expect(!created.session && !created.cancelled &&
                 fixture.device()->createCalls == 0 &&
                 hasDiagnostic(created.diagnostics,
                               "skin.session.live_resource_counters_missing"),
             "a missing app-owned live-resource counter fails closed before "
             "uploads");
    }
  }
}

const SessionPresentationWrite *presentationMutation(
    const SkinFrameMutation &mutation) {
  return std::get_if<SessionPresentationWrite>(&mutation);
}

class SessionFixture final {
public:
  explicit SessionFixture(
      std::uint64_t sessionSerial = 37,
      UiLogicalRect safeUiBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0})
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("SessionContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry),
        profile_(*makeSkinProfileId(
            "55555555-5555-4555-8555-555555555555")),
        viewport_(evaluatePlaySkinViewport(
            {.width = 1280.0, .height = 720.0},
            safeUiBounds, {})),
        quadRenderer_(quadBackend_) {
    chart_.keyCount = 7;
    chart_.laneOrder = {7, 0};
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
  writer_once = function(value)
    _G.session_writer_once_count =
      (rawget(_G, "session_writer_once_count") or 0) + 1
    if _G.session_writer_once_count ~= 1 then
      error("writer queued more than once for one Down")
    end
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  writer_once_verify = function()
    _G.session_writer_verify_count =
      (rawget(_G, "session_writer_verify_count") or 0) + 1
    if _G.session_writer_verify_count >= 2 and
       (rawget(_G, "session_writer_once_count") or 0) ~= 1 then
      error("one Down did not queue exactly one writer")
    end
    return 0.5
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
         .profileId = profile_});
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
    writerOnce_ = header.value->callbackNamed("writer_once");
    writerOnceVerify_ = header.value->callbackNamed("writer_once_verify");
    expect(writerA_ && writerB_ && writerFail_ && writerOnce_ &&
               writerOnceVerify_,
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
        {.id = SkinFloatWriterId{4}, .source = *writerOnce_},
    };
    model_.model.header.width = 1280;
    model_.model.header.height = 720;
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
        .model = &model_,
        .configuration = configuration_,
        .runtime = *runtime_,
        .mutationTable = mutations_});
    session_ = std::make_unique<PlaySkinSession>(PlaySkinSessionFrameContext{
        .sessionSerial = sessionSerial,
        .identity = {.sessionSerial = sessionSerial,
                     .profileId = profile_,
                     .entry = entry_,
                     .revisionDigest = "session-revision",
                     .configurationDigest = "session-configuration"},
        .chartModel = chart_,
        .model = model_,
        .configuration = configuration_,
        .resources = resources_,
        .viewportSettings = {},
        .viewport = viewport_,
        .runtime = *runtime_,
        .bridge = *bridge_,
        .renderer = renderer_,
        .quadRenderer = quadRenderer_,
        .configurationWrites = configurationWrites_,
        .applyAudioVolume = [this](SkinAudioVolumeWriterTarget target,
                                   float value) {
          audioVolumeWrites_.emplace_back(target, value);
        },
        .applyPracticeItemScroll = [this](float position) {
          practiceItemScrollWrites_.push_back(position);
        },
        .applyPracticeMenuItem = [this](std::size_t index, bool increment) {
          practiceMenuItemWrites_.emplace_back(index, increment);
        }});
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
  const SessionResources &resources() const { return resources_; }
  const PlaySkinViewport &viewport() const { return viewport_; }
  SessionQuadBackend &quadBackend() { return quadBackend_; }
  SkinConfigurationWriteQueue &configurationWrites() {
    return configurationWrites_;
  }
  const std::vector<std::pair<SkinAudioVolumeWriterTarget, float>> &
  audioVolumeWrites() const {
    return audioVolumeWrites_;
  }
  const std::vector<float> &practiceItemScrollWrites() const {
    return practiceItemScrollWrites_;
  }
  const std::vector<std::pair<std::size_t, bool>> &practiceMenuItemWrites()
      const {
    return practiceMenuItemWrites_;
  }

  void addBgaMarker(std::uint32_t ordinal = 90) {
    model_.model.objects.push_back(
        {.id = 90,
         .authoredName = "session-bga",
         .payload = SkinBgaObject{},
         .authoredOrdinal = ordinal,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 90,
         .presentation =
             {.loop = 0,
              .frames = {{.timeMillis = 0,
                          .x = 100.0,
                          .y = 100.0,
                          .width = 640.0,
                          .height = 360.0}},
              .authoredOrdinal = ordinal}});
  }

  void addTouchGeometry(
      SkinFloatWriterId writer = SkinFloatWriterId{1},
      std::optional<double> firstLaneSecondaryDestinationY = std::nullopt) {
    resources_.addImage(80);
    const SkinFloatPropertyId valueProperty{
        writer == SkinFloatWriterId{4} ? 2U : 1U};
    model_.model.floatProperties.push_back(
        {.id = valueProperty,
         .domain = SkinFloatPropertyDomain::Rate,
         .source = writer == SkinFloatWriterId{4}
                       ? std::variant<SkinBuiltinPropertySelector,
                                      LuaCallbackId>{*writerOnceVerify_}
                       : std::variant<SkinBuiltinPropertySelector,
                                      LuaCallbackId>{
                             SkinBuiltinPropertySelector{.value = 4}},
         .authoredOrdinal = 1});
    SkinSliderObject slider;
    slider.knob = {.resource = 80,
                   .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}};
    slider.value = valueProperty;
    slider.writer = writer;
    slider.direction = 1;
    slider.range = 100.0;
    slider.changeable = true;
    SkinNoteObject notes;
    notes.lanes = {
        {.authoredLane = 7,
         .laneDestination = {.x = 100.0,
                             .y = 20.0,
                             .width = 80.0,
                             .height = 500.0},
         .secondaryDestinationY = firstLaneSecondaryDestinationY},
        {.authoredLane = 0,
         .laneDestination = {.x = 200.0,
                             .y = 20.0,
                             .width = 100.0,
                             .height = 500.0}},
    };
    model_.model.objects.push_back(
        {.id = 80,
         .authoredName = "session-slider",
         .payload = std::move(slider),
         .authoredOrdinal = 80,
         .critical = true});
    model_.model.objects.push_back(
        {.id = 81,
         .authoredName = "session-notes",
         .payload = std::move(notes),
         .authoredOrdinal = 81,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 80,
         .presentation =
             {.loop = 0,
              .frames = {{.timeMillis = 0,
                          .x = 100.0,
                          .y = 100.0,
                          .width = 20.0,
                          .height = 20.0}},
              .authoredOrdinal = 800}});
  }

  void addReplayGhostNoteGeometry() {
    const auto addVisuals = [](SkinLaneNotePresentation &lane) {
      constexpr std::array kinds{
          SkinNoteVisualKind::Normal,
          SkinNoteVisualKind::LnEnd,
          SkinNoteVisualKind::LnStart,
          SkinNoteVisualKind::LnBodyActive,
          SkinNoteVisualKind::LnBodyInactive,
          SkinNoteVisualKind::HcnEnd,
          SkinNoteVisualKind::HcnStart,
          SkinNoteVisualKind::HcnBodyActive,
          SkinNoteVisualKind::HcnBodyInactive,
          SkinNoteVisualKind::HcnDamage,
          SkinNoteVisualKind::HcnReactive,
          SkinNoteVisualKind::Mine,
          SkinNoteVisualKind::Hidden,
          SkinNoteVisualKind::Processed,
      };
      for (const auto kind : kinds) {
        lane.visuals.emplace(kind, SkinSynthesizedNoteVisual{.kind = kind});
      }
    };
    SkinNoteObject notes;
    notes.lanes = {
        {.authoredLane = 7,
         .laneDestination = {.x = 10.0,
                             .y = 40.0,
                             .width = 30.0,
                             .height = 200.0},
         .authoredNoteHeight = 8.0},
        {.authoredLane = 0,
         .laneDestination = {.x = 80.0,
                             .y = 160.0,
                             .width = 54.0,
                             .height = 400.0},
         .authoredNoteHeight = 17.0},
    };
    addVisuals(notes.lanes[0]);
    addVisuals(notes.lanes[1]);
    model_.model.objects.push_back(
        {.id = 83,
         .authoredName = "session-replay-ghost-notes",
         .payload = std::move(notes),
         .authoredOrdinal = 83,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 83, .presentation = {.authoredOrdinal = 830}});
  }

  void addClickableImage() {
    resources_.addImage(82);
    model_.model.events.push_back(
        {.id = SkinEventBindingId{1},
         .source = SkinBuiltinPropertySelector{.value = 900},
         .authoredOrdinal = 1});
    SkinImageObject image;
    image.orderedStates = {{.resource = 82,
                            .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}}};
    image.clickEvent = SkinEventBindingId{1};
    image.clickMode = 2;
    model_.model.objects.push_back(
        {.id = 82,
         .authoredName = "session-click-image",
         .payload = std::move(image),
         .authoredOrdinal = 82,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 82,
         .presentation = {.loop = 0,
                          .frames = {{.timeMillis = 0,
                                      .x = 100.0,
                                      .y = 100.0,
                                      .width = 40.0,
                                      .height = 20.0}},
                          .authoredOrdinal = 820}});
  }

  void destroySession() { session_.reset(); }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> writerA_;
  std::optional<LuaCallbackId> writerB_;
  std::optional<LuaCallbackId> writerFail_;
  std::optional<LuaCallbackId> writerOnce_;
  std::optional<LuaCallbackId> writerOnceVerify_;
  PlayfieldChartVisualModel chart_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  SkinEventMutationTable mutations_;
  SessionResources resources_;
  PlaySkinViewport viewport_;
  Skin2DRenderer renderer_;
  SessionQuadBackend quadBackend_;
  rendering::SkinQuadBatchRenderer quadRenderer_;
  SkinConfigurationWriteQueue configurationWrites_;
  std::vector<std::pair<SkinAudioVolumeWriterTarget, float>>
      audioVolumeWrites_;
  std::vector<float> practiceItemScrollWrites_;
  std::vector<std::pair<std::size_t, bool>> practiceMenuItemWrites_;
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

void testSyntheticReplayGhostUsesMatchingLaneGeometry() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 9,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}},
          {.lane = 1,
           .normalNote = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 500.0}},
      }};
  const std::array events{ReplayGhostEvent{.lane = 1,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 2.2,
                                            .judgement = Great}};

  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 9,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.frameSerial == 9 && overlay.commands.size() == 4,
         "enabled synthetic replay ghost emits one outline");
  if (overlay.commands.size() != 4) {
    return;
  }
  const auto *top =
      std::get_if<SkinPrimitiveCommand>(&overlay.commands.front().payload);
  expect(top != nullptr && top->vertices.size() == 4 &&
             std::abs(top->vertices[0].x - 90.0F) < 0.0001F &&
             std::abs(top->vertices[0].y - 290.0F) < 0.0001F &&
             std::abs(top->vertices[1].x - 142.0F) < 0.0001F &&
             std::abs(top->vertices[2].y - 291.92F) < 0.0001F,
         "synthetic replay ghost uses lane one width, height, and scroll");
}

void testSelectedSkinHudUsesThePublishedSkinNoteLaneSpan() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 17,
      .viewport = {.authoredToUi = {.m00 = 0.5, .tx = 7.0,
                                    .m11 = 0.25, .ty = 9.0},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}},
          {.lane = 1,
           .normalNote = {.x = 90.0, .y = 100.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 90.0, .y = 100.0, .width = 52.0, .height = 500.0}},
      }};

  const auto hud = selectedSkinHudGeometry(geometry);
  expect(hud && hud->frameSerial == 17 && hud->laneCount == 2 &&
             std::abs(hud->playArea.x - 12.0) < 0.0001 &&
             std::abs(hud->playArea.y - 34.0) < 0.0001 &&
             std::abs(hud->playArea.width - 66.0) < 0.0001 &&
             std::abs(hud->playArea.height - 125.0) < 0.0001 &&
             std::abs(hud->judgementLineY - 34.0) < 0.0001,
         "selected-skin HUD derives and projects its full lane span and "
         "judgement line from published SkinNote geometry");
}

void testPmsPoorDestinationUsesFirstSelectedSkinLane() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addTouchGeometry(SkinFloatWriterId{1}, -42.0);
  const auto geometry = fixture.session().pmsPoorDestinationGeometry();
  expect(geometry.has_value() &&
             std::abs(geometry->laneOriginY - 20.0) < 0.0001 &&
             std::abs(geometry->laneHeight - 500.0) < 0.0001 &&
             std::abs(geometry->secondaryDestinationY + 42.0) < 0.0001,
         "PMS dst2 forwards the selected SkinNote's source lane-zero geometry");
}

void testSyntheticStartLaneIndicatorsUseSelectedSkinLaneGeometry() {
  const PlaySkinViewport viewport{
      .authoredToUi = {},
      .uiToAuthored = {},
      .drawableAuthoredBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
      .safeUiBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
      .valid = true};
  const std::array geometry{
      SyntheticStartLaneIndicatorLaneGeometry{
          .lane = 0,
          .laneRegion = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0},
          .rgba = {1.0F, 1.0F, 1.0F, 1.0F}},
      SyntheticStartLaneIndicatorLaneGeometry{
          .lane = 1,
          .laneRegion = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 500.0},
          .rgba = {1.0F, 0.0F, 0.0F, 1.0F}},
  };
  const std::array requestedLanes{1, 99};
  const auto overlay = buildSyntheticStartLaneIndicatorOverlay(
      viewport, geometry,
      {.frameSerial = 12,
       .lanes = requestedLanes,
       .visibleLaneHeightRatio = 0.5});
  expect(overlay.frameSerial == 12 && overlay.commands.size() == 1,
         "start-lane overlay emits only selected skin lanes that exist");
  if (overlay.commands.size() != 1) {
    return;
  }
  const auto *triangle =
      std::get_if<SkinPrimitiveCommand>(&overlay.commands.front().payload);
  expect(triangle != nullptr &&
             triangle->kind == SkinPrimitiveKind::TriangleStrip &&
             triangle->vertices.size() == 3 &&
             std::abs(triangle->vertices[0].x - 104.04F) < 0.001F &&
             std::abs(triangle->vertices[0].y - 499.92F) < 0.001F &&
             std::abs(triangle->vertices[1].x - 116.0F) < 0.001F &&
             std::abs(triangle->vertices[1].y - 479.12F) < 0.001F &&
             triangle->vertices[0].rgba == 0xff0000ffU &&
             triangle->state.scissor.has_value(),
         "start-lane triangle uses the selected skin lane width, origin, "
         "color, and live lane-cover edge");
}

void testSyntheticReplayGhostRespectsDisabledOption() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 2.2,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = false,
                 .events = events});
  expect(overlay.commands.empty(),
         "disabled replay ghost option suppresses synthetic skin ghosts");
}

void testSyntheticReplayGhostSkipsEventsOutsideLaneClip() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 10.0,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghost skips events outside the active lane clip");
}

void testSyntheticReplayGhostUsesSharedPlayAreaClip() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          // Beatoraja's LaneRenderer uses lane zero for the common vertical
          // play-area bounds, even when the next lane has a taller region.
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 100.0}},
          {.lane = 1,
           .normalNote = {.x = 80.0, .y = 100.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 80.0, .y = 100.0, .width = 52.0, .height = 500.0}},
      }};
  const std::array events{ReplayGhostEvent{.lane = 1,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 1.0,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 0.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghosts obey the shared skin play-area clip");
}

void testSyntheticReplayGhostRespectsLaneCoverVisibleHeight() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneOriginY = 100.0,
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 200.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 0.75,
                                            .judgement = Great}};

  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 0.0,
                 .hispeed = 1.0,
                 .visibleLaneHeightRatio = 0.5,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghosts remain below the active lane-cover cutoff");
}

void testEvaluatedSkinPublishesPerLaneReplayGhostGeometry() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addReplayGhostNoteGeometry();
  const auto frame = fixture.session().prepareFrame(stateAt(1), projectionAt(1), {});
  const auto &geometry = frame.evaluation.syntheticReplayGhostGeometry;
  expect(frame.ready() && geometry && geometry->frameSerial == 1 &&
             geometry->sharedLaneOriginY == 40.0 &&
             geometry->sharedLaneHeight == 200.0 &&
             geometry->lanes.size() == 2 && geometry->lanes[0].lane == 7 &&
             geometry->lanes[0].normalNote.width == 30.0 &&
             geometry->lanes[1].lane == 0 &&
             geometry->lanes[1].normalNote.x == 80.0 &&
             geometry->lanes[1].normalNote.height == 17.0,
         "evaluated skin publishes the active note source's per-lane ghost geometry");
}

void testSubmittedSkinRendersOptionGatedSyntheticReplayGhosts() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addReplayGhostNoteGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(301), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "skin frame is submitted before its optional replay overlay");
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 100,
                                            .judgeTimeMicros = 200,
                                            .judgeScrollPosition = 1.2,
                                            .judgement = Great}};
  const auto submitsBeforeGhost = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticReplayGhosts(
      context, {.frameSerial = 1,
                .visualTimeMicros = 100,
                .currentScrollPosition = 1.0,
                .hispeed = 1.0,
                .enabled = true,
                .events = events});
  expect(fixture.quadBackend().submitCalls > submitsBeforeGhost,
         "submitted selected skin draws the enabled synthetic replay ghost");
  const auto submitsAfterEnabled = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticReplayGhosts(
      context, {.frameSerial = 1,
                .visualTimeMicros = 100,
                .currentScrollPosition = 1.0,
                .hispeed = 1.0,
                .enabled = false,
                .events = events});
  expect(fixture.quadBackend().submitCalls == submitsAfterEnabled,
         "submitted selected skin suppresses synthetic ghosts when disabled");
}

void testSubmittedSkinRendersPreparationIndicatorsFromItsLaneLayout() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(302), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "skin frame publishes its static SkinNote lane layout before the cue");
  const std::array requestedLanes{0, 7};
  const auto submitsBeforeCue = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticStartLaneIndicators(
      context, {.frameSerial = 1, .lanes = requestedLanes});
  expect(fixture.quadBackend().submitCalls > submitsBeforeCue,
         "selected skin submits preparation indicators through its own renderer");
  const auto submitsAfterCue = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticStartLaneIndicators(
      context, {.frameSerial = 2, .lanes = requestedLanes});
  expect(fixture.quadBackend().submitCalls == submitsAfterCue,
         "a preparation cue cannot use a stale selected-skin frame geometry");
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

void testProductionPrepareIsExternallySideEffectFreeAndRejectsDoublePrepare() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;

  const auto prepared =
      fixture.session().prepareFrame(stateAt(1), projectionAt(1));
  const auto doublePrepare =
      fixture.session().prepareFrame(stateAt(2), projectionAt(2));
  expect(prepared == PresentationFrameOutcome::Ready &&
             doublePrepare == PresentationFrameOutcome::CriticalFailure &&
             bga.preflightCalls == 0 && bga.submitCalls == 0 &&
             fixture.quadBackend().submitCalls == 0 &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.configurationWrites().drain().empty(),
         "prepare retains one value-owned transaction without submission, "
         "layout publication, persistence, or replacement by a second frame");

  RenderContext context;
  const auto exactBga = bgaFrame(1);
  const auto rendered = fixture.session().render(context, exactBga, bga);
  expect(rendered.frameSerial == 1 &&
             rendered.outcome == PresentationFrameOutcome::Ready &&
             rendered.submittedMode == PresentationMode::Skin &&
             rendered.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             rendered.preparedBga &&
             sameBgaFrame(*rendered.preparedBga, exactBga),
         "the first pending frame remains renderable after double-prepare "
         "rejection");
}

void testSuccessfulRenderConsumesOnceSubmitsExactBgaAndPublishesLayout() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  const auto exactBga = bgaFrame(44);

  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "successful frame prepares");
  RenderContext context;
  const auto result = fixture.session().render(context, exactBga, bga);
  const auto layout = fixture.session().touchLayout();
  expect(result.frameSerial == 1 &&
             result.outcome == PresentationFrameOutcome::Ready &&
             result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             result.preparedBga &&
             sameBgaFrame(*result.preparedBga, exactBga) &&
             bga.preflightCalls == 1 && bga.commitCalls == 1 &&
             bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
             sameBgaFrame(bga.preflightFrame, exactBga) &&
             sameBgaFrame(bga.committedFrame, exactBga) &&
             sameBgaFrame(bga.submittedFrame, exactBga) &&
             sameBgaFrame(bga.finalizedFrame, exactBga) &&
             fixture.quadBackend().submitCalls == 1,
         "successful render emits one complete skin frame with the exact "
         "prepared BGA value");
  expect(layout.revision == fixture.session().touchLayoutRevision() &&
             layout.keyMode == 7 && layout.laneCount == 2 &&
             layout.laneRegions.size() == 2 &&
             layout.laneRegions[0].lane == 7 &&
             layout.laneRegions[0].scratch &&
             layout.laneRegions[1].lane == 0 &&
             !layout.laneRegions[1].scratch &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.0520833F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y -
                      0.6481481F) < 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.15625F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.1851852F) <
                 0.0001F &&
             fixture.session().touchHitRegions().size() == 1,
         "successful submission publishes normalized authored lane and "
         "control geometry");

  const auto repeated = fixture.session().render(context, exactBga, bga);
  expect(repeated.outcome == PresentationFrameOutcome::CriticalFailure &&
             bga.preflightCalls == 1 && bga.commitCalls == 1 &&
             bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
             fixture.quadBackend().submitCalls == 1 &&
             fixture.configurationWrites().drain().empty(),
         "repeat render cannot resubmit the consumed frame or enqueue writes");
}

void testSkinLaneTouchLayoutUsesDrawableScreenCoordinates() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();

  const auto savedWindowWidth = rendering::window_width;
  const auto savedWindowHeight = rendering::window_height;
  const auto savedRenderWidth = rendering::render_width;
  const auto savedRenderHeight = rendering::render_height;
  const auto savedScaleX = rendering::ui_scale_x;
  const auto savedScaleY = rendering::ui_scale_y;
  const auto savedOffsetX = rendering::ui_offset_x;
  const auto savedOffsetY = rendering::ui_offset_y;
  rendering::window_width = 1920;
  rendering::window_height = 1080;
  rendering::render_width = 2400;
  rendering::render_height = 1400;
  rendering::ui_scale_x = 1.1F;
  rendering::ui_scale_y = 1.2F;
  rendering::ui_offset_x = 240;
  rendering::ui_offset_y = 70;

  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(71), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "drawable-coordinate fixture publishes skin lane geometry");
  const auto layout = fixture.session().touchLayout();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x -
                      (350.0F / 2400.0F)) < 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y -
                      (910.0F / 1400.0F)) < 0.0001F,
         "skin lane routing uses the same drawable scale and offset as raw "
         "iOS touch input");

  rendering::window_width = savedWindowWidth;
  rendering::window_height = savedWindowHeight;
  rendering::render_width = savedRenderWidth;
  rendering::render_height = savedRenderHeight;
  rendering::ui_scale_x = savedScaleX;
  rendering::ui_scale_y = savedScaleY;
  rendering::ui_offset_x = savedOffsetX;
  rendering::ui_offset_y = savedOffsetY;
}

void testCriticalEvaluationAndPreflightFailuresPublishNoFrameState() {
  {
    SessionFixture fixture;
    if (fixture.ready()) {
      fixture.addBgaMarker();
      fixture.addTouchGeometry();
      SessionBgaSubmitter bga;
      RenderContext context;
      expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
                 fixture.session().render(context, bgaFrame(70), bga).outcome ==
                     PresentationFrameOutcome::Ready,
             "evaluation-failure fixture first publishes valid geometry");
      const UiLogicalPoint oldPoint{.x = 150.0F, .y = 610.0F};
      const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
      expect(fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = oldPoint,
                  .eventMicros = 1,
                  .hit = oldHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
             "evaluation-failure fixture captures the published control");
      fixture.model().model.destinations.push_back(
          {.object = 999, .presentation = {.authoredOrdinal = 1}});
      const auto outcome =
          fixture.session().prepareFrame(stateAt(2), projectionAt(2));
      const auto exactBga = bgaFrame(71);
      const auto result = fixture.session().render(context, exactBga, bga);
      expect(outcome == PresentationFrameOutcome::CriticalFailure &&
                 result.outcome == PresentationFrameOutcome::CriticalFailure &&
                 result.frameSerial == 2 && result.failure &&
                 result.preparedBga &&
                 sameBgaFrame(*result.preparedBga, exactBga) &&
                 result.failure->frameSerial == 2 &&
                 result.failure->entry == fixture.session().identity().entry &&
                 result.failure->revisionDigest ==
                     fixture.session().identity().revisionDigest &&
                 result.failure->configurationDigest ==
                     fixture.session().identity().configurationDigest &&
                 result.submittedMode == PresentationMode::BuiltIn &&
                 result.bgaCompositeMode ==
                     GameplayBgaCompositeMode::FullscreenBuiltIn &&
                 bga.preflightCalls == 1 && bga.commitCalls == 1 &&
                 bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
                 bga.fullscreenCalls == 0 &&
                 fixture.session().touchLayout().laneRegions.empty() &&
                 fixture.session().touchHitRegions().empty() &&
                 fixture.session().updatePresentationTouch(
                     {.pointerId = 1,
                      .uiPoint = oldPoint,
                      .eventMicros = 2,
                      .hit = oldHit}) == PresentationTouchResult{} &&
                 fixture.configurationWrites().drain().empty(),
             "critical evaluation failure carries exact identity and frame, "
             "performs no additional BGA work, and clears prior geometry, "
             "capture, and writes");
    }
  }
  {
    SessionFixture fixture;
    if (fixture.ready()) {
      fixture.addBgaMarker();
      fixture.addTouchGeometry();
      SessionBgaSubmitter bga;
      expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready,
             "preflight fixture first evaluates");
      RenderContext context;
      expect(fixture.session().render(context, bgaFrame(70), bga).outcome ==
                 PresentationFrameOutcome::Ready,
             "preflight fixture first publishes valid geometry");
      const UiLogicalPoint oldPoint{.x = 150.0F, .y = 610.0F};
      const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
      expect(fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = oldPoint,
                  .eventMicros = 1,
                  .hit = oldHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
             "preflight fixture captures the published control");
      bga.preflightReady = false;
      expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready,
             "preflight failure frame evaluates before submission");
      const auto exactBga = bgaFrame(72);
      const auto result = fixture.session().render(context, exactBga, bga);
      expect(result.outcome == PresentationFrameOutcome::Ready &&
                 result.frameSerial == 2 && !result.failure &&
                 result.preparedBga &&
                 sameBgaFrame(*result.preparedBga, exactBga) &&
                 result.submittedMode == PresentationMode::Skin &&
                 result.bgaCompositeMode ==
                     GameplayBgaCompositeMode::EmbeddedSkin &&
                 bga.preflightCalls == 2 && bga.commitCalls == 1 &&
                 bga.submitCalls == 1 && bga.finalizeCalls == 2 &&
                 bga.fullscreenCalls == 0 &&
                 fixture.quadBackend().submitCalls == 2 &&
                 !fixture.session().touchLayout().laneRegions.empty() &&
                 fixture.configurationWrites().drain().empty(),
             "BGA preflight failure blanks only BGA while the authored skin "
             "continues to draw and releases its prepared frame");
    }
  }
}

void testForwardCompatiblePersistedMutationsEnqueueOneExactOrderedBatch() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  // Pinned v1 exposes no persisted mutation selector. These value-owned
  // variants exercise only the forward-compatible post-submit session
  // plumbing, through the same pending evaluation and renderer path.
  std::vector<SkinFrameMutation> extraMutations{
      SessionPresentationWrite{.eventId = 900,
                               .arguments = {1, 0},
                               .argumentCount = 1},
      persisted(SetSkinOption{.key = "gauge", .value = 3}),
      SessionPresentationWrite{.eventId = 901,
                               .arguments = {2, 0},
                               .argumentCount = 1},
      persisted(SetSkinFilePath{.key = "background",
                                .declaredValue = "blue.png"}),
      persisted(SetSkinOffset{.key = "lane",
                              .value = {.x = 1,
                                        .y = 2,
                                        .w = 3,
                                        .h = 4,
                                        .r = 5,
                                        .a = 6}}),
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, extraMutations) ==
             PresentationFrameOutcome::Ready,
         "test-only forward-compatible mutations enter the regular pending "
         "frame");
  std::get<SetSkinOption>(
      std::get<PersistedSkinConfigurationWrite>(extraMutations[1])).key =
      "mutated-gauge";
  std::get<SetSkinFilePath>(
      std::get<PersistedSkinConfigurationWrite>(extraMutations[3]))
      .declaredValue = "mutated.png";
  extraMutations.clear();
  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(81), bga);
  const auto requests = fixture.configurationWrites().drain();
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             requests.size() == 1 && requests[0].sessionSerial == 37 &&
             requests[0].profileId == fixture.session().identity().profileId &&
             requests[0].entry == fixture.session().identity().entry &&
             requests[0].expectedRevisionDigest == "session-revision" &&
             requests[0].expectedConfigurationDigest ==
                 "session-configuration" &&
             requests[0].frameSerial == 1 &&
             requests[0].orderedWrites.size() == 3 &&
             std::get<SetSkinOption>(requests[0].orderedWrites[0]).key ==
                 "gauge" &&
             std::get<SetSkinOption>(requests[0].orderedWrites[0]).value == 3 &&
             std::get<SetSkinFilePath>(requests[0].orderedWrites[1])
                     .declaredValue == "blue.png" &&
             std::get<SetSkinOffset>(requests[0].orderedWrites[2]).value.r == 5,
         "successful skin draw consumes session-local mutations and enqueues "
         "one exact deep-owned persisted batch in authored order");
}

void testAudioVolumeMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetSkinAudioVolume{.target = SkinAudioVolumeWriterTarget::Keysound,
                         .value = 0.35F},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.audioVolumeWrites().empty(),
         "audio volume mutation remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(82), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.audioVolumeWrites().size() == 1 &&
             fixture.audioVolumeWrites().front().first ==
                 SkinAudioVolumeWriterTarget::Keysound &&
             fixture.audioVolumeWrites().front().second == 0.35F,
         "submitted skin frame applies the staged native audio writer exactly "
         "once");
}

void testPracticeScrollMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetPracticeItemScroll{.position = 0.5F},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.practiceItemScrollWrites().empty(),
         "practice scroll remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(83), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.practiceItemScrollWrites().size() == 1 &&
             fixture.practiceItemScrollWrites().front() == 0.5F,
         "submitted skin frame applies the staged practice viewport writer "
         "exactly once");
}

void testPracticeMenuItemMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetPracticeMenuItem{.visibleIndex = 9, .increment = false},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.practiceMenuItemWrites().empty(),
         "practice item remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(84), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.practiceMenuItemWrites().size() == 1 &&
             fixture.practiceMenuItemWrites().front() ==
                 std::pair<std::size_t, bool>{9, false},
         "submitted skin frame applies the staged source practice row exactly "
         "once");
}

void testPersistenceRequestIsFullyAllocatedBeforeSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  const std::vector<SkinFrameMutation> writes{
      persisted(SetSkinFilePath{.key = "background",
                                .declaredValue = "preallocated.png"})};
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready,
         "allocation-boundary fixture prepares a persisted write");
  fixture.quadBackend().failNextAllocationAfterSubmit = true;
  SessionBgaSubmitter bga;
  RenderContext context;
  const auto exactBga = bgaFrame(85);
  PresentationFrameResult result;
  bool exceptionEscaped = false;
  try {
    result = fixture.session().render(context, exactBga, bga);
  } catch (...) {
    exceptionEscaped = true;
  }
  // A correct implementation performs no allocation after backend submit, so
  // the one-shot fault can remain armed until the observation phase.
  session_test_allocation_fault::failNext = false;
  const auto requests = fixture.configurationWrites().drain();
  expect(!exceptionEscaped && result.outcome == PresentationFrameOutcome::Ready &&
             result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             result.preparedBga &&
             sameBgaFrame(*result.preparedBga, exactBga) &&
             bga.commitCalls == 1 && bga.submitCalls == 1 &&
             bga.finalizeCalls == 1 && bga.fullscreenCalls == 0 &&
             fixture.quadBackend().submitCalls == 1 &&
             requests.size() == 1 && requests[0].frameSerial == 1 &&
             requests[0].orderedWrites.size() == 1 &&
             std::get<SetSkinFilePath>(requests[0].orderedWrites.front())
                     .declaredValue == "preallocated.png",
         "persistence ownership and diagnostics allocate before drawing, with "
         "no post-submit exception or hybrid fallback boundary");
}

void testQueueFullAndClosedAreRecoverableOnlyAfterSuccessfulSkinDraw() {
  const auto exercise = [](bool closeQueue) {
    SessionFixture fixture;
    if (!fixture.ready()) {
      return;
    }
    fixture.addBgaMarker();
    if (closeQueue) {
      fixture.configurationWrites().close();
    } else {
      for (std::size_t index = 0;
           index < SkinConfigurationWriteQueue::maxPending; ++index) {
        SkinConfigurationWriteRequest request;
        request.frameSerial = index + 1;
        expect(fixture.configurationWrites().enqueue(std::move(request)) ==
                   SkinConfigurationEnqueueResult::Enqueued,
               "queue-full fixture fills every slot");
      }
    }
    const std::vector<SkinFrameMutation> writes{
        persisted(SetSkinOption{.key = "lane-cover", .value = 1})};
    expect(fixture.session().prepareFrameForTesting(
               stateAt(1), projectionAt(1), {}, writes) ==
               PresentationFrameOutcome::Ready,
           "recoverable queue fixture prepares normally");
    SessionBgaSubmitter bga;
    RenderContext context;
    const auto exactBga = bgaFrame(closeQueue ? 83 : 82);
    const auto result = fixture.session().render(context, exactBga, bga);
    expect(result.outcome == PresentationFrameOutcome::RecoverableFailure &&
               result.frameSerial == 1 && result.failure &&
               result.failure->frameSerial == 1 &&
               result.failure->diagnostic.code ==
                   (closeQueue
                        ? "skin.session.configuration_write_queue_closed"
                        : "skin.session.configuration_write_queue_full") &&
               result.submittedMode == PresentationMode::Skin &&
               result.bgaCompositeMode ==
                   GameplayBgaCompositeMode::EmbeddedSkin &&
               result.preparedBga &&
               sameBgaFrame(*result.preparedBga, exactBga) &&
               bga.preflightCalls == 1 && bga.commitCalls == 1 &&
               bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
               bga.fullscreenCalls == 0,
           closeQueue
               ? "closed persistence queue reports recoverable failure after "
                 "the complete skin draw"
               : "full persistence queue reports recoverable failure after "
                 "the complete skin draw");
  };
  exercise(false);
  exercise(true);
}

void testTouchCaptureLifecycleFailsClosedAndQueuesOnlyDown() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{4});
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "touch fixture prepares");
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().render(context, bgaFrame(91), bga).outcome ==
             PresentationFrameOutcome::Ready,
         "touch fixture publishes geometry after draw");
  const UiLogicalPoint point{.x = 150.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  const PresentationTouchEvent down{
      .pointerId = 1, .uiPoint = point, .eventMicros = 1'000, .hit = hit};
  expect(hit.kind == PresentationUiControlKind::Slider && hit.writer &&
             fixture.session().beginPresentationTouch(down) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().beginPresentationTouch(down) ==
                 PresentationTouchResult{} &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = {.x = 180.0F, .y = 610.0F},
                  .eventMicros = 1'001,
                  .hit = hit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().endPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = point,
                  .eventMicros = 1'002,
                  .hit = hit},
                 false) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().updatePresentationTouch(down) ==
                 PresentationTouchResult{},
         "Down captures once, Move stays consumed without writing, and Up "
         "clears the capture");

  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(92), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "one valid Down queues exactly one writer for the next frame while "
         "Move and Up queue none");

  auto stale = hit;
  ++stale.layoutRevision;
  auto forged = hit;
  ++forged.sourceObject;
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 2,
              .uiPoint = point,
              .eventMicros = 2'000,
              .hit = stale}) == PresentationTouchResult{} &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 3,
                  .uiPoint = point,
                  .eventMicros = 2'001,
                  .hit = forged}) == PresentationTouchResult{} &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 4,
                  .uiPoint = {.x = std::numeric_limits<float>::quiet_NaN(),
                              .y = point.y},
                  .eventMicros = 2'002,
                  .hit = hit}) == PresentationTouchResult{},
         "stale, forged, and nonfinite Down events fail closed");

  for (long long pointer = 10; pointer < 42; ++pointer) {
    expect(fixture.session().beginPresentationTouch(
               {.pointerId = pointer,
                .uiPoint = point,
                .eventMicros = pointer,
                .hit = hit}) ==
               PresentationTouchResult{.consumed = true,
                                       .excludeFromGameplay = true},
           "each of the first 32 distinct pointers captures");
  }
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 42,
              .uiPoint = point,
              .eventMicros = 42,
              .hit = hit}) == PresentationTouchResult{},
         "the 33rd simultaneous pointer fails closed");
  fixture.session().cancelPresentationTouches(3'000);
  expect(fixture.session().updatePresentationTouch(
             {.pointerId = 10,
              .uiPoint = point,
              .eventMicros = 3'001,
              .hit = hit}) == PresentationTouchResult{},
         "cancel-all clears every capture without a writer invocation");

  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 50,
              .uiPoint = point,
              .eventMicros = 50,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().endPresentationTouch(
                 {.pointerId = 50,
                  .uiPoint = point,
                  .eventMicros = 51,
                  .hit = forged},
                 false) == PresentationTouchResult{} &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 50,
                  .uiPoint = point,
                  .eventMicros = 52,
                  .hit = hit}) == PresentationTouchResult{},
         "a mismatched End fails closed but still releases its capture");
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 51,
              .uiPoint = point,
              .eventMicros = 53,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().endPresentationTouch(
                 {.pointerId = 51,
                  .uiPoint = point,
                  .eventMicros = 54,
                  .hit = hit},
                 true) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 51,
                  .uiPoint = point,
                  .eventMicros = 55,
                  .hit = hit}) == PresentationTouchResult{},
         "a cancelled matching End consumes and releases without a writer");
}

void testImageActTouchQueuesPinnedEventOnDown() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addClickableImage();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(120), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "Image act fixture publishes its first rendered hit region");
  const UiLogicalPoint point{.x = 130.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(hit.kind == PresentationUiControlKind::Image &&
             hit.eventBinding == 1U &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = point,
                  .eventMicros = 2'000,
                  .hit = hit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
         "Image act consumes the primary pointer-down and queues its event");
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(121), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the queued Image act event reaches the next frame transaction");
}

void testViewportChangeCancelsCapturesAndInvalidatesPublishedGeometry() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{3});
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "viewport fixture prepares");
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().render(context, bgaFrame(101), bga).outcome ==
             PresentationFrameOutcome::Ready,
         "viewport fixture publishes geometry");
  const UiLogicalPoint point{.x = 150.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 1,
              .uiPoint = point,
              .eventMicros = 1,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true},
         "viewport fixture captures the failing writer before reset");
  const auto oldLayoutRevision = fixture.session().touchLayoutRevision();
  const auto oldHitRevision = fixture.session().touchHitRegionsRevision();
  const auto originalIdentity = fixture.session().identity();
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Fit,
      .scaleX = 0.75F,
      .scaleY = 0.75F,
      .translateX = 15.0F,
      .translateY = -10.0F};
  fixture.session().setViewport(customViewport);
  expect(fixture.session().touchLayoutRevision() != oldLayoutRevision &&
             fixture.session().touchHitRegionsRevision() != oldHitRevision &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = point,
                  .eventMicros = 2,
                  .hit = hit}) == PresentationTouchResult{} &&
             fixture.configurationWrites().drain().empty(),
         "setViewport cancels captures, clears geometry, advances revisions, "
         "and never persists directly");
  // The captured writer throws if invoked. Ready proves the viewport change
  // discarded its queued old-layout Down before the next transaction.
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready,
         "viewport reset discards the captured old-layout writer");

  const auto submitsBeforePendingReset = bga.submitCalls;
  fixture.session().setViewport(customViewport);
  const auto discarded =
      fixture.session().render(context, bgaFrame(102), bga);
  expect(discarded.outcome == PresentationFrameOutcome::CriticalFailure &&
             !discarded.preparedBga &&
             bga.preflightCalls == submitsBeforePendingReset &&
             bga.commitCalls == submitsBeforePendingReset &&
             bga.submitCalls == submitsBeforePendingReset &&
             bga.finalizeCalls == submitsBeforePendingReset &&
             bga.fullscreenCalls == 0 &&
             fixture.configurationWrites().drain().empty(),
         "setViewport discards an already prepared old-viewport frame before "
         "any BGA, skin, fullscreen, or persistence submission");

  expect(fixture.session().prepareFrame(stateAt(3), projectionAt(3)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(103), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the next frame republishes with the custom viewport");
  const auto customLayout = fixture.session().touchLayout();
  const auto customRegions = fixture.session().touchHitRegions();
  UiLogicalPoint customControlPoint;
  if (!customRegions.empty()) {
    customControlPoint = {
        .x = (customRegions.front().boundary[0].x +
              customRegions.front().boundary[2].x) /
             2.0F,
        .y = (customRegions.front().boundary[0].y +
              customRegions.front().boundary[2].y) /
             2.0F};
  }
  const auto customHit =
      fixture.session().hitTestUiControl(customControlPoint);
  const auto &identityAfter = fixture.session().identity();
  expect(customLayout.laneRegions.size() == 2 &&
             std::abs(customLayout.laneRegions[0].bottomLeft.x -
                      0.1302083F) < 0.0001F &&
             std::abs(customLayout.laneRegions[0].bottomLeft.y -
                      0.5601852F) < 0.0001F &&
             std::abs(customLayout.laneRegions[1].topRight.x -
                      0.2083333F) < 0.0001F &&
             std::abs(customLayout.laneRegions[1].topRight.y -
                      0.2129630F) < 0.0001F,
         "custom viewport republishes window-normalized touch coordinates");
  expect(customRegions.size() == 1 &&
             customHit.kind == PresentationUiControlKind::LaneCover &&
             customHit.writer == SkinFloatWriterId{3},
         "custom viewport republishes the transformed lane-cover hit region");
  expect(identityAfter.sessionSerial == originalIdentity.sessionSerial &&
             identityAfter.profileId == originalIdentity.profileId &&
             identityAfter.entry == originalIdentity.entry &&
             identityAfter.revisionDigest == originalIdentity.revisionDigest &&
             identityAfter.configurationDigest ==
                 originalIdentity.configurationDigest,
         "viewport changes do not alter the five-field session identity");
}

void testViewportGeometryChangeCancelsOldInputAndPreservesSessionIdentity() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{3});
  SessionBgaSubmitter bga;
  RenderContext context;
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Fit,
      .scaleX = 0.75F,
      .scaleY = 0.75F,
      .translateX = 15.0F,
      .translateY = -10.0F};
  fixture.session().setViewport(customViewport);
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(201), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "resize fixture publishes its initial touch geometry");

  const auto initialRegions = fixture.session().touchHitRegions();
  UiLogicalPoint oldPoint;
  if (!initialRegions.empty()) {
    oldPoint = {
        .x = (initialRegions.front().boundary[0].x +
              initialRegions.front().boundary[2].x) /
             2.0F,
        .y = (initialRegions.front().boundary[0].y +
              initialRegions.front().boundary[2].y) /
             2.0F};
  }
  const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 91,
              .uiPoint = oldPoint,
              .eventMicros = 1,
              .hit = oldHit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true},
         "resize fixture owns one authored capture and queued Down writer");

  const auto identityBefore = fixture.session().identity();
  const auto layoutRevisionBefore = fixture.session().touchLayoutRevision();
  const auto hitRevisionBefore =
      fixture.session().touchHitRegionsRevision();
  fixture.session().updateViewportGeometry(
      {.x = 100.0, .y = 50.0, .width = 1280.0, .height = 720.0});

  expect(fixture.session().touchLayoutRevision() != layoutRevisionBefore &&
             fixture.session().touchHitRegionsRevision() !=
                 hitRevisionBefore &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 91,
                  .uiPoint = oldPoint,
                  .eventMicros = 2,
                  .hit = oldHit}) == PresentationTouchResult{} &&
             fixture.configurationWrites().drain().empty(),
         "safe-area replacement cancels old captures, queued writers, and "
         "published geometry");

  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(202), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the next frame republishes against the replacement safe area");
  const auto layout = fixture.session().touchLayout();
  const auto identityAfter = fixture.session().identity();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.1822917F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y - 0.6064815F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.2604167F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.2592593F) <
                 0.0001F,
         "rotation republishes lane routing with the unchanged Custom "
         "viewport settings in the new UI-logical geometry");
  expect(identityAfter.sessionSerial == identityBefore.sessionSerial &&
             identityAfter.profileId == identityBefore.profileId &&
             identityAfter.entry == identityBefore.entry &&
             identityAfter.revisionDigest == identityBefore.revisionDigest &&
             identityAfter.configurationDigest ==
                 identityBefore.configurationDigest,
         "geometry-only replacement preserves immutable activation identity");
}

void testTouchLayoutNormalizesAgainstTheWholeWindowWithSafeOrigin() {
  SessionFixture fixture(
      37, {.x = 100.0, .y = 50.0, .width = 1280.0, .height = 720.0});
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(104), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "safe-origin fixture publishes geometry");
  const auto layout = fixture.session().touchLayout();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.1041667F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y - 0.6944444F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.2083333F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.2314815F) <
                 0.0001F,
         "nonzero safe origin is retained in UI coordinates before whole-"
         "window normalization");
}

void testSuccessfulGeometryChangesOnlyHitRevisionAndTeardownDiscardsState() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(111), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "first geometry frame succeeds");
  const auto layoutRevision = fixture.session().touchLayoutRevision();
  const auto hitRevision = fixture.session().touchHitRegionsRevision();
  fixture.model().model.destinations.back().presentation.frames.front().x =
      300.0;
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(112), bga).outcome ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().touchLayoutRevision() == layoutRevision &&
             fixture.session().touchHitRegionsRevision() != hitRevision,
         "successful animated control geometry advances hit publication while "
         "lane topology remains stable");
  const UiLogicalPoint point{.x = 350.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 9,
              .uiPoint = point,
              .eventMicros = 9,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().prepareFrame(stateAt(3), projectionAt(3)) ==
                 PresentationFrameOutcome::Ready,
         "teardown fixture owns both a capture and pending frame");
  const auto submitsBeforeTeardown = bga.submitCalls;
  fixture.destroySession();
  expect(bga.submitCalls == submitsBeforeTeardown &&
             fixture.configurationWrites().drain().empty(),
         "session destruction discards pending frame, captures, and writes "
         "without submission or enqueue");
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
  auto unrestrictedInputs = inputs;
  unrestrictedInputs.safetyPolicy = SkinSafetyPolicy(
      SkinSafetyLevel::Unrestricted);
  expect(skinFrameMaximumCommands(inputs) ==
             SkinCommandPolicy::maximumCommands &&
             skinFrameMaximumGlyphInstances(inputs) ==
                 SkinCommandPolicy::maximumGlyphInstances &&
             skinFrameMaximumCommands(unrestrictedInputs) ==
                 std::numeric_limits<std::size_t>::max() &&
             skinFrameMaximumGlyphInstances(unrestrictedInputs) ==
                 std::numeric_limits<std::size_t>::max(),
         "Unrestricted frame inputs lift the renderer command and glyph limits");
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
  testConfiguredLoadUsesTheInitializedAuthoritativeState();
  testRepeatedPomyuObjectsShareCyclePreparation();
  testExplicitOversizedPomyuDoesNotFallBackToSibling();
  testRequestedExternalGameplaySkinCreatesARealSession();
  testActivationRejectsAReconciledDigestMismatch();
  testResourceSessionOwnsUploadsAndExactRuntimeStringAtlas();
  testPostUploadCancellationRollsBackResourcesOnOwnerThread();
  testInvalidViewportRollsBackUploadedResourcesOnOwnerThread();
  testActivationCancellationAndZeroSerialDoNotPublishSessions();
  testSuccessfulFrameCommitsWriterMutationsInInputOrder();
  testWriterFailureDiscardsEarlierAndFailedCallbackMutations();
  testEvaluatorFailureDiscardsWriterTransaction();
  testSerialMismatchDoesNotConsumeRuntimeFrame();
  testSyntheticReplayGhostUsesMatchingLaneGeometry();
  testSelectedSkinHudUsesThePublishedSkinNoteLaneSpan();
  testPmsPoorDestinationUsesFirstSelectedSkinLane();
  testSyntheticStartLaneIndicatorsUseSelectedSkinLaneGeometry();
  testSyntheticReplayGhostRespectsDisabledOption();
  testSyntheticReplayGhostSkipsEventsOutsideLaneClip();
  testSyntheticReplayGhostUsesSharedPlayAreaClip();
  testSyntheticReplayGhostRespectsLaneCoverVisibleHeight();
  testEvaluatedSkinPublishesPerLaneReplayGhostGeometry();
  testSubmittedSkinRendersOptionGatedSyntheticReplayGhosts();
  testSubmittedSkinRendersPreparationIndicatorsFromItsLaneLayout();
  testInvalidSessionSerialDoesNotConsumeFrameOwners();
  testPassiveCustomTimerUsesTheSharedSessionFrame();
  testProductionPrepareIsExternallySideEffectFreeAndRejectsDoublePrepare();
  testSuccessfulRenderConsumesOnceSubmitsExactBgaAndPublishesLayout();
  testSkinLaneTouchLayoutUsesDrawableScreenCoordinates();
  testCriticalEvaluationAndPreflightFailuresPublishNoFrameState();
  testForwardCompatiblePersistedMutationsEnqueueOneExactOrderedBatch();
  testAudioVolumeMutationAppliesOnlyAfterSkinSubmission();
  testPracticeScrollMutationAppliesOnlyAfterSkinSubmission();
  testPracticeMenuItemMutationAppliesOnlyAfterSkinSubmission();
  testPersistenceRequestIsFullyAllocatedBeforeSkinSubmission();
  testQueueFullAndClosedAreRecoverableOnlyAfterSuccessfulSkinDraw();
  testTouchCaptureLifecycleFailsClosedAndQueuesOnlyDown();
  testImageActTouchQueuesPinnedEventOnDown();
  testViewportChangeCancelsCapturesAndInvalidatesPublishedGeometry();
  testViewportGeometryChangeCancelsOldInputAndPreservesSessionIdentity();
  testTouchLayoutNormalizesAgainstTheWholeWindowWithSafeOrigin();
  testSuccessfulGeometryChangesOnlyHitRevisionAndTeardownDiscardsState();
  testLegacyRendererAdapterBeginsInternallyAndRejectsDoubleBegin();
  if (failures != 0) {
    std::cerr << failures << " play skin session test(s) failed\n";
    return 1;
  }
  std::cout << "play skin session tests passed\n";
  return 0;
}
