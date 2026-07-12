#pragma once

#include "LongNoteModeUtils.h"
#include "ReplayData.h"
#include "bms_parser.hpp"

#include <algorithm>
#include <cstddef>
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

inline bms_parser::LongNoteType resolveEffectiveLongNoteType(
    const bms_parser::LongNote *longNote, const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  if (longNote == nullptr) {
    return bms_parser::LongNoteType::LongNote;
  }
  const bms_parser::LongNote *head = longNoteHeadOrSelf(longNote);
  int lnMode = chart != nullptr ? normalizeChartLongNoteModeValue(
                                      chart->Meta.LnMode)
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

inline bool effectiveLongNoteIsClassic(
    const bms_parser::LongNote *longNote, const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsClassic(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsClassic(
    const bms_parser::LongNote *longNote, const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsClassic(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsCharge(
    const bms_parser::LongNote *longNote, const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsCharge(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsCharge(
    const bms_parser::LongNote *longNote, const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsCharge(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsHellCharge(
    const bms_parser::LongNote *longNote, const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  return longNoteTypeIsHellCharge(
      resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride));
}

inline bool effectiveLongNoteIsHellCharge(
    const bms_parser::LongNote *longNote, const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return effectiveLongNoteIsHellCharge(longNote, &chart, longNoteModeOverride);
}

inline bool effectiveLongNoteIsCounted(
    const bms_parser::LongNote *longNote, const bms_parser::Chart &chart,
    int longNoteModeOverride = 0) {
  return longNote != nullptr &&
         (resolveEffectiveLongNoteType(longNote, &chart,
                                       longNoteModeOverride) !=
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

inline void recalculateEffectiveLongNoteCounts(
    bms_parser::Chart &chart, int longNoteModeOverride = 0) {
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

inline void applyEffectiveLongNoteModeToChart(
    bms_parser::Chart &chart, int longNoteModeOverride = 0) {
  const int lnMode = normalizeChartLongNoteModeValue(longNoteModeOverride);
  if (chart.Meta.LnMode == 0 && lnMode > 0 && chartContainsLongNote(chart)) {
    chart.Meta.LnMode = lnMode;
  }
  recalculateEffectiveLongNoteCounts(chart, lnMode);
}

inline void applyCourseConstraintsToChart(
    bms_parser::Chart &chart, const CourseConstraintRules &constraints) {
  const int lnMode = courseLongNoteModeToChartMetaValue(constraints.longNoteMode);
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

  CoursePlayChartResult(const bms_parser::ChartMeta &meta,
                        const RhythmState &state)
      : meta(meta), state(state) {}
};

struct CoursePlaySession {
  int courseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::vector<CoursePlayEntry> entries;
  std::vector<CoursePlayChartResult> completedResults;
  std::vector<CourseReplayStageData> replayStages;
  RulesetDescriptor rulesetDescriptor = RulesetDescriptor::Current();
  std::vector<std::optional<ScoreProvenance>> stageProvenance;
  std::size_t currentIndex = 0;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
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
  std::optional<bool> replayTouchVisualizationEnabled;
  std::optional<bool> replayGhostRenderingEnabled;

  [[nodiscard]] bool validCurrentIndex() const {
    return currentIndex < entries.size();
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

  void recordResult(const bms_parser::ChartMeta &meta,
                    const RhythmState &state) {
    if (completedResults.size() > currentIndex) {
      completedResults[currentIndex] = CoursePlayChartResult(meta, state);
      completedResults.erase(completedResults.begin() +
                                 static_cast<std::ptrdiff_t>(currentIndex + 1),
                             completedResults.end());
      return;
    }
    while (completedResults.size() < currentIndex) {
      const auto &entry = entries[completedResults.size()];
      completedResults.emplace_back(entry.meta, RhythmState(nullptr, false));
    }
    completedResults.emplace_back(meta, state);
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
      aggregate.eligibility = ScoreEligibility::LegacyUnverified;
    }
    return aggregate;
  }

  void recordRestMicrosAfterCurrentStage(long long restMicros) {
    ensureReplayStage(currentIndex).restMicrosAfterStage =
        std::max(0LL, restMicros);
  }
};
