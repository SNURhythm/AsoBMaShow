#include "skin/beatoraja/Skin2DRenderer.h"

#include "FileChecksum.h"
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

#include <nlohmann/json.hpp>

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
              "local number=function() return 42 end\n"
              "local fractional_number=function() return 4.9 end\n"
              "local hidden=function() return false end\n"
              "local forbidden_timer=function() error('timer invoked') end\n"
              "local callback_order=0\n"
              "local ordered_timer=function() callback_order=callback_order+1 "
              "return 0 end\n"
              "local ordered_rate=function() if callback_order~=2 then "
              "error('rate before timer') end return 0.5 end\n"
              "return {type=0,w=1280,h=720,name='command-test',"
              "host_fail_callback=fail,host_number_callback=number,"
              "host_fractional_number_callback=fractional_number,"
              "host_false_callback=hidden,"
              "host_forbidden_timer_callback=forbidden_timer,"
              "host_ordered_timer_callback=ordered_timer,"
              "host_ordered_rate_callback=ordered_rate}");
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
      numberCallback_ = header.value->callbackNamed("host_number_callback");
      fractionalNumberCallback_ =
          header.value->callbackNamed("host_fractional_number_callback");
      falseCallback_ = header.value->callbackNamed("host_false_callback");
      forbiddenTimerCallback_ =
          header.value->callbackNamed("host_forbidden_timer_callback");
      orderedTimerCallback_ =
          header.value->callbackNamed("host_ordered_timer_callback");
      orderedRateCallback_ =
          header.value->callbackNamed("host_ordered_rate_callback");
      expect(numberCallback_ && fractionalNumberCallback_ && falseCallback_ &&
                 forbiddenTimerCallback_ && orderedTimerCallback_ &&
                 orderedRateCallback_,
             "runtime ordering callbacks are retained");
    }
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "runtime configured phase executes");
    configured.value.reset();
    expect(runtime_->enterRenderPhase().ok, "runtime enters render phase");
  }

  LuaSkinRuntime &runtime() { return *runtime_; }
  LuaCallbackId failCallback() const { return *failCallback_; }
  LuaCallbackId numberCallback() const { return *numberCallback_; }
  LuaCallbackId fractionalNumberCallback() const {
    return *fractionalNumberCallback_;
  }
  LuaCallbackId falseCallback() const { return *falseCallback_; }
  LuaCallbackId forbiddenTimerCallback() const {
    return *forbiddenTimerCallback_;
  }
  LuaCallbackId orderedTimerCallback() const { return *orderedTimerCallback_; }
  LuaCallbackId orderedRateCallback() const { return *orderedRateCallback_; }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> failCallback_;
  std::optional<LuaCallbackId> numberCallback_;
  std::optional<LuaCallbackId> fractionalNumberCallback_;
  std::optional<LuaCallbackId> falseCallback_;
  std::optional<LuaCallbackId> forbiddenTimerCallback_;
  std::optional<LuaCallbackId> orderedTimerCallback_;
  std::optional<LuaCallbackId> orderedRateCallback_;
};

class FakeResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id, SkinSourceRect authored, int width = 100,
                int height = 100) {
    PreparedSkinResource resource;
    resource.id = id;
    resource.width = width;
    resource.height = height;
    resource.regions = {authored};
    resource.regionMappings = {{.authored = authored, .resolved = authored}};
    images_.emplace(id, std::move(resource));
  }

  void addImageAtlas(SkinResourceId id,
                     const std::vector<SkinSourceRect> &regions, int width,
                     int height) {
    PreparedSkinResource resource;
    resource.id = id;
    resource.width = width;
    resource.height = height;
    resource.regions = regions;
    resource.regionMappings.reserve(regions.size());
    for (const auto &region : regions) {
      resource.regionMappings.push_back(
          {.authored = region, .resolved = region});
    }
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
  findTextAtlas(SkinTextAtlasId id) const noexcept override {
    const auto found = atlases_.find(id);
    return found == atlases_.end() ? nullptr : &found->second;
  }

  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId object) const noexcept override {
    const auto found = textAtlasesByObject_.find(object);
    return found == textAtlasesByObject_.end() ? nullptr
                                               : findTextAtlas(found->second);
  }

  void addTextAtlas(SkinObjectId object, PreparedSkinTextAtlas atlas) {
    textAtlasesByObject_.emplace(object, atlas.id);
    atlases_.emplace(atlas.id, std::move(atlas));
  }

private:
  std::map<SkinResourceId, PreparedSkinResource> images_;
  std::map<SkinTextAtlasId, PreparedSkinTextAtlas> atlases_;
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject_;
};

class FakeState final : public ISkinFrameState {
public:
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &selector) override {
    ++booleanCalls;
    accessOrder.push_back(1'000 + (std::holds_alternative<int>(selector.value)
                                       ? std::get<int>(selector.value)
                                       : -1));
    return booleanResult;
  }
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override {
    return integerResult;
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override {
    return floatResult;
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return stringResult;
  }
  SkinPropertyLookup<ConfigOffset> offsetProperty(int id) override {
    accessOrder.push_back(3'000 + id);
    const auto found = offsets.find(id);
    return found == offsets.end() ? SkinPropertyLookup<ConfigOffset>{}
                                  : found->second;
  }
  std::int64_t
  timerProperty(const SkinBuiltinPropertySelector &selector) override {
    ++timerCalls;
    accessOrder.push_back(2'000 + (std::holds_alternative<int>(selector.value)
                                       ? std::get<int>(selector.value)
                                       : -1));
    if (timerSequenceIndex < timerSequence.size()) {
      return timerSequence[timerSequenceIndex++];
    }
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
  SkinGaugeStateView gaugeState() const noexcept override {
    return gaugeResult;
  }
  SkinJudgeStateView judgeState(int) const noexcept override {
    return judgeResult;
  }
  SkinNoteExpansionStateView noteExpansionState() const noexcept override {
    ++noteExpansionCalls;
    return noteExpansionResult;
  }
  std::uint64_t frameSerial() const noexcept override { return capturedSerial; }

  std::vector<SkinProjectedNoteView> notes;
  std::vector<SkinProjectedLongNoteView> longNotes;
  std::vector<SkinProjectedLineView> lines;
  SkinGaugeStateView gaugeResult;
  SkinJudgeStateView judgeResult;
  SkinNoteExpansionStateView noteExpansionResult;
  SkinPropertyLookup<bool> booleanResult;
  SkinPropertyLookup<std::int64_t> integerResult;
  SkinPropertyLookup<double> floatResult;
  SkinPropertyLookup<std::string_view> stringResult;
  std::int64_t timerResult = INT64_MIN;
  std::vector<std::int64_t> timerSequence;
  std::size_t timerSequenceIndex = 0;
  std::size_t booleanCalls = 0;
  std::size_t timerCalls = 0;
  mutable std::size_t noteExpansionCalls = 0;
  std::map<int, SkinPropertyLookup<ConfigOffset>> offsets;
  std::vector<int> accessOrder;
  std::uint64_t capturedSerial = 1;
};

class FakeGaugeRandom final : public ISkinGaugeRandomSource {
public:
  std::optional<std::uint32_t>
  next(SkinObjectId object, std::uint64_t animationEpoch,
       std::uint32_t exclusiveUpperBound) override {
    ++calls;
    lastObject = object;
    lastEpoch = animationEpoch;
    lastUpperBound = exclusiveUpperBound;
    return value;
  }

  std::optional<std::uint32_t> value;
  std::size_t calls = 0;
  SkinObjectId lastObject = 0;
  std::uint64_t lastEpoch = 0;
  std::uint32_t lastUpperBound = 0;
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

std::vector<SkinSourceRect> glyphRegions(int glyphCount, int row = 0) {
  std::vector<SkinSourceRect> result;
  result.reserve(static_cast<std::size_t>(glyphCount));
  for (int glyph = 0; glyph < glyphCount; ++glyph) {
    result.push_back({.x = glyph * 10, .y = row * 20, .w = 10, .h = 20});
  }
  return result;
}

SkinSpriteFrames glyphSprite(SkinResourceId resource,
                             std::vector<SkinSourceRect> frames,
                             int cycleMillis = 0) {
  return {.resource = resource,
          .frames = std::move(frames),
          .cycleMillis = cycleMillis};
}

SkinDestination destination(SkinObjectId object, std::uint32_t ordinal,
                            double x) {
  SkinDestinationBody body;
  body.loop = -1;
  body.authoredOrdinal = ordinal;
  body.frames = {
      {.timeMillis = 0, .x = x, .y = 20.0, .width = 40.0, .height = 30.0}};
  return {.object = object, .presentation = std::move(body)};
}

PlaySkinViewport viewport() {
  return evaluatePlaySkinViewport(
      {.width = 1280.0, .height = 720.0},
      {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0}, {});
}

SkinFrameEvaluationResult
evaluate(Skin2DRenderer &renderer, RuntimeHarness &runtime,
         ValidatedBeatorajaSkinModel &model, const FakeResources &resources,
         FakeState &state, std::uint64_t serial = 1,
         std::int64_t visualTimeMicros = 0,
         const BeatorajaSkinConfiguration *configured = nullptr,
         std::optional<std::uint64_t> capturedSerial = std::nullopt,
         ISkinGaugeRandomSource *gaugeRandomSource = nullptr,
         std::uint64_t sessionSerial = 1) {
  static const BeatorajaSkinConfiguration emptyConfiguration;
  const auto &configuration = configured ? *configured : emptyConfiguration;
  const auto playViewport = viewport();
  state.capturedSerial = capturedSerial.value_or(serial);
  return renderer.evaluateFrame({.frameSerial = serial,
                                 .sessionSerial = sessionSerial,
                                 .visualTimeMicros = visualTimeMicros,
                                 .model = model,
                                 .configuration = configuration,
                                 .resources = resources,
                                 .viewport = playViewport,
                                 .runtime = runtime.runtime(),
                                 .state = state,
                                 .gaugeRandomSource = gaugeRandomSource});
}

bool hasDiagnostic(const SkinFrameEvaluationResult &result,
                   std::string_view code);

void testCapturedFrameSerialMustMatchCallbacksAndProjection() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  const auto result =
      evaluate(renderer, runtime, model, resources, state, 8, 0, nullptr, 7);
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
  state.offsets.emplace(5, SkinPropertyLookup<ConfigOffset>{.value = {.x = 10},
                                                            .supported = true});
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {imageObject(1, 1, true)};
  auto presented = destination(1, 10, 10.0);
  presented.presentation.offsetIds = {0, -9, 200, 5};
  model.model.destinations = {std::move(presented)};

  const auto dynamic = evaluate(renderer, runtime, model, resources, state, 1);
  expect(
      dynamic.submitReady && dynamic.submitReady->commands.size() == 1,
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

void testHiddenImageStillSelectsSourceAfterRuntimeSuppression() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.booleanResult = {.value = false, .supported = true};
  state.timerResult = 0;

  auto object = imageObject(1, 1, true);
  auto &sprite =
      std::get<SkinImageObject>(object.payload).orderedStates.front();
  sprite.cycleMillis = 100;
  sprite.timer = SkinTimerPropertyId{1};

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties.push_back({
      .id = SkinBooleanPropertyId{1},
      .source = SkinBuiltinPropertySelector{.value = 10},
      .authoredOrdinal = 1,
  });
  model.model.timerProperties.push_back({
      .id = SkinTimerPropertyId{1},
      .source = SkinBuiltinPropertySelector{.value = 20},
      .authoredOrdinal = 2,
  });
  model.model.objects.push_back(std::move(object));
  auto presented = destination(1, 10, 10.0);
  presented.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             result.diagnostics.empty() &&
             state.accessOrder == std::vector<int>({1'010, 2'020, 2'020}),
         "runtime-hidden image still probes and reads its source timer after "
         "the false destination condition");
}

void testEmptyDestinationIsDroppedBeforeObjectStateResolution() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.booleanResult = {.supported = false};
  state.timerResult = 0;

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties.push_back({
      .id = SkinBooleanPropertyId{1},
      .source = SkinBuiltinPropertySelector{.value = 10},
      .authoredOrdinal = 1,
  });
  model.model.timerProperties.push_back({
      .id = SkinTimerPropertyId{1},
      .source = SkinBuiltinPropertySelector{.value = 20},
      .authoredOrdinal = 2,
  });
  model.model.objects.push_back(imageObject(1, 1, true));
  auto presented = destination(1, 10, 10.0);
  presented.presentation.frames.clear();
  presented.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  presented.presentation.timer = SkinTimerPropertyId{1};
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             result.diagnostics.empty() && state.booleanCalls == 0 &&
             state.timerCalls == 0,
         "an empty destination is removed before it can resolve object state, "
         "matching Beatoraja Skin.prepare");
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

void testProjectionOrdinalIrregularitiesNeverDiscardAGameplayFrame() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.notes = {{.submissionOrdinal = 2}, {.submissionOrdinal = 1}};
    ValidatedBeatorajaSkinModel model;
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady &&
               !hasDiagnostic(result, "skin.renderer.projection.order"),
           "out-of-order internal projection ordinals do not discard a frame");
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
    expect(result.submitReady &&
               !hasDiagnostic(result, "skin.renderer.projection.order"),
           "duplicate internal projection ordinals do not discard a frame");
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.notes = {{.submissionOrdinal = 0}};
    ValidatedBeatorajaSkinModel model;
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady &&
               !hasDiagnostic(result, "skin.renderer.projection.order"),
           "the native zero-based projection ordinal is valid");
  }
}

void testLargeProjectionDoesNotHitAnAppSpecificFrameLimit() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  // This exceeds the former app-defined 32,768-note gate.
  state.notes.resize(32'769);
  for (std::size_t index = 0; index < state.notes.size(); ++index) {
    state.notes[index].submissionOrdinal =
        static_cast<std::uint32_t>(index + 1);
  }
  ValidatedBeatorajaSkinModel model;
  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady &&
             !hasDiagnostic(result, "skin.renderer.projection.limit"),
         "large valid projection does not hit an app-specific frame limit");
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
         .source =
             SkinBuiltinPropertySelector{.value = static_cast<int>(index)},
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

void testNumberUsesSignedGlyphSetPaddingAndNegativeAlignmentShift() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const auto regions = glyphRegions(12);
  resources.addImageAtlas(1, regions, 120, 20);
  resources.addImageAtlas(2, regions, 120, 20);
  FakeState state;
  state.integerResult = {.value = -42, .supported = true};

  SkinNumberObject number;
  number.digits.positive = glyphSprite(1, regions);
  number.digits.negative = glyphSprite(2, regions);
  number.digits.glyphsPerAnimationFrame = 12;
  number.value = SkinIntegerPropertyId{1};
  number.digitCount = 4;
  number.spacing = 2;
  number.alignment = 1;
  number.perDigitOffsets.resize(4);
  number.perDigitOffsets[2] = {.x = 1.0, .y = 2.0, .width = 3.0, .height = 4.0};

  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = number.value,
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 71},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "signed-number",
                                 .payload = std::move(number),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 10, 100.0);
  presented.presentation.frames.front().width = 10.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 3,
         "signed number emits sign and two value glyphs");
  if (!result.submitReady || result.submitReady->commands.size() != 3) {
    return;
  }
  const auto &sign = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &four = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  const auto &two = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[2].payload);
  expect(sign.resource == 2 && sign.vertices[0].u == 110.0F / 120.0F,
         "negative value selects the negative glyph set and sign glyph 11");
  expect(sign.vertices[0].x == 100.0F && four.vertices[0].x == 113.0F &&
             two.vertices[0].x == 124.0F,
         "number omits one slot, shifts left, then applies per-digit x");
  expect(four.vertices[0].y == 698.0F &&
             four.vertices[2].x - four.vertices[0].x == 13.0F &&
             four.vertices[2].y - four.vertices[0].y == -24.0F,
         "number per-digit y/width/height offsets remain authored geometry");
  expect(four.vertices[0].u == 40.0F / 120.0F &&
             two.vertices[0].u == 20.0F / 120.0F,
         "number digit values select exact normalized atlas regions");
}

void testFloatTruncatesAndUsesPositiveAlignmentShift() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const auto regions = glyphRegions(13);
  resources.addImageAtlas(3, regions, 130, 20);
  FakeState state;
  state.floatResult = {.value = 4.29, .supported = true};

  SkinFloatObject floating;
  floating.digits.positive = glyphSprite(3, regions);
  floating.digits.glyphsPerAnimationFrame = 13;
  floating.value = SkinFloatPropertyId{1};
  floating.integerDigits = 2;
  floating.fractionalDigits = 1;
  floating.spacing = 2;
  floating.alignment = 1;
  floating.signVisible = true;

  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = floating.value,
       .domain = SkinFloatPropertyDomain::FloatValue,
       .source = SkinBuiltinPropertySelector{.value = 72},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "float-number",
                                 .payload = std::move(floating),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 10, 50.0);
  presented.presentation.frames.front().width = 10.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 4,
         "float emits sign, integer, decimal point, and truncated fraction");
  if (!result.submitReady || result.submitReady->commands.size() != 4) {
    return;
  }
  const std::array<float, 4> expectedU = {120.0F / 130.0F, 40.0F / 130.0F,
                                          110.0F / 130.0F, 20.0F / 130.0F};
  for (std::size_t index = 0; index < expectedU.size(); ++index) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[index].payload);
    expect(quad.vertices[0].u == expectedU[index],
           "float glyph order matches FloatFormatter");
    expect(quad.vertices[0].x == 62.0F + 12.0F * index,
           "float applies the pinned positive omitted-slot shift");
  }
}

