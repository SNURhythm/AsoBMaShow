#pragma once

#include "PlayOptionUtils.h"
#include "ReplayClearMarkUtils.h"
#include "ReplayDBHelper.h"
#include "ScoreRankUtils.h"
#include "scene/play/RhythmState.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

enum class ReplayRecordSortCriterion {
  Newest,
  ClearMark,
  Score,
  MaxCombo,
};

struct ReplayRecordFilters {
  std::optional<int> clearMarkRank;
  std::optional<std::string> playOption;
  std::optional<std::string> scoreRank;
  ReplayRecordSortCriterion sort = ReplayRecordSortCriterion::Newest;
};

namespace replay_record_filters {

inline int clearMarkBucket(int clearTypeRank) {
  if (clearTypeRank >= kClearTypeFullComboRank) {
    return kClearTypeFullComboRank;
  }
  if (clearTypeRank >= kClearTypeExHardClearRank) {
    return kClearTypeExHardClearRank;
  }
  if (clearTypeRank >= kClearTypeHardClearRank) {
    return kClearTypeHardClearRank;
  }
  if (clearTypeRank >= kClearTypeNormalClearRank) {
    return kClearTypeNormalClearRank;
  }
  if (clearTypeRank >= kClearTypeEasyClearRank) {
    return kClearTypeEasyClearRank;
  }
  if (clearTypeRank >= kClearTypeAssistedEasyClearRank) {
    return kClearTypeAssistedEasyClearRank;
  }
  return kClearTypeFailedRank;
}

inline std::string normalizedPlayOptionForFilter(
    const std::optional<std::string> &option) {
  return option.has_value() ? play_options::normalizePlayOption(*option)
                            : "NORMAL";
}

inline bool matchesPlayOption(const ReplaySummary &summary,
                              const std::string &filterOption) {
  const std::string normalizedFilter =
      play_options::normalizePlayOption(filterOption);
  const std::string option = normalizedPlayOptionForFilter(summary.playOption);
  const std::string option2 = normalizedPlayOptionForFilter(summary.playOption2);
  if (normalizedFilter == "NORMAL") {
    return play_options::isNormalPlayOption(option) &&
           play_options::isNormalPlayOption(option2);
  }
  return option == normalizedFilter || option2 == normalizedFilter;
}

inline bool matches(const ReplaySummary &summary,
                    const ReplayRecordFilters &filters) {
  if (filters.clearMarkRank.has_value() &&
      clearMarkBucket(replay_clear_mark::effectiveClearRank(summary)) !=
          *filters.clearMarkRank) {
    return false;
  }
  if (filters.playOption.has_value() &&
      !matchesPlayOption(summary, *filters.playOption)) {
    return false;
  }
  if (filters.scoreRank.has_value() &&
      score_rank::labelForScore(summary.finalScore, summary.maxScore) !=
          *filters.scoreRank) {
    return false;
  }
  return true;
}

inline std::vector<ReplaySummary>
apply(const std::vector<ReplaySummary> &summaries,
      const ReplayRecordFilters &filters) {
  std::vector<ReplaySummary> result;
  result.reserve(summaries.size());
  for (const ReplaySummary &summary : summaries) {
    if (matches(summary, filters)) {
      result.push_back(summary);
    }
  }

  std::stable_sort(result.begin(), result.end(),
                   [&](const ReplaySummary &a, const ReplaySummary &b) {
                     const int aClearRank =
                         replay_clear_mark::effectiveClearRank(a);
                     const int bClearRank =
                         replay_clear_mark::effectiveClearRank(b);
                     switch (filters.sort) {
                     case ReplayRecordSortCriterion::ClearMark:
                       if (aClearRank != bClearRank) {
                         return aClearRank > bClearRank;
                       }
                       if (a.finalScore != b.finalScore) {
                         return a.finalScore > b.finalScore;
                       }
                       if (a.maxCombo != b.maxCombo) {
                         return a.maxCombo > b.maxCombo;
                       }
                       break;
                     case ReplayRecordSortCriterion::Score:
                       if (a.finalScore != b.finalScore) {
                         return a.finalScore > b.finalScore;
                       }
                       if (aClearRank != bClearRank) {
                         return aClearRank > bClearRank;
                       }
                       if (a.maxCombo != b.maxCombo) {
                         return a.maxCombo > b.maxCombo;
                       }
                       break;
                     case ReplayRecordSortCriterion::MaxCombo:
                       if (a.maxCombo != b.maxCombo) {
                         return a.maxCombo > b.maxCombo;
                       }
                       if (a.finalScore != b.finalScore) {
                         return a.finalScore > b.finalScore;
                       }
                       if (aClearRank != bClearRank) {
                         return aClearRank > bClearRank;
                       }
                       break;
                     case ReplayRecordSortCriterion::Newest:
                       break;
                     }
                     return a.id > b.id;
                   });
  return result;
}

inline bool hasActiveCriteria(const ReplayRecordFilters &filters) {
  return filters.clearMarkRank.has_value() || filters.playOption.has_value() ||
         filters.scoreRank.has_value() ||
         filters.sort != ReplayRecordSortCriterion::Newest;
}

} // namespace replay_record_filters
