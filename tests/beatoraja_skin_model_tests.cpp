#include "skin/beatoraja/LuaSkinTableDecoder.h"
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

BeatorajaSkinModelDecodeResult decodeInlineModel(std::string_view sourceText) {
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
                                {.runtime = *created.runtime});
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
    return decoder.decodeGameplay(*configured.value,
                                  {.runtime = *created.runtime});
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
             actual.h == expected.at("h").get<int>(),
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
  expect(model.integerProperties.size() == integerJson.size(),
         "integer bindings are interned once per selector domain");
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
  expect(model.floatProperties.size() == floatJson.size(),
         "float bindings are interned once per selector domain");
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
  expect(model.resources.size() == resourceJson.size(),
         "only authored Source records become image resources");
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
  expect(model.objects.size() == objectJson.size() &&
             model.destinations.size() == objectJson.size(),
         "effective object and destination vectors retain authored order");
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
  expect(strip.orderedStates.size() == 2,
         "Image len partitions one per-use row-major grid into states");
  for (std::size_t state = 0;
       state < strip.orderedStates.size() && state < stateJson.size();
       ++state) {
    expect(strip.orderedStates[state].resource == 1,
           "shared atlas resource ID remains division-neutral");
    expect(strip.orderedStates[state].frames.size() == stateJson[state].size(),
           "partitioned state preserves its frame count");
    for (std::size_t frame = 0;
         frame < strip.orderedStates[state].frames.size() &&
         frame < stateJson[state].size();
         ++frame) {
      expectRect(strip.orderedStates[state].frames[frame],
                 stateJson[state][frame],
                 "per-use strip division expands in row-major order");
    }
  }

  BeatorajaSkinModel invalidChild = *decoded.model;
  const auto outer = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
      [](const auto &object) { return object.authoredName == "judge"; });
  if (outer != invalidChild.objects.end()) {
    const auto &outerJudge = std::get<SkinJudgeObject>(outer->payload);
    const SkinObjectId outerId = outer->id;
    if (outerJudge.grades[0].image) {
      const auto child = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
          [&](const auto &object) { return object.id == outerJudge.grades[0].image->object; });
      if (child != invalidChild.objects.end()) {
        std::get<SkinImageObject>(child->payload).orderedStates.clear();
        const auto validated = test_support::validateWithAuthoredBuiltins(
            std::move(invalidChild));
        expect(validated.model && std::ranges::find(
                   validated.model->disabledOptionalObjects, outerId) !=
                   validated.model->disabledOptionalObjects.end(),
               "validator disables an outer Judge whose synthetic child is invalid");
      }
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
  expect(imageSet.orderedStates.size() == expected.size(),
         "ImageSet retains one complete state per authored image ID");
  for (std::size_t state = 0;
       state < imageSet.orderedStates.size() && state < expected.size();
       ++state) {
    const auto &actual = imageSet.orderedStates[state];
    const auto &entry = expected[state];
    expect(
        actual.resource == entry.at("resource") &&
            actual.cycleMillis == entry.at("cycleMillis") &&
            actual.timer.has_value() &&
            timerSelector(model, *actual.timer) ==
                entry.at("timerSelector").get<int>() &&
            actual.frames.size() == entry.at("frames").size(),
        "ImageSet state retains resource, cycle, timer, and crop cardinality");
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
  expect(frames.size() == expectedFrames.size(),
         "all inherited destination frames survive normalization");
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
             source.find("std::vector<double> expansionRate;") !=
                 std::string::npos,
         "raw note owns hidden, processed, and expansion parse vectors");
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
             !note.lanes.front().authoredNoteHeight.has_value(),
         "missing note size remains unresolved until resource preparation");
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
    const auto &expectedDisabled = expected.at("disabledOptionalObjectIds");
    expect(optionalResult.model->disabledOptionalObjects.size() ==
                   expectedDisabled.size() &&
               optionalResult.model->disabledOptionalObjects.front() ==
                   expectedDisabled.front().get<SkinObjectId>(),
           "validator reports the exact stable ID of the disabled optional "
           "object");
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
  expect(
      expected.at("missingNormalNoteIsCritical").get<bool>() &&
          criticalResult.criticalFailure && !criticalResult.model,
      "missing critical normal-note presentation rejects the complete model");

  BeatorajaSkinModel duplicateBindings = *decoded.model;
  duplicateBindings.integerProperties.push_back(
      duplicateBindings.integerProperties.front());
  const auto duplicateResult =
      test_support::validateWithAuthoredBuiltins(std::move(duplicateBindings));
  expect(duplicateResult.criticalFailure && !duplicateResult.model,
         "duplicate typed binding IDs reject the model");

  BeatorajaSkinModel oversizedCanvas = *decoded.model;
  oversizedCanvas.header.width =
      LuaSkinTableDecoderPolicy::maxAuthoredDimension + 1;
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
    const SkinObjectId scoreId = score->id;
    const auto domainResult = test_support::validateWithAuthoredBuiltins(
        std::move(wrongNumericDomain));
    expect(domainResult.model &&
               std::ranges::find(domainResult.model->disabledOptionalObjects,
                                 scoreId) !=
                   domainResult.model->disabledOptionalObjects.end(),
           "number objects reject image-index bindings with the wrong domain");
  }
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
          images={{id='judge-image',dst={{time=20,x=100,y=200,w=30,h=40}}},{id='missing-image',dst={{}}}},
          numbers={{id='judge-number',dst={{time=10,x=80,y=210,w=20,h=8}}},{id='missing-number',dst={{}}}}}},
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
  }
  BeatorajaSkinModel invalidChild = *decoded.model;
  const auto outer = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
      [](const auto &object) { return object.authoredName == "judge"; });
  if (outer != invalidChild.objects.end()) {
    const auto &outerJudge = std::get<SkinJudgeObject>(outer->payload);
    const auto outerId = outer->id;
    if (outerJudge.grades[0].image) {
      const auto child = std::find_if(invalidChild.objects.begin(), invalidChild.objects.end(),
          [&](const auto &object) { return object.id == outerJudge.grades[0].image->object; });
      if (child != invalidChild.objects.end()) {
        std::get<SkinImageObject>(child->payload).orderedStates.clear();
        const auto validated = test_support::validateWithAuthoredBuiltins(
            std::move(invalidChild));
        expect(validated.model && std::ranges::find(
                   validated.model->disabledOptionalObjects, outerId) !=
                   validated.model->disabledOptionalObjects.end(),
               "validator disables an outer Judge whose synthetic child is invalid");
      }
    }
  }
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
  testSparsePinnedBlendIdsMapExactlyAndRejectUnknownValues();
  testClipComponentsInheritUntilTheFirstCompleteRectangle();
  testLuaProtectedDecodeOwnsEveryRawParseTemporaryInTheRequest();
  testNoteHeightAndIgnoredAuthoredVisualsRemainExplicitPolicies();
  testValidatorRejectsCriticalNoteDependencyAndDisablesOptionalObject();
  testLiveCoverJudgeAndBgaSpecialObjects();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "beatoraja skin model tests passed\n";
  return 0;
}
