#pragma once

#include "ReplayLimits.h"

#include "../AssistOptionUtils.h"
#include "../audio/PlaybackRate.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include "../scene/play/GameplayJudgeRules.h"
#include "../scene/play/GameplayRuleset.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

struct ReplayChartIdentity {
  std::string md5;
  std::string sha256;
  int keyMode = 0;

  bool operator==(const ReplayChartIdentity &) const = default;
};

struct ReplayPlayerOption {
  std::string option = "NORMAL";
  std::optional<std::int64_t> seed;

  bool operator==(const ReplayPlayerOption &) const = default;
};

enum class DoublePlayOption : std::uint8_t {
  Normal = 0,
  Flip = 1,
};

struct ReplaySetup {
  ReplayChartIdentity chart;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::optional<std::uint64_t> chartRandomSeed;
  std::optional<std::string> chartRandomPrng;
  std::vector<int> chartRandomValues;
  ReplayPlayerOption player1;
  ReplayPlayerOption player2;
  DoublePlayOption doublePlayOption = DoublePlayOption::Normal;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  RulesetDescriptor ruleset = RulesetDescriptor::Current();
  audio::PlaybackRate playback;
  gameplay::CandidateSelectionMode candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  int judgeWindowScalePercent = 100;
  float startingGaugePercent = 20.0F;
  int initialLaneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool clubMode = false;

  bool operator==(const ReplaySetup &) const = default;
};

enum class ReplaySetupSource : std::uint8_t {
  LocalCapture,
  AsoExtension,
  StockBeatoraja,
};

enum class ReplaySetupIssue : std::uint8_t {
  None,
  Source,
  Limits,
  ChartSha256,
  ChartMd5,
  KeyMode,
  LongNoteMode,
  RandomValues,
  RandomPrng,
  PlayerOneOption,
  PlayerTwoOption,
  PlayerOptions,
  DoublePlayOption,
  AssistOption,
  GaugeType,
  GaugeProfile,
  GaugeAutoShift,
  GaugeAutoShiftLowerBound,
  Ruleset,
  PlaybackRate,
  CandidateSelection,
  JudgeWindowScale,
  StartingGauge,
  InitialLaneCover,
};

struct ReplaySetupValidation {
  ReplaySetupIssue issue = ReplaySetupIssue::None;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return issue == ReplaySetupIssue::None;
  }
};

[[nodiscard]] ReplaySetupValidation validateReplaySetup(
    const ReplaySetup &setup, ReplaySetupSource source,
    const ReplayLimits &limits = kReplayLimits);

enum class ReplayChartMatch : std::uint8_t {
  Match,
  Sha256Mismatch,
  Md5Mismatch,
  KeyModeMismatch,
};

[[nodiscard]] ReplayChartMatch compareReplayChartIdentity(
    const ReplayChartIdentity &recorded,
    const ReplayChartIdentity &selected) noexcept;

} // namespace replay
