#pragma once

#include "../AssistOptionUtils.h"
#include "../CourseIdentity.h"
#include "../audio/PlaybackRate.h"
#include "../bms_parser.hpp"
#include "../replay/ReplayPlaybackData.h"
#include "../scene/play/GameplayRuleset.h"
#include "../scene/play/GameplayJudgeRules.h"
#include "../scene/play/Judge.h"
#include "../scene/play/RhythmState.h"

#include <map>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace analysis {

enum class JudgedPlaybackAction : std::uint8_t {
  Press,
  Release,
  Miss,
  Mine,
  Gauge,
  MultiBad,
};

struct JudgedPlaybackEvent {
  JudgedPlaybackAction action = JudgedPlaybackAction::Press;
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

  bool operator==(const JudgedPlaybackEvent &) const = default;
};

struct JudgedWindow {
  gameplay::JudgeWindowContext context = gameplay::JudgeWindowContext::Normal;
  Judgement judgement = None;
  std::int64_t earlyMicros = 0;
  std::int64_t lateMicros = 0;

  bool operator==(const JudgedWindow &) const = default;
};

struct PlaybackPolicySnapshot {
  std::string chartMd5;
  std::string chartSha256;
  int longNoteMode = 0;
  std::optional<int> sourceJudgeRank;
  int totalNotes = 0;
  std::optional<double> authoredGaugeTotal;
  double effectiveGaugeTotal = 0.0;
  gameplay::CandidateSelectionMode candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  std::vector<JudgedWindow> effectiveJudgeWindows;

  bool operator==(const PlaybackPolicySnapshot &) const = default;
};

struct PlaybackAnalysisContext {
  RulesetDescriptor ruleset = RulesetDescriptor::Legacy();
  audio::PlaybackRate playback;
  gameplay::CandidateSelectionMode candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  int judgeWindowScalePercent = 100;
  std::optional<int> startingGaugePercent;
  std::optional<PlaybackPolicySnapshot> policy;
  bool clubMode = false;

  bool operator==(const PlaybackAnalysisContext &) const = default;
};

// In-memory judgement projection for presentation, retry, practice analysis,
// ghosts, pacemakers, and video rendering. It is deliberately not a replay
// persistence model and contains no repository identity, provenance document,
// IR state, replay path, or file hash.
struct JudgedPlaybackData {
  bool autoPlay = false;
  bms_parser::ChartMeta chartMeta;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::vector<JudgedPlaybackEvent> events;
  std::vector<replay::ReplayTouchSample> touchSamples;
  // Empty for judged projections that predate raw playback setup ownership.
  std::optional<int> initialLaneCoverPercent;
  bool laneCoverEnabled = false;
  std::vector<replay::ReplayLaneCoverEvent> laneCoverEvents;
  PlaybackAnalysisContext context;
};

[[nodiscard]] inline int
initialLaneCoverPercentForRendering(const JudgedPlaybackData &playback,
                                    int settingsFallbackPercent) noexcept {
  if (!playback.initialLaneCoverPercent.has_value()) {
    return settingsFallbackPercent;
  }
  return playback.laneCoverEnabled ? *playback.initialLaneCoverPercent : 0;
}

struct JudgedCoursePlaybackStage {
  JudgedPlaybackData replay;
  long long restMicrosAfterStage = 0;
};

struct JudgedCourseEntryFacts {
  int totalNotes = 0;
  std::int64_t playLengthMicros = 0;

  bool operator==(const JudgedCourseEntryFacts &) const = default;
};

struct JudgedCoursePlaybackData {
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
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string createdAt;
  std::vector<JudgedCourseEntryFacts> entryFacts;
  std::vector<JudgedCoursePlaybackStage> stages;
  PlaybackAnalysisContext context;
};

namespace judged_course_playback {

inline std::optional<JudgedCoursePlaybackStage>
prepareStage(const JudgedCoursePlaybackStage &recorded,
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

  JudgedCoursePlaybackStage prepared = recorded;
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

inline std::optional<std::vector<JudgedCoursePlaybackStage>>
prepareCompletedPrefix(
    std::span<const JudgedCoursePlaybackStage> recordedStages,
    std::span<const bms_parser::ChartMeta> expectedMetas,
    std::size_t completedCharts) {
  if (completedCharts == 0 || completedCharts > recordedStages.size() ||
      completedCharts > expectedMetas.size()) {
    return std::nullopt;
  }
  std::vector<JudgedCoursePlaybackStage> prepared;
  prepared.reserve(completedCharts);
  for (std::size_t index = 0; index < completedCharts; ++index) {
    auto stage = prepareStage(recordedStages[index], expectedMetas[index]);
    if (!stage.has_value()) {
      return std::nullopt;
    }
    prepared.push_back(std::move(*stage));
  }
  return prepared;
}

} // namespace judged_course_playback
} // namespace analysis

using ReplayEventAction = analysis::JudgedPlaybackAction;
using ReplayTouchAction = replay::ReplayTouchAction;
using ReplayEvent = analysis::JudgedPlaybackEvent;
using ReplayTouchSample = replay::ReplayTouchSample;
using ReplayLaneCoverEvent = replay::ReplayLaneCoverEvent;
using JudgedPlaybackData = analysis::JudgedPlaybackData;
using JudgedCoursePlaybackStage = analysis::JudgedCoursePlaybackStage;
using JudgedCoursePlaybackData = analysis::JudgedCoursePlaybackData;

namespace judged_course_playback = analysis::judged_course_playback;
