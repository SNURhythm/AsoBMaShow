#include "LuaSkinBindingDecoder.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

bool sameType(SkinBindingType left, SkinBindingType right) noexcept {
  if (left.kind != right.kind) {
    return false;
  }
  if (left.kind == SkinBindingKind::IntegerProperty) {
    return left.integerDomain == right.integerDomain;
  }
  if (left.kind == SkinBindingKind::FloatProperty) {
    return left.floatDomain == right.floatDomain;
  }
  return true;
}

template <typename Variant>
bool sameVariant(const Variant &left, const Variant &right) noexcept {
  if (left.index() != right.index()) {
    return false;
  }
  return std::visit(
      [&right](const auto &value) noexcept {
        using Value = std::decay_t<decltype(value)>;
        return value == std::get<Value>(right);
      },
      left);
}

bool supportsNumericFactory(SkinBindingKind kind) noexcept {
  return kind != SkinBindingKind::StringWriter;
}

bool supportsNameFactory(SkinBindingKind kind) noexcept {
  return kind != SkinBindingKind::TimerProperty;
}

LuaCallbackScriptKind scriptKind(SkinBindingKind kind) noexcept {
  switch (kind) {
  case SkinBindingKind::BooleanProperty:
  case SkinBindingKind::IntegerProperty:
  case SkinBindingKind::FloatProperty:
  case SkinBindingKind::StringProperty:
    return LuaCallbackScriptKind::ReturnExpression;
  case SkinBindingKind::TimerProperty:
    return LuaCallbackScriptKind::Timer;
  case SkinBindingKind::FloatWriter:
  case SkinBindingKind::StringWriter:
  case SkinBindingKind::Event:
    return LuaCallbackScriptKind::Statement;
  }
  return LuaCallbackScriptKind::Statement;
}

void hashCombine(std::size_t &seed, std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

bool LuaSkinBindingDecoder::InternKey::operator==(
    const InternKey &other) const noexcept {
  return script == other.script && sameType(type, other.type) &&
         sameVariant(source, other.source);
}

std::size_t LuaSkinBindingDecoder::InternKeyHash::operator()(
    const InternKey &key) const noexcept {
  std::size_t result =
      std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.type.kind));
  if (key.type.kind == SkinBindingKind::IntegerProperty) {
    hashCombine(result, std::hash<std::uint8_t>{}(
                            static_cast<std::uint8_t>(key.type.integerDomain)));
  }
  if (key.type.kind == SkinBindingKind::FloatProperty) {
    hashCombine(result, std::hash<std::uint8_t>{}(
                            static_cast<std::uint8_t>(key.type.floatDomain)));
  }
  hashCombine(result, std::hash<bool>{}(key.script));
  hashCombine(result, key.source.index());
  std::visit(
      [&](const auto &source) noexcept {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, LuaCallbackId>) {
          hashCombine(result, std::hash<std::uint32_t>{}(source.slot));
          hashCombine(result, std::hash<std::uint32_t>{}(source.generation));
        } else {
          hashCombine(result, std::hash<Source>{}(source));
        }
      },
      key.source);
  return result;
}

bool SkinBuiltinBindingCatalogView::contains(
    SkinBindingType type,
    const SkinBuiltinPropertySelector &selector) const noexcept {
  return std::ranges::any_of(entries_, [&](const auto &entry) {
    return sameType(entry.type, type) &&
           sameVariant(entry.selector.value, selector.value);
  });
}

