#include "MusicSelectSkinStateBridge.h"

#include "BeatorajaBooleanPropertyNames.h"
#include "BeatorajaIntegerPropertyNames.h"
#include "BeatorajaStringPropertyNames.h"
#include "GameplaySkinBuiltinCatalog.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace skin {
namespace {

template <typename Value> SkinPropertyLookup<Value> supported(Value value) {
  return {.value = std::move(value), .supported = true};
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
    const MusicSelectSkinFrame &frame)
    : frame_(&frame) {}

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
  const auto found = frame_->properties.booleans.find(positive);
  const bool value = found != frame_->properties.booleans.end()
                         ? found->second
                         : false;
  return supported(negated ? !value : value);
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
  if (const auto value = numericValue(selector, numeric)) {
    return supported(*value);
  }
  if (const auto value = namedValue(selector, named)) {
    return supported(*value);
  }
  if (!gameplaySkinBuiltinCatalog().contains(floatType(domain), selector)) {
    return {};
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

} // namespace skin
