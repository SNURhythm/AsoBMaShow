#pragma once

#include "../bms_parser.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace audio {
// Estimate a representative active section using only parsed chart timing.
// Sparse one-second bins bound memory by chart events, even for huge gaps.
// The parser supplies ordered measures/timelines; malformed order falls back
// to the beginning. No asset reads or audio decoding are needed.
inline long long FindChartPreviewHighlightMicros(const bms_parser::Chart &chart) {
  constexpr long long second = 1'000'000;
  constexpr long long window = 12 * second;
  struct Bin { long long time; double activity; };
  std::vector<Bin> bins;
  std::vector<long long> boundaries;
  long long previous = -1;
  long long first = -1, last = 0;
  for (const auto *measure : chart.Measures) {
    if (!measure) continue;
    if (measure->Timing < 0 ||
        (!boundaries.empty() && measure->Timing < boundaries.back())) return 0;
    boundaries.push_back(measure->Timing);
    for (const auto *timeline : measure->TimeLines) {
      if (!timeline) continue;
      if (timeline->Timing < previous || timeline->Timing < 0) return 0;
      previous = timeline->Timing;
      double activity = 0;
      // Placed notes carry musical density even without assigned keysounds.
      // Invisible notes, mines and silent LN tails are not extra attacks.
      // Placed and background events carry the same musical activity weight.
      for (auto *note : timeline->Notes) {
        if (!note) continue;
        if (note->Wav == bms_parser::Parser::NoWav && note->IsLongNote() &&
            static_cast<const bms_parser::LongNote *>(note)->IsTail()) continue;
        activity += 1;
      }
      for (const auto *note : timeline->BackgroundNotes)
        if (note && note->Wav != bms_parser::Parser::NoWav) activity += 1;
      if (!activity) continue;
      if (first < 0) first = timeline->Timing;
      last = timeline->Timing;
      const auto time = timeline->Timing / second;
      if (bins.empty() || bins.back().time != time) bins.push_back({time, 0});
      bins.back().activity = std::min(32.0, bins.back().activity + activity);
    }
  }
  if (first < 0 || last - first < 2 * window) return 0;

  // Square-root compression and balanced halves favor sustained sections over
  // isolated chords/rolls. Prefix sums and moving cursors keep scoring linear.
  std::vector<double> prefix(1, 0);
  prefix.reserve(bins.size() + 1);
  for (const auto &bin : bins) prefix.push_back(prefix.back() + std::sqrt(bin.activity));
  const auto span = last - first;
  const auto earliest = first + span / 100 * 15;
  const auto latest = std::min(first + span / 100 * 80, last - window);
  std::size_t before = 0, begin = 0, middle = 0, end = 0;
  double bestScore = 0;
  long long best = 0;
  for (const auto start : boundaries) {
    if (start < earliest) continue;
    if (start > latest) break;
    // Score the nearest second: parser timing can put a measure a microsecond
    // before an integral second. Playback still starts at the exact measure.
    const auto binStart = start / second + (start % second >= second / 2 ? 1 : 0);
    while (before < bins.size() && bins[before].time < binStart - 6) ++before;
    while (begin < bins.size() && bins[begin].time < binStart) ++begin;
    while (middle < bins.size() && bins[middle].time < binStart + 6) ++middle;
    while (end < bins.size() && bins[end].time < binStart + 12) ++end;
    if (middle - begin < 2 || end - middle < 2) continue;
    const auto left = prefix[middle] - prefix[begin];
    const auto right = prefix[end] - prefix[middle];
    const auto sustained = std::min(left, right);
    double score = left + right + sustained;
    if (start - first >= window / 2) {
      const auto prior = prefix[begin] - prefix[before];
      const auto rise = std::max(0.0, sustained - prior);
      const auto position = static_cast<double>(start - first) / span;
      const auto centerWeight = 2 * std::sqrt(position * (1 - position));
      // Qwilight-inspired density rise, sustained across both forward halves.
      // Bound the bonus to at most one sixth of the base activity score so a
      // short spike cannot displace a substantially stronger passage.
      score += 0.5 * rise * centerWeight;
    }
    // Keep the earliest equivalent phrase, including floating-point ties.
    if (score > bestScore + 1e-9) {
      bestScore = score;
      best = start;
    }
  }
  return best;
}
} // namespace audio
