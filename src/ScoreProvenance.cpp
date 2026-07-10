#include "ScoreProvenance.h"

#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::ordered_json;

template <typename Enum> int enumValue(Enum value) {
  return static_cast<int>(value);
}

void canonicalizeDevices(std::vector<InputDeviceCategory> &devices) {
  std::sort(devices.begin(), devices.end(),
            [](auto lhs, auto rhs) { return enumValue(lhs) < enumValue(rhs); });
  devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
}

void canonicalizeWindows(std::vector<JudgeWindowProvenance> &windows) {
  std::stable_sort(windows.begin(), windows.end(),
                   [](const auto &lhs, const auto &rhs) {
                     return enumValue(lhs.judgement) < enumValue(rhs.judgement);
                   });
}

ScoreEligibility worseEligibility(ScoreEligibility lhs, ScoreEligibility rhs) {
  return enumValue(lhs) >= enumValue(rhs) ? lhs : rhs;
}

const char *eligibilityName(ScoreEligibility value) {
  switch (value) {
  case ScoreEligibility::Verified:
    return "verified";
  case ScoreEligibility::Modified:
    return "modified";
  case ScoreEligibility::LegacyUnverified:
    return "legacy-unverified";
  }
  throw std::invalid_argument("Unknown score eligibility value.");
}

std::optional<ScoreEligibility> eligibilityFromName(std::string_view value) {
  if (value == "verified") {
    return ScoreEligibility::Verified;
  }
  if (value == "modified") {
    return ScoreEligibility::Modified;
  }
  if (value == "legacy-unverified") {
    return ScoreEligibility::LegacyUnverified;
  }
  return std::nullopt;
}

const char *judgeRankSourceName(JudgeRankSource value) {
  switch (value) {
  case JudgeRankSource::Chart:
    return "chart";
  case JudgeRankSource::CourseConstraint:
    return "course-constraint";
  case JudgeRankSource::Override:
    return "override";
  case JudgeRankSource::Unknown:
    return "unknown";
  }
  throw std::invalid_argument("Unknown judge-rank source value.");
}

std::optional<JudgeRankSource> judgeRankSourceFromName(std::string_view value) {
  if (value == "chart") {
    return JudgeRankSource::Chart;
  }
  if (value == "course-constraint") {
    return JudgeRankSource::CourseConstraint;
  }
  if (value == "override") {
    return JudgeRankSource::Override;
  }
  if (value == "unknown") {
    return JudgeRankSource::Unknown;
  }
  return std::nullopt;
}

const char *inputDeviceName(InputDeviceCategory value) {
  switch (value) {
  case InputDeviceCategory::Keyboard:
    return "keyboard";
  case InputDeviceCategory::GameController:
    return "game-controller";
  case InputDeviceCategory::Joystick:
    return "joystick";
  case InputDeviceCategory::Touch:
    return "touch";
  case InputDeviceCategory::Midi:
    return "midi";
  case InputDeviceCategory::Unknown:
    return "unknown";
  }
  throw std::invalid_argument("Unknown input-device category value.");
}

std::optional<InputDeviceCategory> inputDeviceFromName(std::string_view value) {
  if (value == "keyboard") {
    return InputDeviceCategory::Keyboard;
  }
  if (value == "game-controller") {
    return InputDeviceCategory::GameController;
  }
  if (value == "joystick") {
    return InputDeviceCategory::Joystick;
  }
  if (value == "touch") {
    return InputDeviceCategory::Touch;
  }
  if (value == "midi") {
    return InputDeviceCategory::Midi;
  }
  if (value == "unknown") {
    return InputDeviceCategory::Unknown;
  }
  return std::nullopt;
}

const char *judgementName(Judgement value) {
  switch (value) {
  case PGreat:
    return "pgreat";
  case Great:
    return "great";
  case Good:
    return "good";
  case Bad:
    return "bad";
  case Kpoor:
    return "kpoor";
  case Poor:
    return "poor";
  case None:
    return "none";
  case JudgementCount:
    break;
  }
  throw std::invalid_argument("Unknown judgement value.");
}

