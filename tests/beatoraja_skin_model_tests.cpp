#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/PomyuCharaCycles.h"
#include "skin/beatoraja/SkinModelValidator.h"
#include "skin/beatoraja/SkinCoverNormalization.h"
#include "lua_skin_binding_test_support.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-model-test-" + std::to_string(++serial));
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

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

BeatorajaSkinModelDecodeResult
decodeInlineModel(std::string_view sourceText,
                  BeatorajaSkinHeader *decodedHeader = nullptr) {
  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = *normalizePackageId("InlineModelContract").package;
  const fs::path source = temp.root() / "source";
  writeText(source / "skin/model.luaskin", sourceText);

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "inline model fixture snapshots");
  if (!snapshot.prepared) {
    return {};
  }

  const auto entry = *normalizeEntryPath(package, "skin/model.luaskin").entry;
  const auto makeFileSystem = [&]() {
    return LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  };
  auto runtimeFileSystem = makeFileSystem();
  auto reconciliationFileSystem = makeFileSystem();
  expect(runtimeFileSystem != nullptr && reconciliationFileSystem != nullptr,
         "inline model filesystems create");
  if (!runtimeFileSystem || !reconciliationFileSystem) {
    return {};
  }

  auto created =
      LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::Validation,
                              .fileSystem = std::move(runtimeFileSystem)});
  expect(created.runtime != nullptr, "inline model runtime creates");
  if (!created.runtime) {
    return {};
  }

  auto headerValue = created.runtime->loadHeader();
  expect(headerValue.value.has_value(), "inline model header executes");
  if (!headerValue.value) {
    return {};
  }
  LuaSkinTableDecoder decoder;
  const auto header = decoder.decodeHeader(*headerValue.value);
  expect(header.header.has_value(), "inline model header decodes");
  if (!header.header) {
    return {};
  }
  if (decodedHeader != nullptr) {
    *decodedHeader = *header.header;
  }
  headerValue.value.reset();

  const auto reconciled = reconcileSkinConfiguration(*header.header, nullptr,
                                                     *reconciliationFileSystem);
  expect(reconciled.configuration.has_value(),
         "inline model configuration reconciles");
  if (!reconciled.configuration) {
    return {};
  }

  auto configured = created.runtime->loadConfigured(*reconciled.configuration);
  expect(configured.value.has_value(), "inline model configured phase runs");
  if (!configured.value) {
    return {};
  }
  return decoder.decodeGameplay(*configured.value,
                                {.runtime = *created.runtime,
                                 .builtins = gameplaySkinBuiltinCatalog()});
}

const Json &expectedContract() {
  static const Json value = [] {
    const fs::path path =
        fs::path(ASOBMASHOW_SOURCE_DIR) /
        "tests/fixtures/beatoraja_skin/model/all_v1_objects.expected.json";
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "model expected representation opens");
    if (!input.good()) {
      return Json::object();
    }
    return Json::parse(input, nullptr, true, true);
  }();
  return value;
}

class ModelFixture {
public:
  ModelFixture()
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions = temp.root() / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("ModelContract").package) {
    const fs::path source = fs::path(ASOBMASHOW_SOURCE_DIR) /
                            "tests/fixtures/beatoraja_skin/lua/model";
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "model fixture snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  BeatorajaSkinModelDecodeResult decode() {
    if (!prepared) {
      return {};
    }
    auto runtimeFileSystem = fileSystem();
    auto reconciliationFileSystem = fileSystem();
    expect(runtimeFileSystem != nullptr && reconciliationFileSystem != nullptr,
           "model fixture filesystems create");
    if (!runtimeFileSystem || !reconciliationFileSystem) {
      return {};
    }

    auto created =
        LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::Validation,
                                .fileSystem = std::move(runtimeFileSystem)});
    expect(created.runtime != nullptr, "model fixture runtime creates");
    if (!created.runtime) {
      return {};
    }

    auto headerValue = created.runtime->loadHeader();
    expect(headerValue.value.has_value(), "model fixture header executes");
    if (!headerValue.value) {
      return {};
    }
    LuaSkinTableDecoder decoder;
    const auto header = decoder.decodeHeader(*headerValue.value);
    expect(header.header.has_value(), "model fixture header decodes");
    if (!header.header) {
      return {};
    }
    headerValue.value.reset();

    const auto reconciled = reconcileSkinConfiguration(
        *header.header, nullptr, *reconciliationFileSystem);
    expect(reconciled.configuration.has_value(),
           "model fixture configuration reconciles");
    if (!reconciled.configuration) {
      return {};
    }

    auto configured =
        created.runtime->loadConfigured(*reconciled.configuration);
    expect(configured.value.has_value(), "model fixture configured phase runs");
    if (!configured.value) {
      return {};
    }
    static const std::array modelFixtureBuiltins{
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::IntegerProperty,
                     .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
            .selector = SkinBuiltinPropertySelector{700}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::IntegerProperty,
                     .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
            .selector = SkinBuiltinPropertySelector{701}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::IntegerProperty,
                     .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
            .selector = SkinBuiltinPropertySelector{700}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::FloatProperty,
                     .floatDomain = SkinFloatPropertyDomain::FloatValue},
            .selector = SkinBuiltinPropertySelector{800}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::FloatProperty,
                     .floatDomain = SkinFloatPropertyDomain::Rate},
            .selector = SkinBuiltinPropertySelector{800}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::TimerProperty},
            .selector = SkinBuiltinPropertySelector{42}},
        SkinBuiltinBindingCatalogEntry{
            .type = {.kind = SkinBindingKind::TimerProperty},
            .selector = SkinBuiltinPropertySelector{43}},
    };
    return decoder.decodeGameplay(*configured.value,
                                  {.runtime = *created.runtime,
                                   .builtins = SkinBuiltinBindingCatalogView(
                                       modelFixtureBuiltins)});
  }

private:
  std::unique_ptr<LuaSkinFileSystem> fileSystem() {
    const auto entry =
        *normalizeEntryPath(package, "all_v1_objects.luaskin").entry;
    return LuaSkinFileSystem::create({.revision = prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

const BeatorajaSkinModelDecodeResult &decodedFixture() {
  static ModelFixture fixture;
  static const BeatorajaSkinModelDecodeResult decoded = fixture.decode();
  return decoded;
}

const SkinObjectDefinition *findObject(const BeatorajaSkinModel &model,
                                       std::string_view name) {
  const auto found = std::find_if(
      model.objects.begin(), model.objects.end(),
      [&](const auto &object) { return object.authoredName == name; });
  return found != model.objects.end() ? &*found : nullptr;
}

template <typename Binding>
std::optional<int> builtinIntegerSelector(const Binding &binding) {
  const auto *builtin =
      std::get_if<SkinBuiltinPropertySelector>(&binding.source);
  if (builtin == nullptr) {
    return std::nullopt;
  }
  const auto *value = std::get_if<int>(&builtin->value);
  return value != nullptr ? std::optional<int>(*value) : std::nullopt;
}

std::optional<int> timerSelector(const BeatorajaSkinModel &model,
                                 SkinTimerPropertyId id) {
  const auto found =
      std::find_if(model.timerProperties.begin(), model.timerProperties.end(),
                   [&](const auto &binding) { return binding.id == id; });
  return found != model.timerProperties.end() ? builtinIntegerSelector(*found)
                                              : std::nullopt;
}

void expectRect(const SkinSourceRect &actual, const Json &expected,
                std::string_view message) {
  expect(actual.x == expected.at("x").get<int>() &&
             actual.y == expected.at("y").get<int>() &&
             actual.w == expected.at("w").get<int>() &&
             actual.h == expected.at("h").get<int>() &&
             actual.gridColumn == expected.at("gridColumn").get<int>() &&
             actual.gridRow == expected.at("gridRow").get<int>() &&
             actual.gridColumns == expected.at("gridColumns").get<int>() &&
             actual.gridRows == expected.at("gridRows").get<int>(),
         message);
}

void expectFrame(const SkinDestinationFrame &actual, const Json &expected,
                 std::string_view message) {
  const auto rgba = expected.at("rgba");
  expect(actual.timeMillis == expected.at("timeMillis").get<int>() &&
             actual.x == expected.at("x").get<double>() &&
             actual.y == expected.at("y").get<double>() &&
             actual.width == expected.at("width").get<double>() &&
             actual.height == expected.at("height").get<double>() &&
             actual.rgba ==
                 std::array<std::uint8_t, 4>{rgba.at(0).get<std::uint8_t>(),
                                             rgba.at(1).get<std::uint8_t>(),
                                             rgba.at(2).get<std::uint8_t>(),
                                             rgba.at(3).get<std::uint8_t>()},
         message);
}

void testGameplayTimingPreservesDefaultsAndEveryAuthoredField() {
  const auto &expected = expectedContract();
  const SkinGameplayTiming defaults;
  const auto &defaultJson = expected.at("timingDefaults");
  expect(defaults.fadeoutMillis == defaultJson.at("fadeoutMillis") &&
             defaults.inputMillis == defaultJson.at("inputMillis") &&
             defaults.sceneMillis == defaultJson.at("sceneMillis") &&
             defaults.closeMillis == defaultJson.at("closeMillis") &&
             defaults.loadEndMillis == defaultJson.at("loadEndMillis") &&
             defaults.playStartMillis == defaultJson.at("playStartMillis") &&
             defaults.judgeTimerMillis == defaultJson.at("judgeTimerMillis") &&
             defaults.finishMarginMillis ==
                 defaultJson.at("finishMarginMillis"),
         "gameplay timing default values match pinned JsonSkin and PlaySkin");

  const auto &decoded = decodedFixture();
  expect(decoded.model.has_value(), "complete gameplay model decodes");
  if (!decoded.model) {
    return;
  }
  const auto &timing = decoded.model->timing;
  const auto &timingJson = expected.at("timing");
  expect(timing.fadeoutMillis == timingJson.at("fadeoutMillis") &&
             timing.inputMillis == timingJson.at("inputMillis") &&
             timing.sceneMillis == timingJson.at("sceneMillis") &&
             timing.closeMillis == timingJson.at("closeMillis") &&
             timing.loadEndMillis == timingJson.at("loadEndMillis") &&
             timing.playStartMillis == timingJson.at("playStartMillis") &&
             timing.judgeTimerMillis == timingJson.at("judgeTimerMillis") &&
             timing.finishMarginMillis == timingJson.at("finishMarginMillis"),
         "configured gameplay timing is independent of note construction");
}

void testPropertyInterningIncludesIntegerAndFloatDomains() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &model = *decoded.model;
  const auto &expected = expectedContract();
  const auto &integerJson = expected.at("integerBindings");
  const bool integerBindingCountMatches =
      model.integerProperties.size() == integerJson.size();
  expect(integerBindingCountMatches,
         "integer bindings are interned once per selector domain");
  if (!integerBindingCountMatches) {
    return;
  }
  for (std::size_t index = 0;
       index < model.integerProperties.size() && index < integerJson.size();
       ++index) {
    const auto &binding = model.integerProperties[index];
    const auto &entry = integerJson.at(index);
    const auto expectedDomain = entry.at("domain") == "ImageIndex"
                                    ? SkinIntegerPropertyDomain::ImageIndex
                                    : SkinIntegerPropertyDomain::IntegerValue;
    expect(binding.id.value == entry.at("id") &&
               binding.domain == expectedDomain &&
               builtinIntegerSelector(binding) ==
                   entry.at("selector").get<int>() &&
               binding.authoredOrdinal == entry.at("authoredOrdinal"),
           "integer binding retains selector, domain, stable ID, and order");
  }

  const auto &floatJson = expected.at("floatBindings");
  const bool floatBindingCountMatches =
      model.floatProperties.size() == floatJson.size();
  expect(floatBindingCountMatches,
         "float bindings are interned once per selector domain");
  if (!floatBindingCountMatches) {
    return;
  }
  for (std::size_t index = 0;
       index < model.floatProperties.size() && index < floatJson.size();
       ++index) {
    const auto &binding = model.floatProperties[index];
    const auto &entry = floatJson.at(index);
    const auto expectedDomain = entry.at("domain") == "FloatValue"
                                    ? SkinFloatPropertyDomain::FloatValue
                                    : SkinFloatPropertyDomain::Rate;
    expect(binding.id.value == entry.at("id") &&
               binding.domain == expectedDomain &&
               builtinIntegerSelector(binding) ==
                   entry.at("selector").get<int>() &&
               binding.authoredOrdinal == entry.at("authoredOrdinal"),
           "float binding retains selector, domain, stable ID, and order");
  }

  const auto *strip = findObject(model, "strip");
  const auto *stateSet = findObject(model, "state-set");
  const auto *score = findObject(model, "score");
  const auto *ratio = findObject(model, "ratio");
  const auto *slider = findObject(model, "rate-slider");
  expect(strip && stateSet && score && ratio && slider,
         "domain fixture objects exist");
  if (!strip || !stateSet || !score || !ratio || !slider) {
    return;
  }
  expect(std::get<SkinImageObject>(strip->payload).stateIndex ==
                 SkinIntegerPropertyId{1} &&
             std::get<SkinImageObject>(stateSet->payload).stateIndex ==
                 SkinIntegerPropertyId{2} &&
             std::get<SkinNumberObject>(score->payload).value ==
                 SkinIntegerPropertyId{3} &&
             std::get<SkinFloatObject>(ratio->payload).value ==
                 SkinFloatPropertyId{1} &&
             std::get<SkinFloatPropertyId>(
                 std::get<SkinSliderObject>(slider->payload).value) ==
                 SkinFloatPropertyId{2},
         "object references do not collapse equal selectors across domains");
}

void testStableIdsAndPerUseFrameExpansionAreSourceNeutral() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &model = *decoded.model;
  const auto &expected = expectedContract();
  const auto &resourceJson = expected.at("resources");
  const bool resourceCountMatches = model.resources.size() == resourceJson.size();
  expect(resourceCountMatches,
         "only authored Source records become image resources");
  if (!resourceCountMatches) {
    return;
  }
  for (std::size_t index = 0;
       index < model.resources.size() && index < resourceJson.size(); ++index) {
    const auto *resource =
        std::get_if<SkinImageResource>(&model.resources[index]);
    const auto &entry = resourceJson.at(index);
    expect(resource != nullptr && resource->id == entry.at("id") &&
               resource->virtualPath ==
                   entry.at("virtualPath").get<std::string>() &&
               resource->authoredOrdinal == entry.at("authoredOrdinal"),
           "resource IDs depend on authored source order, not host paths");
  }

  const auto &objectJson = expected.at("objects");
  const bool objectCountMatches = model.objects.size() == objectJson.size() &&
                                  model.destinations.size() == objectJson.size();
  expect(objectCountMatches,
         "effective object and destination vectors retain authored order");
  if (!objectCountMatches) {
    return;
  }
  for (std::size_t index = 0;
       index < model.objects.size() && index < objectJson.size(); ++index) {
    const auto &object = model.objects[index];
    const auto &entry = objectJson.at(index);
    expect(object.id == entry.at("id") &&
               object.authoredName == entry.at("name").get<std::string>() &&
               object.authoredOrdinal == entry.at("authoredOrdinal") &&
               object.critical == entry.at("critical") &&
               model.destinations[index].object == object.id,
           "stable object IDs follow effective destination order");
  }

  const auto *stripDefinition = findObject(model, "strip");
  expect(stripDefinition != nullptr, "partitioned image object exists");
  if (!stripDefinition) {
    return;
  }
  const auto &strip = std::get<SkinImageObject>(stripDefinition->payload);
  const auto &stateJson = expected.at("stripStates");
  const bool stripStateCountMatches =
      strip.orderedStates.size() == stateJson.size();
  expect(stripStateCountMatches,
         "Image len partitions one per-use row-major grid into states");
  if (!stripStateCountMatches) {
    return;
  }
  for (std::size_t state = 0;
       state < strip.orderedStates.size() && state < stateJson.size();
       ++state) {
    expect(strip.orderedStates[state].resource == 1,
           "shared atlas resource ID remains division-neutral");
    const bool frameCountMatches =
        strip.orderedStates[state].frames.size() == stateJson[state].size();
    expect(frameCountMatches,
           "partitioned state preserves its frame count");
    if (!frameCountMatches) {
      return;
    }
    for (std::size_t frame = 0;
         frame < strip.orderedStates[state].frames.size() &&
         frame < stateJson[state].size();
         ++frame) {
      expectRect(strip.orderedStates[state].frames[frame],
                 stateJson[state][frame],
                 "per-use strip division expands in row-major order");
    }
  }

}

void testImageSetPreservesIndependentStateResourcesCropsAndTimers() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &model = *decoded.model;
  const auto *definition = findObject(model, "state-set");
  expect(definition != nullptr, "ImageSet object exists");
  if (!definition) {
    return;
  }
  const auto &imageSet = std::get<SkinImageObject>(definition->payload);
  const auto &expected = expectedContract().at("imageSetStates");
  const bool imageSetStateCountMatches =
      imageSet.orderedStates.size() == expected.size();
  expect(imageSetStateCountMatches,
         "ImageSet retains one complete state per authored image ID");
  if (!imageSetStateCountMatches) {
    return;
  }
  for (std::size_t state = 0;
       state < imageSet.orderedStates.size() && state < expected.size();
       ++state) {
    const auto &actual = imageSet.orderedStates[state];
    const auto &entry = expected[state];
    const bool stateMatches =
        actual.resource == entry.at("resource") &&
            actual.cycleMillis == entry.at("cycleMillis") &&
            actual.timer.has_value() &&
            timerSelector(model, *actual.timer) ==
                entry.at("timerSelector").get<int>() &&
            actual.frames.size() == entry.at("frames").size();
    expect(stateMatches,
        "ImageSet state retains resource, cycle, timer, and crop cardinality");
    if (!stateMatches) {
      return;
    }
    for (std::size_t frame = 0;
         frame < actual.frames.size() && frame < entry.at("frames").size();
         ++frame) {
      expectRect(
          actual.frames[frame], entry.at("frames")[frame],
          "ImageSet state preserves per-use division and negative width");
    }
  }
}

