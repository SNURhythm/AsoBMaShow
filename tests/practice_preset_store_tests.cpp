#include "AtomicFile.h"
#include "practice/PracticePresetStore.h"
#include "scene/ChartViewerScene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../yoga/lib/nlohmann/json.hpp"

namespace {
constexpr std::string_view kHash = "0123456789ABCDEF0123456789ABCDEF"
                                   "0123456789ABCDEF0123456789ABCDEF";
constexpr std::string_view kNormalizedHash = "0123456789abcdef0123456789abcdef"
                                             "0123456789abcdef0123456789abcdef";
constexpr std::string_view kOtherHash = "ffffffffffffffffffffffffffffffff"
                                        "ffffffffffffffffffffffffffffffff";

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-practice-store-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

practice::Configuration configuration(long long start, long long end) {
  return {.chartSha256 = std::string(kHash),
          .startMicros = start,
          .endMicros = end,
          .loop = true,
          .countInBeats = 8,
          .gaugeType = GaugeType::ExHard,
          .gaugeAutoShift = true,
          .startingGaugePercent = 60,
          .judge = {.scalePercent = 85},
          .playback = {.percent = 90}};
}

std::vector<std::string>
directoryFilenames(const std::filesystem::path &directory) {
  std::vector<std::string> result;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    result.push_back(entry.path().filename().string());
  }
  std::ranges::sort(result);
  return result;
}

void testRoundTripAndMutations() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  const auto missing = store.load(kHash, 12'000'000);
  expect(missing.status == versioned_json::LoadStatus::Missing,
         "missing chart data reports the missing status");
  expect(missing.data.lastUsed.chartSha256 == kNormalizedHash &&
             missing.data.lastUsed.startMicros == 0 &&
             missing.data.lastUsed.endMicros == 12'000'000,
         "missing chart data returns neutral chart-scoped defaults");
  expect(!missing.diagnostics.empty(),
         "missing chart data explains that defaults were selected");

  std::string error;
  const auto lastUsed = configuration(1'000'000, 9'000'000);
  expect(store.saveLastUsed(kHash, lastUsed, error),
         "last-used configuration saves: " + error);
  const auto firstId = store.saveNamed(
      kHash, "Warmup", configuration(1'000'000, 3'000'000), error);
  const auto secondId = store.saveNamed(
      kHash, "Finish", configuration(7'000'000, 9'000'000), error);
  expect(firstId.has_value() && secondId.has_value() && firstId != secondId,
         "two named presets receive distinct identities: " + error);

  const auto loaded = store.load(kHash, 12'000'000);
  expect(loaded.status == versioned_json::LoadStatus::Loaded,
         "saved chart data reloads");
  expect(loaded.data.lastUsed.startMicros == 1'000'000 &&
             loaded.data.lastUsed.chartSha256 == kNormalizedHash &&
             loaded.data.lastUsed.countInBeats == 8 &&
             loaded.data.lastUsed.gaugeType == GaugeType::ExHard &&
             loaded.data.lastUsed.gaugeAutoShift,
         "last-used configuration, GAS, and explicit count-in round-trip");
  expect(loaded.data.named.size() == 2 &&
             loaded.data.named[0].configuration.chartSha256 == kNormalizedHash,
         "named presets round-trip as chart-scoped configurations");
  expect(
      directoryFilenames(temp.path()) ==
          std::vector<std::string>{std::string(kNormalizedHash) + ".json",
                                   std::string(kNormalizedHash) + ".json.bak"},
      "successful repeated saves retain only the primary preset file and "
      "its exact atomic backup sidecar");

  if (firstId) {
    expect(store.renameNamed(kHash, *firstId, "Opening", error),
           "named preset renames: " + error);
    expect(store.updateNamed(kHash, *firstId,
                             configuration(2'000'000, 4'000'000), error),
           "named preset updates: " + error);
  }
  if (secondId) {
    expect(store.deleteNamed(kHash, *secondId, error),
           "named preset deletes: " + error);
  }
  const auto mutated = store.load(kHash, 12'000'000);
  expect(mutated.data.named.size() == 1 &&
             mutated.data.named.front().name == "Opening" &&
             mutated.data.named.front().configuration.startMicros == 2'000'000,
         "rename, update, and delete mutations persist");
  expect(std::filesystem::exists(temp.path() /
                                 (std::string(kNormalizedHash) + ".json")),
         "only the normalized hash is used as the preset filename");
}

