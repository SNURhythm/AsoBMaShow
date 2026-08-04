#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/SkinModelValidator.h"

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
    return decoder.decodeGameplay(*configured.value);
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
               resource->virtualPath == entry.at("virtualPath") &&
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
               object.authoredName == entry.at("name") &&
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
  SkinModelValidator validator;

  const auto optionalResult = validator.validate(*decoded.model);
  expect(!optionalResult.criticalFailure && optionalResult.model.has_value(),
         "missing optional image source does not reject the gameplay model");
  if (optionalResult.model) {
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
  const auto criticalResult =
      validator.validate(std::move(missingCriticalNote));
  expect(
      expected.at("missingNormalNoteIsCritical").get<bool>() &&
          criticalResult.criticalFailure && !criticalResult.model,
      "missing critical normal-note presentation rejects the complete model");
}

} // namespace

int main() {
  testGameplayTimingPreservesDefaultsAndEveryAuthoredField();
  testPropertyInterningIncludesIntegerAndFloatDomains();
  testStableIdsAndPerUseFrameExpansionAreSourceNeutral();
  testImageSetPreservesIndependentStateResourcesCropsAndTimers();
  testDestinationDefaultsInheritanceAndStableTimeOrder();
  testNoteHeightAndIgnoredAuthoredVisualsRemainExplicitPolicies();
  testValidatorRejectsCriticalNoteDependencyAndDisablesOptionalObject();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "beatoraja skin model tests passed\n";
  return 0;
}