void testDestinationDefaultsInheritanceAndStableTimeOrder() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &destinations = decoded.model->destinations;
  expect(destinations.size() >= 2 && destinations[0].object == 1 &&
             destinations[1].object == 2,
         "top-level destinations preserve authored draw order");
  if (destinations.size() < 2) {
    return;
  }

  const auto &expectedFrames = expectedContract().at("stripDestinationFrames");
  const auto &frames = destinations[0].presentation.frames;
  const bool destinationFrameCountMatches =
      frames.size() == expectedFrames.size();
  expect(destinationFrameCountMatches,
         "all inherited destination frames survive normalization");
  if (!destinationFrameCountMatches) {
    return;
  }
  for (std::size_t index = 0;
       index < frames.size() && index < expectedFrames.size(); ++index) {
    expectFrame(
        frames[index], expectedFrames[index],
        "destination inherits in authored order before stable time sort");
  }

  const auto &defaults = expectedContract().at("firstFrameDefaults");
  const auto &defaultBody = destinations[1].presentation;
  expect(defaultBody.frames.size() == 1,
         "empty first keyframe produces one default frame");
  if (defaultBody.frames.size() == 1) {
    const auto &frame = defaultBody.frames.front();
    const auto rgba = defaults.at("rgba");
    expect(
        frame.timeMillis == defaults.at("timeMillis") &&
            frame.x == defaults.at("x") && frame.y == defaults.at("y") &&
            frame.width == defaults.at("width") &&
            frame.height == defaults.at("height") &&
            frame.angleDegrees == defaults.at("angleDegrees") &&
            frame.acceleration == defaults.at("acceleration") &&
            frame.rgba ==
                std::array<std::uint8_t, 4>{rgba.at(0).get<std::uint8_t>(),
                                            rgba.at(1).get<std::uint8_t>(),
                                            rgba.at(2).get<std::uint8_t>(),
                                            rgba.at(3).get<std::uint8_t>()} &&
            defaultBody.loop == defaults.at("loop"),
        "first destination keyframe uses exact portable defaults");
  }
}

void testMaterializedSpriteFrameBudgetRejectsCopyAmplification() {
  const auto boundary = decodeInlineModel(R"lua(
local choices = {"atlas", "atlas"}
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "atlas", src = "source", w = 100000, h = 1,
            divx = 100000, divy = 1}},
  imageset = {{id = "states", images = choices}},
  destination = {{id = "states", dst = {{}}}},
}
)lua");
  expect(boundary.model.has_value(),
         "exact materialized sprite frame budget remains accepted");

  const auto repeatedImageSet = decodeInlineModel(R"lua(
local choices = {"atlas", "atlas"}
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "atlas", src = "source", w = 100001, h = 1,
            divx = 100001, divy = 1}},
  imageset = {{id = "states", images = choices}},
  destination = {{id = "states", dst = {{}}}},
}
)lua");
  expect(!repeatedImageSet.model && !repeatedImageSet.diagnostics.empty() &&
             repeatedImageSet.diagnostics.front().code ==
                 "skin_lua_model_limit_exceeded",
         "repeated ImageSet IDs cannot amplify legal frames past the copy "
         "budget");

  const auto noteAmplification = decodeInlineModel(R"lua(
local lanes = {}
local laneDestinations = {}
for i = 1, 167 do
  lanes[i] = "atlas"
  laneDestinations[i] = {x = i, y = 0, w = 1, h = 1}
end
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "atlas", src = "source", w = 100, h = 1,
            divx = 100, divy = 1}},
  note = {
    id = "notes", note = lanes, mine = lanes,
    lnend = lanes, lnstart = lanes, lnbodyActive = lanes, lnbody = lanes,
    hcnend = lanes, hcnstart = lanes, hcnbodyActive = lanes,
    hcnbody = lanes, hcnbodyMiss = lanes, hcnbodyReactive = lanes,
    dst = laneDestinations,
  },
  destination = {{id = "notes", dst = {{}}}},
}
)lua");
  expect(!noteAmplification.model && !noteAmplification.diagnostics.empty() &&
             noteAmplification.diagnostics.front().code ==
                 "skin_lua_model_limit_exceeded",
         "note lane visuals cannot amplify legal frames past the copy budget");
}

void testFullTextureSentinelsRemainDeferredAcrossSpriteDivisions() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "full", src = "source", x = 7, y = 11, w = -1, h = -1,
            divx = 2, divy = 3}},
  destination = {{id = "full", dst = {{}}}},
}

)lua");
  expect(decoded.model.has_value(),
         "full-texture sentinel model decodes before texture dimensions exist");
  if (!decoded.model || decoded.model->objects.empty()) {
    return;
  }
  const auto &states =
      std::get<SkinImageObject>(decoded.model->objects.front().payload)
          .orderedStates;
  expect(states.size() == 1 && states.front().frames.size() == 6,
         "deferred full-texture grid preserves its authored frame count");
  if (states.empty() || states.front().frames.size() != 6) {
    return;
  }
  const auto &first = states.front().frames.front();
  const auto &last = states.front().frames.back();
  expect(first.x == 7 && first.y == 11 && first.w == -1 && first.h == -1 &&
             first.gridColumn == 0 && first.gridRow == 0 &&
             first.gridColumns == 2 && first.gridRows == 3 && last.x == 7 &&
             last.y == 11 && last.w == -1 && last.h == -1 &&
             last.gridColumn == 1 && last.gridRow == 2 &&
             last.gridColumns == 2 && last.gridRows == 3,
         "divx/divy never divide or erase a full-texture -1 sentinel");
}

void testExplicitSpriteGridsRemainDeferredUntilTexturePreparation() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "grid", src = "source", x = 7, y = 11, w = 40, h = 20,
            divx = 2, divy = 2}},
  destination = {{id = "grid", dst = {{}}}},
}
)lua");
  expect(decoded.model.has_value(),
         "explicit grid model decodes before texture dimensions exist");
  if (!decoded.model || decoded.model->objects.empty()) {
    return;
  }
  const auto &frames =
      std::get<SkinImageObject>(decoded.model->objects.front().payload)
          .orderedStates.front()
          .frames;
  expect(frames.size() == 4 && frames.front().x == 7 &&
             frames.front().y == 11 && frames.front().w == 40 &&
             frames.front().h == 20 && frames.front().gridColumn == 0 &&
             frames.front().gridRow == 0 && frames.back().x == 7 &&
             frames.back().y == 11 && frames.back().w == 40 &&
             frames.back().h == 20 && frames.back().gridColumn == 1 &&
             frames.back().gridRow == 1,
         "explicit sprite grids preserve their full authored rectangle for "
         "texture-dimension resolution");
}

