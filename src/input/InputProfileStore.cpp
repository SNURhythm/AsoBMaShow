#include "InputProfileStore.h"

#include "../../yoga/lib/nlohmann/json.hpp"

#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using Json = nlohmann::json;

#ifdef INPUT_PROFILE_STORE_TESTING
bool forceFinalRenameFailure = false;
#endif

std::string_view toString(input::DeviceClass value) {
  switch (value) {
  case input::DeviceClass::Keyboard:
    return "keyboard";
  case input::DeviceClass::GameController:
    return "gameController";
  case input::DeviceClass::Joystick:
    return "joystick";
  case input::DeviceClass::Touch:
    return "touch";
  case input::DeviceClass::Midi:
    return "midi";
  case input::DeviceClass::Gyroscope:
    return "gyroscope";
  }
  throw std::invalid_argument("Unknown input device class.");
}

std::string_view toString(input::ControlKind value) {
  switch (value) {
  case input::ControlKind::Key:
    return "key";
  case input::ControlKind::Button:
    return "button";
  case input::ControlKind::Axis:
    return "axis";
  case input::ControlKind::Hat:
    return "hat";
  case input::ControlKind::TouchRegion:
    return "touchRegion";
  case input::ControlKind::MidiNote:
    return "midiNote";
  case input::ControlKind::MidiControl:
    return "midiControl";
  }
  throw std::invalid_argument("Unknown input control kind.");
}

std::string_view toString(input::ControlDirection value) {
  switch (value) {
  case input::ControlDirection::Any:
    return "any";
  case input::ControlDirection::Negative:
    return "negative";
  case input::ControlDirection::Positive:
    return "positive";
  case input::ControlDirection::Up:
    return "up";
  case input::ControlDirection::Right:
    return "right";
  case input::ControlDirection::Down:
    return "down";
  case input::ControlDirection::Left:
    return "left";
  }
  throw std::invalid_argument("Unknown input control direction.");
}

std::string_view toString(input::LogicalActionKind value) {
  switch (value) {
  case input::LogicalActionKind::Lane:
    return "lane";
  case input::LogicalActionKind::ScratchClockwise:
    return "scratchClockwise";
  case input::LogicalActionKind::ScratchCounterClockwise:
    return "scratchCounterClockwise";
  case input::LogicalActionKind::Start:
    return "start";
  case input::LogicalActionKind::Select:
    return "select";
  case input::LogicalActionKind::Pause:
    return "pause";
  case input::LogicalActionKind::Retry:
    return "retry";
  case input::LogicalActionKind::LaneCoverIncrease:
    return "laneCoverIncrease";
  case input::LogicalActionKind::LaneCoverDecrease:
    return "laneCoverDecrease";
  }
  throw std::invalid_argument("Unknown logical input action.");
}

input::DeviceClass parseDeviceClass(std::string_view value) {
  if (value == "keyboard")
    return input::DeviceClass::Keyboard;
  if (value == "gameController")
    return input::DeviceClass::GameController;
  if (value == "joystick")
    return input::DeviceClass::Joystick;
  if (value == "touch")
    return input::DeviceClass::Touch;
  if (value == "midi")
    return input::DeviceClass::Midi;
  if (value == "gyroscope")
    return input::DeviceClass::Gyroscope;
  throw std::invalid_argument("Unknown input device class: " +
                              std::string(value));
}

void parseGyroscopeConfigMember(const Json &config, const char *name,
                                int defaultValue, int &destination,
                                std::vector<std::string> &diagnostics) {
  const auto member = config.find(name);
  if (member == config.end() || !member->is_number_integer()) {
    destination = defaultValue;
    diagnostics.emplace_back("Reset missing or invalid gyroscope turntable " +
                             std::string(name) + ".");
    return;
  }

  try {
    destination = member->get<int>();
  } catch (const std::exception &) {
    destination = defaultValue;
    diagnostics.emplace_back("Reset invalid gyroscope turntable " +
                             std::string(name) + ".");
  }
}