void testZeroCycleNumericSpriteDoesNotConsultItsTimer() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  auto regions = glyphRegions(12, 0);
  auto secondRow = glyphRegions(12, 1);
  regions.insert(regions.end(), secondRow.begin(), secondRow.end());
  resources.addImageAtlas(4, regions, 120, 40);
  FakeState state;
  state.integerResult = {.value = 7, .supported = true};
  state.timerResult = INT64_MIN;

  SkinNumberObject number;
  number.digits.positive = glyphSprite(4, regions, 0);
  number.digits.positive.timer = SkinTimerPropertyId{1};
  number.digits.glyphsPerAnimationFrame = 12;
  number.value = SkinIntegerPropertyId{1};
  number.digitCount = 1;
  auto timedNumber = number;
  timedNumber.digits.positive.cycleMillis = 100;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = number.value,
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 74},
       .authoredOrdinal = 1});
  model.model.timerProperties.push_back(
      {.id = SkinTimerPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 75},
       .authoredOrdinal = 2});
  model.model.objects = {{.id = 1,
                          .authoredName = "zero-cycle-number",
                          .payload = std::move(number),
                          .authoredOrdinal = 1,
                          .critical = true},
                         {.id = 2,
                          .authoredName = "off-timer-number",
                          .payload = std::move(timedNumber),
                          .authoredOrdinal = 2,
                          .critical = true}};
  auto presented = destination(1, 10, 10.0);
  presented.presentation.frames.front().width = 10.0;
  presented.presentation.frames.front().height = 20.0;
  auto timedPresentation = destination(2, 20, 30.0);
  timedPresentation.presentation.frames.front().width = 10.0;
  timedPresentation.presentation.frames.front().height = 20.0;
  model.model.destinations = {std::move(presented),
                              std::move(timedPresentation)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 2,
         "zero-cycle and off-timer numeric sprites render row zero");
  expect(state.timerCalls == 1,
         "zero cycle skips its timer while nonzero cycle reads it once");
  if (result.submitReady && !result.submitReady->commands.empty()) {
    for (const auto &command : result.submitReady->commands) {
      const auto &quad = std::get<SkinTexturedQuadCommand>(command.payload);
      expect(quad.vertices[0].v == 0.5F,
             "zero-cycle and off-timer sprites both select row zero");
    }
  }
}

void testFalseDestinationSkipsNumericSourceTimerAfterValueLookup() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const auto regions = glyphRegions(12);
  resources.addImageAtlas(4, regions, 120, 20);
  FakeState state;

  SkinNumberObject number;
  number.digits.positive = glyphSprite(4, regions, 100);
  number.digits.positive.timer = SkinTimerPropertyId{1};
  number.digits.glyphsPerAnimationFrame = 12;
  number.value = SkinIntegerPropertyId{1};
  number.digitCount = 2;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = number.value,
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = runtime.numberCallback(),
       .authoredOrdinal = 1});
  model.model.booleanProperties.push_back({.id = SkinBooleanPropertyId{1},
                                           .source = runtime.falseCallback(),
                                           .authoredOrdinal = 2});
  model.model.timerProperties.push_back(
      {.id = SkinTimerPropertyId{1},
       .source = runtime.forbiddenTimerCallback(),
       .authoredOrdinal = 3});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "conditioned-number",
                                 .payload = std::move(number),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 10, 10.0);
  presented.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             result.diagnostics.empty(),
         "false destination resolves numeric value but never its source timer");
}

void testLuaFractionalNumberUsesPinnedIntegerCoercion() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const auto regions = glyphRegions(10);
  resources.addImageAtlas(5, regions, 100, 20);
  FakeState state;

  SkinNumberObject number;
  number.digits.positive = glyphSprite(5, regions);
  number.digits.glyphsPerAnimationFrame = 10;
  number.value = SkinIntegerPropertyId{1};
  number.digitCount = 1;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = number.value,
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = runtime.fractionalNumberCallback(),
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "coerced-number",
                                 .payload = std::move(number),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 10, 10.0);
  presented.presentation.frames.front().width = 10.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(
      result.submitReady && result.submitReady->commands.size() == 1,
      "fractional Lua number coerces instead of failing its integer binding");
  if (result.submitReady && !result.submitReady->commands.empty()) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands.front().payload);
    expect(quad.vertices[0].u == 0.4F,
           "Lua integer coercion truncates toward zero like pinned toint");
  }
}

PreparedSkinTextAtlas avAtlas() {
  PreparedSkinTextAtlas atlas;
  atlas.id = 7;
  atlas.width = 100;
  atlas.height = 50;
  atlas.key.pointSize = 20;
  atlas.ascent = 16;
  atlas.capHeight = 15;
  atlas.descent = -4;
  atlas.lineHeight = 20;
  atlas.glyphs.emplace(U'A', SkinPreparedGlyphMetrics{
                                 .region = {.x = 10, .y = 5, .w = 10, .h = 20},
                                 .bearingX = 1,
                                 .bearingY = 15,
                                 .advance = 12,
                                 .layoutOffsetY = -20});
  atlas.glyphs.emplace(U'V', SkinPreparedGlyphMetrics{
                                 .region = {.x = 30, .y = 5, .w = 12, .h = 20},
                                 .bearingX = 2,
                                 .bearingY = 14,
                                 .advance = 13,
                                 .layoutOffsetY = -21});
  atlas.glyphs.emplace(U' ', SkinPreparedGlyphMetrics{
                                 .region = {.x = 50, .y = 5, .w = 4, .h = 20},
                                 .bearingX = 0,
                                 .bearingY = 15,
                                 .advance = 5,
                                 .layoutOffsetY = -20});
  atlas.kerning.emplace(std::pair{U'A', U'V'}, -2);
  return atlas;
}

PreparedSkinTextAtlas verticalAtlas() {
  PreparedSkinTextAtlas atlas;
  atlas.id = 8;
  atlas.width = 100;
  atlas.height = 100;
  atlas.key.pointSize = 20;
  atlas.ascent = 16;
  atlas.capHeight = 12;
  atlas.descent = -4;
  atlas.lineHeight = 20;
  atlas.glyphs.emplace(U'A', SkinPreparedGlyphMetrics{
                                 .region = {.x = 5, .y = 5, .w = 14, .h = 16},
                                 .bearingX = -1,
                                 .bearingY = 14,
                                 .advance = 13,
                                 .layoutOffsetY = -14});
  atlas.glyphs.emplace(U'g', SkinPreparedGlyphMetrics{
                                 .region = {.x = 25, .y = 5, .w = 13, .h = 20},
                                 .bearingX = -1,
                                 .bearingY = 10,
                                 .advance = 11,
                                 .layoutOffsetY = -22});
  atlas.glyphs.emplace(
      U'\u00c9',
      SkinPreparedGlyphMetrics{.region = {.x = 45, .y = 5, .w = 13, .h = 24},
                               .bearingX = -1,
                               .bearingY = 20,
                               .advance = 10,
                               .layoutOffsetY = -16});
  return atlas;
}

SkinObjectDefinition textObject(SkinObjectId id, bool critical,
                                SkinStringPropertyId value = {}) {
  SkinTextObject text;
  text.font = 90;
  text.pointSize = 20;
  text.alignment = 0;
  if (value) {
    text.value = value;
  } else {
    text.literal = "AV";
  }
  return {.id = id,
          .authoredName = "text-" + std::to_string(id),
          .payload = std::move(text),
          .authoredOrdinal = id,
          .critical = critical};
}

void testTextUsesPreparedMetricsKerningAndAtlasUvs() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addTextAtlas(1, avAtlas());
  FakeState state;
  state.stringResult = {.value = "AV", .supported = true};
  ValidatedBeatorajaSkinModel model;
  model.model.stringProperties.push_back(
      {.id = SkinStringPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 73},
       .authoredOrdinal = 1});
  model.model.objects.push_back(textObject(1, true, SkinStringPropertyId{1}));
  auto presented = destination(1, 10, 100.0);
  presented.presentation.frames.front().width = 100.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1,
         "prepared text lowers to one glyph-run command");
  if (!result.submitReady || result.submitReady->commands.size() != 1 ||
      !std::holds_alternative<SkinGlyphRunCommand>(
          result.submitReady->commands[0].payload)) {
    return;
  }
  const auto &run =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[0].payload);
  expect(run.atlas == 7 && run.glyphs.size() == 2,
         "text command retains the prepared object atlas and both glyphs");
  if (run.glyphs.size() != 2) {
    return;
  }
  expect(run.glyphs[0].codepoint == U'A' &&
             run.glyphs[0].vertices[0].x == 100.0F &&
             run.glyphs[0].vertices[0].u == 0.1F &&
             run.glyphs[0].vertices[0].v == 0.5F,
         "first text glyph uses prepared bearing and nontrivial atlas UV");
  expect(run.glyphs[1].codepoint == U'V' &&
             run.glyphs[1].vertices[0].x == 111.0F,
         "AV placement consumes the prepared negative pair kerning");
}

void testTextVerticalPlacementMatchesPinnedBitmapFontBaseline() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addTextAtlas(1, verticalAtlas());
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  auto text = textObject(1, true);
  std::get<SkinTextObject>(text.payload).literal =
      std::string("Ag") + "\xc3\x89\nA";
  model.model.objects.push_back(std::move(text));
  auto presented = destination(1, 10, 100.0);
  presented.presentation.frames.front().y = 100.0;
  presented.presentation.frames.front().width = 100.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1,
         "cap, descender, and diacritic text lowers as one glyph run");
  if (!result.submitReady || result.submitReady->commands.size() != 1) {
    return;
  }
  const auto &run = std::get<SkinGlyphRunCommand>(
      result.submitReady->commands.front().payload);
  expect(run.glyphs.size() == 4,
         "vertical baseline fixture retains both hard lines");
  if (run.glyphs.size() != 4) {
    return;
  }
  expect(run.glyphs[0].vertices[0].y == 614.0F,
         "cap glyph subtracts cap height from the destination-top baseline");
  expect(run.glyphs[1].vertices[0].y == 622.0F,
         "descender uses its taller prepared bitmap below the baseline");
  expect(run.glyphs[2].vertices[0].y == 616.0F,
         "diacritic bearing can extend above the cap-height line");
  expect(run.glyphs[3].vertices[0].y == 634.0F,
         "second hard line applies one prepared line-height step");
}

void testMissingTextGlyphHonorsCriticalityWithoutPartialCommands() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
    resources.addTextAtlas(2, avAtlas());
    FakeState state;
    ValidatedBeatorajaSkinModel model;
    auto missing = textObject(2, true);
    std::get<SkinTextObject>(missing.payload).literal = "AZ";
    model.model.objects = {imageObject(1, 1, true), std::move(missing)};
    model.model.destinations = {destination(1, 10, 10.0),
                                destination(2, 20, 60.0)};
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(!result.submitReady &&
               hasDiagnostic(result, "skin.renderer.text.glyph"),
           "critical missing glyph discards commands emitted earlier");
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    resources.addImage(1, {.x = 0, .y = 0, .w = 10, .h = 10});
    resources.addTextAtlas(2, avAtlas());
    FakeState state;
    ValidatedBeatorajaSkinModel model;
    auto missing = textObject(2, false);
    std::get<SkinTextObject>(missing.payload).literal = "AZ";
    model.model.objects = {imageObject(1, 1, true), std::move(missing)};
    model.model.destinations = {destination(1, 10, 10.0),
                                destination(2, 20, 60.0)};
    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady && result.submitReady->commands.size() == 1 &&
               result.submitReady->commands[0].sourceObject == 1,
           "optional missing glyph suppresses its whole text run only");
  }
}

void testFalseDestinationSkipsTextValueCallback() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  model.model.stringProperties.push_back({.id = SkinStringPropertyId{1},
                                          .source = runtime.failCallback(),
                                          .authoredOrdinal = 1});
  model.model.booleanProperties.push_back({.id = SkinBooleanPropertyId{1},
                                           .source = runtime.falseCallback(),
                                           .authoredOrdinal = 2});
  model.model.objects.push_back(textObject(1, true, SkinStringPropertyId{1}));
  auto presented = destination(1, 10, 10.0);
  presented.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  model.model.destinations.push_back(std::move(presented));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             result.diagnostics.empty(),
         "false text destination does not invoke its value callback");
}

void testTextGlyphLimitFailsBeforePublishingACommand() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addTextAtlas(1, avAtlas());
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  auto oversized = textObject(1, true);
  std::get<SkinTextObject>(oversized.payload)
      .literal.assign(SkinCommandPolicy::maximumGlyphInstances + 1, 'A');
  model.model.objects.push_back(std::move(oversized));
  model.model.destinations.push_back(destination(1, 10, 10.0));

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(!result.submitReady &&
             hasDiagnostic(result, "skin.renderer.command.limit"),
         "oversized text is rejected before a glyph-run can be published");
}

void testTextAlignmentWrappingAndShrinkUsePreparedAdvances() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addTextAtlas(1, avAtlas());
  resources.addTextAtlas(2, avAtlas());
  resources.addTextAtlas(3, avAtlas());
  FakeState state;
  ValidatedBeatorajaSkinModel model;

  auto centered = textObject(1, true);
  std::get<SkinTextObject>(centered.payload).alignment = 1;
  auto wrapped = textObject(2, true);
  std::get<SkinTextObject>(wrapped.payload).wrapping = true;
  auto shrunk = textObject(3, true);
  auto &shrunkText = std::get<SkinTextObject>(shrunk.payload);
  shrunkText.literal = "AVAV";
  shrunkText.overflow = 1;
  model.model.objects = {std::move(centered), std::move(wrapped),
                         std::move(shrunk)};

  auto centerDestination = destination(1, 10, 100.0);
  centerDestination.presentation.frames.front().width = 100.0;
  centerDestination.presentation.frames.front().height = 20.0;
  auto wrapDestination = destination(2, 20, 200.0);
  wrapDestination.presentation.frames.front().width = 12.0;
  wrapDestination.presentation.frames.front().height = 20.0;
  auto shrinkDestination = destination(3, 30, 300.0);
  shrinkDestination.presentation.frames.front().width = 23.0;
  shrinkDestination.presentation.frames.front().height = 20.0;
  model.model.destinations = {std::move(centerDestination),
                              std::move(wrapDestination),
                              std::move(shrinkDestination)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 3,
         "centered, wrapped, and shrunk text all lower");
  if (!result.submitReady || result.submitReady->commands.size() != 3) {
    return;
  }
  const auto &centerRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[0].payload);
  const auto &wrapRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[1].payload);
  const auto &shrinkRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[2].payload);
  expect(centerRun.glyphs[0].vertices[0].x == 88.5F &&
             centerRun.glyphs[1].vertices[0].x == 99.5F,
         "center alignment uses the kerned run width around its anchor");
  expect(wrapRun.glyphs[1].vertices[0].x == 200.0F &&
             wrapRun.glyphs[1].vertices[0].y > wrapRun.glyphs[0].vertices[0].y,
         "wrapping starts the second glyph on the next prepared line");
  expect(
      shrinkRun.glyphs.size() == 4 &&
          shrinkRun.glyphs[1].vertices[0].x == 305.5F,
      "shrink scales horizontal advances and kerning without scaling height");
}

void testTextWordWrapAndMultilineTruncateMatchGlyphLayoutBoundaries() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addTextAtlas(1, avAtlas());
  resources.addTextAtlas(2, avAtlas());
  resources.addTextAtlas(3, avAtlas());
  FakeState state;
  ValidatedBeatorajaSkinModel model;

  auto wordWrapped = textObject(1, true);
  auto &wordText = std::get<SkinTextObject>(wordWrapped.payload);
  wordText.literal = "AV AV";
  wordText.wrapping = true;
  auto multiline = textObject(2, true);
  auto &multilineText = std::get<SkinTextObject>(multiline.payload);
  multilineText.literal = "A\nAVAV\nV";
  multilineText.overflow = 2;
  auto firstTooWide = textObject(3, true);
  auto &wideText = std::get<SkinTextObject>(firstTooWide.payload);
  wideText.literal = "A";
  wideText.overflow = 2;
  model.model.objects = {std::move(wordWrapped), std::move(multiline),
                         std::move(firstTooWide)};

  auto wordDestination = destination(1, 10, 100.0);
  wordDestination.presentation.frames.front().width = 25.0;
  wordDestination.presentation.frames.front().height = 20.0;
  auto multilineDestination = destination(2, 20, 200.0);
  multilineDestination.presentation.frames.front().width = 23.0;
  multilineDestination.presentation.frames.front().height = 20.0;
  auto wideDestination = destination(3, 30, 300.0);
  wideDestination.presentation.frames.front().width = 1.0;
  wideDestination.presentation.frames.front().height = 20.0;
  model.model.destinations = {std::move(wordDestination),
                              std::move(multilineDestination),
                              std::move(wideDestination)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 3,
         "word-wrapped and truncated text lower deterministically");
  if (!result.submitReady || result.submitReady->commands.size() != 3) {
    return;
  }
  const auto &wordRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[0].payload);
  const auto &multilineRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[1].payload);
  const auto &wideRun =
      std::get<SkinGlyphRunCommand>(result.submitReady->commands[2].payload);
  expect(wordRun.glyphs.size() == 4 &&
             wordRun.glyphs[2].vertices[0].x ==
                 wordRun.glyphs[0].vertices[0].x &&
             wordRun.glyphs[2].vertices[0].y > wordRun.glyphs[0].vertices[0].y,
         "word wrap removes boundary whitespace and moves the whole next word");
  expect(multilineRun.glyphs.size() == 3 &&
             multilineRun.glyphs[0].codepoint == U'A' &&
             multilineRun.glyphs[1].codepoint == U'A' &&
             multilineRun.glyphs[2].codepoint == U'V',
         "truncate stops after the first overflowing hard line");
  expect(wideRun.glyphs.size() == 1 && wideRun.glyphs[0].codepoint == U'A',
         "truncate retains a first glyph wider than the destination");
}

