#pragma once

#include "../audio/PlaybackRate.h"
#include "../scene/play/RhythmState.h"

#include <array>
#include <cstddef>
#include <cstdint>
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
  long long lastTimelineMicros = 0;
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

// PracticeConfiguration.PracticeProperty retains values that are not present
// in Aso's persisted practice Configuration.  This controller deliberately
// keeps the source menu value domain intact; applying all values to an
// attempt is a separate lifecycle concern.
enum class SkinMenuGaugeCategory : std::uint8_t {
  FiveKeys,
  SevenKeys,
  Pms,
  Keyboard,
  Lr2,
};

struct SkinMenuProperty {
  int startTimeMillis = 0;
  int endTimeMillis = 10'000;
  SkinMenuGaugeCategory gaugeCategory = SkinMenuGaugeCategory::SevenKeys;
  int gaugeType = 2;
  int startGauge = 20;
  int random1P = 0;
  int random2P = 0;
  int doublePlay = 0;
  int judgeRank = 100;
  int frequencyPercent = 100;
  double total = 0.0;
  int graphType = 0;

  bool operator==(const SkinMenuProperty &) const = default;
};

class SkinMenuController {
public:
  SkinMenuController(Configuration, SkinMenuInputs);

  void setItemScrollPosition(float) noexcept;
  [[nodiscard]] float itemScrollPosition() const noexcept;
  [[nodiscard]] SkinMenuState skinMenuState() const;
  [[nodiscard]] bool changeVisibleItem(std::size_t index, bool increment,
                                       bool analog = false,
                                       bool turbo = false);
  [[nodiscard]] const Configuration &configuration() const noexcept;
  [[nodiscard]] const SkinMenuProperty &property() const noexcept;

private:
  [[nodiscard]] bool elementAvailable(std::size_t) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  visibleElement(std::size_t) const noexcept;
  [[nodiscard]] std::size_t availableElementCount() const noexcept;
  [[nodiscard]] std::size_t maxItemOffset() const noexcept;
  void synchronizeConfiguration() noexcept;

  Configuration configuration_;
  SkinMenuInputs inputs_;
  SkinMenuProperty property_;
  std::size_t cursorPosition_ = 0;
  std::size_t itemOffset_ = 0;
};

[[nodiscard]] SkinMenuState buildSkinMenuState(const Configuration &,
                                                const SkinMenuInputs &,
                                                float itemScrollPosition = 0.0F);

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
