#include "MusicSelectSkinStateBridge.h"

#include "BeatorajaBooleanPropertyNames.h"
#include "BeatorajaIntegerPropertyNames.h"
#include "BeatorajaStringPropertyNames.h"
#include "GameplaySkinBuiltinCatalog.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace skin {
namespace {

template <typename Value> SkinPropertyLookup<Value> supported(Value value) {
  return {.value = std::move(value), .supported = true};
}

std::optional<int> namedFloatPropertySelector(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, int>, 71> direct = {{
      {"musicselect_position", 1}, {"lanecover", 4},
      {"lanecover2", 5},            {"music_progress", 6},
      {"skinselect_position", 7},  {"ranking_position", 8},
      {"mastervolume", 17},         {"keyvolume", 18},
      {"bgmvolume", 19},            {"practice_position", 20},
      {"music_progress_bar", 101},  {"load_progress", 102},
      {"level", 103},                {"level_beginner", 105},
      {"level_normal", 106},         {"level_hyper", 107},
      {"level_another", 108},        {"level_insane", 109},
      {"scorerate", 110},           {"scorerate_final", 111},
      {"bestscorerate_now", 112},   {"bestscorerate", 113},
      {"targetscorerate_now", 114}, {"targetscorerate", 115},
      {"rate_pgreat", 140},          {"rate_great", 141},
      {"rate_good", 142},            {"rate_bad", 143},
      {"rate_poor", 144},            {"rate_maxcombo", 145},
      {"rate_exscore", 147},
      {"score_rate", 1102},         {"total_rate", 1115},
      {"score_rate2", 155},         {"duration_average", 372},
      {"timing_average", 374},       {"timign_stddev", 376},
      {"perfect_rate", 85},          {"great_rate", 86},
      {"good_rate", 87},             {"bad_rate", 88},
      {"poor_rate", 89},             {"rival_perfect_rate", 285},
      {"rival_great_rate", 286},     {"rival_good_rate", 287},
      {"rival_bad_rate", 288},       {"rival_poor_rate", 289},
      {"best_rate", 183},            {"rival_rate", 122},
      {"target_rate", 135},          {"target_rate2", 157},
      {"hispeed", 310},              {"groovegauge_1p", 1107},
      {"chart_averagedensity", 367}, {"chart_enddensity", 362},
      {"chart_peakdensity", 360},    {"chart_totalgauge", 368},
      {"loading_progress", 165},     {"ir_totalclearrate", 227},
      {"ir_totalfullcomborate", 229},
      {"ir_player_noplay_rate", 203}, {"ir_player_failed_rate", 211},
      {"ir_player_assist_rate", 205},
      {"ir_player_lightassist_rate", 207},
      {"ir_player_easy_rate", 213}, {"ir_player_normal_rate", 215},
      {"ir_player_hard_rate", 217}, {"ir_player_exhard_rate", 209},
      {"ir_player_fullcombo_rate", 219},
      {"ir_player_perfect_rate", 223},
      {"ir_player_max_rate", 225},
  }};
  for (const auto &[candidate, id] : direct) {
    if (name == candidate) {
      return id;
    }
  }
  return std::nullopt;
}

SkinBindingType integerType(SkinIntegerPropertyDomain domain) {
  return {.kind = SkinBindingKind::IntegerProperty,
          .integerDomain = domain};
}

SkinBindingType floatType(SkinFloatPropertyDomain domain) {
  return {.kind = SkinBindingKind::FloatProperty, .floatDomain = domain};
}

std::optional<int> integerName(std::string_view name,
                               SkinIntegerPropertyDomain domain) {
  return domain == SkinIntegerPropertyDomain::IntegerValue
             ? beatorajaIntegerValuePropertySelector(name)
             : beatorajaImageIndexPropertySelector(name);
}

template <typename Value>
std::optional<Value> numericValue(const SkinBuiltinPropertySelector &selector,
                                  const std::map<int, Value> &values) {
  const auto *id = std::get_if<int>(&selector.value);
  if (!id) {
    return std::nullopt;
  }
  const auto found = values.find(*id);
  return found == values.end() ? std::nullopt
                               : std::optional<Value>(found->second);
}

template <typename Value>
std::optional<Value> namedValue(
    const SkinBuiltinPropertySelector &selector,
    const std::map<std::string, Value, std::less<>> &values) {
  const auto *name = std::get_if<std::string>(&selector.value);
  if (!name) {
    return std::nullopt;
  }
  const auto found = values.find(*name);
  return found == values.end() ? std::nullopt
                               : std::optional<Value>(found->second);
}

} // namespace

MusicSelectSkinStateBridge::MusicSelectSkinStateBridge(
    const MusicSelectSkinFrame &frame, MusicSelectSkinActionSink actionSink)
    : frame_(&frame), actionSink_(std::move(actionSink)) {}

