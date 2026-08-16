#include "PlayfieldChartVisualModel.h"
#include "GameplayScrollGeometry.h"

#include "../../ChartPlaybackDuration.h"
#include "../../bms_parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace {

std::optional<std::uint32_t> nextUtf8CodePoint(std::string_view value,
                                               std::size_t &index) {
  if (index >= value.size()) {
    return std::nullopt;
  }
  const auto byte = [&](std::size_t offset) {
    return static_cast<unsigned char>(value[index + offset]);
  };
  const unsigned char lead = byte(0);
  if (lead < 0x80) {
    ++index;
    return lead;
  }
  std::size_t length = 0;
  std::uint32_t codePoint = 0;
  std::uint32_t minimum = 0;
  if (lead >= 0xc2 && lead <= 0xdf) {
    length = 2;
    codePoint = lead & 0x1fU;
    minimum = 0x80;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    length = 3;
    codePoint = lead & 0x0fU;
    minimum = 0x800;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    length = 4;
    codePoint = lead & 0x07U;
    minimum = 0x10000;
  } else {
    return std::nullopt;
  }
  if (index + length > value.size()) {
    return std::nullopt;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const unsigned char continuation = byte(offset);
    if ((continuation & 0xc0U) != 0x80U) {
      return std::nullopt;
    }
    codePoint = (codePoint << 6U) | (continuation & 0x3fU);
  }
  if (codePoint < minimum || codePoint > 0x10ffffU ||
      (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
    return std::nullopt;
  }
  index += length;
  return codePoint;
}

std::optional<unsigned int> javaDecimalDigit(std::uint32_t codePoint) {
  // Integer.parseInt iterates UTF-16 chars and Character.digit(char, 10).
  // These are the complete BMP decimal-digit blocks in pinned OpenJDK 25;
  // supplementary mathematical digits are rejected because Java sees their
  // surrogate halves separately in parseInt(String).
  constexpr std::array<std::uint16_t, 37> zeroCodePoints = {
      0x0030, 0x0660, 0x06f0, 0x07c0, 0x0966, 0x09e6, 0x0a66, 0x0ae6,
      0x0b66, 0x0be6, 0x0c66, 0x0ce6, 0x0d66, 0x0de6, 0x0e50, 0x0ed0,
      0x0f20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x1a80,
      0x1a90, 0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900,
      0xa9d0, 0xa9f0, 0xaa50, 0xabf0, 0xff10,
  };
  if (codePoint > 0xffffU) {
    return std::nullopt;
  }
  for (const std::uint16_t zero : zeroCodePoints) {
    if (codePoint >= zero && codePoint <= zero + 9U) {
      return static_cast<unsigned int>(codePoint - zero);
    }
  }
  return std::nullopt;
}

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

std::optional<int> javaParseInt(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  bool negative = false;
  std::size_t index = 0;
  if (value.front() == '+' || value.front() == '-') {
    negative = value.front() == '-';
    index = 1;
  }
  if (index == value.size()) {
    return std::nullopt;
  }
  constexpr std::uint64_t positiveLimit =
      static_cast<std::uint64_t>(std::numeric_limits<int>::max());
  constexpr std::uint64_t negativeLimit = positiveLimit + 1;
  const std::uint64_t limit = negative ? negativeLimit : positiveLimit;
  std::uint64_t magnitude = 0;
  while (index < value.size()) {
    const auto codePoint = nextUtf8CodePoint(value, index);
    if (!codePoint) {
      return std::nullopt;
    }
    const auto parsedDigit = javaDecimalDigit(*codePoint);
    if (!parsedDigit) {
      return std::nullopt;
    }
    const std::uint64_t digit = *parsedDigit;
    if (magnitude > (limit - digit) / 10) {
      return std::nullopt;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (!negative) {
    return static_cast<int>(magnitude);
  }
  if (magnitude == negativeLimit) {
    return std::numeric_limits<int>::min();
  }
  return -static_cast<int>(magnitude);
}

std::uint64_t javaDoubleBits(double value) {
  if (std::isnan(value)) {
    return UINT64_C(0x7ff8000000000000);
  }
  return std::bit_cast<std::uint64_t>(value);
}

std::uint32_t javaHashMapHash(double value) {
  const std::uint64_t bits = javaDoubleBits(value);
  const std::uint32_t hashCode = static_cast<std::uint32_t>(bits) ^
                                 static_cast<std::uint32_t>(bits >> 32U);
  return hashCode ^ (hashCode >> 16U);
}

int songInformationTimelineNoteCount(const bms_parser::TimeLine &timeline,
                                     const bms_parser::Chart &chart,
                                     int longNoteModeOverride) {
  int count = 0;
  for (const auto *note : timeline.Notes) {
    if (note == nullptr) {
      continue;
    }
    const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
    if (longNote == nullptr) {
      ++count;
      continue;
    }
    const ChartLongNoteMode mode =
        longNoteMode(*longNote, chart, longNoteModeOverride);
    if (mode != ChartLongNoteMode::LN || !longNote->IsTail()) {
      ++count;
    }
  }
  return count;
}

double beatorajaMainBpm(const bms_parser::Chart &chart,
                        int longNoteModeOverride) {
  struct Entry {
    std::uint64_t keyBits = 0;
    double bpm = 0.0;
    int noteCount = 0;
    std::uint32_t hash = 0;
  };
  std::vector<Entry> entries;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      const std::uint64_t bits = javaDoubleBits(timeline->Bpm);
      const auto found = std::ranges::find_if(
          entries, [bits](const Entry &entry) { return entry.keyBits == bits; });
      const int noteCount = songInformationTimelineNoteCount(
          *timeline, chart, longNoteModeOverride);
      if (found != entries.end()) {
        found->noteCount += noteCount;
      } else {
        entries.push_back({.keyBits = bits,
                           .bpm = timeline->Bpm,
                           .noteCount = noteCount,
                           .hash = javaHashMapHash(timeline->Bpm)});
      }
    }
  }

  std::size_t capacity = 16;
  while (entries.size() > capacity - capacity / 4 &&
         capacity <= (std::numeric_limits<std::size_t>::max() / 2)) {
    capacity *= 2;
  }
  // Pinned SongInformation uses HashMap<Double, Integer> and a strict `>`
  // winner. OpenJDK visits buckets in ascending order and preserves linked-bin
  // insertion order across resize. The [120, 180] reference vector therefore
  // visits [180, 120] and selects 180 on a tie. Java does not specify HashMap
  // iteration order; this makes that observed behavior deterministic here.
  std::stable_sort(entries.begin(), entries.end(),
                   [capacity](const Entry &left, const Entry &right) {
                     return (left.hash & (capacity - 1)) <
                            (right.hash & (capacity - 1));
                   });
  int maximumCount = 0;
  double mainBpm = 0.0;
  for (const auto &entry : entries) {
    if (entry.noteCount > maximumCount) {
      maximumCount = entry.noteCount;
      mainBpm = entry.bpm;
    }
  }
  return mainBpm;
}

struct BeatorajaNoteCounts {
  int normalKey = 0;
  int longKey = 0;
  int normalScratch = 0;
  int longScratch = 0;
};

struct BeatorajaLongNoteFeatures {
  bool any = false;
  bool undefined = false;
  bool longNote = false;
  bool chargeNote = false;
  bool hellChargeNote = false;
};

BeatorajaLongNoteFeatures
beatorajaLongNoteFeatures(const bms_parser::Chart &chart) {
  BeatorajaLongNoteFeatures result;
  std::unordered_set<const bms_parser::LongNote *> heads;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote == nullptr) {
          continue;
        }
        const auto *head =
            longNote->IsTail() && longNote->Head != nullptr ? longNote->Head
                                                             : longNote;
        if (!heads.insert(head).second) {
          continue;
        }
        result.any = true;
        switch (head->Type) {
        case bms_parser::LongNoteType::Undefined:
          result.undefined = true;
          break;
        case bms_parser::LongNoteType::LongNote:
          result.longNote = true;
          break;
        case bms_parser::LongNoteType::ChargeNote:
          result.chargeNote = true;
          break;
        case bms_parser::LongNoteType::HellChargeNote:
          result.hellChargeNote = true;
          break;
        }
      }
    }
  }
  return result;
}