void testSparsePinnedBlendIdsMapExactlyAndRejectUnknownValues() {
  const auto decoded = decodeInlineModel(R"lua(
local authored = {0, 1, 2, 3, 4, 9}
local images = {}
local destinations = {}
for i, blend in ipairs(authored) do
  local id = "image-" .. i
  images[i] = {id = id, src = "source", w = 1, h = 1}
  destinations[i] = {id = id, blend = blend, dst = {{}}}
end
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = images,
  destination = destinations,
}
)lua");
  expect(decoded.model.has_value(), "all pinned blend IDs decode");
  if (decoded.model) {
    const std::array expected{
        SkinBlendMode::Normal,   SkinBlendMode::Normal,
        SkinBlendMode::Additive, SkinBlendMode::Subtractive,
        SkinBlendMode::Multiply, SkinBlendMode::Inverse,
    };
    expect(decoded.model->destinations.size() == expected.size(),
           "blend fixture preserves every authored destination");
    for (std::size_t index = 0;
         index < decoded.model->destinations.size() && index < expected.size();
         ++index) {
      expect(decoded.model->destinations[index].presentation.blend ==
                 expected[index],
             "sparse authored blend ID maps to the exact portable mode");
    }
  }

  const auto unsupported = decodeInlineModel(R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "image", src = "source", w = 1, h = 1}},
  destination = {{id = "image", blend = 5, dst = {{}}}},
}
)lua");
  expect(!unsupported.model && !unsupported.diagnostics.empty(),
         "unsupported blend IDs fail closed");
}

void testClipComponentsInheritUntilTheFirstCompleteRectangle() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "source", path = "atlas.png"}},
  image = {{id = "image", src = "source", w = 1, h = 1}},
  destination = {{
    id = "image",
    dst = {
      {time = 0, clip_x = 1},
      {time = 1, clip_y = 2, clip_w = 3},
      {time = 2, clip_h = 4},
      {time = 3, clip_x = 5},
    },
  }},
}
)lua");
  expect(decoded.model.has_value(),
         "partial clip components inherit without rejecting the model");
  if (!decoded.model || decoded.model->destinations.empty()) {
    return;
  }
  const auto &frames = decoded.model->destinations.front().presentation.frames;
  const auto clipEquals = [](const std::optional<SkinSourceRect> &clip, int x,
                             int y, int w, int h) {
    return clip && clip->x == x && clip->y == y && clip->w == w && clip->h == h;
  };
  expect(frames.size() == 4 && !frames[0].clip && !frames[1].clip &&
             clipEquals(frames[2].clip, 1, 2, 3, 4) &&
             clipEquals(frames[3].clip, 5, 2, 3, 4),
         "clip applies only after all four independently inherited fields "
         "exist");
}

void testLuaProtectedDecodeOwnsEveryRawParseTemporaryInTheRequest() {
  const fs::path path = fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "src/skin/beatoraja/LuaSkinTableDecoder.cpp";
  std::ifstream input(path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  const auto protectedStart = source.find("void decodeGameplayProtected(");
  const auto protectedEnd =
      source.find("\nLuaValuePath bindingPath(", protectedStart);
  expect(!source.empty() && protectedStart != std::string::npos &&
             protectedEnd != std::string::npos,
         "gameplay decoder source is available for the longjmp audit");
  if (protectedStart == std::string::npos ||
      protectedEnd == std::string::npos) {
    return;
  }
  const std::string_view protectedBody(source.data() + protectedStart,
                                       protectedEnd - protectedStart);
  for (const std::string_view field : {
           "std::vector<RawSkinSource> rawSources;",
           "std::vector<RawSkinImage> rawImages;",
           "std::vector<RawSkinImageSet> rawImageSets;",
           "std::vector<RawSkinNumber> rawNumbers;",
           "std::vector<RawSkinFloat> rawFloats;",
           "std::vector<RawSkinSlider> rawSliders;",
           "std::vector<RawDestination> rawDestinations;",
       }) {
    expect(source.find(field) != std::string::npos,
           "request declares every owning raw parse vector");
  }
  expect(source.find("std::vector<std::string> hidden;") != std::string::npos &&
             source.find("std::vector<std::string> processed;") !=
                 std::string::npos &&
             source.find("std::array<int, 2> expansionRatePercent") !=
                 std::string::npos &&
             source.find("std::vector<RawDestination> group;") !=
                 std::string::npos &&
             source.find("std::vector<RawDestination> time;") !=
                 std::string::npos,
         "raw note owns visual and nested line parse storage");
  expect(protectedBody.find("std::vector<Raw") == std::string_view::npos &&
             protectedBody.find("RawSkinNote note") == std::string_view::npos,
         "lua_cpcall callback has no owning raw parse automatic locals");
  const auto noteOwner = protectedBody.find("request->note.emplace();");
  const auto noteAccess = protectedBody.find(
      "rawGetField(state, index, \"note\", request->decoding)");
  expect(noteOwner != std::string_view::npos &&
             noteAccess != std::string_view::npos && noteOwner < noteAccess,
         "raw note ownership exists before the first fallible Lua access");
}

void testNoteHeightAndIgnoredAuthoredVisualsRemainExplicitPolicies() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &expected = expectedContract().at("note");
  const auto *definition = findObject(*decoded.model, "notes");
  expect(definition != nullptr && definition->id == expected.at("objectId") &&
             definition->critical,
         "note object has a stable critical identity");
  if (!definition) {
    return;
  }
  const auto &note = std::get<SkinNoteObject>(definition->payload);
  expect(note.lanes.size() == 1 &&
             note.lanes.front().authoredLane == expected.at("authoredLane") &&
             note.lanes.front().authoredNoteHeight ==
                 expected.at("authoredNoteHeight").get<double>(),
         "missing note size uses the known normal first-frame height");
  if (note.lanes.empty()) {
    return;
  }
  const auto &visuals = note.lanes.front().visuals;
  const auto hidden = visuals.find(SkinNoteVisualKind::Hidden);
  const auto processed = visuals.find(SkinNoteVisualKind::Processed);
  expect(
      hidden != visuals.end() && processed != visuals.end() &&
          std::holds_alternative<SkinSynthesizedNoteVisual>(hidden->second) &&
          std::holds_alternative<SkinSynthesizedNoteVisual>(processed->second),
      "pinned ignored hidden/processed fields produce synthesized visuals");

  const std::string diagnosticCode =
      expected.at("ignoredAuthoredDiagnosticCode").get<std::string>();
  const auto ignoredCount =
      std::count_if(decoded.diagnostics.begin(), decoded.diagnostics.end(),
                    [&](const auto &diagnostic) {
                      return diagnostic.code == diagnosticCode;
                    });
  expect(
      ignoredCount == 1,
      "authored hidden/processed divergence emits one deduplicated diagnostic");
}

void testValidatorRejectsCriticalNoteDependencyAndDisablesOptionalObject() {
  const auto &decoded = decodedFixture();
  if (!decoded.model) {
    return;
  }
  const auto &expected = expectedContract().at("validation");
  const auto optionalResult =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(!optionalResult.criticalFailure && optionalResult.model.has_value(),
         "missing optional image source does not reject the gameplay model");
  if (optionalResult.model) {
    const auto atlas = optionalResult.model->resourceIds.find("atlas");
    const auto alternate = optionalResult.model->resourceIds.find("alternate");
    const auto notes = optionalResult.model->objectIds.find("notes");
    const auto missing =
        optionalResult.model->objectIds.find("optional-missing");
    expect(atlas != optionalResult.model->resourceIds.end() &&
               atlas->second == 1 &&
               alternate != optionalResult.model->resourceIds.end() &&
               alternate->second == 2 &&
               notes != optionalResult.model->objectIds.end() &&
               notes->second == 6 &&
               missing != optionalResult.model->objectIds.end() &&
               missing->second == 7,
           "validator publishes stable source-neutral resource and object "
           "identity maps");
    expect(optionalResult.model->disabledOptionalObjects.empty(),
           "validator leaves prepare-time object omission to the renderer");
  }

  BeatorajaSkinModel missingCriticalNote = *decoded.model;
  const auto note = std::find_if(
      missingCriticalNote.objects.begin(), missingCriticalNote.objects.end(),
      [](const auto &object) { return object.authoredName == "notes"; });
  expect(note != missingCriticalNote.objects.end(),
         "critical mutation finds the note object");
  if (note == missingCriticalNote.objects.end()) {
    return;
  }
  auto &notePayload = std::get<SkinNoteObject>(note->payload);
  expect(!notePayload.lanes.empty(),
         "critical mutation finds the authored lane");
  if (notePayload.lanes.empty()) {
    return;
  }
  notePayload.lanes.front().visuals.erase(SkinNoteVisualKind::Normal);
  const auto criticalResult = test_support::validateWithAuthoredBuiltins(
      std::move(missingCriticalNote));
  expect(criticalResult.model && !criticalResult.criticalFailure &&
             criticalResult.model->disabledOptionalObjects.empty(),
         "missing Note presentation remains object-local like Skin.prepare");

  BeatorajaSkinModel duplicateBindings = *decoded.model;
  expect(!duplicateBindings.integerProperties.empty(),
         "duplicate-binding mutation requires a decoded integer binding");
  if (duplicateBindings.integerProperties.empty()) {
    return;
  }
  duplicateBindings.integerProperties.push_back(
      duplicateBindings.integerProperties.front());
  const auto duplicateResult =
      test_support::validateWithAuthoredBuiltins(std::move(duplicateBindings));
  expect(duplicateResult.criticalFailure && !duplicateResult.model,
         "duplicate typed binding IDs reject the model");

  BeatorajaSkinModel oversizedCanvas = *decoded.model;
  oversizedCanvas.header.width =
      LuaSkinTableDecoderPolicy::maxGameplayDimension + 1;
  const auto canvasResult =
      test_support::validateWithAuthoredBuiltins(std::move(oversizedCanvas));
  expect(canvasResult.criticalFailure && !canvasResult.model,
         "authored canvas axes enforce the model dimension limit");

  BeatorajaSkinModel wrongNumericDomain = *decoded.model;
  const auto score = std::find_if(
      wrongNumericDomain.objects.begin(), wrongNumericDomain.objects.end(),
      [](const auto &object) { return object.authoredName == "score"; });
  expect(score != wrongNumericDomain.objects.end(),
         "numeric-domain mutation finds the score object");
  if (score != wrongNumericDomain.objects.end()) {
    std::get<SkinNumberObject>(score->payload).value = SkinIntegerPropertyId{1};
    const auto domainResult = test_support::validateWithAuthoredBuiltins(
        std::move(wrongNumericDomain));
    expect(domainResult.model && !domainResult.criticalFailure &&
               domainResult.model->disabledOptionalObjects.empty(),
           "number binding domain remains a runtime object concern");
  }
}

void testValidatedLaneCoverRateIndexClassifiesOnlyPinnedSelectors() {
  BeatorajaSkinModel authored;
  authored.header.type = 0;
  authored.floatProperties = {
      {.id = SkinFloatPropertyId{90},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 5},
       .authoredOrdinal = 1},
      {.id = SkinFloatPropertyId{40},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 400},
       .authoredOrdinal = 2},
      {.id = SkinFloatPropertyId{70},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 4},
       .authoredOrdinal = 3},
      {.id = SkinFloatPropertyId{20},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = 6},
       .authoredOrdinal = 4},
      {.id = SkinFloatPropertyId{10},
       .domain = SkinFloatPropertyDomain::Rate,
       .source = SkinBuiltinPropertySelector{.value = std::string("lanecover")},
       .authoredOrdinal = 5},
      {.id = SkinFloatPropertyId{80},
       .domain = SkinFloatPropertyDomain::Rate,
       .source =
           SkinBuiltinPropertySelector{.value = std::string("lanecover2")},
       .authoredOrdinal = 6},
      {.id = SkinFloatPropertyId{30},
       .domain = SkinFloatPropertyDomain::FloatValue,
       .source = SkinBuiltinPropertySelector{.value = 4},
       .authoredOrdinal = 7},
  };

  const auto validated =
      test_support::validateWithAuthoredBuiltins(std::move(authored));
  expect(validated.model &&
             validated.model->laneCoverRatePropertyIndexReady,
         "validated models publish a Rate-property interaction index");
  if (!validated.model) {
    return;
  }
  expect(validated.model->laneCoverRatePropertyIds ==
             std::vector<SkinFloatPropertyId>{{10}, {70}, {80}, {90}},
         "validated Rate-property index is sorted and contains only the exact "
         "numeric or named lane-cover selectors");
}

