#pragma once

#include "../ChartTiming.h"
#include "../view/TextRange.h"
#include <charconv>
#include <string_view>

namespace chart_source {
inline std::string decode(const std::vector<unsigned char> &bytes) {
  const auto decoded = bms_parser::Parser::DecodeText(bytes);
  std::string text;
  text.reserve(decoded.size());
  for (size_t i = 0; i < decoded.size(); ++i) {
    if (decoded[i] == '\r') {
      text += '\n';
      if (i + 1 < decoded.size() && decoded[i + 1] == '\n') ++i;
    } else text += decoded[i];
  }
  return text;
}

// Byte offsets into decoded UTF-8. Match the parser's directive semantics,
// including its innermost #IF and ignored #ELSE/#SETRANDOM directives.
inline std::vector<text_range::Range> highlights(
    std::string_view text, const bms_parser::Chart &chart,
    long long startMicros, long long endMicros) {
  std::vector<text_range::Range> result;
  if (endMicros <= startMicros) return result;
  std::vector<double> measurePositions;
  double position = 0;
  std::vector<const bms_parser::TimeLine *> timelines;
  for (const auto *measure : chart.Measures) {
    measurePositions.push_back(position);
    position += measure->Scale;
    for (const auto *timeline : measure->TimeLines) timelines.push_back(timeline);
  }
  std::stable_sort(timelines.begin(), timelines.end(), [](auto *a, auto *b) {
    return a->BeatPosition < b->BeatPosition;
  });
  const auto timeAt = [&](double beat) {
    const auto next = std::upper_bound(timelines.begin(), timelines.end(), beat,
        [](double value, const auto *timeline) { return value < timeline->BeatPosition; });
    if (next == timelines.begin()) return 0LL;
    const auto &timeline = **std::prev(next);
    if (std::abs(beat - timeline.BeatPosition) < 1e-10) return timeline.Timing;
    if (!(timeline.Bpm > 0)) return timeline.Timing;
    return chart_timing::add(chart_timing::stopEnd(timeline),
        chart_timing::rounded((beat - timeline.BeatPosition) * 240000000.0 / timeline.Bpm));
  };
  const auto append = [&](size_t begin, size_t end) {
    if (!result.empty() && result.back().end == begin) result.back().end = end;
    else result.push_back({begin, end});
  };
  std::vector<int> randomStack;
  std::vector<bool> skipped;
  size_t randomIndex = 0;
  for (size_t offset = 0; offset < text.size();) {
    const auto newline = text.find('\n', offset);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    const auto line = text.substr(offset, end - offset);
    const size_t lineOffset = offset;
    offset = newline == std::string_view::npos ? text.size() : newline + 1;
    const auto directive = [&](std::string_view prefix) {
      if (line.size() < prefix.size()) return false;
      for (size_t i = 0; i < prefix.size(); ++i) {
        const char c = line[i] >= 'a' && line[i] <= 'z' ? line[i] - 'a' + 'A' : line[i];
        if (c != prefix[i]) return false;
      }
      return true;
    };
    const auto integer = [&](size_t start) -> std::optional<int> {
      if (start >= line.size()) return std::nullopt;
      auto value = line.substr(start);
      if (value.front() == '+') value.remove_prefix(1);
      int parsed = 0;
      const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (error != std::errc{} || last != value.data() + value.size()) return std::nullopt;
      return parsed;
    };
    if (directive("#RANDOM")) {
      if (integer(8)) {
        if (randomIndex >= chart.Meta.RandomValues.size()) break;
        randomStack.push_back(chart.Meta.RandomValues[randomIndex++]);
      }
      continue;
    }
    if (directive("#IF")) {
      if (!randomStack.empty()) {
        if (const auto value = integer(4)) skipped.push_back(randomStack.back() != *value);
      }
      continue;
    }
    if (directive("#ENDIF")) { if (!skipped.empty()) skipped.pop_back(); continue; }
    if (directive("#ENDRANDOM")) { if (!randomStack.empty()) randomStack.pop_back(); continue; }
    if (!skipped.empty() && skipped.back()) continue;
    if (line.size() < 8 || line[0] != '#' || line[6] != ':' ||
        !std::all_of(line.begin() + 1, line.begin() + 4, [](char c) { return c >= '0' && c <= '9'; })) continue;
    const size_t measure = (line[1] - '0') * 100 + (line[2] - '0') * 10 + line[3] - '0';
    if (measure >= chart.Measures.size()) continue;
    const auto data = line.substr(7);
    const double base = measurePositions[measure];
    if (line.substr(4, 2) == "02" || line.substr(4, 2) == "06") {
      const auto time = timeAt(base);
      if (time >= startMicros && time < endMicros) append(lineOffset + 7, end);
      continue;
    }
    // BMS channel objects are two ASCII characters; an odd trailing character
    // is ignored by the loader. Keep empty (00) cells visible for selected rests.
    if (!std::all_of(data.begin(), data.end(), [](unsigned char c) { return c < 0x80; })) continue;
    const size_t cells = data.size() / 2;
    for (size_t cell = 0; cell < cells; ++cell) {
      const auto time = timeAt(base + (static_cast<double>(cell) / cells) * chart.Measures[measure]->Scale);
      if (time >= startMicros && time < endMicros)
        append(lineOffset + 7 + cell * 2, lineOffset + 9 + cell * 2);
    }
  }
  return result;
}
} // namespace chart_source
