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

constexpr std::array<std::string_view, 9> kSkinMenuGaugeNames = {
    "ASSIST EASY", "EASY",      "NORMAL",       "HARD",    "EX-HARD",
    "HAZARD",      "GRADE",     "EX GRADE",     "EXHARD GRADE"};
constexpr std::array<std::string_view, 10> kSkinMenuRandomNames = {
    "NORMAL", "MIRROR", "RANDOM", "R-RANDOM", "S-RANDOM",
    "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX"};
constexpr std::array<std::string_view, 2> kSkinMenuDoublePlayNames = {
    "NORMAL", "FLIP"};
constexpr std::array<std::string_view, 3> kSkinMenuGraphTypeNames = {
    "NOTETYPE", "JUDGE", "EARLYLATE"};

constexpr bool isDoublePlayKeyMode(int keyMode) noexcept {
  return keyMode == 10 || keyMode == 14 || keyMode == 48;
}

constexpr bool isPopnKeyMode(int keyMode) noexcept {
  return keyMode == 5 || keyMode == 9;
}

constexpr SkinMenuGaugeCategory
defaultSkinMenuGaugeCategory(int keyMode) noexcept {
  if (keyMode == 5 || keyMode == 10) {
    return SkinMenuGaugeCategory::FiveKeys;
  }
  if (keyMode == 9) {
    return SkinMenuGaugeCategory::Pms;
  }
  if (keyMode == 24 || keyMode == 48) {
    return SkinMenuGaugeCategory::Keyboard;
  }
  return SkinMenuGaugeCategory::SevenKeys;
}

constexpr std::string_view
skinMenuGaugeCategoryName(SkinMenuGaugeCategory category) noexcept {
  switch (category) {
  case SkinMenuGaugeCategory::FiveKeys:
    return "FIVEKEYS";
  case SkinMenuGaugeCategory::SevenKeys:
    return "SEVENKEYS";
  case SkinMenuGaugeCategory::Pms:
    return "PMS";
  case SkinMenuGaugeCategory::Keyboard:
    return "KEYBOARD";
  case SkinMenuGaugeCategory::Lr2:
    return "LR2";
  }
  return "SEVENKEYS";
}

constexpr int skinMenuGaugeInitial(SkinMenuGaugeCategory category,
                                   int gaugeType) noexcept {
  switch (category) {
  case SkinMenuGaugeCategory::Pms:
    return gaugeType <= 2 ? 30 : 100;
  case SkinMenuGaugeCategory::Keyboard:
    return gaugeType == 0 ? 30 : (gaugeType <= 2 ? 20 : 100);
  case SkinMenuGaugeCategory::FiveKeys:
  case SkinMenuGaugeCategory::SevenKeys:
  case SkinMenuGaugeCategory::Lr2:
    return gaugeType <= 2 ? 20 : 100;
  }
  return 20;
}

constexpr int skinMenuGaugeMaximum(SkinMenuGaugeCategory category,
                                   int gaugeType) noexcept {
  return category == SkinMenuGaugeCategory::Pms && gaugeType <= 2 ? 120
                                                                    : 100;
}

constexpr int roundDownToHundredMillis(int value) noexcept {
  return value / 100 * 100;
}

std::string formatSkinMenuTime(int milliseconds) {
  std::ostringstream value;
  value << std::setw(2) << milliseconds / 60'000 << ':' << std::setfill('0')
        << std::setw(2) << (milliseconds / 1'000) % 60 << '.'
        << (milliseconds / 100) % 10;
  return value.str();
}
} // namespace