void testLegacyPresetDefaultsGaugeAutoShiftOff() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  std::string error;
  expect(store.saveLastUsed(kHash, configuration(1'000'000, 5'000'000), error),
         "legacy GAS fixture saves: " + error);

  const auto path = temp.path() / (std::string(kNormalizedHash) + ".json");
  nlohmann::json document;
  {
    std::ifstream input(path);
    input >> document;
  }
  document.at("lastUsed").erase("gaugeAutoShift");
  for (auto &preset : document.at("named")) {
    preset.at("configuration").erase("gaugeAutoShift");
  }
  {
    std::ofstream output(path, std::ios::trunc);
    output << document.dump();
  }

  const auto loaded = store.load(kHash, 10'000'000);
  expect(loaded.status == versioned_json::LoadStatus::Loaded &&
             !loaded.data.lastUsed.gaugeAutoShift,
         "version-one preset JSON without GAS defaults auto shift off");
}

void testHashMismatchIsRejected() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  auto mismatched = configuration(0, 1'000'000);
  mismatched.chartSha256 = std::string(kOtherHash);
  std::string error;
  expect(!store.saveLastUsed(kHash, mismatched, error) && !error.empty(),
         "saving a configuration for a different chart is rejected");
  expect(!std::filesystem::exists(temp.path() /
                                  (std::string(kNormalizedHash) + ".json")),
         "a mismatched chart does not create a file");
}

void testPortableFilenameClassificationIsExact() {
  using practice::PresetFileKind;
  const std::string primary = std::string(kNormalizedHash) + ".json";
  expect(practice::classifyPresetFilename(primary) == PresetFileKind::Primary,
         "normalized chart hash JSON is a portable primary preset file");
  for (const std::string_view suffix :
       {".tmp", ".bak", ".bak.pending", ".bak.previous"}) {
    expect(practice::classifyPresetFilename(primary + std::string(suffix)) ==
               PresetFileKind::AtomicSidecar,
           "the exact atomic writer sidecar is classified as transient");
  }
  for (const std::string &unknown :
       {primary + ".previous", primary + ".bak.tmp", primary + ".old",
        std::string(kHash) + ".json"}) {
    expect(practice::classifyPresetFilename(unknown) == PresetFileKind::Invalid,
           "unknown or non-normalized practice filename is rejected");
  }
}

void testPortableFileValidationRejectsInvalidData() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  std::string error;
  expect(store.saveLastUsed(kHash, configuration(1'000'000, 5'000'000), error),
         "portable validation fixture saves: " + error);
  const auto path = temp.path() / (std::string(kNormalizedHash) + ".json");

  auto validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::Loaded,
         "current semantic preset data validates for portability");

  nlohmann::json document;
  {
    std::ifstream input(path);
    input >> document;
  }
  document["schemaVersion"] = 2;
  {
    std::ofstream output(path, std::ios::trunc);
    output << document.dump();
  }
  validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::FutureVersion,
         "future portable preset schema is rejected distinctly");

  document["schemaVersion"] = 1;
  document["chartSha256"] = std::string(kOtherHash);
  {
    std::ofstream output(path, std::ios::trunc);
    output << document.dump();
  }
  validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::InvalidRoot,
         "portable preset root hash must match its filename");

  document["chartSha256"] = std::string(kNormalizedHash);
  document["lastUsed"]["chartSha256"] = std::string(kOtherHash);
  {
    std::ofstream output(path, std::ios::trunc);
    output << document.dump();
  }
  validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::InvalidRoot,
         "portable preset configuration hash must match its filename");

  document["lastUsed"]["chartSha256"] = std::string(kNormalizedHash);
  document["lastUsed"]["countInBeats"] = 17;
  {
    std::ofstream output(path, std::ios::trunc);
    output << document.dump();
  }
  validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::InvalidRoot,
         "portable preset configurations requiring sanitization are rejected");

  {
    std::ofstream output(path, std::ios::trunc);
    output << "{not-json";
  }
  validation = practice::validatePresetFile(path, 1);
  expect(validation.status == versioned_json::LoadStatus::Malformed,
         "malformed portable preset JSON is rejected distinctly");
}

