#include "PlayfieldChartVisualModel.h"
#include "GameplayScrollGeometry.h"

#include "../../ChartPlaybackDuration.h"
#include "../../bms_parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  std::unordered_map<std::uint64_t, std::size_t> entryIndexByBits;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      const std::uint64_t bits = javaDoubleBits(timeline->Bpm);
      const int noteCount = songInformationTimelineNoteCount(
          *timeline, chart, longNoteModeOverride);
      if (const auto found = entryIndexByBits.find(bits);
          found != entryIndexByBits.end()) {
        entries[found->second].noteCount += noteCount;
      } else {
        entryIndexByBits.emplace(bits, entries.size());
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
        // bms-parser keeps mine-channel notes in the ordinary slot array,
        // but Beatoraja's score note total excludes them. Invisible notes
        // are in TimeLine::InvisibleNotes and therefore never enter here.
        if (note == nullptr ||
            dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
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

int javaDoubleToIntForSongInformation(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(value);
}

PlayfieldSongInformation
beatorajaSongInformation(const bms_parser::Chart &chart,
                         int longNoteModeOverride) {
  // This follows SongInformation(BMSModel) at the pinned Beatoraja revision.
  // Keep the source's fixed seven buckets even though gameplay skins only
  // consume its aggregate density values.
  long long lastTimeMicros = 0;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline != nullptr) {
        lastTimeMicros = std::max(lastTimeMicros, timeline->Timing);
      }
    }
  }
  const std::size_t bucketCount =
      static_cast<std::size_t>(std::max(0LL, lastTimeMicros / 1'000'000)) +
      2U;
  // SongInformation uses one bucket per elapsed second. Retaining every zero
  // bucket lets a distant malformed timeline allocate gigabytes, while its
  // aggregate results only depend on sparse note changes. Keep the source's
  // inclusive per-second semantics as delta intervals instead.
  std::unordered_map<std::size_t, long long> noteCountDeltas;
  if (chart.Meta.TotalNotes > 0) {
    noteCountDeltas.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(chart.Meta.TotalNotes) * 2U + 2U,
        1U << 20U));
  }
  const auto addNoteRange = [&](std::size_t start, std::size_t end) {
    if (start > end) {
      return;
    }
    ++noteCountDeltas[start];
    --noteCountDeltas[end + 1U];
  };
  const double borderValue =
      static_cast<double>(chart.Meta.TotalNotes) *
      (1.0 - 100.0 / chart.Meta.Total);
  int border = javaDoubleToIntForSongInformation(borderValue);
  std::size_t borderPosition = 0;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      const std::size_t second =
          static_cast<std::size_t>(timeline->Timing / 1'000'000);
      for (const auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote != nullptr && !longNote->IsTail()) {
          const long long tailMillis =
              longNote->Tail->Timeline->Timing / 1'000'000;
          const std::size_t tailSecond = static_cast<std::size_t>(tailMillis);
          addNoteRange(second, tailSecond);
        }

        const bool omittedLnTail =
            longNote != nullptr && longNote->IsTail() &&
            longNoteMode(*longNote, chart, longNoteModeOverride) ==
                ChartLongNoteMode::LN;
        if (omittedLnTail) {
          continue;
        }
        if (dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
          // Mines do not contribute to the density aggregate.
        } else if (longNote == nullptr) {
          addNoteRange(second, second);
        }
        --border;
        if (border == 0) {
          borderPosition = second;
        }
      }
    }
  }

  const int bucketCountForMinimum =
      bucketCount > static_cast<std::size_t>(std::numeric_limits<int>::max())
          ? std::numeric_limits<int>::max()
          : static_cast<int>(bucketCount);
  const int minimumDensity =
      chart.Meta.TotalNotes / bucketCountForMinimum / 4;
  PlayfieldSongInformation result;
  std::vector<std::pair<std::size_t, long long>> orderedNoteCountDeltas(
      noteCountDeltas.begin(), noteCountDeltas.end());
  std::ranges::sort(orderedNoteCountDeltas, {},
                    [](const auto &entry) { return entry.first; });
  struct NoteCountSegment {
    std::size_t start = 0;
    std::size_t end = 0;
    long long notes = 0;
  };
  std::vector<NoteCountSegment> segments;
  segments.reserve(noteCountDeltas.size() + 1U);
  std::size_t segmentStart = 0;
  long long currentNotes = 0;
  const auto appendSegment = [&](std::size_t end) {
    if (segmentStart < end) {
      segments.push_back(
          {.start = segmentStart, .end = end, .notes = currentNotes});
    }
  };
  for (const auto &[second, delta] : orderedNoteCountDeltas) {
    if (second >= bucketCount) {
      break;
    }
    appendSegment(second);
    currentNotes += delta;
    segmentStart = second;
  }
  appendSegment(bucketCount);

  long double densityTotal = 0.0;
  long double densityCount = 0.0;
  for (const auto &segment : segments) {
    result.peakDensity =
        std::max(result.peakDensity, static_cast<double>(segment.notes));
    if (segment.notes >= minimumDensity) {
      const long double length =
          static_cast<long double>(segment.end - segment.start);
      densityTotal += static_cast<long double>(segment.notes) * length;
      densityCount += length;
    }
  }
  result.density = static_cast<double>(densityTotal / densityCount);

  const std::size_t window =
      std::min<std::size_t>(5U, bucketCount - borderPosition - 1U);
  const std::size_t lastWindowStart = bucketCount - window - 1U;
  std::set<std::size_t> windowStarts = {borderPosition, lastWindowStart};
  for (const auto &[second, delta] : orderedNoteCountDeltas) {
    (void)delta;
    const std::size_t first =
        second > window ? second - window : static_cast<std::size_t>(0);
    const std::size_t last = std::min(second, lastWindowStart);
    for (std::size_t start = std::max(first, borderPosition); start <= last;
         ++start) {
      windowStarts.insert(start);
    }
  }
  const auto notesAt = [&segments](std::size_t second) {
    const auto it = std::upper_bound(
        segments.begin(), segments.end(), second,
        [](std::size_t value, const NoteCountSegment &segment) {
          return value < segment.start;
        });
    return std::prev(it)->notes;
  };
  for (const std::size_t start : windowStarts) {
    long long notes = 0;
    for (std::size_t next = 0; next < window; ++next) {
      notes += notesAt(start + next);
    }
    result.endDensity = std::max(result.endDensity,
                                 static_cast<double>(notes) /
                                     static_cast<double>(window));
  }
  result.total = chart.Meta.Total;
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
  result.initialBpm = chart.Meta.Bpm;
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
      // ScoreDataProperty and skin total-note properties use the prepared
      // BMSModel total. It is the gameplay score denominator after play
      // options and effective long-note mode have been applied; deriving it
      // from the render projection can drift from the authoritative score.
      .totalNotes = chart.Meta.TotalNotes,
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
      .songInformation = beatorajaSongInformation(chart, longNoteModeOverride),
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
      const double previousSpeed =
          previous != nullptr ? previous->Speed : timeline->Speed;
      const bool terminalScrollContinuation =
          lastVisualNoteTimelineOrdinal.has_value() &&
          timelineOrdinal > *lastVisualNoteTimelineOrdinal;
      const bool retainedForProjection =
          terminalScrollContinuation ||
          gameplay_scroll_geometry::shouldKeepRenderTimeline(
              previousBpm, timeline->Bpm, stopMicros(*timeline), previousScroll,
              timeline->Scroll, previousSpeed, timeline->Speed,
              timeline->HasSpeedObject, timeline->IsFirstInMeasure,
              hasAny(timeline->Notes), hasAny(timeline->InvisibleNotes),
              hasAny(timeline->LandmineNotes));
      ChartVisualTimeline value{
          .id = nextId++,
          .timeMicros = timeline->Timing,
          .beat = timeline->BeatPosition,
          .scrollPosition = scrollPosition,
          .bpm = timeline->Bpm,
          .scrollRate = timeline->Scroll,
          .speed = timeline->Speed,
          .hasSpeedObject = timeline->HasSpeedObject,
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
      if (value.hasSpeedObject) {
        result.speedPoints.push_back(
            {.timeMicros = value.timeMicros, .speed = value.speed});
      }
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

  auto &graph = result.skinGameplayGraph;
  graph.mainBpm = result.staticMetadata.mainBpm;
  const std::size_t distributionSeconds =
      result.timelines.empty()
          ? 0
          : static_cast<std::size_t>(
                std::max(0LL, result.timelines.back().timeMicros) /
                1'000'000) +
                2;
  graph.normalDistribution.assign(distributionSeconds, {});
  graph.judgementDistributionSeconds =
      distributionSeconds == 0 ? 0 : distributionSeconds - 1;

  std::unordered_map<ChartVisualId, long long> graphTimelineTimes;
  graphTimelineTimes.reserve(result.timelines.size());
  for (const auto &timeline : result.timelines) {
    graphTimelineTimes.emplace(timeline.id, timeline.timeMicros);
  }
  const auto graphSecond = [&graphTimelineTimes](const ChartVisualNote &note) {
    const auto timeline = graphTimelineTimes.find(note.timelineId);
    return timeline == graphTimelineTimes.end()
               ? -1
               : static_cast<int>(timeline->second / 1'000'000);
  };
  std::unordered_map<ChartVisualId, int> graphSecondsByNoteId;
  graphSecondsByNoteId.reserve(result.notes.size());
  for (const auto &note : result.notes) {
    graphSecondsByNoteId.emplace(note.id, graphSecond(note));
  }
  const auto graphScratchLanes = chart.Meta.GetScratchLaneIndices();
  graph.judgementNotes.reserve(result.notes.size());
  for (const auto &note : result.notes) {
    const int second = graphSecondsByNoteId.at(note.id);
    const bool classicTail = note.source == ChartVisualNoteSource::Playable &&
                             note.kind == ChartVisualNoteKind::LongTail &&
                             note.longNoteMode == ChartLongNoteMode::LN;
    const bool countsTowardJudgement =
        note.source == ChartVisualNoteSource::Playable &&
        note.kind != ChartVisualNoteKind::Mine && !classicTail;
    graph.judgementNotes.push_back(
        {.sourceId = note.id,
         .second = second,
         .countsTowardJudgement = countsTowardJudgement,
         .redirectSourceId = classicTail ? note.pairId
                                         : kInvalidSkinGameplayGraphSourceId});
    if (second < 0 || static_cast<std::size_t>(second) >=
                          graph.normalDistribution.size()) {
      continue;
    }
    const bool scratch = std::ranges::find(graphScratchLanes, note.lane) !=
                         graphScratchLanes.end();
    if (note.kind == ChartVisualNoteKind::Mine) {
      ++graph.normalDistribution[second][6];
    } else if (note.source != ChartVisualNoteSource::Playable) {
      continue;
    } else if (note.kind == ChartVisualNoteKind::Normal) {
      ++graph.normalDistribution[second][scratch ? 2 : 5];
    } else if (note.kind == ChartVisualNoteKind::LongHead ||
               note.kind == ChartVisualNoteKind::LongTail) {
      if (note.kind == ChartVisualNoteKind::LongHead) {
        const auto pair = graphSecondsByNoteId.find(note.pairId);
        if (pair == graphSecondsByNoteId.end()) {
          continue;
        }
        const int tailSecond = pair->second;
        if (tailSecond < second || tailSecond < 0 ||
            static_cast<std::size_t>(tailSecond) >=
                graph.normalDistribution.size()) {
          continue;
        }
        for (int index = second; index <= tailSecond; ++index) {
          ++graph.normalDistribution[index][scratch ? 1 : 4];
        }
      }
      if (!(note.kind == ChartVisualNoteKind::LongTail &&
            note.longNoteMode == ChartLongNoteMode::LN)) {
        ++graph.normalDistribution[second][scratch ? 0 : 3];
        --graph.normalDistribution[second][scratch ? 1 : 4];
      }
    }
  }

  double graphSpeed = result.initialBpm;
  long long lastGraphPointTime = 0;
  graph.bpmSeries.reserve(result.timelines.size() + 2);
  graph.bpmSeries.push_back({.chartTimeMicros = 0,
                             .sourceOrder = 0,
                             .bpm = result.initialBpm,
                             .scroll = 1.0,
                             .bpmTimesScroll = result.initialBpm,
                             .graphSpeed = result.initialBpm,
                             .emitsGraphPoint = true,
                             .synthetic = true});
  for (const auto &timeline : result.timelines) {
    const double bpmTimesScroll = timeline.bpm * timeline.scrollRate;
    bool emitsGraphPoint = false;
    if (timeline.stopMicros > 0) {
      if (graphSpeed != 0.0) {
        graphSpeed = 0.0;
        emitsGraphPoint = true;
      }
    } else if (graphSpeed != bpmTimesScroll) {
      graphSpeed = bpmTimesScroll;
      emitsGraphPoint = true;
    }
    if (emitsGraphPoint) {
      lastGraphPointTime = timeline.timeMicros;
    }
    graph.bpmSeries.push_back({.chartTimeMicros = timeline.timeMicros,
                               .sourceOrder = timeline.authoredOrdinal,
                               .bpm = timeline.bpm,
                               .scroll = timeline.scrollRate,
                               .bpmTimesScroll = bpmTimesScroll,
                               .stopMicros = timeline.stopMicros,
                               .graphSpeed = graphSpeed,
                               .emitsGraphPoint = emitsGraphPoint});
  }
  if (!result.timelines.empty() &&
      lastGraphPointTime != result.timelines.back().timeMicros) {
    const auto &last = result.timelines.back();
    graph.bpmSeries.push_back({.chartTimeMicros = last.timeMicros,
                               .sourceOrder = last.authoredOrdinal,
                               .bpm = last.bpm,
                               .scroll = last.scrollRate,
                               .bpmTimesScroll = last.bpm * last.scrollRate,
                               .stopMicros = last.stopMicros,
                               .graphSpeed = graphSpeed,
                               .emitsGraphPoint = true,
                               .synthetic = true});
  }
  graph.minimumBpm = std::numeric_limits<double>::max();
  graph.maximumBpm = std::numeric_limits<double>::lowest();
  for (const auto &point : graph.bpmSeries) {
    if (!point.emitsGraphPoint) {
      continue;
    }
    if (point.graphSpeed > 0.0) {
      graph.minimumBpm = std::min(graph.minimumBpm, point.graphSpeed);
    }
    graph.maximumBpm = std::max(graph.maximumBpm, point.graphSpeed);
  }
  if (graph.minimumBpm == std::numeric_limits<double>::max()) {
    graph.minimumBpm = 0.0;
  }
  if (graph.maximumBpm == std::numeric_limits<double>::lowest()) {
    graph.maximumBpm = 0.0;
  }
  return result;
}

