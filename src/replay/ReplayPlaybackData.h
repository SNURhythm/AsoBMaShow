#pragma once

#include "../AssistOptionUtils.h"
#include "../audio/PlaybackRate.h"
#include "../scene/play/GameplayJudgeRules.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include "../scene/play/GameplayScoreState.h"
#include "../scene/play/Judgement.h"
#include "DoublePlayOption.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

inline constexpr std::int64_t kMinimumReplaySongTimeMicros = -30'000'000;

[[nodiscard]] inline bool
hasUndefinedLongNotesForReplay(int authoredLongNoteMode, int totalLongNotes,
                               int totalBackSpinNotes) noexcept {
  return authoredLongNoteMode == 0 &&
         (totalLongNotes > 0 || totalBackSpinNotes > 0);
}

enum class LogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
};

struct LogicalControl {
  LogicalControlKind kind = LogicalControlKind::Lane;
  int player = 1;
  int lane = -1;

  bool operator==(const LogicalControl &) const = default;
};

struct InputTransition {
  std::int64_t songTimeMicros = 0;
  LogicalControl control;
  bool pressed = false;
  bool replayOnly = false;

  bool operator==(const InputTransition &) const = default;
};

struct ChartPlaybackSetup {
  std::string chartMd5;
  std::string chartSha256;
  int keyMode = 0;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<std::int64_t> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<std::int64_t> playOption2Seed;
  DoublePlayOption doublePlayOption = DoublePlayOption::Normal;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::string playbackRulesetId;
  int playbackRulesetRevision = 0;
  int playbackRatePercent = 100;
  audio::PlaybackMode playbackMode = audio::PlaybackMode::PitchShift;
  gameplay::CandidateSelectionMode candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  int judgeWindowScalePercent = 100;
  float startingGaugePercent = 20.0F;
  std::optional<GaugeStateSnapshot> startingGaugeState;
  bool clubMode = false;
  // Empty only for judged/provenance projections that have no raw replay
  // proof. A durable replay records the remembered percentage even when lane
  // cover is disabled.
  std::optional<int> initialLaneCoverPercent;
  bool laneCoverEnabled = false;

  bool operator==(const ChartPlaybackSetup &) const = default;
};

[[nodiscard]] inline int initialLaneCoverPercentForRendering(
    const ChartPlaybackSetup &setup,
    int settingsFallbackPercent) noexcept {
  if (!setup.initialLaneCoverPercent.has_value()) {
    return settingsFallbackPercent;
  }
  return setup.laneCoverEnabled ? *setup.initialLaneCoverPercent : 0;
}

enum class LegacyPlaybackAction : std::uint8_t {
  Press = 0,
  Release = 1,
  Miss = 2,
  Mine = 3,
  Gauge = 4,
  MultiBad = 5,
};

struct LegacyPlaybackEvent {
  LegacyPlaybackAction action = LegacyPlaybackAction::Press;
  int lane = -1;
  std::int64_t noteTimeMicros = -1;
  std::int64_t songTimeMicros = 0;
  std::int64_t judgeTimeMicros = 0;
  Judgement judgement = None;
  std::int64_t diffMicros = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;

  bool operator==(const LegacyPlaybackEvent &) const = default;
};

enum class ReplayTouchAction : std::uint8_t {
  Down = 0,
  Move = 1,
  Up = 2,
  Cancel = 3,
};

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  std::int64_t fingerId = 0;
  std::int64_t songTimeMicros = 0;
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const ReplayTouchSample &) const = default;
};

struct ReplayLaneCoverEvent {
  std::int64_t songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool resetVisibleTimeReference = false;

  bool operator==(const ReplayLaneCoverEvent &) const = default;
};

struct LegacyPlaybackTrack {
  std::vector<LegacyPlaybackEvent> events;
  bool stockScratchDirectionBestEffort = false;

  bool operator==(const LegacyPlaybackTrack &) const = default;
};

struct ReplayPlaybackData {
  ChartPlaybackSetup setup;
  std::vector<InputTransition> input;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
  std::optional<LegacyPlaybackTrack> legacy;

  bool operator==(const ReplayPlaybackData &) const = default;
};

struct CourseReplayPlaybackData {
  std::vector<ReplayPlaybackData> stages;
  std::vector<std::int64_t> restMicrosAfterStage;

  bool operator==(const CourseReplayPlaybackData &) const = default;
};

} // namespace replay