LuaSkinBindingDecodeResult
LuaSkinBindingDecoder::decode(const LuaValueHandle &value,
                              const LuaSkinBindingRequest &request) {
  if (runtime_ == nullptr || request.path.empty()) {
    return {.failure = diagnostic("skin_lua_binding_invalid",
                                  "Lua binding request is incomplete")};
  }

  auto lookedUp = value.lookupBindingSource(request.path);
  if (!lookedUp.source) {
    return {.failure = lookedUp.failure
                           ? std::move(lookedUp.failure)
                           : std::optional<SkinDiagnostic>(
                                 diagnostic("skin_lua_binding_missing",
                                            "Lua binding source is missing"))};
  }

  InternKey internKey;
  try {
    internKey = {.type = request.type,
                 .source = std::move(*lookedUp.source),
                 .script = false};
  } catch (...) {
    return {.failure = diagnostic("skin_lua_allocator_limit_exceeded",
                                  "Lua binding key allocation failed")};
  }
  bool isScript = false;
  std::variant<SkinBuiltinPropertySelector, LuaCallbackId> bindingSource;

  if (const auto *numeric = std::get_if<int>(&internKey.source)) {
    if (!supportsNumericFactory(request.type.kind)) {
      return {.failure = diagnostic(
                  "skin_lua_binding_type_invalid",
                  "Lua binding kind does not accept a numeric selector")};
    }
    bindingSource = SkinBuiltinPropertySelector{*numeric};
  } else if (const auto *callback =
                 std::get_if<LuaCallbackId>(&internKey.source)) {
    if (callback->slot == 0 || callback->generation == 0) {
      return {.failure = diagnostic("skin_lua_callback_invalid",
                                    "Lua callback ID is zero")};
    }
    bindingSource = *callback;
  } else {
    const auto &text = std::get<std::string>(internKey.source);
    try {
      SkinBuiltinPropertySelector named{text};
      if (supportsNameFactory(request.type.kind) &&
          builtins_.contains(request.type, named)) {
        bindingSource = std::move(named);
      } else {
        isScript = true;
        internKey.script = true;
      }
    } catch (...) {
      return {.failure = diagnostic("skin_lua_allocator_limit_exceeded",
                                    "Lua binding source allocation failed")};
    }
  }

  const auto repeated = interned_.find(internKey);
  if (repeated != interned_.end()) {
    return {.id = repeated->second};
  }

  const auto tooMany = [](std::size_t size) {
    return size >= LuaSkinBindingDecoderPolicy::maxBindingsPerKind ||
           size >= std::numeric_limits<std::uint32_t>::max();
  };

  const auto bindingCount = [&]() noexcept {
    switch (request.type.kind) {
    case SkinBindingKind::BooleanProperty:
      return booleanProperties_.size();
    case SkinBindingKind::IntegerProperty:
      return integerProperties_.size();
    case SkinBindingKind::FloatProperty:
      return floatProperties_.size();
    case SkinBindingKind::StringProperty:
      return stringProperties_.size();
    case SkinBindingKind::TimerProperty:
      return timerProperties_.size();
    case SkinBindingKind::FloatWriter:
      return floatWriters_.size();
    case SkinBindingKind::StringWriter:
      return stringWriters_.size();
    case SkinBindingKind::Event:
      return events_.size();
    }
    return LuaSkinBindingDecoderPolicy::maxBindingsPerKind;
  };
  if (tooMany(bindingCount())) {
    return {.failure = diagnostic("skin_lua_binding_limit_exceeded",
                                  "Lua binding kind limit is exhausted")};
  }

  if (isScript) {
    const auto &text = std::get<std::string>(internKey.source);
    auto compiled =
        runtime_->compileCallbackScript(text, scriptKind(request.type.kind));
    if (!compiled.callback) {
      return {.failure = compiled.failure
                             ? std::move(compiled.failure)
                             : std::optional<SkinDiagnostic>(diagnostic(
                                   "skin_lua_callback_script_invalid",
                                   "Lua callback script did not compile"))};
    }
    if (compiled.callback->slot == 0 || compiled.callback->generation == 0) {
      return {.failure = diagnostic("skin_lua_callback_invalid",
                                    "compiled Lua callback ID is zero")};
    }
    bindingSource = *compiled.callback;
  }

  SkinDecodedBindingId decodedId;
  switch (request.type.kind) {
  case SkinBindingKind::BooleanProperty:
    decodedId = SkinBooleanPropertyId{
        static_cast<std::uint32_t>(booleanProperties_.size() + 1)};
    break;
  case SkinBindingKind::IntegerProperty:
    decodedId = SkinIntegerPropertyId{
        static_cast<std::uint32_t>(integerProperties_.size() + 1)};
    break;
  case SkinBindingKind::FloatProperty:
    decodedId = SkinFloatPropertyId{
        static_cast<std::uint32_t>(floatProperties_.size() + 1)};
    break;
  case SkinBindingKind::StringProperty:
    decodedId = SkinStringPropertyId{
        static_cast<std::uint32_t>(stringProperties_.size() + 1)};
    break;
  case SkinBindingKind::TimerProperty:
    decodedId = SkinTimerPropertyId{
        static_cast<std::uint32_t>(timerProperties_.size() + 1)};
    break;
  case SkinBindingKind::FloatWriter:
    decodedId =
        SkinFloatWriterId{static_cast<std::uint32_t>(floatWriters_.size() + 1)};
    break;
  case SkinBindingKind::StringWriter:
    decodedId = SkinStringWriterId{
        static_cast<std::uint32_t>(stringWriters_.size() + 1)};
    break;
  case SkinBindingKind::Event:
    decodedId =
        SkinEventBindingId{static_cast<std::uint32_t>(events_.size() + 1)};
    break;
  }

  auto inserted = interned_.end();
  try {
    auto insertion = interned_.emplace(std::move(internKey), decodedId);
    if (!insertion.second) {
      return {.id = insertion.first->second};
    }
    inserted = insertion.first;
    switch (request.type.kind) {
    case SkinBindingKind::BooleanProperty: {
      const SkinBooleanPropertyId id{
          static_cast<std::uint32_t>(booleanProperties_.size() + 1)};
      booleanProperties_.push_back(
          {.id = id,
           .source = std::move(bindingSource),
           .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::IntegerProperty: {
      const SkinIntegerPropertyId id{
          static_cast<std::uint32_t>(integerProperties_.size() + 1)};
      integerProperties_.push_back(
          {.id = id,
           .domain = request.type.integerDomain,
           .source = std::move(bindingSource),
           .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::FloatProperty: {
      const SkinFloatPropertyId id{
          static_cast<std::uint32_t>(floatProperties_.size() + 1)};
      floatProperties_.push_back({.id = id,
                                  .domain = request.type.floatDomain,
                                  .source = std::move(bindingSource),
                                  .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::StringProperty: {
      const SkinStringPropertyId id{
          static_cast<std::uint32_t>(stringProperties_.size() + 1)};
      stringProperties_.push_back({.id = id,
                                   .source = std::move(bindingSource),
                                   .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::TimerProperty: {
      const SkinTimerPropertyId id{
          static_cast<std::uint32_t>(timerProperties_.size() + 1)};
      timerProperties_.push_back({.id = id,
                                  .source = std::move(bindingSource),
                                  .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::FloatWriter: {
      const SkinFloatWriterId id{
          static_cast<std::uint32_t>(floatWriters_.size() + 1)};
      floatWriters_.push_back({.id = id,
                               .source = std::move(bindingSource),
                               .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::StringWriter: {
      const SkinStringWriterId id{
          static_cast<std::uint32_t>(stringWriters_.size() + 1)};
      stringWriters_.push_back({.id = id,
                                .source = std::move(bindingSource),
                                .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    case SkinBindingKind::Event: {
      const SkinEventBindingId id{
          static_cast<std::uint32_t>(events_.size() + 1)};
      events_.push_back({.id = id,
                         .source = std::move(bindingSource),
                         .authoredOrdinal = request.authoredOrdinal});
      break;
    }
    }
  } catch (...) {
    if (inserted != interned_.end()) {
      interned_.erase(inserted);
    }
    return {.failure = diagnostic("skin_lua_allocator_limit_exceeded",
                                  "Lua binding could not be retained")};
  }

  return {.id = std::move(decodedId)};
}

SkinBindingCatalogView LuaSkinBindingDecoder::bindings() const noexcept {
  return {.booleanProperties = booleanProperties_,
          .integerProperties = integerProperties_,
          .floatProperties = floatProperties_,
          .stringProperties = stringProperties_,
          .timerProperties = timerProperties_,
          .floatWriters = floatWriters_,
          .stringWriters = stringWriters_,
          .events = events_};
}

} // namespace skin

#endif