void testLiveCoverJudgeAndBgaSpecialObjects() {
  constexpr std::string_view fixture = R"lua(
return {
  type=0,w=1280,h=720,source={{id='atlas',path='atlas.png'}},
  image={{id='judge-image',src='atlas',x=0,y=0,w=40,h=20,divx=2,timer=41,cycle=50,
          len=2,ref=911,act=912}},
  value={{id='judge-number',src='atlas',x=0,y=20,w=110,h=10,divx=11,
          ref=42,timer=43,cycle=60,digit=4,space=3}},
  hiddenCover={{id='hidden',src='atlas',x=0,y=40,w=20,h=10,divx=2,
                timer=51,cycle=70,disapearLine=33,isDisapearLineLinkLift=false},
               {id='hidden-default',src='atlas',x=0,y=40,w=20,h=10}},
  liftCover={{id='lift',src='atlas',x=0,y=50,w=20,h=10,divy=2,
             timer=52,cycle=71,disapearLine=44,isDisapearLineLinkLift=true},
             {id='lift-default',src='atlas',x=0,y=50,w=20,h=10}},
  bga={id='bga'},
  judge={{id='judge',index=2,shift=true,
          images={{id='judge-image',timer=81,op={32},draw=33,
                   dst={{time=20,x=100,y=200,w=30,h=40}}},{id='missing-image',dst={{}}}},
          numbers={{id='judge-number',timer=82,draw=33,
                    dst={{time=10,x=80,y=210,w=20,h=8}}},{id='missing-number',dst={{}}}}}},
  destination={
    {id='hidden',offsets={99},offset=98,dst={{x=1,y=2,w=3,h=4}}},
    {id='lift',offsets={97},offset=96,dst={{}}},
    {id='hidden-default',dst={{}}},
    {id='lift-default',dst={{}}},
    {id='bga',dst={{}}},
    {id='judge',dst={{}}}
  }
})lua";
  const auto decoded = decodeInlineModel(fixture);
  expect(decoded.model.has_value(),
         "live hidden/lift, sparse Judge, and BGA fixture decodes");
  if (!decoded.model) {
    return;
  }
  const auto hidden = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "hidden"; });
  const auto lift = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "lift"; });
  const auto bga = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "bga"; });
  const auto judge = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "judge"; });
  const auto hiddenDefault = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "hidden-default"; });
  const auto liftDefault = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
      [](const auto &object) { return object.authoredName == "lift-default"; });
  expect(hidden != decoded.model->objects.end() && lift != decoded.model->objects.end() &&
             bga != decoded.model->objects.end() && judge != decoded.model->objects.end(),
         "every live special destination resolves to an object");
  if (hidden != decoded.model->objects.end()) {
    const auto *cover = std::get_if<SkinCoverObject>(&hidden->payload);
    const auto &destination = decoded.model->destinations[0].presentation;
    expect(cover && cover->kind == SkinCoverKind::Hidden &&
               cover->sprite.frames.size() == 2 && cover->sprite.timer &&
               cover->sprite.timer->value != 0 && cover->sprite.cycleMillis == 70 &&
               cover->disappearLine == 33.0 && !cover->disappearLineLinksLift &&
               destination.offsetIds == std::vector<int>{99, 98, kSkinCoverLiftOffsetId,
                                                          kSkinCoverHiddenOffsetId},
           "Hidden preserves sprite/timer/cycle/line/link and synthesizes offsets");
  }
  if (lift != decoded.model->objects.end()) {
    const auto *cover = std::get_if<SkinCoverObject>(&lift->payload);
    const auto &destination = decoded.model->destinations[1].presentation;
    expect(cover && cover->kind == SkinCoverKind::Lift && cover->sprite.frames.size() == 2 &&
               cover->sprite.timer && cover->sprite.cycleMillis == 71 &&
               cover->disappearLine == 44.0 && cover->disappearLineLinksLift &&
               destination.offsetIds == std::vector<int>{97, 96, kSkinCoverLiftOffsetId},
           "Lift preserves its own cover semantics and only synthesizes Lift offset");
  }
  expect(hiddenDefault != decoded.model->objects.end() &&
             liftDefault != decoded.model->objects.end() &&
             std::get<SkinCoverObject>(hiddenDefault->payload).disappearLineLinksLift &&
             !std::get<SkinCoverObject>(liftDefault->payload).disappearLineLinksLift,
         "omitted Hidden and Lift link fields retain their distinct pinned defaults");
  expect(bga != decoded.model->objects.end() && std::holds_alternative<SkinBgaObject>(bga->payload),
         "BGA decodes as an identity-only marker");
  if (judge != decoded.model->objects.end()) {
    const auto *payload = std::get_if<SkinJudgeObject>(&judge->payload);
    expect(payload && payload->grades.size() == 7 && payload->player == 2 &&
               payload->shiftImageByHalfDetailWidth && payload->grades[0].image &&
               payload->grades[0].detailNumber && !payload->grades[1].image &&
               !payload->grades[1].detailNumber,
           "Judge retains first seven sparse independently optional grade children");
    if (payload && payload->grades[0].image) {
      const auto child = std::find_if(decoded.model->objects.begin(), decoded.model->objects.end(),
          [&](const auto &object) { return object.id == payload->grades[0].image->object; });
      const auto *image = child != decoded.model->objects.end()
                              ? std::get_if<SkinImageObject>(&child->payload)
                              : nullptr;
      expect(image && image->orderedStates.size() == 1 &&
                 image->orderedStates.front().frames.size() == 2 &&
                 !image->stateIndex && !image->clickEvent,
             "Judge child image ignores referenced Image len/ref/act semantics");
    }
    if (payload && payload->grades[0].image &&
        payload->grades[0].detailNumber) {
      const auto &imageDestination = payload->grades[0].image->destination;
      const auto &numberDestination = payload->grades[0].detailNumber->destination;
      const auto *imageCondition = imageDestination.conditions.empty()
                                       ? nullptr
                                       : std::get_if<SkinBooleanPropertyId>(
                                             &imageDestination.conditions.front());
      expect(imageDestination.timer && imageDestination.timer->value != 0 &&
                 timerSelector(*decoded.model, *imageDestination.timer) == 81 &&
                 imageCondition && imageCondition->value != 0 &&
                 !decoded.model->booleanProperties.empty() &&
                 imageDestination.drawCondition &&
                 imageDestination.drawCondition->value != 0 &&
                 numberDestination.timer && numberDestination.timer->value != 0 &&
                 timerSelector(*decoded.model, *numberDestination.timer) == 82 &&
                 numberDestination.drawCondition &&
                 numberDestination.drawCondition->value != 0,
             "Judge child destinations retain typed timer, op, and draw bindings");
      const auto validated =
          test_support::validateWithAuthoredBuiltins(*decoded.model);
      expect(validated.model && !validated.criticalFailure,
             "Judge child destination bindings remain validator-live");
    }
  }
  BeatorajaSkinModel invalidChild = *decoded.model;
  const auto outer = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
      [](const auto &object) { return object.authoredName == "judge"; });
  if (outer != invalidChild.objects.end()) {
    const auto &outerJudge = std::get<SkinJudgeObject>(outer->payload);
    if (outerJudge.grades[0].image) {
      const auto child = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
          [&](const auto &object) { return object.id == outerJudge.grades[0].image->object; });
      if (child != invalidChild.objects.end()) {
        std::get<SkinImageObject>(child->payload).orderedStates.clear();
        const auto validated = test_support::validateWithAuthoredBuiltins(
            std::move(invalidChild));
        expect(validated.model && !validated.criticalFailure &&
                   validated.model->disabledOptionalObjects.empty(),
               "Judge child presentation remains object-local like Skin.prepare");
      }
    }
  }
}

void testLiveGenericObjectPrecedesSameIdGameplaySpecials() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,source={{id='atlas',path='atlas.png'}},
  image={{id='shared',src='atlas',w=10,h=10}},
  hiddenCover={{id='shared',src='atlas',w=10,h=10}},
  bga={id='shared'}, judge={{id='shared'}},
  destination={{id='shared',dst={{}}}}
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 1 &&
             std::holds_alternative<SkinImageObject>(
                 decoded.model->objects.front().payload),
         "generic Image wins same-ID Hidden/BGA/Judge specials without rejecting the model");
}

void testUnresolvedDestinationMatchesPinnedLoaderIgnoreBehavior() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},
  image={{id='known',src='atlas',w=10,h=10}},
  destination={
    {id='lamp',dst={{x=1,y=2,w=3,h=4}}},
    {id='known',dst={{x=5,y=6,w=7,h=8}}}
  }
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 1 &&
             decoded.model->destinations.size() == 1 &&
             decoded.model->objects.front().authoredName == "known",
         "an unresolved destination is ignored while later resolved "
         "destinations remain live");
  expect(std::ranges::find_if(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_unsupported_object";
         }) == decoded.diagnostics.end(),
         "unresolved destinations do not become model validation errors");
}

void testDuplicateSourceDeclarationsUsePinnedLastDefinition() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={
    {id="shared",path="first.png"},
    {id="shared",path="last.png"}
  },
  image={{id="visible",src="shared",x=0,y=0,w=1,h=1}},
  destination={{id="visible",dst={{x=0,y=0,w=1,h=1}}}}
}
)lua");
  expect(decoded.model.has_value(),
         "duplicate source declarations decode before model validation");
  if (!decoded.model) {
    return;
  }
  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(!validated.criticalFailure && validated.model.has_value(),
         "duplicate source declarations validate like JSONSkinLoader's "
         "last-definition source map");
  if (!validated.model) {
    return;
  }
  const auto source = validated.model->resourceIds.find("shared");
  expect(source != validated.model->resourceIds.end() && source->second == 2,
         "validated source lookup retains the final duplicate declaration");
}

void testDuplicateImageDeclarationsUsePinnedFirstDefinition() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id="first",path="first.png"},{id="second",path="second.png"}},
  image={
    {id="visible",src="first",x=0,y=0,w=1,h=1},
    {id="visible",src="second",x=0,y=0,w=1,h=1}
  },
  destination={{id="visible",dst={{x=0,y=0,w=1,h=1}}}}
}
)lua");
  expect(decoded.model.has_value(),
         "duplicate image declarations decode like JsonSkinObjectLoader");
  if (!decoded.model || decoded.model->objects.empty()) {
    return;
  }
  const auto *image = std::get_if<SkinImageObject>(
      &decoded.model->objects.front().payload);
  expect(image && image->orderedStates.size() == 1 &&
             image->orderedStates.front().resource == SkinResourceId{1},
         "duplicate image declarations retain the first authored definition");
}

