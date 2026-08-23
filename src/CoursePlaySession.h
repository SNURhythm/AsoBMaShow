#pragma once

#include "LongNoteModeUtils.h"
#include "ReplayData.h"
#include "scene/play/SkinGameplayGraphState.h"
#include "bms_parser.hpp"
#include "replay/CourseContinuation.h"
#include "replay/CourseReplayCapture.h"
#include "replay/CourseResultPersistence.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class CourseLongNoteMode { Unspecified, LN, CN, HCN };

namespace course_rules {
// Course runs intentionally prohibit modified playback speed. Keeping the
// required value named prevents a neutral default from looking like an omitted
// setting at course start, stage transitions, replay, or export boundaries.
inline constexpr audio::PlaybackRate kRequiredPlaybackRate{
    .percent = 100,
    .mode = audio::PlaybackMode::PitchShift,
};
} // namespace course_rules

struct CourseConstraintRules {
  bool noSpeed = false;
  CourseJudgementConstraint judgement = CourseJudgementConstraint::None;
  CourseLongNoteMode longNoteMode = CourseLongNoteMode::Unspecified;
};

inline int courseLongNoteModeToChartMetaValue(CourseLongNoteMode mode) {
  switch (mode) {
  case CourseLongNoteMode::LN:
    return long_note_mode::kLnValue;
  case CourseLongNoteMode::CN:
    return long_note_mode::kCnValue;
  case CourseLongNoteMode::HCN:
    return long_note_mode::kHcnValue;
  case CourseLongNoteMode::Unspecified:
    return long_note_mode::kUnknownValue;
  }
  return long_note_mode::kUnknownValue;
}

inline int normalizeChartLongNoteModeValue(int lnMode) {
  return long_note_mode::normalizeValue(lnMode);
}

inline const bms_parser::LongNote *
longNoteHeadOrSelf(const bms_parser::LongNote *longNote) {
  if (longNote == nullptr) {
    return nullptr;
  }
  return longNote->IsTail() && longNote->Head != nullptr ? longNote->Head
                                                         : longNote;
}

inline bms_parser::LongNoteType
resolveEffectiveLongNoteType(const bms_parser::LongNote *longNote,
                             const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  if (longNote == nullptr) {
    return bms_parser::LongNoteType::LongNote;
  }
  const bms_parser::LongNote *head = longNoteHeadOrSelf(longNote);
  int lnMode = chart != nullptr
                   ? normalizeChartLongNoteModeValue(chart->Meta.LnMode)
                                : 0;
  if (lnMode == 0) {
    lnMode = normalizeChartLongNoteModeValue(longNoteModeOverride);
  }
  bms_parser::LongNoteType type =
      bms_parser::ResolveLongNoteType(head->Type, lnMode);
  return type == bms_parser::LongNoteType::Undefined
             ? bms_parser::LongNoteType::LongNote
             : type;
}

inline bool longNoteTypeIsClassic(bms_parser::LongNoteType type) {
  return type == bms_parser::LongNoteType::LongNote;
}

inline bool longNoteTypeIsCharge(bms_parser::LongNoteType type) {
  return type == bms_parser::LongNoteType::ChargeNote ||
         type == bms_parser::LongNoteType::HellChargeNote;
}

inline bool longNoteTypeIsHellCharge(bms_parser::LongNoteType type) {
  return type == bms_parser::LongNoteType::HellChargeNote;
}