void testFalseDestinationSkipsSliderAndGraphSourceAndValueCallbacks() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(30, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;

  SkinSpriteFrames sprite =
      glyphSprite(30, {{.x = 0, .y = 0, .w = 10, .h = 10}}, 100);
  sprite.timer = SkinTimerPropertyId{1};
  SkinSliderObject slider;
  slider.knob = sprite;
  slider.value = SkinFloatPropertyId{1};
  SkinGraphObject graph;
  graph.fill = sprite;
  graph.value = SkinFloatPropertyId{2};

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties.push_back({.id = SkinBooleanPropertyId{1},
                                           .source = runtime.falseCallback(),
                                           .authoredOrdinal = 1});
  model.model.timerProperties.push_back(
      {.id = SkinTimerPropertyId{1},
       .source = runtime.forbiddenTimerCallback(),
       .authoredOrdinal = 2});
  model.model.floatProperties = {{.id = SkinFloatPropertyId{1},
                                  .domain = SkinFloatPropertyDomain::Rate,
                                  .source = runtime.failCallback(),
                                  .authoredOrdinal = 3},
                                 {.id = SkinFloatPropertyId{2},
                                  .domain = SkinFloatPropertyDomain::Rate,
                                  .source = runtime.failCallback(),
                                  .authoredOrdinal = 4}};
  model.model.objects = {{.id = 1,
                          .authoredName = "slider",
                          .payload = std::move(slider),
                          .authoredOrdinal = 1,
                          .critical = true},
                         {.id = 2,
                          .authoredName = "graph",
                          .payload = std::move(graph),
                          .authoredOrdinal = 2,
                          .critical = true}};
  auto sliderDestination = destination(1, 80, 10.0);
  sliderDestination.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  auto graphDestination = destination(2, 81, 60.0);
  graphDestination.presentation.conditions.push_back(SkinBooleanPropertyId{1});
  model.model.destinations = {std::move(sliderDestination),
                              std::move(graphDestination)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             result.diagnostics.empty(),
         "false destinations skip slider/graph source timers and values");
}

void testSliderSourceTimerPrecedesValueAndZeroCycleSkipsTimer() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const std::vector<SkinSourceRect> frames = {
      {.x = 0, .y = 0, .w = 10, .h = 10}, {.x = 10, .y = 0, .w = 10, .h = 10}};
  resources.addImageAtlas(30, frames, 20, 10);
  FakeState state;
  state.floatResult = {.value = 0.25, .supported = true};

  SkinSliderObject ordered;
  ordered.knob = glyphSprite(30, frames, 100);
  ordered.knob.timer = SkinTimerPropertyId{1};
  ordered.value = SkinFloatPropertyId{1};
  ordered.direction = 1;
  ordered.range = 40.0;
  SkinSliderObject zeroCycle;
  zeroCycle.knob = glyphSprite(30, frames, 0);
  zeroCycle.knob.timer = SkinTimerPropertyId{2};
  zeroCycle.value = SkinFloatPropertyId{2};

  ValidatedBeatorajaSkinModel model;
  model.model.timerProperties = {{.id = SkinTimerPropertyId{1},
                                  .source = runtime.orderedTimerCallback(),
                                  .authoredOrdinal = 1},
                                 {.id = SkinTimerPropertyId{2},
                                  .source = runtime.forbiddenTimerCallback(),
                                  .authoredOrdinal = 2}};
  model.model.floatProperties = {
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = runtime.orderedRateCallback(),
       .authoredOrdinal = 3},
      {.id = SkinFloatPropertyId{2},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 95},
       .authoredOrdinal = 4}};
  model.model.objects = {{.id = 1,
                          .authoredName = "ordered-slider",
                          .payload = std::move(ordered),
                          .authoredOrdinal = 1,
                          .critical = true},
                         {.id = 2,
                          .authoredName = "zero-cycle-slider",
                          .payload = std::move(zeroCycle),
                          .authoredOrdinal = 2,
                          .critical = true}};
  auto first = destination(1, 82, 100.0);
  first.presentation.loop = 0;
  auto second = destination(2, 83, 200.0);
  second.presentation.loop = 0;
  model.model.destinations = {std::move(first), std::move(second)};

  const auto result =
      evaluate(renderer, runtime, model, resources, state, 1, 50'000);
  expect(
      result.submitReady && result.submitReady->commands.size() == 2,
      "slider source timer resolves before value and zero cycle skips timer");
  if (result.submitReady && result.submitReady->commands.size() == 2) {
    const auto &orderedQuad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[0].payload);
    const auto &zeroCycleQuad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[1].payload);
    expect(orderedQuad.vertices[0].x == 120.0F &&
               orderedQuad.vertices[0].u == 0.5F,
           "ordered slider uses timer-selected frame before its rate callback");
    expect(zeroCycleQuad.vertices[0].u == 0.0F,
           "zero-cycle slider selects row zero without invoking its timer");
  }
}

void testSliderDirectionsMoveBeforeSharedProjection() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(30, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.floatResult = {.value = 0.25, .supported = true};
  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 91},
       .authoredOrdinal = 1});
  for (std::uint8_t direction = 0; direction < 4; ++direction) {
    SkinSliderObject slider;
    slider.knob = glyphSprite(30, {{.x = 0, .y = 0, .w = 10, .h = 10}});
    slider.value = SkinFloatPropertyId{1};
    slider.direction = direction;
    slider.range = 40.0;
    const SkinObjectId id = static_cast<SkinObjectId>(direction + 1);
    model.model.objects.push_back({.id = id,
                                   .authoredName = "slider",
                                   .payload = std::move(slider),
                                   .authoredOrdinal = id,
                                   .critical = true});
    auto presented = destination(id, 90 + direction, 100.0);
    presented.presentation.frames.front().y = 20.0;
    presented.presentation.frames.front().width = 40.0;
    presented.presentation.frames.front().height = 30.0;
    model.model.destinations.push_back(std::move(presented));
  }

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 4,
         "all four slider directions emit one moved knob");
  if (!result.submitReady || result.submitReady->commands.size() != 4) {
    return;
  }
  const auto &up = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &right = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  const auto &down = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[2].payload);
  const auto &left = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[3].payload);
  expect(up.vertices[0].x == 100.0F && up.vertices[0].y == 690.0F,
         "slider direction zero moves upward in authored space");
  expect(right.vertices[0].x == 110.0F && right.vertices[0].y == 700.0F,
         "slider direction one moves right in authored space");
  expect(down.vertices[0].x == 100.0F && down.vertices[0].y == 710.0F,
         "slider direction two moves downward in authored space");
  expect(left.vertices[0].x == 90.0F && left.vertices[0].y == 700.0F,
         "slider direction three moves left in authored space");
}

void testGraphCropsLeftOrBottomWithJavaTruncation() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 9, .h = 7};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.floatResult = {.value = 0.5, .supported = true};
  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 92},
       .authoredOrdinal = 1});
  for (int direction = 0; direction < 2; ++direction) {
    SkinGraphObject graph;
    graph.fill = glyphSprite(31, {region});
    graph.value = SkinFloatPropertyId{1};
    graph.direction = direction;
    const SkinObjectId id = static_cast<SkinObjectId>(direction + 1);
    model.model.objects.push_back({.id = id,
                                   .authoredName = "graph",
                                   .payload = std::move(graph),
                                   .authoredOrdinal = id,
                                   .critical = true});
    auto presented = destination(id, 100 + direction, 100.0);
    presented.presentation.frames.front().y = 20.0;
    presented.presentation.frames.front().width = 40.0;
    presented.presentation.frames.front().height = 30.0;
    model.model.destinations.push_back(std::move(presented));
  }

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 2,
         "horizontal and vertical graphs each emit one cropped quad");
  if (!result.submitReady || result.submitReady->commands.size() != 2) {
    return;
  }
  const auto &horizontal = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &vertical = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  expect(
      horizontal.vertices[1].x == 120.0F && horizontal.vertices[1].u == 0.14F,
      "horizontal graph truncates nine source pixels to four and halves width");
  expect(
      vertical.vertices[2].y == 685.0F && vertical.vertices[0].v == 0.27F &&
          vertical.vertices[2].v == 0.24F,
      "vertical graph keeps the bottom three source pixels and halves height");
}

void testExplicitSliderAndGraphRatesRemainUnclamped() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 9, .h = 7};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.floatResult = {.value = 1.5, .supported = true};
  SkinSliderObject slider;
  slider.knob = glyphSprite(31, {region});
  slider.value = SkinFloatPropertyId{1};
  slider.direction = 1;
  slider.range = 40.0;
  SkinGraphObject graph;
  graph.fill = glyphSprite(31, {region});
  graph.value = SkinFloatPropertyId{1};
  graph.direction = 0;

  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 94},
       .authoredOrdinal = 1});
  model.model.objects = {{.id = 1,
                          .authoredName = "unclamped-slider",
                          .payload = std::move(slider),
                          .authoredOrdinal = 1,
                          .critical = true},
                         {.id = 2,
                          .authoredName = "unclamped-graph",
                          .payload = std::move(graph),
                          .authoredOrdinal = 2,
                          .critical = true}};
  auto sliderDestination = destination(1, 102, 100.0);
  sliderDestination.presentation.loop = 0;
  auto graphDestination = destination(2, 103, 100.0);
  graphDestination.presentation.loop = 0;
  model.model.destinations = {std::move(sliderDestination),
                              std::move(graphDestination)};

  const auto aboveOne = evaluate(renderer, runtime, model, resources, state, 1);
  expect(aboveOne.submitReady && aboveOne.submitReady->commands.size() == 2,
         "finite explicit rates above one remain renderable");
  if (aboveOne.submitReady && aboveOne.submitReady->commands.size() == 2) {
    const auto &sliderQuad = std::get<SkinTexturedQuadCommand>(
        aboveOne.submitReady->commands[0].payload);
    const auto &graphQuad = std::get<SkinTexturedQuadCommand>(
        aboveOne.submitReady->commands[1].payload);
    expect(sliderQuad.vertices[0].x == 160.0F,
           "slider consumes the authored rate above one without clamping");
    expect(graphQuad.vertices[1].x == 160.0F &&
               graphQuad.vertices[1].u == 0.23F,
           "graph expands destination and source crop above one");
  }

  state.floatResult.value = -0.5;
  const auto negative = evaluate(renderer, runtime, model, resources, state, 2);
  expect(negative.submitReady && negative.submitReady->commands.size() == 2,
         "finite negative explicit rates preserve pinned flipped geometry");
  if (negative.submitReady && negative.submitReady->commands.size() == 2) {
    const auto &sliderQuad = std::get<SkinTexturedQuadCommand>(
        negative.submitReady->commands[0].payload);
    const auto &graphQuad = std::get<SkinTexturedQuadCommand>(
        negative.submitReady->commands[1].payload);
    expect(sliderQuad.vertices[0].x == 80.0F,
           "negative slider rate moves opposite the authored direction");
    expect(graphQuad.vertices[1].x == 80.0F && graphQuad.vertices[1].u == 0.06F,
           "negative graph rate retains signed source and destination widths");
  }
}

void testNegativeGraphsUseAbsoluteIntrinsicSizeForStretching() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 9, .h = 7};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.floatResult = {.value = -0.5, .supported = true};
  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 94},
       .authoredOrdinal = 1});

  for (int direction : {0, 1, 0}) {
    SkinGraphObject graph;
    graph.fill = glyphSprite(31, {region});
    graph.value = SkinFloatPropertyId{1};
    graph.direction = direction;
    const auto id = static_cast<SkinObjectId>(model.model.objects.size() + 1);
    model.model.objects.push_back({.id = id,
                                   .authoredName = "negative-graph",
                                   .payload = std::move(graph),
                                   .authoredOrdinal = id,
                                   .critical = true});
    auto presented = destination(id, 110 + id, 100.0);
    presented.presentation.loop = 0;
    presented.presentation.stretch =
        id == 3 ? SkinStretchMode::KeepAspectRatioFitWidth
                : SkinStretchMode::NoResize;
    model.model.destinations.push_back(std::move(presented));
  }

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 3,
         "negative graphs render through no-resize and aspect modes");
  if (!result.submitReady || result.submitReady->commands.size() != 3) {
    return;
  }
  const auto &horizontal = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &vertical = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  const auto &aspect = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[2].payload);
  expect(horizontal.vertices[0].x == 88.0F &&
             horizontal.vertices[1].x == 92.0F &&
             horizontal.vertices[0].u == 0.10F &&
             horizontal.vertices[1].u == 0.06F,
         "no-resize centers a positive intrinsic width while retaining "
         "reversed horizontal UVs");
  expect(vertical.vertices[0].y == 709.0F && vertical.vertices[2].y == 706.0F &&
             vertical.vertices[0].v == 0.27F && vertical.vertices[2].v == 0.30F,
         "no-resize centers a positive intrinsic height while retaining "
         "reversed vertical UVs");
  expect(aspect.vertices[0].y == 667.5F && aspect.vertices[2].y == 702.5F,
         "aspect stretching divides by the positive intrinsic source width");
}

void testExtremeFiniteSliderAndGraphRatesFailWithoutPublishing() {
  const auto run = [](bool graphObject) {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    const SkinSourceRect region{.x = 10, .y = 20, .w = 9, .h = 7};
    resources.addImage(31, region, 100, 100);
    FakeState state;
    state.floatResult = {.value = std::numeric_limits<double>::max(),
                         .supported = true};
    ValidatedBeatorajaSkinModel model;
    model.model.floatProperties.push_back(
        {.id = SkinFloatPropertyId{1},
         .domain = SkinFloatPropertyDomain::Rate,
         .source = SkinBuiltinPropertySelector{.value = 94},
         .authoredOrdinal = 1});
    if (graphObject) {
      SkinGraphObject graph;
      graph.fill = glyphSprite(31, {region});
      graph.value = SkinFloatPropertyId{1};
      graph.direction = 0;
      model.model.objects.push_back({.id = 1,
                                     .authoredName = "extreme-graph",
                                     .payload = std::move(graph),
                                     .authoredOrdinal = 1,
                                     .critical = true});
    } else {
      SkinSliderObject slider;
      slider.knob = glyphSprite(31, {region});
      slider.value = SkinFloatPropertyId{1};
      slider.direction = 1;
      slider.range = 40.0;
      model.model.objects.push_back({.id = 1,
                                     .authoredName = "extreme-slider",
                                     .payload = std::move(slider),
                                     .authoredOrdinal = 1,
                                     .critical = true});
    }
    auto presented = destination(1, 114, 100.0);
    presented.presentation.loop = 0;
    model.model.destinations = {std::move(presented)};
    return evaluate(renderer, runtime, model, resources, state);
  };

  const auto slider = run(false);
  const auto graph = run(true);
  expect(!slider.submitReady &&
             hasDiagnostic(slider, "skin.renderer.geometry.invalid"),
         "extreme finite slider rates fail atomically before float upload");
  expect(!graph.submitReady &&
             hasDiagnostic(graph, "skin.renderer.geometry.invalid"),
         "extreme finite graph rates fail atomically before float upload");
}

void testSliderAndGraphRatesRoundAtTheJavaFloatBoundary() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 10, .h = 8};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.floatResult = {.value = 0.49999999, .supported = true};
  SkinGraphObject graph;
  graph.fill = glyphSprite(31, {region});
  graph.value = SkinFloatPropertyId{1};
  graph.direction = 0;
  ValidatedBeatorajaSkinModel model;
  model.model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 94},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "float-boundary-graph",
                                 .payload = std::move(graph),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 115, 100.0);
  presented.presentation.loop = 0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1,
         "a graph rate just below one half remains renderable");
  if (result.submitReady && result.submitReady->commands.size() == 1) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[0].payload);
    expect(quad.vertices[1].u == 0.15F,
           "graph rate rounds to Java float before the pixel crop");
  }
}

void testIntegerGraphRatesUsePinnedFloatSubtractionOrder() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 10, .h = 8};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.integerResult = {.value = 16'777'217, .supported = true};
  SkinGraphObject graph;
  graph.fill = glyphSprite(31, {region});
  graph.value =
      SkinSliderObject::IntegerRangeSource{.value = SkinIntegerPropertyId{1},
                                           .minimum = 16'777'216,
                                           .maximum = 16'777'217};
  graph.direction = 0;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{1},
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 95},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "integer-boundary-graph",
                                 .payload = std::move(graph),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 116, 100.0);
  presented.presentation.loop = 0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty(),
         "integer graph casts the value to Java float before subtracting its "
         "large minimum");
}

