#include "ScoreProvenance.h"

#include "BmsMetadataText.h"
#include "CanonicalDigest.h"
#include "scene/play/GameplayAttemptSetup.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::ordered_json;

std::optional<std::string> normalizedHexHash(const std::string &value,
                                             std::size_t expectedSize) {
  std::string normalized = asobmshow::bms_metadata::normalizedHash(value);
  if (!canonical_digest::isCanonicalLowerHex(normalized, expectedSize)) {
    return std::nullopt;
  }
  return normalized;
}

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
                     if (lhs.context != rhs.context) {
                       return enumValue(lhs.context) < enumValue(rhs.context);
                     }
                     return enumValue(lhs.judgement) <
                            enumValue(rhs.judgement);
                   });
}

constexpr std::array<gameplay::JudgeWindowContext, 4> kJudgeContexts{
    gameplay::JudgeWindowContext::Normal,
    gameplay::JudgeWindowContext::Scratch,
    gameplay::JudgeWindowContext::LongNoteTail,
    gameplay::JudgeWindowContext::LongScratchTail,
};
constexpr std::array<Judgement, 5> kPolicyJudgements{
    PGreat, Great, Good, Bad, Kpoor};
constexpr std::int64_t kMaximumJudgeWindowMagnitude = 2'000'000;

const char *judgeContextName(gameplay::JudgeWindowContext value) {
  switch (value) {
  case gameplay::JudgeWindowContext::Normal:
    return "normal";
  case gameplay::JudgeWindowContext::Scratch:
    return "scratch";
  case gameplay::JudgeWindowContext::LongNoteTail:
    return "long-note-tail";
  case gameplay::JudgeWindowContext::LongScratchTail:
    return "long-scratch-tail";
  }
  throw std::invalid_argument("Unknown judge-window context value.");
}

std::optional<gameplay::JudgeWindowContext>
judgeContextFromName(std::string_view value) {
  if (value == "normal") {
    return gameplay::JudgeWindowContext::Normal;
  }
  if (value == "scratch") {
    return gameplay::JudgeWindowContext::Scratch;
  }
  if (value == "long-note-tail") {
    return gameplay::JudgeWindowContext::LongNoteTail;
  }
  if (value == "long-scratch-tail") {
    return gameplay::JudgeWindowContext::LongScratchTail;
  }
  return std::nullopt;
}

const char *candidateSelectionName(gameplay::CandidateSelectionMode value) {
  switch (value) {
  case gameplay::CandidateSelectionMode::LR2:
    return "lr2";
  case gameplay::CandidateSelectionMode::Lowest:
    return "lowest";
  case gameplay::CandidateSelectionMode::Combo:
    return "combo";
  case gameplay::CandidateSelectionMode::Duration:
    return "duration";
  case gameplay::CandidateSelectionMode::Score:
    return "score";
  }
  throw std::invalid_argument("Unknown candidate-selection value.");
}

std::optional<gameplay::CandidateSelectionMode>
candidateSelectionFromName(std::string_view value) {
  if (value == "lr2") {
    return gameplay::CandidateSelectionMode::LR2;
  }
  if (value == "lowest") {
    return gameplay::CandidateSelectionMode::Lowest;
  }
  if (value == "combo") {
    return gameplay::CandidateSelectionMode::Combo;
  }
  if (value == "duration") {
    return gameplay::CandidateSelectionMode::Duration;
  }
  if (value == "score") {
    return gameplay::CandidateSelectionMode::Score;
  }
  return std::nullopt;
}

ScoreEligibility mergeEligibility(ScoreEligibility lhs,
                                  ScoreEligibility rhs) {
  if (lhs == ScoreEligibility::Modified ||
      rhs == ScoreEligibility::Modified) {
    return ScoreEligibility::Modified;
  }
  if (lhs == ScoreEligibility::LegacyUnverified ||
      rhs == ScoreEligibility::LegacyUnverified) {
    return ScoreEligibility::LegacyUnverified;
  }
  return ScoreEligibility::Verified;
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
  case InputDeviceCategory::Gyroscope:
    return "gyroscope";
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
  if (value == "gyroscope") {
    return InputDeviceCategory::Gyroscope;
  }
  return std::nullopt;
}

const char *playbackModeName(audio::PlaybackMode value) {
  switch (value) {
  case audio::PlaybackMode::PitchShift:
    return "pitch-shift";
  case audio::PlaybackMode::TimeStretch:
    return "time-stretch";
  }
  throw std::invalid_argument("Unknown playback mode value.");
}

