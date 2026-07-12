#pragma once

#include "AssistOptionUtils.h"
#include "audio/PlaybackRate.h"
#include "bms_parser.hpp"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class ScoreEligibility : int {
  Verified = 0,
  Modified = 1,
  LegacyUnverified = 2,
};

enum class JudgeRankSource : int {
  Chart = 0,
  CourseConstraint = 1,
  Override = 2,
  Unknown = 3,
};

enum class InputDeviceCategory : int {
  Keyboard = 0,
  GameController = 1,
  Joystick = 2,
  Touch = 3,
  Midi = 4,
  Unknown = 5,
  Gyroscope = 6,
};

struct RulesetDescriptor {
  static constexpr int kCurrentVersion = 1;

  int version = kCurrentVersion;
  std::string scoringModel = "asobmashow-v1";
  std::string judgementModel = "bms-rank-v1";
  std::string gaugeModel = "asobmashow-gauge-v1";

  bool operator==(const RulesetDescriptor &) const = default;

  static RulesetDescriptor Current();
  static RulesetDescriptor Legacy();
};

struct JudgeWindowProvenance {
  Judgement judgement = None;
  std::int64_t earlyMicros = 0;
  std::int64_t lateMicros = 0;

  bool operator==(const JudgeWindowProvenance &) const = default;
};

struct PlayerOptionProvenance {
  std::string option = "NORMAL";
  std::optional<std::int64_t> seed;

  bool operator==(const PlayerOptionProvenance &) const = default;
};

struct ScoreStageProvenance {
  std::string chartMd5;
  std::string chartSha256;
  int longNoteMode = 0;
  std::optional<std::uint64_t> chartRandomSeed;
  std::optional<std::string> chartRandomPrng;
  std::vector<int> chartRandomValues;
  JudgeRankSource judgeRankSource = JudgeRankSource::Unknown;
  std::optional<int> sourceJudgeRank;
  std::vector<JudgeWindowProvenance> effectiveJudgeWindows;

  bool operator==(const ScoreStageProvenance &) const = default;
};

struct ScoreProvenance {
  static constexpr int kSchemaVersion = 3;

  int schemaVersion = kSchemaVersion;
  RulesetDescriptor ruleset;
  std::vector<ScoreStageProvenance> stages;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  PlayerOptionProvenance player1;
  PlayerOptionProvenance player2;
  std::string assistOption = assist_options::kOff;
  std::vector<InputDeviceCategory> inputDevices;
  bool autoPlay = false;
  bool practice = false;
  bool clubMode = false;
  audio::PlaybackRate playback;
  int judgeWindowScalePercent = 100;
  std::optional<int> startingGaugePercent;
  ScoreEligibility eligibility = ScoreEligibility::LegacyUnverified;

  bool operator==(const ScoreProvenance &) const = default;

  static ScoreProvenance Legacy();
};

namespace score_provenance {

[[nodiscard]] bool stageMatchesChart(const ScoreStageProvenance &stage,
                                     const bms_parser::ChartMeta &chartMeta);

[[nodiscard]] const ScoreStageProvenance *
uniqueStageForChart(const ScoreProvenance &provenance,
                    const bms_parser::ChartMeta &chartMeta);

} // namespace score_provenance

struct ScoreProvenanceBuildInput {
  bms_parser::ChartMeta chartMeta;
  int longNoteMode = 0;
  JudgeRankSource judgeRankSource = JudgeRankSource::Chart;
  std::optional<int> sourceJudgeRank;
  std::map<Judgement, std::pair<long long, long long>> effectiveJudgeWindows;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  PlayerOptionProvenance player1;
  PlayerOptionProvenance player2;
  std::string assistOption = assist_options::kOff;
  std::vector<InputDeviceCategory> inputDevices;
  bool autoPlay = false;
  bool practice = false;
  bool clubMode = false;
  audio::PlaybackRate playback;
  int judgeWindowScalePercent = 100;
  std::optional<int> startingGaugePercent;
  RulesetDescriptor ruleset = RulesetDescriptor::Current();
};

[[nodiscard]] std::string
serializeScoreProvenance(const ScoreProvenance &provenance);

[[nodiscard]] std::optional<std::string>
serializeValidatedScoreProvenance(const ScoreProvenance &provenance,
                                  std::string &error);

[[nodiscard]] std::optional<ScoreProvenance>
deserializeScoreProvenance(std::string_view serialized, std::string &error);

[[nodiscard]] ScoreProvenance
makeScoreProvenance(const ScoreProvenanceBuildInput &input);

[[nodiscard]] ScoreProvenance
mergeCourseProvenance(std::span<const ScoreProvenance> stages);