void testPortableFileValidationRejectsUndeclaredFieldsAndInvalidIds() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  std::string error;
  expect(store.saveLastUsed(kHash, configuration(1'000'000, 5'000'000), error),
         "strict portable validation fixture saves: " + error);
  expect(store
             .saveNamed(kHash, "Opening", configuration(1'000'000, 3'000'000),
                        error)
             .has_value(),
         "strict portable validation named fixture saves: " + error);
  const auto path = temp.path() / (std::string(kNormalizedHash) + ".json");

  nlohmann::json document;
  {
    std::ifstream input(path);
    input >> document;
  }
  auto statusFor = [&](const nlohmann::json &candidate) {
    {
      std::ofstream output(path, std::ios::trunc);
      output << candidate.dump();
    }
    return practice::validatePresetFile(path, 1).status;
  };

  nlohmann::json unexpectedRoot = document;
  unexpectedRoot["future"] = true;
  expect(statusFor(unexpectedRoot) == versioned_json::LoadStatus::InvalidRoot,
         "portable root rejects undeclared schema-v1 fields");

  nlohmann::json unexpectedLastUsed = document;
  unexpectedLastUsed["lastUsed"]["future"] = true;
  expect(statusFor(unexpectedLastUsed) ==
             versioned_json::LoadStatus::InvalidRoot,
         "portable last-used configuration rejects undeclared fields");

  nlohmann::json unexpectedJudge = document;
  unexpectedJudge["lastUsed"]["judge"]["future"] = true;
  expect(statusFor(unexpectedJudge) == versioned_json::LoadStatus::InvalidRoot,
         "portable judge object rejects undeclared fields");

  nlohmann::json unexpectedPlayback = document;
  unexpectedPlayback["lastUsed"]["playback"]["future"] = true;
  expect(statusFor(unexpectedPlayback) ==
             versioned_json::LoadStatus::InvalidRoot,
         "portable playback object rejects undeclared fields");

  nlohmann::json unexpectedNamed = document;
  unexpectedNamed["named"][0]["future"] = true;
  expect(statusFor(unexpectedNamed) == versioned_json::LoadStatus::InvalidRoot,
         "portable named entry rejects undeclared fields");

  nlohmann::json unexpectedNamedConfiguration = document;
  unexpectedNamedConfiguration["named"][0]["configuration"]["future"] = true;
  expect(statusFor(unexpectedNamedConfiguration) ==
             versioned_json::LoadStatus::InvalidRoot,
         "portable named configuration rejects undeclared fields");

  nlohmann::json invalidId = document;
  invalidId["named"][0]["id"] = "not-a-valid-preset-id";
  expect(statusFor(invalidId) == versioned_json::LoadStatus::InvalidRoot,
         "portable named entry rejects invalid preset IDs");

  nlohmann::json duplicateId = document;
  duplicateId["named"].push_back(duplicateId["named"][0]);
  expect(statusFor(duplicateId) == versioned_json::LoadStatus::InvalidRoot,
         "portable named entries require unique preset IDs");
}

void testMalformedFileReturnsNeutralDefaults() {
  TempDirectory temp;
  {
    std::ofstream output(temp.path() /
                         (std::string(kNormalizedHash) + ".json"));
    output << "{not-json";
  }
  practice::PresetStore store(temp.path());
  const auto loaded = store.load(kHash, 7'000'000);
  expect(loaded.status == versioned_json::LoadStatus::Malformed &&
             loaded.data.lastUsed.chartSha256 == kNormalizedHash &&
             loaded.data.lastUsed.startMicros == 0 &&
             loaded.data.lastUsed.endMicros == 7'000'000 &&
             !loaded.diagnostics.empty(),
         "malformed chart data returns neutral defaults with diagnostics");
}

