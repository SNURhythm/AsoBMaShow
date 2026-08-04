#include "PlayfieldChartVisualModel.h"
#include "GameplayScrollGeometry.h"

#include "../../bms_parser.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace {

ChartLongNoteMode longNoteMode(const bms_parser::LongNote &note,
                               const bms_parser::Chart &chart,
                               int longNoteModeOverride) {
  const bms_parser::LongNote *head =
      note.IsTail() && note.Head != nullptr ? note.Head : &note;
  int mode = chart.Meta.LnMode;
  if (mode == 0) {
    mode = longNoteModeOverride;
  }
  const auto resolved = bms_parser::ResolveLongNoteType(head->Type, mode);
  switch (resolved) {
  case bms_parser::LongNoteType::ChargeNote:
    return ChartLongNoteMode::CN;
  case bms_parser::LongNoteType::HellChargeNote:
    return ChartLongNoteMode::HCN;
  case bms_parser::LongNoteType::Undefined:
  case bms_parser::LongNoteType::LongNote:
    return ChartLongNoteMode::LN;
  }
  return ChartLongNoteMode::LN;
}

long long stopMicros(const bms_parser::TimeLine &timeline) {
  const double value = timeline.GetStopDuration();
  if (!std::isfinite(value) || value <= 0.0) {
    return 0;
  }
  return static_cast<long long>(std::llround(value));
}

bool hasAny(const std::vector<bms_parser::Note *> &notes) {
  return std::ranges::any_of(notes,
                             [](const auto *note) { return note != nullptr; });
}

bool hasAny(const std::vector<bms_parser::LandmineNote *> &notes) {
  return std::ranges::any_of(notes,
                             [](const auto *note) { return note != nullptr; });
}

} // namespace

std::vector<std::string> PlayfieldChartVisualModel::runtimeStrings() const {
  std::vector<std::string> result;
  result.reserve(5 + text.auditedStringProperties.size());
  std::unordered_set<std::string> seen;
  const auto append = [&](const std::string &value) {
    if (!value.empty() && seen.insert(value).second) {
      result.push_back(value);
    }
  };
  append(text.title);
  append(text.subtitle);
  append(text.artist);
  append(text.subartist);
  append(text.genre);
  for (const auto &[id, value] : text.auditedStringProperties) {
    (void)id;
    append(value);
  }
  return result;
}