void parseGyroscopeConfig(const Json &document, InputProfile &profile,
                          std::vector<std::string> &diagnostics) {
  const auto config = document.find("gyroscopeTurntable");
  if (config == document.end() || !config->is_object()) {
    profile.gyroscopeTurntable = {};
    diagnostics.emplace_back(
        "Reset missing or invalid gyroscope turntable settings.");
    return;
  }

  parseGyroscopeConfigMember(
      *config, "stepAngleDegrees",
      input::GyroscopeTurntableConfig::kDefaultStepAngleDegrees,
      profile.gyroscopeTurntable.stepAngleDegrees, diagnostics);
  parseGyroscopeConfigMember(
      *config, "releaseDelayMs",
      input::GyroscopeTurntableConfig::kDefaultReleaseDelayMs,
      profile.gyroscopeTurntable.releaseDelayMs, diagnostics);
}

void parseVirtualControllerConfigMember(const Json &config, const char *name,
                                        float defaultValue, float &destination,
                                        std::vector<std::string> &diagnostics) {
  const auto member = config.find(name);
  if (member == config.end() || !member->is_number()) {
    destination = defaultValue;
    diagnostics.emplace_back("Reset missing or invalid virtual controller " +
                             std::string(name) + ".");
    return;
  }
  try {
    destination = member->get<float>();
  } catch (const std::exception &) {
    destination = defaultValue;
    diagnostics.emplace_back("Reset invalid virtual controller " +
                             std::string(name) + ".");
  }
}

void parseVirtualControllerConfig(const Json &document, InputProfile &profile,
                                  bool hasAxisSpacing,
                                  std::vector<std::string> &diagnostics) {
  const auto config = document.find("virtualController");
  if (config == document.end() || !config->is_object()) {
    profile.virtualController = {};
    diagnostics.emplace_back(
        "Reset missing or invalid virtual controller settings.");
    return;
  }
  const auto enabled = config->find("enabled");
  if (enabled == config->end() || !enabled->is_boolean()) {
    profile.virtualController.enabled = false;
    diagnostics.emplace_back(
        "Reset missing or invalid virtual controller enabled setting.");
  } else {
    profile.virtualController.enabled = enabled->get<bool>();
  }
  parseVirtualControllerConfigMember(
      *config, "centerX", input::VirtualControllerConfig::kDefaultCenterX,
      profile.virtualController.centerX, diagnostics);
  parseVirtualControllerConfigMember(
      *config, "centerY", input::VirtualControllerConfig::kDefaultCenterY,
      profile.virtualController.centerY, diagnostics);
  parseVirtualControllerConfigMember(
      *config, "buttonSize",
      input::VirtualControllerConfig::kDefaultButtonSize,
      profile.virtualController.buttonSize, diagnostics);
  if (hasAxisSpacing) {
    parseVirtualControllerConfigMember(
        *config, "keySpacingX",
        input::VirtualControllerConfig::kDefaultKeySpacingX,
        profile.virtualController.keySpacingX, diagnostics);
    parseVirtualControllerConfigMember(
        *config, "keySpacingY",
        input::VirtualControllerConfig::kDefaultKeySpacingY,
        profile.virtualController.keySpacingY, diagnostics);
    parseVirtualControllerConfigMember(
        *config, "scratchKeyplateSpacing",
        input::VirtualControllerConfig::kDefaultScratchKeyplateSpacing,
        profile.virtualController.scratchKeyplateSpacing, diagnostics);
    return;
  }

  // Schema four used one non-negative gap for every relation. Preserve that
  // user's deliberate geometry during migration, then write the explicit
  // axis-based values at schema five on the next save.
  float legacyKeyGap = input::VirtualControllerConfig::kDefaultKeySpacingY;
  parseVirtualControllerConfigMember(
      *config, "keyGap", legacyKeyGap, legacyKeyGap, diagnostics);
  profile.virtualController.keySpacingX = legacyKeyGap;
  profile.virtualController.keySpacingY = legacyKeyGap;
  profile.virtualController.scratchKeyplateSpacing = legacyKeyGap;
}

