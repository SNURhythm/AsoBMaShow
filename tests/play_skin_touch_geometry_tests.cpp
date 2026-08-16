#include "skin/beatoraja/Skin2DRenderer.h"

#include "FileChecksum.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <cmath>
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

bool near(double actual, double expected) {
  return std::abs(actual - expected) < 0.000'001;
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
              ("asobmashow-skin-touch-geometry-" +
               std::to_string(++serial));
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
        package_(*normalizePackageId("TouchGeometry").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry) {
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/main.luaskin",
              "return {type=0,w=100,h=50,name='touch-geometry'}");
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
             *makeSkinProfileId("55555555-5555-4555-8555-555555555555")});
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
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "runtime configured phase executes");
    configured.value.reset();
    expect(runtime_->enterRenderPhase().ok, "runtime enters render phase");
  }

  LuaSkinRuntime &runtime() { return *runtime_; }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
};

class FakeResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id) {
    const SkinSourceRect region{.x = 0, .y = 0, .w = 10, .h = 10};
    PreparedSkinResource resource;
    resource.id = id;
    resource.width = 10;
    resource.height = 10;
    resource.regions = {region};
    resource.regionMappings = {{.authored = region, .resolved = region}};
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
    if (!resource || resource->regionMappings.empty()) {
      return nullptr;
    }
    const auto &mapping = resource->regionMappings.front();
    return mapping.authored.x == authored.x &&
                   mapping.authored.y == authored.y &&
                   mapping.authored.w == authored.w &&
                   mapping.authored.h == authored.h
               ? &mapping
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
  std::map<SkinResourceId, PreparedSkinResource> images_;
};

class FakeState final : public ISkinFrameState {
public:
  std::uint64_t frameSerial() const noexcept override { return serial; }
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
    return {.value = 0.5, .supported = true};
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

  std::uint64_t serial = 1;
};

SkinSpriteFrames sprite(SkinResourceId resource) {
  return {.resource = resource,
          .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}};
}

SkinDestination destination(SkinObjectId object, std::uint32_t ordinal,
                            double x, double y, double width = 20.0,
                            double height = 10.0) {
  return {.object = object,
          .presentation =
              {.loop = -1,
               .frames = {{.timeMillis = 0,
                           .x = x,
                           .y = y,
                           .width = width,
                           .height = height}},
               .authoredOrdinal = ordinal}};
}

UiLogicalPoint uiPointForAuthored(const PlaySkinViewport &viewport, double x,
                                  double y) {
  return {.x = static_cast<float>(viewport.authoredToUi.m00 * x +
                                  viewport.authoredToUi.m01 * y +
                                  viewport.authoredToUi.tx),
          .y = static_cast<float>(viewport.authoredToUi.m10 * x +
                                  viewport.authoredToUi.m11 * y +
                                  viewport.authoredToUi.ty)};
}

void addSlider(ValidatedBeatorajaSkinModel &model,
               FakeResources &resources, SkinObjectId id,
               std::uint8_t direction, double range, bool changeable,
               std::optional<SkinFloatWriterId> writer,
               std::uint32_t objectOrdinal, bool critical = true) {
  resources.addImage(id);
  SkinSliderObject slider;
  slider.knob = sprite(id);
  slider.value = SkinFloatPropertyId{1};
  slider.writer = writer;
  slider.direction = direction;
  slider.range = range;
  slider.changeable = changeable;
  model.model.objects.push_back({.id = id,
                                 .authoredName = "slider-" +
                                                     std::to_string(id),
                                 .payload = std::move(slider),
                                 .authoredOrdinal = objectOrdinal,
                                 .critical = critical});
}

void addClickableImage(ValidatedBeatorajaSkinModel &model,
                       FakeResources &resources, SkinObjectId id,
                       SkinEventBindingId event, int clickMode,
                       std::uint32_t objectOrdinal) {
  resources.addImage(id);
  SkinImageObject image;
  image.orderedStates = {sprite(id)};
  image.clickEvent = event;
  image.clickMode = clickMode;
  model.model.objects.push_back({.id = id,
                                 .authoredName = "image-" +
                                                     std::to_string(id),
                                 .payload = std::move(image),
                                 .authoredOrdinal = objectOrdinal,
                                 .critical = true});
}