void testLiveDestinationMouseRectAndCenterNormalization() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,source={{id='atlas',path='atlas.png'}},
  image={
    {id='normal',src='atlas',w=10,h=10},
    {id='negative-center',src='atlas',w=10,h=10},
    {id='large-center',src='atlas',w=10,h=10},
    {id='judge-image',src='atlas',w=10,h=10},
    {id='note-image',src='atlas',w=10,h=10},
    {id='line-image',src='atlas',w=10,h=10}
  },
  note={id='notes',note={'note-image'},mine={'note-image'},
        lnend={'note-image'},lnstart={'note-image'},lnbody={'note-image'},
        lnactive={'note-image'},hcnend={'note-image'},hcnstart={'note-image'},
        hcnbody={'note-image'},hcnactive={'note-image'},
        hcndamage={'note-image'},hcnreactive={'note-image'},
        dst={{x=10,y=20,w=30,h=40}},
        group={{id='line-image',mouseRect={x=21,y=22,w=23,h=24},
                dst={{x=1,y=2,w=3,h=4}}}}},
  judge={{id='judge',images={{id='judge-image',
          mouseRect={x=11,y=12,w=13,h=14},dst={{}}}},numbers={{id='none',dst={{}}}}}},
  destination={
    {id='normal',center=9,mouseRect={x=1,y=2,w=3,h=4},dst={{}}},
    {id='negative-center',center=-1,dst={{}}},
    {id='large-center',center=10,dst={{}}},
    {id='notes',dst={{}}},{id='judge',dst={{}}}
  }
}
)lua");
  expect(decoded.model && decoded.model->destinations.size() == 5,
         "mouse-rectangle and center fixture decodes");
  if (!decoded.model || decoded.model->destinations.size() != 5) {
    return;
  }
  const auto &destinations = decoded.model->destinations;
  expect(destinations[0].presentation.center == 9 &&
             destinations[0].presentation.mouseRect &&
             destinations[0].presentation.mouseRect->x == 1 &&
             destinations[0].presentation.mouseRect->y == 2 &&
             destinations[0].presentation.mouseRect->width == 3 &&
             destinations[0].presentation.mouseRect->height == 4 &&
             destinations[1].presentation.center == 0 &&
             destinations[2].presentation.center == 0,
         "destination mouseRect is preserved and center is clamped to pinned 0..9 semantics");
  const auto *judgeDefinition = findObject(*decoded.model, "judge");
  if (judgeDefinition != nullptr) {
    const auto &judge = std::get<SkinJudgeObject>(judgeDefinition->payload);
    expect(judge.grades[0].image && judge.grades[0].image->destination.mouseRect &&
               judge.grades[0].image->destination.mouseRect->x == 11 &&
               judge.grades[0].image->destination.mouseRect->height == 14,
           "nested Judge destinations use shared mouseRect decoding");
  }
  const auto *noteDefinition = findObject(*decoded.model, "notes");
  if (noteDefinition != nullptr) {
    const auto &note = std::get<SkinNoteObject>(noteDefinition->payload);
    expect(!note.lines.empty() && note.lines.front().destination &&
               note.lines.front().destination->mouseRect &&
               note.lines.front().destination->mouseRect->x == 21 &&
               note.lines.front().destination->mouseRect->height == 24,
           "nested Note-line destinations use shared mouseRect decoding");
  }
}

void testGameplayTypeMustBeSupported() {
  BeatorajaSkinHeader header;
  const auto decoded = decodeInlineModel(
      "return {type=18,w=1280,h=720,destination={}}", &header);
  expect(header.type == 18,
         "header-only decoding continues to accept non-gameplay types");
  expect(!decoded.model && !decoded.diagnostics.empty() &&
             decoded.diagnostics.front().code ==
                 "skin_lua_model_type_unsupported",
         "gameplay decoding rejects non-gameplay skin types");

  auto valid = decodeInlineModel(
      "return {type=1,w=1280,h=720,destination={}}");
  expect(valid.model.has_value(),
         "supported gameplay type decodes before validation mutation");
  if (valid.model) {
    valid.model->header.type = 18;
    const auto validated = test_support::validateWithAuthoredBuiltins(
        std::move(*valid.model));
    expect(validated.criticalFailure && !validated.model &&
               !validated.diagnostics.empty() &&
               validated.diagnostics.front().code ==
                   "skin_lua_model_type_unsupported",
           "model validation independently enforces a supported gameplay type gate");
  }
}

void testBooleanFieldsUseLuaTruthiness() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},
  slider={{id='truthy-slider',src='atlas',w=10,h=10,type=800,changeable=0}},
  destination={{id='truthy-slider',dst={{}}}}
}
)lua");
  expect(decoded.model.has_value(),
         "non-Boolean Lua values decode in Boolean schema fields");
  if (!decoded.model) {
    return;
  }
  const auto *definition = findObject(*decoded.model, "truthy-slider");
  expect(definition != nullptr, "truthy slider definition exists");
  if (definition == nullptr) {
    return;
  }
  const auto *slider = std::get_if<SkinSliderObject>(&definition->payload);
  expect(slider != nullptr && slider->changeable,
         "Boolean schema fields retain LuaJ's non-nil, non-false truthiness");
}

void testPracticePositionSliderDecodesItsExecutableWriter() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},
  slider={{id='practice-scroll',src='atlas',w=10,h=10,type=20,
           changeable=true}},
  destination={{id='practice-scroll',dst={{}}}}
}
)lua");
  expect(decoded.model.has_value(), "practice-position slider model decodes");
  if (!decoded.model) {
    return;
  }
  const auto *definition = findObject(*decoded.model, "practice-scroll");
  const auto *slider = definition != nullptr
                           ? std::get_if<SkinSliderObject>(&definition->payload)
                           : nullptr;
  expect(slider != nullptr && slider->writer.has_value() &&
             decoded.model->floatWriters.size() == 1,
         "practice-position slider retains its executable float writer");
  if (decoded.model->floatWriters.size() == 1) {
    const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
        &decoded.model->floatWriters.front().source);
    const auto *selector =
        builtin != nullptr ? std::get_if<int>(&builtin->value) : nullptr;
    expect(selector != nullptr && *selector == 20,
           "practice-position slider binds its authored type as writer source");
  }
}

void testJudgeGraphsDecodePinnedModesAndPresentationFlags() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  judgegraph={
    {id='normal',backTexOff=2,orderReverse=-1,noGap=2,noGapX=-1},
    {id='judge',type=1,backTexOff=1,delay=750,orderReverse=1,noGap=1,noGapX=1},
    {id='early-late',type=2,delay=0},
  },
  destination={
    {id='normal',dst={{x=0,y=0,w=100,h=50}}},
    {id='judge',dst={{x=100,y=0,w=100,h=50}}},
    {id='early-late',dst={{x=200,y=0,w=100,h=50}}},
  }
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 3,
         "all three pinned judgegraph modes remain live model objects");
  if (!decoded.model || decoded.model->objects.size() != 3) {
    return;
  }
  const auto *normalDefinition = findObject(*decoded.model, "normal");
  const auto *judgeDefinition = findObject(*decoded.model, "judge");
  const auto *earlyLateDefinition = findObject(*decoded.model, "early-late");
  const auto *normal = normalDefinition != nullptr
                           ? std::get_if<SkinNoteDistributionGraphObject>(
                                 &normalDefinition->payload)
                           : nullptr;
  const auto *judge = judgeDefinition != nullptr
                          ? std::get_if<SkinNoteDistributionGraphObject>(
                                &judgeDefinition->payload)
                          : nullptr;
  const auto *earlyLate = earlyLateDefinition != nullptr
                              ? std::get_if<SkinNoteDistributionGraphObject>(
                                    &earlyLateDefinition->payload)
                              : nullptr;
  expect(normal != nullptr &&
             normal->type == SkinNoteDistributionGraphType::Normal &&
             !normal->backgroundTextureOff && normal->delayMillis == 500 &&
             !normal->reverseOrder && !normal->noGap &&
             !normal->noHorizontalGap,
         "judgegraph defaults match SkinNoteDistributionGraph");
  expect(judge != nullptr &&
             judge->type == SkinNoteDistributionGraphType::Judge &&
             judge->backgroundTextureOff && judge->delayMillis == 750 &&
             judge->reverseOrder && judge->noGap &&
             judge->noHorizontalGap,
         "judgegraph authored flags retain their exact equals-one semantics");
  expect(earlyLate != nullptr &&
             earlyLate->type == SkinNoteDistributionGraphType::EarlyLate &&
             earlyLate->delayMillis == 0,
         "early-late judgegraph decodes independently of judge mode");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_judgegraph_unsupported";
         }),
         "supported judgegraphs no longer emit the legacy unsupported diagnostic");

  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::none_of(validated.model->model.objects,
                                  [](const auto &object) {
                                    return std::holds_alternative<
                                        SkinBlankObject>(object.payload);
                                  }),
         "valid judgegraphs remain typed through model validation");

  auto invalidModel = *decoded.model;
  auto &invalidGraph = std::get<SkinNoteDistributionGraphObject>(
      invalidModel.objects.front().payload);
  invalidGraph.type = static_cast<SkinNoteDistributionGraphType>(9);
  const auto invalid = test_support::validateWithAuthoredBuiltins(
      std::move(invalidModel));
  expect(invalid.criticalFailure && !invalid.model &&
             std::ranges::find_if(invalid.diagnostics, [](const auto &entry) {
               return entry.code == "skin_lua_model_judgegraph_invalid";
             }) != invalid.diagnostics.end(),
         "model validation rejects an out-of-range judgegraph mode");

  const auto invalidDecode = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  judgegraph={{id='invalid',type=3}},
  destination={{id='invalid',dst={{x=0,y=0,w=10,h=10}}}}
}
)lua");
  expect(!invalidDecode.model &&
             std::ranges::find_if(invalidDecode.diagnostics,
                                  [](const auto &entry) {
                                    return entry.code ==
                                           "skin_lua_model_judgegraph_invalid";
                                  }) != invalidDecode.diagnostics.end(),
         "decoder rejects a judgegraph constructor mode absent from the pinned implementation");
}

void testTimingVisualizerDecodesPinnedPresentationFields() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingvisualizer={
    {id='default'},
    {id='custom',width=401,judgeWidthMillis=200,lineWidth=9,
     lineColor='11223344',centerColor='55667788',PGColor='01020304',
     GRColor='11121314',GDColor='21222324',BDColor='31323334',
     PRColor='41424344',transparent=1,drawDecay=0},
    {id='invalid-colour',lineColor='not-a-colour'}
  },
  destination={
    {id='default',dst={{x=0,y=0,w=100,h=50}}},
    {id='custom',dst={{x=0,y=0,w=100,h=50}}},
    {id='invalid-colour',dst={{x=0,y=0,w=100,h=50}}}
  }
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 3,
         "timingvisualizer declarations create one live object per destination");
  if (!decoded.model || decoded.model->objects.size() != 3) {
    return;
  }
  const auto *defaults = findObject(*decoded.model, "default");
  const auto *custom = findObject(*decoded.model, "custom");
  const auto *invalid = findObject(*decoded.model, "invalid-colour");
  const auto *defaultTiming = defaults != nullptr
                                  ? std::get_if<SkinTimingVisualizerObject>(
                                        &defaults->payload)
                                  : nullptr;
  const auto *customTiming = custom != nullptr
                                 ? std::get_if<SkinTimingVisualizerObject>(
                                       &custom->payload)
                                 : nullptr;
  const auto *invalidTiming = invalid != nullptr
                                  ? std::get_if<SkinTimingVisualizerObject>(
                                        &invalid->payload)
                                  : nullptr;
  expect(defaultTiming != nullptr && defaultTiming->width == 301 &&
             defaultTiming->judgeWidthMillis == 150 &&
             defaultTiming->lineWidth == 1 &&
             defaultTiming->lineRgba == 0x00ff00ffU &&
             defaultTiming->centerRgba == 0xffffffffU &&
             defaultTiming->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x000088ffU, 0x008800ffU,
                                               0x888800ffU, 0x880000ffU,
                                               0x000000ffU} &&
             !defaultTiming->transparent && defaultTiming->drawDecay,
         "timingvisualizer defaults retain the pinned constructor values");
  expect(customTiming != nullptr && customTiming->width == 401 &&
             customTiming->judgeWidthMillis == 200 &&
             customTiming->lineWidth == 4 &&
             customTiming->lineRgba == 0x11223344U &&
             customTiming->centerRgba == 0x55667788U &&
             customTiming->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x01020304U, 0x11121314U,
                                               0x21222324U, 0x31323334U,
                                               0U} &&
             customTiming->transparent && !customTiming->drawDecay,
         "timingvisualizer clamps line width, preserves custom non-poor colours, and applies flags");
  expect(invalidTiming != nullptr && invalidTiming->lineRgba == 0xff0000ffU,
         "timingvisualizer invalid colours use the pinned opaque-red fallback");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_timingvisualizer_unsupported";
         }),
         "supported timingvisualizers never emit the legacy unsupported diagnostic");

  const auto validated = test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::all_of(validated.model->model.objects,
                                 [](const auto &object) {
                                   return !std::holds_alternative<SkinBlankObject>(
                                       object.payload);
                                 }),
         "valid timingvisualizers remain typed through model validation");
}

