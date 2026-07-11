#pragma once

#include "../audio/PlaybackRate.h"
#include "../scene/play/RhythmState.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace practice {
enum class Marker : std::uint8_t { Start = 0, End = 1 };
enum class TimelineDirection : std::uint8_t { Previous = 0, Next = 1 };

[[nodiscard]] std::optional<long long>
adjacentTimelineMicros(std::span<const long long> timelineMicros,
                       long long currentMicros, TimelineDirection direction);

struct RangeSelection {
  long long startMicros = 0;
  long long endMicros = 0;
  Marker active = Marker::Start;

  void placeActiveMarker(long long timeMicros, long long chartEndMicros);
  bool operator==(const RangeSelection &) const = default;
};

enum class JudgeOverrideKind : std::uint8_t { Scale = 0, Custom = 1 };

struct JudgeOverride {
  JudgeOverrideKind kind = JudgeOverrideKind::Scale;
  int scalePercent = 100;
  bool operator==(const JudgeOverride &) const = default;
};

struct Configuration {
  std::string chartSha256;
  long long startMicros = 0;
  long long endMicros = 0;
  bool loop = false;
  int countInBeats = 4;
  GaugeType gaugeType = GaugeType::Normal;
  std::optional<int> startingGaugePercent;
  JudgeOverride judge;
  audio::PlaybackRate playback;
  bool operator==(const Configuration &) const = default;
};

struct SanitizedConfiguration {
  Configuration configuration;
  std::vector<std::string> diagnostics;
  [[nodiscard]] bool playable() const noexcept;
};

[[nodiscard]] int
defaultCountInBeatsForChart(int effectiveBeatsPerMeasure) noexcept;
SanitizedConfiguration sanitize(Configuration value, long long chartEndMicros);
[[nodiscard]] std::optional<std::string>
firstPlayabilityIssue(const Configuration &value, long long chartEndMicros);
} // namespace practice
