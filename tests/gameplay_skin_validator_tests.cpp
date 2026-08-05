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
  return value.size() == 64 &&
         std::ranges::all_of(value, [](char character) {
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
  expect(mapDigests.size() == 4,
         "each persisted option, file, and offset map mutation changes the digest");

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
               std::stop_token stop = {}) {
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

void testAuthoritativeCatalogAdmitsOnlyExecutableBridgeSelectors() {
  const SkinBuiltinBindingCatalogView catalog = gameplaySkinBuiltinCatalog();
  expect(catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                          SkinBuiltinPropertySelector{241}),
         "catalog admits an authoritative gameplay boolean");
  for (const int selector : {80, 81, 84, 271, 272, 273, 1080, 1243}) {
    expect(catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                            SkinBuiltinPropertySelector{selector}),
           "catalog admits every implemented gameplay mode/loading/judge boolean");
  }
  expect(catalog.contains(
             {.kind = SkinBindingKind::IntegerProperty,
              .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
             SkinBuiltinPropertySelector{107}),
         "catalog admits an authoritative gameplay integer");
  for (const int selector : {314, 315, 316}) {
    expect(catalog.contains(
               {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
               SkinBuiltinPropertySelector{selector}) &&
               !catalog.contains(
                   {.kind = SkinBindingKind::IntegerProperty,
                    .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
                   SkinBuiltinPropertySelector{selector}),
           "validator admits each lane-cover family amount only through the "
           "upstream Value domain");
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
  expect(!catalog.contains(
             {.kind = SkinBindingKind::IntegerProperty,
              .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
             SkinBuiltinPropertySelector{312}),
         "catalog rejects an upstream integer absent from the bridge");
  expect(!catalog.contains({.kind = SkinBindingKind::FloatWriter},
                           SkinBuiltinPropertySelector{4}),
         "catalog rejects the lane-cover writer until it is executable");
  expect(!catalog.contains({.kind = SkinBindingKind::FloatProperty,
                            .floatDomain = SkinFloatPropertyDomain::FloatValue},
                           SkinBuiltinPropertySelector{4}),
         "catalog does not widen the lane-cover Rate into FloatValue dispatch");
  expect(!catalog.contains({.kind = SkinBindingKind::Event},
                           SkinBuiltinPropertySelector{1000}),
         "catalog rejects custom event IDs from context-free built-in binding "
         "paths");
  expect(!catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                           SkinBuiltinPropertySelector{32}) &&
             !catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                               SkinBuiltinPropertySelector{33}),
         "catalog keeps autoplay booleans closed without exact authority");
}

void testValidNumericBindingPublishesOwnedTwoPhaseMetadata() {
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

  expect(result.disposition == SkinValidationDisposition::Selectable7Key,
         "valid 7-key Lua skin with a numeric built-in is selectable");
  expect(!result.cancelled &&
             !hasDiagnostic(result, "skin_lua_validation_failed"),
         "positive validation completes without cancellation or catch-all "
         "failure");
  expect(
      result.metadata && result.metadata->displayName == "configured-phase-2" &&
          result.metadata->author == "validator fixture" &&
          result.metadata->skinType == 0 &&
          result.metadata->authoredWidth == 1280 &&
          result.metadata->authoredHeight == 720,
      "configured phase reuses header Lua state and publishes copied metadata");
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
          result.metadata->options.front().choices.size() == 2,
      "validation output remains owned after all staging state is destroyed");
}

void testConfiguredPhaseCanReadStaticMainState() {
  const SkinValidationResult result = validateScript(R"lua(
if skin_config then
  assert(main_state.option(241) == false)
  assert(main_state.number(107) == 0)
  assert(main_state.float_number(4) == 0)
  assert(main_state.text(10) == "")
  local offset = main_state.offset(10)
  assert(offset.x == 0 and offset.y == 0 and offset.w == 0 and offset.h == 0)
  assert(main_state.timer(40) == main_state.timer_off_value)
  assert(main_state.exscore() == 0 and main_state.judge(1) == 0)
  assert(main_state.rate() == 0 and main_state.time() == 0)
  assert(main_state.volume_bg() == 0 and main_state.volume_key() == 0 and main_state.volume_sys() == 0)
  assert(main_state.gauge() == 0 and main_state.gauge_type() == 0)
end
return {
  type = 0, w = 1280, h = 720, name = "configured state access"
}
)lua");

  expect(result.disposition == SkinValidationDisposition::Selectable7Key,
         "validation supplies deterministic main_state values to configured Lua");
  expect(!hasDiagnostic(result, "skin_lua_execution_failed"),
         "configured main_state access does not abort validation");
}

void testUnsupportedNumericBindingFailsClosed() {
  const SkinValidationResult result = validateScript(R"lua(
return {
  type = 0, w = 1280, h = 720, name = "unsupported timer",
  customTimers = {{id = 10000, timer = -1}}
}
)lua");

  expect(result.disposition == SkinValidationDisposition::Invalid,
         "unsupported numeric built-in keeps the skin unselectable");
  expect(hasDiagnostic(result, "skin_lua_model_binding_source_invalid"),
         "unsupported numeric built-in reports typed catalog rejection");
  expect(!result.reconciledSettings && !result.metadata &&
             result.configurationDigest.empty(),
         "unsupported binding publishes no partial validation output");
}

void testMissingCriticalResourceFailsClosed() {
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

  expect(result.disposition == SkinValidationDisposition::Invalid,
         "missing critical note resource keeps the skin unselectable");
  expectDiagnostic(
      result, "skin.resource.missing_critical",
      "resource preparation contributes its critical failure diagnostic");
  expect(!result.reconciledSettings && !result.metadata &&
             result.configurationDigest.empty(),
         "resource failure publishes no partial validation output");
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
      configuredEntry != nullptr && *configuredEntry != '\0'
          ? configuredEntry
          : "play7.luaskin";

  TempDirectory temp;
  const auto package = normalizePackageId("ExternalGameplaySkin").package;
  const auto entry = package ? normalizeEntryPath(*package, entryPath).entry
                             : std::nullopt;
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
  if (result.disposition != SkinValidationDisposition::Selectable7Key) {
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << "external gameplay validation diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(!hasDiagnostic(result, "skin_lua_execution_failed"),
         "requested external gameplay Lua does not require live main_state during validation");
  expect(result.disposition == SkinValidationDisposition::Selectable7Key,
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
  testAuthoritativeCatalogAdmitsOnlyExecutableBridgeSelectors();
  testValidNumericBindingPublishesOwnedTwoPhaseMetadata();
  testConfiguredPhaseCanReadStaticMainState();
  testUnsupportedNumericBindingFailsClosed();
  testMissingCriticalResourceFailsClosed();
  testRequestedExternalGameplaySkinAvoidsConfiguredStateErrors();
  testCancellationFailsClosedBeforeRetainingTheRevisionView();
  return failures == 0 ? 0 : 1;
}