std::optional<audio::PlaybackMode>
playbackModeFromName(std::string_view value) {
  if (value == "pitch-shift") {
    return audio::PlaybackMode::PitchShift;
  }
  if (value == "time-stretch") {
    return audio::PlaybackMode::TimeStretch;
  }
  return std::nullopt;
}

Json playbackToJson(const audio::PlaybackRate &playback) {
  Json value = Json::object();
  value["percent"] = playback.percent;
  value["mode"] = playbackModeName(playback.mode);
  return value;
}

audio::PlaybackRate playbackFromJson(const Json &value) {
  if (!value.is_object()) {
    throw std::runtime_error("Score provenance playback must be an object.");
  }
  audio::PlaybackRate result;
  result.percent = value.value("percent", result.percent);
  const auto mode = playbackModeFromName(value.value("mode", "pitch-shift"));
  if (!mode.has_value()) {
    throw std::runtime_error("Unknown playback mode in score provenance.");
  }
  result.mode = *mode;
  if (!result.valid()) {
    throw std::runtime_error(
        "Score provenance playback percentage is out of range.");
  }
  return result;
}

void validatePracticePercentages(const ScoreProvenance &value) {
  if (!value.playback.valid()) {
    throw std::runtime_error("Score provenance playback is invalid.");
  }
  if (!gameplay::validJudgeWindowScalePercent(
          value.judgeWindowScalePercent)) {
    throw std::runtime_error(
        "Score provenance judge window scale percentage is out of range.");
  }
  if (value.startingGaugePercent.has_value() &&
      !gameplay::validStartingGaugePercent(*value.startingGaugePercent)) {
    throw std::runtime_error(
        "Score provenance starting gauge percentage is out of range.");
  }
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
  case GaugeType::Hazard:
    return "hazard";
  case GaugeType::Grade:
    return "grade";
  case GaugeType::ExGrade:
    return "ex-grade";
  case GaugeType::ExHardGrade:
    return "exhard-grade";
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
  if (value == "hazard") {
    return GaugeType::Hazard;
  }
  if (value == "grade") {
    return GaugeType::Grade;
  }
  if (value == "ex-grade") {
    return GaugeType::ExGrade;
  }
  if (value == "exhard-grade") {
    return GaugeType::ExHardGrade;
  }
  return std::nullopt;
}