std::optional<Judgement> judgementFromName(std::string_view value) {
  if (value == "pgreat") {
    return PGreat;
  }
  if (value == "great") {
    return Great;
  }
  if (value == "good") {
    return Good;
  }
  if (value == "bad") {
    return Bad;
  }
  if (value == "kpoor") {
    return Kpoor;
  }
  if (value == "poor") {
    return Poor;
  }
  if (value == "none") {
    return None;
  }
  return std::nullopt;
}

const char *gaugeTypeName(GaugeType value) {
  switch (value) {
  case GaugeType::AssistedEasy:
    return "assisted-easy";
  case GaugeType::Easy:
    return "easy";
  case GaugeType::Normal:
    return "normal";
  case GaugeType::Hard:
    return "hard";
  case GaugeType::ExHard:
    return "ex-hard";
  }
  throw std::invalid_argument("Unknown gauge type value.");
}

std::optional<GaugeType> gaugeTypeFromName(std::string_view value) {
  if (value == "assisted-easy") {
    return GaugeType::AssistedEasy;
  }
  if (value == "easy") {
    return GaugeType::Easy;
  }
  if (value == "normal") {
    return GaugeType::Normal;
  }
  if (value == "hard") {
    return GaugeType::Hard;
  }
  if (value == "ex-hard") {
    return GaugeType::ExHard;
  }
  return std::nullopt;
}

const char *gaugeProfileName(GaugeProfile value) {
  switch (value) {
  case GaugeProfile::Standard:
    return "standard";
  case GaugeProfile::CourseDefault:
    return "course-default";
  case GaugeProfile::Course5Keys:
    return "course-5-keys";
  case GaugeProfile::Course7Keys:
    return "course-7-keys";
  case GaugeProfile::Course9Keys:
    return "course-9-keys";
  case GaugeProfile::Course24Keys:
    return "course-24-keys";
  case GaugeProfile::CourseLR2:
    return "course-lr2";
  }
  throw std::invalid_argument("Unknown gauge profile value.");
}

std::optional<GaugeProfile> gaugeProfileFromName(std::string_view value) {
  if (value == "standard") {
    return GaugeProfile::Standard;
  }
  if (value == "course-default") {
    return GaugeProfile::CourseDefault;
  }
  if (value == "course-5-keys") {
    return GaugeProfile::Course5Keys;
  }
  if (value == "course-7-keys") {
    return GaugeProfile::Course7Keys;
  }
  if (value == "course-9-keys") {
    return GaugeProfile::Course9Keys;
  }
  if (value == "course-24-keys") {
    return GaugeProfile::Course24Keys;
  }
  if (value == "course-lr2") {
    return GaugeProfile::CourseLR2;
  }
  return std::nullopt;
}

template <typename Value>
void writeOptional(Json &object, const char *key,
                   const std::optional<Value> &value) {
  if (value.has_value()) {
    object[key] = *value;
  } else {
    object[key] = nullptr;
  }
}

template <typename Value>
std::optional<Value> readOptional(const Json &object, const char *key) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    return std::nullopt;
  }
  return found->template get<Value>();
}

template <typename Value>
Value enumOrThrow(std::optional<Value> value, const char *message) {
  if (!value.has_value()) {
    throw std::runtime_error(message);
  }
  return *value;
}

Json rulesetToJson(const RulesetDescriptor &ruleset) {
  Json value = Json::object();
  value["version"] = ruleset.version;
  value["scoringModel"] = ruleset.scoringModel;
  value["judgementModel"] = ruleset.judgementModel;
  value["gaugeModel"] = ruleset.gaugeModel;
  return value;
}

RulesetDescriptor rulesetFromJson(const Json &value) {
  if (!value.is_object()) {
    throw std::runtime_error("Score provenance ruleset must be an object.");
  }

  const int version = value.value("version", 0);
  if (version > RulesetDescriptor::kCurrentVersion) {
    throw std::runtime_error("Cannot read future ruleset version " +
                             std::to_string(version) + ".");
  }
  if (version < 0) {
    throw std::runtime_error("Ruleset version cannot be negative.");
  }

  RulesetDescriptor result =
      version == 0 ? RulesetDescriptor::Legacy() : RulesetDescriptor::Current();
  result.version = version;
  result.scoringModel = value.value("scoringModel", result.scoringModel);
  result.judgementModel = value.value("judgementModel", result.judgementModel);
  result.gaugeModel = value.value("gaugeModel", result.gaugeModel);
  return result;
}