SkinMenuController::SkinMenuController(Configuration configuration,
                                       SkinMenuInputs inputs)
    : configuration_(std::move(configuration)), inputs_(std::move(inputs)) {
  property_.startTimeMillis =
      static_cast<int>(configuration_.startMicros / 1'000LL);
  property_.endTimeMillis =
      static_cast<int>(configuration_.endMicros / 1'000LL);
  property_.gaugeCategory = defaultSkinMenuGaugeCategory(inputs_.keyMode);
  property_.gaugeType = gaugeTypeIndex(configuration_.gaugeType);
  property_.startGauge = configuration_.startingGaugePercent.value_or(
      skinMenuGaugeInitial(property_.gaugeCategory, property_.gaugeType));
  property_.random1P = inputs_.random1P;
  property_.random2P = inputs_.random2P;
  property_.doublePlay = inputs_.doublePlay;
  property_.judgeRank = inputs_.judgeRank;
  property_.frequencyPercent = configuration_.playback.percent;
  property_.total = inputs_.chartTotal;
  synchronizeConfiguration();
}

void SkinMenuController::setItemScrollPosition(float position) noexcept {
  const float clampedPosition = position < 0.0F   ? 0.0F
                                : position > 1.0F ? 1.0F
                                                   : position;
  // MathUtils.clamp preserves NaN and Math.round(NaN) returns zero.
  itemOffset_ = std::isnan(clampedPosition)
                    ? 0
                    : static_cast<std::size_t>(std::floor(
                          clampedPosition * static_cast<float>(maxItemOffset()) +
                          0.5F));
  if (const auto firstVisible = visibleElement(0); firstVisible.has_value()) {
    cursorPosition_ = *firstVisible;
  }
}

float SkinMenuController::itemScrollPosition() const noexcept {
  const std::size_t maximum = maxItemOffset();
  return maximum == 0
             ? 0.0F
             : static_cast<float>(itemOffset_) / static_cast<float>(maximum);
}

SkinMenuState SkinMenuController::skinMenuState() const {
  SkinMenuState result;
  const auto set = [&result](std::size_t index, std::string label,
                             std::string value) {
    auto &item = result.items[index];
    item.label = std::move(label);
    item.value = std::move(value);
    item.text = item.label + " : " + item.value;
  };
  const auto visible = [this](std::size_t index) {
    return visibleElement(index);
  };
  const auto elementLabel = [](std::size_t element) -> std::string_view {
    static constexpr std::array<std::string_view, 12> labels = {
        "START TIME", "END TIME",  "GAUGE TYPE", "GAUGE CATEGORY",
        "GAUGE VALUE", "JUDGERANK", "TOTAL",      "FREQUENCY",
        "GRAPHTYPE",  "OPTION-1P", "OPTION-2P",  "OPTION-DP"};
    return labels[element];
  };
  const auto elementValue = [this](std::size_t element) -> std::string {
    switch (element) {
    case 0:
      return formatSkinMenuTime(property_.startTimeMillis);
    case 1:
      return formatSkinMenuTime(property_.endTimeMillis);
    case 2:
      return std::string(kSkinMenuGaugeNames[static_cast<std::size_t>(
          property_.gaugeType)]);
    case 3:
      return std::string(skinMenuGaugeCategoryName(property_.gaugeCategory));
    case 4:
      return std::to_string(property_.startGauge);
    case 5:
      return std::to_string(property_.judgeRank);
    case 6:
      return std::to_string(static_cast<int>(property_.total));
    case 7:
      return std::to_string(property_.frequencyPercent);
    case 8:
      return std::string(kSkinMenuGraphTypeNames[static_cast<std::size_t>(
          property_.graphType)]);
    case 9:
      return std::string(kSkinMenuRandomNames[static_cast<std::size_t>(
          property_.random1P)]);
    case 10:
      return std::string(kSkinMenuRandomNames[static_cast<std::size_t>(
          property_.random2P)]);
    case 11:
      return std::string(kSkinMenuDoublePlayNames[static_cast<std::size_t>(
          property_.doublePlay)]);
    default:
      return {};
    }
  };

  result.itemScrollPosition = itemScrollPosition();
  for (std::size_t index = 0; index < 10; ++index) {
    const auto element = visible(index);
    if (!element.has_value()) {
      continue;
    }
    set(index, std::string(elementLabel(*element)), elementValue(*element));
    result.items[index].available = true;
    result.items[index].selected = *element == cursorPosition_;
  }
  return result;
}

bool SkinMenuController::changeVisibleItem(std::size_t index, bool increment,
                                           bool analog, bool turbo) {
  const auto element = visibleElement(index);
  if (!element.has_value()) {
    return false;
  }
  cursorPosition_ = *element;
  switch (*element) {
  case 0: {
    const int maximum = roundDownToHundredMillis(
        static_cast<int>(inputs_.lastTimelineMicros / 1'000LL) - 2'000);
    const int change = turbo ? (analog ? 1'000 : 2'500) : 100;
    if (increment) {
      property_.startTimeMillis =
          std::min(property_.startTimeMillis + change, maximum);
      property_.endTimeMillis = std::max(
          property_.endTimeMillis, property_.startTimeMillis + 1'000);
    } else {
      property_.startTimeMillis =
          std::max(property_.startTimeMillis - change, 0);
    }
    break;
  }
  case 1: {
    const int maximum = roundDownToHundredMillis(
        static_cast<int>(inputs_.lastTimelineMicros / 1'000LL) + 1'000);
    const int minimum = roundDownToHundredMillis(property_.startTimeMillis +
                                                  1'000);
    const int change = turbo ? (analog ? 1'000 : 2'500) : 100;
    property_.endTimeMillis = increment
                                 ? std::min(property_.endTimeMillis + change,
                                            maximum)
                                 : std::max(property_.endTimeMillis - change,
                                            minimum);
    break;
  }
  case 2:
    property_.gaugeType =
        (property_.gaugeType + (increment ? 1 : 8)) % 9;
    if (isPopnKeyMode(inputs_.keyMode) && property_.gaugeType >= 3 &&
        property_.startGauge > 100) {
      property_.startGauge = 100;
    }
    break;
  case 3:
    property_.gaugeCategory = static_cast<SkinMenuGaugeCategory>(
        (static_cast<int>(property_.gaugeCategory) + (increment ? 1 : 4)) %
        5);
    property_.startGauge =
        skinMenuGaugeInitial(property_.gaugeCategory, property_.gaugeType);
    break;
  case 4: {
    const int change = turbo ? 10 : 1;
    const int maximum =
        skinMenuGaugeMaximum(property_.gaugeCategory, property_.gaugeType);
    if (increment && turbo && property_.startGauge == 1) {
      property_.startGauge = std::min(change, maximum);
    } else {
      property_.startGauge = std::clamp(
          property_.startGauge + (increment ? change : -change), 1, maximum);
    }
    break;
  }
  case 5: {
    const int change = turbo ? 25 : 1;
    if (increment && turbo && property_.judgeRank == 1) {
      property_.judgeRank = std::min(change, 400);
    } else {
      property_.judgeRank =
          std::clamp(property_.judgeRank + (increment ? change : -change), 1,
                     400);
    }
    break;
  }
  case 6: {
    const int change = analog ? (turbo ? 20 : 1) : (turbo ? 25 : 5);
    property_.total = std::clamp(property_.total + (increment ? change : -change),
                                 10.0, 5'000.0);
    break;
  }
  case 7: {
    const int change = analog ? (turbo ? 10 : 1) : (turbo ? 25 : 5);
    property_.frequencyPercent =
        std::clamp(property_.frequencyPercent + (increment ? change : -change),
                   50, 200);
    break;
  }
  case 8:
    property_.graphType = (property_.graphType + (increment ? 1 : 2)) % 3;
    break;
  case 9: {
    const int options = isPopnKeyMode(inputs_.keyMode) ? 7 : 10;
    property_.random1P =
        (property_.random1P + (increment ? 1 : options - 1)) % options;
    break;
  }
  case 10:
    property_.random2P = (property_.random2P + (increment ? 1 : 9)) % 10;
    break;
  case 11:
    property_.doublePlay = (property_.doublePlay + 1) % 2;
    break;
  default:
    return false;
  }
  synchronizeConfiguration();
  return true;
}

const Configuration &SkinMenuController::configuration() const noexcept {
  return configuration_;
}

const SkinMenuProperty &SkinMenuController::property() const noexcept {
  return property_;
}

bool SkinMenuController::elementAvailable(std::size_t element) const noexcept {
  return element < 10 || (element < 12 && isDoublePlayKeyMode(inputs_.keyMode));
}

std::optional<std::size_t>
SkinMenuController::visibleElement(std::size_t index) const noexcept {
  if (index >= 10) {
    return std::nullopt;
  }
  std::size_t availableIndex = itemOffset_ + index;
  for (std::size_t element = 0; element < 12; ++element) {
    if (elementAvailable(element) && availableIndex-- == 0) {
      return element;
    }
  }
  return std::nullopt;
}

std::size_t SkinMenuController::availableElementCount() const noexcept {
  return isDoublePlayKeyMode(inputs_.keyMode) ? 12 : 10;
}

std::size_t SkinMenuController::maxItemOffset() const noexcept {
  return availableElementCount() - 10;
}

void SkinMenuController::synchronizeConfiguration() noexcept {
  configuration_.startMicros =
      static_cast<long long>(property_.startTimeMillis) * 1'000LL;
  configuration_.endMicros =
      static_cast<long long>(property_.endTimeMillis) * 1'000LL;
  configuration_.startingGaugePercent = property_.startGauge;
  configuration_.playback.percent = property_.frequencyPercent;
  if (property_.gaugeType <= 5) {
    configuration_.gaugeType = gaugeTypeAtIndex(property_.gaugeType);
  }
}

SkinMenuState buildSkinMenuState(const Configuration &configuration,
                                 const SkinMenuInputs &inputs,
                                 float itemScrollPosition) {
  SkinMenuController controller(configuration, inputs);
  controller.setItemScrollPosition(itemScrollPosition);
  return controller.skinMenuState();
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