GaugeAutoShiftMode gaugeAutoShiftFromJson(const Json &value) {
  if (value.is_boolean()) {
    return value.get<bool>() ? GaugeAutoShiftMode::SelectToUnder
                             : GaugeAutoShiftMode::None;
  }
  if (value.is_number_integer()) {
    return gaugeAutoShiftModeFromValue(value.get<int>());
  }
  throw std::runtime_error("Gauge auto shift mode must be an integer.");
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
  case GaugeProfile::Standard5Keys:
    return "standard-5-keys";
  case GaugeProfile::Standard9Keys:
    return "standard-9-keys";
  case GaugeProfile::Standard24Keys:
    return "standard-24-keys";
  case GaugeProfile::StandardLr2:
    return "standard-lr2";
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
  if (value == "standard-5-keys") {
    return GaugeProfile::Standard5Keys;
  }
  if (value == "standard-9-keys") {
    return GaugeProfile::Standard9Keys;
  }
  if (value == "standard-24-keys") {
    return GaugeProfile::Standard24Keys;
  }
  if (value == "standard-lr2") {
    return GaugeProfile::StandardLr2;
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
  value["id"] = ruleset.id;
  value["version"] = ruleset.version;
  value["scoringModel"] = ruleset.scoringModel;
  value["judgementModel"] = ruleset.judgementModel;
  value["gaugeModel"] = ruleset.gaugeModel;
  return value;
}

RulesetDescriptor rulesetFromJson(const Json &value, int schemaVersion) {
  if (!value.is_object()) {
    throw std::runtime_error("Score provenance ruleset must be an object.");
  }

  const int version = value.value("version", 0);
  if (version < 0) {
    throw std::runtime_error("Ruleset version cannot be negative.");
  }

  const auto id = value.find("id");
  if (id != value.end() && !id->is_string()) {
    throw std::runtime_error("Score provenance ruleset id must be a string.");
  }
  const bool hasId = id != value.end();
  const std::string idValue = hasId ? id->get<std::string>() : std::string();
  if (schemaVersion >= 4 && (!hasId || idValue.empty())) {
    throw std::runtime_error(
        "Score provenance ruleset id is required by this schema.");
  }
  RulesetDescriptor result;
  if (version == 0 && !hasId) {
    result = RulesetDescriptor::Legacy();
  } else if (hasId) {
    const auto known = gameplayRulesetFromId(idValue);
    result = known.has_value()
                 ? RulesetDescriptor::For(*known)
                 : RulesetDescriptor{.id = idValue,
                                     .version = version,
                                     .scoringModel = {},
                                     .judgementModel = {},
                                     .gaugeModel = {}};
  } else {
    result = {.id = {},
              .version = version,
              .scoringModel = {},
              .judgementModel = {},
              .gaugeModel = {}};
  }
  result.version = version;
  result.scoringModel = value.value("scoringModel", result.scoringModel);
  result.judgementModel = value.value("judgementModel", result.judgementModel);
  result.gaugeModel = value.value("gaugeModel", result.gaugeModel);
  if (!hasId && version == 2 &&
      result.scoringModel == "asobmashow-v1" &&
      result.judgementModel == "bms-rank-v1" &&
      result.gaugeModel == "beatoraja-profile-gauge-v2") {
    result = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  }
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

void validateStageWindows(const ScoreStageProvenance &stage,
                          bool requireCompleteContexts) {
  std::array<bool, kJudgeContexts.size() * kPolicyJudgements.size()> found{};
  for (const auto &window : stage.effectiveJudgeWindows) {
    const auto context = std::ranges::find(kJudgeContexts, window.context);
    const auto judgement =
        std::ranges::find(kPolicyJudgements, window.judgement);
    if (context == kJudgeContexts.end() ||
        judgement == kPolicyJudgements.end()) {
      throw std::runtime_error(
          "Score provenance contains an unknown policy window.");
    }
    if (window.earlyMicros > 0 || window.lateMicros < 0 ||
        window.earlyMicros > window.lateMicros ||
        window.earlyMicros < -kMaximumJudgeWindowMagnitude ||
        window.lateMicros > kMaximumJudgeWindowMagnitude) {
      throw std::runtime_error(
          "Score provenance judge window is outside safe bounds.");
    }
    const std::size_t contextIndex = static_cast<std::size_t>(
        std::distance(kJudgeContexts.begin(), context));
    const std::size_t judgementIndex = static_cast<std::size_t>(
        std::distance(kPolicyJudgements.begin(), judgement));
    const std::size_t index =
        contextIndex * kPolicyJudgements.size() + judgementIndex;
    if (found[index]) {
      throw std::runtime_error(
          "Score provenance contains a duplicate policy window.");
    }
    found[index] = true;
  }
  if (requireCompleteContexts &&
      !std::ranges::all_of(found, [](bool value) { return value; })) {
    throw std::runtime_error(
        "Score provenance policy windows are incomplete.");
  }
}

void validateStageProof(const ScoreStageProvenance &stage) {
  if (stage.longNoteMode < 0 || stage.longNoteMode > 3) {
    throw std::runtime_error(
        "Score provenance long-note mode is not recognized.");
  }
  if (stage.totalNotes <= 0) {
    throw std::runtime_error(
        "Score provenance total note count must be positive.");
  }
  if (stage.authoredGaugeTotal.has_value() &&
      !std::isfinite(*stage.authoredGaugeTotal)) {
    throw std::runtime_error(
        "Score provenance authored gauge TOTAL is not finite.");
  }
  if (!std::isfinite(stage.effectiveGaugeTotal) ||
      stage.effectiveGaugeTotal <= 0.0) {
    throw std::runtime_error(
        "Score provenance effective gauge TOTAL must be finite and positive.");
  }
  (void)candidateSelectionName(stage.candidateSelection);
  validateStageWindows(stage, true);
}

void migrateLegacyBeatorajaWindows(ScoreStageProvenance &stage) {
  if (stage.effectiveJudgeWindows.size() != kPolicyJudgements.size() ||
      std::ranges::any_of(stage.effectiveJudgeWindows, [](const auto &window) {
        return window.context != gameplay::JudgeWindowContext::Normal;
      })) {
    return;
  }
  const auto normal = stage.effectiveJudgeWindows;
  for (const auto context : kJudgeContexts) {
    if (context == gameplay::JudgeWindowContext::Normal) {
      continue;
    }
    for (auto window : normal) {
      window.context = context;
      stage.effectiveJudgeWindows.push_back(window);
    }
  }
}

Json stageToJson(ScoreStageProvenance stage, int wireSchemaVersion) {
  validateStageProof(stage);
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
  value["totalNotes"] = stage.totalNotes;
  if (wireSchemaVersion >= ScoreProvenance::kPlayDurationSchemaVersion) {
    value["playDurationSeconds"] = stage.playDurationSeconds;
  }
  writeOptional(value, "authoredGaugeTotal", stage.authoredGaugeTotal);
  value["effectiveGaugeTotal"] = stage.effectiveGaugeTotal;
  value["candidateSelection"] =
      candidateSelectionName(stage.candidateSelection);

  Json windows = Json::array();
  for (const auto &window : stage.effectiveJudgeWindows) {
    Json serializedWindow = Json::object();
    serializedWindow["context"] = judgeContextName(window.context);
    serializedWindow["judgement"] = judgementName(window.judgement);
    serializedWindow["earlyMicros"] = window.earlyMicros;
    serializedWindow["lateMicros"] = window.lateMicros;
    windows.push_back(std::move(serializedWindow));
  }
  value["effectiveJudgeWindows"] = std::move(windows);
  return value;
}

ScoreStageProvenance stageFromJson(const Json &value, int schemaVersion,
                                   const RulesetDescriptor &ruleset) {
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
  result.totalNotes = value.value("totalNotes", result.totalNotes);
  result.playDurationSeconds =
      value.value("playDurationSeconds", result.playDurationSeconds);
  result.authoredGaugeTotal =
      readOptional<double>(value, "authoredGaugeTotal");
  const auto effectiveTotal = value.find("effectiveGaugeTotal");
  if (effectiveTotal != value.end()) {
    if (!effectiveTotal->is_number()) {
      throw std::runtime_error(
          "Score provenance effective gauge TOTAL must be numeric.");
    }
    result.effectiveGaugeTotal = effectiveTotal->get<double>();
  }
  result.candidateSelection = enumOrThrow(
      candidateSelectionFromName(
          value.value("candidateSelection", "lowest")),
      "Unknown candidate-selection mode in score provenance.");

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
          .context = enumOrThrow(
              judgeContextFromName(window.value("context", "normal")),
              "Unknown judge-window context in score provenance."),
          .judgement =
              enumOrThrow(judgementFromName(window.value("judgement", "")),
                          "Unknown judgement in score provenance."),
          .earlyMicros = window.value("earlyMicros", std::int64_t{0}),
          .lateMicros = window.value("lateMicros", std::int64_t{0}),
      });
    }
  }
  if (schemaVersion < 4 &&
      ruleset == RulesetDescriptor::For(GameplayRuleset::Beatoraja)) {
    migrateLegacyBeatorajaWindows(result);
  }
  canonicalizeWindows(result.effectiveJudgeWindows);
  if (schemaVersion >= 4) {
    validateStageProof(result);
  } else {
    validateStageWindows(result, false);
  }
  return result;
}

} // namespace

