#include "BeatorajaHiSpeedChart.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace gameplay_hispeed {
namespace {

bool countsTowardTimelineTotal(const bms_parser::Note *note,
                               const bms_parser::Chart &chart) {
  const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
  if (longNote == nullptr) {
    return note != nullptr;
  }
  // BMSModel.TimeLine.getTotalNotes counts both endpoints of CN/HCN but only
  // the head of an LN. The chart passed to gameplay has already received its
  // effective LN mode, so this is the same endpoint rule LaneRenderer sees.
  const auto kind =
      bms_parser::ResolveLongNoteType(longNote->Type, chart.Meta.LnMode);
  return kind != bms_parser::LongNoteType::LongNote || !longNote->IsTail();
}

bool validBpm(double value) { return std::isfinite(value) && value > 0.0; }

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

} // namespace

ChartBpmSummary summarizeChartBpm(const bms_parser::Chart &chart) {
  ChartBpmSummary summary{
      .start = chart.Meta.Bpm,
      .minimum = chart.Meta.MinBpm,
      .maximum = chart.Meta.MaxBpm,
      .main = 0.0,
  };
  // LaneRenderer uses HashMap<Double, Integer> and a strict `>` winner. The
  // pinned app's runtime is OpenJDK, whose observed bucket traversal resolves
  // tied note counts by the same hash ordering used below—not ordered-BPM map
  // order. Keep the accumulator faithful because its winner drives fixed MAIN
  // Hi-Speed at chart start.
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
      if (timeline == nullptr || !validBpm(timeline->Bpm)) {
        continue;
      }
      if (!validBpm(summary.minimum) || timeline->Bpm < summary.minimum) {
        summary.minimum = timeline->Bpm;
      }
      if (!validBpm(summary.maximum) || timeline->Bpm > summary.maximum) {
        summary.maximum = timeline->Bpm;
      }
      int count = 0;
      for (const auto *note : timeline->Notes) {
        if (countsTowardTimelineTotal(note, chart)) {
          ++count;
        }
      }
      const std::uint64_t bits = javaDoubleBits(timeline->Bpm);
      const auto found = std::ranges::find_if(
          entries, [bits](const Entry &entry) { return entry.keyBits == bits; });
      if (found != entries.end()) {
        found->noteCount += count;
      } else {
        entries.push_back({.keyBits = bits,
                           .bpm = timeline->Bpm,
                           .noteCount = count,
                           .hash = javaHashMapHash(timeline->Bpm)});
      }
    }
  }

  std::size_t capacity = 16;
  while (entries.size() > capacity - capacity / 4 &&
         capacity <= (std::numeric_limits<std::size_t>::max() / 2)) {
    capacity *= 2;
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [capacity](const Entry &left, const Entry &right) {
                     return (left.hash & (capacity - 1)) <
                            (right.hash & (capacity - 1));
                   });
  int maximumNoteCount = 0;
  for (const auto &entry : entries) {
    if (entry.noteCount > maximumNoteCount) {
      maximumNoteCount = entry.noteCount;
      summary.main = entry.bpm;
    }
  }
  if (!validBpm(summary.main)) {
    summary.main = validBpm(summary.start) ? summary.start : summary.minimum;
  }
  if (!validBpm(summary.minimum)) {
    summary.minimum = summary.start;
  }
  if (!validBpm(summary.maximum)) {
    summary.maximum = summary.start;
  }
  return summary;
}

} // namespace gameplay_hispeed