void testFailedLoadsReplacePriorChartStateWithNeutralCurrentChartState() {
  for (const auto status : {versioned_json::LoadStatus::Malformed,
                            versioned_json::LoadStatus::FutureVersion}) {
    TempDirectory temp;
    practice::PresetStore store(temp.path());
    std::string error;
    expect(store
               .saveNamed(kHash, "Opening",
                          configuration(1'000'000, 3'000'000), error)
               .has_value(),
           "failed-load fixture saves a named preset: " + error);

    const auto path = temp.path() / (std::string(kNormalizedHash) + ".json");
    if (status == versioned_json::LoadStatus::Malformed) {
      std::ofstream output(path, std::ios::trunc);
      output << "{not-json";
    } else {
      nlohmann::json document;
      {
        std::ifstream input(path);
        input >> document;
      }
      document["schemaVersion"] = practice::kPresetSchemaVersion + 1;
      std::ofstream output(path, std::ios::trunc);
      output << document.dump();
    }

    std::ifstream beforeInput(path, std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(beforeInput)),
                             std::istreambuf_iterator<char>());
    auto loaded = store.load(kHash, 7'000'000);
    expect(loaded.status == status,
           "failed-load fixture produces the intended load status");
    const auto diagnostic = loaded.notice();
    auto priorConfiguration = configuration(2'000'000, 6'000'000);
    priorConfiguration.chartSha256 = std::string(kOtherHash);
    std::vector<practice::NamedPreset> priorNamed = {
        {.id = "12345678-1234-1234-1234-123456789abc",
         .name = "Prior chart",
         .configuration = priorConfiguration}};
    std::optional<std::string> selected = priorNamed.front().id;

    const bool usable = practice::installPresetLoadState(
        std::move(loaded), false, priorConfiguration, priorNamed, selected);

    expect(!usable && priorConfiguration.chartSha256 == kNormalizedHash &&
               priorConfiguration.startMicros == 0 &&
               priorConfiguration.endMicros == 7'000'000 &&
               priorNamed.empty() && !selected.has_value(),
           "failed preset loads replace prior-chart configuration, named "
           "data, and selection with neutral current-chart state");
    expect(diagnostic.has_value(),
           "failed preset state replacement preserves load diagnostics");
    std::ifstream afterInput(path, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(afterInput)),
                            std::istreambuf_iterator<char>());
    expect(after == before,
           "failed preset state replacement never rewrites malformed or "
           "future named files");
  }
}

void testReplacementChartReloadUsesNewEndBeforeStateIsConsumed() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  std::string error;
  expect(store.saveLastUsed(kHash, configuration(1'000'000, 9'000'000), error),
         "replacement-chart fixture saves last used: " + error);
  expect(store
             .saveNamed(kHash, "Middle",
                        configuration(2'000'000, 8'000'000), error)
             .has_value(),
         "replacement-chart fixture saves a named preset: " + error);

  auto loaded = store.load(kHash, 4'000'000);
  const auto diagnostic = loaded.notice();
  auto current = configuration(1'000'000, 9'000'000);
  std::vector<practice::NamedPreset> named;
  std::optional<std::string> selected = "prior-selection";
  chart_viewer_practice::GhostRefreshState state{
      .chartEndMicros = 9'000'000,
      .configuration = current,
      .namedPresets = named,
      .selectedPresetId = selected,
      .pendingLaunchRequest = practice::LaunchRequest{},
      .ghostReplay = ReplayData{.provenance = ScoreProvenance{}},
      .loadedGhostReplayId = 42,
      .playOption = "RANDOM",
      .playOptionSeed = 1234,
      .playOption2 = "MIRROR",
      .playOption2Seed = 5678};
  std::optional<chart_viewer_practice::GhostRefreshState> committed;
  bool panelRefreshObserved = false;
  const bool usable = chart_viewer_practice::installGhostRefreshState(
      std::move(state), 4'000'000, std::move(loaded), "Ghost loaded",
      [&](chart_viewer_practice::GhostRefreshState installed) {
        panelRefreshObserved = true;
        expect(installed.chartEndMicros == 4'000'000 &&
                   installed.configuration.endMicros == 4'000'000 &&
                   installed.namedPresets.size() == 1 &&
                   installed.namedPresets.front().configuration.endMicros ==
                       4'000'000,
               "replacement-chart state is installed before panel refresh");
        committed = std::move(installed);
      });

  expect(usable && panelRefreshObserved && committed.has_value() &&
             !committed->selectedPresetId.has_value(),
         "replacement-chart reload installs ranges sanitized against the new "
         "chart end before panel, save, or launch state is consumed");
  expect(diagnostic.has_value() &&
             committed->visibleStatus == "Practice presets: " + *diagnostic,
         "replacement-chart sanitization diagnostic remains visible instead "
         "of the ghost success text");
  expect(committed->pendingLaunchRequest.has_value() &&
             committed->ghostReplay.has_value() &&
             committed->loadedGhostReplayId == 42 &&
             committed->playOption == "RANDOM" &&
             committed->playOptionSeed == 1234 &&
             committed->playOption2 == "MIRROR" &&
             committed->playOption2Seed == 5678,
         "ghost refresh preserves pending launch, ghost, and play options");
}

