#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/BeatorajaSkinConfiguration.h"
#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>

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

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-gameplay-skin-validator-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

void writeText(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

bool hasDiagnostic(const SkinValidationResult &result, std::string_view code) {
  return std::ranges::any_of(result.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.code == code;
  });
}

void expectDiagnostic(const SkinValidationResult &result, std::string_view code,
                      std::string_view message) {
  if (hasDiagnostic(result, code)) {
    return;
  }
  std::cerr << "FAIL: " << message << " (wanted " << code << "; received";
  for (const auto &diagnostic : result.diagnostics) {
    std::cerr << " [" << diagnostic.code << ": " << diagnostic.message << "; "
              << diagnostic.virtualPath << ']';
  }
  std::cerr << ")\n";
  ++failures;
}

bool isLowercaseSha256(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

void testConfigurationDigestFramesOnlyPersistedConfigurationMaps() {
  const EntryProfileSettings empty;
  expect(skinConfigurationDigest(empty) ==
             "f3c2c52f1de34a366df4f5bad4eb6a5bc080153949ea6422cb81aebfc84bc4b3",
         "empty settings retain the independently derived V1 framing digest");

  EntryProfileSettings configured;
  configured.options.emplace("Gauge", -12);
  configured.filePaths.emplace("Judge", "parts/judge.png");
  configured.offsets.emplace(
      "Lane", ConfigOffset{.x = -1, .y = 2, .w = -3, .h = 4, .r = -5, .a = 6});
  const std::string baseline = skinConfigurationDigest(configured);
  expect(isLowercaseSha256(baseline) &&
             baseline.find("SENTINEL") == std::string::npos,
         "configuration digests are opaque lowercase SHA-256 values");

  std::set<std::string> mapDigests{baseline};
  auto optionMutation = configured;
  optionMutation.options["Gauge"] = -11;
  mapDigests.insert(skinConfigurationDigest(optionMutation));
  auto fileMutation = configured;
  fileMutation.filePaths["Judge"] = "parts/alternate.png";
  mapDigests.insert(skinConfigurationDigest(fileMutation));
  auto offsetMutation = configured;
  offsetMutation.offsets["Lane"].a = 7;
  mapDigests.insert(skinConfigurationDigest(offsetMutation));
  expect(mapDigests.size() == 4, "each persisted option, file, and offset map "
                                 "mutation changes the digest");

  auto viewportMutation = configured;
  viewportMutation.viewport = {
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Stretch,
      .scaleX = 1.75F,
      .scaleY = 0.5F,
      .translateX = 321.0F,
      .translateY = -654.0F,
  };
  expect(skinConfigurationDigest(viewportMutation) == baseline,
         "viewport-only changes do not change the configuration digest");

  BeatorajaSkinConfiguration runtimeConfiguration;
  runtimeConfiguration.options = configured.options;
  runtimeConfiguration.filePaths = configured.filePaths;
  runtimeConfiguration.offsets = configured.offsets;
  runtimeConfiguration.orderedOptions.push_back(
      {.name = "SENTINEL-ORDER", .value = 999});
  runtimeConfiguration.enabledOptionIds.insert(999);
  runtimeConfiguration.orderedFiles.push_back(
      {.name = "SENTINEL-FILE",
       .pattern = "SENTINEL-HOST-PATH",
       .selectedValue = "SENTINEL-SELECTION"});
  runtimeConfiguration.offsetPermissions.emplace("SENTINEL-PERMISSION", 63);
  runtimeConfiguration.offsetsById.emplace(999, ConfigOffset{.x = 999});
  runtimeConfiguration.lowercaseSha256 = "SENTINEL-PRIOR-DIGEST";
  expect(skinConfigurationDigest(runtimeConfiguration) == baseline,
         "runtime-only ordering, declarations, permissions, IDs, and prior "
         "digest sentinels cannot enter V1 framing");
}

SkinValidationResult
validateScript(std::string_view script,
               const EntryProfileSettings *desiredSettings = nullptr,
               std::stop_token stop = {},
               const std::map<std::string, std::string> &resourceFiles = {}) {
  TempDirectory temp;
  const auto package = normalizePackageId("FixtureSkin");
  expect(package.package.has_value(), "fixture package ID is valid");
  if (!package.package) {
    return {};
  }
  const auto entry = normalizeEntryPath(*package.package, "play/play7.luaskin");
  expect(entry.entry.has_value(), "fixture entry ID is valid");
  if (!entry.entry) {
    return {};
  }

  const fs::path source = temp.root() / "source";
  writeText(source / "play/play7.luaskin", script);
  for (const auto &[path, contents] : resourceFiles) {
    writeText(source / path, contents);
  }
  NoAliases aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const auto snapshot = snapshotter.snapshot(source, *package.package, {}, {});
  expect(snapshot.prepared.has_value(),
         "fixture revision snapshot is prepared");
  if (!snapshot.prepared) {
    return {};
  }

  SkinResourcePreparationService resources;
  GameplaySkinValidator validator(resources);
  return validator.validate(snapshot.prepared->readView(), *entry.entry,
                            desiredSettings, stop);
}

void testAuthoritativeCatalogAdmitsCompatibilityIntegerFactoryDomain() {
  const SkinBuiltinBindingCatalogView catalog = gameplaySkinBuiltinCatalog();
  expect(catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                          SkinBuiltinPropertySelector{241}),
         "catalog admits an authoritative gameplay boolean");
  for (const int selector : {80, 81, 84, 271, 272, 273, 1080, 1243}) {
    expect(
        catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                         SkinBuiltinPropertySelector{selector}),
        "catalog admits every implemented gameplay mode/loading/judge boolean");
  }
  for (const int selector : {32,   -32,   42,   -42,   400,  -400,  603,  -603,
                             1002, -1002, 1177, -1177, 2246, -2246, 3000, -3000,
                             3015, -3015, 3020, -3020, 3035, -3035}) {
    expect(catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                            SkinBuiltinPropertySelector{selector}),
           "catalog admits each executable signed Beatoraja boolean selector");
  }
  expect(catalog.contains(
             {.kind = SkinBindingKind::IntegerProperty,
              .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
             SkinBuiltinPropertySelector{107}),
         "catalog admits an authoritative gameplay integer");
  for (const int selector : {12, 100, 121, 165}) {
    expect(catalog.contains(
               {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
               SkinBuiltinPropertySelector{selector}),
           "catalog admits each reported IntegerPropertyFactory ValueType "
           "selector");
  }
  for (const int selector : {0, 65'535}) {
    expect(catalog.contains(
               {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
               SkinBuiltinPropertySelector{selector}) &&
               catalog.contains(
                   {.kind = SkinBindingKind::IntegerProperty,
                    .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
                   SkinBuiltinPropertySelector{selector}),
           "integer factory cache domain is admitted at both inclusive bounds");
  }
  for (const int selector : {314, 315, 316}) {
    expect(catalog.contains(
               {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
               SkinBuiltinPropertySelector{selector}) &&
               catalog.contains(
                   {.kind = SkinBindingKind::IntegerProperty,
                    .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
                   SkinBuiltinPropertySelector{selector}),
           "compatibility catalog accepts each integer selector in both "
           "IntegerPropertyFactory input domains");
  }
  for (const int selector : {4, 5}) {
    expect(catalog.contains({.kind = SkinBindingKind::FloatProperty,
                             .floatDomain = SkinFloatPropertyDomain::Rate},
                            SkinBuiltinPropertySelector{selector}),
           "catalog admits each readable lane-cover rate");
  }
  expect(catalog.contains({.kind = SkinBindingKind::FloatProperty,
                           .floatDomain = SkinFloatPropertyDomain::Rate},
                          SkinBuiltinPropertySelector{"lanecover2"}),
         "catalog admits the upstream second lane-cover rate alias");
  expect(catalog.contains({.kind = SkinBindingKind::TimerProperty},
                          SkinBuiltinPropertySelector{41}),
         "catalog admits a timer backed by play state");
  expect(catalog.contains({.kind = SkinBindingKind::TimerProperty},
                          SkinBuiltinPropertySelector{40}),
         "catalog admits the bridge's exact inactive timer fallback");
  expect(catalog.contains({.kind = SkinBindingKind::Event},
                          SkinBuiltinPropertySelector{301}),
         "catalog admits an executable read-only event");

  expect(!catalog.contains({.kind = SkinBindingKind::TimerProperty},
                           SkinBuiltinPropertySelector{-1}),
         "catalog rejects the negative timer range upstream also rejects");
  for (const int selector :
       {312, 313, 1312, 1313, 1314, 1315, 1316, 1317, 1318, 1319, 1320, 1321,
        1322, 1323, 1324, 1325, 1326, 1327}) {
    expect(catalog.contains(
               {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
               SkinBuiltinPropertySelector{selector}),
           "catalog admits every pinned duration and lane-cover selector");
  }
  expect(!catalog.contains({.kind = SkinBindingKind::FloatWriter},
                           SkinBuiltinPropertySelector{4}),
         "catalog rejects the lane-cover writer until it is executable");
  for (const int selector : {17, 18, 19}) {
    expect(catalog.contains({.kind = SkinBindingKind::FloatWriter},
                            SkinBuiltinPropertySelector{selector}),
           "catalog admits each executable Config.AudioConfig writer");
  }
  expect(catalog.contains({.kind = SkinBindingKind::FloatWriter},
                          SkinBuiltinPropertySelector{20}) &&
             catalog.contains({.kind = SkinBindingKind::FloatWriter},
                              SkinBuiltinPropertySelector{
                                  "practice_position"}),
         "catalog admits the executable practice viewport writer by numeric "
         "and named selector");
  expect(catalog.contains({.kind = SkinBindingKind::FloatProperty,
                           .floatDomain = SkinFloatPropertyDomain::FloatValue},
                          SkinBuiltinPropertySelector{4}),
         "getFloatProperty retains the upstream RateType fallback, including "
         "the lane-cover rate");
  for (const int selector : {std::numeric_limits<int>::min(), 74, 1000,
                             std::numeric_limits<int>::max()}) {
    expect(catalog.contains({.kind = SkinBindingKind::Event},
                            SkinBuiltinPropertySelector{selector}),
           "EventFactory dispatches every signed integer event ID");
  }
  expect(!catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                           SkinBuiltinPropertySelector{34}) &&
             !catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                               SkinBuiltinPropertySelector{39}),
         "catalog keeps numeric SkinProperty constants absent from "
         "BooleanPropertyFactory closed");
}

void testCatalogPublishesOwnedHeaderMetadataWithoutLoadingGameplay() {
  constexpr std::string_view script = R"lua(
__validator_phase_count = (__validator_phase_count or 0) + 1
return {
  type = 0, w = 1280, h = 720,
  name = "configured-phase-" .. __validator_phase_count,
  author = "validator fixture",
  property = {{
    category = "Play", name = "Gauge",
    item = {{name = "Normal", op = 11}, {name = "Hard", op = 12}},
    def = "Normal"
  }},
  customTimers = {{id = 10000, timer = 41}}
}

)lua";

  EntryProfileSettings desired;
  desired.options["Gauge"] = 12;
  SkinValidationResult result = validateScript(script, &desired);

  expect(result.disposition == SkinValidationDisposition::SelectableGameplay,
         "valid 7-key Lua skin with a numeric built-in is selectable");
  expect(!result.cancelled &&
             !hasDiagnostic(result, "skin_lua_validation_failed"),
         "positive validation completes without cancellation or catch-all "
         "failure");
  expect(result.metadata &&
             result.metadata->displayName == "configured-phase-1" &&
             result.metadata->author == "validator fixture" &&
             result.metadata->skinType == 0 &&
             result.metadata->authoredWidth == 1280 &&
             result.metadata->authoredHeight == 720,
         "catalog publishes metadata from the header-only Lua execution");
  expect(result.reconciledSettings &&
             result.reconciledSettings->options.at("Gauge") == 12,
         "validated result publishes reconciled profile settings");
  expect(result.configurationDigest.size() == 64 &&
             std::ranges::all_of(result.configurationDigest,
                                 [](char value) {
                                   return (value >= '0' && value <= '9') ||
                                          (value >= 'a' && value <= 'f');
                                 }),
         "validated result publishes the canonical lowercase configuration "
         "digest");
  expect(result.reconciledSettings &&
             result.configurationDigest ==
                 skinConfigurationDigest(*result.reconciledSettings),
         "validator reports the digest of the exact reconciled settings it "
         "publishes");

  // validateScript has already destroyed the immutable revision, Lua runtime,
  // resource service and validator. Accessing these values proves the public
  // result owns its settings, strings and metadata rather than borrowing them.
  expect(
      result.metadata && result.metadata->options.size() == 1 &&
          result.metadata->options.front().name == "Gauge" &&
          result.metadata->options.front().choices.size() == 3 &&
          result.metadata->options.front().choices.back().label == "Random" &&
          result.metadata->options.front().choices.back().value == -1,
      "validation output remains owned after all staging state is destroyed");
}

void testCatalogPublishesFilePathChoicesAfterSavedSelection() {
  constexpr std::string_view script = R"lua(
return {
  type = 0, w = 1280, h = 720, name = "file-path catalog",
  filepath = {{
    category = "Play", name = "Settings File(7keys)",
    path = "customize/settings/*.lua", def = "FHD_default_1P.lua"
  }}
}
)lua";

  EntryProfileSettings desired;
  desired.filePaths["Settings File(7keys)"] = "alternate.lua";
  const SkinValidationResult result = validateScript(
      script, &desired, {},
      {{"play/customize/settings/FHD_default_1P.lua", "return {}"},
       {"play/customize/settings/alternate.lua", "return {}"},
       {"play/customize/settings/not-a-choice.txt", "ignored"}});

  expect(result.disposition == SkinValidationDisposition::SelectableGameplay &&
             result.metadata && result.metadata->files.size() == 1 &&
             result.metadata->files.front().name == "Settings File(7keys)" &&
             result.metadata->files.front().choices ==
                 std::vector<std::string>{"FHD_default_1P.lua", "alternate.lua",
                                          "Random"},
         "catalog retains every Beatoraja custom-file choice after a saved "
         "selection, including Random");
  expect(result.reconciledSettings &&
             result.reconciledSettings->filePaths == desired.filePaths,
         "catalog preserves the saved custom-file selection while presenting "
         "the other available choices");
}

void testCatalogAppendsBeatorajaRandomToCustomOptions() {
  constexpr std::string_view script = R"lua(
return {
  type = 0, w = 1280, h = 720, name = "one choice catalog",
  property = {{
    name = "---------PLAY OPTION---------",
    item = {{name = "-", op = 998}}
  }}
}
)lua";

  EntryProfileSettings desired;
  desired.options["---------PLAY OPTION---------"] = -1;
  const SkinValidationResult result = validateScript(script, &desired);
  const bool choicesMatch =
      result.metadata && result.metadata->options.size() == 1 &&
      result.metadata->options.front().choices.size() == 2 &&
      result.metadata->options.front().choices[0].label == "-" &&
      result.metadata->options.front().choices[0].value == 998 &&
      result.metadata->options.front().choices[1].label == "Random" &&
      result.metadata->options.front().choices[1].value == -1;

  expect(
      result.disposition == SkinValidationDisposition::SelectableGameplay &&
          choicesMatch && result.reconciledSettings &&
          result.reconciledSettings->options == desired.options,
      "catalog appends Beatoraja's Random entry to a singleton CustomOption "
      "and persists its random sentinel");
}

void testCatalogDoesNotExecuteConfiguredLuaOrFabricateMainState() {
  const SkinValidationResult result = validateScript(R"lua(
if skin_config then
  error("catalog validation must not execute configured Lua")
end
return {
  type = 0, w = 1280, h = 720, name = "header-only catalog"
}
)lua");

  expect(
      result.disposition == SkinValidationDisposition::SelectableGameplay,
      "catalog selection is based on a Beatoraja header pass, not fabricated "
      "main_state values");
  expect(!hasDiagnostic(result, "skin_lua_execution_failed"),
         "unexecuted configured Lua cannot abort catalog validation");
}

void testCatalogKeepsBeatorajaEmptyOptionDeclarationsSelectable() {
  const SkinValidationResult result = validateScript(R"lua(
return {
  type = 0, w = 1280, h = 720, name = "empty option declaration",
  property = {{name = "No choices", item = {}}}
}
)lua");

  expect(result.disposition == SkinValidationDisposition::SelectableGameplay &&
             result.reconciledSettings &&
             result.reconciledSettings->options ==
                 std::map<std::string, int>{{"No choices", -1}} &&
             result.configurationDigest ==
                 skinConfigurationDigest(*result.reconciledSettings),
         "an empty Beatoraja CustomOption keeps its random sentinel through "
         "catalog activation without a digest mismatch");
}

void testCatalogRejectsNonGameplayBeatorajaSkinTypes() {
  const SkinValidationResult result = validateScript(R"lua(
return {
  type = 5, w = 1280, h = 720, name = "music select is not gameplay"
}
)lua");

  expect(result.disposition == SkinValidationDisposition::UnavailableType &&
             result.metadata && result.metadata->skinType == 5 &&
             hasDiagnostic(result, "skin_lua_type_unavailable"),
         "non-gameplay Beatoraja skin types cannot appear in gameplay tabs");
}

void testCatalogDefersGameplayBindingFailureToGameplayLoading() {
  const SkinValidationResult result = validateScript(R"lua(
return {
  type = 0, w = 1280, h = 720, name = "unsupported timer",
  customTimers = {{id = 10000, timer = -1}}
}
)lua");

  expect(result.disposition == SkinValidationDisposition::SelectableGameplay,
         "catalog keeps a syntactically valid skin selectable before the live "
         "gameplay loader evaluates its bindings");
  expect(!hasDiagnostic(result, "skin_lua_model_binding_source_invalid"),
         "catalog does not decode the full gameplay model");
}

void testCatalogDefersGameplayResourceFailureToGameplayLoading() {
  const SkinValidationResult result = validateScript(R"lua(
return {
  type = 0, w = 1280, h = 720, name = "missing note atlas",
  source = {{id = "atlas", path = "missing.png"}},
  image = {{id = "note-frame", src = "atlas", x = 0, y = 0, w = 8, h = 8}},
  note = {
    id = "notes",
    note = {"note-frame"},
    mine = {"note-frame"},
    lnend = {"note-frame"}, lnstart = {"note-frame"},
    lnbody = {"note-frame"}, lnbodyActive = {"note-frame"},
    hcnend = {"note-frame"}, hcnstart = {"note-frame"},
    hcnbody = {"note-frame"}, hcnbodyActive = {"note-frame"},
    hcnbodyReactive = {"note-frame"}, hcnbodyMiss = {"note-frame"},
    dst = {{x = 0, y = 0, w = 64, h = 720}},
    group = {}, bpm = {}, stop = {}, time = {}
  },
  destination = {{id = "notes", dst = {{x = 0, y = 0, w = 64, h = 720}}}}
}
)lua");

  expect(result.disposition == SkinValidationDisposition::SelectableGameplay,
         "catalog does not reject a header because its full gameplay resources "
         "have not been loaded");
  expect(!hasDiagnostic(result, "skin.resource.missing_critical"),
         "catalog validation does not prepare gameplay resources");
}

void testRequestedExternalGameplaySkinAvoidsConfiguredStateErrors() {
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
      configuredEntry != nullptr && *configuredEntry != '\0' ? configuredEntry
                                                             : "play7.luaskin";

  TempDirectory temp;
  const auto package = normalizePackageId("ExternalGameplaySkin").package;
  const auto entry =
      package ? normalizeEntryPath(*package, entryPath).entry : std::nullopt;
  expect(package.has_value() && entry.has_value(),
         "requested external gameplay entry has a portable virtual identity");
  if (!package || !entry) {
    return;
  }
  NoAliases aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const auto snapshot = snapshotter.snapshot(source, *package, {}, {});
  expect(snapshot.prepared.has_value(),
         "requested external gameplay package snapshots");
  if (!snapshot.prepared) {
    return;
  }

  SkinResourcePreparationService resources;
  GameplaySkinValidator validator(resources);
  const SkinValidationResult result =
      validator.validate(snapshot.prepared->readView(), *entry, nullptr, {});
  if (result.disposition != SkinValidationDisposition::SelectableGameplay) {
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << "external gameplay validation diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(!hasDiagnostic(result, "skin_lua_execution_failed"),
         "requested external gameplay Lua does not require live main_state "
         "during validation");
  expect(result.disposition == SkinValidationDisposition::SelectableGameplay,
         "requested external gameplay skin validates as selectable");
}

void testCancellationFailsClosedBeforeRetainingTheRevisionView() {
  TempDirectory temp;
  const auto package = normalizePackageId("FixtureSkin");
  expect(package.package.has_value(), "fixture package ID is valid");
  if (!package.package) {
    return;
  }
  const auto entry = normalizeEntryPath(*package.package, "play/play7.luaskin");
  expect(entry.entry.has_value(), "fixture entry ID is valid");
  if (!entry.entry) {
    return;
  }
  const fs::path source = temp.root() / "source";
  fs::create_directories(source / "play");
  {
    std::ofstream output(source / "play/play7.luaskin", std::ios::binary);
    output << "return { type = 0 }";
  }
  NoAliases aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const auto snapshot = snapshotter.snapshot(source, *package.package, {}, {});
  expect(snapshot.prepared.has_value(),
         "fixture revision snapshot is prepared");
  if (!snapshot.prepared) {
    return;
  }

  SkinResourcePreparationService resources;
  GameplaySkinValidator validator(resources);
  std::stop_source stop;
  stop.request_stop();
  const SkinValidationResult result = validator.validate(
      snapshot.prepared->readView(), *entry.entry, nullptr, stop.get_token());

  expect(result.cancelled, "pre-cancelled validation reports cancellation");
  expect(result.disposition == SkinValidationDisposition::Invalid,
         "pre-cancelled validation fails closed");
  expect(!result.reconciledSettings && !result.metadata &&
             result.configurationDigest.empty(),
         "pre-cancelled validation publishes no retained validation output");
}

} // namespace

int main() {
  testConfigurationDigestFramesOnlyPersistedConfigurationMaps();
  testAuthoritativeCatalogAdmitsCompatibilityIntegerFactoryDomain();
  testCatalogPublishesOwnedHeaderMetadataWithoutLoadingGameplay();
  testCatalogPublishesFilePathChoicesAfterSavedSelection();
  testCatalogAppendsBeatorajaRandomToCustomOptions();
  testCatalogDoesNotExecuteConfiguredLuaOrFabricateMainState();
  testCatalogKeepsBeatorajaEmptyOptionDeclarationsSelectable();
  testCatalogRejectsNonGameplayBeatorajaSkinTypes();
  testCatalogDefersGameplayBindingFailureToGameplayLoading();
  testCatalogDefersGameplayResourceFailureToGameplayLoading();
  testRequestedExternalGameplaySkinAvoidsConfiguredStateErrors();
  testCancellationFailsClosedBeforeRetainingTheRevisionView();
  return failures == 0 ? 0 : 1;
}
