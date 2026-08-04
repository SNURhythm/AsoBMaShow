#include "SkinModelValidator.h"

#include "../LuaGameplaySkinFeature.h"
#include "LuaSkinTableDecoder.h"
#include "NumericGlyphAtlas.h"
#include "SkinNoteLaneGeometryNormalization.h"
#include "SkinNoteLineNormalization.h"
#include "SkinNoteNormalization.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

SkinDiagnostic validationDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

template <typename Binding>
bool collectBindingIds(const std::vector<Binding> &bindings,
                       std::set<std::uint32_t> &result) {
  for (const auto &binding : bindings) {
    if (binding.id.value == 0 || !result.insert(binding.id.value).second) {
      return false;
    }
  }
  return true;
}

template <typename Domain>
bool hasDomain(const std::map<std::uint32_t, Domain> &domains, std::uint32_t id,
               Domain expected) {
  const auto found = domains.find(id);
  return found != domains.end() && found->second == expected;
}

template <typename Binding>
bool validBindingSource(const Binding &binding, SkinBindingType type,
                        const SkinBindingValidationContext &context) {
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding.source)) {
    return context.builtins.contains(type, *builtin);
  }
  return context.callbacks.contains(std::get<LuaCallbackId>(binding.source));
}

template <typename Binding>
bool collectValidBindingIds(const std::vector<Binding> &bindings,
                            SkinBindingType type,
                            const SkinBindingValidationContext &context,
                            std::set<std::uint32_t> &result) {
  bool allValid = true;
  for (const auto &binding : bindings) {
    if (validBindingSource(binding, type, context)) {
      result.insert(binding.id.value);
    } else {
      allValid = false;
    }
  }
  return allValid;
}

bool validSprite(const SkinSpriteFrames &sprite,
                 const std::set<SkinResourceId> &imageResources,
                 const std::set<std::uint32_t> &timers) {
  return sprite.resource != 0 && imageResources.contains(sprite.resource) &&
         !sprite.frames.empty() &&
         (!sprite.timer || timers.contains(sprite.timer->value));
}

bool validDigits(const SkinDigitSpriteSet &digits, NumericGlyphAtlasKind kind,
                 const std::set<SkinResourceId> &imageResources,
                 const std::set<std::uint32_t> &timers) {
  return validSprite(digits.positive, imageResources, timers) &&
         (!digits.negative ||
          validSprite(*digits.negative, imageResources, timers)) &&
         validateNumericGlyphAtlas(digits, kind) ==
             NumericGlyphAtlasError::None;
}

bool validNoteVisual(const SkinNoteVisual &visual,
                     const std::set<SkinResourceId> &imageResources,
                     const std::set<std::uint32_t> &timers) {
  if (const auto *sprite = std::get_if<SkinSpriteFrames>(&visual)) {
    return validSprite(*sprite, imageResources, timers);
  }
  return true;
}

struct ValidationContext {
  const std::set<SkinResourceId> &imageResources;
  const std::set<SkinResourceId> &fontResources;
  const std::set<SkinObjectId> &objects;
  const std::set<std::uint32_t> &booleans;
  const std::map<std::uint32_t, SkinIntegerPropertyDomain> &integers;
  const std::map<std::uint32_t, SkinFloatPropertyDomain> &floats;
  const std::set<std::uint32_t> &strings;
  const std::set<std::uint32_t> &timers;
  const std::set<std::uint32_t> &floatWriters;
  const std::set<std::uint32_t> &stringWriters;
  const std::set<std::uint32_t> &events;
};

bool validDestination(const SkinDestinationBody &destination,
                      const ValidationContext &context) {
  if ((destination.timer &&
       !context.timers.contains(destination.timer->value)) ||
      (destination.drawCondition &&
       !context.booleans.contains(destination.drawCondition->value))) {
    return false;
  }
  return std::ranges::all_of(destination.conditions, [&](const auto &entry) {
    const auto *condition = std::get_if<SkinBooleanPropertyId>(&entry);
    return condition == nullptr || context.booleans.contains(condition->value);
  });
}

