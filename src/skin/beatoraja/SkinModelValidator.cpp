#include "SkinModelValidator.h"

#include "../LuaGameplaySkinFeature.h"
#include "LuaSkinTableDecoder.h"
#include "NumericGlyphAtlas.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
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

template <typename Binding, typename Domain>
std::map<std::uint32_t, Domain>
bindingDomains(const std::vector<Binding> &bindings) {
  std::map<std::uint32_t, Domain> result;
  for (const auto &binding : bindings) {
    result.emplace(binding.id.value, binding.domain);
  }
  return result;
}

template <typename Domain>
bool hasDomain(const std::map<std::uint32_t, Domain> &domains, std::uint32_t id,
               Domain expected) {
  const auto found = domains.find(id);
  return found != domains.end() && found->second == expected;
}

bool validSprite(const SkinSpriteFrames &sprite,
                 const std::set<SkinResourceId> &resources,
                 const std::set<std::uint32_t> &timers) {
  return sprite.resource != 0 && resources.contains(sprite.resource) &&
         !sprite.frames.empty() &&
         (!sprite.timer || timers.contains(sprite.timer->value));
}

bool validDigits(const SkinDigitSpriteSet &digits, NumericGlyphAtlasKind kind,
                 const std::set<SkinResourceId> &resources,
                 const std::set<std::uint32_t> &timers) {
  return validSprite(digits.positive, resources, timers) &&
         (!digits.negative || validSprite(*digits.negative, resources, timers)) &&
         validateNumericGlyphAtlas(digits, kind) ==
             NumericGlyphAtlasError::None;
}

bool validNoteVisual(const SkinNoteVisual &visual,
                     const std::set<SkinResourceId> &resources,
                     const std::set<std::uint32_t> &timers) {
  if (const auto *sprite = std::get_if<SkinSpriteFrames>(&visual)) {
    return validSprite(*sprite, resources, timers);
  }
  return true;
}

struct ValidationContext {
  const std::set<SkinResourceId> &resources;
  const std::set<SkinObjectId> &objects;
  const std::map<std::uint32_t, SkinIntegerPropertyDomain> &integers;
  const std::map<std::uint32_t, SkinFloatPropertyDomain> &floats;
  const std::set<std::uint32_t> &strings;
  const std::set<std::uint32_t> &timers;
};

bool validPayload(const SkinObjectPayload &payload,
                  const ValidationContext &context) {
  return std::visit(
      [&](const auto &object) {
        using T = std::decay_t<decltype(object)>;
        if constexpr (std::is_same_v<T, SkinImageObject>) {
          return !object.orderedStates.empty() &&
                 std::ranges::all_of(object.orderedStates,
                                     [&](const auto &state) {
                                       return validSprite(state,
                                                          context.resources,
                                                          context.timers);
                                     }) &&
                 (!object.stateIndex ||
                  hasDomain(context.integers, object.stateIndex->value,
                            SkinIntegerPropertyDomain::ImageIndex));
        } else if constexpr (std::is_same_v<T, SkinNumberObject>) {
          return validDigits(object.digits, NumericGlyphAtlasKind::Number,
                             context.resources,
                             context.timers) &&
                 hasDomain(context.integers, object.value.value,
                           SkinIntegerPropertyDomain::IntegerValue);
        } else if constexpr (std::is_same_v<T, SkinFloatObject>) {
          return validDigits(object.digits, NumericGlyphAtlasKind::Float,
                             context.resources,
                             context.timers) &&
                 hasDomain(context.floats, object.value.value,
                           SkinFloatPropertyDomain::FloatValue);
        } else if constexpr (std::is_same_v<T, SkinTextObject>) {
          return object.font != 0 && context.resources.contains(object.font) &&
                 (!object.value ||
                  context.strings.contains(object.value->value));
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
          return validSprite(object.knob, context.resources, context.timers) &&
                 object.direction <= 3 && valueValid;
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
          return validSprite(object.fill, context.resources, context.timers) &&
                 valueValid;
        } else if constexpr (std::is_same_v<T, SkinGaugeObject>) {
          return !object.orderedNodes.empty() &&
                 std::ranges::all_of(
                     object.orderedNodes, [&](const auto &node) {
                       return validSprite(node, context.resources,
                                          context.timers);
                     });
        } else if constexpr (std::is_same_v<T, SkinNoteObject>) {
          if (object.lanes.empty()) {
            return false;
          }
          return std::ranges::all_of(object.lanes, [&](const auto &lane) {
            const auto normal = lane.visuals.find(SkinNoteVisualKind::Normal);
            return normal != lane.visuals.end() &&
                   validNoteVisual(normal->second, context.resources,
                                   context.timers) &&
                   std::ranges::all_of(lane.visuals, [&](const auto &entry) {
                     return validNoteVisual(entry.second, context.resources,
                                            context.timers);
                   });
          });
        } else if constexpr (std::is_same_v<T, SkinCoverObject>) {
          return validSprite(object.sprite, context.resources, context.timers);
        } else if constexpr (std::is_same_v<T, SkinJudgeObject>) {
          return std::ranges::all_of(object.grades, [&](const auto &grade) {
            const auto nestedValid = [&](const auto &nested) {
              return !nested || context.objects.contains(nested->object);
            };
            return nestedValid(grade.image) && nestedValid(grade.detailNumber);
          });
        } else {
          return true;
        }
      },
      payload);
}

} // namespace

SkinModelValidationResult
SkinModelValidator::validate(BeatorajaSkinModel model) const {
  SkinModelValidationResult result;
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
  std::set<SkinObjectId> objectIds;
  std::map<std::string, SkinResourceId, std::less<>> resourceNames;
  std::map<std::string, SkinObjectId, std::less<>> objectNames;

  for (const auto &definition : model.resources) {
    const bool valid = std::visit(
        [&](const auto &resource) {
          return resource.id != 0 && !resource.authoredName.empty() &&
                 resourceIds.insert(resource.id).second &&
                 resourceNames.emplace(resource.authoredName, resource.id)
                     .second;
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

  const auto integers =
      bindingDomains<SkinIntegerPropertyBinding, SkinIntegerPropertyDomain>(
          model.integerProperties);
  const auto floats =
      bindingDomains<SkinFloatPropertyBinding, SkinFloatPropertyDomain>(
          model.floatProperties);
  const ValidationContext context{.resources = resourceIds,
                                  .objects = objectIds,
                                  .integers = integers,
                                  .floats = floats,
                                  .strings = stringIds,
                                  .timers = timerIds};

  std::vector<SkinObjectId> disabled;
  for (const auto &object : model.objects) {
    if (validPayload(object.payload, context)) {
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
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_optional_object_disabled",
        "Lua skin optional object has an invalid dependency"));
  }

  for (const auto &destination : model.destinations) {
    if (!objectIds.contains(destination.object)) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_destination_invalid",
          "Lua skin destination references an unknown object"));
      return result;
    }
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