void testBpmGraphsDecodePinnedDefaultsNormalizationAndValidation() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  bpmgraph={
    {id='default'},
    {id='fallbacks',delay=-5,lineWidth=0,
     mainBPMColor='ghijkl',minBPMColor='---',maxBPMColor=' ',
     otherBPMColor='?',stopLineColor='xyz',transitionLineColor=''},
    {id='custom',delay=750,lineWidth=3,
     mainBPMColor='##12-34-56-78',minBPMColor='aaBBcc',
     maxBPMColor='DEADBE-EF',otherBPMColor='010203',
     stopLineColor='f0-e1-d2',transitionLineColor='ABCDEF'}
  },
  destination={
    {id='default',dst={{x=0,y=0,w=100,h=50}}},
    {id='fallbacks',dst={{x=100,y=0,w=100,h=50}}},
    {id='custom',dst={{x=200,y=0,w=100,h=50}}}
  }
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 3,
         "bpmgraph declarations create one typed object per destination");
  if (!decoded.model || decoded.model->objects.size() != 3) {
    return;
  }
  const auto payload = [&](std::string_view id) {
    const auto *definition = findObject(*decoded.model, id);
    return definition != nullptr
               ? std::get_if<SkinBpmGraphObject>(&definition->payload)
               : nullptr;
  };
  const auto *defaults = payload("default");
  const auto *fallbacks = payload("fallbacks");
  const auto *custom = payload("custom");
  expect(defaults != nullptr && defaults->delayMillis == 0 &&
             defaults->lineWidth == 2 &&
             defaults->mainRgba == 0x00ff00ffU &&
             defaults->minimumRgba == 0x0000ffffU &&
             defaults->maximumRgba == 0xff0000ffU &&
             defaults->otherRgba == 0xffff00ffU &&
             defaults->stopRgba == 0xff00ffffU &&
             defaults->transitionRgba == 0x7f7f7fffU,
         "bpmgraph defaults match the pinned constructor palette and timing");
  expect(fallbacks != nullptr && fallbacks->delayMillis == 0 &&
             fallbacks->lineWidth == 2 &&
             fallbacks->mainRgba == 0x00ff00ffU &&
             fallbacks->minimumRgba == 0x0000ffffU &&
             fallbacks->maximumRgba == 0xff0000ffU &&
             fallbacks->otherRgba == 0xffff00ffU &&
             fallbacks->stopRgba == 0xff00ffffU &&
             fallbacks->transitionRgba == 0x7f7f7fffU,
         "nonpositive geometry and empty normalized colours retain pinned defaults");
  expect(custom != nullptr && custom->delayMillis == 750 &&
             custom->lineWidth == 3 && custom->mainRgba == 0x123456ffU &&
             custom->minimumRgba == 0xaabbccffU &&
             custom->maximumRgba == 0xdeadbeffU &&
             custom->otherRgba == 0x010203ffU &&
             custom->stopRgba == 0xf0e1d2ffU &&
             custom->transitionRgba == 0xabcdefffU,
         "bpmgraph strips nonhex characters and truncates normalized colours to six digits");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_bpmgraph_unsupported";
         }),
         "typed bpmgraphs emit no legacy unsupported diagnostic");

  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::all_of(validated.model->model.objects,
                                 [](const auto &object) {
                                   return !std::holds_alternative<
                                       SkinBlankObject>(object.payload);
                                 }),
         "valid bpmgraphs remain typed through model validation");

  auto invalidModel = *decoded.model;
  std::get<SkinBpmGraphObject>(invalidModel.objects.front().payload)
      .lineWidth = 0;
  const auto invalid = test_support::validateWithAuthoredBuiltins(
      std::move(invalidModel));
  expect(invalid.criticalFailure && !invalid.model &&
             std::ranges::find_if(invalid.diagnostics, [](const auto &entry) {
               return entry.code == "skin_lua_model_bpmgraph_invalid";
             }) != invalid.diagnostics.end(),
         "model validation rejects a BPM graph outside its normalized constructor domain");

  const auto shortColor = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  bpmgraph={{id='short',mainBPMColor='1-2-3'}},
  destination={{id='short',dst={{x=0,y=0,w=10,h=10}}}}
}
)lua");
  expect(!shortColor.model &&
             std::ranges::find_if(shortColor.diagnostics,
                                  [](const auto &entry) {
                                    return entry.code ==
                                           "skin_lua_model_bpmgraph_invalid";
                                  }) != shortColor.diagnostics.end(),
         "a nonempty normalized BPM colour shorter than six digits preserves Color.valueOf's construction failure");

  const auto sharedGauge = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  bpmgraph={{id='shared'}},
  gauge={id='shared',nodes={}},
  destination={{id='shared',dst={{x=0,y=0,w=10,h=10}}}}
}
)lua");
  const auto *shared =
      sharedGauge.model ? findObject(*sharedGauge.model, "shared") : nullptr;
  expect(shared != nullptr &&
             std::holds_alternative<SkinBpmGraphObject>(shared->payload),
         "BPM graph retains its pinned generic precedence over a same-ID gameplay Gauge");
}

void testGaugeGraphsAreTypedSupportedGameplayObjects() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  gaugegraph={
    {id='gauge-history'},
    {id='custom',color={
      'aBcDeF','1234567','01020304','#89AbCdEf',
      'fedcba987','#0A1b2C345678','000007','000008',
      '000009','00000a','00000b','00000c',
      '00000d','00000e','00000f','000010',
      '000011','000012','000013','000014',
      '000015','000016','000017','000018'}},
    {id='short',color={[1]='112233',[4]='445566'}},
    {id='legacy-lengths',assistClearBGColor='1234567',
     assistClearLineColor='01020304',borderlineColor='#AaBbCcDd',
     borderColor='fedcba987'}
  },
  destination={
    {id='gauge-history',dst={{x=0,y=0,w=100,h=50}}},
    {id='custom',dst={{x=100,y=0,w=100,h=50}}},
    {id='short',dst={{x=200,y=0,w=100,h=50}}},
    {id='legacy-lengths',dst={{x=300,y=0,w=100,h=50}}}
  }
}
)lua");
  const auto *definition =
      decoded.model ? findObject(*decoded.model, "gauge-history") : nullptr;
  const auto *customDefinition =
      decoded.model ? findObject(*decoded.model, "custom") : nullptr;
  const auto *shortDefinition =
      decoded.model ? findObject(*decoded.model, "short") : nullptr;
  const auto *legacyDefinition =
      decoded.model ? findObject(*decoded.model, "legacy-lengths") : nullptr;
  const auto *defaults =
      definition ? std::get_if<SkinGaugeGraphObject>(&definition->payload)
                 : nullptr;
  const auto *custom = customDefinition
                           ? std::get_if<SkinGaugeGraphObject>(
                                 &customDefinition->payload)
                           : nullptr;
  const auto *shortPalette =
      shortDefinition ? std::get_if<SkinGaugeGraphObject>(
                            &shortDefinition->payload)
                      : nullptr;
  const auto *legacyPalette =
      legacyDefinition ? std::get_if<SkinGaugeGraphObject>(
                             &legacyDefinition->payload)
                       : nullptr;
  expect(definition != nullptr &&
             !std::holds_alternative<SkinBlankObject>(definition->payload),
         "valid gaugegraph destinations decode as live typed objects");
  expect(defaults != nullptr &&
             defaults->rgba ==
                 std::array<std::array<std::uint32_t, 4>, 6>{
                     std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                                   0xff00ffffU, 0x440044ffU},
                     std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                                   0x00ffffffU, 0x004444ffU},
                     std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                                   0x00ff00ffU, 0x004400ffU},
                     std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                                   0xff0000ffU, 0x440000ffU},
                     std::array<std::uint32_t, 4>{0xffff00ffU, 0x444400ffU,
                                                   0xffff00ffU, 0x444400ffU},
                     std::array<std::uint32_t, 4>{0xccccccffU, 0x444444ffU,
                                                   0xccccccffU, 0x444444ffU}},
         "gaugegraph defaults reproduce the pinned six-category legacy palette");
  expect(custom != nullptr &&
             custom->rgba[0] ==
                 std::array<std::uint32_t, 4>{0xabcdefffU, 0x123456ffU,
                                               0x01020304U, 0x89abcdefU} &&
             custom->rgba[1][0] == 0xfedcbaffU &&
             custom->rgba[1][1] == 0x0a1b2cffU &&
             custom->rgba[2][3] == 0x00000cffU &&
             custom->rgba[5][3] == 0x000018ffU,
         "gaugegraph custom colours preserve pinned 6/7/8/9+ digit and hash parsing");
  const std::array<std::uint32_t, 4> blackRow{
      0x000000ffU, 0x000000ffU, 0x000000ffU, 0x000000ffU};
  expect(shortPalette != nullptr &&
             shortPalette->rgba[0] ==
                 std::array<std::uint32_t, 4>{0x112233ffU, 0x000000ffU,
                                               0x000000ffU, 0x445566ffU} &&
             shortPalette->rgba[1] == blackRow &&
             shortPalette->rgba[5] == blackRow,
         "a custom gaugegraph palette maps null and absent entries to black");
  expect(legacyPalette != nullptr &&
             legacyPalette->rgba[0] ==
                 std::array<std::uint32_t, 4>{0xaabbccddU, 0xfedcbaffU,
                                               0x01020304U, 0x123456ffU},
         "legacy gaugegraph fields use the same mixed-case direct colour parser as custom slots");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_gaugegraph_unsupported";
         }),
         "valid gaugegraphs emit no legacy unsupported diagnostic");
  if (!decoded.model) {
    return;
  }
  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             validated.model->disabledOptionalObjects.empty(),
         "valid gaugegraphs remain enabled through model validation");

  const auto shortColor = decodeInlineModel(R"lua(
return {type=0,w=1280,h=720,
 gaugegraph={{id='short-color',color={'12345'}}},
 destination={{id='short-color',dst={{x=0,y=0,w=10,h=10}}}}}
)lua");
  const auto nonhexLegacy = decodeInlineModel(R"lua(
return {type=0,w=1280,h=720,
 gaugegraph={{id='nonhex',borderColor='12zz56'}},
 destination={{id='nonhex',dst={{x=0,y=0,w=10,h=10}}}}}
)lua");
  const auto invalidGaugeColor = [](const auto &entry) {
    return entry.code == "skin_lua_model_gaugegraph_invalid";
  };
  expect(!shortColor.model &&
             std::ranges::any_of(shortColor.diagnostics,
                                 invalidGaugeColor) &&
             !nonhexLegacy.model &&
             std::ranges::any_of(nonhexLegacy.diagnostics,
                                 invalidGaugeColor),
         "direct gaugegraph colours reject fewer than six digits and nonhex parsed channels");
}

void testTimingVisualizerParsesPoorColourOnlyWhenOpaque() {
  const auto opaqueHashes = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingvisualizer={
    {id='opaque-rgb',PRColor='#000000'},
    {id='opaque-rgba',PRColor='#1234567F'}
  },
  destination={
    {id='opaque-rgb',dst={{x=0,y=0,w=100,h=50}}},
    {id='opaque-rgba',dst={{x=0,y=0,w=100,h=50}}}
  }
}
)lua");
  const auto *opaqueRgbDefinition =
      opaqueHashes.model ? findObject(*opaqueHashes.model, "opaque-rgb")
                         : nullptr;
  const auto *opaqueRgbaDefinition =
      opaqueHashes.model ? findObject(*opaqueHashes.model, "opaque-rgba")
                         : nullptr;
  const auto *opaqueRgb = opaqueRgbDefinition != nullptr
                              ? std::get_if<SkinTimingVisualizerObject>(
                                    &opaqueRgbDefinition->payload)
                              : nullptr;
  const auto *opaqueRgba = opaqueRgbaDefinition != nullptr
                               ? std::get_if<SkinTimingVisualizerObject>(
                                     &opaqueRgbaDefinition->payload)
                               : nullptr;
  expect(opaqueRgb != nullptr && opaqueRgba != nullptr &&
             opaqueRgb->judgeRgba[4] == 0x000000ffU &&
             opaqueRgba->judgeRgba[4] == 0x1234567fU,
         "opaque timingvisualizers parse libGDX leading-hash RGB and RGBA PRColors");

  const auto transparent = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingvisualizer={{id='transparent',transparent=1,PRColor='not-a-colour'}},
  destination={{id='transparent',dst={{x=0,y=0,w=100,h=50}}}}
}
)lua");
  const auto *transparentDefinition =
      transparent.model ? findObject(*transparent.model, "transparent")
                        : nullptr;
  const auto *transparentTiming = transparentDefinition != nullptr
                                      ? std::get_if<SkinTimingVisualizerObject>(
                                            &transparentDefinition->payload)
                                      : nullptr;
  expect(transparentTiming != nullptr && transparentTiming->transparent &&
             transparentTiming->judgeRgba[4] == 0U,
         "transparent timingvisualizers ignore an invalid PRColor and retain a clear poor band");

  const auto opaque = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingvisualizer={{id='opaque',transparent=0,PRColor='not-a-colour'}},
  destination={{id='opaque',dst={{x=0,y=0,w=100,h=50}}}}
}
)lua");
  expect(!opaque.model &&
             std::ranges::find_if(opaque.diagnostics, [](const auto &entry) {
               return entry.code == "skin_lua_model_timingvisualizer_invalid";
             }) != opaque.diagnostics.end(),
         "opaque timingvisualizers keep the pinned direct PRColor load-failure boundary");
}