ScoreEligibility
scoreEligibilityForProvenance(const ScoreProvenance &provenance) {
  if (provenance.ruleset.version <= 0) {
    return ScoreEligibility::LegacyUnverified;
  }
  const bool unknownJudgeSource =
      std::ranges::any_of(provenance.stages, [](const auto &stage) {
        return stage.judgeRankSource == JudgeRankSource::Unknown;
      });
  const bool modified =
      !isSupportedRulesetDescriptor(provenance.ruleset) ||
      provenance.autoPlay || provenance.practice ||
      assist_options::isEnabled(provenance.assistOption) ||
      unknownJudgeSource || !provenance.playback.neutral() ||
      provenance.judgeWindowScalePercent != 100 ||
      provenance.startingGaugePercent.has_value();
  return modified ? ScoreEligibility::Modified : ScoreEligibility::Verified;
}

namespace score_provenance {

bool stageMatchesChart(const ScoreStageProvenance &stage,
                       const bms_parser::ChartMeta &chartMeta) {
  const auto stageSha = normalizedHexHash(stage.chartSha256, 64);
  const auto chartSha = normalizedHexHash(chartMeta.SHA256, 64);
  if (stageSha.has_value() && chartSha.has_value()) {
    return stageSha == chartSha;
  }
  const auto stageMd5 = normalizedHexHash(stage.chartMd5, 32);
  const auto chartMd5 = normalizedHexHash(chartMeta.MD5, 32);
  return stageMd5.has_value() && chartMd5.has_value() && stageMd5 == chartMd5;
}

const ScoreStageProvenance *
uniqueStageForChart(const ScoreProvenance &provenance,
                    const bms_parser::ChartMeta &chartMeta) {
  const ScoreStageProvenance *matching = nullptr;
  for (const auto &stage : provenance.stages) {
    if (!stageMatchesChart(stage, chartMeta)) {
      continue;
    }
    if (matching != nullptr) {
      return nullptr;
    }
    matching = &stage;
  }
  return matching;
}

std::optional<SavedChartRandomParseSetup>
savedChartRandomParseSetup(const ScoreProvenance &provenance,
                           const bms_parser::ChartMeta &chartMeta,
                           std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    const auto *stage = uniqueStageForChart(provenance, chartMeta);
    if (stage == nullptr) {
      diagnostic = "Saved result has no unique chart random branch.";
      return std::nullopt;
    }
    if (stage->chartRandomSeed &&
        *stage->chartRandomSeed >
            std::numeric_limits<unsigned int>::max()) {
      diagnostic = "Saved chart random seed is unsupported.";
      return std::nullopt;
    }
    if (stage->chartRandomPrng &&
        *stage->chartRandomPrng != bms_parser::Parser::RandomPrngId) {
      diagnostic = "Saved chart random PRNG is unsupported.";
      return std::nullopt;
    }
    return SavedChartRandomParseSetup{
        .randomSeed =
            stage->chartRandomSeed
                ? std::optional<unsigned int>(
                      static_cast<unsigned int>(*stage->chartRandomSeed))
                : std::nullopt,
        .randomPrng = stage->chartRandomPrng,
        .randomValues =
            stage->chartRandomValues.empty()
                ? std::nullopt
                : std::optional(stage->chartRandomValues),
    };
  } catch (...) {
    diagnostic = "Saved chart random branch is invalid.";
    return std::nullopt;
  }
}

} // namespace score_provenance