SkinAuthoredNoteVisualSlots noteVisualSlots(const SkinNoteObject &object,
                                            SkinNoteVisualKind kind,
                                            bool &complete) {
  SkinAuthoredNoteVisualSlots slots;
  slots.reserve(object.lanes.size());
  for (const auto &lane : object.lanes) {
    const auto found = lane.visuals.find(kind);
    if (found == lane.visuals.end()) {
      complete = false;
      slots.emplace_back(std::nullopt);
      continue;
    }
    if (const auto *sprite = std::get_if<SkinSpriteFrames>(&found->second)) {
      slots.emplace_back(*sprite);
      continue;
    }
    const auto &synthesized =
        std::get<SkinSynthesizedNoteVisual>(found->second);
    if (synthesized.kind != kind) {
      complete = false;
    }
    slots.emplace_back(std::nullopt);
  }
  return slots;
}

bool equalRect(const SkinAuthoredRect &left, const SkinAuthoredRect &right) {
  return left.x == right.x && left.y == right.y && left.width == right.width &&
         left.height == right.height;
}

bool validNoteObject(const SkinNoteObject &object,
                     const ValidationContext &context) {
  if (object.lanes.empty()) {
    return false;
  }
  bool complete = true;
  for (std::size_t index = 0; index < object.lanes.size(); ++index) {
    if (object.lanes[index].authoredLane != static_cast<int>(index)) {
      complete = false;
    }
  }

  SkinNoteNormalizationInput visualInput;
  visualInput.note =
      noteVisualSlots(object, SkinNoteVisualKind::Normal, complete);
  visualInput.mine =
      noteVisualSlots(object, SkinNoteVisualKind::Mine, complete);
  visualInput.lnEnd =
      noteVisualSlots(object, SkinNoteVisualKind::LnEnd, complete);
  visualInput.lnStart =
      noteVisualSlots(object, SkinNoteVisualKind::LnStart, complete);
  visualInput.lnBody =
      noteVisualSlots(object, SkinNoteVisualKind::LnBodyInactive, complete);
  visualInput.lnBodyActive =
      noteVisualSlots(object, SkinNoteVisualKind::LnBodyActive, complete);
  visualInput.hcnEnd =
      noteVisualSlots(object, SkinNoteVisualKind::HcnEnd, complete);
  visualInput.hcnStart =
      noteVisualSlots(object, SkinNoteVisualKind::HcnStart, complete);
  visualInput.hcnBody =
      noteVisualSlots(object, SkinNoteVisualKind::HcnBodyInactive, complete);
  visualInput.hcnBodyActive =
      noteVisualSlots(object, SkinNoteVisualKind::HcnBodyActive, complete);
  visualInput.hcnBodyMiss =
      noteVisualSlots(object, SkinNoteVisualKind::HcnDamage, complete);
  visualInput.hcnBodyReactive =
      noteVisualSlots(object, SkinNoteVisualKind::HcnReactive, complete);
  (void)noteVisualSlots(object, SkinNoteVisualKind::Hidden, complete);
  (void)noteVisualSlots(object, SkinNoteVisualKind::Processed, complete);
  if (!complete || !normalizeSkinNote(visualInput).note) {
    return false;
  }

  SkinNoteLaneGeometryNormalizationInput geometryInput;
  geometryInput.expansionRatePercent = object.expansionRatePercent;
  geometryInput.normalFirstFrameHeights.reserve(object.lanes.size());
  geometryInput.laneDestinations.reserve(object.lanes.size());
  const std::optional<double> authoredSecondaryDestinationY =
      object.lanes.front().secondaryDestinationY;
  for (std::size_t index = 0; index < object.lanes.size(); ++index) {
    const auto &lane = object.lanes[index];
    geometryInput.normalFirstFrameHeights.push_back(lane.authoredNoteHeight);
    geometryInput.laneDestinations.push_back(lane.laneDestination);
    if (lane.secondaryDestinationY != authoredSecondaryDestinationY) {
      return false;
    }
  }
  std::optional<int> secondaryDestinationY;
  if (authoredSecondaryDestinationY) {
    const double value = *authoredSecondaryDestinationY;
    if (!std::isfinite(value) || std::trunc(value) != value ||
        value < static_cast<double>(std::numeric_limits<int>::min()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
      return false;
    }
    secondaryDestinationY = static_cast<int>(value);
  }
  geometryInput.secondaryDestinationY = secondaryDestinationY;
  if (!normalizeSkinNoteLaneGeometry(geometryInput).geometry) {
    return false;
  }

  if (object.lines.size() % 4 != 0) {
    return false;
  }
  const std::size_t groupCount = object.lines.size() / 4;
  SkinNoteLineNormalizationInput lineInput;
  lineInput.group.resize(groupCount);
  lineInput.bpm.resize(groupCount);
  lineInput.stop.resize(groupCount);
  lineInput.time.resize(groupCount);
  std::array<SkinAuthoredNoteLineSlots *, 4> arrays{
      &lineInput.group, &lineInput.bpm, &lineInput.stop, &lineInput.time};
  constexpr std::array kinds{SkinNoteLineKind::Group, SkinNoteLineKind::Bpm,
                             SkinNoteLineKind::Stop, SkinNoteLineKind::Time};
  for (std::size_t kindIndex = 0; kindIndex < kinds.size(); ++kindIndex) {
    for (std::size_t group = 0; group < groupCount; ++group) {
      const auto &line = object.lines[kindIndex * groupCount + group];
      if (line.kind != kinds[kindIndex]) {
        return false;
      }
      (*arrays[kindIndex])[group] = SkinAuthoredNoteLineSlot{
          .image = line.sprite,
          .destination = line.destination,
      };
    }
  }
  auto normalizedLines = normalizeSkinNoteLines(lineInput);
  if (!normalizedLines.lines ||
      normalizedLines.lines->lines.size() != object.lines.size()) {
    return false;
  }
  for (std::size_t index = 0; index < object.lines.size(); ++index) {
    const auto &line = object.lines[index];
    const std::size_t group = groupCount == 0 ? 0 : index % groupCount;
    if (groupCount != 0 &&
        !equalRect(line.laneGroupDestination,
                   normalizedLines.lines->groups[group].laneRect)) {
      return false;
    }
    if ((line.sprite &&
         !validSprite(*line.sprite, context.imageResources, context.timers)) ||
        (line.destination && !validDestination(*line.destination, context))) {
      return false;
    }
  }

  return std::ranges::all_of(object.lanes, [&](const auto &lane) {
    return std::ranges::all_of(lane.visuals, [&](const auto &entry) {
      return validNoteVisual(entry.second, context.imageResources,
                             context.timers);
    });
  });
}

bool validPayload(const SkinObjectPayload &payload,
                  const ValidationContext &context) {
  return std::visit(
      [&](const auto &object) {
        using T = std::decay_t<decltype(object)>;
        if constexpr (std::is_same_v<T, SkinImageObject>) {
          return !object.orderedStates.empty() &&
                 std::ranges::all_of(object.orderedStates,
                                     [&](const auto &state) {
                                       return validSprite(
                                           state, context.imageResources,
                                           context.timers);
                                     }) &&
                 (!object.stateIndex ||
                  hasDomain(context.integers, object.stateIndex->value,
                            SkinIntegerPropertyDomain::ImageIndex)) &&
                 (!object.clickEvent ||
                  context.events.contains(object.clickEvent->value));
        } else if constexpr (std::is_same_v<T, SkinNumberObject>) {
          const NumericGlyphFormat format{
              .integerDigits = object.digitCount,
              .zeroPadding = object.zeroPadding,
              .perDigitOffsets = object.perDigitOffsets,
          };
          return validDigits(object.digits, NumericGlyphAtlasKind::Number,
                             context.imageResources, context.timers) &&
                 validateNumericGlyphFormat(
                     format, NumericGlyphAtlasKind::Number,
                     object.digits.glyphsPerAnimationFrame) ==
                     NumericGlyphAtlasError::None &&
                 hasDomain(context.integers, object.value.value,
                           SkinIntegerPropertyDomain::IntegerValue);
        } else if constexpr (std::is_same_v<T, SkinFloatObject>) {
          const NumericGlyphFormat format{
              .integerDigits = object.integerDigits,
              .fractionalDigits = object.fractionalDigits,
              .zeroPadding = object.zeroPadding,
              .signVisible = object.signVisible,
              .gain = object.gain,
              .perDigitOffsets = object.perDigitOffsets,
          };
          return validDigits(object.digits, NumericGlyphAtlasKind::Float,
                             context.imageResources, context.timers) &&
                 validateNumericGlyphFormat(
                     format, NumericGlyphAtlasKind::Float,
                     object.digits.glyphsPerAnimationFrame) ==
                     NumericGlyphAtlasError::None &&
                 hasDomain(context.floats, object.value.value,
                           SkinFloatPropertyDomain::FloatValue);
        } else if constexpr (std::is_same_v<T, SkinTextObject>) {
          return object.font != 0 &&
                 context.fontResources.contains(object.font) &&
                 (!object.value ||
                  context.strings.contains(object.value->value)) &&
                 (!object.writer ||
                  context.stringWriters.contains(object.writer->value));
        } else if constexpr (std::is_same_v<T, SkinSliderObject>) {
          const bool valueValid = std::visit(
              [&](const auto &source) {
                using V = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<V, SkinFloatPropertyId>) {
                  return hasDomain(context.floats, source.value,
                                   SkinFloatPropertyDomain::Rate);
                } else {
                  return hasDomain(context.integers, source.value.value,
                                   SkinIntegerPropertyDomain::IntegerValue) &&
                         source.minimum <= source.maximum;
                }
              },
              object.value);
          return validSprite(object.knob, context.imageResources,
                             context.timers) &&
                 object.direction <= 3 && valueValid &&
                 (!object.writer ||
                  context.floatWriters.contains(object.writer->value));
        } else if constexpr (std::is_same_v<T, SkinGraphObject>) {
          const bool valueValid = std::visit(
              [&](const auto &source) {
                using V = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<V, SkinFloatPropertyId>) {
                  return hasDomain(context.floats, source.value,
                                   SkinFloatPropertyDomain::Rate);
                } else {
                  return hasDomain(context.integers, source.value.value,
                                   SkinIntegerPropertyDomain::IntegerValue) &&
                         source.minimum <= source.maximum;
                }
              },
              object.value);
          return validSprite(object.fill, context.imageResources,
                             context.timers) &&
                 object.direction >= 0 && object.direction <= 1 && valueValid;
        } else if constexpr (std::is_same_v<T, SkinGaugeObject>) {
          return object.orderedNodes.size() == 36 &&
                 std::ranges::all_of(
                     object.orderedNodes, [&](const auto &node) {
                       return validSprite(node, context.imageResources,
                                          context.timers);
                     });
        } else if constexpr (std::is_same_v<T, SkinNoteObject>) {
          return validNoteObject(object, context);
        } else if constexpr (std::is_same_v<T, SkinCoverObject>) {
          return validSprite(object.sprite, context.imageResources,
                             context.timers);
        } else if constexpr (std::is_same_v<T, SkinJudgeObject>) {
          return std::ranges::all_of(object.grades, [&](const auto &grade) {
            const auto nestedValid = [&](const auto &nested) {
              return !nested ||
                     (context.objects.contains(nested->object) &&
                      validDestination(nested->destination, context));
            };
            return nestedValid(grade.image) && nestedValid(grade.detailNumber);
          });
        } else {
          return true;
        }
      },
      payload);
}