inline bool effectiveLongNoteIsClassic(const bms_parser::LongNote *longNote,
                                       const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsClassic(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsClassic(const bms_parser::LongNote *longNote,
                                       const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsClassic(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsCharge(const bms_parser::LongNote *longNote,
                                      const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsCharge(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsCharge(const bms_parser::LongNote *longNote,
                                      const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsCharge(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsHellCharge(const bms_parser::LongNote *longNote,
                                          const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsHellCharge(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsHellCharge(const bms_parser::LongNote *longNote,
                                          const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsHellCharge(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsCounted(const bms_parser::LongNote *longNote,
                                       const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return longNote != nullptr && (resolveEffectiveLongNoteType(
                                     longNote, &chart, longNoteModeOverride) !=
              bms_parser::LongNoteType::LongNote ||
          !longNote->IsTail());
}

inline bool chartLaneIsScratch(const bms_parser::ChartMeta &meta, int lane) {
  for (int scratchLane : meta.GetScratchLaneIndices()) {
    if (scratchLane == lane) {
      return true;
    }
  }
  return false;
}

inline bool chartContainsUndefinedLongNote(const bms_parser::Chart &chart) {
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || !note->IsLongNote()) {
          continue;
        }
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        const auto *head = longNoteHeadOrSelf(longNote);
        if (head->Type == bms_parser::LongNoteType::Undefined) {
          return true;
        }
      }
    }
  }
  return false;
}

inline bool chartContainsLongNote(const bms_parser::Chart &chart) {
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note != nullptr && note->IsLongNote()) {
          return true;
        }
      }
    }
  }
  return false;
}

inline void recalculateEffectiveLongNoteCounts(bms_parser::Chart &chart,
                                               int longNoteModeOverride = 0) {
  int totalNotes = 0;
  int totalLongNotes = 0;
  int totalScratchNotes = 0;
  int totalBackSpinNotes = 0;
  int totalLandmineNotes = 0;

  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || note->IsLandmineNote()) {
          continue;
        }
        const bool scratch = chartLaneIsScratch(chart.Meta, note->Lane);
        if (note->IsLongNote()) {
          auto *longNote = static_cast<bms_parser::LongNote *>(note);
          if (!effectiveLongNoteIsCounted(longNote, chart,
                                          longNoteModeOverride)) {
            continue;
          }
          ++totalNotes;
          if (scratch) {
            ++totalBackSpinNotes;
          } else {
            ++totalLongNotes;
          }
          continue;
        }
        ++totalNotes;
        if (scratch) {
          ++totalScratchNotes;
        }
      }
      for (auto *landmine : timeline->LandmineNotes) {
        if (landmine != nullptr) {
          ++totalLandmineNotes;
        }
      }
    }
  }

  chart.Meta.TotalNotes = totalNotes;
  chart.Meta.TotalLongNotes = totalLongNotes;
  chart.Meta.TotalScratchNotes = totalScratchNotes;
  chart.Meta.TotalBackSpinNotes = totalBackSpinNotes;
  chart.Meta.TotalLandmineNotes = totalLandmineNotes;
}

inline void applyEffectiveLongNoteModeToChart(bms_parser::Chart &chart,
                                              int longNoteModeOverride = 0) {
  const int lnMode = normalizeChartLongNoteModeValue(longNoteModeOverride);
  if (chart.Meta.LnMode == 0 && lnMode > 0 && chartContainsLongNote(chart)) {
    chart.Meta.LnMode = lnMode;
  }
  recalculateEffectiveLongNoteCounts(chart, lnMode);
}

inline void
applyCourseConstraintsToChart(bms_parser::Chart &chart,
                              const CourseConstraintRules &constraints) {
  const int lnMode =
      courseLongNoteModeToChartMetaValue(constraints.longNoteMode);
  if (lnMode > 0 && chart.Meta.LnMode == 0) {
    chart.Meta.LnMode = lnMode;
  }
}

struct CoursePlayEntry {
  bms_parser::ChartMeta meta;
};

struct CoursePlayChartResult {
  bms_parser::ChartMeta meta;
  RhythmState state;
  SkinGameplayGraphState gameplayGraph;

  CoursePlayChartResult(const bms_parser::ChartMeta &meta,
                        const RhythmState &state,
                        SkinGameplayGraphState gameplayGraph = {})
      : meta(meta), state(state), gameplayGraph(std::move(gameplayGraph)) {}
};