void testDescendingIntegerGraphRatesUsePinnedDirection() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 10, .h = 8};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.integerResult = {.value = 25, .supported = true};
  SkinGraphObject graph;
  graph.fill = glyphSprite(31, {region});
  graph.value = SkinSliderObject::IntegerRangeSource{
      .value = SkinIntegerPropertyId{1}, .minimum = 100, .maximum = 0};
  graph.direction = 0;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{1},
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 95},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "descending-range-graph",
                                 .payload = std::move(graph),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 117, 100.0);
  presented.presentation.loop = 0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1,
         "descending integer Graph rates remain renderable");
  if (result.submitReady && result.submitReady->commands.size() == 1) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[0].payload);
    expect(quad.vertices[1].u == 0.17F && quad.vertices[1].x == 130.0F,
           "descending range maps value 25 within 100-to-0 to rate 0.75");
  }
}

void testIntegerGraphOverflowRangeDoesNotRejectTheFrame() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  const SkinSourceRect region{.x = 10, .y = 20, .w = 10, .h = 8};
  resources.addImage(31, region, 100, 100);
  FakeState state;
  state.integerResult = {.value = 0, .supported = true};
  SkinGraphObject graph;
  graph.fill = glyphSprite(31, {region});
  graph.value = SkinSliderObject::IntegerRangeSource{
      .value = SkinIntegerPropertyId{1},
      .minimum = std::numeric_limits<int>::min(),
      .maximum = std::numeric_limits<int>::max()};
  graph.direction = 0;
  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{1},
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 95},
       .authoredOrdinal = 1});
  model.model.objects.push_back({.id = 1,
                                 .authoredName = "overflow-range-graph",
                                 .payload = std::move(graph),
                                 .authoredOrdinal = 1,
                                 .critical = true});
  auto presented = destination(1, 118, 100.0);
  presented.presentation.loop = 0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             !hasDiagnostic(result, "skin.renderer.rate.range"),
         "a Java-overflow integer Graph range is retained but emits no unsafe quad");
}

SkinGaugeObject gaugeObject(FakeResources &resources,
                            SkinGaugeAnimationType animation, int parts = 5) {
  SkinGaugeObject gauge;
  gauge.parts = parts;
  gauge.animation = animation;
  gauge.animationRange = 3;
  gauge.animationCycleMillis = 4;
  for (int role = 0; role < 36; ++role) {
    const SkinResourceId resource = static_cast<SkinResourceId>(100 + role);
    const SkinSourceRect region{.x = 0, .y = 0, .w = 10, .h = 10};
    resources.addImage(resource, region);
    gauge.orderedNodes.push_back(glyphSprite(resource, {region}));
  }
  return gauge;
}

void testGaugeMapsFamiliesRolesAndPartGeometry() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.gaugeResult = {.supported = true,
                       .value = 40.0,
                       .gaugeType = 6,
                       .minimum = 0.0,
                       .maximum = 100.0,
                       .border = 20.0};
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {
      {.id = 1,
       .authoredName = "gauge",
       .payload = gaugeObject(resources, SkinGaugeAnimationType::Increase),
       .authoredOrdinal = 1,
       .critical = true}};
  auto presented = destination(1, 110, 10.0);
  presented.presentation.frames.front().y = 20.0;
  presented.presentation.frames.front().width = 200.0;
  presented.presentation.frames.front().height = 20.0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 5,
         "five-part gauge emits one node per part");
  if (!result.submitReady || result.submitReady->commands.size() != 5) {
    return;
  }
  const std::array<SkinResourceId, 5> resourcesByPart = {118, 122, 120, 120,
                                                         120};
  for (std::size_t part = 0; part < resourcesByPart.size(); ++part) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[part].payload);
    expect(quad.resource == resourcesByPart[part],
           "class gauge maps to the hard family and exact local node role");
    expect(quad.vertices[0].x == 10.0F + 40.0F * part &&
               quad.vertices[1].x == 50.0F + 40.0F * part,
           "gauge part geometry slices the destination left to right");
  }
}

void testGaugeSegmentSelectionRoundsAtJavaFloatBoundary() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.gaugeResult = {.supported = true,
                       .value = 197.2418975830078,
                       .gaugeType = 0,
                       .minimum = 0.0,
                       .maximum = 198.51992797851562,
                       .border = 0.0};
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {
      {.id = 1,
       .authoredName = "float-boundary-gauge",
       .payload = gaugeObject(resources, SkinGaugeAnimationType::Increase, 466),
       .authoredOrdinal = 1,
       .critical = true}};
  auto presented = destination(1, 119, 10.0);
  presented.presentation.loop = 0;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 466,
         "large valid gauge emits every authored part");
  if (result.submitReady && result.submitReady->commands.size() == 466) {
    const auto &boundary = std::get<SkinTexturedQuadCommand>(
        result.submitReady->commands[462].payload);
    expect(boundary.resource == 104,
           "gauge filled-part truncation uses pinned Java float arithmetic");
  }
}

void testGaugeAnimationUsesStrictDeadlineAndFlickerOverlayOrder() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.gaugeResult = {.supported = true,
                         .value = 60.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    ValidatedBeatorajaSkinModel model;
    auto gauge = gaugeObject(resources, SkinGaugeAnimationType::Decrease, 5);
    gauge.animationRange = 2;
    model.model.objects = {{.id = 1,
                            .authoredName = "animated-gauge",
                            .payload = std::move(gauge),
                            .authoredOrdinal = 1,
                            .critical = true}};
    auto animatedDestination = destination(1, 120, 10.0);
    animatedDestination.presentation.loop = 0;
    model.model.destinations = {std::move(animatedDestination)};

    const auto initial =
        evaluate(renderer, runtime, model, resources, state, 1, 0);
    const auto advanced =
        evaluate(renderer, runtime, model, resources, state, 2, 1'000);
    const auto exactDeadline =
        evaluate(renderer, runtime, model, resources, state, 3, 5'000);
    expect(initial.submitReady && advanced.submitReady &&
               exactDeadline.submitReady,
           "gauge animation frames evaluate across increasing snapshots");
    if (initial.submitReady && advanced.submitReady &&
        exactDeadline.submitReady) {
      const auto roleAt = [](const SkinFrameEvaluationResult &frame,
                             std::size_t part) {
        return std::get<SkinTexturedQuadCommand>(
                   frame.submitReady->commands[part].payload)
            .resource;
      };
      expect(roleAt(initial, 1) == 100 && roleAt(advanced, 1) == 102 &&
                 roleAt(exactDeadline, 1) == 102,
             "Decrease advances by one only when deadline is strictly less");
    }
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.gaugeResult = {.supported = true,
                         .value = 50.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    ValidatedBeatorajaSkinModel model;
    model.model.objects = {
        {.id = 1,
         .authoredName = "flicker-gauge",
         .payload = gaugeObject(resources, SkinGaugeAnimationType::Flicker, 2),
         .authoredOrdinal = 1,
         .critical = true}};
    auto flickerDestination = destination(1, 121, 10.0);
    flickerDestination.presentation.loop = 0;
    model.model.destinations = {std::move(flickerDestination)};
    const auto result =
        evaluate(renderer, runtime, model, resources, state, 1, 1'000);
    expect(result.submitReady && result.submitReady->commands.size() == 3,
           "flicker emits the current-part overlay immediately after its base");
    if (result.submitReady && result.submitReady->commands.size() == 3) {
      const std::array<SkinResourceId, 3> expected = {100, 104, 102};
      for (std::size_t command = 0; command < expected.size(); ++command) {
        expect(
            std::get<SkinTexturedQuadCommand>(
                result.submitReady->commands[command].payload)
                    .resource == expected[command],
            "flicker base, active overlay, and empty node keep pinned order");
      }
    }
  }
}

void testHiddenGaugeStillAdvancesAndDirectDrawIgnoresImageTransforms() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.booleanResult = {.value = false, .supported = true};
    state.gaugeResult = {.supported = true,
                         .value = 60.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    ValidatedBeatorajaSkinModel model;
    model.model.booleanProperties = {
        {.id = SkinBooleanPropertyId{1},
         .source = SkinBuiltinPropertySelector{.value = 93},
         .authoredOrdinal = 1},
        {.id = SkinBooleanPropertyId{2},
         .source = SkinBuiltinPropertySelector{.value = 94},
         .authoredOrdinal = 2}};
    model.model.timerProperties.push_back(
        {.id = SkinTimerPropertyId{1},
         .source = SkinBuiltinPropertySelector{.value = 95},
         .authoredOrdinal = 3});
    auto gauge = gaugeObject(resources, SkinGaugeAnimationType::Decrease, 5);
    gauge.animationRange = 2;
    model.model.objects = {{.id = 1,
                            .authoredName = "hidden-gauge",
                            .payload = std::move(gauge),
                            .authoredOrdinal = 1,
                            .critical = true}};
    auto presented = destination(1, 124, 10.0);
    presented.presentation.loop = 0;
    presented.presentation.conditions.push_back(SkinBooleanPropertyId{1});
    presented.presentation.conditions.push_back(SkinBooleanPropertyId{2});
    presented.presentation.offsetIds.push_back(5);
    presented.presentation.timer = SkinTimerPropertyId{1};
    model.model.destinations = {std::move(presented)};

    const auto hidden =
        evaluate(renderer, runtime, model, resources, state, 1, 1'000);
    state.booleanResult.value = true;
    state.offsets.emplace(
        5, SkinPropertyLookup<ConfigOffset>{.value = {}, .supported = true});
    state.timerResult = 0;
    const auto visible =
        evaluate(renderer, runtime, model, resources, state, 2, 5'000);
    expect(hidden.submitReady && hidden.submitReady->commands.empty() &&
               visible.submitReady && visible.submitReady->commands.size() == 5,
           "condition-hidden gauge advances without emitting commands");
    expect(state.booleanCalls == 3 && state.timerCalls == 2,
           "first false gauge condition skips later conditions, offsets, and "
           "timer before the visible frame");
    if (visible.submitReady && visible.submitReady->commands.size() == 5) {
      expect(std::get<SkinTexturedQuadCommand>(
                 visible.submitReady->commands[1].payload)
                     .resource == 102,
             "visible gauge retains the animation advanced while hidden");
    }
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.gaugeResult = {.supported = true,
                         .value = 100.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    ValidatedBeatorajaSkinModel model;
    model.model.objects = {
        {.id = 1,
         .authoredName = "direct-gauge",
         .payload = gaugeObject(resources, SkinGaugeAnimationType::Increase, 1),
         .authoredOrdinal = 1,
         .critical = true}};
    auto presented = destination(1, 125, 10.0);
    presented.presentation.frames.front().y = 20.0;
    presented.presentation.frames.front().width = 100.0;
    presented.presentation.frames.front().height = 20.0;
    presented.presentation.frames.front().angleDegrees = 90.0;
    presented.presentation.stretch = SkinStretchMode::NoResize;
    presented.presentation.filter = SkinFilterMode::Linear;
    model.model.destinations = {std::move(presented)};

    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady && result.submitReady->commands.size() == 1,
           "single-part direct gauge emits one node");
    if (result.submitReady && result.submitReady->commands.size() == 1) {
      const auto &quad = std::get<SkinTexturedQuadCommand>(
          result.submitReady->commands[0].payload);
      expect(quad.vertices[0].x == 10.0F && quad.vertices[0].y == 700.0F &&
                 quad.vertices[1].x == 110.0F && quad.vertices[2].y == 680.0F,
             "gauge direct draw ignores destination stretch and rotation");
      expect(quad.state.filter == SkinFilterMode::Nearest,
             "gauge direct draw forces nearest filtering");
    }
  }
}

void testGaugeAnimationStateResetsWhenSessionChangesInSameModelStorage() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources firstResources;
  FakeResources secondResources;
  FakeState state;
  state.gaugeResult = {.supported = true,
                       .value = 60.0,
                       .gaugeType = 0,
                       .minimum = 0.0,
                       .maximum = 100.0,
                       .border = 0.0};
  ValidatedBeatorajaSkinModel model;
  ValidatedBeatorajaSkinModel secondModel;
  auto firstGauge =
      gaugeObject(firstResources, SkinGaugeAnimationType::Decrease, 5);
  firstGauge.animationRange = 2;
  auto secondGauge =
      gaugeObject(secondResources, SkinGaugeAnimationType::Decrease, 5);
  secondGauge.animationRange = 2;
  model.model.objects = {{.id = 1,
                          .authoredName = "first-gauge",
                          .payload = std::move(firstGauge),
                          .authoredOrdinal = 1,
                          .critical = true}};
  secondModel.model.objects = {{.id = 1,
                                .authoredName = "second-gauge",
                                .payload = std::move(secondGauge),
                                .authoredOrdinal = 1,
                                .critical = true}};
  auto firstDestination = destination(1, 126, 10.0);
  firstDestination.presentation.loop = 0;
  auto secondDestination = destination(1, 127, 10.0);
  secondDestination.presentation.loop = 0;
  model.model.destinations = {std::move(firstDestination)};
  secondModel.model.destinations = {std::move(secondDestination)};

  const auto advanced =
      evaluate(renderer, runtime, model, firstResources, state, 1, 1'000);
  model = std::move(secondModel);
  const auto reset = evaluate(renderer, runtime, model, secondResources, state,
                              2, 0, nullptr, std::nullopt, nullptr, 2);
  expect(advanced.submitReady && reset.submitReady &&
             reset.submitReady->commands.size() == 5,
         "gauge session transition evaluates with bounded fresh state");
  if (reset.submitReady && reset.submitReady->commands.size() == 5) {
    expect(std::get<SkinTexturedQuadCommand>(
               reset.submitReady->commands[1].payload)
                   .resource == 100,
           "same-address model replacement cannot inherit prior session "
           "animation");
  }
}

void testGaugeRandomUsesBoundedSessionSourceAndFailsAtomically() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.gaugeResult = {.supported = true,
                         .value = 60.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    FakeGaugeRandom random;
    random.value = 3;
    ValidatedBeatorajaSkinModel model;
    model.model.objects = {
        {.id = 1,
         .authoredName = "random-gauge",
         .payload = gaugeObject(resources, SkinGaugeAnimationType::Random, 5),
         .authoredOrdinal = 1,
         .critical = true}};
    auto presented = destination(1, 122, 10.0);
    presented.presentation.loop = 0;
    model.model.destinations = {std::move(presented)};

    const auto result = evaluate(renderer, runtime, model, resources, state, 1,
                                 1'000, nullptr, std::nullopt, &random);
    expect(result.submitReady && random.calls == 1 && random.lastObject == 1 &&
               random.lastEpoch == 1 && random.lastUpperBound == 4,
           "random gauge advances through the bounded session-owned seam");
  }
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.gaugeResult = {.supported = true,
                         .value = 60.0,
                         .gaugeType = 0,
                         .minimum = 0.0,
                         .maximum = 100.0,
                         .border = 0.0};
    FakeGaugeRandom random;
    random.value = 4;
    ValidatedBeatorajaSkinModel model;
    model.model.objects = {
        {.id = 1,
         .authoredName = "invalid-random-gauge",
         .payload = gaugeObject(resources, SkinGaugeAnimationType::Random, 5),
         .authoredOrdinal = 1,
         .critical = true}};
    auto presented = destination(1, 123, 10.0);
    presented.presentation.loop = 0;
    model.model.destinations = {std::move(presented)};

    const auto result = evaluate(renderer, runtime, model, resources, state, 1,
                                 1'000, nullptr, std::nullopt, &random);
    expect(!result.submitReady &&
               hasDiagnostic(result, "skin.renderer.gauge.random"),
           "out-of-range random gauge result discards a critical frame");
  }
}

void testJudgeLowersRelativeComboBeforeShiftedImage() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(20, {.x = 0, .y = 0, .w = 10, .h = 10}, 20, 10);
  const auto digits = glyphRegions(10);
  resources.addImageAtlas(21, digits, 100, 20);
  FakeState state;
  state.judgeResult = {.supported = true,
                       .optionalZeroBasedGrade = 1,
                       .combo = 42,
                       .maximumGauge = false};

  auto childImage = imageObject(2, 20, false);
  SkinNumberObject childNumber;
  childNumber.digits.positive = glyphSprite(21, digits);
  childNumber.digits.glyphsPerAnimationFrame = 10;
  childNumber.value = SkinIntegerPropertyId{999};
  childNumber.digitCount = 3;
  childNumber.spacing = 2;
  childNumber.alignment = 2;
  childNumber.relativeToJudgeImage = true;

  SkinDestinationBody imageDestination;
  imageDestination.loop = -1;
  imageDestination.frames = {
      {.timeMillis = 0,
       .x = 100.0,
       .y = 200.0,
       .width = 40.0,
       .height = 20.0,
       .clip = SkinSourceRect{.x = 1, .y = 2, .w = 3, .h = 4}}};
  SkinDestinationBody numberDestination;
  numberDestination.loop = -1;
  numberDestination.frames = {
      {.timeMillis = 0,
       .x = -15.0,
       .y = 0.0,
       .width = 10.0,
       .height = 20.0,
       .clip = SkinSourceRect{.x = 5, .y = 6, .w = 7, .h = 8}}};

  SkinJudgeObject judge;
  judge.player = 2;
  judge.shiftImageByHalfDetailWidth = true;
  judge.grades.resize(7);
  judge.grades[1].image = SkinNestedObjectPresentation{
      .object = 2, .destination = imageDestination};
  judge.grades[1].detailNumber = SkinNestedObjectPresentation{
      .object = 3, .destination = numberDestination};

  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "judge",
                          .payload = std::move(judge),
                          .authoredOrdinal = 1,
                          .critical = true},
                         std::move(childImage),
                         {.id = 3,
                          .authoredName = "judge-number",
                          .payload = std::move(childNumber),
                          .authoredOrdinal = 3,
                          .critical = false}};
  auto outerDestination = destination(1, 50, 0.0);
  outerDestination.presentation.frames.front().clip =
      SkinSourceRect{.x = 80, .y = 190, .w = 100, .h = 50};
  model.model.destinations = {std::move(outerDestination)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 3,
         "judge emits two combo digits before its selected image");
  if (!result.submitReady || result.submitReady->commands.size() != 3) {
    return;
  }
  expect(result.submitReady->commands[0].sourceObject == 3 &&
             result.submitReady->commands[1].sourceObject == 3 &&
             result.submitReady->commands[2].sourceObject == 2,
         "judge detail commands precede the selected image command");
  expect(std::ranges::all_of(result.submitReady->commands,
                             [](const SkinDrawCommand &command) {
                               return command.authoredOrdinal == 50;
                             }),
         "nested judge commands retain the outer authored ordinal");
  expect(std::ranges::all_of(
             result.submitReady->commands,
             [](const SkinDrawCommand &command) {
               const auto &state =
                   std::get<SkinTexturedQuadCommand>(command.payload).state;
               return state.scissor && state.scissor->x == 80.0 &&
                      state.scissor->y == 480.0 &&
                      state.scissor->width == 100.0 &&
                      state.scissor->height == 50.0;
             }),
         "outer judge clip scopes detail and image while nested clips are "
         "ignored");
  const auto &four = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &two = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  const auto &image = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[2].payload);
  expect(four.vertices[0].u == 0.4F && two.vertices[0].u == 0.2F,
         "judge detail formats the projected combo without reading its ref");
  expect(four.vertices[0].x == 91.0F && two.vertices[0].x == 103.0F &&
             four.vertices[0].y == 520.0F,
         "judge detail destination is relative to the selected image region");
  expect(image.vertices[0].x == 88.0F,
         "judge image shifts left by half the rendered detail width");
}