Json playerOptionToJson(const PlayerOptionProvenance &player) {
  Json value = Json::object();
  value["option"] = player.option;
  writeOptional(value, "seed", player.seed);
  return value;
}

PlayerOptionProvenance playerOptionFromJson(const Json &value) {
  if (!value.is_object()) {
    throw std::runtime_error("Player option provenance must be an object.");
  }
  PlayerOptionProvenance result;
  result.option = value.value("option", result.option);
  result.seed = readOptional<std::int64_t>(value, "seed");
  return result;
}

Json stageToJson(ScoreStageProvenance stage) {
  canonicalizeWindows(stage.effectiveJudgeWindows);

  Json value = Json::object();
  value["chartMd5"] = stage.chartMd5;
  value["chartSha256"] = stage.chartSha256;
  value["longNoteMode"] = stage.longNoteMode;
  writeOptional(value, "chartRandomSeed", stage.chartRandomSeed);
  writeOptional(value, "chartRandomPrng", stage.chartRandomPrng);
  value["chartRandomValues"] = stage.chartRandomValues;
  value["judgeRankSource"] = judgeRankSourceName(stage.judgeRankSource);
  writeOptional(value, "sourceJudgeRank", stage.sourceJudgeRank);

  Json windows = Json::array();
  for (const auto &window : stage.effectiveJudgeWindows) {
    Json serializedWindow = Json::object();
    serializedWindow["judgement"] = judgementName(window.judgement);
    serializedWindow["earlyMicros"] = window.earlyMicros;
    serializedWindow["lateMicros"] = window.lateMicros;
    windows.push_back(std::move(serializedWindow));
  }
  value["effectiveJudgeWindows"] = std::move(windows);
  return value;
}

ScoreStageProvenance stageFromJson(const Json &value) {
  if (!value.is_object()) {
    throw std::runtime_error("Score provenance stage must be an object.");
  }

  ScoreStageProvenance result;
  result.chartMd5 = value.value("chartMd5", result.chartMd5);
  result.chartSha256 = value.value("chartSha256", result.chartSha256);
  result.longNoteMode = value.value("longNoteMode", result.longNoteMode);
  result.chartRandomSeed =
      readOptional<std::uint64_t>(value, "chartRandomSeed");
  result.chartRandomPrng = readOptional<std::string>(value, "chartRandomPrng");
  result.chartRandomValues =
      value.value("chartRandomValues", result.chartRandomValues);
  result.judgeRankSource = enumOrThrow(
      judgeRankSourceFromName(value.value("judgeRankSource", "unknown")),
      "Unknown judge-rank source in score provenance.");
  result.sourceJudgeRank = readOptional<int>(value, "sourceJudgeRank");

  const auto windows = value.find("effectiveJudgeWindows");
  if (windows != value.end()) {
    if (!windows->is_array()) {
      throw std::runtime_error(
          "Score provenance effective judge windows must be an array.");
    }
    for (const auto &window : *windows) {
      if (!window.is_object()) {
        throw std::runtime_error(
            "Score provenance judge window must be an object.");
      }
      result.effectiveJudgeWindows.push_back({
          .judgement =
              enumOrThrow(judgementFromName(window.value("judgement", "")),
                          "Unknown judgement in score provenance."),
          .earlyMicros = window.value("earlyMicros", std::int64_t{0}),
          .lateMicros = window.value("lateMicros", std::int64_t{0}),
      });
    }
  }
  canonicalizeWindows(result.effectiveJudgeWindows);
  return result;
}

bool buildIsModified(const ScoreProvenanceBuildInput &input) {
  return input.ruleset != RulesetDescriptor::Current() || input.autoPlay ||
         input.practice || assist_options::isEnabled(input.assistOption) ||
         input.judgeRankSource != JudgeRankSource::Chart ||
         input.gaugeProfile != GaugeProfile::Standard;
}

} // namespace

RulesetDescriptor RulesetDescriptor::Current() { return {}; }

RulesetDescriptor RulesetDescriptor::Legacy() {
  return {
      .version = 0,
      .scoringModel = "legacy-unknown",
      .judgementModel = "legacy-unknown",
      .gaugeModel = "legacy-unknown",
  };
}