input::ControlKind parseControlKind(std::string_view value) {
  if (value == "key")
    return input::ControlKind::Key;
  if (value == "button")
    return input::ControlKind::Button;
  if (value == "axis")
    return input::ControlKind::Axis;
  if (value == "hat")
    return input::ControlKind::Hat;
  if (value == "touchRegion")
    return input::ControlKind::TouchRegion;
  if (value == "midiNote")
    return input::ControlKind::MidiNote;
  if (value == "midiControl")
    return input::ControlKind::MidiControl;
  throw std::invalid_argument("Unknown input control kind: " +
                              std::string(value));
}

input::ControlDirection parseControlDirection(std::string_view value) {
  if (value == "any")
    return input::ControlDirection::Any;
  if (value == "negative")
    return input::ControlDirection::Negative;
  if (value == "positive")
    return input::ControlDirection::Positive;
  if (value == "up")
    return input::ControlDirection::Up;
  if (value == "right")
    return input::ControlDirection::Right;
  if (value == "down")
    return input::ControlDirection::Down;
  if (value == "left")
    return input::ControlDirection::Left;
  throw std::invalid_argument("Unknown input control direction: " +
                              std::string(value));
}

input::LogicalActionKind parseLogicalActionKind(std::string_view value) {
  if (value == "lane")
    return input::LogicalActionKind::Lane;
  if (value == "scratchClockwise")
    return input::LogicalActionKind::ScratchClockwise;
  if (value == "scratchCounterClockwise")
    return input::LogicalActionKind::ScratchCounterClockwise;
  if (value == "start")
    return input::LogicalActionKind::Start;
  if (value == "select")
    return input::LogicalActionKind::Select;
  if (value == "pause")
    return input::LogicalActionKind::Pause;
  if (value == "retry")
    return input::LogicalActionKind::Retry;
  if (value == "laneCoverIncrease")
    return input::LogicalActionKind::LaneCoverIncrease;
  if (value == "laneCoverDecrease")
    return input::LogicalActionKind::LaneCoverDecrease;
  throw std::invalid_argument("Unknown logical input action: " +
                              std::string(value));
}

Json serializeBinding(const input::InputBinding &binding) {
  return {
      {"id", binding.id},
      {"scope",
       {{"player", binding.scope.player}, {"keyMode", binding.scope.keyMode}}},
      {"action",
       {{"kind", toString(binding.action.kind)},
        {"lane", binding.action.lane}}},
      {"control",
       {{"deviceId", binding.control.deviceId},
        {"deviceClass", toString(binding.control.deviceClass)},
        {"kind", toString(binding.control.kind)},
        {"index", binding.control.index},
        {"direction", toString(binding.control.direction)}}},
      {"deadZone", binding.deadZone},
      {"activationThreshold", binding.activationThreshold},
      {"releaseThreshold", binding.releaseThreshold},
      {"inverted", binding.inverted},
  };
}

input::InputBinding parseBinding(const Json &document) {
  if (!document.is_object()) {
    throw std::invalid_argument("Input binding must be an object.");
  }

  const auto &scope = document.at("scope");
  const auto &action = document.at("action");
  const auto &control = document.at("control");
  if (!scope.is_object() || !action.is_object() || !control.is_object()) {
    throw std::invalid_argument(
        "Input binding scope, action, and control must be objects.");
  }

  input::InputBinding binding;
  binding.id = document.value("id", std::string{});
  binding.scope.player = scope.at("player").get<int>();
  binding.scope.keyMode = scope.at("keyMode").get<int>();
  binding.action.kind =
      parseLogicalActionKind(action.at("kind").get<std::string>());
  binding.action.lane = action.value("lane", 0);
  binding.control.deviceId = control.value("deviceId", std::string{});
  binding.control.deviceClass =
      parseDeviceClass(control.at("deviceClass").get<std::string>());
  binding.control.kind =
      parseControlKind(control.at("kind").get<std::string>());
  binding.control.index = control.at("index").get<int>();
  binding.control.direction =
      parseControlDirection(control.value("direction", std::string("any")));
  binding.deadZone = document.value("deadZone", 0.0f);
  binding.activationThreshold = document.value(
      "activationThreshold", input::kDefaultBindingActivationThreshold);
  binding.releaseThreshold = document.value(
      "releaseThreshold", input::kDefaultBindingReleaseThreshold);
  binding.inverted = document.value("inverted", false);
  return binding;
}

