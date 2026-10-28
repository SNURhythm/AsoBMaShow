#pragma once

#include "../scene/play/Judgement.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace judgement_indicator {
enum class ColorizationMode { Default, PerJudgement, PerRange };

struct EventStyle {
  bool visible = true;
  // No override follows the event's judgement, including chart-specific windows.
  std::optional<std::uint32_t> color;
  bool operator==(const EventStyle &) const = default;
};

struct ColorRange {
  int fromMilliseconds = -20;
  int toMilliseconds = 20;
  EventStyle style;
  bool operator==(const ColorRange &) const = default;
};

inline constexpr int kMinColorRangeMilliseconds = -1000;
inline constexpr int kMaxColorRangeMilliseconds = 1000;
inline constexpr std::size_t kMaxColorRanges = 32;

struct Colorization {
  ColorizationMode mode = ColorizationMode::Default;
  std::array<EventStyle, 6> judgements{};
  EventStyle outerRange{false, std::nullopt};
  // LR2 NORMAL (rank 2): PGREAT ±18, GREAT ±40, GOOD ±100, BAD ±200 ms;
  // empty POOR extends to -1000 ms. Colors still inherit the actual judgement.
  std::vector<ColorRange> ranges{
      {-1000, -200, {}}, {-200, -100, {}}, {-100, -40, {}}, {-40, -18, {}},
      {-18, 18, {}}, {18, 40, {}}, {40, 100, {}}, {100, 200, {}}};
  bool operator==(const Colorization &) const = default;
};

inline std::uint32_t defaultEventColor(Judgement judgement) {
  switch (judgement) {
  case PGreat: return 0xC8FFFFU;
  case Great: return 0xC8FFC8U;
  case Good: return 0xC8C8FFU;
  case Bad: return 0xFFC8C8U;
  case Kpoor: return 0xFF3232U;
  case Poor: return 0xFF6464U;
  default: return 0xFFFFFFU;
  }
}

inline void sanitizeColorization(Colorization &settings) {
  if (settings.mode < ColorizationMode::Default ||
      settings.mode > ColorizationMode::PerRange)
    settings.mode = ColorizationMode::Default;
  const auto sanitizeStyle = [](EventStyle &style) {
    if (style.color) *style.color &= 0xFFFFFFU;
  };
  for (auto &style : settings.judgements) sanitizeStyle(style);
  sanitizeStyle(settings.outerRange);
  if (settings.ranges.size() > kMaxColorRanges) settings.ranges.resize(kMaxColorRanges);
  for (auto &range : settings.ranges) {
    range.fromMilliseconds = std::clamp(range.fromMilliseconds,
        kMinColorRangeMilliseconds, kMaxColorRangeMilliseconds);
    range.toMilliseconds = std::clamp(range.toMilliseconds,
        kMinColorRangeMilliseconds, kMaxColorRangeMilliseconds);
    sanitizeStyle(range.style);
  }
  std::stable_sort(settings.ranges.begin(), settings.ranges.end(),
      [](const auto &a, const auto &b) { return a.fromMilliseconds < b.fromMilliseconds; });
  int previousEnd = kMinColorRangeMilliseconds;
  std::erase_if(settings.ranges, [&previousEnd](const auto &range) {
    if (range.fromMilliseconds >= range.toMilliseconds ||
        range.fromMilliseconds < previousEnd) return true;
    previousEnd = range.toMilliseconds;
    return false;
  });
}

// Input must be sanitized; neither resolution nor rendering allocates.
inline EventStyle resolveEventStyle(const Colorization &settings,
                                    Judgement judgement, long long diffMicros) {
  if (settings.mode == ColorizationMode::PerJudgement &&
      judgement >= PGreat && judgement <= Poor)
    return settings.judgements[static_cast<std::size_t>(judgement)];
  if (settings.mode == ColorizationMode::PerRange) {
    if (settings.ranges.empty() ||
        diffMicros < settings.ranges.front().fromMilliseconds * 1000LL ||
        diffMicros >= settings.ranges.back().toMilliseconds * 1000LL)
      return settings.outerRange;
    for (const auto &range : settings.ranges) {
      if (diffMicros >= range.fromMilliseconds * 1000LL &&
          diffMicros < range.toMilliseconds * 1000LL) return range.style;
    }
  }
  return {};
}

// Fill an uncovered interval first; otherwise split the widest existing range.
inline bool addColorRange(Colorization &settings) {
  if (settings.ranges.size() >= kMaxColorRanges) return false;
  int start = kMinColorRangeMilliseconds;
  for (std::size_t i = 0; i < settings.ranges.size(); ++i) {
    if (start < settings.ranges[i].fromMilliseconds) {
      settings.ranges.insert(settings.ranges.begin() + i,
                             {start, settings.ranges[i].fromMilliseconds, {}});
      return true;
    }
    start = settings.ranges[i].toMilliseconds;
  }
  if (start < kMaxColorRangeMilliseconds) {
    settings.ranges.push_back({start, kMaxColorRangeMilliseconds, {}});
    return true;
  }
  auto widest = std::max_element(settings.ranges.begin(), settings.ranges.end(),
      [](const auto &a, const auto &b) {
        return a.toMilliseconds - a.fromMilliseconds < b.toMilliseconds - b.fromMilliseconds;
      });
  if (widest == settings.ranges.end() || widest->toMilliseconds - widest->fromMilliseconds < 2)
    return false;
  const int end = widest->toMilliseconds;
  const int midpoint = widest->fromMilliseconds + (end - widest->fromMilliseconds) / 2;
  widest->toMilliseconds = midpoint;
  settings.ranges.insert(widest + 1, {midpoint, end, {}});
  return true;
}
} // namespace judgement_indicator