ScoreProvenance ScoreProvenance::Legacy() {
  ScoreProvenance result;
  result.ruleset = RulesetDescriptor::Legacy();
  result.eligibility = ScoreEligibility::LegacyUnverified;
  return result;
}

std::string serializeScoreProvenance(const ScoreProvenance &provenance) {
  ScoreProvenance canonical = provenance;
  canonicalizeDevices(canonical.inputDevices);

  Json root = Json::object();
  root["schemaVersion"] = canonical.schemaVersion;
  root["ruleset"] = rulesetToJson(canonical.ruleset);

  Json stages = Json::array();
  for (const auto &stage : canonical.stages) {
    stages.push_back(stageToJson(stage));
  }
  root["stages"] = std::move(stages);
  root["gaugeType"] = gaugeTypeName(canonical.gaugeType);
  root["gaugeProfile"] = gaugeProfileName(canonical.gaugeProfile);
  root["gaugeAutoShift"] = canonical.gaugeAutoShift;
  root["player1"] = playerOptionToJson(canonical.player1);
  root["player2"] = playerOptionToJson(canonical.player2);
  root["assistOption"] = canonical.assistOption;

  Json devices = Json::array();
  for (const auto device : canonical.inputDevices) {
    devices.push_back(inputDeviceName(device));
  }
  root["inputDevices"] = std::move(devices);
  root["autoPlay"] = canonical.autoPlay;
  root["practice"] = canonical.practice;
  root["eligibility"] = eligibilityName(canonical.eligibility);
  return root.dump();
}

std::optional<ScoreProvenance>
deserializeScoreProvenance(std::string_view serialized, std::string &error) {
  error.clear();
  try {
    const Json root = Json::parse(serialized.begin(), serialized.end());
    if (!root.is_object()) {
      throw std::runtime_error("Score provenance root must be an object.");
    }

    const int schemaVersion = root.value("schemaVersion", -1);
    if (schemaVersion > ScoreProvenance::kSchemaVersion) {
      throw std::runtime_error("Cannot read future score provenance schema " +
                               std::to_string(schemaVersion) + ".");
    }
    if (schemaVersion != ScoreProvenance::kSchemaVersion) {
      throw std::runtime_error("Unsupported score provenance schema version.");
    }

    ScoreProvenance result = ScoreProvenance::Legacy();
    result.schemaVersion = schemaVersion;
    if (const auto ruleset = root.find("ruleset"); ruleset != root.end()) {
      result.ruleset = rulesetFromJson(*ruleset);
    }

    if (const auto stages = root.find("stages"); stages != root.end()) {
      if (!stages->is_array()) {
        throw std::runtime_error("Score provenance stages must be an array.");
      }
      for (const auto &stage : *stages) {
        result.stages.push_back(stageFromJson(stage));
      }
    }

    result.gaugeType =
        enumOrThrow(gaugeTypeFromName(root.value("gaugeType", "normal")),
                    "Unknown gauge type in score provenance.");
    result.gaugeProfile = enumOrThrow(
        gaugeProfileFromName(root.value("gaugeProfile", "standard")),
        "Unknown gauge profile in score provenance.");
    result.gaugeAutoShift = root.value("gaugeAutoShift", result.gaugeAutoShift);
    if (const auto player = root.find("player1"); player != root.end()) {
      result.player1 = playerOptionFromJson(*player);
    }
    if (const auto player = root.find("player2"); player != root.end()) {
      result.player2 = playerOptionFromJson(*player);
    }
    result.assistOption = root.value("assistOption", result.assistOption);

    if (const auto devices = root.find("inputDevices"); devices != root.end()) {
      if (!devices->is_array()) {
        throw std::runtime_error(
            "Score provenance input devices must be an array.");
      }
      for (const auto &device : *devices) {
        result.inputDevices.push_back(
            enumOrThrow(inputDeviceFromName(device.get<std::string>()),
                        "Unknown input-device category in score provenance."));
      }
      canonicalizeDevices(result.inputDevices);
    }

    result.autoPlay = root.value("autoPlay", result.autoPlay);
    result.practice = root.value("practice", result.practice);
    result.eligibility = enumOrThrow(
        eligibilityFromName(root.value("eligibility", "legacy-unverified")),
        "Unknown eligibility in score provenance.");
    return result;
  } catch (const std::exception &exception) {
    error = exception.what();
    return std::nullopt;
  }
}