class TestContext final {
public:
  TestContext() {
    model.model.floatProperties.push_back(
        {.id = SkinFloatPropertyId{1},
         .domain = SkinFloatPropertyDomain::Rate,
         .source = SkinBuiltinPropertySelector{.value = 4},
         .authoredOrdinal = 1});
  }

  SkinFrameEvaluationResult evaluate(const PlaySkinViewport &viewport,
                                     std::uint64_t sessionSerial = 1) {
    const std::uint64_t serial = nextSerial++;
    state.serial = serial;
    static const BeatorajaSkinConfiguration configuration;
    return renderer.evaluateFrame({.frameSerial = serial,
                                   .sessionSerial = sessionSerial,
                                   .visualTimeMicros = 0,
                                   .model = model,
                                   .configuration = configuration,
                                   .resources = resources,
                                   .viewport = viewport,
                                   .runtime = runtime.runtime(),
                                   .state = state});
  }

  RuntimeHarness runtime;
  Skin2DRenderer renderer;
  FakeResources resources;
  FakeState state;
  ValidatedBeatorajaSkinModel model;
  std::uint64_t nextSerial = 1;
};

void testViewportInverseMappingUsesUiLogicalSafeArea() {
  TestContext context;
  context.model.model.floatWriters.push_back(
      {.id = SkinFloatWriterId{1},
       .source = SkinBuiltinPropertySelector{.value = 4},
       .authoredOrdinal = 1});
  addSlider(context.model, context.resources, 1, 1, 40.0, true,
            SkinFloatWriterId{1}, 1);
  context.model.model.destinations = {destination(1, 1, 10.0, 5.0)};

  struct Scenario {
    ViewportSettings settings;
    double uiX;
    double uiY;
  };
  const std::vector<Scenario> scenarios{
      {.settings = {.mode = ViewportMode::Fit}, .uiX = 30.0, .uiY = 130.0},
      {.settings = {.mode = ViewportMode::Stretch},
       .uiX = 30.0,
       .uiY = 140.0},
      {.settings = {.mode = ViewportMode::Custom,
                    .customBase = CustomViewportBase::Fit,
                    .scaleX = 2.0F,
                    .scaleY = 0.5F,
                    .translateX = 7.0F,
                    .translateY = -3.0F},
       .uiX = -43.0,
       .uiY = 122.0},
  };

  for (const auto &scenario : scenarios) {
    const auto viewport = evaluatePlaySkinViewport(
        {.width = 100.0, .height = 50.0},
        {.x = 10.0, .y = 20.0, .width = 200.0, .height = 200.0},
        scenario.settings);
    const auto result = context.evaluate(viewport);
    expect(result.interactionLayout.has_value(),
           "valid Fit/Stretch/Custom viewport publishes touch geometry");
    if (!result.interactionLayout) {
      continue;
    }
    const auto authored = result.interactionLayout->authoredPointForUi(
        scenario.uiX, scenario.uiY);
    expect(authored && near(authored->x, 10.0) && near(authored->y, 20.0),
           "layout applies the captured viewport inverse transform");
    expect(result.interactionLayout->safeUiBounds.x == 10.0 &&
               result.interactionLayout->safeUiBounds.y == 20.0 &&
               result.interactionLayout->safeUiBounds.width == 200.0 &&
               result.interactionLayout->safeUiBounds.height == 200.0,
           "safe area stays in UI-logical units independent of HiDPI backing pixels");
  }
}

