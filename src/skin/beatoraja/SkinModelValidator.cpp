#include "SkinModelValidator.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
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
std::set<std::uint32_t> bindingIds(const std::vector<Binding> &bindings) {
  std::set<std::uint32_t> result;
  for (const auto &binding : bindings) {
    if (binding.id.value != 0) {
      result.insert(binding.id.value);
    }
  }
  return result;
}

bool validSprite(const SkinSpriteFrames &sprite,
                 const std::set<SkinResourceId> &resources,
                 const std::set<std::uint32_t> &timers) {
  return sprite.resource != 0 && resources.contains(sprite.resource) &&
         !sprite.frames.empty() &&
         (!sprite.timer || timers.contains(sprite.timer->value));
}

bool validDigits(const SkinDigitSpriteSet &digits,
                 const std::set<SkinResourceId> &resources,
                 const std::set<std::uint32_t> &timers) {
  return validSprite(digits.positive, resources, timers) &&
         (!digits.negative || validSprite(*digits.negative, resources, timers));
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
  const std::set<std::uint32_t> &integers;
  const std::set<std::uint32_t> &floats;
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
                  context.integers.contains(object.stateIndex->value));
        } else if constexpr (std::is_same_v<T, SkinNumberObject>) {
          return validDigits(object.digits, context.resources,
                             context.timers) &&
                 context.integers.contains(object.value.value);
        } else if constexpr (std::is_same_v<T, SkinFloatObject>) {
          return validDigits(object.digits, context.resources,
                             context.timers) &&
                 context.floats.contains(object.value.value);
        } else if constexpr (std::is_same_v<T, SkinTextObject>) {
          return object.font != 0 && context.resources.contains(object.font) &&
                 (!object.value ||
                  context.strings.contains(object.value->value));
        } else if constexpr (std::is_same_v<T, SkinSliderObject>) {
          const bool valueValid = std::visit(
              [&](const auto &source) {
                using V = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<V, SkinFloatPropertyId>) {
                  return context.floats.contains(source.value);
                } else {
                  return context.integers.contains(source.value.value) &&
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
                  return context.floats.contains(source.value);
                } else {
                  return context.integers.contains(source.value.value) &&
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

  const auto integers = bindingIds(model.integerProperties);
  const auto floats = bindingIds(model.floatProperties);
  const auto strings = bindingIds(model.stringProperties);
  const auto timers = bindingIds(model.timerProperties);
  const ValidationContext context{.resources = resourceIds,
                                  .objects = objectIds,
                                  .integers = integers,
                                  .floats = floats,
                                  .strings = strings,
                                  .timers = timers};

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