InputProfileLoadResult defaultsResult(InputProfileLoadStatus status,
                                      std::string diagnostic) {
  InputProfileLoadResult result;
  result.status = status;
  result.profile = makeDefaultInputProfile();
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

std::filesystem::path siblingPath(const std::filesystem::path &path,
                                  std::string_view suffix) {
  return std::filesystem::path(path.string() + std::string(suffix));
}

bool removeTemporary(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return !ignored;
}

} // namespace

InputProfileLoadResult
InputProfileStore::load(const std::filesystem::path &path) {
  std::error_code filesystemError;
  const bool exists = std::filesystem::exists(path, filesystemError);
  if (filesystemError) {
    return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                          "Unable to inspect input profile: " +
                              filesystemError.message());
  }
  if (!exists) {
    return defaultsResult(InputProfileLoadStatus::MissingDefaults,
                          "Input profile is missing; using defaults.");
  }

  try {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                            "Unable to open input profile.");
    }

    const Json document = Json::parse(input);
    if (!document.is_object()) {
      return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                            "Input profile root must be an object.");
    }

    const int schemaVersion = document.at("schemaVersion").get<int>();
    if (schemaVersion > InputProfile::kSchemaVersion) {
      return defaultsResult(InputProfileLoadStatus::FutureVersion,
                            "Input profile uses a future schema version.");
    }
    if (schemaVersion < 0 || schemaVersion > InputProfile::kSchemaVersion) {
      return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                            "Input profile schema version is unsupported.");
    }

    const auto &bindings = document.at("bindings");
    if (!bindings.is_array()) {
      return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                            "Input profile bindings must be an array.");
    }

    InputProfileLoadResult result;
    result.status = InputProfileLoadStatus::Loaded;
    result.profile.schemaVersion = InputProfile::kSchemaVersion;
    if (schemaVersion >= 2) {
      parseGyroscopeConfig(document, result.profile, result.diagnostics);
    }
    if (schemaVersion >= 4) {
      parseVirtualControllerConfig(document, result.profile, schemaVersion >= 5,
                                   result.diagnostics);
    }
    result.profile.bindings.reserve(bindings.size());
    for (const auto &binding : bindings) {
      result.profile.bindings.push_back(parseBinding(binding));
    }
    if (schemaVersion < 3 &&
        input_profile::migrateCompactScratchlessLaneBindings(result.profile)) {
      result.diagnostics.emplace_back(
          "Migrated scratchless bindings to BMS channel lanes.");
    }
    result.profile.sanitize(result.diagnostics);
    return result;
  } catch (const std::exception &error) {
    return defaultsResult(InputProfileLoadStatus::InvalidDocument,
                          "Invalid input profile: " +
                              std::string(error.what()));
  }
}