void testSliderTracksPreservePinnedDirectionsAndEndpoints() {
  TestContext context;
  for (std::uint32_t id = 1; id <= 4; ++id) {
    SkinFloatWriterBinding writer{
        .id = SkinFloatWriterId{id},
        .source = SkinBuiltinPropertySelector{.value = static_cast<int>(id)},
        .authoredOrdinal = id};
    if (id == 4) {
      writer.source = LuaCallbackId{.slot = 4, .generation = 1};
    }
    context.model.model.floatWriters.push_back(std::move(writer));
    addSlider(context.model, context.resources, id,
              static_cast<std::uint8_t>(id - 1), 40.0, id != 4,
              SkinFloatWriterId{id}, id);
    context.model.model.destinations.push_back(
        destination(id, id, 10.0, 20.0));
  }
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout->slidersTopmostFirst.size() == 4,
         "every visible slider publishes one queue-only geometry record");
  if (!result.interactionLayout ||
      result.interactionLayout->slidersTopmostFirst.size() != 4) {
    return;
  }

  const auto &left = result.interactionLayout->slidersTopmostFirst[0];
  const auto &down = result.interactionLayout->slidersTopmostFirst[1];
  const auto &right = result.interactionLayout->slidersTopmostFirst[2];
  const auto &up = result.interactionLayout->slidersTopmostFirst[3];
  expect(up.direction == 0 && near(up.valueZero.x, 10.0) &&
             near(up.valueZero.y, 20.0) && near(up.valueOne.x, 10.0) &&
             near(up.valueOne.y, 60.0) && near(up.authoredHitRegion.x, 10.0) &&
             near(up.authoredHitRegion.y, 20.0) &&
             near(up.authoredHitRegion.width, 20.0) &&
             near(up.authoredHitRegion.height, 40.0),
         "direction zero runs from the destination origin upward");
  expect(right.direction == 1 && near(right.valueOne.x, 50.0) &&
             near(right.valueOne.y, 20.0) &&
             near(right.authoredHitRegion.width, 40.0) &&
             near(right.authoredHitRegion.height, 10.0),
         "direction one runs right and uses knob height for its hit region");
  expect(down.direction == 2 && near(down.valueOne.x, 10.0) &&
             near(down.valueOne.y, -20.0) &&
             near(down.authoredHitRegion.y, -20.0) &&
             near(down.authoredHitRegion.height, 40.0),
         "direction two runs downward from the destination origin");
  expect(left.direction == 3 && near(left.valueOne.x, -30.0) &&
             near(left.valueOne.y, 20.0) &&
             near(left.authoredHitRegion.x, -30.0) &&
             near(left.authoredHitRegion.width, 40.0),
         "direction three runs left from the destination origin");
  expect(left.writer == SkinFloatWriterId{4} && !left.changeable &&
             up.writer == SkinFloatWriterId{1} && up.changeable,
         "layout records writer identity and authored changeability without invoking it");
  const UiLogicalPoint explicitWriterPoint =
      uiPointForAuthored(viewport, -10.0, 25.0);
  const auto explicitWriterHit =
      result.interactionLayout->hitTestUiControl(explicitWriterPoint);
  const auto explicitWriterInvocation =
      result.interactionLayout->writerInvocationFor(
          explicitWriterHit, explicitWriterPoint, 4'000);
  expect(result.interactionLayout->uiHitRegions().size() == 4 &&
             explicitWriterHit.sourceObject == 4 &&
             explicitWriterHit.writer == SkinFloatWriterId{4} &&
             explicitWriterInvocation &&
             explicitWriterInvocation->writer == SkinFloatWriterId{4} &&
             near(explicitWriterInvocation->normalizedValue, 0.5),
         "an explicit callback writer remains interactive when authored changeable is false");
}