void testJudgeMaxGaugeFallsBackImageAndDetailIndependently() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(20, {.x = 0, .y = 0, .w = 10, .h = 10}, 20, 10);
  resources.addImage(22, {.x = 0, .y = 0, .w = 10, .h = 10}, 20, 10);
  const auto digits = glyphRegions(10);
  resources.addImageAtlas(21, digits, 100, 20);
  FakeState state;
  state.judgeResult = {.supported = true,
                       .optionalZeroBasedGrade = 0,
                       .combo = 7,
                       .maximumGauge = true};

  auto gradeZeroImage = imageObject(2, 20, false);
  auto gradeSixImage = imageObject(4, 22, false);
  SkinNumberObject gradeZeroNumber;
  gradeZeroNumber.digits.positive = glyphSprite(21, digits);
  gradeZeroNumber.digits.glyphsPerAnimationFrame = 10;
  gradeZeroNumber.value = SkinIntegerPropertyId{999};
  gradeZeroNumber.digitCount = 1;
  gradeZeroNumber.relativeToJudgeImage = true;

  SkinDestinationBody imageDestination;
  imageDestination.loop = -1;
  imageDestination.frames = {
      {.timeMillis = 0, .x = 100.0, .y = 200.0, .width = 40.0, .height = 20.0}};
  SkinDestinationBody detailDestination;
  detailDestination.loop = -1;
  detailDestination.frames = {
      {.timeMillis = 0, .x = 0.0, .y = 0.0, .width = 10.0, .height = 20.0}};
  detailDestination.offsetIds = {5};

  SkinJudgeObject judge;
  judge.grades.resize(7);
  judge.grades[0].image = SkinNestedObjectPresentation{
      .object = 2, .destination = imageDestination};
  judge.grades[0].detailNumber = SkinNestedObjectPresentation{
      .object = 3, .destination = detailDestination};
  judge.grades[6].image = SkinNestedObjectPresentation{
      .object = 4, .destination = imageDestination};

  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "max-judge",
                          .payload = std::move(judge),
                          .authoredOrdinal = 1,
                          .critical = true},
                         std::move(gradeZeroImage),
                         {.id = 3,
                          .authoredName = "max-judge-number",
                          .payload = std::move(gradeZeroNumber),
                          .authoredOrdinal = 3,
                          .critical = false},
                         std::move(gradeSixImage)};
  model.model.destinations = {destination(1, 60, 0.0)};

  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(
      5, ConfigOffset{.x = 50, .y = 60, .w = 4, .h = 6});
  const auto result = evaluate(renderer, runtime, model, resources, state, 1, 0,
                               &configuration);
  expect(result.submitReady && result.submitReady->commands.size() == 2,
         "max-gauge judge combines fallback detail with grade-six image");
  if (!result.submitReady || result.submitReady->commands.size() != 2) {
    return;
  }
  const auto &detail = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &image = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  expect(result.submitReady->commands[0].sourceObject == 3 &&
             detail.vertices[0].u == 0.7F,
         "missing grade-six detail independently falls back to grade zero");
  expect(detail.vertices[0].x == 100.0F && detail.vertices[0].y == 520.0F &&
             detail.vertices[2].x == 114.0F && detail.vertices[2].y == 494.0F,
         "relative judge detail offsets resize without translating its origin");
  expect(result.submitReady->commands[1].sourceObject == 4 &&
             image.resource == 22,
         "available grade-six image is selected at maximum gauge");
}

void testHiddenJudgeStillPreparesChildrenInPinnedCallbackOrder() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  resources.addImage(20, {.x = 0, .y = 0, .w = 10, .h = 10});
  FakeState state;
  state.booleanResult = {.value = true, .supported = true};
  state.timerResult = 0;
  state.offsets.emplace(
      5, SkinPropertyLookup<ConfigOffset>{.value = {}, .supported = true});
  state.judgeResult = {.supported = true,
                       .optionalZeroBasedGrade = 0,
                       .combo = 0,
                       .maximumGauge = false};

  auto childImage = imageObject(2, 20, false);
  auto &sprite =
      std::get<SkinImageObject>(childImage.payload).orderedStates.front();
  sprite.cycleMillis = 100;
  sprite.timer = SkinTimerPropertyId{2};
  SkinDestinationBody childDestination;
  childDestination.loop = 0;
  childDestination.conditions = {SkinBooleanPropertyId{1},
                                 SkinBooleanPropertyId{2}};
  childDestination.timer = SkinTimerPropertyId{1};
  childDestination.offsetIds = {5};
  childDestination.frames = {
      {.timeMillis = 0, .x = 100.0, .y = 200.0, .width = 40.0, .height = 20.0}};
  SkinJudgeObject judge;
  judge.grades.resize(7);
  judge.grades[0].image = SkinNestedObjectPresentation{
      .object = 2, .destination = childDestination};

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 10},
       .authoredOrdinal = 1},
      {.id = SkinBooleanPropertyId{2},
       .source = SkinBuiltinPropertySelector{.value = 11},
       .authoredOrdinal = 2},
      {.id = SkinBooleanPropertyId{3},
       .source = runtime.falseCallback(),
       .authoredOrdinal = 3}};
  model.model.timerProperties = {
      {.id = SkinTimerPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 20},
       .authoredOrdinal = 3},
      {.id = SkinTimerPropertyId{2},
       .source = SkinBuiltinPropertySelector{.value = 21},
       .authoredOrdinal = 4}};
  model.model.objects = {{.id = 1,
                          .authoredName = "hidden-judge",
                          .payload = std::move(judge),
                          .authoredOrdinal = 1,
                          .critical = true},
                         std::move(childImage)};
  auto outerDestination = destination(1, 61, 0.0);
  outerDestination.presentation.loop = 0;
  outerDestination.presentation.conditions.push_back(SkinBooleanPropertyId{3});
  model.model.destinations = {std::move(outerDestination)};

  const auto hidden =
      evaluate(renderer, runtime, model, resources, state, 1, 1'000);
  expect(hidden.submitReady && hidden.submitReady->commands.empty() &&
             state.accessOrder == std::vector<int>({1'010, 1'011, 2'020, 2'020,
                                                    3'005, 2'021, 2'021}),
         "hidden outer judge still prepares child conditions, destination "
         "timer, offset, then source timer");

  state.accessOrder.clear();
  state.booleanResult.value = false;
  const auto childHidden =
      evaluate(renderer, runtime, model, resources, state, 2, 2'000);
  expect(childHidden.submitReady && childHidden.submitReady->commands.empty() &&
             state.accessOrder == std::vector<int>({1'010, 2'021, 2'021}),
         "false first child condition skips later condition, timer, offset, "
         "but still selects the nested image source frame");

  state.accessOrder.clear();
  state.judgeResult.supported = false;
  model.model.destinations.front().presentation.conditions = {999};
  const auto optionHidden =
      evaluate(renderer, runtime, model, resources, state, 3, 3'000);
  expect(optionHidden.submitReady &&
             optionHidden.submitReady->commands.empty() &&
             optionHidden.diagnostics.empty() && state.accessOrder.empty(),
         "false configured option removes Judge before state and child "
         "preparation");
}

void testCriticalJudgeRequiresSupportedState() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  SkinJudgeObject judge;
  judge.grades.resize(7);
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "judge",
                          .payload = std::move(judge),
                          .authoredOrdinal = 1,
                          .critical = true}};
  model.model.destinations = {destination(1, 70, 0.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(!result.submitReady &&
             hasDiagnostic(result, "skin.renderer.judge.state"),
         "unsupported critical judge state discards the frame");
}

SkinSpriteFrames singleFrameSprite(SkinResourceId resource) {
  return {.resource = resource, .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}};
}

SkinNoteObject gameplayNoteObject(FakeResources &resources,
                                  std::size_t groupCount = 1) {
  SkinNoteObject note;
  SkinLaneNotePresentation lane;
  lane.authoredLane = 0;
  lane.laneDestination = {
      .x = 100.0, .y = 200.0, .width = 40.0, .height = 300.0};
  lane.authoredNoteHeight = 10.0;
  for (int kind = static_cast<int>(SkinNoteVisualKind::Normal);
       kind <= static_cast<int>(SkinNoteVisualKind::HcnReactive); ++kind) {
    const auto visualKind = static_cast<SkinNoteVisualKind>(kind);
    const SkinResourceId resource = 100 + static_cast<SkinResourceId>(kind);
    const auto sprite = singleFrameSprite(resource);
    resources.addImage(resource, sprite.frames.front());
    lane.visuals.emplace(visualKind, sprite);
  }
  note.lanes.push_back(std::move(lane));

  for (int kind = static_cast<int>(SkinNoteLineKind::Group);
       kind <= static_cast<int>(SkinNoteLineKind::Time); ++kind) {
    for (std::size_t group = 0; group < groupCount; ++group) {
      const SkinResourceId resource =
          200 + static_cast<SkinResourceId>(kind) + group * 10;
      auto sprite = singleFrameSprite(resource);
      resources.addImage(resource, sprite.frames.front());
      SkinDestinationBody lineDestination;
      lineDestination.loop = -1;
      lineDestination.frames = {
          {.timeMillis = 0,
           .x = 50.0 + static_cast<double>(group) * 100.0,
           .y = 60.0,
           .width = 80.0,
           .height = 4.0,
           .clip = SkinSourceRect{.x = 60, .y = 61, .w = 1, .h = 1}}};
      note.lines.push_back(
          {.kind = static_cast<SkinNoteLineKind>(kind),
           .sprite = std::move(sprite),
           .laneGroupDestination = {.x = 50.0 +
                                         static_cast<double>(group) * 100.0,
                                    .y = 60.0,
                                    .width = 80.0,
                                    .height = 4.0},
           .destination = std::move(lineDestination)});
    }
  }
  return note;
}

void testNoteLongNoteAndLineCommandsPreserveMergedProjectionOrder() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.notes = {
      {.visualId = 10,
       .lane = 0,
       .kind = SkinProjectedNoteKind::Normal,
       .authoredYDisplacement = 30.0,
       .judged = false,
       .submissionOrdinal = 2},
      {.visualId = 11,
       .lane = 0,
       .kind = SkinProjectedNoteKind::Invisible,
       .authoredYDisplacement = 50.0,
       .judged = false,
       .submissionOrdinal = 4},
      {.visualId = 12,
       .lane = 0,
       .kind = SkinProjectedNoteKind::Mine,
       .authoredYDisplacement = 80.0,
       .judged = false,
       .submissionOrdinal = 7},
      {.visualId = 13,
       .lane = 0,
       .kind = SkinProjectedNoteKind::Normal,
       .authoredYDisplacement = 90.0,
       .judged = true,
       .submissionOrdinal = 9},
  };
  state.longNotes = {
      {.headVisualId = 20,
       .tailVisualId = 21,
       .lane = 0,
       .mode = SkinProjectedLongNoteMode::LN,
       .headAuthoredYDisplacement = 40.0,
       .tailAuthoredYDisplacement = 100.0,
       .active = false,
       .submissionOrdinal = 3},
      {.headVisualId = 22,
       .tailVisualId = 23,
       .lane = 0,
       .mode = SkinProjectedLongNoteMode::CN,
       .headAuthoredYDisplacement = 70.0,
       .tailAuthoredYDisplacement = 140.0,
       .active = true,
       .submissionOrdinal = 6},
      {.headVisualId = 24,
       .tailVisualId = 25,
       .lane = 0,
       .mode = SkinProjectedLongNoteMode::HCN,
       .headAuthoredYDisplacement = 90.0,
       .tailAuthoredYDisplacement = 160.0,
       .reactive = true,
       .submissionOrdinal = 8},
  };
  state.lines = {
      {.timelineVisualId = 0,
       .kind = SkinProjectedLineKind::Group,
       .authoredYDisplacement = 20.0,
       .submissionOrdinal = 1},
      {.timelineVisualId = 0,
       .kind = SkinProjectedLineKind::Bpm,
       .authoredYDisplacement = 60.0,
       .submissionOrdinal = 5},
      {.timelineVisualId = 0,
       .kind = SkinProjectedLineKind::Stop,
       .authoredYDisplacement = 100.0,
       .submissionOrdinal = 10},
      {.timelineVisualId = 0,
       .kind = SkinProjectedLineKind::Time,
       .authoredYDisplacement = 120.0,
       .submissionOrdinal = 11},
  };

  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "notes",
                          .payload = gameplayNoteObject(resources),
                          .authoredOrdinal = 1,
                          .critical = true}};
  auto presented = destination(1, 77, 0.0);
  presented.presentation.offsetIds = {7};
  // JsonPlaySkinObjectLoader builds SkinNote's drawable lane geometry from
  // note.dst. The containing destination may intentionally omit dst frames,
  // as simple-play-simple does for its `notes` entry.
  presented.presentation.frames.clear();
  model.model.destinations = {std::move(presented)};
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(
      7, ConfigOffset{.x = 3, .y = 4, .w = 5, .h = 6});

  const auto result = evaluate(renderer, runtime, model, resources, state, 1, 0,
                               &configuration);
  expect(result.submitReady && result.submitReady->commands.size() == 16,
         "merged note projection emits every line, note, body, and cap");
  if (!result.submitReady || result.submitReady->commands.size() != 16) {
    return;
  }
  const std::vector<SkinResourceId> expectedResources = {
      200, 100, 107, 105, 102, 201, 106, 104,
      105, 101, 112, 108, 109, 103, 202, 203};
  std::vector<SkinResourceId> actualResources;
  for (const auto &command : result.submitReady->commands) {
    actualResources.push_back(
        std::get<SkinTexturedQuadCommand>(command.payload).resource);
  }
  expect(actualResources == expectedResources,
         "projection merge and pinned long-note body/cap order are exact");
  expect(std::ranges::all_of(result.submitReady->commands,
                             [](const SkinDrawCommand &command) {
                               return command.authoredOrdinal == 77;
                             }),
         "all projected commands retain their containing destination order");

  const auto &normal = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  const auto &invisible = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[4].payload);
  expect(normal.vertices[0].x == 103.0F && normal.vertices[0].y == 486.0F &&
             normal.vertices[2].x == 148.0F && normal.vertices[2].y == 470.0F,
         "normal note composes wrapper x/y/width/height offsets");
  expect(invisible.vertices[0].x == 100.0F &&
             invisible.vertices[0].y == 470.0F &&
             invisible.vertices[2].x == 140.0F &&
             invisible.vertices[2].y == 460.0F,
         "invisible notes preserve pinned offset-free lane geometry");
  const auto &groupLine = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands.front().payload);
  expect(groupLine.vertices[0].x == 50.0F && groupLine.vertices[0].y == 640.0F,
         "timeline displacement offsets its nested line destination only");
}