struct CoursePlaySession {
  static constexpr std::size_t kBeatorajaSkinCourseTitleCount = 10;

  int courseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::vector<CoursePlayEntry> entries;
  std::vector<CoursePlayChartResult> completedResults;
  std::vector<std::shared_ptr<bms_parser::Chart>> ownedResultBrowseCharts;
  std::vector<std::unique_ptr<bms_parser::Chart>> preparedCourseCharts;
  std::vector<CourseReplayStageData> replayStages;
  GameplayRuleset ruleset = kDefaultGameplayRuleset;
  RulesetDescriptor rulesetDescriptor = RulesetDescriptor::Current();
  std::vector<std::optional<ScoreProvenance>> stageProvenance;
  std::string modernCourseAttemptId;
  std::int64_t modernCoursePlayedAtUnixMillis = 0;
  std::vector<result_persistence::ModernCourseStageResult>
      modernCourseStageResults;
  std::vector<replay::CourseReplayStageCapture> modernCourseReplayStages;
  std::optional<replay::CourseContinuationState> modernCourseContinuation;
  std::optional<replay::CapturedCourseReplayAttempt> modernCourseAttempt;
  std::optional<replay::CourseResultPersistenceOutcome>
      modernCoursePersistenceOutcome;
  std::string modernCourseDiagnostic;
  std::vector<std::filesystem::path> modernCourseChartPaths;
  bool modernCourseResultBrowsing = false;
  bool modernCourseRetrySameAllowed = false;
  std::size_t currentIndex = 0;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int longNoteMode = 0;
  CourseConstraintRules constraints;
  std::optional<GaugeStateSnapshot> carriedGauge;
  int carriedCombo = 0;
  int maxCombo = 0;
  bool courseScoreSaved = false;
  std::string requestedPlayOption = "NORMAL";
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  bool autoKeySound = false;
  bool courseReplayPlayback = false;
  bool courseReplaySaved = false;
  int savedCourseReplayId = 0;
  std::shared_ptr<CourseReplayData> courseReplayData = nullptr;
  std::shared_ptr<CourseReplayData> courseRetrySameData = nullptr;
  std::optional<bool> replayTouchVisualizationEnabled;
  std::optional<bool> replayGhostRenderingEnabled;

  [[nodiscard]] bool validCurrentIndex() const {
    return currentIndex < entries.size();
  }