void testOverlappingSlidersPublishReverseAuthoredTopmostOrder() {
  TestContext context;
  context.model.model.floatProperties.front().source =
      SkinBuiltinPropertySelector{.value = 40};
  context.model.model.floatWriters = {
      {.id = SkinFloatWriterId{1},
       .source = SkinBuiltinPropertySelector{.value = 40},
       .authoredOrdinal = 1},
      {.id = SkinFloatWriterId{2},
       .source = SkinBuiltinPropertySelector{.value = 50},
       .authoredOrdinal = 2}};
  addSlider(context.model, context.resources, 1, 1, 40.0, true,
            SkinFloatWriterId{1}, 1);
  addSlider(context.model, context.resources, 2, 1, 40.0, true,
            SkinFloatWriterId{2}, 2);
  context.model.model.destinations = {destination(1, 900, 10.0, 20.0),
                                      destination(2, 1, 10.0, 20.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout->slidersTopmostFirst.size() == 2 &&
             result.interactionLayout->slidersTopmostFirst[0].sourceObject ==
                 2 &&
             result.interactionLayout->slidersTopmostFirst[1].sourceObject ==
                 1,
         "overlap queue reverses authored insertion order like Skin.mousePressed");
  if (result.interactionLayout &&
      result.interactionLayout->slidersTopmostFirst.size() == 2) {
    expect(result.interactionLayout->slidersTopmostFirst[0].authoredOrdinal ==
               1 &&
               result.interactionLayout->slidersTopmostFirst[1]
                       .authoredOrdinal == 900,
           "authored ordinals remain metadata and do not replace source order");
    const auto hit = result.interactionLayout->hitTestUiControl(
        uiPointForAuthored(viewport, 20.0, 25.0));
    const auto publishedRegions = result.interactionLayout->uiHitRegions();
    expect(hit.kind == PresentationUiControlKind::Slider &&
               hit.sourceObject == 2 &&
               hit.authoredOrdinal == 1 &&
               hit.writer == SkinFloatWriterId{2},
           "overlap hit testing captures the reverse-authored topmost slider");
    expect(publishedRegions.size() == 2 &&
               publishedRegions.front().hit == hit,
           "immutable presentation geometry preserves reverse-authored overlap identity");
  }
}

void testImageActPublishesPointerDownHitGeometry() {
  TestContext context;
  context.model.model.events.push_back(
      {.id = SkinEventBindingId{7},
       .source = SkinBuiltinPropertySelector{.value = 42},
       .authoredOrdinal = 1});
  addClickableImage(context.model, context.resources, 7, SkinEventBindingId{7},
                    2, 1);
  context.model.model.destinations = {destination(7, 1, 10.0, 20.0, 40.0,
                                                   20.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  const auto hit = result.interactionLayout
                       ? result.interactionLayout->hitTestUiControl(
                             uiPointForAuthored(viewport, 35.0, 25.0))
                       : PresentationUiHit{};
  const auto right = result.interactionLayout
                         ? result.interactionLayout->eventInvocationFor(
                               hit, uiPointForAuthored(viewport, 35.0, 25.0),
                               7'000)
                         : std::optional<SkinEventInvocation>{};
  const auto left = result.interactionLayout
                        ? result.interactionLayout->eventInvocationFor(
                              hit, uiPointForAuthored(viewport, 15.0, 25.0),
                              8'000)
                        : std::optional<SkinEventInvocation>{};
  expect(result.interactionLayout &&
             hit.kind == PresentationUiControlKind::Image &&
             hit.sourceObject == 7 && hit.eventBinding == 7U && right && left &&
             right->argument == 1 && right->eventMicros == 7'000 &&
             left->argument == -1 && left->eventMicros == 8'000,
         "a visible Image act publishes Beatoraja's split click event arguments");

  const auto eventArgumentFor = [](int clickMode, UiLogicalPoint point) {
    SkinInteractionLayout layout;
    layout.revision = 17;
    layout.controlsTopmostFirst.push_back(SkinImageInteractionGeometry{
        .sourceObject = 90,
        .authoredOrdinal = 91,
        .authoredRegion = {.x = 10.0, .y = 20.0, .width = 40.0, .height = 20.0},
        .event = SkinEventBindingId{92},
        .clickMode = clickMode});
    const auto modeHit = layout.hitTestUiControl(point);
    const auto invocation = layout.eventInvocationFor(modeHit, point, 9'000);
    return invocation ? std::optional<int>{invocation->argument}
                      : std::optional<int>{};
  };
  expect(eventArgumentFor(0, {.x = 15.0F, .y = 25.0F}) == 1 &&
             eventArgumentFor(1, {.x = 45.0F, .y = 35.0F}) == -1 &&
             eventArgumentFor(2, {.x = 15.0F, .y = 25.0F}) == -1 &&
             eventArgumentFor(2, {.x = 35.0F, .y = 25.0F}) == 1 &&
             eventArgumentFor(3, {.x = 15.0F, .y = 25.0F}) == -1 &&
             eventArgumentFor(3, {.x = 15.0F, .y = 35.0F}) == 1,
         "all four pinned Image click modes preserve their signed arguments");
}

void testLaneCoverWriterUsesTypedHitAndQueueOnlyDrag() {
  TestContext context;
  context.model.model.floatWriters.push_back(
      {.id = SkinFloatWriterId{1},
       .source = LuaCallbackId{.slot = 1, .generation = 1},
       .authoredOrdinal = 1});
  addSlider(context.model, context.resources, 1, 2, 40.0, true,
            SkinFloatWriterId{1}, 1);
  context.model.model.destinations = {destination(1, 1, 10.0, 20.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout.has_value(),
         "lane-cover fixture publishes interaction geometry");
  if (!result.interactionLayout) {
    return;
  }
  const UiLogicalPoint dragPoint =
      uiPointForAuthored(viewport, 15.0, -10.0);
  const auto hit = result.interactionLayout->hitTestUiControl(dragPoint);
  const auto invocation = result.interactionLayout->writerInvocationFor(
      hit, dragPoint, 5'000);
  expect(hit.kind == PresentationUiControlKind::LaneCover && invocation &&
             invocation->writer == SkinFloatWriterId{1} &&
             near(invocation->normalizedValue, 0.75) &&
             invocation->eventMicros == 5'000,
         "lane-cover Rate property with a separate callback writer queues the drag");
}

void testRendererUsesLaneCoverRateIndexAndDirectPropertyFallback() {
  TestContext context;
  context.model.model.floatWriters.push_back(
      {.id = SkinFloatWriterId{1},
       .source = LuaCallbackId{.slot = 1, .generation = 1},
       .authoredOrdinal = 1});
  context.model.laneCoverRatePropertyIndexReady = true;
  context.model.laneCoverRatePropertyIds = {SkinFloatPropertyId{1}};
  addSlider(context.model, context.resources, 1, 1, 40.0, true,
            SkinFloatWriterId{1}, 1);
  context.model.model.destinations = {destination(1, 1, 10.0, 20.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});

  const auto indexed = context.evaluate(viewport);
  expect(indexed.interactionLayout &&
             indexed.interactionLayout->slidersTopmostFirst.size() == 1 &&
             indexed.interactionLayout->slidersTopmostFirst.front().kind ==
                 PresentationUiControlKind::LaneCover,
         "renderer uses the validated Rate-property index with a separate writer");

  context.model.laneCoverRatePropertyIndexReady = false;
  context.model.model.floatProperties.front().source =
      SkinBuiltinPropertySelector{.value = std::string("lanecover2")};
  const auto direct = context.evaluate(viewport);
  expect(direct.interactionLayout &&
             direct.interactionLayout->slidersTopmostFirst.size() == 1 &&
             direct.interactionLayout->slidersTopmostFirst.front().kind ==
                 PresentationUiControlKind::LaneCover,
         "direct models retain the exact linear Rate-property fallback");

  context.model.model.floatProperties.front().source =
      SkinBuiltinPropertySelector{.value = 400};
  context.model.model.floatWriters.front().source =
      SkinBuiltinPropertySelector{.value = 4};
  const auto writerOnly = context.evaluate(viewport);
  expect(writerOnly.interactionLayout &&
             writerOnly.interactionLayout->slidersTopmostFirst.size() == 1 &&
             writerOnly.interactionLayout->slidersTopmostFirst.front().kind ==
                 PresentationUiControlKind::Slider,
         "writer selector four alone never grants lane-cover identity");
}

void testInvalidSliderGeometryCannotCaptureGameplayTouch() {
  TestContext context;
  context.model.model.floatWriters.push_back(
      {.id = SkinFloatWriterId{1},
       .source = SkinBuiltinPropertySelector{.value = 40},
       .authoredOrdinal = 1});
  addSlider(context.model, context.resources, 1, 1, 0.0, true,
            SkinFloatWriterId{1}, 1);
  context.model.model.destinations = {destination(1, 1, 10.0, 20.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout
                     ->hitTestUiControl(
                         uiPointForAuthored(viewport, 10.0, 20.0))
                     .kind == PresentationUiControlKind::None,
         "zero-range slider cannot exclude gameplay without a writable value");
}

void testSliderHitsProduceQueueOnlyWriterInvocations() {
  TestContext context;
  for (std::uint32_t id = 1; id <= 4; ++id) {
    context.model.model.floatWriters.push_back(
        {.id = SkinFloatWriterId{id},
         .source = SkinBuiltinPropertySelector{.value = static_cast<int>(id)},
         .authoredOrdinal = id});
    addSlider(context.model, context.resources, id,
              static_cast<std::uint8_t>(id - 1), 40.0, true,
              SkinFloatWriterId{id}, id);
    context.model.model.destinations.push_back(
        destination(id, id, 10.0, 20.0));
  }
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout.has_value(),
         "slider invocation fixture publishes interaction geometry");
  if (!result.interactionLayout) {
    return;
  }

  struct Scenario {
    UiLogicalPoint point;
    SkinFloatWriterId writer;
    double value;
  };
  const std::vector<Scenario> scenarios{
      {.point = uiPointForAuthored(viewport, 10.0, 60.0),
       .writer = SkinFloatWriterId{1},
       .value = 1.0},
      {.point = uiPointForAuthored(viewport, 50.0, 20.0),
       .writer = SkinFloatWriterId{2},
       .value = 1.0},
      {.point = uiPointForAuthored(viewport, 10.0, -20.0),
       .writer = SkinFloatWriterId{3},
       .value = 1.0},
      {.point = uiPointForAuthored(viewport, -30.0, 20.0),
       .writer = SkinFloatWriterId{4},
       .value = 1.0},
  };
  for (const auto &scenario : scenarios) {
    const auto hit =
        result.interactionLayout->hitTestUiControl(scenario.point);
    const auto invocation = result.interactionLayout->writerInvocationFor(
        hit, scenario.point, 4'321);
    expect(invocation && invocation->writer == scenario.writer &&
               near(invocation->normalizedValue, scenario.value) &&
               invocation->eventMicros == 4'321,
           "each pinned slider direction queues its endpoint value without invoking a callback");
  }

  const UiLogicalPoint nearOrigin =
      uiPointForAuthored(viewport, 10.5, 20.0);
  const auto nearOriginHit =
      result.interactionLayout->hitTestUiControl(nearOrigin);
  const auto nearOriginInvocation =
      result.interactionLayout->writerInvocationFor(nearOriginHit, nearOrigin,
                                                     4'322);
  expect(nearOriginInvocation &&
             near(nearOriginInvocation->normalizedValue, 0.0),
         "slider values within one authored unit of the origin snap to zero");

  const auto nextFrame = context.evaluate(viewport);
  expect(nextFrame.interactionLayout &&
             nextFrame.interactionLayout->frameSerial !=
                 result.interactionLayout->frameSerial &&
             nextFrame.interactionLayout->revision ==
                 result.interactionLayout->revision &&
             nextFrame.interactionLayout->writerInvocationFor(
                 nearOriginHit, nearOrigin, 4'323),
         "a captured Down hit remains actionable after a normal render-frame publication");

  const auto changedLayout = context.evaluate(viewport, 2);
  expect(changedLayout.interactionLayout &&
             changedLayout.interactionLayout->revision !=
                 result.interactionLayout->revision &&
             !changedLayout.interactionLayout->writerInvocationFor(
                 nearOriginHit, nearOrigin, 4'324),
         "a captured hit from a genuinely changed interaction layout fails closed");

  auto staleHit = nearOriginHit;
  ++staleHit.layoutRevision;
  expect(!result.interactionLayout->writerInvocationFor(
             staleHit, nearOrigin, 4'325),
         "a hit from an older published layout cannot queue a writer");
  auto forgedHit = nearOriginHit;
  ++forgedHit.sourceObject;
  expect(!result.interactionLayout->writerInvocationFor(
             forgedHit, nearOrigin, 4'326),
         "a writer ID without the exact current source identity fails closed");
  expect(!result.interactionLayout->writerInvocationFor(
             nearOriginHit, uiPointForAuthored(viewport, 90.0, 45.0), 4'327),
         "a captured hit cannot queue a writer from outside its current region");
}

void testImplicitLaneCoverWithoutWriterStaysNonInteractive() {
  TestContext context;
  addSlider(context.model, context.resources, 9, 1, 25.0, true,
            std::nullopt, 1);
  context.model.model.destinations = {destination(9, 1, 5.0, 6.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout->slidersTopmostFirst.size() == 1 &&
             !result.interactionLayout->slidersTopmostFirst.front().writer &&
             result.interactionLayout->slidersTopmostFirst.front().changeable,
         "an authored changeable lane-cover slider remains visible when its "
         "implicit builtin writer is unavailable");
  if (result.interactionLayout) {
    expect(result.interactionLayout
                   ->hitTestUiControl(uiPointForAuthored(viewport, 5.0, 6.0))
                   .kind == PresentationUiControlKind::None,
           "an unavailable implicit lane-cover writer never captures a pointer");
  }
}

void testDroppedNoncriticalSliderPublishesNoInteractionGeometry() {
  TestContext context;
  constexpr SkinObjectId fillerId = 1;
  constexpr SkinObjectId sliderId = 2;
  context.resources.addImage(fillerId);
  context.model.model.objects.push_back(
      {.id = fillerId,
       .authoredName = "capacity-filler",
       .payload = SkinImageObject{.orderedStates = {sprite(fillerId)}},
       .authoredOrdinal = 1,
       .critical = true});
  addSlider(context.model, context.resources, sliderId, 1, 25.0, true,
            SkinFloatWriterId{1}, 2, false);
  context.model.model.destinations.reserve(SkinCommandPolicy::maximumCommands +
                                           1);
  for (std::size_t ordinal = 0;
       ordinal < SkinCommandPolicy::maximumCommands; ++ordinal) {
    context.model.model.destinations.push_back(
        destination(fillerId, static_cast<std::uint32_t>(ordinal), 0.0, 0.0));
  }
  context.model.model.destinations.push_back(
      destination(sliderId,
                  static_cast<std::uint32_t>(SkinCommandPolicy::maximumCommands),
                  5.0, 6.0));

  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.submitReady &&
             result.submitReady->commands.size() ==
                 SkinCommandPolicy::maximumCommands,
         "a noncritical command beyond the capacity is dropped without aborting the frame");
  expect(result.interactionLayout &&
             result.interactionLayout->slidersTopmostFirst.empty(),
         "a slider dropped from the command buffer cannot remain interactive");
}

SkinNoteObject noteObject(FakeResources &resources,
                          double laneBaseX = 10.0,
                          double groupBaseX = 8.0) {
  SkinNoteObject note;
  for (int laneIndex = 0; laneIndex < 2; ++laneIndex) {
    SkinLaneNotePresentation lane;
    lane.authoredLane = laneIndex;
    lane.laneDestination = {.x = laneBaseX + laneIndex * 20.0,
                            .y = 5.0,
                            .width = 18.0,
                            .height = 42.0};
    lane.authoredNoteHeight = 6.0;
    for (int kind = static_cast<int>(SkinNoteVisualKind::Normal);
         kind <= static_cast<int>(SkinNoteVisualKind::HcnReactive); ++kind) {
      const SkinResourceId resource =
          100 + static_cast<SkinResourceId>(laneIndex * 20 + kind);
      resources.addImage(resource);
      lane.visuals.emplace(static_cast<SkinNoteVisualKind>(kind),
                           sprite(resource));
    }
    note.lanes.push_back(std::move(lane));
  }
  for (int kind = static_cast<int>(SkinNoteLineKind::Group);
       kind <= static_cast<int>(SkinNoteLineKind::Time); ++kind) {
    SkinDestinationBody lineDestination;
    lineDestination.loop = -1;
    lineDestination.frames = {{.timeMillis = 0,
                               .x = 8.0,
                               .y = 5.0,
                               .width = 42.0,
                               .height = 2.0}};
    note.lines.push_back(
        {.kind = static_cast<SkinNoteLineKind>(kind),
         .sprite = std::nullopt,
         .laneGroupDestination =
             {.x = groupBaseX, .y = 5.0, .width = 42.0, .height = 42.0},
         .destination = std::move(lineDestination)});
  }
  return note;
}

void testLastNoteObjectOwnsStaticRegionsEvenWhenClipped() {
  TestContext context;
  context.model.model.objects.push_back(
      {.id = 10,
       .authoredName = "notes-first",
       .payload = noteObject(context.resources, 10.0, 8.0),
       .authoredOrdinal = 1,
       .critical = true});
  context.model.model.objects.push_back(
      {.id = 11,
       .authoredName = "notes-last",
       .payload = noteObject(context.resources, 60.0, 58.0),
       .authoredOrdinal = 2,
       .critical = true});
  context.model.model.destinations = {
      destination(10, 1, 0.0, 0.0, 100.0, 50.0),
      destination(11, 2, 500.0, 500.0, 100.0, 50.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout->laneRegions.size() == 2 &&
             result.interactionLayout->laneGroupRegions.size() == 1,
         "multiple Note objects publish one replacement lane layout");
  if (!result.interactionLayout ||
      result.interactionLayout->laneRegions.size() != 2 ||
      result.interactionLayout->laneGroupRegions.size() != 1) {
    return;
  }
  expect(result.interactionLayout->laneRegions[0].sourceObject == 11 &&
             near(result.interactionLayout->laneRegions[0].authoredRegion.x,
                  60.0) &&
             result.interactionLayout->laneGroupRegions[0].sourceObject == 11 &&
             near(result.interactionLayout->laneGroupRegions[0]
                      .authoredRegion.x,
                  58.0),
         "the last source-order Note owns static regions even when its draw destination is clipped");
}

void testNoteModelPublishesLaneAndLaneGroupRegions() {
  TestContext context;
  context.model.model.objects.push_back(
      {.id = 10,
       .authoredName = "notes",
       .payload = noteObject(context.resources),
       .authoredOrdinal = 1,
       .critical = true});
  context.model.model.destinations = {
      destination(10, 1, 0.0, 0.0, 100.0, 50.0)};
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0},
      {.x = 0.0, .y = 0.0, .width = 100.0, .height = 50.0}, {});
  const auto result = context.evaluate(viewport);
  expect(result.interactionLayout &&
             result.interactionLayout->laneRegions.size() == 2 &&
             result.interactionLayout->laneGroupRegions.size() == 1,
         "normalized Note geometry exposes the lane and group regions available for lane-cover touch planning");
  if (!result.interactionLayout ||
      result.interactionLayout->laneRegions.size() != 2 ||
      result.interactionLayout->laneGroupRegions.size() != 1) {
    return;
  }
  expect(result.interactionLayout->laneRegions[0].authoredLane == 0 &&
             near(result.interactionLayout->laneRegions[0].authoredRegion.x,
                  10.0) &&
             near(result.interactionLayout->laneRegions[1].authoredRegion.x,
                  30.0),
         "lane records preserve normalized authored lane indices and rectangles");
  expect(result.interactionLayout->laneGroupRegions[0].authoredGroup == 0 &&
             near(result.interactionLayout->laneGroupRegions[0]
                      .authoredRegion.width,
                  42.0) &&
             near(result.interactionLayout->laneGroupRegions[0]
                      .authoredRegion.height,
                  42.0),
         "group record uses the pinned getLaneGroupRegion source rectangle");
}

void testSingularViewportDoesNotPublishCommandsOrTouchGeometry() {
  TestContext context;
  context.model.model.floatWriters.push_back(
      {.id = SkinFloatWriterId{1},
       .source = SkinBuiltinPropertySelector{.value = 4},
       .authoredOrdinal = 1});
  addSlider(context.model, context.resources, 1, 1, 40.0, true,
            SkinFloatWriterId{1}, 1);
  context.model.model.destinations = {destination(1, 1, 10.0, 5.0)};
  PlaySkinViewport singular;
  singular.valid = true;
  singular.authoredToUi = {.m00 = 1.0,
                           .m01 = 2.0,
                           .tx = 0.0,
                           .m10 = 2.0,
                           .m11 = 4.0,
                           .ty = 0.0};
  singular.uiToAuthored = {};
  singular.safeUiBounds = {.x = 0.0,
                           .y = 0.0,
                           .width = 100.0,
                           .height = 50.0};
  const auto result = context.evaluate(singular);
  expect(!result.submitReady && !result.interactionLayout,
         "singular authored transform fails closed before publishing either queue");
}

} // namespace

int main() {
  testViewportInverseMappingUsesUiLogicalSafeArea();
  testSliderTracksPreservePinnedDirectionsAndEndpoints();
  testOverlappingSlidersPublishReverseAuthoredTopmostOrder();
  testImageActPublishesPointerDownHitGeometry();
  testLaneCoverWriterUsesTypedHitAndQueueOnlyDrag();
  testRendererUsesLaneCoverRateIndexAndDirectPropertyFallback();
  testInvalidSliderGeometryCannotCaptureGameplayTouch();
  testSliderHitsProduceQueueOnlyWriterInvocations();
  testImplicitLaneCoverWithoutWriterStaysNonInteractive();
  testDroppedNoncriticalSliderPublishesNoInteractionGeometry();
  testNoteModelPublishesLaneAndLaneGroupRegions();
  testLastNoteObjectOwnsStaticRegionsEvenWhenClipped();
  testSingularViewportDoesNotPublishCommandsOrTouchGeometry();
  return failures == 0 ? 0 : 1;
}
