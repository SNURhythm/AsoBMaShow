#include "skin/beatoraja/Skin2DRenderer.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
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
              ("asobmashow-skin-command-" + std::to_string(++serial));
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

class RuntimeHarness final {
public:
  RuntimeHarness()
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("CommandContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry) {
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/main.luaskin",
              "local fail=function() error('forced failure') end\n"
              "return {type=0,w=1280,h=720,name='command-test',"
              "host_fail_callback=fail}");
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "runtime fixture snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = prepared_->readView(),
         .entry = entry_,
         .storageRoots = roots_,
         .profileId =
             *makeSkinProfileId("33333333-3333-4333-8333-333333333333")});
    expect(fileSystem.fileSystem != nullptr, "runtime filesystem creates");
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    runtime_ = std::move(created.runtime);
    expect(runtime_ != nullptr, "gameplay runtime creates");
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "runtime header executes");
    if (header.value) {
      failCallback_ = header.value->callbackNamed("host_fail_callback");
      expect(failCallback_.has_value(), "runtime failure callback is retained");
    }
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "runtime configured phase executes");
    configured.value.reset();
    expect(runtime_->enterRenderPhase().ok, "runtime enters render phase");
  }

  LuaSkinRuntime &runtime() { return *runtime_; }
  LuaCallbackId failCallback() const { return *failCallback_; }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> failCallback_;
};

class FakeResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id, SkinSourceRect authored,
                int width = 100, int height = 100) {
    PreparedSkinResource resource;
    resource.id = id;
    resource.width = width;
    resource.height = height;
    resource.regions = {authored};
    resource.regionMappings = {{.authored = authored, .resolved = authored}};
    images_.emplace(id, std::move(resource));
  }

  const PreparedSkinResource *find(SkinResourceId id) const noexcept override {
    const auto found = images_.find(id);
    return found == images_.end() ? nullptr : &found->second;
  }

  const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId id,
                     const SkinSourceRect &authored) const noexcept override {
    const auto *resource = find(id);
    if (!resource) {
      return nullptr;
    }
    const auto found = std::ranges::find_if(
        resource->regionMappings, [&](const SkinResolvedRegion &mapping) {
          return mapping.authored.x == authored.x &&
                 mapping.authored.y == authored.y &&
                 mapping.authored.w == authored.w &&
                 mapping.authored.h == authored.h &&
                 mapping.authored.gridColumn == authored.gridColumn &&
                 mapping.authored.gridRow == authored.gridRow &&
                 mapping.authored.gridColumns == authored.gridColumns &&
                 mapping.authored.gridRows == authored.gridRows;
        });
    return found == resource->regionMappings.end() ? nullptr : &*found;
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
  std::map<SkinResourceId, PreparedSkinResource> images_;
};

class FakeState final : public ISkinFrameState {
public:
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override {
    return booleanResult;
  }
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &) override {
    return integerResult;
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<ConfigOffset> offsetProperty(int id) override {
    const auto found = offsets.find(id);
    return found == offsets.end() ? SkinPropertyLookup<ConfigOffset>{}
                                  : found->second;
  }
  std::int64_t
  timerProperty(const SkinBuiltinPropertySelector &) override {
    return timerResult;
  }
  std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override {
    return notes;
  }
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override {
    return longNotes;
  }
  std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override {
    return lines;
  }
  SkinGaugeStateView gaugeState() const noexcept override { return {}; }
  SkinJudgeStateView judgeState(int) const noexcept override { return {}; }
  std::uint64_t frameSerial() const noexcept override { return capturedSerial; }

  std::vector<SkinProjectedNoteView> notes;
  std::vector<SkinProjectedLongNoteView> longNotes;
  std::vector<SkinProjectedLineView> lines;
  SkinPropertyLookup<bool> booleanResult;
  SkinPropertyLookup<std::int64_t> integerResult;
  std::int64_t timerResult = INT64_MIN;
  std::map<int, SkinPropertyLookup<ConfigOffset>> offsets;
  std::uint64_t capturedSerial = 1;
};

