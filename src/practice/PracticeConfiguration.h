#pragma once

#include "../audio/PlaybackRate.h"
#include "../scene/play/RhythmState.h"

#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

struct GaugeOption {
  std::string_view id;
  std::string_view label;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
};

struct Configuration {
  std::string chartSha256;
  long long startMicros = 0;
  long long endMicros = 0;
  bool loop = false;
  int countInBeats = 4;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::optional<int> startingGaugePercent;
  JudgeOverride judge;
  audio::PlaybackRate playback;
  bool operator==(const Configuration &) const = default;
};

struct SkinMenuItem {
  bool available = false;
  bool selected = false;
  std::string label;
  std::string value;
  std::string text;
};

struct SkinMenuInputs {
  long long chartEndMicros = 0;
  int judgeRank = 0;
  double chartTotal = 0.0;
  int keyMode = 0;
  int random1P = 0;
  int random2P = 0;
  int doublePlay = 0;
};

struct SkinMenuState {
  std::array<SkinMenuItem, 16> items;
  float itemScrollPosition = 0.0F;
};

[[nodiscard]] SkinMenuState buildSkinMenuState(const Configuration &,
                                                const SkinMenuInputs &);

[[nodiscard]] std::span<const GaugeOption> practiceGaugeOptions();
[[nodiscard]] std::string practiceGaugeOptionId(const Configuration &value);
bool applyPracticeGaugeOption(Configuration &value, std::string_view optionId);
[[nodiscard]] std::span<const GaugeOption> practiceGaugeAutoShiftOptions();
[[nodiscard]] std::string
practiceGaugeAutoShiftOptionId(const Configuration &value);
bool applyPracticeGaugeAutoShiftOption(Configuration &value,
                                       std::string_view optionId);
[[nodiscard]] std::string
practiceGaugeLowerBoundOptionId(const Configuration &value);
bool applyPracticeGaugeLowerBoundOption(Configuration &value,
                                        std::string_view optionId);

struct SanitizedConfiguration {
  Configuration configuration;
  std::vector<std::string> diagnostics;
  [[nodiscard]] bool playable() const noexcept;
};

[[nodiscard]] int
defaultCountInBeatsForChart(int effectiveBeatsPerMeasure) noexcept;
[[nodiscard]] int defaultStartingGaugePercent(
    const Configuration &configuration,
    GaugeProfile gaugeProfile = GaugeProfile::Standard);
SanitizedConfiguration sanitize(Configuration value, long long chartEndMicros,
                                int startingGaugeMaximumPercent = 120);
[[nodiscard]] std::optional<std::string>
firstPlayabilityIssue(const Configuration &value, long long chartEndMicros);
} // namespace practice