  // SkinProperty exposes STRING_COURSE1_TITLE through
  // STRING_COURSE10_TITLE only. Retain precisely that observable prefix in
  // each gameplay-frame snapshot rather than copying an unbounded course.
  [[nodiscard]] std::vector<std::string> beatorajaSkinStageTitles() const {
    const std::size_t count =
        std::min(entries.size(), kBeatorajaSkinCourseTitleCount);
    std::vector<std::string> titles;
    titles.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      titles.push_back(entries[index].meta.Title);
    }
    return titles;
  }

  void snapshotRulesetFromReplay(const ReplayData &replay) {
    if (replay.provenance.ruleset == RulesetDescriptor::Legacy()) {
      ruleset = GameplayRuleset::Beatoraja;
      rulesetDescriptor = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
      return;
    }
    rulesetDescriptor = replay.provenance.ruleset;
    if (const auto recorded = gameplayRulesetFromId(rulesetDescriptor.id)) {
      ruleset = *recorded;
    }
  }

  [[nodiscard]] bool hasNextChart() const {
    return currentIndex + 1 < entries.size();
  }

  [[nodiscard]] bool hasNextCourseReplayStage() const {
    return courseReplayData != nullptr &&
           currentIndex + 1 < courseReplayData->stages.size();
  }

  [[nodiscard]] const bms_parser::ChartMeta *currentMeta() const {
    return validCurrentIndex() ? &entries[currentIndex].meta : nullptr;
  }

  [[nodiscard]] bool hasCourseReplayStage(std::size_t index) const {
    return courseReplayData != nullptr &&
           index < courseReplayData->stages.size();
  }

  [[nodiscard]] bool hasPreparedCourseChart(std::size_t index) const {
    return index < preparedCourseCharts.size() &&
           preparedCourseCharts[index] != nullptr;
  }

  [[nodiscard]] std::unique_ptr<bms_parser::Chart>
  takePreparedCourseChart(std::size_t index) {
    return hasPreparedCourseChart(index)
               ? std::move(preparedCourseCharts[index])
               : nullptr;
  }

  [[nodiscard]] const ReplayData *courseRetrySameStageSetup(
      std::size_t index) const {
    return courseRetrySameData != nullptr &&
                   index < courseRetrySameData->stages.size()
               ? &courseRetrySameData->stages[index].replay
               : nullptr;
  }

  [[nodiscard]] const CourseReplayStageData *
  courseReplayStage(std::size_t index) const {
    return hasCourseReplayStage(index) ? &courseReplayData->stages[index]
                                       : nullptr;
  }

  [[nodiscard]] std::shared_ptr<ReplayData>
  currentCourseReplayStageReplay() const {
    const auto *stage = courseReplayStage(currentIndex);
    return stage == nullptr ? nullptr
                            : std::make_shared<ReplayData>(stage->replay);
  }

  void applyReplayStagePlayOptions(const ReplayData &replay) {
    playOption = replay.playOption;
    playOptionSeed = replay.playOptionSeed;
    playOption2 = replay.playOption2;
    playOption2Seed = replay.playOption2Seed;
  }

  [[nodiscard]] const GaugeStateSnapshot *courseCarriedGauge() const {
    if (modernCourseContinuation.has_value()) {
      return &modernCourseContinuation->gauge;
    }
    return carriedGauge.has_value() ? &*carriedGauge : nullptr;
  }

  [[nodiscard]] int courseCarriedCombo() const {
    return modernCourseContinuation.has_value()
               ? modernCourseContinuation->combo
               : carriedCombo;
  }

  [[nodiscard]] int courseMaximumCombo() const {
    return modernCourseContinuation.has_value()
               ? modernCourseContinuation->maximumCombo
               : maxCombo;
  }

  void adoptModernCourseContinuation(
      replay::CourseContinuationState continuation) {
    carriedGauge = continuation.gauge;
    carriedCombo = continuation.combo;
    maxCombo = continuation.maximumCombo;
    modernCourseContinuation = std::move(continuation);
  }

  CourseReplayStageData &ensureReplayStage(std::size_t index) {
    while (replayStages.size() <= index) {
      replayStages.emplace_back();
    }
    return replayStages[index];
  }

  [[nodiscard]] long long restMicrosAfterCurrentStage() const {
    if (courseReplayData != nullptr &&
        currentIndex < courseReplayData->stages.size()) {
      return std::max(
          0LL, courseReplayData->stages[currentIndex].restMicrosAfterStage);
    }
    if (currentIndex < replayStages.size()) {
      return std::max(0LL, replayStages[currentIndex].restMicrosAfterStage);
    }
    return 0;
  }

  void recordResult(const bms_parser::ChartMeta &meta, const RhythmState &state,
                    SkinGameplayGraphState gameplayGraph = {}) {
    if (completedResults.size() > currentIndex) {
      completedResults[currentIndex] =
          CoursePlayChartResult(meta, state, std::move(gameplayGraph));
      completedResults.erase(completedResults.begin() +
                                 static_cast<std::ptrdiff_t>(currentIndex + 1),
                             completedResults.end());
      return;
    }
    while (completedResults.size() < currentIndex) {
      const auto &entry = entries[completedResults.size()];
      completedResults.emplace_back(entry.meta, RhythmState(nullptr, false));
    }
    completedResults.emplace_back(meta, state, std::move(gameplayGraph));
  }

  void recordReplayStage(const ReplayData &replay) {
    ensureReplayStage(currentIndex).replay = replay;
  }

  void recordStageProvenance(std::size_t index,
                             const ScoreProvenance &provenance) {
    if (stageProvenance.size() <= index) {
      stageProvenance.resize(index + 1);
    }
    stageProvenance[index] = provenance;
  }

  bool
  recordModernCourseStage(result_persistence::ModernCourseStageResult result,
                          replay::CourseReplayStageCapture replayCapture) {
    if (result.stageIndex < 0 ||
        static_cast<std::size_t>(result.stageIndex) != currentIndex ||
        currentIndex >= entries.size()) {
      return false;
    }
    if (modernCourseStageResults.size() > currentIndex) {
      modernCourseStageResults[currentIndex] = std::move(result);
      modernCourseReplayStages[currentIndex] = std::move(replayCapture);
      modernCourseStageResults.resize(currentIndex + 1);
      modernCourseReplayStages.resize(currentIndex + 1);
    } else if (modernCourseStageResults.size() == currentIndex &&
               modernCourseReplayStages.size() == currentIndex) {
      modernCourseStageResults.push_back(std::move(result));
      modernCourseReplayStages.push_back(std::move(replayCapture));
    } else {
      return false;
    }
    modernCourseAttempt.reset();
    modernCoursePersistenceOutcome.reset();
    modernCoursePlayedAtUnixMillis = 0;
    return true;
  }

  void resetModernCourseAttempt() {
    modernCourseAttemptId.clear();
    modernCoursePlayedAtUnixMillis = 0;
    modernCourseStageResults.clear();
    modernCourseReplayStages.clear();
    modernCourseContinuation.reset();
    modernCourseAttempt.reset();
    modernCoursePersistenceOutcome.reset();
    modernCourseDiagnostic.clear();
  }

  [[nodiscard]] ScoreProvenance aggregateProvenance() const {
    std::vector<ScoreProvenance> recordedStages;
    recordedStages.reserve(stageProvenance.size());
    bool hasMissingStage = false;
    for (const auto &stage : stageProvenance) {
      if (stage.has_value()) {
        recordedStages.push_back(*stage);
      } else {
        hasMissingStage = true;
      }
    }
    ScoreProvenance aggregate = mergeCourseProvenance(recordedStages);
    if (hasMissingStage) {
      aggregate.ruleset = RulesetDescriptor::Legacy();
      if (aggregate.eligibility != ScoreEligibility::Modified) {
        aggregate.eligibility = ScoreEligibility::LegacyUnverified;
      }
    }
    return aggregate;
  }

  bool recordRestMicrosAfterCurrentStage(long long restMicros) {
    if (currentIndex < modernCourseReplayStages.size()) {
      if (!modernCourseContinuation.has_value()) {
        modernCourseReplayStages[currentIndex].playback.reset();
        modernCourseAttempt.reset();
        if (modernCourseDiagnostic.empty()) {
          modernCourseDiagnostic =
              "Course continuation is unavailable for the completed stage.";
        }
        return false;
      }
      auto updated = replay::recordCourseContinuationRest(
          *modernCourseContinuation, currentIndex, restMicros);
      if (!updated.advanced() || !updated.state.has_value()) {
        modernCourseReplayStages[currentIndex].playback.reset();
        modernCourseAttempt.reset();
        modernCourseDiagnostic =
            "Course rest exceeded the shared replay limit.";
        return false;
      }
      adoptModernCourseContinuation(std::move(*updated.state));
      modernCourseReplayStages[currentIndex].restMicrosAfterStage =
          restMicros;
      modernCourseAttempt.reset();
      return true;
    }

    // Temporary legacy course adapters retain their historical non-negative
    // presentation behavior until the Slice 7 cutover.
    ensureReplayStage(currentIndex).restMicrosAfterStage =
        std::max(0LL, restMicros);
    return true;
  }
};