MusicSelectSkinStateBridge::MusicSelectSkinStateBridge(
    const MusicSelectSkinFrame &frame,
    std::map<int, std::int64_t> &persistentCustomTimerValues,
    const std::set<int> &activeCustomTimerIds,
    MusicSelectSkinActionSink actionSink)
    : frame_(&frame),
      persistentCustomTimerValues_(&persistentCustomTimerValues),
      activeCustomTimerIds_(&activeCustomTimerIds),
      actionSink_(std::move(actionSink)) {}

std::uint64_t MusicSelectSkinStateBridge::frameSerial() const noexcept {
  return frame_->serial;
}

SkinPropertyLookup<bool> MusicSelectSkinStateBridge::booleanProperty(
    const SkinBuiltinPropertySelector &selector) {
  std::optional<int> id;
  if (const auto *numeric = std::get_if<int>(&selector.value)) {
    id = *numeric;
  } else if (const auto *name = std::get_if<std::string>(&selector.value)) {
    if (const auto direct = frame_->properties.namedBooleans.find(*name);
        direct != frame_->properties.namedBooleans.end()) {
      return supported(direct->second);
    }
    id = beatorajaBooleanPropertySelector(*name);
  }
  if (!id || !gameplaySkinBuiltinCatalog().contains(
                 {.kind = SkinBindingKind::BooleanProperty}, selector)) {
    return {};
  }
  const bool negated = *id < 0;
  const int positive = negated ? -*id : *id;
  if (publishedSongResources_) {
    std::optional<bool> available;
    switch (positive) {
    case 190:
    case 191: available = publishedSongResources_->stageFile; break;
    case 192:
    case 193: available = publishedSongResources_->banner; break;
    case 194:
    case 195: available = publishedSongResources_->backBmp; break;
    default: break;
    }
    if (available) {
      const bool artworkProperty = positive == 191 || positive == 193 ||
                                   positive == 195;
      const bool value = artworkProperty ? *available : !*available;
      return supported(negated ? !value : value);
    }
  }
  const auto found = frame_->properties.booleans.find(positive);
  const bool value = found != frame_->properties.booleans.end()
                         ? found->second
                         : false;
  return supported(negated ? !value : value);
}

void MusicSelectSkinStateBridge::setPublishedSongResources(
    MusicSelectPublishedSongResources resources) noexcept {
  publishedSongResources_ = resources;
}

SkinPropertyLookup<std::int64_t> MusicSelectSkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector,
    SkinIntegerPropertyDomain domain) {
  const auto &numeric = domain == SkinIntegerPropertyDomain::IntegerValue
                            ? frame_->properties.integers
                            : frame_->properties.imageIndexes;
  const auto &named = domain == SkinIntegerPropertyDomain::IntegerValue
                          ? frame_->properties.namedIntegers
                          : frame_->properties.namedImageIndexes;
  if (const auto value = numericValue(selector, numeric)) {
    return supported(*value);
  }
  if (const auto value = namedValue(selector, named)) {
    return supported(*value);
  }
  SkinBuiltinPropertySelector normalized = selector;
  if (const auto *name = std::get_if<std::string>(&selector.value)) {
    const auto id = integerName(*name, domain);
    if (!id) {
      return {};
    }
    normalized.value = *id;
    if (const auto found = numeric.find(*id); found != numeric.end()) {
      return supported(found->second);
    }
  }
  if (!gameplaySkinBuiltinCatalog().contains(integerType(domain), selector)) {
    return {};
  }
  return supported<std::int64_t>(std::numeric_limits<std::int32_t>::min());
}

SkinPropertyLookup<double> MusicSelectSkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &selector,
    SkinFloatPropertyDomain domain) {
  const auto &numeric = domain == SkinFloatPropertyDomain::Rate
                            ? frame_->properties.rates
                            : frame_->properties.floats;
  const auto &named = domain == SkinFloatPropertyDomain::Rate
                          ? frame_->properties.namedRates
                          : frame_->properties.namedFloats;
  if (domain == SkinFloatPropertyDomain::Rate) {
    if (const auto *id = std::get_if<int>(&selector.value)) {
      if (const auto override = floatOverrides_.find(*id);
          override != floatOverrides_.end()) {
        return supported(override->second);
      }
    }
  }
  if (const auto value = numericValue(selector, numeric)) {
    return supported(*value);
  }
  if (const auto value = namedValue(selector, named)) {
    return supported(*value);
  }
  if (!gameplaySkinBuiltinCatalog().contains(floatType(domain), selector)) {
    return {};
  }
  if (const auto *name = std::get_if<std::string>(&selector.value)) {
    if (const auto id = namedFloatPropertySelector(*name)) {
      if (domain == SkinFloatPropertyDomain::Rate) {
        return floatProperty({.value = *id}, domain);
      }
      if (const auto value = numericValue({.value = *id}, numeric)) {
        return supported(*value);
      }
    }
  }
  if (domain == SkinFloatPropertyDomain::FloatValue &&
      gameplaySkinBuiltinCatalog().contains(
          floatType(SkinFloatPropertyDomain::Rate), selector)) {
    return floatProperty(selector, SkinFloatPropertyDomain::Rate);
  }
  return supported<double>(std::numeric_limits<float>::denorm_min());
}