void testLiveScrollUnitsUseSkinLaneHeightAndHispeed() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.notes = {{.visualId = 10,
                  .lane = 0,
                  .kind = SkinProjectedNoteKind::Normal,
                  .authoredYDisplacement = 0.0,
                  .submissionOrdinal = 1}};
  state.lines = {{.timelineVisualId = 20,
                  .kind = SkinProjectedLineKind::Group,
                  .authoredYDisplacement = 0.0,
                  .submissionOrdinal = 2}};

  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "skin-scroll-units",
                          .payload = gameplayNoteObject(resources),
                          .authoredOrdinal = 1,
                          .critical = true}};
  auto presented = destination(1, 1, 0.0);
  presented.presentation.frames.clear();
  model.model.destinations = {std::move(presented)};

  const auto legacy = evaluate(renderer, runtime, model, resources, state);
  state.notes.front().scrollSpeed = 2.0;
  state.notes.front().authoredYDisplacement = 0.25;
  state.lines.front().scrollSpeed = 2.0;
  state.lines.front().authoredYDisplacement = 0.25;
  state.longNotes = {{.headVisualId = 30,
                      .tailVisualId = 31,
                      .lane = 0,
                      .mode = SkinProjectedLongNoteMode::CN,
                      .scrollSpeed = 2.0,
                      .headAuthoredYDisplacement = 0.25,
                      .tailAuthoredYDisplacement = 0.5,
                      .submissionOrdinal = 3}};
  const auto scaled = evaluate(renderer, runtime, model, resources, state, 2);

  const auto vertexY = [](const SkinFrameEvaluationResult &result,
                          SkinResourceId resource) -> std::optional<float> {
    if (!result.submitReady) {
      return std::nullopt;
    }
    for (const auto &command : result.submitReady->commands) {
      const auto &quad = std::get<SkinTexturedQuadCommand>(command.payload);
      if (quad.resource == resource) {
        return quad.vertices[0].y;
      }
    }
    return std::nullopt;
  };
  const auto legacyNoteY = vertexY(legacy, 100);
  const auto legacyLineY = vertexY(legacy, 200);
  const auto scaledNoteY = vertexY(scaled, 100);
  const auto scaledLineY = vertexY(scaled, 200);
  const auto longBodyY = vertexY(
      scaled, 100 + static_cast<SkinResourceId>(SkinNoteVisualKind::LnBodyInactive));
  // Beatoraja LaneRenderer uses (note.dst lane height * hispeed) as rxhs.
  // A 0.25 scroll delta with a 300px lane and hispeed 2 must therefore travel
  // 150 authored pixels for both notes and measure lines.
  expect(legacyNoteY && scaledNoteY &&
             *legacyNoteY - *scaledNoteY == 150.0F,
         "live note scroll units are scaled by the skin lane height and hispeed");
  expect(legacyLineY && scaledLineY &&
             *legacyLineY - *scaledLineY == 150.0F,
         "live measure-line scroll units are scaled by the skin lane height and hispeed");
  expect(scaled.submitReady && scaled.submitReady->commands.size() == 5 &&
             longBodyY.has_value(),
         "long-note extent uses the same skin lane height and hispeed");
}

void testEveryLongNoteBodyStateUsesItsDistinctVisualRole() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  const auto addLongNote = [&](SkinProjectedLongNoteMode mode, bool active,
                               bool damaged, bool reactive,
                               std::uint32_t ordinal) {
    state.longNotes.push_back(
        {.headVisualId = ordinal * 2,
         .tailVisualId = ordinal * 2 + 1,
         .lane = 0,
         .mode = mode,
         .headAuthoredYDisplacement = ordinal * 20.0,
         .tailAuthoredYDisplacement = ordinal * 20.0 + 40.0,
         .active = active,
         .damaged = damaged,
         .reactive = reactive,
         .submissionOrdinal = ordinal});
  };
  addLongNote(SkinProjectedLongNoteMode::LN, true, false, false, 1);
  addLongNote(SkinProjectedLongNoteMode::CN, false, false, false, 2);
  addLongNote(SkinProjectedLongNoteMode::HCN, false, false, false, 3);
  addLongNote(SkinProjectedLongNoteMode::HCN, true, false, false, 4);
  addLongNote(SkinProjectedLongNoteMode::HCN, false, true, false, 5);
  addLongNote(SkinProjectedLongNoteMode::HCN, false, false, true, 6);

  auto note = gameplayNoteObject(resources);
  note.hcnBodySlotLayout = SkinHcnBodySlotLayout::Modern;
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "long-note-roles",
                          .payload = std::move(note),
                          .authoredOrdinal = 1,
                          .critical = true}};
  model.model.destinations = {destination(1, 88, 0.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 17,
         "all long-note modes and states emit pinned body/cap counts");
  if (!result.submitReady || result.submitReady->commands.size() != 17) {
    return;
  }
  const std::vector<SkinResourceId> expectedResources = {
      106, 105, 107, 104, 105, 111, 108, 109, 110,
      108, 109, 112, 108, 109, 113, 108, 109};
  std::vector<SkinResourceId> actualResources;
  for (const auto &command : result.submitReady->commands) {
    actualResources.push_back(
        std::get<SkinTexturedQuadCommand>(command.payload).resource);
  }
  expect(actualResources == expectedResources,
         "LN active/inactive and HCN inactive/active/damage/reactive roles "
         "remain distinct");
}

void testProjectedLineEmitsEveryLaneGroupAndIgnoresNestedClip() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.lines = {
      {.timelineVisualId = 400,
       .kind = SkinProjectedLineKind::Group,
       .authoredYDisplacement = 10.0,
       .submissionOrdinal = 1},
      {.timelineVisualId = 401,
       .kind = SkinProjectedLineKind::Bpm,
       .authoredYDisplacement = 20.0,
       .submissionOrdinal = 2},
      {.timelineVisualId = 402,
       .kind = SkinProjectedLineKind::Stop,
       .authoredYDisplacement = 30.0,
       .submissionOrdinal = 3},
      {.timelineVisualId = 403,
       .kind = SkinProjectedLineKind::Time,
       .authoredYDisplacement = 40.0,
       .submissionOrdinal = 4},
  };
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "two-group-lines",
                          .payload = gameplayNoteObject(resources, 2),
                          .authoredOrdinal = 1,
                          .critical = true}};
  auto presented = destination(1, 95, 0.0);
  presented.presentation.frames.front().clip =
      SkinSourceRect{.x = 20, .y = 30, .w = 500, .h = 400};
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 8,
         "each projected timeline event emits all lane-group presentations");
  if (!result.submitReady || result.submitReady->commands.size() != 8) {
    return;
  }
  const std::vector<SkinResourceId> expectedResources = {200, 210, 201, 211,
                                                         202, 212, 203, 213};
  std::vector<SkinResourceId> actualResources;
  for (const auto &command : result.submitReady->commands) {
    const auto &quad = std::get<SkinTexturedQuadCommand>(command.payload);
    actualResources.push_back(quad.resource);
    expect(quad.state.scissor && quad.state.scissor->x == 20.0 &&
               quad.state.scissor->y == 290.0 &&
               quad.state.scissor->width == 500.0 &&
               quad.state.scissor->height == 400.0,
           "nested line clip is inert while the outer Note clip remains");
  }
  expect(actualResources == expectedResources,
         "line groups remain increasing inside each projected kind");

  auto &note = std::get<SkinNoteObject>(model.model.objects.front().payload);
  note.lines[3].sprite.reset();
  state.lines = {{.timelineVisualId = 404,
                  .kind = SkinProjectedLineKind::Bpm,
                  .submissionOrdinal = 1}};
  const auto sparse = evaluate(renderer, runtime, model, resources, state, 2);
  expect(!sparse.submitReady &&
             hasDiagnostic(sparse, "skin.renderer.note.line"),
         "a projected sparse line hole fails the critical note atomically");
}

void testSynthesizedNoteFallbacksEmitBoundedColoredPrimitives() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.notes = {
      {.lane = 0,
       .kind = SkinProjectedNoteKind::Normal,
       .submissionOrdinal = 1},
      {.lane = 0,
       .kind = SkinProjectedNoteKind::Invisible,
       .submissionOrdinal = 2},
      {.lane = 0, .kind = SkinProjectedNoteKind::Mine, .submissionOrdinal = 3},
      {.lane = 0,
       .kind = SkinProjectedNoteKind::Normal,
       .judged = true,
       .submissionOrdinal = 4},
  };

  SkinNoteObject note = gameplayNoteObject(resources);
  for (auto &[kind, visual] : note.lanes.front().visuals) {
    visual = SkinSynthesizedNoteVisual{.kind = kind};
  }
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "fallback-notes",
                          .payload = std::move(note),
                          .authoredOrdinal = 1,
                          .critical = true}};
  model.model.destinations = {destination(1, 89, 0.0)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 18,
         "solid and double-outline note fallbacks emit fixed-width strips");
  if (!result.submitReady || result.submitReady->commands.size() != 18) {
    return;
  }
  const auto &normal =
      std::get<SkinPrimitiveCommand>(result.submitReady->commands[0].payload);
  const auto &hiddenOuterBottom =
      std::get<SkinPrimitiveCommand>(result.submitReady->commands[1].payload);
  const auto &hiddenInnerBottom =
      std::get<SkinPrimitiveCommand>(result.submitReady->commands[5].payload);
  const auto &mine =
      std::get<SkinPrimitiveCommand>(result.submitReady->commands[9].payload);
  const auto &processedOuter =
      std::get<SkinPrimitiveCommand>(result.submitReady->commands[10].payload);
  expect(normal.kind == SkinPrimitiveKind::SolidQuad &&
             normal.vertices.size() == 4 &&
             normal.vertices.front().rgba == 0xffffffffU &&
             hiddenOuterBottom.kind == SkinPrimitiveKind::SolidQuad &&
             hiddenOuterBottom.vertices.size() == 4 &&
             hiddenOuterBottom.vertices.front().rgba == 0xff007fffU &&
             hiddenInnerBottom.vertices.size() == 4 &&
             mine.vertices.front().rgba == 0xff0000ffU &&
             processedOuter.vertices.front().rgba == 0xffffff00U,
         "fallback shapes, vertex bounds, and pinned colors are explicit");
  expect(hiddenOuterBottom.vertices[0].x == 100.0F &&
             hiddenOuterBottom.vertices[0].y == 520.0F &&
             hiddenOuterBottom.vertices[2].x == 140.0F &&
             hiddenOuterBottom.vertices[2].y == 518.75F &&
             hiddenInnerBottom.vertices[0].x == 101.25F &&
             hiddenInnerBottom.vertices[0].y == 518.75F,
         "double-outline fallback scales its 32x8 source-pixel thickness");
}

void testHiddenAndLiftCoversApplyDisappearLineClipping() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  auto hiddenSprite = singleFrameSprite(300);
  auto liftSprite = singleFrameSprite(301);
  resources.addImage(300, hiddenSprite.frames.front());
  resources.addImage(301, liftSprite.frames.front());

  ValidatedBeatorajaSkinModel model;
  model.model.objects = {
      {.id = 1,
       .authoredName = "hidden-cover",
       .payload = SkinCoverObject{.kind = SkinCoverKind::Hidden,
                                  .sprite = std::move(hiddenSprite),
                                  .disappearLine = 150.0,
                                  .disappearLineLinksLift = true},
       .authoredOrdinal = 1,
       .critical = true},
      {.id = 2,
       .authoredName = "lift-cover",
       .payload = SkinCoverObject{.kind = SkinCoverKind::Lift,
                                  .sprite = std::move(liftSprite),
                                  .disappearLine = 150.0,
                                  .disappearLineLinksLift = false},
       .authoredOrdinal = 2,
       .critical = true}};
  auto hiddenDestination = destination(1, 90, 100.0);
  hiddenDestination.presentation.frames.front().y = 100.0;
  hiddenDestination.presentation.frames.front().width = 100.0;
  hiddenDestination.presentation.frames.front().height = 100.0;
  hiddenDestination.presentation.offsetIds = {3, 5};
  auto liftDestination = destination(2, 91, 300.0);
  liftDestination.presentation.frames.front().y = 100.0;
  liftDestination.presentation.frames.front().width = 100.0;
  liftDestination.presentation.frames.front().height = 100.0;
  liftDestination.presentation.offsetIds = {3};
  model.model.destinations = {std::move(hiddenDestination),
                              std::move(liftDestination)};
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(3, ConfigOffset{.y = 10});
  configuration.offsetsById.emplace(5, ConfigOffset{.y = 20});

  const auto result = evaluate(renderer, runtime, model, resources, state, 1, 0,
                               &configuration);
  expect(result.submitReady && result.submitReady->commands.size() == 2,
         "hidden and lift covers emit after disappear-line clipping");
  if (!result.submitReady || result.submitReady->commands.size() != 2) {
    return;
  }
  const auto &hidden = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[0].payload);
  const auto &lift = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands[1].payload);
  expect(hidden.state.scissor && hidden.state.scissor->x == 100.0 &&
             hidden.state.scissor->y == 490.0 &&
             hidden.state.scissor->width == 100.0 &&
             hidden.state.scissor->height == 70.0,
         "hidden cover links its disappear line to Lift after offsets");
  expect(lift.state.scissor && lift.state.scissor->x == 300.0 &&
             lift.state.scissor->y == 510.0 &&
             lift.state.scissor->width == 100.0 &&
             lift.state.scissor->height == 60.0,
         "lift cover uses its fixed authored disappear line");
}

void testHiddenCoverStillSelectsSourceAfterRuntimeSuppression() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.booleanResult = {.value = false, .supported = true};
  state.timerResult = 0;
  auto sprite = singleFrameSprite(302);
  sprite.cycleMillis = 100;
  sprite.timer = SkinTimerPropertyId{1};
  resources.addImage(302, sprite.frames.front());

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 41},
       .authoredOrdinal = 1}};
  model.model.timerProperties = {
      {.id = SkinTimerPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 42},
       .authoredOrdinal = 2}};
  model.model.objects = {
      {.id = 1,
       .authoredName = "hidden-cover-prepare",
       .payload = SkinCoverObject{.kind = SkinCoverKind::Lift,
                                  .sprite = std::move(sprite),
                                  .disappearLine = -1.0,
                                  .disappearLineLinksLift = false},
       .authoredOrdinal = 1,
       .critical = true}};
  auto presented = destination(1, 97, 100.0);
  presented.presentation.conditions = {SkinBooleanPropertyId{1}};
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.empty() &&
             state.accessOrder == std::vector<int>({1'041, 2'042, 2'042}),
         "runtime-hidden cover still probes and reads its animated source");
}

void testBgaEmitsOneRoleFreePreStretchCommand() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "bga",
                          .payload = SkinBgaObject{},
                          .authoredOrdinal = 1,
                          .critical = true}};
  auto presented = destination(1, 92, 123.0);
  presented.presentation.frames.front().y = 45.0;
  presented.presentation.frames.front().width = 640.0;
  presented.presentation.frames.front().height = 360.0;
  presented.presentation.frames.front().rgba = {1, 2, 3, 4};
  presented.presentation.stretch = SkinStretchMode::KeepAspectRatioFitInner;
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  expect(result.submitReady && result.submitReady->commands.size() == 1 &&
             result.submitReady->adjacentBatches.size() == 1,
         "BGA emits exactly one unbatchable compositor command");
  if (!result.submitReady || result.submitReady->commands.size() != 1) {
    return;
  }
  const auto *bga = std::get_if<SkinBgaCommand>(
      &result.submitReady->commands.front().payload);
  expect(bga && bga->authoredOrdinal == 92 &&
             bga->authoredGeometry.rect.x == 123.0 &&
             bga->authoredGeometry.rect.y == 45.0 &&
             bga->authoredGeometry.rect.width == 640.0 &&
             bga->authoredGeometry.rect.height == 360.0 &&
             bga->authoredGeometry.stretch ==
                 SkinStretchMode::KeepAspectRatioFitInner &&
             bga->viewport.valid,
         "BGA preserves evaluated authored geometry, stretch, and viewport");
}

void testTimersProbeThenReadAndTruncateMillisecondsIndependently() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.timerSequence = {0, 1'999};

  SkinSpriteFrames sprite;
  sprite.resource = 400;
  sprite.frames = {{.x = 0, .y = 0, .w = 10, .h = 10}};
  resources.addImage(400, sprite.frames.front());
  SkinImageObject image;
  image.orderedStates = {sprite};
  ValidatedBeatorajaSkinModel model;
  model.model.timerProperties = {
      {.id = SkinTimerPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 31},
       .authoredOrdinal = 1}};
  model.model.objects = {{.id = 1,
                          .authoredName = "two-call-timer",
                          .payload = std::move(image),
                          .authoredOrdinal = 1,
                          .critical = true}};
  SkinDestinationBody body;
  body.loop = -1;
  body.timer = SkinTimerPropertyId{1};
  body.authoredOrdinal = 93;
  body.frames = {
      {.timeMillis = 0, .x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0},
      {.timeMillis = 1, .x = 50.0, .y = 20.0, .width = 30.0, .height = 40.0}};
  model.model.destinations = {{.object = 1, .presentation = std::move(body)}};

  const auto result =
      evaluate(renderer, runtime, model, resources, state, 1, 2'000);
  expect(result.submitReady && result.submitReady->commands.size() == 1 &&
             state.timerCalls == 2,
         "destination timer performs an OFF probe and an independent read");
  if (!result.submitReady || result.submitReady->commands.empty()) {
    return;
  }
  const auto &quad = std::get<SkinTexturedQuadCommand>(
      result.submitReady->commands.front().payload);
  expect(quad.vertices[0].x == 50.0F,
         "timer subtraction truncates each microsecond value to milliseconds "
         "before subtraction");
}

