#pragma once

#include "AssistOptionUtils.h"
#include "CourseIdentity.h"
#include "ScoreProvenance.h"
#include "bms_parser.hpp"
#include "replay/ReplayLaneCoverChange.h"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class ReplayEventAction {
  Press = 0,
  Release = 1,
  Miss = 2,
  Mine = 3,
  Gauge = 4,
  MultiBad = 5,
};

enum class ReplayTouchAction {
  Down = 0,
  Move = 1,
  Up = 2,
  Cancel = 3,
};

struct ReplayEvent {
  ReplayEventAction action = ReplayEventAction::Press;
  int lane = -1;
  long long noteTimeMicros = -1;
  long long songTimeMicros = 0;
  long long judgeTimeMicros = 0;
  Judgement judgement = None;
  long long diffMicros = 0;
  float gauge = 0.0f;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;
};

namespace replay_note {

inline constexpr const char *kUnsupportedIdentityDiagnostic =
    "Replay playback cannot represent a detached long-note endpoint. "
    "The original replay and saved result are unchanged.";

// Runtime event adapters use active lane/time slots, unlike the live judge's
// complete graph identities. Null notes are non-judging input or gauge events.
inline bool hasActiveIdentity(const bms_parser::Note *note) {
  if (note == nullptr) return true;
  if (note->Timeline == nullptr) return false;
  for (const auto *active : note->Timeline->Notes) {
    if (active == note) return true;
  }
  for (const auto *active : note->Timeline->LandmineNotes) {
    if (active == note) return true;
  }
  return false;
}

inline std::string key(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

} // namespace replay_note

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  long long fingerId = 0;
  long long songTimeMicros = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct ReplayLaneCoverEvent {
  long long songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool laneCoverEnabled = false;
  ReplayLaneCoverChangeKind changeKind = ReplayLaneCoverChangeKind::Value;
  bool resetVisibleTimeReference = false;
  std::optional<lane_cover::State> coverState;
};

struct ReplayData {
  int id = 0;
  bool autoPlay = false;
  // Runtime only: no schema/provenance claim is invented for stored replays.
  bool consumerIdentityCompatible = true;
  // Runtime rejudging facts; the authenticated saved provenance is retained.
  bool staleResult = false;
  std::optional<RulesetDescriptor> playbackRuleset;
  std::optional<ScoreStageProvenance> playbackPolicy;
  std::optional<GaugeProfile> playbackGaugeProfile;
  bms_parser::ChartMeta chartMeta;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  // Stock Beatoraja replays retain these lane assignments separately from
  // chart randomization. Result skins use them for pattern_1p/2p indexes.
  std::optional<std::vector<int>> laneShufflePattern1P;
  std::optional<std::vector<int>> laneShufflePattern2P;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int initialLaneCoverPercent = 0;
  bool initialLaneCoverEnabled = false;
  bool hasInitialLaneCoverState = false;
  std::optional<lane_cover::State> initialCoverState;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::optional<long long> abortedAtSongTimeMicros;
  std::string createdAt;
  // Runtime identity of the authenticated result, retained by materialization.
  std::optional<std::string> resultAttemptId;
  // Runtime result fact from canonical judging; durable BP is stored separately.
  std::optional<int> resultPassedNotes;
  std::vector<ReplayEvent> events;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
};

struct ReplayInitialLaneCoverState {
  float percent = 0;
  bool enabled = false;
};

[[nodiscard]] inline ReplayInitialLaneCoverState replayInitialLaneCoverState(
    const ReplayData &replay, float fallbackPercent,
    bool fallbackEnabled) noexcept {
  if (replay.hasInitialLaneCoverState) {
    return {.percent = replay.initialCoverState
                           ? replay.initialCoverState->laneCoverPercent
                           : static_cast<float>(replay.initialLaneCoverPercent),
            .enabled = replay.initialLaneCoverEnabled};
  }
  return {.percent = fallbackPercent, .enabled = fallbackEnabled};
}

struct CourseReplayStageData {
  ReplayData replay;
  long long restMicrosAfterStage = 0;
};

namespace course_replay {

inline std::optional<CourseReplayStageData>
prepareStageForSave(const CourseReplayStageData &recorded,
                    const bms_parser::ChartMeta &expectedMeta) {
  const course_identity::ChartIdentity recordedIdentity{
      .sha256 = recorded.replay.chartMeta.SHA256,
      .md5 = recorded.replay.chartMeta.MD5};
  const course_identity::ChartIdentity expectedIdentity{
      .sha256 = expectedMeta.SHA256, .md5 = expectedMeta.MD5};
  if (recorded.replay.events.empty() ||
      !course_identity::sameChart(recordedIdentity, expectedIdentity)) {
    return std::nullopt;
  }

  CourseReplayStageData prepared = recorded;
  if (prepared.replay.chartMeta.BmsPath.empty()) {
    prepared.replay.chartMeta.BmsPath = expectedMeta.BmsPath;
  }
  if (prepared.replay.chartMeta.Title.empty()) {
    prepared.replay.chartMeta.Title = expectedMeta.Title;
  }
  if (prepared.replay.chartMeta.Artist.empty()) {
    prepared.replay.chartMeta.Artist = expectedMeta.Artist;
  }
  return prepared;
}

inline std::optional<std::vector<CourseReplayStageData>>
prepareCompletedPrefixForSave(
    std::span<const CourseReplayStageData> recordedStages,
    std::span<const bms_parser::ChartMeta> expectedMetas,
    std::size_t completedCharts) {
  if (completedCharts == 0 || completedCharts > recordedStages.size() ||
      completedCharts > expectedMetas.size()) {
    return std::nullopt;
  }

  std::vector<CourseReplayStageData> prepared;
  prepared.reserve(completedCharts);
  for (std::size_t index = 0; index < completedCharts; ++index) {
    auto stage = prepareStageForSave(recordedStages[index],
                                     expectedMetas[index]);
    if (!stage.has_value()) {
      return std::nullopt;
    }
    prepared.push_back(std::move(*stage));
  }
  return prepared;
}

} // namespace course_replay

struct CourseReplayEntryFacts {
  int totalNotes = 0;
  long long playLengthMicros = 0;
};

struct CourseReplayData {
  int id = 0;
  bool staleResult = false;
  int courseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::string requestedPlayOption = "NORMAL";
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int longNoteMode = 0;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string createdAt;
  std::vector<CourseReplayStageData> stages;
  // Saved full-course facts, including unplayed entries. Empty for adapters
  // whose historical source did not retain these facts.
  std::vector<CourseReplayEntryFacts> entryFacts;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
};