BeatorajaNoteCounts beatorajaNoteCounts(const bms_parser::Chart &chart,
                                        int longNoteModeOverride) {
  BeatorajaNoteCounts result;
  const auto scratchLanes = chart.Meta.GetScratchLaneIndices();
  const auto isScratch = [&](int lane) {
    return std::ranges::find(scratchLanes, lane) != scratchLanes.end();
  };
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        const bool scratch = isScratch(note->Lane);
        const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote == nullptr) {
          ++(scratch ? result.normalScratch : result.normalKey);
          continue;
        }
        const ChartLongNoteMode mode =
            longNoteMode(*longNote, chart, longNoteModeOverride);
        if (mode != ChartLongNoteMode::LN || !longNote->IsTail()) {
          ++(scratch ? result.longScratch : result.longKey);
        }
      }
    }
  }
  return result;
}

} // namespace

std::vector<std::string> PlayfieldChartVisualModel::runtimeStrings() const {
  std::vector<std::string> result;
  result.reserve(6 + text.auditedStringProperties.size());
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
  append(text.fullArtist);
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
  result.chartMd5 = chart.Meta.MD5;
  result.chartSha256 = chart.Meta.SHA256;
  result.keyCount = chart.Meta.KeyMode;
  result.text.title = chart.Meta.Title;
  result.text.subtitle = chart.Meta.SubTitle;
  result.text.artist = chart.Meta.Artist;
  result.text.subartist = chart.Meta.SubArtist;
  result.text.fullArtist = chart.Meta.SubArtist.empty()
                              ? chart.Meta.Artist
                              : chart.Meta.Artist + " " + chart.Meta.SubArtist;
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
      // A chart session has no selected table unless the application provides
      // one.  Beatoraja concatenates two empty resource values in that case.
      {1003, ""},
  };
  const BeatorajaNoteCounts noteCounts =
      beatorajaNoteCounts(chart, longNoteModeOverride);
  const BeatorajaLongNoteFeatures longNoteFeatures =
      beatorajaLongNoteFeatures(chart);
  const bool hasBpmStop = std::ranges::any_of(
      chart.Measures, [](const bms_parser::Measure *measure) {
        return measure != nullptr && std::ranges::any_of(
                   measure->TimeLines,
                   [](const bms_parser::TimeLine *timeline) {
                     return timeline != nullptr &&
                            timeline->GetStopDuration() > 0.0;
                   });
      });
  result.staticMetadata = {
      .difficulty = chart.Meta.Difficulty,
      .judgeRank = chart.Meta.Rank,
      .minimumBpm = chart.Meta.MinBpm,
      .maximumBpm = chart.Meta.MaxBpm,
      .mainBpm = beatorajaMainBpm(chart, longNoteModeOverride),
      .durationMicros = chart.Meta.TotalLength,
      .authoredPlayLevel = chart.Meta.PlayLevelText,
      .playLevel = javaParseInt(chart.Meta.PlayLevelText).value_or(0),
      .normalKeyNotes = noteCounts.normalKey,
      .longKeyNotes = noteCounts.longKey,
      .normalScratchNotes = noteCounts.normalScratch,
      .longScratchNotes = noteCounts.longScratch,
      .totalNotes = noteCounts.normalKey + noteCounts.longKey +
                    noteCounts.normalScratch + noteCounts.longScratch,
      .totalLandmineNotes = chart.Meta.TotalLandmineNotes,
      .hasAnyLongNote = longNoteFeatures.any,
      .hasUndefinedLongNote = longNoteFeatures.undefined,
      .hasLongNote = longNoteFeatures.longNote,
      .hasChargeNote = longNoteFeatures.chargeNote,
      .hasHellChargeNote = longNoteFeatures.hellChargeNote,
      .selectedLongNoteMode = longNoteModeOverride >= 1 &&
                                      longNoteModeOverride <= 3
                                  ? longNoteModeOverride
                                  : 1,
      .hasBga = !chart.ReferencedBmpTable.empty(),
      .hasRandomSequence = !chart.Meta.RandomValues.empty(),
      .hasBpmStop = hasBpmStop,
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

  // Normal notes remain projected through their active late-POOR judgement
  // window.  Every real parser timeline after the last visual note must
  // therefore remain in the scroll lookup: otherwise a note in the final
  // measure is left at the last retained scroll position even when later
  // zero-note rows exist.
  std::optional<std::uint32_t> lastVisualNoteTimelineOrdinal;
  std::uint32_t authoredTimelineOrdinal = 0;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      if (hasAny(timeline->Notes) || hasAny(timeline->InvisibleNotes) ||
          hasAny(timeline->LandmineNotes)) {
        lastVisualNoteTimelineOrdinal = authoredTimelineOrdinal;
      }
      ++authoredTimelineOrdinal;
    }
  }

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
      const bool terminalScrollContinuation =
          lastVisualNoteTimelineOrdinal.has_value() &&
          timelineOrdinal > *lastVisualNoteTimelineOrdinal;
      const bool retainedForProjection =
          terminalScrollContinuation ||
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

  if (previous != nullptr) {
    if (const auto endpoint = chart_playback_duration::terminalScrollEndpointAfter(
            chart, previous->Timing, previous->BeatPosition);
        endpoint.has_value()) {
      result.terminalScrollAnchor = {
          .timeMicros = endpoint->timeMicros,
          .scrollPosition =
              scrollPosition +
              (endpoint->beatPosition - previous->BeatPosition) *
                  previous->Scroll,
          .stopMicros = 0,
          .bpm = previous->Bpm,
          .scrollRate = previous->Scroll,
      };
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
      // bms-parser keeps BMS mine-channel entries in TimeLine::Notes. The
      // built-in renderer recognizes that concrete type at draw time; retain
      // the same source family before the selected-skin DTO erases parser
      // objects.
      const auto *mine = dynamic_cast<const bms_parser::LandmineNote *>(note);
      const ChartVisualNoteSource noteSource =
          source == ChartVisualNoteSource::Playable && mine != nullptr
              ? ChartVisualNoteSource::Mine
              : source;
      ChartVisualNote value{
          .id = nextId++,
          .timelineId = timelineIt->second,
          .lane = note->Lane,
          .kind = noteSource == ChartVisualNoteSource::Invisible
                      ? ChartVisualNoteKind::Invisible
                      : noteSource == ChartVisualNoteSource::Mine
                            ? ChartVisualNoteKind::Mine
                            : ChartVisualNoteKind::Normal,
          .source = noteSource,
          .authoredOrdinal = noteOrdinal++,
      };
      if (mine != nullptr) {
        value.mineDamage = static_cast<int>(std::lround(mine->Damage));
      }
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