void testTimerProbeOnKeepsIndependentSentinelValueReadActive() {
  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.timerSequence = {0, INT64_MIN};
    resources.addImage(401, {.x = 0, .y = 0, .w = 10, .h = 10});
    auto object = imageObject(1, 401, true);
    ValidatedBeatorajaSkinModel model;
    model.model.timerProperties = {
        {.id = SkinTimerPropertyId{1},
         .source = SkinBuiltinPropertySelector{.value = 32},
         .authoredOrdinal = 1}};
    model.model.objects = {std::move(object)};
    auto presented = destination(1, 98, 10.0);
    presented.presentation.timer = SkinTimerPropertyId{1};
    presented.presentation.loop = 0;
    presented.presentation.frames.push_back({.timeMillis = 100,
                                             .x = 110.0,
                                             .y = 20.0,
                                             .width = 40.0,
                                             .height = 30.0});
    model.model.destinations = {std::move(presented)};

    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady && result.submitReady->commands.size() == 1 &&
               state.timerCalls == 2,
           "ON destination timer uses its independent INT64_MIN value read");
  }

  {
    RuntimeHarness runtime;
    Skin2DRenderer renderer;
    FakeResources resources;
    FakeState state;
    state.timerSequence = {0, INT64_MIN};
    const std::vector<SkinSourceRect> frames{
        {.x = 0, .y = 0, .w = 10, .h = 10},
        {.x = 10, .y = 0, .w = 10, .h = 10}};
    resources.addImageAtlas(402, frames, 20, 10);
    auto object = imageObject(1, 402, true);
    auto &sprite =
        std::get<SkinImageObject>(object.payload).orderedStates.front();
    sprite.frames = frames;
    sprite.cycleMillis = 100;
    sprite.timer = SkinTimerPropertyId{1};
    ValidatedBeatorajaSkinModel model;
    model.model.timerProperties = {
        {.id = SkinTimerPropertyId{1},
         .source = SkinBuiltinPropertySelector{.value = 33},
         .authoredOrdinal = 1}};
    model.model.objects = {std::move(object)};
    model.model.destinations = {destination(1, 99, 10.0)};

    const auto result = evaluate(renderer, runtime, model, resources, state);
    expect(result.submitReady && result.submitReady->commands.size() == 1 &&
               state.timerCalls == 2 &&
               std::get<SkinTexturedQuadCommand>(
                   result.submitReady->commands.front().payload)
                       .vertices[0]
                       .u == 0.5F,
           "ON sprite timer uses its independent INT64_MIN value read");
  }
}

void testNoteExpansionUsesCapturedQuarterNotePhase() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.noteExpansionResult = {.supported = true,
                               .elapsedSinceQuarterNoteMillis = 9};
  state.notes = {
      {.lane = 0,
       .kind = SkinProjectedNoteKind::Normal,
       .submissionOrdinal = 1},
      {.lane = 0,
       .kind = SkinProjectedNoteKind::Invisible,
       .authoredYDisplacement = 30.0,
       .submissionOrdinal = 2},
  };
  state.longNotes = {{.lane = 0,
                      .mode = SkinProjectedLongNoteMode::CN,
                      .headAuthoredYDisplacement = 60.0,
                      .tailAuthoredYDisplacement = 120.0,
                      .submissionOrdinal = 3}};

  auto note = gameplayNoteObject(resources);
  note.expansionRatePercent = {150, 200};
  ValidatedBeatorajaSkinModel model;
  model.model.objects = {{.id = 1,
                          .authoredName = "expanded-notes",
                          .payload = std::move(note),
                          .authoredOrdinal = 1,
                          .critical = true}};
  model.model.destinations = {destination(1, 94, 0.0)};

  const auto expanded = evaluate(renderer, runtime, model, resources, state);
  expect(expanded.submitReady && expanded.submitReady->commands.size() == 5,
         "captured quarter-note phase expands notes and long-note chips");
  if (!expanded.submitReady || expanded.submitReady->commands.size() != 5) {
    return;
  }
  const auto &normal = std::get<SkinTexturedQuadCommand>(
      expanded.submitReady->commands[0].payload);
  const auto &invisible = std::get<SkinTexturedQuadCommand>(
      expanded.submitReady->commands[1].payload);
  const auto &longStart = std::get<SkinTexturedQuadCommand>(
      expanded.submitReady->commands[4].payload);
  expect(normal.vertices[0].x == 90.0F && normal.vertices[0].y == 525.0F &&
             normal.vertices[2].x == 150.0F && normal.vertices[2].y == 505.0F,
         "normal note reaches the authored expansion maximum at 9 ms");
  expect(invisible.vertices[0].x == 100.0F &&
             invisible.vertices[0].y == 490.0F &&
             invisible.vertices[2].x == 140.0F &&
             invisible.vertices[2].y == 480.0F,
         "invisible note ignores the expansion pulse");
  expect(longStart.vertices[0].x == 90.0F &&
             longStart.vertices[0].y == 465.0F &&
             longStart.vertices[2].x == 150.0F &&
             longStart.vertices[2].y == 445.0F,
         "long-note start uses the same expanded chip geometry");

  state.noteExpansionResult.supported = false;
  const auto unavailable =
      evaluate(renderer, runtime, model, resources, state, 2);
  expect(!unavailable.submitReady &&
             hasDiagnostic(unavailable, "skin.renderer.note.expansion"),
         "non-default note expansion fails closed without a captured phase");

  state.notes = {{.lane = 0,
                  .kind = SkinProjectedNoteKind::Invisible,
                  .submissionOrdinal = 1}};
  state.longNotes.clear();
  const auto callsBeforeInvisible = state.noteExpansionCalls;
  const auto invisibleOnly =
      evaluate(renderer, runtime, model, resources, state, 3);
  expect(invisibleOnly.submitReady &&
             invisibleOnly.submitReady->commands.size() == 1 &&
             state.noteExpansionCalls == callsBeforeInvisible,
         "invisible-only projection never requests note expansion phase");

  state.notes.clear();
  state.lines = {
      {.kind = SkinProjectedLineKind::Group, .submissionOrdinal = 1}};
  const auto callsBeforeLine = state.noteExpansionCalls;
  const auto lineOnly = evaluate(renderer, runtime, model, resources, state, 4);
  expect(lineOnly.submitReady && lineOnly.submitReady->commands.size() == 1 &&
             state.noteExpansionCalls == callsBeforeLine,
         "line-only projection never requests note expansion phase");
}

void testNoteLineCallbacksRunAfterTopLevelPreparationAndOnlyWhenDrawable() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.timerResult = 0;
  state.lines = {
      {.kind = SkinProjectedLineKind::Group, .submissionOrdinal = 1}};

  auto note = gameplayNoteObject(resources);
  note.lines.front().sprite->cycleMillis = 100;
  note.lines.front().sprite->timer = SkinTimerPropertyId{1};
  note.lines.front().destination->timer = SkinTimerPropertyId{2};
  auto laterImage = imageObject(2, 500, true);
  auto &laterSprite =
      std::get<SkinImageObject>(laterImage.payload).orderedStates.front();
  laterSprite.cycleMillis = 100;
  laterSprite.timer = SkinTimerPropertyId{3};
  resources.addImage(500, laterSprite.frames.front());

  ValidatedBeatorajaSkinModel model;
  model.model.timerProperties = {
      {.id = SkinTimerPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 51},
       .authoredOrdinal = 1},
      {.id = SkinTimerPropertyId{2},
       .source = SkinBuiltinPropertySelector{.value = 52},
       .authoredOrdinal = 2},
      {.id = SkinTimerPropertyId{3},
       .source = SkinBuiltinPropertySelector{.value = 53},
       .authoredOrdinal = 3}};
  model.model.objects = {{.id = 1,
                          .authoredName = "deferred-line",
                          .payload = std::move(note),
                          .authoredOrdinal = 1,
                          .critical = true},
                         std::move(laterImage)};
  model.model.destinations = {destination(1, 100, 0.0),
                              destination(2, 101, 0.0)};

  const auto visible = evaluate(renderer, runtime, model, resources, state);
  expect(visible.submitReady && visible.submitReady->commands.size() == 2 &&
             state.accessOrder ==
                 std::vector<int>({2'053, 2'053, 2'052, 2'052, 2'051, 2'051}),
         "nested line callbacks run during Note draw after every top-level "
         "object has prepared");

  state.accessOrder.clear();
  state.timerCalls = 0;
  auto &noteDestination = model.model.destinations.front().presentation;
  noteDestination.frames.front().clip =
      SkinSourceRect{.x = 2'000, .y = 2'000, .w = 10, .h = 10};
  const auto clipped = evaluate(renderer, runtime, model, resources, state, 2);
  expect(clipped.submitReady && clipped.submitReady->commands.size() == 1 &&
             state.accessOrder == std::vector<int>({2'053, 2'053}),
         "fully clipped Note skips nested line draw callbacks");
}

