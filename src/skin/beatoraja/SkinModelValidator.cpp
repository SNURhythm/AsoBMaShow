#include "SkinModelValidator.h"

#include "../LuaGameplaySkinFeature.h"
#include "../GameplaySkinTraits.h"
#include "LuaSkinTableDecoder.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

bool laneCoverRateSelector(const SkinBuiltinPropertySelector &builtin) {
  if (const auto *selector = std::get_if<int>(&builtin.value)) {
    return *selector == 4 || *selector == 5;
  }
  const auto &selector = std::get<std::string>(builtin.value);
  return selector == "lanecover" || selector == "lanecover2";
}

std::vector<SkinFloatPropertyId> laneCoverRatePropertyIds(
    const std::vector<SkinFloatPropertyBinding> &bindings,
    const std::set<std::uint32_t> &validIds) {
  std::vector<SkinFloatPropertyId> result;
  result.reserve(bindings.size());
  for (const auto &binding : bindings) {
    if (binding.domain != SkinFloatPropertyDomain::Rate ||
        !validIds.contains(binding.id.value)) {
      continue;
    }
    const auto *builtin =
        std::get_if<SkinBuiltinPropertySelector>(&binding.source);
    if (builtin != nullptr && laneCoverRateSelector(*builtin)) {
      result.push_back(binding.id);
    }
  }
  std::ranges::sort(result);
  const auto unique = std::ranges::unique(result);
  result.erase(unique.begin(), result.end());
  return result;
}

} // namespace

SkinModelValidationResult SkinModelValidator::validate(
    BeatorajaSkinModel model,
    SkinBindingValidationContext bindingContext) const {
  SkinModelValidationResult result;
  if (!gameplaySkinTraitForSkinType(model.header.type)) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_type_unsupported",
        "Lua gameplay skins require a supported Beatoraja gameplay type"));
    return result;
  }
  if (model.header.width <= 0 || model.header.height <= 0 ||
      model.header.width > LuaSkinTableDecoderPolicy::maxGameplayDimension ||
      model.header.height > LuaSkinTableDecoderPolicy::maxGameplayDimension) {
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

  const auto containsLuaCallback = [](const auto &bindings) {
    return std::ranges::any_of(bindings, [](const auto &binding) {
      return std::holds_alternative<LuaCallbackId>(binding.source);
    });
  };
  if (!bindingContext.callbacks &&
      (containsLuaCallback(model.booleanProperties) ||
       containsLuaCallback(model.integerProperties) ||
       containsLuaCallback(model.floatProperties) ||
       containsLuaCallback(model.stringProperties) ||
       containsLuaCallback(model.timerProperties) ||
       containsLuaCallback(model.floatWriters) ||
       containsLuaCallback(model.stringWriters) ||
       containsLuaCallback(model.events))) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin.model.callback_runtime_missing",
        "Lua callback bindings require a live gameplay runtime."));
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
              !resourceIds.insert(resource.id).second) {
            return false;
          }
          // JSONSkinLoader's sourceMap is populated with Map.put(), so a
          // later authored source declaration intentionally replaces an
          // earlier declaration with the same name. Keep that effective
          // lookup while still requiring our internal resource IDs to be
          // unique.
          resourceNames.insert_or_assign(resource.authoredName, resource.id);
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
        !objectIds.insert(object.id).second) {
      result.criticalFailure = true;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_model_object_identity_invalid",
          "Lua skin object identities must be nonzero and unique"));
      return result;
    }
    // The pinned loader creates a fresh SkinObject for each destination.
    // Keep the first matching internal ID for legacy lookup clients while
    // allowing repeated authored destination names.
    objectNames.try_emplace(object.authoredName, object.id);
  }
  if (std::ranges::any_of(model.objects, [](const auto &object) {
        const auto *graph =
            std::get_if<SkinNoteDistributionGraphObject>(&object.payload);
        return graph != nullptr &&
               graph->type != SkinNoteDistributionGraphType::Normal &&
               graph->type != SkinNoteDistributionGraphType::Judge &&
               graph->type != SkinNoteDistributionGraphType::EarlyLate;
      })) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_judgegraph_invalid",
        "Lua skin judgegraph type is outside the pinned range"));
    return result;
  }

  if (std::ranges::any_of(model.objects, [](const auto &object) {
        const auto *visualizer =
            std::get_if<SkinTimingVisualizerObject>(&object.payload);
        return visualizer != nullptr &&
               (visualizer->lineWidth < 1 || visualizer->lineWidth > 4);
      })) {
    result.criticalFailure = true;
    result.diagnostics.push_back(validationDiagnostic(
        "skin_lua_model_timingvisualizer_invalid",
        "Lua skin timingvisualizer line width is outside the pinned range"));
    return result;
  }

  // The Java factories are deliberately nullable. JSONSkinLoader keeps the
  // object and passes the factory result to it, so built-in catalog matches
  // and individual callback liveness remain runtime concerns after the
  // callback-bearing model has proved that a runtime exists.
  (void)bindingContext.builtins;
  const auto validBooleanIds = booleanIds;
  const auto validIntegerIds = integerIds;
  const auto validFloatIds = floatIds;
  const auto validStringIds = stringIds;
  const auto validTimerIds = timerIds;
  const auto validFloatWriterIds = floatWriterIds;
  const auto validStringWriterIds = stringWriterIds;
  const auto validEventIds = eventIds;

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
  auto laneCoverRateIds =
      laneCoverRatePropertyIds(model.floatProperties, validFloatIds);
  result.model.emplace(ValidatedBeatorajaSkinModel{
      .model = std::move(model),
      .resourceIds = std::move(resourceNames),
      .objectIds = std::move(objectNames),
      // JSONSkinLoader retains the complete object list produced by its
      // object loader. Individual SkinObject.validate()/prepare() calls
      // decide whether one draw is omitted; they do not turn the skin into a
      // catalog validation failure.
      .disabledOptionalObjects = {},
      .laneCoverRatePropertyIds = std::move(laneCoverRateIds),
      .laneCoverRatePropertyIndexReady = true,
  });
  return result;
}

} // namespace skin

#endif