void testFirstSaveAsSeedsLastUsedFromSuppliedConfiguration() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  const auto supplied = configuration(1'000'000, 3'000'000);
  std::string error;
  const auto id = store.saveNamed(kHash, "Opening", supplied, error);
  expect(id.has_value(), "first Save As creates its named preset: " + error);

  const auto loaded = store.load(kHash, 12'000'000);
  auto expected = supplied;
  expected.chartSha256 = std::string(kNormalizedHash);
  expect(loaded.status == versioned_json::LoadStatus::Loaded &&
             loaded.data.lastUsed == expected,
         "first Save As seeds last used from the supplied configuration");
}

void testFailedAtomicReplacePreservesPreviousFile() {
  TempDirectory temp;
  practice::PresetStore store(temp.path());
  std::string error;
  expect(store.saveLastUsed(kHash, configuration(1'000'000, 5'000'000), error),
         "rollback fixture saves its initial file: " + error);

  atomic_file::Operations operations = atomic_file::defaultOperations();
  const auto realReplace = operations.replace;
  operations.replace = [realReplace](const std::filesystem::path &from,
                                     const std::filesystem::path &to,
                                     std::string &replaceError) {
    if (from.extension() == ".tmp") {
      replaceError = "injected final replace failure";
      return false;
    }
    return realReplace(from, to, replaceError);
  };
  practice::PresetStore failingStore(temp.path(), &operations);
  expect(!failingStore.saveLastUsed(kHash, configuration(2'000'000, 6'000'000),
                                    error),
         "injected final replace failure is reported");

  const auto reloaded = store.load(kHash, 10'000'000);
  expect(reloaded.status == versioned_json::LoadStatus::Loaded &&
             reloaded.data.lastUsed.startMicros == 1'000'000 &&
             reloaded.data.lastUsed.endMicros == 5'000'000,
         "the previously committed file remains readable after replacement "
         "failure");
}

void testLoadResultUsabilityAndNotices() {
  practice::PresetLoadResult missing{.status =
                                         versioned_json::LoadStatus::Missing};
  expect(missing.usable() && !missing.notice(),
         "missing preset data is a usable neutral default without an error");

  practice::PresetLoadResult loaded{
      .status = versioned_json::LoadStatus::Loaded,
      .diagnostics = {"practice markers were clamped"}};
  expect(loaded.usable() && loaded.notice() == "practice markers were clamped",
         "loaded sanitization diagnostics are available to the viewer");

  practice::PresetLoadResult malformed{
      .status = versioned_json::LoadStatus::Malformed,
      .diagnostics = {"practice preset JSON is malformed"}};
  expect(!malformed.usable() &&
             malformed.notice() == "practice preset JSON is malformed",
         "failed preset loads are unusable and surface their diagnostic");

  practice::PresetLoadResult future{
      .status = versioned_json::LoadStatus::FutureVersion};
  expect(!future.usable() &&
             future.notice() ==
                 "Practice presets were created by a newer version.",
         "failed loads without diagnostics still receive a concise status");
}
} // namespace

int main() {
  testRoundTripAndMutations();
  testLegacyPresetDefaultsGaugeAutoShiftOff();
  testHashMismatchIsRejected();
  testPortableFilenameClassificationIsExact();
  testPortableFileValidationRejectsInvalidData();
  testPortableFileValidationRejectsUndeclaredFieldsAndInvalidIds();
  testMalformedFileReturnsNeutralDefaults();
  testFailedLoadsReplacePriorChartStateWithNeutralCurrentChartState();
  testReplacementChartReloadUsesNewEndBeforeStateIsConsumed();
  testFirstSaveAsSeedsLastUsedFromSuppliedConfiguration();
  testFailedAtomicReplacePreservesPreviousFile();
  testLoadResultUsabilityAndNotices();
  if (failures == 0) {
    std::cout << "practice preset store tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
