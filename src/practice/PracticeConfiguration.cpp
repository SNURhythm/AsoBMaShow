#include "PracticeConfiguration.h"

#include "../CanonicalDigest.h"
#include "../scene/play/GameplayAttemptSetup.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <iomanip>
#include <sstream>

namespace practice {
namespace {
constexpr std::array<GaugeOption, 6> kGaugeOptions = {{
    {.id = "0", .label = "Assisted Easy", .gaugeType = GaugeType::AssistedEasy},
    {.id = "1", .label = "Easy", .gaugeType = GaugeType::Easy},
    {.id = "2", .label = "Normal", .gaugeType = GaugeType::Normal},
    {.id = "3", .label = "Hard", .gaugeType = GaugeType::Hard},
    {.id = "4", .label = "Ex-Hard", .gaugeType = GaugeType::ExHard},
    {.id = "5", .label = "Hazard", .gaugeType = GaugeType::Hazard},
}};
constexpr std::array<GaugeOption, 5> kGaugeAutoShiftOptions = {{
    {.id = "none", .label = "Off"},
    {.id = "continue",
     .label = "Continue at 0%",
     .gaugeAutoShift = GaugeAutoShiftMode::Continue},
    {.id = "survival_to_groove",
     .label = "Survival to Groove",
     .gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove},
    {.id = "best_clear",
     .label = "Best Clear",
     .gaugeAutoShift = GaugeAutoShiftMode::BestClear},
    {.id = "select_to_under",
     .label = "Select to Under",
     .gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder},
}};

void normalizeSha256(std::string &value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
}

bool isSha256(std::string_view value) {
  std::string normalized(value);
  normalizeSha256(normalized);
  return canonical_digest::isCanonicalLowerHex(normalized, 64);
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

SkinMenuState buildSkinMenuState(const Configuration &configuration,
                                 const SkinMenuInputs &inputs,
                                 float itemScrollPosition) {
  SkinMenuState result;
  const auto formatTime = [](long long micros) {
    const long long tenths = micros / 100'000LL;
    std::ostringstream value;
    value << std::setw(2) << tenths / 600 << ':' << std::setfill('0')
          << std::setw(2) << (tenths / 10) % 60 << '.' << tenths % 10;
    return value.str();
  };
  std::array<SkinMenuItem, 12> elements;
  const auto set = [&elements](std::size_t index, std::string label,
                               std::string value) {
    auto &item = elements[index];
    item.label = std::move(label);
    item.value = std::move(value);
    item.text = item.label + " : " + item.value;
  };
  set(0, "START TIME", formatTime(configuration.startMicros));
  set(1, "END TIME", formatTime(configuration.endMicros));
  static constexpr std::array<std::string_view, 6> gaugeNames = {
      "ASSIST EASY", "EASY", "NORMAL", "HARD", "EX-HARD", "HAZARD"};
  set(2, "GAUGE TYPE", std::string(gaugeNames[static_cast<std::size_t>(
                              gaugeTypeIndex(configuration.gaugeType))]));
  const std::string_view category =
      inputs.keyMode == 5 || inputs.keyMode == 10 ? "FIVEKEYS"
      : inputs.keyMode == 9 ? "PMS"
      : inputs.keyMode == 24 || inputs.keyMode == 48 ? "KEYBOARD"
      : "SEVENKEYS";
  set(3, "GAUGE CATEGORY", std::string(category));
  set(4, "GAUGE VALUE", configuration.startingGaugePercent
                              ? std::to_string(*configuration.startingGaugePercent)
                              : std::to_string(defaultStartingGaugePercent(configuration)));
  set(5, "JUDGERANK", std::to_string(inputs.judgeRank));
  set(6, "TOTAL", std::to_string(static_cast<int>(inputs.chartTotal)));
  set(7, "FREQUENCY", std::to_string(configuration.playback.percent));
  set(8, "GRAPHTYPE", "NOTETYPE");
  static constexpr std::array<std::string_view, 10> randomNames = {
      "NORMAL", "MIRROR", "RANDOM", "R-RANDOM", "S-RANDOM", "SPIRAL",
      "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX"};
  static constexpr std::array<std::string_view, 2> doublePlayNames = {
      "NORMAL", "FLIP"};
  set(9, "OPTION-1P",
      std::string(randomNames.at(static_cast<std::size_t>(inputs.random1P))));

  const bool doublePlay =
      inputs.keyMode == 10 || inputs.keyMode == 14 || inputs.keyMode == 48;
  std::size_t elementCount = 10;
  if (doublePlay) {
    set(10, "OPTION-2P",
        std::string(randomNames.at(
            static_cast<std::size_t>(inputs.random2P))));
    set(11, "OPTION-DP",
        std::string(doublePlayNames.at(
            static_cast<std::size_t>(inputs.doublePlay))));
    elementCount = 12;
  }

  constexpr std::size_t visibleItemCount = 10;
  const std::size_t maxOffset = elementCount - visibleItemCount;
  const float clampedPosition = itemScrollPosition < 0.0F   ? 0.0F
                                : itemScrollPosition > 1.0F ? 1.0F
                                                             : itemScrollPosition;
  // MathUtils.clamp preserves NaN and Math.round(NaN) returns zero.
  const std::size_t offset = std::isnan(clampedPosition)
                                 ? 0
                                 : static_cast<std::size_t>(std::floor(
                                       clampedPosition *
                                           static_cast<float>(maxOffset) +
                                       0.5F));
  result.itemScrollPosition =
      maxOffset == 0 ? 0.0F
                     : static_cast<float>(offset) /
                           static_cast<float>(maxOffset);
  for (std::size_t visible = 0; visible < visibleItemCount; ++visible) {
    const std::size_t element = offset + visible;
    if (element >= elementCount) {
      continue;
    }
    result.items[visible] = elements[element];
    result.items[visible].available = true;
    result.items[visible].selected = visible == 0;
  }
  return result;
}

std::span<const GaugeOption> practiceGaugeOptions() { return kGaugeOptions; }

std::string practiceGaugeOptionId(const Configuration &value) {
  return std::to_string(gaugeTypeIndex(value.gaugeType));
}

bool applyPracticeGaugeOption(Configuration &value, std::string_view optionId) {
  const auto option =
      std::ranges::find(kGaugeOptions, optionId, &GaugeOption::id);
  if (option == kGaugeOptions.end()) {
    return false;
  }
  value.gaugeType = option->gaugeType;
  return true;
}

std::span<const GaugeOption> practiceGaugeAutoShiftOptions() {
  return kGaugeAutoShiftOptions;
}

std::string practiceGaugeAutoShiftOptionId(const Configuration &value) {
  switch (value.gaugeAutoShift) {
  case GaugeAutoShiftMode::Continue:
    return "continue";
  case GaugeAutoShiftMode::SurvivalToGroove:
    return "survival_to_groove";
  case GaugeAutoShiftMode::BestClear:
    return "best_clear";
  case GaugeAutoShiftMode::SelectToUnder:
    return "select_to_under";
  case GaugeAutoShiftMode::None:
  default:
    return "none";
  }
}

bool applyPracticeGaugeAutoShiftOption(Configuration &value,
                                       std::string_view optionId) {
  const auto option =
      std::ranges::find(kGaugeAutoShiftOptions, optionId, &GaugeOption::id);
  if (option == kGaugeAutoShiftOptions.end()) {
    return false;
  }
  value.gaugeAutoShift = option->gaugeAutoShift;
  return true;
}

std::string practiceGaugeLowerBoundOptionId(const Configuration &value) {
  return std::to_string(gaugeTypeIndex(value.gaugeAutoShiftLowerBound));
}

bool applyPracticeGaugeLowerBoundOption(Configuration &value,
                                        std::string_view optionId) {
  const auto option =
      std::ranges::find(kGaugeOptions, optionId, &GaugeOption::id);
  if (option == kGaugeOptions.end()) {
    return false;
  }
  value.gaugeAutoShiftLowerBound = option->gaugeType;
  return true;
}

int defaultCountInBeatsForChart(int effectiveBeatsPerMeasure) noexcept {
  return effectiveBeatsPerMeasure >= 1 && effectiveBeatsPerMeasure <= 16
             ? effectiveBeatsPerMeasure
             : 4;
}

int defaultStartingGaugePercent(const Configuration &configuration,
                                GaugeProfile gaugeProfile) {
  RhythmState state(nullptr, false);
  state.configureGauge(configuration.gaugeType,
                       configuration.gaugeAutoShift, gaugeProfile,
                       configuration.gaugeAutoShiftLowerBound);
  return static_cast<int>(state.currentGauge);
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

SanitizedConfiguration sanitize(Configuration value, long long chartEndMicros,
                                int startingGaugeMaximumPercent) {
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
    const int maximum = std::clamp(
        startingGaugeMaximumPercent, 0,
        gameplay::kMaximumStartingGaugePercent);
    const int originalGauge = *value.startingGaugePercent;
    *value.startingGaugePercent =
        std::clamp(*value.startingGaugePercent, 0, maximum);
    diagnoseChange(originalGauge != *value.startingGaugePercent,
                   "starting gauge was clamped to 0 through " +
                       std::to_string(maximum) + " percent");
  }
  if (!validGaugeType(value.gaugeType)) {
    value.gaugeType = GaugeType::Normal;
    result.diagnostics.emplace_back("unknown gauge type was reset to Normal");
  }
  if (!validGaugeType(value.gaugeAutoShiftLowerBound)) {
    value.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
    result.diagnostics.emplace_back(
        "unknown gauge auto shift lower bound was reset to Assisted Easy");
  }

  const int originalJudgeScale = value.judge.scalePercent;
  value.judge.scalePercent = nearestStep(
      value.judge.scalePercent,
      gameplay::kMinimumJudgeWindowScalePercent,
      gameplay::kMaximumJudgeWindowScalePercent,
      gameplay::kJudgeWindowScaleStepPercent);
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