SkinPropertyLookup<std::string_view>
MusicSelectSkinStateBridge::stringProperty(
    const SkinBuiltinPropertySelector &selector) {
  if (const auto *id = std::get_if<int>(&selector.value)) {
    if (const auto found = frame_->properties.strings.find(*id);
        found != frame_->properties.strings.end()) {
      return supported<std::string_view>(found->second);
    }
  } else if (const auto *name = std::get_if<std::string>(&selector.value)) {
    if (const auto found = frame_->properties.namedStrings.find(*name);
        found != frame_->properties.namedStrings.end()) {
      return supported<std::string_view>(found->second);
    }
    if (const auto id = beatorajaStringPropertySelector(*name)) {
      if (const auto found = frame_->properties.strings.find(*id);
          found != frame_->properties.strings.end()) {
        return supported<std::string_view>(found->second);
      }
    }
  }
  if (!gameplaySkinBuiltinCatalog().contains(
          {.kind = SkinBindingKind::StringProperty}, selector)) {
    return {};
  }
  return supported<std::string_view>({});
}

SkinPropertyLookup<SkinRuntimeOffset>
MusicSelectSkinStateBridge::offsetProperty(int id) {
  if (id < 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
    return {};
  }
  return supported(SkinRuntimeOffset{});
}

std::int64_t MusicSelectSkinStateBridge::timerProperty(
    const SkinBuiltinPropertySelector &selector) {
  if (const auto *id = std::get_if<int>(&selector.value)) {
    if (persistentCustomTimerValues_ != nullptr) {
      if (const auto custom = persistentCustomTimerValues_->find(*id);
          custom != persistentCustomTimerValues_->end()) {
        return custom->second;
      }
    }
    if (const auto custom = customTimerValues_.find(*id);
        custom != customTimerValues_.end()) {
      return custom->second;
    }
  }
  if (const auto value = numericValue(selector, frame_->properties.timers)) {
    return *value;
  }
  if (const auto value = namedValue(selector, frame_->properties.namedTimers)) {
    return *value;
  }
  if (!gameplaySkinBuiltinCatalog().contains(
          {.kind = SkinBindingKind::TimerProperty}, selector)) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return std::numeric_limits<std::int64_t>::min();
}

bool MusicSelectSkinStateBridge::setTimerProperty(int id,
                                                  std::int64_t value) {
  if (id < 10'000 || id > 19'999) return false;
  if (activeCustomTimerIds_ != nullptr &&
      activeCustomTimerIds_->contains(id)) {
    return true;
  }
  if (persistentCustomTimerValues_ != nullptr) {
    persistentCustomTimerValues_->insert_or_assign(id, value);
  } else {
    customTimerValues_.insert_or_assign(id, value);
  }
  return true;
}

bool MusicSelectSkinStateBridge::setFloatProperty(int id, double value) {
  if ((id < 17 || id > 19) || !actionSink_.floatWriter) return false;
  try {
    floatOverrides_.insert_or_assign(id, value);
    actionSink_.floatWriter(id, value);
    return true;
  } catch (...) {
    return false;
  }
}

void MusicSelectSkinStateBridge::setCustomTimer(int id, std::int64_t value) {
  if (persistentCustomTimerValues_ != nullptr) {
    persistentCustomTimerValues_->insert_or_assign(id, value);
  } else {
    customTimerValues_.insert_or_assign(id, value);
  }
}

std::span<const SkinProjectedNoteView>
MusicSelectSkinStateBridge::projectedNotes() const noexcept {
  return {};
}

std::span<const SkinProjectedLongNoteView>
MusicSelectSkinStateBridge::projectedLongNotes() const noexcept {
  return {};
}

std::span<const SkinProjectedLineView>
MusicSelectSkinStateBridge::projectedLines() const noexcept {
  return {};
}

SkinGaugeStateView MusicSelectSkinStateBridge::gaugeState() const noexcept {
  return {};
}

SkinJudgeStateView
MusicSelectSkinStateBridge::judgeState(int) const noexcept {
  return {};
}

SkinNoteExpansionStateView
MusicSelectSkinStateBridge::noteExpansionState() const noexcept {
  return {};
}

SkinGameplayGraphStateView
MusicSelectSkinStateBridge::gameplayGraphState() const noexcept {
  SkinGameplayGraphStateView result;
  if (frame_ == nullptr || frame_->gameplayGraph.chart == nullptr) {
    return result;
  }
  const auto &chart = *frame_->gameplayGraph.chart;
  result.normalDistribution = chart.normalDistribution;
  result.bpmSeries = chart.bpmSeries;
  result.mainBpm = chart.mainBpm;
  result.minimumBpm = chart.minimumBpm;
  result.maximumBpm = chart.maximumBpm;
  return result;
}

} // namespace skin