ScoreProvenance ScoreProvenance::Legacy() {
  ScoreProvenance result;
  result.ruleset = RulesetDescriptor::Legacy();
  result.eligibility = ScoreEligibility::LegacyUnverified;
  return result;
}

std::string serializeScoreProvenance(const ScoreProvenance &provenance) {
  ScoreProvenance canonical = provenance;
  canonicalizeDevices(canonical.inputDevices);
  if (canonical.fingerprintSchemaVersion > 0) {
    canonical.schemaVersion = canonical.fingerprintSchemaVersion;
  }
  // Schema v6 adds playDurationSeconds solely to a stage proof. Preserve the
  // established v5 wire payload for an unverified no-stage provenance so
  // durable IR snapshots and their fingerprints remain byte-for-byte stable.
  if (canonical.stages.empty() && canonical.ruleset.version == 0) {
    canonical.schemaVersion = ScoreProvenance::kDoublePlayFlipSchemaVersion;
  }

  Json root = Json::object();
  root["schemaVersion"] = canonical.schemaVersion;
  root["ruleset"] = rulesetToJson(canonical.ruleset);

  Json stages = Json::array();
  for (const auto &stage : canonical.stages) {
    stages.push_back(stageToJson(stage, canonical.schemaVersion));
  }
  root["stages"] = std::move(stages);
  root["gaugeType"] = gaugeTypeName(canonical.gaugeType);
  root["gaugeProfile"] = gaugeProfileName(canonical.gaugeProfile);
  root["gaugeAutoShift"] =
      gaugeAutoShiftModeValue(canonical.gaugeAutoShift);
  root["gaugeAutoShiftLowerBound"] =
      gaugeTypeName(canonical.gaugeAutoShiftLowerBound);
  root["player1"] = playerOptionToJson(canonical.player1);
  root["player2"] = playerOptionToJson(canonical.player2);
  root["doublePlayFlip"] = canonical.doublePlayFlip;
  root["assistOption"] = canonical.assistOption;

  Json devices = Json::array();
  for (const auto device : canonical.inputDevices) {
    devices.push_back(inputDeviceName(device));
  }
  root["inputDevices"] = std::move(devices);
  root["autoPlay"] = canonical.autoPlay;
  root["practice"] = canonical.practice;
  root["clubMode"] = canonical.clubMode;
  root["playback"] = playbackToJson(canonical.playback);
  root["judgeWindowScalePercent"] = canonical.judgeWindowScalePercent;
  writeOptional(root, "startingGaugePercent", canonical.startingGaugePercent);
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
    if (schemaVersion < 1) {
      throw std::runtime_error("Unsupported score provenance schema version.");
    }

    ScoreProvenance result = ScoreProvenance::Legacy();
    result.schemaVersion = ScoreProvenance::kSchemaVersion;
    result.fingerprintSchemaVersion =
        schemaVersion < ScoreProvenance::kPlayDurationSchemaVersion
            ? schemaVersion
            : 0;
    if (const auto ruleset = root.find("ruleset"); ruleset != root.end()) {
      result.ruleset = rulesetFromJson(*ruleset, schemaVersion);
    }

    if (const auto stages = root.find("stages"); stages != root.end()) {
      if (!stages->is_array()) {
        throw std::runtime_error("Score provenance stages must be an array.");
      }
      for (const auto &stage : *stages) {
        result.stages.push_back(
            stageFromJson(stage, schemaVersion, result.ruleset));
      }
    }

    result.gaugeType =
        enumOrThrow(gaugeTypeFromName(root.value("gaugeType", "normal")),
                    "Unknown gauge type in score provenance.");
    result.gaugeProfile = enumOrThrow(
        gaugeProfileFromName(root.value("gaugeProfile", "standard")),
        "Unknown gauge profile in score provenance.");
    if (const auto mode = root.find("gaugeAutoShift"); mode != root.end()) {
      result.gaugeAutoShift = gaugeAutoShiftFromJson(*mode);
    }
    if (const auto lower = root.find("gaugeAutoShiftLowerBound");
        lower != root.end()) {
      result.gaugeAutoShiftLowerBound = enumOrThrow(
          gaugeTypeFromName(lower->get<std::string>()),
          "Unknown gauge auto shift lower bound in score provenance.");
    }
    if (const auto player = root.find("player1"); player != root.end()) {
      result.player1 = playerOptionFromJson(*player);
    }
    if (const auto player = root.find("player2"); player != root.end()) {
      result.player2 = playerOptionFromJson(*player);
    }
    if (schemaVersion >= ScoreProvenance::kDoublePlayFlipSchemaVersion) {
      const auto flip = root.find("doublePlayFlip");
      if (flip == root.end() || !flip->is_boolean()) {
        throw std::runtime_error(
            "Score provenance double-play orientation is missing or malformed.");
      }
      result.doublePlayFlip = flip->get<bool>();
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
    result.clubMode = root.value("clubMode", result.clubMode);
    if (schemaVersion >= 3) {
      if (const auto playback = root.find("playback"); playback != root.end()) {
        result.playback = playbackFromJson(*playback);
      }
      result.judgeWindowScalePercent =
          root.value("judgeWindowScalePercent", result.judgeWindowScalePercent);
      result.startingGaugePercent =
          readOptional<int>(root, "startingGaugePercent");
    }
    validatePracticePercentages(result);
    result.eligibility = enumOrThrow(
        eligibilityFromName(root.value("eligibility", "legacy-unverified")),
        "Unknown eligibility in score provenance.");
    return result;
  } catch (const std::exception &exception) {
    error = exception.what();
    return std::nullopt;
  }
}