PlayfieldChartVisualModel
buildPlayfieldChartVisualModel(const bms_parser::Chart &chart,
                               int longNoteModeOverride) {
  PlayfieldChartVisualModel result;
  result.chartSha256 = chart.Meta.SHA256;
  result.keyCount = chart.Meta.KeyMode;
  result.text.title = chart.Meta.Title;
  result.text.subtitle = chart.Meta.SubTitle;
  result.text.artist = chart.Meta.Artist;
  result.text.subartist = chart.Meta.SubArtist;
  result.text.genre = chart.Meta.Genre;
  const std::string fullTitle =
      chart.Meta.SubTitle.empty()
          ? chart.Meta.Title
          : chart.Meta.Title + " " + chart.Meta.SubTitle;
  result.text.auditedStringProperties = {
      {12, fullTitle},
      {13, chart.Meta.Genre},
      {14, chart.Meta.Artist},
      {15, chart.Meta.SubArtist},
  };
  result.staticMetadata = {
      .difficulty = chart.Meta.Difficulty,
      .judgeRank = chart.Meta.Rank,
      .minimumBpm = chart.Meta.MinBpm,
      .maximumBpm = chart.Meta.MaxBpm,
      .durationMicros = chart.Meta.TotalLength,
      .totalNotes = chart.Meta.TotalNotes,
      .totalLongNotes = chart.Meta.TotalLongNotes,
      .totalScratchNotes = chart.Meta.TotalScratchNotes,
      .totalBackSpinNotes = chart.Meta.TotalBackSpinNotes,
      .totalLandmineNotes = chart.Meta.TotalLandmineNotes,
      .hasBga = !chart.ReferencedBmpTable.empty(),
      .stageFilePath = chart.Meta.StageFile.generic_string(),
      .backBmpPath = chart.Meta.BackBmp.generic_string(),
  };
  result.laneOrder = chart.Meta.GetTotalLaneIndices();

  std::size_t timelineCount = 0;
  for (const auto *measure : chart.Measures) {
    if (measure != nullptr) {
      timelineCount += measure->TimeLines.size();
    }
  }
  result.timelines.reserve(timelineCount);
  result.scrollPrefix.reserve(timelineCount);
  std::unordered_map<const bms_parser::TimeLine *, ChartVisualId> timelineIds;
  timelineIds.reserve(timelineCount);

  ChartVisualId nextId = 1;
  std::uint32_t timelineOrdinal = 0;
  std::uint32_t retainedTimelineOrdinal = 0;
  double scrollPosition = 0.0;
  const bms_parser::TimeLine *previous = nullptr;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      if (previous == nullptr) {
        scrollPosition = timeline->BeatPosition;
      } else {
        scrollPosition += (timeline->BeatPosition - previous->BeatPosition) *
                          previous->Scroll;
      }
      const bool hasNotes = hasAny(timeline->Notes) ||
                            hasAny(timeline->InvisibleNotes) ||
                            hasAny(timeline->LandmineNotes);
      const bool hasBga = timeline->BgaBase >= 0 || timeline->BgaLayer >= 0 ||
                          timeline->BgaPoor.has_value();
      const double previousBpm =
          previous != nullptr ? previous->Bpm : timeline->Bpm;
      const double previousScroll =
          previous != nullptr ? previous->Scroll : timeline->Scroll;
      const bool retainedForProjection =
          gameplay_scroll_geometry::shouldKeepRenderTimeline(
              previousBpm, timeline->Bpm, stopMicros(*timeline), previousScroll,
              timeline->Scroll, timeline->IsFirstInMeasure,
              hasAny(timeline->Notes), hasAny(timeline->InvisibleNotes),
              hasAny(timeline->LandmineNotes));
      ChartVisualTimeline value{
          .id = nextId++,
          .timeMicros = timeline->Timing,
          .beat = timeline->BeatPosition,
          .scrollPosition = scrollPosition,
          .bpm = timeline->Bpm,
          .scrollRate = timeline->Scroll,
          .speed = 1.0,
          .stopMicros = stopMicros(*timeline),
          .sectionLine = timeline->IsFirstInMeasure,
          .bgaOnly = hasBga && !hasNotes,
          .retainedForProjection = retainedForProjection,
          .authoredOrdinal = timelineOrdinal++,
      };
      if (retainedForProjection) {
        value.retainedOrdinal = retainedTimelineOrdinal++;
      }
      timelineIds.emplace(timeline, value.id);
      if (timeline->BgaPoor) {
        result.bgaPoorSequences.push_back(
            {.startBgaMicros = timeline->Timing,
             .authoredOrdinal = value.authoredOrdinal,
             .frames = timeline->BgaPoor->Frames});
      }
      result.scrollPrefix.push_back(value.scrollPosition);
      result.timelines.push_back(value);
      previous = timeline;
    }
  }

  struct PendingNote {
    const bms_parser::Note *source = nullptr;
    ChartVisualNote value;
  };
  std::vector<PendingNote> pending;
  std::unordered_map<const bms_parser::Note *, ChartVisualId> noteIds;
  std::uint32_t noteOrdinal = 0;

  const auto appendNotes = [&](const bms_parser::TimeLine &timeline,
                               const std::vector<bms_parser::Note *> &notes,
                               ChartVisualNoteSource source) {
    const auto timelineIt = timelineIds.find(&timeline);
    if (timelineIt == timelineIds.end()) {
      return;
    }
    for (const auto *note : notes) {
      if (note == nullptr) {
        continue;
      }
      ChartVisualNote value{
          .id = nextId++,
          .timelineId = timelineIt->second,
          .lane = note->Lane,
          .kind = source == ChartVisualNoteSource::Invisible
                      ? ChartVisualNoteKind::Invisible
                      : ChartVisualNoteKind::Normal,
          .source = source,
          .authoredOrdinal = noteOrdinal++,
      };
      if (const auto *longNote =
              dynamic_cast<const bms_parser::LongNote *>(note);
          longNote != nullptr) {
        value.kind = longNote->IsTail() ? ChartVisualNoteKind::LongTail
                                        : ChartVisualNoteKind::LongHead;
        value.longNoteMode =
            longNoteMode(*longNote, chart, longNoteModeOverride);
      }
      noteIds.emplace(note, value.id);
      pending.push_back({.source = note, .value = value});
    }
  };

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      appendNotes(*timeline, timeline->Notes, ChartVisualNoteSource::Playable);
      appendNotes(*timeline, timeline->InvisibleNotes,
                  ChartVisualNoteSource::Invisible);
      const auto timelineIt = timelineIds.find(timeline);
      if (timelineIt == timelineIds.end()) {
        continue;
      }
      for (const auto *mine : timeline->LandmineNotes) {
        if (mine == nullptr) {
          continue;
        }
        ChartVisualNote value{
            .id = nextId++,
            .timelineId = timelineIt->second,
            .lane = mine->Lane,
            .kind = ChartVisualNoteKind::Mine,
            .source = ChartVisualNoteSource::Mine,
            .mineDamage = static_cast<int>(std::lround(mine->Damage)),
            .authoredOrdinal = noteOrdinal++,
        };
        noteIds.emplace(mine, value.id);
        pending.push_back({.source = mine, .value = value});
      }
    }
  }

  result.notes.reserve(pending.size());
  for (auto &entry : pending) {
    if (const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(entry.source);
        longNote != nullptr) {
      const bms_parser::LongNote *pair =
          longNote->IsTail() ? longNote->Head : longNote->Tail;
      if (const auto pairIt = noteIds.find(pair); pairIt != noteIds.end()) {
        entry.value.pairId = pairIt->second;
      }
    }
    result.notes.push_back(entry.value);
  }
  return result;
}