SkinObjectDefinition imageObject(SkinObjectId id, SkinResourceId resource,
                                 bool critical) {
  SkinSpriteFrames sprite;
  sprite.resource = resource;
  sprite.frames = {{.x = 0, .y = 0, .w = 10, .h = 10}};
  return {.id = id,
          .authoredName = "image-" + std::to_string(id),
          .payload = SkinImageObject{.orderedStates = {std::move(sprite)}},
          .authoredOrdinal = id,
          .critical = critical};
}

SkinDestination destination(SkinObjectId object, std::uint32_t ordinal,
                            double x) {
  SkinDestinationBody body;
  body.loop = -1;
  body.authoredOrdinal = ordinal;
  body.frames = {{.timeMillis = 0,
                  .x = x,
                  .y = 20.0,
                  .width = 40.0,
                  .height = 30.0}};
  return {.object = object, .presentation = std::move(body)};
}

PlaySkinViewport viewport() {
  return evaluatePlaySkinViewport(
      {.width = 1280.0, .height = 720.0},
      {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0}, {});
}

SkinFrameEvaluationResult evaluate(Skin2DRenderer &renderer,
                                   RuntimeHarness &runtime,
                                   ValidatedBeatorajaSkinModel &model,
                                   const FakeResources &resources,
                                   FakeState &state,
                                   std::uint64_t serial = 1,
                                   std::int64_t visualTimeMicros = 0,
                                   const BeatorajaSkinConfiguration *configured =
                                       nullptr,
                                   std::optional<std::uint64_t> capturedSerial =
                                       std::nullopt) {
  static const BeatorajaSkinConfiguration emptyConfiguration;
  const auto &configuration =
      configured ? *configured : emptyConfiguration;
  const auto playViewport = viewport();
  state.capturedSerial = capturedSerial.value_or(serial);
  return renderer.evaluateFrame({.frameSerial = serial,
                                 .visualTimeMicros = visualTimeMicros,
                                 .model = model,
                                 .configuration = configuration,
                                 .resources = resources,
                                 .viewport = playViewport,
                                 .runtime = runtime.runtime(),
                                 .state = state});
}

bool hasDiagnostic(const SkinFrameEvaluationResult &result,
                   std::string_view code);

void testCapturedFrameSerialMustMatchCallbacksAndProjection() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  const auto result = evaluate(renderer, runtime, model, resources, state, 8,
                               0, nullptr, 7);
  expect(!result.submitReady &&
             hasDiagnostic(result, "skin.renderer.frame.serial"),
         "stale projection snapshot is rejected before frame evaluation");
}

void testOffsetSentinelAndSourceAwarePrecedence() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.offsets.emplace(
      5, SkinPropertyLookup<ConfigOffset>{.value = {.x = 10},
                                          .supported = true});
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {imageObject(1, 1, true)};
  auto presented = destination(1, 10, 10.0);
  presented.presentation.offsetIds = {0, -9, 200, 5};
  model.model.destinations = {std::move(presented)};

  const auto dynamic = evaluate(renderer, runtime, model, resources, state, 1);
  expect(dynamic.submitReady && dynamic.submitReady->commands.size() == 1,
         "nonpositive offset sentinels are ignored and dynamic offset resolves");
  if (dynamic.submitReady && !dynamic.submitReady->commands.empty()) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        dynamic.submitReady->commands.front().payload);
    expect(quad.vertices.front().x == 20.0F,
           "frame-state offset is applied in authored space");
  }

  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(5, ConfigOffset{.x = 20});
  const auto configured = evaluate(renderer, runtime, model, resources, state,
                                   2, 0, &configuration);
  expect(configured.submitReady && configured.submitReady->commands.size() == 1,
         "declared configured offset remains renderable");
  if (configured.submitReady && !configured.submitReady->commands.empty()) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        configured.submitReady->commands.front().payload);
    expect(quad.vertices.front().x == 30.0F,
           "configured offset has explicit precedence over dynamic fallback");
  }
}