void testTimingDistributionGraphRetainsPinnedConstructionFields() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingdistributiongraph={
    {id='default'},
    {id='custom',width=401,lineWidth=3,graphColor='11223344',
     averageColor='55667788',devColor='99AABBCC',PGColor='01020304',
     GRColor='11121314',GDColor='21222324',BDColor='31323334',
     PRColor='41424344',drawAverage=0,drawDev=2},
    {id='ordinary-lower',width=10,lineWidth=0},
    {id='ordinary-upper',width=10,lineWidth=99},
    {id='negative-lower',width=-5,lineWidth=0},
    {id='negative-upper',width=-5,lineWidth=1},
    {id='zero-constructs',width=0,lineWidth=0}
  },
  destination={
    {id='default',dst={{x=0,y=0,w=301,h=60}}},
    {id='custom',dst={{x=0,y=0,w=401,h=60}}},
    {id='ordinary-lower',dst={{x=0,y=0,w=10,h=60}}},
    {id='ordinary-upper',dst={{x=0,y=0,w=10,h=60}}},
    {id='negative-lower',dst={{x=0,y=0,w=1,h=60}}},
    {id='negative-upper',dst={{x=0,y=0,w=1,h=60}}},
    {id='zero-constructs',dst={{x=0,y=0,w=1,h=60}}}
  }
}
)lua");
  expect(decoded.model && decoded.model->objects.size() == 7,
         "timingdistributiongraph declarations create one typed gameplay object per destination");
  if (!decoded.model || decoded.model->objects.size() != 7) {
    return;
  }
  const auto payload = [&](std::string_view id) {
    const auto *definition = findObject(*decoded.model, id);
    return definition != nullptr
               ? std::get_if<SkinTimingDistributionGraphObject>(
                     &definition->payload)
               : nullptr;
  };
  const auto *defaults = payload("default");
  const auto *custom = payload("custom");
  const auto *ordinaryLower = payload("ordinary-lower");
  const auto *ordinaryUpper = payload("ordinary-upper");
  const auto *negativeLower = payload("negative-lower");
  const auto *negativeUpper = payload("negative-upper");
  const auto *zeroConstructs = payload("zero-constructs");
  if (defaults == nullptr || custom == nullptr || ordinaryLower == nullptr ||
      ordinaryUpper == nullptr || negativeLower == nullptr ||
      negativeUpper == nullptr || zeroConstructs == nullptr) {
    expect(false,
           "timingdistributiongraph destinations retain their typed payloads");
    return;
  }
  expect(defaults != nullptr && defaults->width == 301 &&
             defaults->lineWidth == 1 &&
             defaults->graphRgba == 0x00ff00ffU &&
             defaults->averageRgba == 0xffffffffU &&
             defaults->devRgba == 0xffffffffU &&
             defaults->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x000088ffU, 0x008800ffU,
                                               0x888800ffU, 0x880000ffU,
                                               0x000000ffU} &&
             defaults->drawAverage && defaults->drawDev,
         "timingdistributiongraph defaults retain the pinned constructor values");
  expect(custom != nullptr && custom->width == 401 &&
             custom->lineWidth == 3 && custom->graphRgba == 0x11223344U &&
             custom->averageRgba == 0x55667788U &&
             custom->devRgba == 0x99aabbccU &&
             custom->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x01020304U, 0x11121314U,
                                               0x21222324U, 0x31323334U,
                                               0x41424344U} &&
             !custom->drawAverage && !custom->drawDev,
         "timingdistributiongraph retains each authored colour and exact integer flags");
  expect(ordinaryLower->width == 10 && ordinaryLower->lineWidth == 1 &&
             ordinaryUpper->width == 10 && ordinaryUpper->lineWidth == 10,
         "timingdistributiongraph preserves the pinned ordinary lower and upper line-width clamps");
  expect(negativeLower->width == 1 && negativeLower->lineWidth == 1 &&
             negativeUpper->width == 1 && negativeUpper->lineWidth == -5 &&
             zeroConstructs->width == 1 && zeroConstructs->lineWidth == 1,
         "timingdistributiongraph preserves reversed-bound clamp branches that still construct");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code ==
                  "skin_lua_model_timingdistributiongraph_unsupported";
         }),
         "timingdistributiongraph is not reported as an unsupported gameplay widget");

  const auto validated = test_support::validateWithAuthoredBuiltins(
      *decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::all_of(validated.model->model.objects,
                                 [](const auto &object) {
                                   return !std::holds_alternative<
                                       SkinBlankObject>(object.payload);
                                 }),
         "valid timingdistributiongraphs remain typed through model validation");

  auto invalidModel = *decoded.model;
  std::get<SkinTimingDistributionGraphObject>(
      invalidModel.objects.front().payload)
      .lineWidth = 0;
  const auto invalid = test_support::validateWithAuthoredBuiltins(
      std::move(invalidModel));
  expect(invalid.criticalFailure && !invalid.model &&
             std::ranges::find_if(invalid.diagnostics, [](const auto &entry) {
               return entry.code ==
                      "skin_lua_model_timingdistributiongraph_invalid";
             }) != invalid.diagnostics.end(),
         "model validation rejects timingdistributiongraph geometry the pinned constructor cannot use");

  const auto divideByZero = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  timingdistributiongraph={{id='divide-by-zero',width=0,lineWidth=1}},
  destination={{id='divide-by-zero',dst={{x=0,y=0,w=1,h=60}}}}
}
)lua");
  expect(!divideByZero.model &&
             std::ranges::find_if(divideByZero.diagnostics,
                                  [](const auto &entry) {
                                    return entry.code ==
                                           "skin_lua_model_timingdistributiongraph_invalid";
                                  }) != divideByZero.diagnostics.end(),
         "timingdistributiongraph rejects only the zero-width clamp branch that divides by zero");
}

void testHitErrorVisualizerIsTypedInsteadOfBlank() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  hiterrorvisualizer={{id='hiterror'}},
  destination={{id='hiterror',dst={{x=0,y=0,w=301,h=60}}}}
}
)lua");
  const auto *definition =
      decoded.model ? findObject(*decoded.model, "hiterror") : nullptr;
  expect(definition != nullptr &&
             !std::holds_alternative<SkinBlankObject>(definition->payload) &&
             std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
               return entry.code ==
                      "skin_lua_model_hiterrorvisualizer_unsupported";
             }),
         "valid hiterrorvisualizers decode as typed objects without the legacy unsupported diagnostic");
  if (!decoded.model) {
    return;
  }
  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::none_of(validated.model->model.objects,
                                  [](const auto &object) {
                                    return std::holds_alternative<
                                        SkinBlankObject>(object.payload);
                                  }),
         "valid hiterrorvisualizers remain typed through model validation");
}

void testHitErrorVisualizerDecodesPinnedConstructorFields() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  hiterrorvisualizer={
    {id='default'},
    {id='custom',width=401,judgeWidthMillis=200,lineWidth=9,
     colorMode=2,hiterrorMode=1,emaMode=3,lineColor='11223344',
     centerColor='55667788',PGColor='01020304',GRColor='11121314',
     GDColor='21222324',BDColor='31323334',PRColor='not-a-colour',
     emaColor='51525354',alpha=0.25,windowLength=500,transparent=1,
     drawDecay=0},
    {id='invalid',lineWidth=-9,windowLength=-3,lineColor='invalid',
     centerColor='invalid',PGColor='invalid',GRColor='invalid',
     GDColor='invalid',BDColor='invalid',emaColor='invalid'}
  },
  destination={
    {id='default',dst={{x=0,y=0,w=301,h=60}}},
    {id='custom',dst={{x=0,y=0,w=401,h=200}}},
    {id='invalid',dst={{x=0,y=0,w=301,h=60}}}
  }
}
)lua");
  const auto payload = [&](std::string_view id) {
    const auto *definition = decoded.model ? findObject(*decoded.model, id)
                                           : nullptr;
    return definition != nullptr
               ? std::get_if<SkinHitErrorVisualizerObject>(
                     &definition->payload)
               : nullptr;
  };
  const auto *defaults = payload("default");
  const auto *custom = payload("custom");
  const auto *invalid = payload("invalid");
  expect(defaults != nullptr && defaults->width == 301 &&
             defaults->judgeWidthMillis == 150 && defaults->lineWidth == 1 &&
             defaults->colorMode == 1 && defaults->hitErrorMode == 1 &&
             defaults->emaMode == 1 &&
             defaults->lineRgba == 0x99ccff80U &&
             defaults->centerRgba == 0xffffffffU &&
             defaults->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x99ccff80U, 0xf2cb3080U,
                                               0x14cc8f80U, 0xff1ab380U,
                                               0xcc292980U} &&
             defaults->emaRgba == 0xff0000ffU && defaults->alpha == 0.1F &&
             defaults->windowLength == 30 && !defaults->transparent &&
             defaults->drawDecay,
         "hiterrorvisualizer defaults retain every pinned constructor value");
  expect(custom != nullptr && custom->width == 401 &&
             custom->judgeWidthMillis == 200 && custom->lineWidth == 4 &&
             custom->colorMode == 0 && custom->hitErrorMode == 1 &&
             custom->emaMode == 3 && custom->lineRgba == 0x11223344U &&
             custom->centerRgba == 0x55667788U &&
             custom->judgeRgba ==
                 std::array<std::uint32_t, 5>{0x01020304U, 0x11121314U,
                                               0x21222324U, 0x31323334U, 0U} &&
             custom->emaRgba == 0x51525354U && custom->alpha == 0.25F &&
             custom->windowLength == 100 && custom->transparent &&
             !custom->drawDecay,
         "hiterrorvisualizer clamps dimensions and applies exact integer flag and custom-colour semantics");
  expect(invalid != nullptr && invalid->lineWidth == 1 &&
             invalid->windowLength == 1 &&
             invalid->lineRgba == 0xff0000ffU &&
             invalid->centerRgba == 0xff0000ffU &&
             invalid->judgeRgba[0] == 0xff0000ffU &&
             invalid->judgeRgba[1] == 0xff0000ffU &&
             invalid->judgeRgba[2] == 0xff0000ffU &&
             invalid->judgeRgba[3] == 0xff0000ffU &&
             invalid->emaRgba == 0xff0000ffU,
         "validated hiterrorvisualizer colours fall back to opaque red and bounded lengths clamp");

  if (!decoded.model) {
    return;
  }
  auto invalidLineModel = *decoded.model;
  if (!invalidLineModel.objects.empty()) {
    std::get<SkinHitErrorVisualizerObject>(
        invalidLineModel.objects.front().payload)
        .lineWidth = 0;
  }
  const auto invalidLine = test_support::validateWithAuthoredBuiltins(
      std::move(invalidLineModel));
  expect(invalidLine.criticalFailure && !invalidLine.model &&
             std::ranges::find_if(invalidLine.diagnostics,
                                  [](const auto &entry) {
                                    return entry.code ==
                                           "skin_lua_model_hiterrorvisualizer_invalid";
                                  }) != invalidLine.diagnostics.end(),
         "model validation rejects hiterrorvisualizer line widths outside the constructor clamp");

  auto invalidWindowModel = *decoded.model;
  if (!invalidWindowModel.objects.empty()) {
    std::get<SkinHitErrorVisualizerObject>(
        invalidWindowModel.objects.front().payload)
        .windowLength = 101;
  }
  const auto invalidWindow = test_support::validateWithAuthoredBuiltins(
      std::move(invalidWindowModel));
  expect(invalidWindow.criticalFailure && !invalidWindow.model,
         "model validation rejects hiterrorvisualizer windows outside the constructor clamp");
}