double speedObjectMultiplierAtTime(const PlayfieldChartVisualModel &model,
                                   long long timeMicros) noexcept {
  const auto next = std::upper_bound(
      model.speedPoints.begin(), model.speedPoints.end(), timeMicros,
      [](long long time, const ChartVisualSpeedPoint &point) {
        return time < point.timeMicros;
      });
  const ChartVisualSpeedPoint *previous =
      next == model.speedPoints.begin() ? nullptr : &*std::prev(next);
  const double speed = previous != nullptr ? previous->speed : 1.0;
  if (next == model.speedPoints.end()) {
    return speed;
  }
  const long long start = previous != nullptr ? previous->timeMicros : 0;
  const long long end = next->timeMicros;
  if (end <= start) {
    return next->speed;
  }
  double progress = static_cast<double>(timeMicros - start) /
                    static_cast<double>(end - start);
  progress = std::max(0.0, std::min(1.0, progress));
  return speed + (next->speed - speed) * progress;
}

ChartVisualTimelineAuthority chartVisualTimelineAuthorityAtTime(
    const PlayfieldChartVisualModel &model, long long timeMicros) noexcept {
  ChartVisualTimelineAuthority result{.bpm = model.initialBpm};
  const auto next = std::upper_bound(
      model.timelines.begin(), model.timelines.end(), timeMicros,
      [](long long time, const ChartVisualTimeline &timeline) {
        return time < timeline.timeMicros;
      });
  if (next != model.timelines.begin()) {
    const auto &current = *std::prev(next);
    result.bpm = current.bpm;
    result.scrollRate = current.scrollRate;
  }
  result.speedMultiplier = speedObjectMultiplierAtTime(model, timeMicros);
  return result;
}