void testHiddenNoteStillPreparesEveryLaneSourceInPinnedOrder() {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.booleanResult = {.value = false, .supported = true};
  state.timerResult = 0;
  auto note = gameplayNoteObject(resources);
  const std::array<SkinNoteVisualKind, 14> prepareOrder{
      SkinNoteVisualKind::Normal,          SkinNoteVisualKind::LnEnd,
      SkinNoteVisualKind::LnStart,         SkinNoteVisualKind::LnBodyActive,
      SkinNoteVisualKind::LnBodyInactive,  SkinNoteVisualKind::HcnEnd,
      SkinNoteVisualKind::HcnStart,        SkinNoteVisualKind::HcnBodyActive,
      SkinNoteVisualKind::HcnBodyInactive, SkinNoteVisualKind::HcnDamage,
      SkinNoteVisualKind::HcnReactive,     SkinNoteVisualKind::Mine,
      SkinNoteVisualKind::Hidden,          SkinNoteVisualKind::Processed};

  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1},
       .source = SkinBuiltinPropertySelector{.value = 40},
       .authoredOrdinal = 1}};
  int timer = 1;
  for (const auto kind : prepareOrder) {
    auto &sprite =
        std::get<SkinSpriteFrames>(note.lanes.front().visuals.at(kind));
    sprite.cycleMillis = 100;
    sprite.timer = SkinTimerPropertyId{static_cast<std::uint32_t>(timer)};
    model.model.timerProperties.push_back(
        {.id = *sprite.timer,
         .source = SkinBuiltinPropertySelector{.value = 100 + timer},
         .authoredOrdinal = static_cast<std::uint32_t>(timer)});
    ++timer;
  }
  model.model.objects = {{.id = 1,
                          .authoredName = "hidden-note-prepare",
                          .payload = std::move(note),
                          .authoredOrdinal = 1,
                          .critical = true}};
  auto presented = destination(1, 96, 0.0);
  presented.presentation.conditions = {SkinBooleanPropertyId{1}};
  model.model.destinations = {std::move(presented)};

  const auto result = evaluate(renderer, runtime, model, resources, state);
  std::vector<int> expected{1'040};
  for (int selector = 101; selector <= 114; ++selector) {
    expected.push_back(2'000 + selector);
    expected.push_back(2'000 + selector);
  }
  expect(result.submitReady && result.submitReady->commands.empty() &&
             state.accessOrder == expected,
         "runtime-hidden Note prepares normal, LN/HCN slots, mine, hidden, "
         "and processed sources before drawing nothing");
}

nlohmann::json fixtureVertex(const SkinVertex &vertex) {
  return {vertex.x, vertex.y, vertex.u, vertex.v, vertex.rgba};
}

nlohmann::json fixtureState(const SkinRenderState &state) {
  nlohmann::json scissor = nullptr;
  if (state.scissor) {
    scissor = {state.scissor->x, state.scissor->y, state.scissor->width,
               state.scissor->height};
  }
  return {{"blend", static_cast<int>(state.blend)},
          {"filter", static_cast<int>(state.filter)},
          {"scissor", std::move(scissor)}};
}

nlohmann::json fixtureGeometry(const AuthoredDestinationGeometry &geometry) {
  nlohmann::json clip = nullptr;
  if (geometry.clip) {
    clip = {geometry.clip->x, geometry.clip->y, geometry.clip->width,
            geometry.clip->height};
  }
  return {{"rect",
           {geometry.rect.x, geometry.rect.y, geometry.rect.width,
            geometry.rect.height}},
          {"clip", std::move(clip)},
          {"center", {geometry.centerX, geometry.centerY}},
          {"angle", geometry.angleDegrees},
          {"rgba", geometry.rgba},
          {"blend", static_cast<int>(geometry.blend)},
          {"filter", static_cast<int>(geometry.filter)},
          {"stretch", static_cast<int>(geometry.stretch)}};
}

nlohmann::json fixtureViewport(const PlaySkinViewport &viewport) {
  const auto matrix = [](const Affine2D &value) {
    return nlohmann::json{value.m00, value.m01, value.tx,
                          value.m10, value.m11, value.ty};
  };
  return {
      {"authoredToUi", matrix(viewport.authoredToUi)},
      {"uiToAuthored", matrix(viewport.uiToAuthored)},
      {"drawableAuthoredBounds",
       {viewport.drawableAuthoredBounds.x, viewport.drawableAuthoredBounds.y,
        viewport.drawableAuthoredBounds.width,
        viewport.drawableAuthoredBounds.height}},
      {"safeUiBounds",
       {viewport.safeUiBounds.x, viewport.safeUiBounds.y,
        viewport.safeUiBounds.width, viewport.safeUiBounds.height}},
      {"valid", viewport.valid}};
}

nlohmann::json fixtureCommandBuffer(const SkinCommandBuffer &buffer) {
  nlohmann::json commands = nlohmann::json::array();
  for (const auto &command : buffer.commands) {
    nlohmann::json payload;
    if (const auto *quad =
            std::get_if<SkinTexturedQuadCommand>(&command.payload)) {
      nlohmann::json vertices = nlohmann::json::array();
      for (const auto &vertex : quad->vertices) {
        vertices.push_back(fixtureVertex(vertex));
      }
      payload = {{"type", "quad"},
                 {"resource", quad->resource},
                 {"vertices", std::move(vertices)},
                 {"state", fixtureState(quad->state)}};
    } else if (const auto *run =
                   std::get_if<SkinGlyphRunCommand>(&command.payload)) {
      nlohmann::json glyphs = nlohmann::json::array();
      for (const auto &glyph : run->glyphs) {
        nlohmann::json vertices = nlohmann::json::array();
        for (const auto &vertex : glyph.vertices) {
          vertices.push_back(fixtureVertex(vertex));
        }
        glyphs.push_back(
            {{"codepoint", static_cast<std::uint32_t>(glyph.codepoint)},
             {"vertices", std::move(vertices)}});
      }
      payload = {{"type", "glyph"},
                 {"atlas", run->atlas},
                 {"glyphs", std::move(glyphs)},
                 {"state", fixtureState(run->state)}};
    } else if (const auto *primitive =
                   std::get_if<SkinPrimitiveCommand>(&command.payload)) {
      nlohmann::json vertices = nlohmann::json::array();
      for (const auto &vertex : primitive->vertices) {
        vertices.push_back(fixtureVertex(vertex));
      }
      payload = {{"type", "primitive"},
                 {"kind", static_cast<int>(primitive->kind)},
                 {"vertices", std::move(vertices)},
                 {"state", fixtureState(primitive->state)}};
    } else {
      const auto &bga = std::get<SkinBgaCommand>(command.payload);
      payload = {{"type", "bga"},
                 {"geometry", fixtureGeometry(bga.authoredGeometry)},
                 {"viewport", fixtureViewport(bga.viewport)},
                 {"ordinal", bga.authoredOrdinal}};
    }
    commands.push_back({{"ordinal", command.authoredOrdinal},
                        {"sourceObject", command.sourceObject},
                        {"payload", std::move(payload)}});
  }
  nlohmann::json batches = nlohmann::json::array();
  for (const auto &batch : buffer.adjacentBatches) {
    batches.push_back({batch.firstCommand, batch.commandCount});
  }
  return {{"frameSerial", buffer.frameSerial},
          {"commands", std::move(commands)},
          {"batches", std::move(batches)}};
}

SkinFrameEvaluationResult
evaluateAllV1Fixture(const PlaySkinViewport &viewport) {
  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  state.integerResult = {.value = 42, .supported = true};
  state.floatResult = {.value = 0.5, .supported = true};
  state.gaugeResult = {.supported = true,
                       .value = 50.0,
                       .gaugeType = 0,
                       .minimum = 0.0,
                       .maximum = 100.0,
                       .border = 20.0};
  state.judgeResult = {.supported = true,
                       .optionalZeroBasedGrade = 0,
                       .combo = 0,
                       .maximumGauge = false};
  state.notes = {{.lane = 0,
                  .kind = SkinProjectedNoteKind::Normal,
                  .submissionOrdinal = 1},
                 {.lane = 0,
                  .kind = SkinProjectedNoteKind::Invisible,
                  .authoredYDisplacement = 15.0,
                  .submissionOrdinal = 2},
                 {.lane = 0,
                  .kind = SkinProjectedNoteKind::Mine,
                  .authoredYDisplacement = 30.0,
                  .submissionOrdinal = 3},
                 {.lane = 0,
                  .kind = SkinProjectedNoteKind::Normal,
                  .authoredYDisplacement = 45.0,
                  .judged = true,
                  .submissionOrdinal = 4}};
  state.longNotes = {{.lane = 0,
                      .mode = SkinProjectedLongNoteMode::LN,
                      .headAuthoredYDisplacement = 60.0,
                      .tailAuthoredYDisplacement = 100.0,
                      .submissionOrdinal = 5},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::LN,
                      .headAuthoredYDisplacement = 105.0,
                      .tailAuthoredYDisplacement = 145.0,
                      .active = true,
                      .submissionOrdinal = 6},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::CN,
                      .headAuthoredYDisplacement = 150.0,
                      .tailAuthoredYDisplacement = 190.0,
                      .submissionOrdinal = 7},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::HCN,
                      .headAuthoredYDisplacement = 195.0,
                      .tailAuthoredYDisplacement = 235.0,
                      .submissionOrdinal = 8},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::HCN,
                      .headAuthoredYDisplacement = 240.0,
                      .tailAuthoredYDisplacement = 280.0,
                      .active = true,
                      .submissionOrdinal = 9},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::HCN,
                      .headAuthoredYDisplacement = 285.0,
                      .tailAuthoredYDisplacement = 325.0,
                      .damaged = true,
                      .submissionOrdinal = 10},
                     {.lane = 0,
                      .mode = SkinProjectedLongNoteMode::HCN,
                      .headAuthoredYDisplacement = 330.0,
                      .tailAuthoredYDisplacement = 370.0,
                      .reactive = true,
                      .submissionOrdinal = 11}};
  state.lines = {
      {.kind = SkinProjectedLineKind::Group, .submissionOrdinal = 12},
      {.kind = SkinProjectedLineKind::Bpm,
       .authoredYDisplacement = 10.0,
       .submissionOrdinal = 13},
      {.kind = SkinProjectedLineKind::Stop,
       .authoredYDisplacement = 20.0,
       .submissionOrdinal = 14},
      {.kind = SkinProjectedLineKind::Time,
       .authoredYDisplacement = 30.0,
       .submissionOrdinal = 15}};

  resources.addImage(10, {.x = 0, .y = 0, .w = 10, .h = 10});
  const auto numberRegions = glyphRegions(10);
  const auto floatRegions = glyphRegions(13);
  resources.addImageAtlas(20, numberRegions, 100, 20);
  resources.addImageAtlas(21, floatRegions, 130, 20);
  resources.addTextAtlas(4, avAtlas());
  resources.addImage(30, {.x = 0, .y = 0, .w = 10, .h = 10});
  resources.addImage(31, {.x = 10, .y = 20, .w = 10, .h = 10}, 100, 100);
  resources.addImage(40, {.x = 0, .y = 0, .w = 10, .h = 10});

  SkinNumberObject number;
  number.digits.positive = glyphSprite(20, numberRegions);
  number.digits.glyphsPerAnimationFrame = 10;
  number.value = SkinIntegerPropertyId{1};
  number.digitCount = 2;
  SkinFloatObject floating;
  floating.digits.positive = glyphSprite(21, floatRegions);
  floating.digits.glyphsPerAnimationFrame = 13;
  floating.value = SkinFloatPropertyId{1};
  floating.integerDigits = 1;
  floating.fractionalDigits = 1;
  SkinSliderObject slider;
  slider.knob = singleFrameSprite(30);
  slider.value = SkinFloatPropertyId{1};
  slider.direction = 1;
  slider.range = 20.0;
  SkinGraphObject graph;
  graph.fill = {.resource = 31,
                .frames = {{.x = 10, .y = 20, .w = 10, .h = 10}}};
  graph.value = SkinFloatPropertyId{1};
  graph.direction = 0;
  SkinGaugeObject gauge;
  gauge.parts = 2;
  gauge.animation = SkinGaugeAnimationType::Increase;
  gauge.animationRange = 0;
  gauge.animationCycleMillis = 4;
  for (int role = 0; role < 36; ++role) {
    const SkinResourceId resource = 1'000 + role;
    resources.addImage(resource, {.x = 0, .y = 0, .w = 10, .h = 10});
    gauge.orderedNodes.push_back(singleFrameSprite(resource));
  }
  SkinJudgeObject judge;
  judge.grades.resize(7);
  judge.grades[0].image = SkinNestedObjectPresentation{
      .object = 9, .destination = destination(9, 80, 470.0).presentation};
  auto spriteNote = gameplayNoteObject(resources);
  auto synthesizedNote = gameplayNoteObject(resources);
  for (auto &[kind, visual] : synthesizedNote.lanes.front().visuals) {
    visual = SkinSynthesizedNoteVisual{.kind = kind};
  }
  auto hiddenSprite = singleFrameSprite(3'000);
  auto liftSprite = singleFrameSprite(3'001);
  resources.addImage(3'000, hiddenSprite.frames.front());
  resources.addImage(3'001, liftSprite.frames.front());

  ValidatedBeatorajaSkinModel model;
  model.model.integerProperties = {
      {.id = SkinIntegerPropertyId{1},
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = SkinBuiltinPropertySelector{.value = 61},
       .authoredOrdinal = 1}};
  model.model.floatProperties = {
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 62},
       .authoredOrdinal = 2}};
  model.model.objects = {
      imageObject(1, 10, true),
      {.id = 2,
       .authoredName = "fixture-number",
       .payload = std::move(number),
       .authoredOrdinal = 2,
       .critical = true},
      {.id = 3,
       .authoredName = "fixture-float",
       .payload = std::move(floating),
       .authoredOrdinal = 3,
       .critical = true},
      textObject(4, true),
      {.id = 5,
       .authoredName = "fixture-slider",
       .payload = std::move(slider),
       .authoredOrdinal = 5,
       .critical = true},
      {.id = 6,
       .authoredName = "fixture-graph",
       .payload = std::move(graph),
       .authoredOrdinal = 6,
       .critical = true},
      {.id = 7,
       .authoredName = "fixture-gauge",
       .payload = std::move(gauge),
       .authoredOrdinal = 7,
       .critical = true},
      {.id = 8,
       .authoredName = "fixture-judge",
       .payload = std::move(judge),
       .authoredOrdinal = 8,
       .critical = true},
      imageObject(9, 40, false),
      {.id = 10,
       .authoredName = "fixture-note",
       .payload = std::move(spriteNote),
       .authoredOrdinal = 10,
       .critical = true},
      {.id = 11,
       .authoredName = "fixture-synthesized-note",
       .payload = std::move(synthesizedNote),
       .authoredOrdinal = 11,
       .critical = true},
      {.id = 12,
       .authoredName = "fixture-hidden",
       .payload = SkinCoverObject{.kind = SkinCoverKind::Hidden,
                                  .sprite = std::move(hiddenSprite),
                                  .disappearLine = 150.0,
                                  .disappearLineLinksLift = false},
       .authoredOrdinal = 12,
       .critical = true},
      {.id = 13,
       .authoredName = "fixture-lift",
       .payload = SkinCoverObject{.kind = SkinCoverKind::Lift,
                                  .sprite = std::move(liftSprite),
                                  .disappearLine = 150.0,
                                  .disappearLineLinksLift = false},
       .authoredOrdinal = 13,
       .critical = true},
      {.id = 14,
       .authoredName = "fixture-bga",
       .payload = SkinBgaObject{},
       .authoredOrdinal = 14,
       .critical = true}};

  auto sizedDestination = [](SkinObjectId object, std::uint32_t ordinal,
                             double x, double y, double width, double height) {
    auto value = destination(object, ordinal, x);
    value.presentation.frames.front().y = y;
    value.presentation.frames.front().width = width;
    value.presentation.frames.front().height = height;
    return value;
  };
  model.model.destinations = {
      sizedDestination(1, 10, 10.0, 10.0, 30.0, 20.0),
      sizedDestination(2, 20, 50.0, 10.0, 10.0, 20.0),
      sizedDestination(3, 30, 90.0, 10.0, 10.0, 20.0),
      sizedDestination(4, 40, 140.0, 10.0, 100.0, 20.0),
      sizedDestination(5, 50, 250.0, 10.0, 20.0, 20.0),
      sizedDestination(6, 60, 300.0, 10.0, 40.0, 20.0),
      sizedDestination(7, 70, 350.0, 10.0, 100.0, 20.0),
      sizedDestination(8, 80, 0.0, 0.0, 10.0, 10.0),
      sizedDestination(10, 90, 0.0, 0.0, 10.0, 10.0),
      sizedDestination(11, 100, 0.0, 0.0, 10.0, 10.0),
      sizedDestination(12, 110, 600.0, 100.0, 100.0, 100.0),
      sizedDestination(13, 120, 720.0, 100.0, 100.0, 100.0),
      sizedDestination(14, 130, 500.0, 300.0, 640.0, 360.0)};
  model.model.destinations.back().presentation.stretch =
      SkinStretchMode::KeepAspectRatioFitInner;

  static const BeatorajaSkinConfiguration configuration;
  return renderer.evaluateFrame({.frameSerial = 1,
                                 .sessionSerial = 1,
                                 .visualTimeMicros = 0,
                                 .model = model,
                                 .configuration = configuration,
                                 .resources = resources,
                                 .viewport = viewport,
                                 .runtime = runtime.runtime(),
                                 .state = state});
}

void testDesktopAndIpadFitCommandFixtures() {
  const nlohmann::json requiredCoverage = {"Image",
                                           "Number",
                                           "Float",
                                           "Text",
                                           "Slider",
                                           "Graph",
                                           "Gauge",
                                           "Judge",
                                           "Note.Normal",
                                           "Note.Invisible",
                                           "Note.Mine",
                                           "Note.Processed",
                                           "Note.LN.Active",
                                           "Note.LN.Inactive",
                                           "Note.CN",
                                           "Note.HCN.Active",
                                           "Note.HCN.Inactive",
                                           "Note.HCN.Damaged",
                                           "Note.HCN.Reactive",
                                           "Line.Group",
                                           "Line.Bpm",
                                           "Line.Stop",
                                           "Line.Time",
                                           "Note.Synthesized",
                                           "Cover.Hidden",
                                           "Cover.Lift",
                                           "BGA"};
  for (const std::string_view fixtureName : {
           "all_v1_objects_1280x720.json",
           "all_v1_objects_ipad_fit.json",
       }) {
    const fs::path fixturePath = fs::path(ASOBMASHOW_SOURCE_DIR) /
                                 "tests/fixtures/beatoraja_skin/commands" /
                                 fixtureName;
    std::ifstream fixtureInput(fixturePath, std::ios::binary);
    nlohmann::json fixture;
    fixtureInput >> fixture;
    expect(fixture.value("schema", "") == "asobmashow.skin.commands.v1" &&
               fixture.at("coverage") == requiredCoverage,
           "external command fixture covers every v1 gameplay object role");
    const auto authored = fixture.at("authoredSize");
    const auto safe = fixture.at("safeBounds");
    const auto playViewport =
        evaluatePlaySkinViewport({.width = authored.at(0).get<double>(),
                                  .height = authored.at(1).get<double>()},
                                 {.x = safe.at(0).get<double>(),
                                  .y = safe.at(1).get<double>(),
                                  .width = safe.at(2).get<double>(),
                                  .height = safe.at(3).get<double>()},
                                 {});
    const auto result = evaluateAllV1Fixture(playViewport);
    expect(result.submitReady.has_value(),
           "all-v1 fixture scenario evaluates atomically");
    if (!result.submitReady) {
      for (const auto &diagnostic : result.diagnostics) {
        std::cerr << "Fixture diagnostic for " << fixtureName << ": "
                  << diagnostic.code << " " << diagnostic.message << '\n';
      }
      continue;
    }
    const auto canonical = fixtureCommandBuffer(*result.submitReady);
    const std::string digest = file_checksum::sha256(canonical.dump());
    std::map<std::string, std::size_t> payloadCounts{
        {"quad", 0}, {"glyph", 0}, {"primitive", 0}, {"bga", 0}};
    for (const auto &command : canonical.at("commands")) {
      ++payloadCounts.at(command.at("payload").at("type").get<std::string>());
    }
    if (fixture.value("canonicalSha256", "") != digest) {
      std::cerr << "Fixture digest for " << fixtureName << ": " << digest
                << " commands=" << result.submitReady->commands.size()
                << " batches=" << result.submitReady->adjacentBatches.size()
                << " payloads=" << nlohmann::json(payloadCounts).dump() << '\n';
    }
    expect(fixture.at("commandCount").get<std::size_t>() ==
                   result.submitReady->commands.size() &&
               fixture.at("batchCount").get<std::size_t>() ==
                   result.submitReady->adjacentBatches.size() &&
               fixture.at("payloadCounts") == nlohmann::json(payloadCounts) &&
               fixture.value("canonicalSha256", "") == digest,
           "external fixture hash covers exact payloads, vertices, UVs, "
           "colors, states, clips, source IDs, BGA viewport, and batches");
  }
}

} // namespace

int main() {
  testCapturedFrameSerialMustMatchCallbacksAndProjection();
  testOffsetSentinelAndSourceAwarePrecedence();
  testCriticalFailureCannotExposePartialBuffer();
  testCriticalLuaCallbackFailureIsAlsoAtomic();
  testOptionalFailureSuppressesOnlyItsObject();
  testBuiltinImageStateAndOutOfRangeFallback();
  testHiddenImageStillSelectsSourceAfterRuntimeSuppression();
  testEmptyDestinationIsDroppedBeforeObjectStateResolution();
  testProjectionOrdinalIrregularitiesNeverDiscardAGameplayFrame();
  testLargeProjectionDoesNotHitAnAppSpecificFrameLimit();
  testBindingAndDisabledLookupsStayLogarithmicAtModelLimits();
  testImageCommandsPreserveOrderAndBatchOnlyAdjacentCompatibility();
  testNumberUsesSignedGlyphSetPaddingAndNegativeAlignmentShift();
  testFloatTruncatesAndUsesPositiveAlignmentShift();
  testZeroCycleNumericSpriteDoesNotConsultItsTimer();
  testFalseDestinationSkipsNumericSourceTimerAfterValueLookup();
  testLuaFractionalNumberUsesPinnedIntegerCoercion();
  testTextUsesPreparedMetricsKerningAndAtlasUvs();
  testTextVerticalPlacementMatchesPinnedBitmapFontBaseline();
  testMissingTextGlyphHonorsCriticalityWithoutPartialCommands();
  testFalseDestinationSkipsTextValueCallback();
  testTextGlyphLimitFailsBeforePublishingACommand();
  testTextAlignmentWrappingAndShrinkUsePreparedAdvances();
  testTextWordWrapAndMultilineTruncateMatchGlyphLayoutBoundaries();
  testFalseDestinationSkipsSliderAndGraphSourceAndValueCallbacks();
  testSliderSourceTimerPrecedesValueAndZeroCycleSkipsTimer();
  testSliderDirectionsMoveBeforeSharedProjection();
  testGraphCropsLeftOrBottomWithJavaTruncation();
  testExplicitSliderAndGraphRatesRemainUnclamped();
  testNegativeGraphsUseAbsoluteIntrinsicSizeForStretching();
  testExtremeFiniteSliderAndGraphRatesFailWithoutPublishing();
  testSliderAndGraphRatesRoundAtTheJavaFloatBoundary();
  testIntegerGraphRatesUsePinnedFloatSubtractionOrder();
  testDescendingIntegerGraphRatesUsePinnedDirection();
  testIntegerGraphOverflowRangeDoesNotRejectTheFrame();
  testGaugeMapsFamiliesRolesAndPartGeometry();
  testGaugeSegmentSelectionRoundsAtJavaFloatBoundary();
  testGaugeAnimationUsesStrictDeadlineAndFlickerOverlayOrder();
  testHiddenGaugeStillAdvancesAndDirectDrawIgnoresImageTransforms();
  testGaugeAnimationStateResetsWhenSessionChangesInSameModelStorage();
  testGaugeRandomUsesBoundedSessionSourceAndFailsAtomically();
  testJudgeLowersRelativeComboBeforeShiftedImage();
  testJudgeMaxGaugeFallsBackImageAndDetailIndependently();
  testHiddenJudgeStillPreparesChildrenInPinnedCallbackOrder();
  testCriticalJudgeRequiresSupportedState();
  testNoteLongNoteAndLineCommandsPreserveMergedProjectionOrder();
  testLiveScrollUnitsUseSkinLaneHeightAndHispeed();
  testEveryLongNoteBodyStateUsesItsDistinctVisualRole();
  testProjectedLineEmitsEveryLaneGroupAndIgnoresNestedClip();
  testSynthesizedNoteFallbacksEmitBoundedColoredPrimitives();
  testHiddenAndLiftCoversApplyDisappearLineClipping();
  testHiddenCoverStillSelectsSourceAfterRuntimeSuppression();
  testBgaEmitsOneRoleFreePreStretchCommand();
  testTimersProbeThenReadAndTruncateMillisecondsIndependently();
  testTimerProbeOnKeepsIndependentSentinelValueReadActive();
  testNoteExpansionUsesCapturedQuarterNotePhase();
  testNoteLineCallbacksRunAfterTopLevelPreparationAndOnlyWhenDrawable();
  testHiddenNoteStillPreparesEveryLaneSourceInPinnedOrder();
  testDesktopAndIpadFitCommandFixtures();
  return failures == 0 ? 0 : 1;
}
