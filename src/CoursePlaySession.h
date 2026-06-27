#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class CourseLongNoteMode { Unspecified, LN, CN, HCN };

struct CourseConstraintRules {
  bool noSpeed = false;
  CourseJudgementConstraint judgement = CourseJudgementConstraint::None;
  CourseLongNoteMode longNoteMode = CourseLongNoteMode::Unspecified;
};

inline int courseLongNoteModeToChartMetaValue(CourseLongNoteMode mode) {
  switch (mode) {
  case CourseLongNoteMode::LN:
    return 1;
  case CourseLongNoteMode::CN:
    return 2;
  case CourseLongNoteMode::HCN:
    return 3;
  case CourseLongNoteMode::Unspecified:
    return 0;
  }
  return 0;
}

inline int normalizeChartLongNoteModeValue(int lnMode) {
  return lnMode >= 1 && lnMode <= 3 ? lnMode : 0;
}

inline bms_parser::LongNoteType resolveEffectiveLongNoteType(
    const bms_parser::LongNote *longNote, const bms_parser::Chart *chart,
    int longNoteModeOverride = 0) {
  if (longNote == nullptr) {
    return bms_parser::LongNoteType::LongNote;
  }
  const bms_parser::LongNote *head =
      longNote->IsTail() && longNote->Head != nullptr ? longNote->Head
                                                      : longNote;
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
        auto *head =
            longNote->IsTail() && longNote->Head != nullptr ? longNote->Head
                                                            : longNote;
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
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::vector<CoursePlayEntry> entries;
  std::vector<CoursePlayChartResult> completedResults;
  std::size_t currentIndex = 0;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
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

  [[nodiscard]] bool validCurrentIndex() const {
    return currentIndex < entries.size();
  }

  [[nodiscard]] bool hasNextChart() const {
    return currentIndex + 1 < entries.size();
  }

  [[nodiscard]] const bms_parser::ChartMeta *currentMeta() const {
    return validCurrentIndex() ? &entries[currentIndex].meta : nullptr;
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
};