void testCriticalLuaCallbackFailureIsAlsoAtomic() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  auto callbackImage = imageObject(2, 1, true);
  std::get<SkinImageObject>(callbackImage.payload).stateIndex =
      SkinIntegerPropertyId{9};
  model.model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{9},
       .domain = SkinIntegerPropertyDomain::ImageIndex,
       .source = runtime.failCallback(),
       .authoredOrdinal = 1});
  model.model.objects = {imageObject(1, 1, true), std::move(callbackImage)};
  model.model.destinations = {destination(1, 10, 10.0),
                              destination(2, 20, 60.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(!result.submitReady,
         "critical Lua failure after a valid image discards local commands");
  expect(!result.diagnostics.empty(), "critical Lua failure is diagnosed");
}

void testBuiltinImageStateAndOutOfRangeFallback() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  resources.addImage(2, {.x = 10, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.integerResult = {.value = 1, .supported = true};
  ValidatedBeatorajaSkinModel model;
  auto object = imageObject(1, 1, true);
  auto &image = std::get<SkinImageObject>(object.payload);
  SkinSpriteFrames second;
  second.resource = 2;
  second.frames = {{.x = 10, .y = 0, .w = 10, .h = 10}};
  image.orderedStates.push_back(std::move(second));
  image.stateIndex = SkinIntegerPropertyId{7};
  model.model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{7},
       .domain = SkinIntegerPropertyDomain::ImageIndex,
       .source = SkinBuiltinPropertySelector{.value = 42},
       .authoredOrdinal = 1});
  model.model.objects.push_back(std::move(object));
  model.model.destinations.push_back(destination(1, 10, 10.0));

  const auto selected = evaluate(renderer, runtime, model, resources, state, 1);
  expect(selected.submitReady && selected.submitReady->commands.size() == 1 &&
             std::get<SkinTexturedQuadCommand>(
                 selected.submitReady->commands.front().payload)
                     .resource == 2,
         "built-in image index selects the matching ordered state");

  state.integerResult.value = 99;
  const auto fallback = evaluate(renderer, runtime, model, resources, state, 2);
  expect(fallback.submitReady && fallback.submitReady->commands.size() == 1 &&
             std::get<SkinTexturedQuadCommand>(
                 fallback.submitReady->commands.front().payload)
                     .resource == 1,
         "out-of-range nonnegative image state falls back to state zero");
}

bool hasDiagnostic(const SkinFrameEvaluationResult &result,
                   std::string_view code) {
  return std::ranges::any_of(result.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.code == code;
  });
}

void testCriticalFailureCannotExposePartialBuffer() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {imageObject(1, 1, true), imageObject(2, 99, true)};
  model.model.destinations = {destination(1, 10, 10.0),
                              destination(2, 20, 60.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(!result.submitReady,
         "critical resource failure cannot expose the first command");
  expect(hasDiagnostic(result, "skin.renderer.resource.missing"),
         "critical resource failure is diagnosed");
}

void testOptionalFailureSuppressesOnlyItsObject() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {imageObject(1, 1, true), imageObject(2, 99, false)};
  model.model.destinations = {destination(1, 10, 10.0),
                              destination(2, 20, 60.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1,
         "optional resource failure retains independent commands");
  expect(result.submitReady &&
             result.submitReady->commands.front().sourceObject == 1,
         "only the optional dependent command is suppressed");
}

void testProjectionOrderAndCrossSpanUniquenessAreFrameCritical() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.notes = {{.submissionOrdinal = 2}, {.submissionOrdinal = 1}};
    ValidatedBeatorajaSkinModel model;
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(!result.submitReady &&
               hasDiagnostic(result, "skin.renderer.projection.order"),
           "out-of-order projection ordinals discard the frame");
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.notes = {{.submissionOrdinal = 1}};
    state.lines = {{.submissionOrdinal = 1}};
    ValidatedBeatorajaSkinModel model;
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(!result.submitReady &&
               hasDiagnostic(result, "skin.renderer.projection.order"),
           "duplicate ordinals across projection spans discard the frame");
  }
}

void testProjectionLimitsAreCheckedBeforeEvaluation() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.notes.resize(SkinCommandPolicy::maximumProjectedNotes + 1);
  for (std::size_t index = 0; index < state.notes.size(); ++index) {
    state.notes[index].submissionOrdinal =
        static_cast<std::uint32_t>(index + 1);
  }
  ValidatedBeatorajaSkinModel model;
  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(!result.submitReady &&
             hasDiagnostic(result, "skin.renderer.projection.limit"),
         "oversized projection span cannot publish a buffer");
}