bool InputProfileStore::saveAtomic(const std::filesystem::path &path,
                                   const InputProfile &profile,
                                   std::string &errorMessage) {
  errorMessage.clear();
  if (path.empty()) {
    errorMessage = "Input profile path is empty.";
    return false;
  }

  const auto temporaryPath = siblingPath(path, ".tmp");
  const auto backupPath = siblingPath(path, ".bak");
  std::error_code filesystemError;

  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) {
      errorMessage = "Unable to create input profile directory: " +
                     filesystemError.message();
      return false;
    }
  }

  if (std::filesystem::exists(temporaryPath, filesystemError)) {
    if (filesystemError ||
        !std::filesystem::remove(temporaryPath, filesystemError)) {
      errorMessage = "Unable to remove stale input profile temporary file: " +
                     filesystemError.message();
      return false;
    }
  } else if (filesystemError) {
    errorMessage = "Unable to inspect input profile temporary file: " +
                   filesystemError.message();
    return false;
  }

  try {
    InputProfile sanitized = profile;
    sanitized.schemaVersion = InputProfile::kSchemaVersion;
    std::vector<std::string> ignoredDiagnostics;
    sanitized.sanitize(ignoredDiagnostics);

    Json document = {
        {"schemaVersion", InputProfile::kSchemaVersion},
        {"gyroscopeTurntable",
         {{"stepAngleDegrees",
           sanitized.gyroscopeTurntable.stepAngleDegrees},
          {"releaseDelayMs", sanitized.gyroscopeTurntable.releaseDelayMs}}},
        {"virtualController",
         {{"enabled", sanitized.virtualController.enabled},
          {"centerX", sanitized.virtualController.centerX},
          {"centerY", sanitized.virtualController.centerY},
          {"buttonSize", sanitized.virtualController.buttonSize},
          {"keySpacingX", sanitized.virtualController.keySpacingX},
          {"keySpacingY", sanitized.virtualController.keySpacingY},
          {"scratchKeyplateSpacing",
           sanitized.virtualController.scratchKeyplateSpacing}}},
        {"bindings", Json::array()}};
    for (const auto &binding : sanitized.bindings) {
      document["bindings"].push_back(serializeBinding(binding));
    }

    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      errorMessage = "Unable to open input profile temporary file.";
      return false;
    }
    output << document.dump(2) << '\n';
    output.flush();
    if (!output.good()) {
      errorMessage = "Unable to flush input profile temporary file.";
      output.close();
      removeTemporary(temporaryPath);
      return false;
    }
    output.close();
    if (output.fail()) {
      errorMessage = "Unable to close input profile temporary file.";
      removeTemporary(temporaryPath);
      return false;
    }
  } catch (const std::exception &error) {
    errorMessage =
        "Unable to serialize input profile: " + std::string(error.what());
    removeTemporary(temporaryPath);
    return false;
  }

  filesystemError.clear();
  const bool hadOriginal = std::filesystem::exists(path, filesystemError);
  if (filesystemError) {
    errorMessage =
        "Unable to inspect prior input profile: " + filesystemError.message();
    removeTemporary(temporaryPath);
    return false;
  }

  if (hadOriginal) {
    filesystemError.clear();
    if (std::filesystem::exists(backupPath, filesystemError)) {
      if (filesystemError ||
          !std::filesystem::remove(backupPath, filesystemError)) {
        errorMessage = "Unable to replace input profile backup: " +
                       filesystemError.message();
        removeTemporary(temporaryPath);
        return false;
      }
    } else if (filesystemError) {
      errorMessage = "Unable to inspect input profile backup: " +
                     filesystemError.message();
      removeTemporary(temporaryPath);
      return false;
    }

    std::filesystem::rename(path, backupPath, filesystemError);
    if (filesystemError) {
      errorMessage =
          "Unable to back up prior input profile: " + filesystemError.message();
      removeTemporary(temporaryPath);
      return false;
    }
  }

  filesystemError.clear();
#ifdef INPUT_PROFILE_STORE_TESTING
  if (std::exchange(forceFinalRenameFailure, false)) {
    filesystemError = std::make_error_code(std::errc::permission_denied);
  } else {
    std::filesystem::rename(temporaryPath, path, filesystemError);
  }
#else
  std::filesystem::rename(temporaryPath, path, filesystemError);
#endif

  if (!filesystemError) {
    return true;
  }

  const std::string renameFailure = filesystemError.message();
  std::string restoreFailure;
  if (hadOriginal) {
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    std::error_code restoreError;
    std::filesystem::rename(backupPath, path, restoreError);
    if (restoreError) {
      restoreFailure =
          "; unable to restore prior input profile: " + restoreError.message();
    }
  }
  removeTemporary(temporaryPath);
  errorMessage =
      "Unable to install input profile: " + renameFailure + restoreFailure;
  return false;
}

#ifdef INPUT_PROFILE_STORE_TESTING
void InputProfileStore::setForceFinalRenameFailureForTesting(
    bool forceFailure) {
  forceFinalRenameFailure = forceFailure;
}
#endif
