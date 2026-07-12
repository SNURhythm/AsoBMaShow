#include "PracticeConfiguration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace practice {
namespace {
constexpr std::array<GaugeOption, 10> kGaugeOptions = {{
    {.id = "0", .label = "Assisted Easy", .gaugeType = GaugeType::AssistedEasy},
    {.id = "1", .label = "Easy", .gaugeType = GaugeType::Easy},
    {.id = "2", .label = "Normal", .gaugeType = GaugeType::Normal},
    {.id = "3", .label = "Hard", .gaugeType = GaugeType::Hard},
    {.id = "4", .label = "Ex-Hard", .gaugeType = GaugeType::ExHard},
    {.id = "5", .label = "Hazard", .gaugeType = GaugeType::Hazard},
    {.id = "gas_continue",
     .label = "Continue at 0%",
     .gaugeType = GaugeType::ExHard,
     .gaugeAutoShift = GaugeAutoShiftMode::Continue},
    {.id = "gas_survival_to_groove",
     .label = "Survival to Groove",
     .gaugeType = GaugeType::ExHard,
     .gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove},
    {.id = "gas_best_clear",
     .label = "Best Clear",
     .gaugeType = GaugeType::ExHard,
     .gaugeAutoShift = GaugeAutoShiftMode::BestClear},
    {.id = "gas_select_to_under",
     .label = "Select to Under (GAS)",
     .gaugeType = GaugeType::ExHard,
     .gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder},
}};

bool isSha256(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

void normalizeSha256(std::string &value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
}

int nearestStep(int value, int minimum, int maximum, int step) {
  value = std::clamp(value, minimum, maximum);
  const int offset = value - minimum;
  return minimum + ((offset + step / 2) / step) * step;
}

bool validGaugeType(GaugeType value) {
  switch (value) {
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  case GaugeType::Hard:
  case GaugeType::ExHard:
  case GaugeType::Hazard:
    return true;
  }
  return false;
}
} // namespace

std::span<const GaugeOption> practiceGaugeOptions() { return kGaugeOptions; }

std::string practiceGaugeOptionId(const Configuration &value) {
  switch (value.gaugeAutoShift) {
  case GaugeAutoShiftMode::Continue:
    return "gas_continue";
  case GaugeAutoShiftMode::SurvivalToGroove:
    return "gas_survival_to_groove";
  case GaugeAutoShiftMode::BestClear:
    return "gas_best_clear";
  case GaugeAutoShiftMode::SelectToUnder:
    return "gas_select_to_under";
  case GaugeAutoShiftMode::None:
  default:
    return std::to_string(gaugeTypeIndex(value.gaugeType));
  }
}

bool applyPracticeGaugeOption(Configuration &value, std::string_view optionId) {
  const auto option =
      std::ranges::find(kGaugeOptions, optionId, &GaugeOption::id);
  if (option == kGaugeOptions.end()) {
    return false;
  }
  value.gaugeType = option->gaugeType;
  value.gaugeAutoShift = option->gaugeAutoShift;
  return true;
}

int defaultCountInBeatsForChart(int effectiveBeatsPerMeasure) noexcept {
  return effectiveBeatsPerMeasure >= 1 && effectiveBeatsPerMeasure <= 16
             ? effectiveBeatsPerMeasure
             : 4;
}

void RangeSelection::placeActiveMarker(long long timeMicros,
                                       long long chartEndMicros) {
  const long long clamped =
      std::clamp(timeMicros, 0LL, std::max(0LL, chartEndMicros));
  if (active == Marker::Start) {
    startMicros = clamped;
  } else {
    endMicros = clamped;
  }
  if (startMicros > endMicros) {
    std::swap(startMicros, endMicros);
    active = active == Marker::Start ? Marker::End : Marker::Start;
  }
}

std::optional<long long>
adjacentTimelineMicros(std::span<const long long> timelineMicros,
                       long long currentMicros, TimelineDirection direction) {
  if (direction == TimelineDirection::Next) {
    const auto next = std::ranges::upper_bound(timelineMicros, currentMicros);
    return next == timelineMicros.end() ? std::nullopt
                                        : std::optional<long long>(*next);
  }
  auto previous = std::ranges::lower_bound(timelineMicros, currentMicros);
  if (previous == timelineMicros.begin()) {
    return std::nullopt;
  }
  return *--previous;
}

bool SanitizedConfiguration::playable() const noexcept {
  return isSha256(configuration.chartSha256) &&
         configuration.startMicros < configuration.endMicros &&
         configuration.judge.kind == JudgeOverrideKind::Scale &&
         configuration.playback.valid() &&
         configuration.playback.mode == audio::PlaybackMode::PitchShift &&
         validGaugeType(configuration.gaugeType);
}

SanitizedConfiguration sanitize(Configuration value, long long chartEndMicros) {
  SanitizedConfiguration result;
  auto diagnoseChange = [&](bool changed, std::string message) {
    if (changed) {
      result.diagnostics.push_back(std::move(message));
    }
  };

  if (isSha256(value.chartSha256)) {
    const std::string original = value.chartSha256;
    normalizeSha256(value.chartSha256);
    diagnoseChange(original != value.chartSha256,
                   "chart SHA-256 was normalized to lowercase");
  } else {
    result.diagnostics.emplace_back("chart SHA-256 must contain 64 hex digits");
  }

  const long long playableEnd = std::max(0LL, chartEndMicros);
  const long long originalStart = value.startMicros;
  const long long originalEnd = value.endMicros;
  value.startMicros = std::clamp(value.startMicros, 0LL, playableEnd);
  value.endMicros = std::clamp(value.endMicros, 0LL, playableEnd);
  diagnoseChange(originalStart != value.startMicros ||
                     originalEnd != value.endMicros,
                 "practice markers were clamped to the chart range");
  if (value.startMicros > value.endMicros) {
    std::swap(value.startMicros, value.endMicros);
    result.diagnostics.emplace_back("crossed practice markers were ordered");
  }
  if (value.startMicros == value.endMicros) {
    result.diagnostics.emplace_back("practice range must be non-empty");
  }

  const int originalCountIn = value.countInBeats;
  value.countInBeats = std::clamp(value.countInBeats, 0, 16);
  diagnoseChange(originalCountIn != value.countInBeats,
                 "count-in beats were clamped to 0 through 16");

  if (value.startingGaugePercent) {
    const int originalGauge = *value.startingGaugePercent;
    *value.startingGaugePercent =
        std::clamp(*value.startingGaugePercent, 0, 120);
    diagnoseChange(originalGauge != *value.startingGaugePercent,
                   "starting gauge was clamped to 0 through 120 percent");
  }
  if (gaugeAutoShiftEnabled(value.gaugeAutoShift) &&
      value.gaugeType != GaugeType::ExHard) {
    value.gaugeType = GaugeType::ExHard;
    result.diagnostics.emplace_back(
        "gauge auto shift seed was normalized to Ex-Hard");
  } else if (!validGaugeType(value.gaugeType)) {
    value.gaugeType = GaugeType::Normal;
    result.diagnostics.emplace_back("unknown gauge type was reset to Normal");
  }

  const int originalJudgeScale = value.judge.scalePercent;
  value.judge.scalePercent = nearestStep(value.judge.scalePercent, 25, 200, 5);
  diagnoseChange(originalJudgeScale != value.judge.scalePercent,
                 "judge scale was clamped to a supported five-percent step");
  if (value.judge.kind != JudgeOverrideKind::Scale) {
    result.diagnostics.emplace_back(
        "custom judge windows are recognized but not yet playable");
  }

  const int originalPlaybackPercent = value.playback.percent;
  value.playback.percent = nearestStep(value.playback.percent, 50, 200, 5);
  diagnoseChange(originalPlaybackPercent != value.playback.percent,
                 "playback rate was clamped to a supported five-percent step");
  if (value.playback.mode != audio::PlaybackMode::PitchShift) {
    result.diagnostics.emplace_back(
        "time-stretch playback is recognized but not yet available");
  }

  result.configuration = std::move(value);
  return result;
}

std::optional<std::string> firstPlayabilityIssue(const Configuration &value,
                                                 long long chartEndMicros) {
  if (!isSha256(value.chartSha256)) {
    return "Chart SHA-256 is unavailable or invalid.";
  }
  const long long chartEnd = std::max(0LL, chartEndMicros);
  const long long start = std::clamp(value.startMicros, 0LL, chartEnd);
  const long long end = std::clamp(value.endMicros, 0LL, chartEnd);
  if (start >= end) {
    return "Practice range must be non-empty.";
  }
  if (!validGaugeType(value.gaugeType)) {
    return "Gauge selection is invalid.";
  }
  if (value.judge.kind != JudgeOverrideKind::Scale) {
    return "Custom judge windows are not available.";
  }
  if (value.playback.percent < 50 || value.playback.percent > 200 ||
      value.playback.percent % 5 != 0) {
    return "Playback rate must be 50-200% in 5% steps.";
  }
  if (value.playback.mode == audio::PlaybackMode::TimeStretch) {
    return "Time Stretch is not available.";
  }
  if (value.playback.mode != audio::PlaybackMode::PitchShift) {
    return "Playback mode is invalid.";
  }
  return std::nullopt;
}
} // namespace practice