void testBindingAndDisabledLookupsStayLogarithmicAtModelLimits() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.integerResult = {.value = 0, .supported = true};
  ValidatedBeatorajaSkinModel model;
  constexpr std::size_t bindingCount = 8'192;
  constexpr std::size_t destinationCount = 2'048;
  model.model.integerProperties.reserve(bindingCount);
  for (std::size_t index = 0; index < bindingCount; ++index) {
    model.model.integerProperties.push_back(
        {.id = SkinIntegerPropertyId{static_cast<std::uint32_t>(index + 1)},
         .domain = SkinIntegerPropertyDomain::ImageIndex,
         .source = SkinBuiltinPropertySelector{.value =
                                                   static_cast<int>(index)},
         .authoredOrdinal = static_cast<std::uint32_t>(index + 1)});
    model.disabledOptionalObjects.push_back(
        static_cast<SkinObjectId>(20'000 + index));
  }
  auto object = imageObject(1, 1, true);
  std::get<SkinImageObject>(object.payload).stateIndex =
      SkinIntegerPropertyId{static_cast<std::uint32_t>(bindingCount)};
  model.model.objects.push_back(std::move(object));
  model.model.destinations.reserve(destinationCount);
  for (std::size_t index = 0; index < destinationCount; ++index) {
    model.model.destinations.push_back(
        destination(1, static_cast<std::uint32_t>(index + 1),
                    static_cast<double>(index % 100)));
  }

  resetSkinRendererLookupComparisonsForTesting();
  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady &&
             result.submitReady->commands.size() == destinationCount,
         "model-limit lookup fixture evaluates every destination");
  const auto comparisons = skinRendererLookupComparisonsForTesting();
  expect(comparisons > 0 && comparisons <= 70'000,
         "binding and disabled-object lookup work is logarithmically bounded");
}

void testImageCommandsPreserveOrderAndBatchOnlyAdjacentCompatibility() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  resources.addImage(2, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {imageObject(1, 1, true), imageObject(2, 1, true),
                         imageObject(3, 2, true), imageObject(4, 1, true)};
  model.model.destinations = {
      destination(1, 10, 10.0), destination(2, 20, 60.0),
      destination(3, 30, 110.0), destination(4, 40, 160.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 4,
         "all valid image destinations lower to commands");
  if (!result.submitReady || result.submitReady->commands.size() != 4) {
    return;
  }
  expect(result.submitReady->commands[0].authoredOrdinal == 10 &&
             result.submitReady->commands[1].authoredOrdinal == 20 &&
             result.submitReady->commands[2].authoredOrdinal == 30 &&
             result.submitReady->commands[3].authoredOrdinal == 40,
         "command order is exactly authored destination order");
  expect(result.submitReady->adjacentBatches.size() == 3 &&
             result.submitReady->adjacentBatches[0].firstCommand == 0 &&
             result.submitReady->adjacentBatches[0].commandCount == 2 &&
             result.submitReady->adjacentBatches[1].firstCommand == 2 &&
             result.submitReady->adjacentBatches[1].commandCount == 1 &&
             result.submitReady->adjacentBatches[2].firstCommand == 3 &&
             result.submitReady->adjacentBatches[2].commandCount == 1,
         "only contiguous compatible image commands share a batch");
}

} // namespace

int main() {
  testCapturedFrameSerialMustMatchCallbacksAndProjection();
  testOffsetSentinelAndSourceAwarePrecedence();
  testCriticalFailureCannotExposePartialBuffer();
  testCriticalLuaCallbackFailureIsAlsoAtomic();
  testOptionalFailureSuppressesOnlyItsObject();
  testBuiltinImageStateAndOutOfRangeFallback();
  testProjectionOrderAndCrossSpanUniquenessAreFrameCritical();
  testProjectionLimitsAreCheckedBeforeEvaluation();
  testBindingAndDisabledLookupsStayLogarithmicAtModelLimits();
  testImageCommandsPreserveOrderAndBatchOnlyAdjacentCompatibility();
  return failures == 0 ? 0 : 1;
}
