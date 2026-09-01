#include "input/InputProfile.h"
#include "input/InputProfileStore.h"

#include <SDL2/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::filesystem::path fixturePath(std::string_view name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "input" /
         name;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  require(output.good(), "test fixture write succeeds");
}

bool sameBinding(const input::InputBinding &left,
                 const input::InputBinding &right) {
  return left.id == right.id && left.scope == right.scope &&
         left.action == right.action && left.control == right.control &&
         left.deadZone == right.deadZone &&
         left.activationThreshold == right.activationThreshold &&
         left.releaseThreshold == right.releaseThreshold &&
         left.inverted == right.inverted;
}

bool hasNonemptyUniqueBindingIds(const InputProfile &profile) {
  std::set<std::string> ids;
  for (const auto &binding : profile.bindings) {
    if (binding.id.empty() || !ids.insert(binding.id).second) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> bindingIds(const InputProfile &profile) {
  std::vector<std::string> ids;
  ids.reserve(profile.bindings.size());
  for (const auto &binding : profile.bindings) {
    ids.push_back(binding.id);
  }
  return ids;
}

struct ExpectedKeyBinding {
  input::InputScope scope;
  int lane;
  SDL_Scancode scancode;
};

void verifyCurrentKeyboardDefaults(const InputProfile &defaults) {
  const std::vector<ExpectedKeyBinding> expectedLanes = {
      {{1, 4}, 0, SDL_SCANCODE_D},        {{1, 4}, 1, SDL_SCANCODE_F},
      {{1, 4}, 3, SDL_SCANCODE_J},        {{1, 4}, 4, SDL_SCANCODE_K},

      {{1, 5}, 0, SDL_SCANCODE_D},        {{1, 5}, 1, SDL_SCANCODE_F},
      {{1, 5}, 2, SDL_SCANCODE_SPACE},    {{1, 5}, 3, SDL_SCANCODE_J},
      {{1, 5}, 4, SDL_SCANCODE_K},

      {{1, 6}, 0, SDL_SCANCODE_S},        {{1, 6}, 1, SDL_SCANCODE_D},
      {{1, 6}, 2, SDL_SCANCODE_F},        {{1, 6}, 4, SDL_SCANCODE_J},
      {{1, 6}, 5, SDL_SCANCODE_K},        {{1, 6}, 6, SDL_SCANCODE_L},

      {{1, 7}, 0, SDL_SCANCODE_S},        {{1, 7}, 1, SDL_SCANCODE_D},
      {{1, 7}, 2, SDL_SCANCODE_F},        {{1, 7}, 3, SDL_SCANCODE_SPACE},
      {{1, 7}, 4, SDL_SCANCODE_J},        {{1, 7}, 5, SDL_SCANCODE_K},
      {{1, 7}, 6, SDL_SCANCODE_L},

      {{1, 8}, 7, SDL_SCANCODE_A},        {{1, 8}, 0, SDL_SCANCODE_S},
      {{1, 8}, 1, SDL_SCANCODE_D},        {{1, 8}, 2, SDL_SCANCODE_F},
      {{1, 8}, 3, SDL_SCANCODE_J},        {{1, 8}, 4, SDL_SCANCODE_K},
      {{1, 8}, 5, SDL_SCANCODE_L},        {{1, 8}, 6, SDL_SCANCODE_SEMICOLON},

      {{1, 10}, 0, SDL_SCANCODE_Z},       {{1, 10}, 1, SDL_SCANCODE_S},
      {{1, 10}, 2, SDL_SCANCODE_X},       {{1, 10}, 3, SDL_SCANCODE_D},
      {{1, 10}, 4, SDL_SCANCODE_C},
      {{2, 10}, 8, SDL_SCANCODE_COMMA},   {{2, 10}, 9, SDL_SCANCODE_L},
      {{2, 10}, 10, SDL_SCANCODE_PERIOD}, {{2, 10}, 11, SDL_SCANCODE_SEMICOLON},
      {{2, 10}, 12, SDL_SCANCODE_SLASH},

      {{1, 14}, 0, SDL_SCANCODE_Z},       {{1, 14}, 1, SDL_SCANCODE_S},
      {{1, 14}, 2, SDL_SCANCODE_X},       {{1, 14}, 3, SDL_SCANCODE_D},
      {{1, 14}, 4, SDL_SCANCODE_C},       {{1, 14}, 5, SDL_SCANCODE_F},
      {{1, 14}, 6, SDL_SCANCODE_V},
      {{2, 14}, 8, SDL_SCANCODE_M},       {{2, 14}, 9, SDL_SCANCODE_K},
      {{2, 14}, 10, SDL_SCANCODE_COMMA},  {{2, 14}, 11, SDL_SCANCODE_L},
      {{2, 14}, 12, SDL_SCANCODE_PERIOD}, {{2, 14}, 13, SDL_SCANCODE_SEMICOLON},
      {{2, 14}, 14, SDL_SCANCODE_SLASH},
  };

  struct ExpectedScratchBinding {
    input::InputScope scope;
    input::LogicalActionKind action;
    SDL_Scancode scancode;
  };
  const std::vector<ExpectedScratchBinding> expectedScratch = {
      {{1, 5}, input::LogicalActionKind::ScratchCounterClockwise,
       SDL_SCANCODE_LSHIFT},
      {{1, 5}, input::LogicalActionKind::ScratchClockwise,
       SDL_SCANCODE_RSHIFT},
      {{1, 7}, input::LogicalActionKind::ScratchCounterClockwise,
       SDL_SCANCODE_LSHIFT},
      {{1, 7}, input::LogicalActionKind::ScratchClockwise,
       SDL_SCANCODE_RSHIFT},
      {{1, 10}, input::LogicalActionKind::ScratchCounterClockwise,
       SDL_SCANCODE_LSHIFT},
      {{2, 10}, input::LogicalActionKind::ScratchClockwise,
       SDL_SCANCODE_RSHIFT},
      {{1, 14}, input::LogicalActionKind::ScratchCounterClockwise,
       SDL_SCANCODE_LSHIFT},
      {{2, 14}, input::LogicalActionKind::ScratchClockwise,
       SDL_SCANCODE_RSHIFT},
  };

  require(defaults.bindings.size() ==
              expectedLanes.size() + expectedScratch.size(),
          "default profile contains exactly the keyboard bindings");
  for (const auto &binding : expectedLanes) {
    require(defaults.hasDigitalBinding(
                binding.scope, {input::LogicalActionKind::Lane, binding.lane},
                "keyboard", binding.scancode),
            "default keyboard lane binding is present");
  }
  for (const auto &binding : expectedScratch) {
    require(defaults.hasDigitalBinding(binding.scope, {binding.action, 0},
                                       "keyboard", binding.scancode),
            "default directional scratch binding is present");
  }

  require(defaults.bindingsFor({1, 10}).size() == 6,
          "10-key player one has five keys and left scratch");
  require(defaults.bindingsFor({2, 10}).size() == 6,
          "10-key player two has five keys and right scratch");
  require(defaults.bindingsFor({1, 14}).size() == 8,
          "14-key player one has seven keys and left scratch");
  require(defaults.bindingsFor({2, 14}).size() == 8,
          "14-key player two has seven keys and right scratch");
  require(defaults.bindingsFor({2, 7}).empty(),
          "single-player modes do not synthesize player two bindings");
}

} // namespace

int main() {
  try {
    const InputProfile defaults = makeDefaultInputProfile();
    const input::InputBinding canonicalDefaults;
    require(canonicalDefaults.activationThreshold == 0.20F &&
                canonicalDefaults.releaseThreshold == 0.10F,
            "new bindings use the sensitive threshold defaults");
    for (const auto &binding : defaults.bindings) {
      require(binding.activationThreshold == 0.20F &&
                  binding.releaseThreshold == 0.10F,
              "default profile bindings use the canonical thresholds");
    }
    require(defaults.schemaVersion == InputProfile::kSchemaVersion,
            "defaults use the current schema");
    require(defaults.gyroscopeTurntable ==
                input::GyroscopeTurntableConfig{},
            "defaults use the canonical gyroscope turntable settings");
    require(defaults.virtualController == input::VirtualControllerConfig{},
            "defaults keep the optional virtual controller disabled");
    verifyCurrentKeyboardDefaults(defaults);

    input::InputBinding invalid = defaults.bindings.front();
    invalid.scope = {0, 0};
    invalid.deadZone = std::numeric_limits<float>::quiet_NaN();
    invalid.releaseThreshold = 0.9f;
    invalid.activationThreshold = 0.2f;
    InputProfile invalidProfile{.bindings = {invalid}};
    std::vector<std::string> diagnostics;
    invalidProfile.sanitize(diagnostics);
    require(invalidProfile.bindings.front().scope == input::InputScope{1, 7},
            "invalid scope falls back to player one 7-key");
    require(std::isfinite(invalidProfile.bindings.front().deadZone),
            "non-finite normalized values are repaired");
    require(invalidProfile.bindings.front().deadZone >= 0.0f &&
                invalidProfile.bindings.front().deadZone <
                    invalidProfile.bindings.front().releaseThreshold &&
                invalidProfile.bindings.front().releaseThreshold <
                    invalidProfile.bindings.front().activationThreshold &&
                invalidProfile.bindings.front().activationThreshold <= 1.0f,
            "sanitized thresholds are ordered and normalized");
    require(invalidProfile.bindings.front().activationThreshold == 0.20F &&
                invalidProfile.bindings.front().releaseThreshold == 0.10F,
            "non-finite thresholds recover to the canonical defaults");
    require(!diagnostics.empty(), "sanitization describes repairs");

    InputProfile customKeyModeProfile = defaults;
    customKeyModeProfile.bindings.front().scope.keyMode = 17;
    diagnostics.clear();
    customKeyModeProfile.sanitize(diagnostics);
    require(customKeyModeProfile.bindings.front().scope.keyMode == 17,
            "positive custom key counts survive input sanitization");

    InputProfile oldSchemaProfile = defaults;
    oldSchemaProfile.schemaVersion = 1;
    diagnostics.clear();
    oldSchemaProfile.sanitize(diagnostics);
    require(
        oldSchemaProfile.schemaVersion == InputProfile::kSchemaVersion &&
            std::ranges::find(diagnostics,
                              "Reset unsupported input schema version to 7.") !=
                diagnostics.end(),
        "schema repair diagnostics report the real current version");

    input::InputBinding invalidOrder = defaults.bindings.front();
    invalidOrder.deadZone = 0.0F;
    invalidOrder.releaseThreshold = 0.9F;
    invalidOrder.activationThreshold = 0.2F;
    InputProfile invalidOrderProfile{.bindings = {invalidOrder}};
    diagnostics.clear();
    invalidOrderProfile.sanitize(diagnostics);
    require(invalidOrderProfile.bindings.front().activationThreshold == 0.20F &&
                invalidOrderProfile.bindings.front().releaseThreshold == 0.10F,
            "misordered thresholds recover to the canonical defaults");

    input::InputBinding missingDevice = defaults.bindings.front();
    missingDevice.id = "missing-device";
    missingDevice.control.deviceId.clear();
    input::InputBinding exactDuplicate = missingDevice;
    input::InputBinding distinctBinding = missingDevice;
    distinctBinding.id = "distinct-binding";
    InputProfile duplicateProfile{
        .bindings = {missingDevice, exactDuplicate, distinctBinding}};
    diagnostics.clear();
    duplicateProfile.sanitize(diagnostics);
    require(duplicateProfile.bindings.size() == 2,
            "only exact duplicate bindings are removed");
    require(duplicateProfile.bindings.front().control.deviceId.empty(),
            "missing device IDs are retained");

    input::InputBinding blankId = defaults.bindings[0];
    blankId.id.clear();
    input::InputBinding duplicateIdA = defaults.bindings[1];
    duplicateIdA.id = "collision";
    input::InputBinding duplicateIdB = defaults.bindings[2];
    duplicateIdB.id = "collision";
    input::InputBinding reservedSuffix = defaults.bindings[3];
    reservedSuffix.id = "collision-2";
    InputProfile idRepairProfile{
        .bindings = {blankId, duplicateIdA, duplicateIdB, reservedSuffix}};
    InputProfile repeatRepair = idRepairProfile;
    diagnostics.clear();
    idRepairProfile.sanitize(diagnostics);
    std::vector<std::string> repeatDiagnostics;
    repeatRepair.sanitize(repeatDiagnostics);
    require(idRepairProfile.bindings.size() == 4,
            "ID repair preserves every non-exact binding");
    require(hasNonemptyUniqueBindingIds(idRepairProfile),
            "sanitization repairs blank and colliding binding IDs");
    require(bindingIds(idRepairProfile) == bindingIds(repeatRepair),
            "binding ID collision repair is deterministic");
    require(!diagnostics.empty(), "binding ID repairs emit diagnostics");

    input::InputBinding occupied = defaults.bindings.front();
    input::InputBinding otherScope = occupied;
    otherScope.id = "other-scope";
    otherScope.scope.player = 2;
    InputProfile conflictProfile{.bindings = {occupied, otherScope}};

    input::InputBinding candidate = occupied;
    candidate.id = "same-action-candidate";
    require(conflictProfile.conflictsWith(candidate).empty(),
            "the same scoped control and action is not a conflict");

    candidate = occupied;
    candidate.action.lane = 99;
    const auto conflicts = conflictProfile.conflictsWith(candidate);
    require(conflicts.size() == 1 && sameBinding(conflicts.front(), occupied),
            "conflicts are independent of binding IDs");
    candidate.control.index = SDL_SCANCODE_UNKNOWN;
    require(conflictProfile.conflictsWith(candidate).empty(),
            "different physical controls do not conflict");

    const auto fixtureResult =
        InputProfileStore::load(fixturePath("input-v1.json"));
    require(fixtureResult.status == InputProfileLoadStatus::Loaded,
            "version-one fixture loads");
    require(fixtureResult.profile.bindings.size() == 1,
            "version-one fixture retains its binding");
    require(fixtureResult.profile.bindings.front().control.deviceId.empty(),
            "omitted fixture device ID is represented as missing");
    require(fixtureResult.profile.bindings.front().control.index ==
                2 * 128 + 60,
            "MIDI indices use channel times 128 plus note");
    require(
        fixtureResult.profile.bindings.front().activationThreshold == 0.60F &&
            fixtureResult.profile.bindings.front().releaseThreshold == 0.40F,
        "valid explicit thresholds remain unchanged");
    require(fixtureResult.profile.gyroscopeTurntable ==
                input::GyroscopeTurntableConfig{},
            "version-one profiles migrate with default gyroscope settings");

    const auto testRoot = std::filesystem::temp_directory_path() /
                          "asobmashow_input_profile_tests";
    std::filesystem::remove_all(testRoot);
    std::filesystem::create_directories(testRoot);

    const auto repairedIdsPath = testRoot / "repaired-ids.json";
    writeFile(repairedIdsPath, R"json({
  "schemaVersion": 1,
  "bindings": [
    {
      "id": "",
      "scope": {"player": 1, "keyMode": 7},
      "action": {"kind": "lane", "lane": 0},
      "control": {"deviceId": "pad:one", "deviceClass": "gameController", "kind": "button", "index": 0, "direction": "any"}
    },
    {
      "id": "imported-collision",
      "scope": {"player": 1, "keyMode": 7},
      "action": {"kind": "lane", "lane": 1},
      "control": {"deviceId": "pad:one", "deviceClass": "gameController", "kind": "button", "index": 1, "direction": "any"}
    },
    {
      "id": "imported-collision",
      "scope": {"player": 1, "keyMode": 7},
      "action": {"kind": "lane", "lane": 2},
      "control": {"deviceId": "pad:one", "deviceClass": "gameController", "kind": "button", "index": 2, "direction": "any"}
    }
  ]
})json");
    const auto repairedIdsResult = InputProfileStore::load(repairedIdsPath);
    require(repairedIdsResult.status == InputProfileLoadStatus::Loaded,
            "an imported profile with repairable IDs still loads");
    require(repairedIdsResult.profile.bindings.size() == 3 &&
                hasNonemptyUniqueBindingIds(repairedIdsResult.profile),
            "load sanitization retains distinct bindings and repairs their "
            "IDs");
    for (const auto &binding : repairedIdsResult.profile.bindings) {
      require(binding.activationThreshold == 0.20F &&
                  binding.releaseThreshold == 0.10F,
              "missing threshold fields load with canonical defaults");
    }
    require(!repairedIdsResult.diagnostics.empty(),
            "load reports binding ID repairs");

    const auto missingPath = testRoot / "missing-input.json";
    const auto missingResult = InputProfileStore::load(missingPath);
    require(missingResult.status == InputProfileLoadStatus::MissingDefaults,
            "missing input file selects defaults");
    require(missingResult.profile.bindings.size() == defaults.bindings.size(),
            "missing input file returns the current defaults");
    require(!std::filesystem::exists(missingPath),
            "loading a missing file does not create it");

    const auto compactScratchlessV2Path =
        testRoot / "compact-scratchless-v2.json";
    writeFile(compactScratchlessV2Path, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": {"stepAngleDegrees": 4, "releaseDelayMs": 250},
  "bindings": [
    {"id":"4k-third","scope":{"player":1,"keyMode":4},"action":{"kind":"lane","lane":2},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":13,"direction":"any"}},
    {"id":"4k-fourth","scope":{"player":1,"keyMode":4},"action":{"kind":"lane","lane":3},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":14,"direction":"any"}},
    {"id":"6k-fourth","scope":{"player":1,"keyMode":6},"action":{"kind":"lane","lane":3},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":15,"direction":"any"}},
    {"id":"6k-sixth","scope":{"player":1,"keyMode":6},"action":{"kind":"lane","lane":5},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":16,"direction":"any"}},
    {"id":"8k-first","scope":{"player":1,"keyMode":8},"action":{"kind":"lane","lane":0},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":17,"direction":"any"}},
    {"id":"8k-last","scope":{"player":1,"keyMode":8},"action":{"kind":"lane","lane":7},"control":{"deviceId":"keyboard","deviceClass":"keyboard","kind":"key","index":18,"direction":"any"}}
  ]
})json");
    const auto compactScratchlessV2 =
        InputProfileStore::load(compactScratchlessV2Path);
    require(compactScratchlessV2.status == InputProfileLoadStatus::Loaded &&
                compactScratchlessV2.profile.schemaVersion ==
                    InputProfile::kSchemaVersion,
            "version-two scratchless bindings migrate to the current schema");
    const std::vector<int> migratedScratchlessLanes = [&] {
      std::vector<int> lanes;
      for (const auto &binding : compactScratchlessV2.profile.bindings) {
        lanes.push_back(binding.action.lane);
      }
      return lanes;
    }();
    require(migratedScratchlessLanes ==
                std::vector<int>{3, 4, 4, 6, 7, 6},
            "4K, 6K, and 8K compact bindings migrate to their BMS channel "
            "lanes");
    require(compactScratchlessV2.profile.gyroscopeTurntable.stepAngleDegrees ==
                    4 &&
                compactScratchlessV2.profile.gyroscopeTurntable
                        .releaseDelayMs == 250,
            "lane migration preserves version-two gyroscope settings");

    const auto gyroscopeV2Path = testRoot / "gyroscope-v2.json";
    writeFile(gyroscopeV2Path, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": {
    "stepAngleDegrees": 7,
    "releaseDelayMs": 350
  },
  "bindings": [
    {
      "id": "gyro-clockwise",
      "scope": {"player": 1, "keyMode": 7},
      "action": {"kind": "scratchClockwise", "lane": 0},
      "control": {
        "deviceId": "builtin:gyroscope-turntable",
        "deviceClass": "gyroscope",
        "kind": "axis",
        "index": 0,
        "direction": "positive"
      }
    }
  ]
})json");
    const auto gyroscopeV2Result = InputProfileStore::load(gyroscopeV2Path);
    require(gyroscopeV2Result.status == InputProfileLoadStatus::Loaded,
            "version-two gyroscope profile loads");
    require(gyroscopeV2Result.profile.schemaVersion ==
                InputProfile::kSchemaVersion &&
                gyroscopeV2Result.profile.gyroscopeTurntable.stepAngleDegrees ==
                    7 &&
                gyroscopeV2Result.profile.gyroscopeTurntable.releaseDelayMs ==
                    350,
            "version-two gyroscope settings persist");
    require(gyroscopeV2Result.profile.bindings.size() == 1 &&
                gyroscopeV2Result.profile.bindings.front()
                        .control.deviceClass == input::DeviceClass::Gyroscope,
            "gyroscope device class persists on an axis binding");

    const auto missingConfigFieldPath = testRoot / "missing-config-field.json";
    writeFile(missingConfigFieldPath, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": {"stepAngleDegrees": 9},
  "bindings": []
})json");
    const auto missingConfigFieldResult =
        InputProfileStore::load(missingConfigFieldPath);
    require(missingConfigFieldResult.status == InputProfileLoadStatus::Loaded &&
                missingConfigFieldResult.profile.gyroscopeTurntable
                        .stepAngleDegrees == 9 &&
                missingConfigFieldResult.profile.gyroscopeTurntable
                        .releaseDelayMs ==
                    input::GyroscopeTurntableConfig::kDefaultReleaseDelayMs,
            "a missing config member resets only that member");
    require(!missingConfigFieldResult.diagnostics.empty(),
            "a missing config member produces a diagnostic");

    const auto wrongConfigTypePath = testRoot / "wrong-config-type.json";
    writeFile(wrongConfigTypePath, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": {
    "stepAngleDegrees": "three",
    "releaseDelayMs": 425
  },
  "bindings": []
})json");
    const auto wrongConfigTypeResult =
        InputProfileStore::load(wrongConfigTypePath);
    require(wrongConfigTypeResult.status == InputProfileLoadStatus::Loaded &&
                wrongConfigTypeResult.profile.gyroscopeTurntable
                        .stepAngleDegrees ==
                    input::GyroscopeTurntableConfig::kDefaultStepAngleDegrees &&
                wrongConfigTypeResult.profile.gyroscopeTurntable
                        .releaseDelayMs == 425,
            "a wrong-type config member resets without discarding its sibling");
    require(!wrongConfigTypeResult.diagnostics.empty(),
            "a wrong-type config member produces a diagnostic");

    const auto missingConfigObjectPath = testRoot / "missing-config-object.json";
    writeFile(missingConfigObjectPath, R"json({
  "schemaVersion": 2,
  "bindings": [
    {
      "id": "surviving-binding",
      "scope": {"player": 1, "keyMode": 7},
      "action": {"kind": "lane", "lane": 1},
      "control": {
        "deviceId": "keyboard",
        "deviceClass": "keyboard",
        "kind": "key",
        "index": 7,
        "direction": "any"
      }
    }
  ]
})json");
    const auto missingConfigObjectResult =
        InputProfileStore::load(missingConfigObjectPath);
    require(missingConfigObjectResult.status == InputProfileLoadStatus::Loaded &&
                missingConfigObjectResult.profile.gyroscopeTurntable ==
                    input::GyroscopeTurntableConfig{} &&
                missingConfigObjectResult.profile.bindings.size() == 1 &&
                missingConfigObjectResult.profile.bindings.front().id ==
                    "surviving-binding",
            "a missing config object recovers defaults without losing bindings");
    require(!missingConfigObjectResult.diagnostics.empty(),
            "a missing config object produces a diagnostic");

    const auto invalidConfigObjectPath = testRoot / "invalid-config-object.json";
    writeFile(invalidConfigObjectPath, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": [3, 200],
  "bindings": []
})json");
    const auto invalidConfigObjectResult =
        InputProfileStore::load(invalidConfigObjectPath);
    require(invalidConfigObjectResult.status == InputProfileLoadStatus::Loaded &&
                invalidConfigObjectResult.profile.gyroscopeTurntable ==
                    input::GyroscopeTurntableConfig{},
            "a non-object config recovers both default settings");
    require(!invalidConfigObjectResult.diagnostics.empty(),
            "a non-object config produces a diagnostic");

    const auto clampedConfigPath = testRoot / "clamped-config.json";
    writeFile(clampedConfigPath, R"json({
  "schemaVersion": 2,
  "gyroscopeTurntable": {
    "stepAngleDegrees": 0,
    "releaseDelayMs": 5000
  },
  "bindings": []
})json");
    const auto clampedConfigResult = InputProfileStore::load(clampedConfigPath);
    require(clampedConfigResult.status == InputProfileLoadStatus::Loaded &&
                clampedConfigResult.profile.gyroscopeTurntable
                        .stepAngleDegrees ==
                    input::GyroscopeTurntableConfig::kMinStepAngleDegrees &&
                clampedConfigResult.profile.gyroscopeTurntable
                        .releaseDelayMs ==
                    input::GyroscopeTurntableConfig::kMaxReleaseDelayMs,
            "typed gyroscope settings clamp independently to their ranges");
    require(clampedConfigResult.diagnostics.size() >= 2,
            "each clamped config member produces a diagnostic");

    const auto versionZeroPath = testRoot / "input-v0.json";
    std::string versionZero = readFile(fixturePath("input-v1.json"));
    const std::string currentVersion = "\"schemaVersion\": 1";
    const auto versionPosition = versionZero.find(currentVersion);
    require(versionPosition != std::string::npos,
            "version-zero fixture source contains a schema version");
    versionZero.replace(versionPosition, currentVersion.size(),
                        "\"schemaVersion\": 0");
    writeFile(versionZeroPath, versionZero);
    const auto versionZeroResult = InputProfileStore::load(versionZeroPath);
    require(versionZeroResult.status == InputProfileLoadStatus::Loaded &&
                versionZeroResult.profile.schemaVersion ==
                    InputProfile::kSchemaVersion &&
                versionZeroResult.profile.bindings.size() == 1 &&
                sameBinding(versionZeroResult.profile.bindings.front(),
                            fixtureResult.profile.bindings.front()),
            "version-zero input migrates in memory to the current schema");
    require(readFile(versionZeroPath) == versionZero,
            "loading version zero does not rewrite source bytes");
    const auto migratedVersionZeroPath = testRoot / "migrated-input-v0.json";
    std::string errorMessage;
    require(
        InputProfileStore::saveAtomic(
            migratedVersionZeroPath, versionZeroResult.profile, errorMessage) &&
            readFile(migratedVersionZeroPath).find("\"schemaVersion\": 7") !=
                std::string::npos,
        "saving migrated version zero persists the current schema");

    const auto negativeVersionPath = testRoot / "negative-version.json";
    writeFile(negativeVersionPath, "{\"schemaVersion\":-1,\"bindings\":[]}\n");
    require(InputProfileStore::load(negativeVersionPath).status ==
                InputProfileLoadStatus::InvalidDocument,
            "negative input schema versions fail closed");

    const auto roundTripPath = testRoot / "round-trip.json";
    errorMessage.clear();
    require(InputProfileStore::saveAtomic(roundTripPath, fixtureResult.profile,
                                          errorMessage),
            "valid profile saves atomically");
    const auto roundTripResult = InputProfileStore::load(roundTripPath);
    require(roundTripResult.status == InputProfileLoadStatus::Loaded,
            "saved profile reloads");
    require(roundTripResult.profile.bindings.size() == 1 &&
                sameBinding(roundTripResult.profile.bindings.front(),
                            fixtureResult.profile.bindings.front()),
            "JSON round trip preserves a missing device ID and binding fields");

    const auto gyroscopeRoundTripPath = testRoot / "gyroscope-round-trip.json";
    errorMessage.clear();
    require(InputProfileStore::saveAtomic(
                gyroscopeRoundTripPath, gyroscopeV2Result.profile,
                errorMessage),
            "gyroscope profile saves atomically");
    const std::string gyroscopeRoundTripJson = readFile(gyroscopeRoundTripPath);
    require(
        gyroscopeRoundTripJson.find("\"schemaVersion\": 7") !=
                std::string::npos &&
            gyroscopeRoundTripJson.find("\"gyroscopeTurntable\"") !=
                std::string::npos &&
            gyroscopeRoundTripJson.find("\"stepAngleDegrees\": 7") !=
                std::string::npos &&
            gyroscopeRoundTripJson.find("\"releaseDelayMs\": 350") !=
                std::string::npos &&
            gyroscopeRoundTripJson.find("\"deviceClass\": \"gyroscope\"") !=
                std::string::npos,
        "current schema serialization includes config and device vocabulary");
    const auto gyroscopeRoundTripResult =
        InputProfileStore::load(gyroscopeRoundTripPath);
    require(gyroscopeRoundTripResult.status == InputProfileLoadStatus::Loaded &&
                gyroscopeRoundTripResult.profile.gyroscopeTurntable ==
                    gyroscopeV2Result.profile.gyroscopeTurntable &&
                gyroscopeRoundTripResult.profile.bindings.size() == 1 &&
                sameBinding(gyroscopeRoundTripResult.profile.bindings.front(),
                            gyroscopeV2Result.profile.bindings.front()),
            "version-two gyroscope profile round trips without loss");

    InputProfile virtualControllerProfile = defaults;
    virtualControllerProfile.virtualController = {
        .enabled = true,
        .scratchMode = input::VirtualControllerScratchMode::Spin,
        .player = input::VirtualControllerPlayer::Player2,
        .centerX = 0.41F,
        .centerY = 0.72F,
        .buttonSize = 0.16F,
        .keySpacingX = -0.34F,
        .keySpacingY = 0.21F,
        .scratchKeyplateSpacing = -0.12F,
    };
    const auto virtualControllerRoundTripPath =
        testRoot / "virtual-controller-round-trip.json";
    errorMessage.clear();
    require(InputProfileStore::saveAtomic(virtualControllerRoundTripPath,
                                          virtualControllerProfile,
                                          errorMessage),
            "virtual controller profile saves atomically");
    const auto virtualControllerRoundTrip =
        InputProfileStore::load(virtualControllerRoundTripPath);
    require(virtualControllerRoundTrip.status == InputProfileLoadStatus::Loaded &&
                virtualControllerRoundTrip.profile.virtualController ==
                    virtualControllerProfile.virtualController,
            "virtual controller enablement, placement, size, and independent signed spacing round trip");
    const std::string virtualControllerJson =
        readFile(virtualControllerRoundTripPath);
    require(virtualControllerJson.find("\"schemaVersion\": 7") !=
                    std::string::npos &&
                virtualControllerJson.find("\"scratchMode\": \"spin\"") !=
                    std::string::npos &&
                virtualControllerJson.find("\"player\": 2") !=
                    std::string::npos &&
                virtualControllerJson.find("\"keySpacingX\"") !=
                    std::string::npos &&
                virtualControllerJson.find("\"keySpacingY\"") !=
                    std::string::npos &&
                virtualControllerJson.find("\"scratchKeyplateSpacing\"") !=
                    std::string::npos &&
                virtualControllerJson.find("\"keyGap\"") == std::string::npos,
            "virtual-controller geometry, player, and scratch mode serialize "
            "in schema seven");

    const auto legacyVirtualControllerPath =
        testRoot / "virtual-controller-v4.json";
    writeFile(legacyVirtualControllerPath,
              R"({"schemaVersion":4,"gyroscopeTurntable":{"stepAngleDegrees":6,"releaseDelayMs":250},"virtualController":{"enabled":true,"centerX":0.5,"centerY":0.7,"buttonSize":0.1,"keyGap":0.3},"bindings":[]})");
    const auto legacyVirtualController =
        InputProfileStore::load(legacyVirtualControllerPath);
    require(legacyVirtualController.status == InputProfileLoadStatus::Loaded &&
                legacyVirtualController.profile.virtualController.enabled &&
                legacyVirtualController.profile.virtualController.scratchMode ==
                    input::VirtualControllerScratchMode::Flick &&
                legacyVirtualController.profile.virtualController.player ==
                    input::VirtualControllerPlayer::Player1 &&
                legacyVirtualController.profile.virtualController.keySpacingX ==
                    0.3F &&
                legacyVirtualController.profile.virtualController.keySpacingY ==
                    0.3F &&
                legacyVirtualController.profile.virtualController
                        .scratchKeyplateSpacing == 0.3F,
            "schema-four virtual controller profiles migrate their legacy gap to every explicit spacing relation");

    const auto malformedPath = testRoot / "malformed.json";
    writeFile(malformedPath, "{ not valid json");
    const std::string malformedBefore = readFile(malformedPath);
    const auto malformedResult = InputProfileStore::load(malformedPath);
    require(malformedResult.status == InputProfileLoadStatus::InvalidDocument,
            "malformed JSON is rejected");
    require(readFile(malformedPath) == malformedBefore,
            "malformed JSON is never rewritten while loading");

    const auto futurePath = fixturePath("input-future.json");
    const std::string futureBefore = readFile(futurePath);
    const auto futureResult = InputProfileStore::load(futurePath);
    require(futureResult.status == InputProfileLoadStatus::FutureVersion,
            "future schemas are reported separately");
    require(readFile(futurePath) == futureBefore,
            "future schemas are never rewritten while loading");

    const auto rollbackPath = testRoot / "rollback.json";
    constexpr std::string_view priorContents = "prior input profile bytes\n";
    writeFile(rollbackPath, priorContents);
#ifdef INPUT_PROFILE_STORE_TESTING
    InputProfileStore::setForceFinalRenameFailureForTesting(true);
#else
#error input_profile_tests requires the deterministic rename-failure seam
#endif
    errorMessage.clear();
    require(
        !InputProfileStore::saveAtomic(rollbackPath, defaults, errorMessage),
        "forced final rename failure is reported");
    require(!errorMessage.empty(), "rename failure includes an error message");
    require(readFile(rollbackPath) == priorContents,
            "failed final rename restores the exact prior file");
    require(!std::filesystem::exists(rollbackPath.string() + ".tmp"),
            "failed atomic save cleans up its temporary file");

    std::filesystem::remove_all(testRoot);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "input_profile_tests: " << error.what() << '\n';
    return 1;
  }
}