void testHitErrorVisualizerParsesPoorColourOnlyWhenOpaque() {
  const auto hashes = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  hiterrorvisualizer={
    {id='rgb',PRColor='#000000'},
    {id='rgba',PRColor='#1234567F'},
    {id='transparent',transparent=1,PRColor='not-a-colour'}
  },
  destination={
    {id='rgb',dst={{x=0,y=0,w=301,h=60}}},
    {id='rgba',dst={{x=0,y=0,w=301,h=60}}},
    {id='transparent',dst={{x=0,y=0,w=301,h=60}}}
  }
}
)lua");
  const auto payload = [&](std::string_view id) {
    const auto *definition = hashes.model ? findObject(*hashes.model, id)
                                          : nullptr;
    return definition != nullptr
               ? std::get_if<SkinHitErrorVisualizerObject>(
                     &definition->payload)
               : nullptr;
  };
  const auto *rgb = payload("rgb");
  const auto *rgba = payload("rgba");
  const auto *transparent = payload("transparent");
  expect(rgb != nullptr && rgba != nullptr && transparent != nullptr &&
             rgb->judgeRgba[4] == 0x000000ffU &&
             rgba->judgeRgba[4] == 0x1234567fU &&
             transparent->judgeRgba[4] == 0U,
         "hiterrorvisualizer keeps direct libGDX PR parsing only for opaque poor marks");

  const auto invalid = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  hiterrorvisualizer={{id='invalid',transparent=0,PRColor='not-a-colour'}},
  destination={{id='invalid',dst={{x=0,y=0,w=301,h=60}}}}
}
)lua");
  expect(!invalid.model &&
             std::ranges::find_if(invalid.diagnostics, [](const auto &entry) {
               return entry.code ==
                      "skin_lua_model_hiterrorvisualizer_invalid";
             }) != invalid.diagnostics.end(),
         "opaque hiterrorvisualizer retains the pinned direct PRColor load-failure boundary");
}

void testNegativeGenericDistributionGraphStaysOutsideGameplay() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},
  graph={{id='select-only',src='atlas',w=11,h=1,divx=11,type=-1}},
  destination={{id='select-only',dst={{x=0,y=0,w=100,h=20}}}}
}
)lua");
  expect(decoded.model.has_value(),
         "a select-only generic distribution declaration is noncritical");
  if (!decoded.model) {
    return;
  }
  const auto *definition = findObject(*decoded.model, "select-only");
  expect(definition != nullptr &&
             std::holds_alternative<SkinGraphObject>(definition->payload) &&
             !std::holds_alternative<SkinNoteDistributionGraphObject>(
                 definition->payload) &&
             std::ranges::find_if(decoded.diagnostics, [](const auto &entry) {
               return entry.code ==
                      "skin_lua_model_distribution_graph_unsupported";
             }) != decoded.diagnostics.end(),
         "negative generic Graph retains the select-only invalid gameplay boundary");
}

void testOptionalVisualsAndBuiltinImagesStayLiveAcrossRepeatedDestinations() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='unused',path='alpha.chp'}},
  bpmgraph={{id='bpm'}},
  hiterrorvisualizer={{id='hiterror'}},
  judgegraph={{id='judge'}},
  timingvisualizer={{id='timing'}},
  pmchara={{id='pomyu',src='unused',color=1,type=0,side=1}},
  destination={
    {id='bpm',dst={{x=10,y=20,w=30,h=40}}},
    {id='bpm',dst={{x=50,y=60,w=70,h=80}}},
    {id='hiterror',dst={{x=1,y=2,w=3,h=4}}},
    {id='judge',dst={{x=5,y=6,w=7,h=8}}},
    {id='timing',dst={{x=9,y=10,w=11,h=12}}},
    {id='pomyu',dst={{x=9,y=10,w=11,h=12}}},
    {id=-100,dst={{x=0,y=0,w=1,h=1}}},
    {id=-101,dst={{x=0,y=0,w=1,h=1}}},
    {id=-110,dst={{x=0,y=0,w=1,h=1}}},
    {id=-111,dst={{x=0,y=0,w=1,h=1}}}
  }
}
)lua");

  expect(decoded.model && decoded.model->objects.size() == 10 &&
             decoded.model->destinations.size() == 10,
         "every authored destination, including repeated names and negative "
         "built-in image IDs, remains a live model object");
  expect(std::ranges::none_of(decoded.diagnostics, [](const auto &entry) {
           return entry.code == "skin_lua_model_bpmgraph_unsupported" ||
                  entry.code == "skin_lua_model_judgegraph_unsupported";
         }),
         "supported BPM and judge graph arrays emit no unsupported diagnostic");
  if (decoded.model && decoded.model->objects.size() == 10) {
    const auto repeatedNames = static_cast<std::size_t>(std::count_if(
        decoded.model->objects.begin(), decoded.model->objects.end(),
        [](const auto &object) { return object.authoredName == "bpm"; }));
    std::set<SkinObjectId> internalIds;
    for (const auto &object : decoded.model->objects) {
      internalIds.insert(object.id);
    }
    expect(repeatedNames == 2 && internalIds.size() == decoded.model->objects.size(),
           "repeated destination names produce distinct internal instances in "
           "pinned authored order");
    const auto validated = test_support::validateWithAuthoredBuiltins(
        *decoded.model);
    const auto *timing = findObject(*decoded.model, "timing");
    expect(validated.model && !validated.criticalFailure &&
               timing != nullptr &&
               std::holds_alternative<SkinTimingVisualizerObject>(
                   timing->payload),
           "timingvisualizers validate as typed render objects instead of "
           "blank optional destinations");
  }

  const auto oversized = decodeInlineModel(R"lua(
local definitions = {}
for index = 1, 8193 do
  definitions[index] = {id = "bpm-" .. index}
end
return {type=0,w=1280,h=720,bpmgraph=definitions,destination={}}
)lua");
  expect(!oversized.model &&
             std::ranges::find_if(oversized.diagnostics, [](const auto &entry) {
               return entry.code == "skin_lua_header_invalid";
             }) != oversized.diagnostics.end(),
         "unsupported identity arrays remain bounded by the shared decoded-object limit");
}

void testPomyuCharaCycleExtractionFollowsPinnedLoaderOrder() {
  constexpr std::string_view source =
      "#Anime\t100\n"
      "#Frame\t1\t40\n"
      "#Frame\t6\t50\n"
      "#Patern\t1\t000102\n"
      "#Texture\t6\t00010203\n"
      "#Layer\t7\t0001\n";
  const auto cycles = pomyuMotionCyclesFromChp(
      source, /*type=*/0, /*side=*/1,
      std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1});
  expect(cycles.has_value() &&
             *cycles == std::array<int, 8>{120, 200, 200, 1,
                                            1, 1,   1,   1},
         "PomyuCharaLoader derives 1P processor cycles from the authored "
         "Pattern, Texture, and Layer motion data in pinned order");

  const auto secondPlayerCycles = pomyuMotionCyclesFromChp(
      source, /*type=*/0, /*side=*/2,
      std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1});
  expect(secondPlayerCycles.has_value() &&
             *secondPlayerCycles == std::array<int, 8>{1, 1, 1,   1,
                                                        1, 120, 200, 1},
         "PomyuCharaLoader applies a PLAY pmchara's source cycles to its "
         "side-specific processor timer slots");
}

void testPomyuCharaCycleExtractionBoundsHostileFields() {
  std::string tabDense = "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102";
  tabDense.append(4U * 1024U * 1024U, '\t');
  tabDense.push_back('\n');
  const auto cycles = pomyuMotionCyclesFromChp(
      tabDense, /*type=*/0, /*side=*/1,
      std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1});
  expect(cycles.has_value() && (*cycles)[0] == 120,
         "tab-dense CHP lines inspect only the bounded fields needed for a "
         "Pomyu motion");

  const std::string oversized =
      "#Pattern\t1\t" + std::string(64U * 1024U + 1U, '0') + "\n";
  expect(!pomyuMotionCyclesFromChp(
              oversized, /*type=*/0, /*side=*/1,
              std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1})
              .has_value(),
         "oversized relevant CHP fields fail within the parser budget");

  std::stop_source cancelled;
  cancelled.request_stop();
  expect(!pomyuMotionCyclesFromChp(
              tabDense, /*type=*/0, /*side=*/1,
              std::array<int, 8>{1, 1, 1, 1, 1, 1, 1, 1},
              cancelled.get_token())
              .has_value(),
         "Pomyu cycle parsing observes activation cancellation");
}

void testPomyuCharaDefinitionPreservesPinnedSourceFields() {
  const auto decoded = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='chara',path='characters/alpha.chp'}},
  pmchara={{id='pomyu',src='chara',color=2,type=0,side=2}},
  destination={{id='pomyu',dst={{x=10,y=20,w=30,h=40}}}}
}
)lua");
  expect(decoded.model.has_value(), "pmchara source fixture decodes");
  if (!decoded.model) {
    return;
  }
  const auto *definition = findObject(*decoded.model, "pomyu");
  const auto *pmchara = definition != nullptr
                             ? std::get_if<SkinPmCharaObject>(&definition->payload)
                             : nullptr;
  expect(pmchara != nullptr && pmchara->source == SkinResourceId{1} &&
             pmchara->color == 2 && pmchara->type == 0 && pmchara->side == 2,
         "a pmchara destination retains the same source, colour, type, and "
         "side fields supplied to JsonPlaySkinObjectLoader");

  const auto unresolved = decodeInlineModel(R"lua(
return {
  type=0,w=1280,h=720,
  pmchara={{id='missing-source',type=0}},
  destination={{id='missing-source',dst={{x=10,y=20,w=30,h=40}}}}
}
)lua");
  expect(unresolved.model &&
             findObject(*unresolved.model, "missing-source") == nullptr,
         "a pmchara without a source follows JsonPlaySkinObjectLoader's null "
         "object path instead of rejecting the gameplay skin");
}

} // namespace

int main() {
  testGameplayTimingPreservesDefaultsAndEveryAuthoredField();
  testPropertyInterningIncludesIntegerAndFloatDomains();
  testStableIdsAndPerUseFrameExpansionAreSourceNeutral();
  testImageSetPreservesIndependentStateResourcesCropsAndTimers();
  testDestinationDefaultsInheritanceAndStableTimeOrder();
  testMaterializedSpriteFrameBudgetRejectsCopyAmplification();
  testFullTextureSentinelsRemainDeferredAcrossSpriteDivisions();
  testExplicitSpriteGridsRemainDeferredUntilTexturePreparation();
  testSparsePinnedBlendIdsMapExactlyAndRejectUnknownValues();
  testClipComponentsInheritUntilTheFirstCompleteRectangle();
  testLuaProtectedDecodeOwnsEveryRawParseTemporaryInTheRequest();
  testNoteHeightAndIgnoredAuthoredVisualsRemainExplicitPolicies();
  testValidatorRejectsCriticalNoteDependencyAndDisablesOptionalObject();
  testValidatedLaneCoverRateIndexClassifiesOnlyPinnedSelectors();
  testLiveCoverJudgeAndBgaSpecialObjects();
  testLiveGenericObjectPrecedesSameIdGameplaySpecials();
  testUnresolvedDestinationMatchesPinnedLoaderIgnoreBehavior();
  testDuplicateSourceDeclarationsUsePinnedLastDefinition();
  testDuplicateImageDeclarationsUsePinnedFirstDefinition();
  testLiveDestinationMouseRectAndCenterNormalization();
  testGameplayTypeMustBeSupported();
  testBooleanFieldsUseLuaTruthiness();
  testPracticePositionSliderDecodesItsExecutableWriter();
  testJudgeGraphsDecodePinnedModesAndPresentationFlags();
  testGaugeGraphsAreTypedSupportedGameplayObjects();
  testBpmGraphsDecodePinnedDefaultsNormalizationAndValidation();
  testTimingVisualizerDecodesPinnedPresentationFields();
  testTimingVisualizerParsesPoorColourOnlyWhenOpaque();
  testTimingDistributionGraphRetainsPinnedConstructionFields();
  testHitErrorVisualizerIsTypedInsteadOfBlank();
  testHitErrorVisualizerDecodesPinnedConstructorFields();
  testHitErrorVisualizerParsesPoorColourOnlyWhenOpaque();
  testNegativeGenericDistributionGraphStaysOutsideGameplay();
  testOptionalVisualsAndBuiltinImagesStayLiveAcrossRepeatedDestinations();
  testPomyuCharaCycleExtractionFollowsPinnedLoaderOrder();
  testPomyuCharaCycleExtractionBoundsHostileFields();
  testPomyuCharaDefinitionPreservesPinnedSourceFields();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "beatoraja skin model tests passed\n";
  return 0;
}