enum class UnsupportedInteraction : std::uint8_t { None, Image, Text };

UnsupportedInteraction
unsupportedInteraction(const SkinObjectPayload &payload) noexcept {
  if (const auto *image = std::get_if<SkinImageObject>(&payload)) {
    return image->clickEvent || image->clickMode != 0
               ? UnsupportedInteraction::Image
               : UnsupportedInteraction::None;
  }
  if (const auto *text = std::get_if<SkinTextObject>(&payload)) {
    return text->writer || text->editable ? UnsupportedInteraction::Text
                                          : UnsupportedInteraction::None;
  }
  return UnsupportedInteraction::None;
}

} // namespace

SkinModelValidationResult SkinModelValidator::validate(
    BeatorajaSkinModel model,
    SkinBindingValidationContext bindingContext) const {
  SkinModelValidationResult result;
  if (model.header.type != 0) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_type_unsupported",
        "Lua gameplay skins require Beatoraja play type 0 (7 keys)"));
    return result;
  }
  if (model.header.width <= 0 || model.header.height <= 0 ||
      model.header.width > LuaSkinTableDecoderPolicy::maxAuthoredDimension ||
      model.header.height > LuaSkinTableDecoderPolicy::maxAuthoredDimension) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_canvas_invalid",
        "Lua skin authored canvas dimensions are outside the fixed limit"));
    return result;
  }

  std::set<std::uint32_t> booleanIds;
  std::set<std::uint32_t> integerIds;
  std::set<std::uint32_t> floatIds;
  std::set<std::uint32_t> stringIds;
  std::set<std::uint32_t> timerIds;
  std::set<std::uint32_t> floatWriterIds;
  std::set<std::uint32_t> stringWriterIds;
  std::set<std::uint32_t> eventIds;
  if (!collectBindingIds(model.booleanProperties, booleanIds) ||
      !collectBindingIds(model.integerProperties, integerIds) ||
      !collectBindingIds(model.floatProperties, floatIds) ||
      !collectBindingIds(model.stringProperties, stringIds) ||
      !collectBindingIds(model.timerProperties, timerIds) ||
      !collectBindingIds(model.floatWriters, floatWriterIds) ||
      !collectBindingIds(model.stringWriters, stringWriterIds) ||
      !collectBindingIds(model.events, eventIds) ||
      std::ranges::any_of(
          model.integerProperties,
          [](const auto &binding) {
            return binding.domain != SkinIntegerPropertyDomain::IntegerValue &&
                   binding.domain != SkinIntegerPropertyDomain::ImageIndex;
          }) ||
      std::ranges::any_of(model.floatProperties, [](const auto &binding) {
        return binding.domain != SkinFloatPropertyDomain::Rate &&
               binding.domain != SkinFloatPropertyDomain::FloatValue;
      })) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_binding_identity_invalid",
        "Lua skin binding identities must be nonzero, unique, and typed"));
    return result;
  }

  std::set<SkinResourceId> resourceIds;
  std::set<SkinResourceId> imageResourceIds;
  std::set<SkinResourceId> fontResourceIds;
  std::set<SkinObjectId> objectIds;
  std::map<std::string, SkinResourceId, std::less<>> resourceNames;
  std::map<std::string, SkinObjectId, std::less<>> objectNames;

  for (const auto &definition : model.resources) {
    const bool valid = std::visit(
        [&](const auto &resource) {
          using T = std::decay_t<decltype(resource)>;
          if (resource.id == 0 || resource.authoredName.empty() ||
              !resourceIds.insert(resource.id).second ||
              !resourceNames.emplace(resource.authoredName, resource.id)
                   .second) {
            return false;
          }
          if constexpr (std::is_same_v<T, SkinImageResource>) {
            return imageResourceIds.insert(resource.id).second;
          } else {
            return fontResourceIds.insert(resource.id).second;
          }
        },
        definition);
    if (!valid) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_resource_identity_invalid",
          "Lua skin resource identities must be nonzero and unique"));
      return result;
    }
  }

  for (const auto &object : model.objects) {
    if (object.id == 0 || object.authoredName.empty() ||
        !objectIds.insert(object.id).second ||
        !objectNames.emplace(object.authoredName, object.id).second) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_object_identity_invalid",
          "Lua skin object identities must be nonzero and unique"));
      return result;
    }
  }

  std::set<std::uint32_t> validBooleanIds;
  std::set<std::uint32_t> validIntegerIds;
  std::set<std::uint32_t> validFloatIds;
  std::set<std::uint32_t> validStringIds;
  std::set<std::uint32_t> validTimerIds;
  std::set<std::uint32_t> validFloatWriterIds;
  std::set<std::uint32_t> validStringWriterIds;
  std::set<std::uint32_t> validEventIds;
  bool allBindingSourcesValid = true;
  allBindingSourcesValid &= collectValidBindingIds(
      model.booleanProperties, {.kind = SkinBindingKind::BooleanProperty},
      bindingContext, validBooleanIds);
  for (const auto &binding : model.integerProperties) {
    const SkinBindingType type{.kind = SkinBindingKind::IntegerProperty,
                               .integerDomain = binding.domain};
    if (validBindingSource(binding, type, bindingContext)) {
      validIntegerIds.insert(binding.id.value);
    } else {
      allBindingSourcesValid = false;
    }
  }
  for (const auto &binding : model.floatProperties) {
    const SkinBindingType type{.kind = SkinBindingKind::FloatProperty,
                               .floatDomain = binding.domain};
    if (validBindingSource(binding, type, bindingContext)) {
      validFloatIds.insert(binding.id.value);
    } else {
      allBindingSourcesValid = false;
    }
  }
  allBindingSourcesValid &= collectValidBindingIds(
      model.stringProperties, {.kind = SkinBindingKind::StringProperty},
      bindingContext, validStringIds);
  allBindingSourcesValid &= collectValidBindingIds(
      model.timerProperties, {.kind = SkinBindingKind::TimerProperty},
      bindingContext, validTimerIds);
  allBindingSourcesValid &= collectValidBindingIds(
      model.floatWriters, {.kind = SkinBindingKind::FloatWriter},
      bindingContext, validFloatWriterIds);
  allBindingSourcesValid &= collectValidBindingIds(
      model.stringWriters, {.kind = SkinBindingKind::StringWriter},
      bindingContext, validStringWriterIds);
  allBindingSourcesValid &=
      collectValidBindingIds(model.events, {.kind = SkinBindingKind::Event},
                             bindingContext, validEventIds);
  if (!allBindingSourcesValid) {
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_binding_source_invalid",
        "Lua skin binding source is not present in the typed built-in catalog "
        "or live callback generation"));
  }

  std::map<std::uint32_t, SkinIntegerPropertyDomain> integers;
  for (const auto &binding : model.integerProperties) {
    if (validIntegerIds.contains(binding.id.value)) {
      integers.emplace(binding.id.value, binding.domain);
    }
  }
  std::map<std::uint32_t, SkinFloatPropertyDomain> floats;
  for (const auto &binding : model.floatProperties) {
    if (validFloatIds.contains(binding.id.value)) {
      floats.emplace(binding.id.value, binding.domain);
    }
  }
  const ValidationContext context{.imageResources = imageResourceIds,
                                  .fontResources = fontResourceIds,
                                  .objects = objectIds,
                                  .booleans = validBooleanIds,
                                  .integers = integers,
                                  .floats = floats,
                                  .strings = validStringIds,
                                  .timers = validTimerIds,
                                  .floatWriters = validFloatWriterIds,
                                  .stringWriters = validStringWriterIds,
                                  .events = validEventIds};

  std::set<int> customTimerIds;
  const bool customTimersValid =
      std::ranges::all_of(model.customTimers, [&](const auto &timer) {
        return timer.id >= 10'000 && timer.id <= 19'999 &&
               customTimerIds.insert(timer.id).second &&
               (!timer.timer || validTimerIds.contains(timer.timer->value));
      });
  std::set<int> customEventIds;
  const bool customEventsValid =
      std::ranges::all_of(model.customEvents, [&](const auto &event) {
        return event.id >= 1'000 && event.id <= 1'999 &&
               customEventIds.insert(event.id).second && event.action &&
               validEventIds.contains(event.action.value) &&
               (!event.condition ||
                validBooleanIds.contains(event.condition->value)) &&
               event.minimumIntervalMillis >= 0;
      });
  if (!customTimersValid || !customEventsValid) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_custom_object_invalid",
        "Lua skin custom timer/event identities or dependencies are invalid"));
    return result;
  }

  std::vector<SkinObjectId> disabled;
  std::set<SkinObjectId> disabledIds;
  for (const auto &object : model.objects) {
    const auto interaction = unsupportedInteraction(object.payload);
    if (interaction != UnsupportedInteraction::None) {
      result.diagnostics.push_back(validationDiagnostic(
          interaction == UnsupportedInteraction::Image
              ? "skin_lua_model_image_interaction_unsupported"
              : "skin_lua_model_text_interaction_unsupported",
          interaction == UnsupportedInteraction::Image
              ? "Lua skin Image act/click interaction is not supported yet"
              : "Lua skin Text event/editable interaction is not supported "
                "yet"));
    }
    if (interaction == UnsupportedInteraction::None &&
        validPayload(object.payload, context)) {
      continue;
    }
    if (object.critical) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_critical_dependency_invalid",
          "Lua skin critical object has an invalid dependency"));
      return result;
    }
    disabled.push_back(object.id);
    disabledIds.insert(object.id);
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_optional_object_disabled",
        "Lua skin optional object has an invalid dependency"));
  }

  // Judge children are materialized as source-neutral synthetic objects. A
  // valid ID alone is not sufficient when one of those optional children was
  // disabled above: the outer presentation must not retain a dangling live
  // child reference.
  for (const auto &object : model.objects) {
    const auto *judge = std::get_if<SkinJudgeObject>(&object.payload);
    if (judge == nullptr || disabledIds.contains(object.id)) {
      continue;
    }
    const bool childDisabled = std::ranges::any_of(
        judge->grades, [&](const auto &grade) {
          return (grade.image && disabledIds.contains(grade.image->object)) ||
                 (grade.detailNumber &&
                  disabledIds.contains(grade.detailNumber->object));
        });
    if (!childDisabled) {
      continue;
    }
    if (object.critical) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_critical_dependency_invalid",
          "Lua skin critical Judge references a disabled child"));
      return result;
    }
    disabled.push_back(object.id);
    disabledIds.insert(object.id);
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_optional_object_disabled",
        "Lua skin Judge references a disabled child"));
  }

  for (const auto &destination : model.destinations) {
    if (!objectIds.contains(destination.object)) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_destination_invalid",
          "Lua skin destination references an unknown object"));
      return result;
    }
    if (validDestination(destination.presentation, context) ||
        disabledIds.contains(destination.object)) {
      continue;
    }
    const auto object =
        std::ranges::find_if(model.objects, [&](const auto &candidate) {
          return candidate.id == destination.object;
        });
    if (object != model.objects.end() && object->critical) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_critical_dependency_invalid",
          "Lua skin critical object has an invalid destination dependency"));
      return result;
    }
    disabled.push_back(destination.object);
    disabledIds.insert(destination.object);
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_optional_object_disabled",
        "Lua skin optional object has an invalid destination dependency"));
  }

  result.model.emplace(ValidatedBeatorajaSkinModel{
      .model = std::move(model),
      .resourceIds = std::move(resourceNames),
      .objectIds = std::move(objectNames),
      .disabledOptionalObjects = std::move(disabled),
  });
  return result;
}

} // namespace skin

#endif
