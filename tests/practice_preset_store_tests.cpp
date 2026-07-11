#include "AtomicFile.h"
#include "practice/PracticePresetStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
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
  testMalformedFileReturnsNeutralDefaults();
  testFailedAtomicReplacePreservesPreviousFile();
  testLoadResultUsabilityAndNotices();
  if (failures == 0) {
    std::cout << "practice preset store tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