ScoreProvenance makeScoreProvenance(const ScoreProvenanceBuildInput &input) {
  ScoreStageProvenance stage;
  stage.chartMd5 = input.chartMeta.MD5;
  stage.chartSha256 = input.chartMeta.SHA256;
  stage.longNoteMode = input.longNoteMode;
  if (input.chartMeta.RandomSeed.has_value()) {
    stage.chartRandomSeed =
        static_cast<std::uint64_t>(*input.chartMeta.RandomSeed);
  }
  stage.chartRandomPrng = input.chartMeta.RandomPrng;
  stage.chartRandomValues = input.chartMeta.RandomValues;
  stage.judgeRankSource = input.judgeRankSource;
  stage.sourceJudgeRank = input.sourceJudgeRank;
  if (!stage.sourceJudgeRank.has_value() &&
      stage.judgeRankSource == JudgeRankSource::Chart) {
    stage.sourceJudgeRank = input.chartMeta.Rank;
  }
  for (const auto &[judgement, window] : input.effectiveJudgeWindows) {
    stage.effectiveJudgeWindows.push_back({
        .judgement = judgement,
        .earlyMicros = static_cast<std::int64_t>(window.first),
        .lateMicros = static_cast<std::int64_t>(window.second),
    });
  }
  canonicalizeWindows(stage.effectiveJudgeWindows);

  ScoreProvenance result;
  result.ruleset = input.ruleset;
  result.stages.push_back(std::move(stage));
  result.gaugeType = input.gaugeType;
  result.gaugeProfile = input.gaugeProfile;
  result.gaugeAutoShift = input.gaugeAutoShift;
  result.player1 = input.player1;
  result.player2 = input.player2;
  result.assistOption = assist_options::normalize(input.assistOption);
  result.inputDevices = input.inputDevices;
  canonicalizeDevices(result.inputDevices);
  result.autoPlay = input.autoPlay;
  result.practice = input.practice;
  if (input.ruleset.version <= 0) {
    result.eligibility = ScoreEligibility::LegacyUnverified;
  } else {
    result.eligibility = buildIsModified(input) ? ScoreEligibility::Modified
                                                : ScoreEligibility::Verified;
  }
  return result;
}

ScoreProvenance mergeCourseProvenance(std::span<const ScoreProvenance> stages) {
  if (stages.empty()) {
    return ScoreProvenance::Legacy();
  }

  ScoreProvenance result = stages.front();
  result.stages.clear();
  result.inputDevices.clear();
  result.autoPlay = false;
  result.practice = false;

  ScoreEligibility eligibility = ScoreEligibility::Verified;
  bool inconsistent = false;
  for (const auto &stage : stages) {
    result.stages.insert(result.stages.end(), stage.stages.begin(),
                         stage.stages.end());
    result.inputDevices.insert(result.inputDevices.end(),
                               stage.inputDevices.begin(),
                               stage.inputDevices.end());
    result.autoPlay = result.autoPlay || stage.autoPlay;
    result.practice = result.practice || stage.practice;
    eligibility = worseEligibility(eligibility, stage.eligibility);

    inconsistent = inconsistent ||
                   stage.schemaVersion != ScoreProvenance::kSchemaVersion ||
                   stage.ruleset != stages.front().ruleset ||
                   stage.gaugeType != stages.front().gaugeType ||
                   stage.gaugeProfile != stages.front().gaugeProfile ||
                   stage.gaugeAutoShift != stages.front().gaugeAutoShift ||
                   stage.player1 != stages.front().player1 ||
                   stage.player2 != stages.front().player2 ||
                   stage.assistOption != stages.front().assistOption;
  }

  canonicalizeDevices(result.inputDevices);
  if (inconsistent) {
    eligibility = worseEligibility(eligibility, ScoreEligibility::Modified);
  }
  if (result.autoPlay || result.practice) {
    eligibility = worseEligibility(eligibility, ScoreEligibility::Modified);
  }
  if (eligibility == ScoreEligibility::LegacyUnverified) {
    result.ruleset = RulesetDescriptor::Legacy();
  }
  result.schemaVersion = ScoreProvenance::kSchemaVersion;
  result.eligibility = eligibility;
  return result;
}