std::optional<std::string>
serializeValidatedScoreProvenance(const ScoreProvenance &provenance,
                                  std::string &error) {
  error.clear();
  try {
    const std::string serialized = serializeScoreProvenance(provenance);
    auto decoded = deserializeScoreProvenance(serialized, error);
    if (!decoded.has_value()) {
      return std::nullopt;
    }

    const std::string canonical = serializeScoreProvenance(*decoded);
    if (canonical != serialized) {
      error = "Score provenance did not produce canonical JSON.";
      return std::nullopt;
    }
    return canonical;
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
  stage.totalNotes =
      input.totalNotes > 0 ? input.totalNotes : input.chartMeta.TotalNotes;
  stage.playDurationSeconds =
      std::max<std::int64_t>(0, input.chartMeta.PlayLength) / 1'000'000;
  stage.authoredGaugeTotal =
      input.authoredGaugeTotal.has_value()
          ? input.authoredGaugeTotal
          : (input.chartMeta.HasTotal
                 ? std::optional<double>(input.chartMeta.Total)
                 : std::nullopt);
  stage.effectiveGaugeTotal = input.effectiveGaugeTotal;
  stage.candidateSelection = input.candidateSelection;

  const bool hasContextWindows = std::ranges::any_of(
      input.effectiveJudgeContexts, [](const gameplay::JudgeWindowSet &set) {
        return std::ranges::any_of(set.windows, [](const auto &window) {
          return window.judgement != None;
        });
      });
  if (hasContextWindows) {
    for (const auto context : kJudgeContexts) {
      const auto &set = input.effectiveJudgeContexts[enumValue(context)];
      for (const auto &window : set.windows) {
        stage.effectiveJudgeWindows.push_back({
            .context = context,
            .judgement = window.judgement,
            .earlyMicros = window.earlyMicros,
            .lateMicros = window.lateMicros,
        });
      }
    }
  } else {
    for (const auto context : kJudgeContexts) {
      for (const auto &[judgement, window] : input.effectiveJudgeWindows) {
        stage.effectiveJudgeWindows.push_back({
            .context = context,
            .judgement = judgement,
            .earlyMicros = static_cast<std::int64_t>(window.first),
            .lateMicros = static_cast<std::int64_t>(window.second),
        });
      }
    }
  }
  canonicalizeWindows(stage.effectiveJudgeWindows);

  ScoreProvenance result;
  result.ruleset = input.ruleset;
  result.stages.push_back(std::move(stage));
  result.gaugeType = input.gaugeType;
  result.gaugeProfile = input.gaugeProfile;
  result.gaugeAutoShift = input.gaugeAutoShift;
  result.gaugeAutoShiftLowerBound = input.gaugeAutoShiftLowerBound;
  result.player1 = input.player1;
  result.player2 = input.player2;
  result.doublePlayFlip = input.doublePlayFlip;
  result.assistOption = assist_options::effectiveForChart(
      input.assistOption, input.chartMeta.MinBpm, input.chartMeta.MaxBpm);
  result.inputDevices = input.inputDevices;
  canonicalizeDevices(result.inputDevices);
  result.autoPlay = input.autoPlay;
  result.practice = input.practice;
  result.clubMode = input.clubMode;
  result.playback = input.playback;
  result.judgeWindowScalePercent = input.judgeWindowScalePercent;
  result.startingGaugePercent = input.startingGaugePercent;
  result.eligibility = scoreEligibilityForProvenance(result);
  if (!input.policyCanonical &&
      result.eligibility == ScoreEligibility::Verified) {
    result.eligibility = ScoreEligibility::Modified;
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
  result.clubMode = false;

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
    result.clubMode = result.clubMode || stage.clubMode;
    eligibility = mergeEligibility(eligibility, stage.eligibility);

    inconsistent =
        inconsistent ||
        stage.schemaVersion != ScoreProvenance::kSchemaVersion ||
        stage.ruleset != stages.front().ruleset ||
        stage.gaugeType != stages.front().gaugeType ||
        stage.gaugeProfile != stages.front().gaugeProfile ||
        stage.gaugeAutoShift != stages.front().gaugeAutoShift ||
        stage.gaugeAutoShiftLowerBound !=
            stages.front().gaugeAutoShiftLowerBound ||
        stage.player1.option != stages.front().player1.option ||
        stage.player2.option != stages.front().player2.option ||
        stage.doublePlayFlip != stages.front().doublePlayFlip ||
        stage.assistOption != stages.front().assistOption ||
        stage.playback != stages.front().playback ||
        stage.judgeWindowScalePercent !=
            stages.front().judgeWindowScalePercent ||
        stage.startingGaugePercent != stages.front().startingGaugePercent;
  }

  canonicalizeDevices(result.inputDevices);
  if (inconsistent) {
    eligibility = mergeEligibility(eligibility, ScoreEligibility::Modified);
  }
  if (result.autoPlay || result.practice) {
    eligibility = mergeEligibility(eligibility, ScoreEligibility::Modified);
  }
  if (eligibility == ScoreEligibility::LegacyUnverified) {
    result.ruleset = RulesetDescriptor::Legacy();
  }
  result.schemaVersion = ScoreProvenance::kSchemaVersion;
  result.fingerprintSchemaVersion = 0;
  result.eligibility = eligibility;
  return result;
}
