#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"
#include "LuaJValueCoercion.h"
#include "SkinBpmGraphRenderer.h"
#include "SkinCoverNormalization.h"
#include "SkinGaugeGraphRenderer.h"
#include "SkinNoteDistributionGraphRenderer.h"
#include "SkinHitErrorVisualizerRenderer.h"
#include "MusicSelectBarRenderer.h"
#include "SkinTimingVisualizerRenderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

#include <utf8proc.h>

namespace skin {
namespace {

#if defined(ASOBMASHOW_SKIN_RENDERER_TESTING)
std::atomic_size_t lookupComparisonsForTesting{0};
void recordLookupComparison() noexcept {
  lookupComparisonsForTesting.fetch_add(1, std::memory_order_relaxed);
}
#else
void recordLookupComparison() noexcept {}
#endif

SkinDiagnostic
diagnostic(std::string code, std::string message,
           DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = severity};
}

template <typename T> struct ResolvedValue {
  std::optional<T> value;
  std::optional<SkinDiagnostic> failure;
};

int narrowingJavaInt(std::int64_t value) noexcept;
int subtractingJavaInt(int left, int right) noexcept;

bool usesLuaJCompatibilityCoercion(const SkinFrameInputs &inputs) noexcept {
  return !inputs.safetyPolicy.enforces(SkinSafetyGuard::LuaDecoderLimit);
}

std::string coerceLuaJString(const LuaScalar &value) {
  if (const auto *text = std::get_if<std::string>(&value)) return *text;
  if (std::holds_alternative<std::nullptr_t>(value)) return "nil";
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean ? "true" : "false";
  }
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return std::to_string(*integer);
  }
  return std::to_string(std::get<double>(value));
}

std::optional<std::int64_t>
coerceLuaNumericInteger(const LuaScalar &value, std::int64_t minimum,
                        std::int64_t maximum) noexcept {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return std::clamp(*integer, minimum, maximum);
  }
  const auto *floating = std::get_if<double>(&value);
  if (!floating || !std::isfinite(*floating)) {
    return std::nullopt;
  }
  const double truncated = std::trunc(*floating);
  if (truncated <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (truncated >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<std::int64_t>(truncated);
}

bool sameRect(const UiLogicalRect &left, const UiLogicalRect &right) noexcept {
  return left.x == right.x && left.y == right.y && left.width == right.width &&
         left.height == right.height;
}

bool finiteAffine(const Affine2D &value) noexcept {
  return std::isfinite(value.m00) && std::isfinite(value.m01) &&
         std::isfinite(value.tx) && std::isfinite(value.m10) &&
         std::isfinite(value.m11) && std::isfinite(value.ty);
}

bool closeTransformValue(double actual, double expected) noexcept {
  const double scale =
      std::max({1.0, std::abs(actual), std::abs(expected)});
  return std::abs(actual - expected) <= scale * 1e-9;
}

bool invertibleViewport(const PlaySkinViewport &viewport) noexcept {
  if (!viewport.valid || !finiteAffine(viewport.authoredToUi) ||
      !finiteAffine(viewport.uiToAuthored) ||
      !std::isfinite(viewport.safeUiBounds.x) ||
      !std::isfinite(viewport.safeUiBounds.y) ||
      !std::isfinite(viewport.safeUiBounds.width) ||
      !std::isfinite(viewport.safeUiBounds.height) ||
      viewport.safeUiBounds.width <= 0.0 ||
      viewport.safeUiBounds.height <= 0.0) {
    return false;
  }
  const double determinant = viewport.authoredToUi.m00 *
                                 viewport.authoredToUi.m11 -
                             viewport.authoredToUi.m01 *
                                 viewport.authoredToUi.m10;
  if (!std::isfinite(determinant) || determinant == 0.0) {
    return false;
  }
  const auto &forward = viewport.authoredToUi;
  const auto &inverse = viewport.uiToAuthored;
  return closeTransformValue(inverse.m00 * forward.m00 +
                                 inverse.m01 * forward.m10,
                             1.0) &&
         closeTransformValue(inverse.m00 * forward.m01 +
                                 inverse.m01 * forward.m11,
                             0.0) &&
         closeTransformValue(inverse.m10 * forward.m00 +
                                 inverse.m11 * forward.m10,
                             0.0) &&
         closeTransformValue(inverse.m10 * forward.m01 +
                                 inverse.m11 * forward.m11,
                             1.0) &&
         closeTransformValue(inverse.m00 * forward.tx +
                                 inverse.m01 * forward.ty + inverse.tx,
                             0.0) &&
         closeTransformValue(inverse.m10 * forward.tx +
                                 inverse.m11 * forward.ty + inverse.ty,
                             0.0);
}

SkinSliderInteractionGeometry sliderInteraction(
    SkinObjectId sourceObject, std::uint32_t authoredOrdinal,
    const AuthoredDestinationGeometry &geometry,
    const SkinSliderObject &slider, PresentationUiControlKind kind) {
  SkinSliderInteractionGeometry result{
      .sourceObject = sourceObject,
      .authoredOrdinal = authoredOrdinal,
      .kind = kind,
      .authoredDestination = geometry.rect,
      .authoredHitRegion = geometry.rect,
      .valueZero = {.x = geometry.rect.x, .y = geometry.rect.y},
      .valueOne = {.x = geometry.rect.x, .y = geometry.rect.y},
      .direction = slider.direction,
      .range = slider.range,
      .changeable = slider.changeable,
      .writer = slider.writer};
  switch (slider.direction) {
  case 0:
    result.authoredHitRegion.height = slider.range;
    result.valueOne.y += slider.range;
    break;
  case 1:
    result.authoredHitRegion.width = slider.range;
    result.valueOne.x += slider.range;
    break;
  case 2:
    result.authoredHitRegion.y -= slider.range;
    result.authoredHitRegion.height = slider.range;
    result.valueOne.y -= slider.range;
    break;
  case 3:
    result.authoredHitRegion.x -= slider.range;
    result.authoredHitRegion.width = slider.range;
    result.valueOne.x -= slider.range;
    break;
  }
  return result;
}

SkinImageInteractionGeometry imageInteraction(
    SkinObjectId sourceObject, std::uint32_t authoredOrdinal,
    const AuthoredDestinationGeometry &geometry, const SkinImageObject &image) {
  return {.sourceObject = sourceObject,
          .authoredOrdinal = authoredOrdinal,
          .authoredRegion = geometry.rect,
          .event = *image.clickEvent,
          .clickMode = image.clickMode};
}

SkinTextInteractionGeometry textInteraction(
    SkinObjectId sourceObject, std::uint32_t authoredOrdinal,
    const AuthoredDestinationGeometry &geometry, const SkinTextObject &text,
    std::string currentValue) {
  AuthoredRect region = geometry.rect;
  if (text.alignment == 2) {
    region.x -= region.width;
  } else if (text.alignment == 1) {
    region.x -= region.width / 2.0;
  }
  return {.sourceObject = sourceObject,
          .authoredOrdinal = authoredOrdinal,
          .authoredRegion = region,
          .rgba = geometry.rgba,
          .writer = *text.writer,
          .currentValue = std::move(currentValue)};
}

bool laneCoverRateSelector(const SkinBuiltinPropertySelector &builtin) {
  if (const auto *selector = std::get_if<int>(&builtin.value)) {
    return *selector == 4 || *selector == 5;
  }
  const auto &selector = std::get<std::string>(builtin.value);
  return selector == "lanecover" || selector == "lanecover2";
}

bool pinnedLaneCoverRuntimeOffset(int id) noexcept {
  // SkinProperty reserves 3/4/5. LaneRenderer overwrites these MainController
  // offsets every gameplay frame, after the skin's configured offsets load.
  return id == kSkinCoverLiftOffsetId || id == 4 ||
         id == kSkinCoverHiddenOffsetId;
}

bool laneCoverRateProperty(const ValidatedBeatorajaSkinModel &model,
                           const SkinSliderObject &slider) {
  const auto *propertyId = std::get_if<SkinFloatPropertyId>(&slider.value);
  if (propertyId == nullptr) {
    return false;
  }
  if (model.laneCoverRatePropertyIndexReady) {
    return std::ranges::binary_search(model.laneCoverRatePropertyIds,
                                      *propertyId);
  }
  const auto property =
      std::ranges::find(model.model.floatProperties, *propertyId,
                        &SkinFloatPropertyBinding::id);
  if (property == model.model.floatProperties.end() ||
      property->domain != SkinFloatPropertyDomain::Rate) {
    return false;
  }
  const auto *builtin =
      std::get_if<SkinBuiltinPropertySelector>(&property->source);
  return builtin != nullptr && laneCoverRateSelector(*builtin);
}

bool sameState(const SkinRenderState &left,
               const SkinRenderState &right) noexcept {
  if (left.blend != right.blend || left.filter != right.filter ||
      left.scissor.has_value() != right.scissor.has_value() ||
      left.distanceField != right.distanceField) {
    return false;
  }
  return !left.scissor || sameRect(*left.scissor, *right.scissor);
}

bool batchCompatible(const SkinDrawCommand &left,
                     const SkinDrawCommand &right) noexcept {
  if (left.payload.index() != right.payload.index() ||
      std::holds_alternative<SkinBgaCommand>(left.payload)) {
    return false;
  }
  return std::visit(
      [&](const auto &leftPayload) {
        using Payload = std::decay_t<decltype(leftPayload)>;
        const auto *rightPayload = std::get_if<Payload>(&right.payload);
        if (!rightPayload) {
          return false;
        }
        if constexpr (std::is_same_v<Payload, SkinTexturedQuadCommand>) {
          return leftPayload.resource == rightPayload->resource &&
                 sameState(leftPayload.state, rightPayload->state);
        } else if constexpr (std::is_same_v<
                                 Payload,
                                 SkinGeneratedTexturedQuadCommand>) {
          return leftPayload.key == rightPayload->key &&
                 sameState(leftPayload.state, rightPayload->state);
        } else if constexpr (std::is_same_v<Payload, SkinGlyphRunCommand>) {
          return leftPayload.atlas == rightPayload->atlas &&
                 sameState(leftPayload.state, rightPayload->state);
        } else if constexpr (std::is_same_v<Payload, SkinPrimitiveCommand>) {
          return leftPayload.kind == rightPayload->kind &&
                 sameState(leftPayload.state, rightPayload->state);
        } else {
          return false;
        }
      },
      left.payload);
}

void buildAdjacentBatches(SkinCommandBuffer &buffer) {
  if (buffer.commands.empty()) {
    return;
  }
  std::size_t first = 0;
  for (std::size_t index = 1; index < buffer.commands.size(); ++index) {
    if (!batchCompatible(buffer.commands[index - 1], buffer.commands[index])) {
      buffer.adjacentBatches.push_back(
          {.firstCommand = first, .commandCount = index - first});
      first = index;
    }
  }
  buffer.adjacentBatches.push_back(
      {.firstCommand = first, .commandCount = buffer.commands.size() - first});
}

enum class ProjectionElementKind : std::uint8_t { Note, LongNote, Line };

struct ProjectionElement {
  ProjectionElementKind kind = ProjectionElementKind::Note;
  std::size_t index = 0;
  std::uint32_t ordinal = 0;
};

bool validateAndMergeProjection(const ISkinFrameState &state,
                                std::vector<ProjectionElement> &merged) {
  const auto notes = state.projectedNotes();
  const auto longNotes = state.projectedLongNotes();
  const auto lines = state.projectedLines();
  merged.clear();
  const auto append = [&merged](auto values, ProjectionElementKind kind) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      merged.push_back(
          {.kind = kind, .index = index, .ordinal = values[index].submissionOrdinal});
    }
  };
  append(notes, ProjectionElementKind::Note);
  append(longNotes, ProjectionElementKind::LongNote);
  append(lines, ProjectionElementKind::Line);
  // Beatoraja draws each prepared skin object in array order and does not
  // impose a separate gameplay-projection ordinal contract.  The ordinal is
  // Aso's internal ordering hint only: stable sorting keeps it useful while
  // zero, duplicate, or non-monotonic values remain drawable.
  std::stable_sort(merged.begin(), merged.end(),
                   [](const ProjectionElement &left,
                      const ProjectionElement &right) {
                     return left.ordinal < right.ordinal;
                   });
  return true;
}

const SkinObjectDefinition *
findObject(std::span<const SkinObjectDefinition *const> objects,
           SkinObjectId id) noexcept {
  const auto found =
      std::lower_bound(objects.begin(), objects.end(), id,
                       [](const SkinObjectDefinition *object,
                          SkinObjectId value) { return object->id < value; });
  return found == objects.end() || (*found)->id != id ? nullptr : *found;
}

struct FrameLookupIndex {
  std::vector<const SkinBooleanPropertyBinding *> booleans;
  std::vector<const SkinIntegerPropertyBinding *> integers;
  std::vector<const SkinFloatPropertyBinding *> floats;
  std::vector<const SkinStringPropertyBinding *> strings;
  std::vector<const SkinTimerPropertyBinding *> timers;
  std::vector<SkinObjectId> disabledOptionalObjects;
  bool uniqueBindingIds = true;
};

template <typename Binding>
std::vector<const Binding *>
sortedBindingPointers(const std::vector<Binding> &bindings,
                      bool &uniqueBindingIds) {
  std::vector<const Binding *> result;
  result.reserve(bindings.size());
  for (const auto &binding : bindings) {
    result.push_back(&binding);
  }
  std::ranges::sort(result, [](const Binding *left, const Binding *right) {
    return left->id.value < right->id.value;
  });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const Binding *left, const Binding *right) {
                           return left->id == right->id;
                         }) != result.end()) {
    uniqueBindingIds = false;
  }
  return result;
}

FrameLookupIndex
buildFrameLookupIndex(const ValidatedBeatorajaSkinModel &model) {
  FrameLookupIndex index;
  index.booleans = sortedBindingPointers(model.model.booleanProperties,
                                         index.uniqueBindingIds);
  index.integers = sortedBindingPointers(model.model.integerProperties,
                                         index.uniqueBindingIds);
  index.floats = sortedBindingPointers(model.model.floatProperties,
                                       index.uniqueBindingIds);
  index.strings = sortedBindingPointers(model.model.stringProperties,
                                        index.uniqueBindingIds);
  index.timers = sortedBindingPointers(model.model.timerProperties,
                                       index.uniqueBindingIds);
  index.disabledOptionalObjects = model.disabledOptionalObjects;
  std::ranges::sort(index.disabledOptionalObjects);
  index.disabledOptionalObjects.erase(
      std::unique(index.disabledOptionalObjects.begin(),
                  index.disabledOptionalObjects.end()),
      index.disabledOptionalObjects.end());
  return index;
}

template <typename Binding, typename Id>
const Binding *findBinding(const std::vector<const Binding *> &bindings,
                           Id id) noexcept {
  const auto found = std::lower_bound(bindings.begin(), bindings.end(), id,
                                      [](const Binding *binding, Id value) {
                                        recordLookupComparison();
                                        return binding->id.value < value.value;
                                      });
  return found == bindings.end() || (*found)->id != id ? nullptr : *found;
}

bool disabledOptionalObject(const FrameLookupIndex &index,
                            SkinObjectId id) noexcept {
  const auto found =
      std::lower_bound(index.disabledOptionalObjects.begin(),
                       index.disabledOptionalObjects.end(), id,
                       [](SkinObjectId candidate, SkinObjectId value) {
                         recordLookupComparison();
                         return candidate < value;
                       });
  return found != index.disabledOptionalObjects.end() && *found == id;
}

LuaCallbackResult invokeCallback(const SkinFrameInputs &inputs,
                                 LuaCallbackId callback,
                                 std::span<const LuaScalar> arguments = {}) {
  if (inputs.runtime == nullptr) {
    return {.failure = diagnostic(
                "skin.model.callback_runtime_missing",
                "Lua callback binding has no live gameplay runtime.")};
  }
  return inputs.runtime->invoke(callback, arguments);
}

ResolvedValue<bool> resolveBoolean(const SkinFrameInputs &inputs,
                                   const FrameLookupIndex &index,
                                   SkinBooleanPropertyId id) {
  const auto *binding = findBinding(index.booleans, id);
  if (!binding) {
    return {.failure = diagnostic(
                "skin.renderer.binding.missing",
                "Boolean property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto found = inputs.state.booleanProperty(*builtin);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "Boolean property is unsupported by the frame state.")};
    }
    return {.value = found.value};
  }
  const auto invoked =
      invokeCallback(inputs, std::get<LuaCallbackId>(binding->source));
  if (invoked.failure) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = false};
    }
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = false};
    }
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Boolean callback returned a non-boolean value.")};
  }
  if (usesLuaJCompatibilityCoercion(inputs)) {
    return {.value = luaJToBoolean(*invoked.value)};
  }
  if (!std::holds_alternative<bool>(*invoked.value)) {
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Boolean callback returned a non-boolean value.")};
  }
  return {.value = std::get<bool>(*invoked.value)};
}

ResolvedValue<std::int64_t> resolveInteger(const SkinFrameInputs &inputs,
                                           const FrameLookupIndex &index,
                                           SkinIntegerPropertyId id) {
  // SkinNumber keeps a null IntegerProperty when a Lua `value` has no
  // `ref`.  SkinNumber.prepare passes Integer.MIN_VALUE in that case, which
  // hides the number instead of rejecting the destination.
  if (!id) {
    return {.value = std::numeric_limits<std::int32_t>::min()};
  }
  const auto *binding = findBinding(index.integers, id);
  if (!binding) {
    return {.failure = diagnostic(
        "skin.renderer.binding.missing",
        "Integer property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto found = inputs.state.integerProperty(*builtin, binding->domain);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "Integer property is unsupported by the frame state.")};
    }
    return {.value = found.value};
  }
  const auto invoked =
      invokeCallback(inputs, std::get<LuaCallbackId>(binding->source));
  if (invoked.failure) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = 0};
    }
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = 0};
    }
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Integer callback returned no numeric value.")};
  }
  if (usesLuaJCompatibilityCoercion(inputs)) {
    return {.value = static_cast<std::int64_t>(luaJToInt(*invoked.value))};
  }
  const auto value = coerceLuaNumericInteger(
      *invoked.value, std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max());
  if (!value) {
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Integer callback returned a non-numeric value.")};
  }
  return {.value = *value};
}

ResolvedValue<double> resolveFloat(const SkinFrameInputs &inputs,
                                   const FrameLookupIndex &index,
                                   SkinFloatPropertyId id) {
  const auto *binding = findBinding(index.floats, id);
  if (!binding) {
    return {.failure =
                diagnostic("skin.renderer.binding.missing",
                           "Float property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto found = inputs.state.floatProperty(*builtin, binding->domain);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "Float property is unsupported by the frame state.")};
    }
    return {.value = found.value};
  }
  const auto invoked =
      invokeCallback(inputs, std::get<LuaCallbackId>(binding->source));
  if (invoked.failure) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = 0.0};
    }
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = 0.0};
    }
    return {.failure = diagnostic("skin.renderer.binding.type",
                                  "Float callback returned no numeric value.")};
  }
  if (usesLuaJCompatibilityCoercion(inputs)) {
    return {.value = luaJToFloat(*invoked.value)};
  }
  if (const auto *value = std::get_if<double>(&*invoked.value)) {
    return {.value = *value};
  }
  if (const auto *value = std::get_if<std::int64_t>(&*invoked.value)) {
    return {.value = static_cast<double>(*value)};
  }
  return {.failure =
              diagnostic("skin.renderer.binding.type",
                         "Float callback returned a non-numeric value.")};
}

ResolvedValue<double>
resolveRate(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
            const std::variant<SkinFloatPropertyId,
                               SkinSliderObject::IntegerRangeSource> &source) {
  if (const auto *floating = std::get_if<SkinFloatPropertyId>(&source)) {
    const auto resolved = resolveFloat(inputs, index, *floating);
    if (resolved.failure) {
      return {.failure = *resolved.failure};
    }
    if (!std::isfinite(*resolved.value)) {
      return {.failure =
                  diagnostic("skin.renderer.rate.invalid",
                             "Rate property returned a non-finite value.")};
    }
    return resolved;
  }
  const auto &integer = std::get<SkinSliderObject::IntegerRangeSource>(source);
  const auto resolved = resolveInteger(inputs, index, integer.value);
  if (resolved.failure) {
    return {.failure = *resolved.failure};
  }
  if (integer.minimum < integer.maximum) {
    if (*resolved.value > integer.maximum) {
      return {.value = 1.0};
    }
    if (*resolved.value < integer.minimum) {
      return {.value = 0.0};
    }
  } else {
    if (*resolved.value < integer.maximum) {
      return {.value = 1.0};
    }
    if (*resolved.value > integer.minimum) {
      return {.value = 0.0};
    }
  }
  // Pinned RateProperty casts the selected integer value to float before
  // subtracting min, while max-min is wrapping Java int arithmetic.
  const int denominator =
      subtractingJavaInt(integer.maximum, integer.minimum);
  const float numerator =
      static_cast<float>(narrowingJavaInt(*resolved.value)) -
      static_cast<float>(integer.minimum);
  const float rate =
      std::abs(numerator / static_cast<float>(denominator));
  if (!std::isfinite(rate) || rate > 1.0F) {
    // Java keeps this object admitted even when integer overflow makes its
    // arithmetic unusable as a finite textured quad. Retain the frame and
    // omit only this draw rather than rejecting the skin/session.
    return {.value = 0.0};
  }
  return {.value = static_cast<double>(rate)};
}

ResolvedValue<std::string> resolveString(const SkinFrameInputs &inputs,
                                         const FrameLookupIndex &index,
                                         SkinStringPropertyId id) {
  const auto *binding = findBinding(index.strings, id);
  if (!binding) {
    return {.failure = diagnostic(
                "skin.renderer.binding.missing",
                "String property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto found = inputs.state.stringProperty(*builtin);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "String property is unsupported by the frame state.")};
    }
    return {.value = std::string(found.value)};
  }
  const auto invoked =
      invokeCallback(inputs, std::get<LuaCallbackId>(binding->source));
  if (invoked.failure) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = std::string{}};
    }
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = std::string{}};
    }
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "String callback returned a non-string value.")};
  }
  if (usesLuaJCompatibilityCoercion(inputs)) {
    return {.value = coerceLuaJString(*invoked.value)};
  }
  if (!std::holds_alternative<std::string>(*invoked.value)) {
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "String callback returned a non-string value.")};
  }
  return {.value = std::get<std::string>(std::move(*invoked.value))};
}

ResolvedValue<std::int64_t> resolveTimer(const SkinFrameInputs &inputs,
                                         const FrameLookupIndex &index,
                                         SkinTimerPropertyId id) {
  const auto *binding = findBinding(index.timers, id);
  if (!binding) {
    return {.failure =
                diagnostic("skin.renderer.binding.missing",
                           "Timer property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    return {.value = inputs.state.timerProperty(*builtin)};
  }
  const auto invoked =
      invokeCallback(inputs, std::get<LuaCallbackId>(binding->source));
  if (invoked.failure) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = std::numeric_limits<std::int64_t>::min()};
    }
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    if (usesLuaJCompatibilityCoercion(inputs)) {
      return {.value = std::numeric_limits<std::int64_t>::min()};
    }
    return {.failure = diagnostic("skin.renderer.binding.type",
                                  "Timer callback returned no numeric value.")};
  }
  if (usesLuaJCompatibilityCoercion(inputs)) {
    return {.value = luaJToLong(*invoked.value)};
  }
  const auto value = coerceLuaNumericInteger(
      *invoked.value, std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max());
  if (!value) {
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Timer callback returned a non-numeric value.")};
  }
  return {.value = *value};
}

struct ResolvedTimerUse {
  std::int64_t value = INT64_MIN;
  bool off = false;
  std::optional<SkinDiagnostic> failure;
};

ResolvedTimerUse resolveTimerUse(const SkinFrameInputs &inputs,
                                 const FrameLookupIndex &index,
                                 SkinTimerPropertyId id) {
  const auto probe = resolveTimer(inputs, index, id);
  if (probe.failure) {
    return {.failure = *probe.failure};
  }
  if (*probe.value == INT64_MIN) {
    return {.off = true};
  }
  const auto value = resolveTimer(inputs, index, id);
  if (value.failure) {
    return {.failure = *value.failure};
  }
  return {.value = *value.value};
}

struct SpriteSelection {
  const SkinSourceRect *frame = nullptr;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

std::int64_t selectMovieTime(const SkinFrameInputs &inputs) noexcept {
  return std::max<std::int64_t>(0, inputs.visualTimeMicros / 1000);
}

struct AnimationSelection {
  std::size_t frame = 0;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

AnimationSelection
selectAnimationFrame(const SkinFrameInputs &inputs,
                     const FrameLookupIndex &index, std::size_t frameCount,
                     int cycleMillis,
                     std::optional<SkinTimerPropertyId> timer) {
  if (frameCount == 0) {
    return {.failure = diagnostic("skin.renderer.sprite.invalid",
                                  "Sprite has no animation frames.")};
  }
  // SkinSourceImageSet returns row zero immediately for a zero cycle. It does
  // not read the timer in that case, which matters for callback ordering.
  if (cycleMillis == 0) {
    return {.frame = 0};
  }
  std::int64_t elapsedMicros = inputs.visualTimeMicros;
  if (timer) {
    const auto resolved = resolveTimerUse(inputs, index, *timer);
    if (resolved.failure) {
      return {.failure = *resolved.failure};
    }
    if (resolved.off) {
      return {.frame = 0};
    }
    // Dividing each operand first bounds their difference to less than 2^55.
    const auto difference =
        inputs.visualTimeMicros / 1000 - resolved.value / 1000;
    if (difference < 0) {
      return {.frame = 0};
    }
    const auto elapsedMillis = difference;
    elapsedMicros =
        elapsedMillis > std::numeric_limits<std::int64_t>::max() / 1000
            ? std::numeric_limits<std::int64_t>::max()
            : elapsedMillis * 1000;
  }
  std::size_t frame = 0;
  if (frameCount > 1 && cycleMillis > 0) {
    const std::int64_t elapsedMillis =
        std::max<std::int64_t>(0, elapsedMicros / 1000);
    const std::int64_t position = elapsedMillis % cycleMillis;
    const auto cycle = static_cast<std::size_t>(cycleMillis);
    const auto positionInCycle = static_cast<std::size_t>(position);
    // Quotient/remainder decomposition computes position * frameCount / cycle
    // exactly without requiring a non-standard integer type or overflowing.
    frame = (frameCount / cycle) * positionInCycle +
            ((frameCount % cycle) * positionInCycle) / cycle;
    frame = std::min(frame, frameCount - 1);
  }
  return {.frame = frame};
}

SpriteSelection selectSpriteFrame(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinSpriteFrames &sprite) {
  if (sprite.resource == 0 || sprite.frames.empty()) {
    return {.failure =
                diagnostic("skin.renderer.sprite.invalid",
                           "Sprite has no resource or animation frames.")};
  }
  const auto selected = selectAnimationFrame(
      inputs, index, sprite.frames.size(), sprite.cycleMillis, sprite.timer);
  if (selected.failure) {
    return {.failure = *selected.failure};
  }
  if (selected.suppressed) {
    return {.suppressed = true};
  }
  return {.frame = &sprite.frames[selected.frame]};
}

std::optional<UiLogicalRect>
intersectClip(const std::optional<UiLogicalRect> &clip,
              const UiLogicalRect &bounds, bool &empty) noexcept {
  empty = false;
  if (!clip) {
    if (bounds.width <= 0.0 || bounds.height <= 0.0) {
      empty = true;
      return std::nullopt;
    }
    return bounds;
  }
  const double left = std::max(clip->x, bounds.x);
  const double top = std::max(clip->y, bounds.y);
  const double right = std::min(clip->x + clip->width, bounds.x + bounds.width);
  const double bottom =
      std::min(clip->y + clip->height, bounds.y + bounds.height);
  if (right <= left || bottom <= top) {
    empty = true;
    return std::nullopt;
  }
  return UiLogicalRect{
      .x = left, .y = top, .width = right - left, .height = bottom - top};
}

std::uint8_t colorByte(float value) noexcept {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
}

std::uint32_t packAbgr(const std::array<float, 4> &rgba) noexcept {
  const std::uint32_t red = colorByte(rgba[0]);
  const std::uint32_t green = colorByte(rgba[1]);
  const std::uint32_t blue = colorByte(rgba[2]);
  const std::uint32_t alpha = colorByte(rgba[3]);
  return (alpha << 24U) | (blue << 16U) | (green << 8U) | red;
}

struct NumericLayout {
  const SkinSpriteFrames *sprite = nullptr;
  std::size_t animationFrame = 0;
  int glyphsPerAnimationFrame = 0;
  int spacing = 0;
  int alignment = 0;
  bool addAlignmentShift = false;
  std::vector<int> glyphs;
  const std::vector<SkinDigitOffset> *offsets = nullptr;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

bool assignNumericSprite(const SkinSpriteFrames &sprite, int glyphCount,
                         NumericLayout &layout) {
  if (sprite.resource == 0 || glyphCount <= 0 || sprite.frames.empty() ||
      sprite.frames.size() % static_cast<std::size_t>(glyphCount) != 0) {
    layout.failure = diagnostic(
        "skin.renderer.numeric.sprite",
        "Numeric sprite is absent or has an invalid glyph-row shape.");
    return false;
  }
  layout.sprite = &sprite;
  layout.glyphsPerAnimationFrame = glyphCount;
  return true;
}

bool selectNumericAnimation(const SkinFrameInputs &inputs,
                            const FrameLookupIndex &index,
                            NumericLayout &layout) {
  const auto selected = selectAnimationFrame(
      inputs, index,
      layout.sprite->frames.size() /
          static_cast<std::size_t>(layout.glyphsPerAnimationFrame),
      layout.sprite->cycleMillis, layout.sprite->timer);
  if (selected.failure) {
    layout.failure = *selected.failure;
    return false;
  }
  if (selected.suppressed) {
    layout.suppressed = true;
    return false;
  }
  layout.animationFrame = selected.frame;
  return true;
}

std::uint64_t magnitude(std::int32_t value) noexcept {
  return value < 0
             ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(value))
             : static_cast<std::uint64_t>(value);
}

NumericLayout prepareNumberLayoutForValue(const SkinNumberObject &number,
                                          std::int64_t resolvedValue) {
  NumericLayout layout;
  layout.spacing = number.spacing;
  layout.alignment = number.alignment;
  layout.offsets = &number.perDigitOffsets;
  if (resolvedValue <= std::numeric_limits<std::int32_t>::min() ||
      resolvedValue >= std::numeric_limits<std::int32_t>::max() ||
      number.digitCount <= 0) {
    layout.suppressed = true;
    return layout;
  }
  const auto value = static_cast<std::int32_t>(resolvedValue);
  const bool hasSignedSet = number.digits.negative.has_value();
  const auto &sprite = value < 0 && hasSignedSet ? *number.digits.negative
                                                 : number.digits.positive;
  if (!assignNumericSprite(sprite, number.digits.glyphsPerAnimationFrame,
                           layout)) {
    return layout;
  }

  layout.glyphs.assign(static_cast<std::size_t>(number.digitCount), -1);
  auto remaining = magnitude(value);
  const bool padded = number.zeroPadding != SkinZeroPaddingMode::None;
  for (int cell = number.digitCount - 1; cell >= 0; --cell) {
    int glyph = -1;
    if (hasSignedSet && padded) {
      if (cell == 0) {
        glyph = 11;
      } else if (remaining > 0 || cell == number.digitCount - 1) {
        glyph = static_cast<int>(remaining % 10U);
      } else {
        glyph =
            number.zeroPadding == SkinZeroPaddingMode::AlternateZero ? 10 : 0;
      }
    } else if (remaining > 0 || cell == number.digitCount - 1) {
      glyph = static_cast<int>(remaining % 10U);
    } else if (number.zeroPadding == SkinZeroPaddingMode::AlternateZero) {
      glyph = 10;
    } else if (number.zeroPadding == SkinZeroPaddingMode::Zero) {
      glyph = 0;
    } else if (hasSignedSet && cell + 1 < number.digitCount &&
               layout.glyphs[static_cast<std::size_t>(cell + 1)] >= 0 &&
               layout.glyphs[static_cast<std::size_t>(cell + 1)] != 11) {
      glyph = 11;
    }
    layout.glyphs[static_cast<std::size_t>(cell)] = glyph;
    remaining /= 10U;
  }
  return layout;
}

NumericLayout prepareNumberLayout(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinNumberObject &number) {
  const auto resolved = resolveInteger(inputs, index, number.value);
  if (resolved.failure) {
    NumericLayout layout;
    layout.failure = *resolved.failure;
    return layout;
  }
  return prepareNumberLayoutForValue(number, *resolved.value);
}

std::int64_t truncatingJavaLong(double value) noexcept {
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

int truncatingJavaInt(double value) noexcept {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(std::trunc(value));
}

int narrowingJavaInt(std::int64_t value) noexcept {
  const auto bits = static_cast<std::uint32_t>(value);
  return std::bit_cast<std::int32_t>(bits);
}

int subtractingJavaInt(int left, int right) noexcept {
  const auto bits = static_cast<std::uint32_t>(left) -
                    static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(bits);
}

NumericLayout prepareFloatLayout(const SkinFrameInputs &inputs,
                                 const FrameLookupIndex &index,
                                 const SkinFloatObject &floating) {
  NumericLayout layout;
  layout.spacing = floating.spacing;
  layout.alignment = floating.alignment;
  layout.addAlignmentShift = true;
  layout.offsets = &floating.perDigitOffsets;
  const auto resolved = resolveFloat(inputs, index, floating.value);
  if (resolved.failure) {
    layout.failure = *resolved.failure;
    return layout;
  }

  const float source = static_cast<float>(*resolved.value);
  const float value = source * static_cast<float>(floating.gain);
  if (!std::isfinite(value) ||
      source == std::numeric_limits<float>::denorm_min() ||
      source == std::numeric_limits<float>::max() ||
      value == std::numeric_limits<float>::denorm_min() ||
      value == std::numeric_limits<float>::max()) {
    layout.suppressed = true;
    return layout;
  }
  const int integerDigits = floating.integerDigits;
  const int fractionalDigits = floating.fractionalDigits;
  const int sign = floating.signVisible ? 1 : 0;
  if (integerDigits < 0 || fractionalDigits < 0 || integerDigits > 8 ||
      fractionalDigits > 8 || integerDigits + fractionalDigits > 8) {
    layout.failure =
        diagnostic("skin.renderer.numeric.format",
                   "Float digit format is outside its fixed bounds.");
    return layout;
  }
  const int cellCount =
      sign + integerDigits + fractionalDigits + (fractionalDigits != 0 ? 1 : 0);
  if (cellCount == 0) {
    layout.suppressed = true;
    return layout;
  }

  const bool hasSignedSet = floating.digits.negative.has_value();
  const auto &sprite = value < 0.0F && hasSignedSet ? *floating.digits.negative
                                                    : floating.digits.positive;
  if (!assignNumericSprite(sprite, floating.digits.glyphsPerAnimationFrame,
                           layout)) {
    return layout;
  }

  std::vector<int> formatted(static_cast<std::size_t>(cellCount + 1), -1);
  const double absolute = std::abs(static_cast<double>(value));
  if (integerDigits == 0 && fractionalDigits == 0 && sign == 1) {
    formatted[1] = 12;
  } else {
    const bool showSign = sign == 1 && absolute < std::pow(10.0, integerDigits);
    int base = sign + integerDigits;
    if (floating.zeroPadding == SkinZeroPaddingMode::None) {
      const double integral = std::trunc(absolute);
      const int used =
          integral == 0.0
              ? 1
              : static_cast<int>(std::floor(std::log10(integral))) + 1;
      base = std::min(integerDigits, used) + sign;
    }
    std::int64_t remaining =
        truncatingJavaLong(absolute * std::pow(10.0, fractionalDigits));
    int cell = integerDigits == 0
                   ? fractionalDigits + sign + 1
                   : base + fractionalDigits + (fractionalDigits != 0 ? 1 : 0);
    int fractionalRemaining = fractionalDigits;
    while (cell > sign) {
      if (fractionalRemaining > -1) {
        formatted[static_cast<std::size_t>(cell)] =
            static_cast<int>(remaining % 10);
      } else if (remaining == 0 &&
                 floating.zeroPadding == SkinZeroPaddingMode::AlternateZero) {
        formatted[static_cast<std::size_t>(cell)] = 10;
      } else {
        formatted[static_cast<std::size_t>(cell)] =
            static_cast<int>(remaining % 10);
      }
      --fractionalRemaining;
      if (fractionalRemaining == 0) {
        --cell;
        formatted[static_cast<std::size_t>(cell)] = 11;
      }
      remaining /= 10;
      --cell;
    }
    if (cell == 1) {
      formatted[1] = showSign ? 12 : static_cast<int>(remaining % 10);
    }
    if (integerDigits == 0 && sign == 1) {
      formatted[1] = 12;
    }
  }
  layout.glyphs.assign(formatted.begin() + 1, formatted.end());
  return layout;
}

struct NumericLoweringResult {
  std::vector<SkinDrawCommand> commands;
  std::optional<SkinDiagnostic> failure;
};

NumericLoweringResult lowerNumeric(const SkinFrameInputs &inputs,
                                   const SkinObjectDefinition &object,
                                   const SkinDestination &destination,
                                   const AuthoredDestinationGeometry &base,
                                   const NumericLayout &layout) {
  NumericLoweringResult result;
  const auto *resource = inputs.resources.find(layout.sprite->resource);
  if (!resource || resource->width <= 0 || resource->height <= 0) {
    result.failure = diagnostic("skin.renderer.resource.missing",
                                "Prepared numeric image resource is absent.");
    return result;
  }
  const std::size_t omitted = static_cast<std::size_t>(
      std::count(layout.glyphs.begin(), layout.glyphs.end(), -1));
  const double step = base.rect.width + layout.spacing;
  const double shift = layout.alignment == 0   ? 0.0
                       : layout.alignment == 1 ? step * omitted
                                               : step * 0.5 * omitted;
  result.commands.reserve(layout.glyphs.size() - omitted);
  for (std::size_t cell = 0; cell < layout.glyphs.size(); ++cell) {
    const int glyph = layout.glyphs[cell];
    if (glyph < 0) {
      continue;
    }
    if (glyph >= layout.glyphsPerAnimationFrame) {
      result.failure = diagnostic(
          "skin.renderer.numeric.glyph",
          "Numeric formatter selected a glyph absent from its atlas row.");
      result.commands.clear();
      return result;
    }
    const std::size_t frameIndex =
        layout.animationFrame *
            static_cast<std::size_t>(layout.glyphsPerAnimationFrame) +
        static_cast<std::size_t>(glyph);
    const auto &authoredRegion = layout.sprite->frames[frameIndex];
    const auto *region = inputs.resources.findResolvedRegion(
        layout.sprite->resource, authoredRegion);
    if (!region || region->resolved.w <= 0 || region->resolved.h <= 0) {
      result.failure = diagnostic("skin.renderer.resource.missing",
                                  "Prepared numeric image region is absent.");
      result.commands.clear();
      return result;
    }

    auto geometry = base;
    geometry.rect.x += step * static_cast<double>(cell) +
                       (layout.addAlignmentShift ? shift : -shift);
    if (layout.offsets && cell < layout.offsets->size()) {
      const auto &offset = (*layout.offsets)[cell];
      geometry.rect.x += offset.x;
      geometry.rect.y += offset.y;
      geometry.rect.width += offset.width;
      geometry.rect.height += offset.height;
    }
    const auto projected =
        projectSkinDestinationToUi(geometry,
                                   {.textureWidth = resource->width,
                                    .textureHeight = resource->height,
                                    .region = region->resolved},
                                   inputs.viewport);
    bool emptyClip = false;
    const auto clip =
        intersectClip(projected.clip, projectedSkinScissorBounds(inputs.viewport),
                      emptyClip);
    if (emptyClip) {
      continue;
    }
    SkinTexturedQuadCommand command;
    command.resource = layout.sprite->resource;
    command.state = {
        .blend = projected.blend, .filter = projected.filter, .scissor = clip};
    const std::uint32_t color = packAbgr(projected.rgba);
    for (std::size_t vertex = 0; vertex < command.vertices.size(); ++vertex) {
      command.vertices[vertex] = {
          .x = static_cast<float>(projected.vertices[vertex][0]),
          .y = static_cast<float>(projected.vertices[vertex][1]),
          .u = static_cast<float>(projected.normalizedUvs[vertex][0]),
          .v = static_cast<float>(projected.normalizedUvs[vertex][1]),
          .rgba = color};
    }
    result.commands.push_back(
        {.authoredOrdinal = destination.presentation.authoredOrdinal,
         .sourceObject = object.id,
         .payload = std::move(command)});
  }
  return result;
}

struct TextLayoutInput {
  const PreparedSkinTextAtlas *atlas = nullptr;
  std::string value;
  std::vector<char32_t> codepoints;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

TextLayoutInput prepareTextLayoutForValue(const SkinFrameInputs &inputs,
                                          const SkinObjectDefinition &object,
                                          const SkinTextObject &text,
                                          std::string value,
                                          std::size_t maximumCodepoints) {
  TextLayoutInput result;
  result.value = std::move(value);
  if (inputs.observedTextValue) {
    inputs.observedTextValue(object.id, result.value);
  }
  if (result.value.empty() && !(text.editable && text.writer)) {
    result.suppressed = true;
    return result;
  }
  result.atlas = inputs.resources.findTextAtlasForObject(object.id);
  if (!result.atlas || result.atlas->id == 0 ||
      result.atlas->lineHeight <= 0 ||
      (result.atlas->layoutKind != SkinTextLayoutKind::Lr2Image &&
       text.pointSize <= 0) ||
      (result.atlas->bitmapFont &&
       (result.atlas->originalSize <= 0 || result.atlas->pageWidth <= 0 ||
        result.atlas->pageHeight <= 0 || result.atlas->bitmapFontType < 0 ||
        result.atlas->bitmapFontType > 2 ||
        (result.atlas->layoutKind != SkinTextLayoutKind::Bitmap &&
         result.atlas->layoutKind != SkinTextLayoutKind::Lr2Image))) ||
      (!result.atlas->bitmapFont &&
       (result.atlas->width <= 0 || result.atlas->height <= 0 ||
        result.atlas->layoutKind != SkinTextLayoutKind::Scalable))) {
    if (!inputs.safetyPolicy.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
      result.suppressed = true;
      return result;
    }
    result.failure =
        diagnostic("skin.renderer.text.atlas",
                   "Prepared text atlas or its fixed metrics are absent.");
    return result;
  }

  result.codepoints.reserve(std::min(result.value.size(), maximumCodepoints));
  std::size_t offset = 0;
  while (offset < result.value.size()) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(result.value.data() +
                                                   offset),
        static_cast<utf8proc_ssize_t>(result.value.size() - offset),
        &codepoint);
    if (consumed <= 0) {
      result.failure = diagnostic("skin.renderer.text.utf8",
                                  "Text property contains invalid UTF-8.");
      result.codepoints.clear();
      return result;
    }
    offset += static_cast<std::size_t>(consumed);
    const auto scalar = static_cast<char32_t>(codepoint);
    if (scalar != U'\n' && scalar != U'\r' &&
        !result.atlas->glyphs.contains(scalar)) {
      if (result.atlas->bitmapFont) {
        continue;
      }
      if (!inputs.safetyPolicy.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
        // SkinTextFont regenerates a scalable font after SkinText observes a
        // new current value. Our owner-thread atlas replacement completes on
        // a later selector frame, so omit only this transient run after
        // recording it for that matching atlas.
        result.suppressed = true;
        result.codepoints.clear();
        return result;
      }
      result.failure = diagnostic(
          "skin.renderer.text.glyph",
          "Text property contains a glyph absent from the prepared atlas.");
      result.codepoints.clear();
      return result;
    }
    if (scalar != U'\r') {
      if (result.codepoints.size() >= maximumCodepoints) {
        result.failure = diagnostic(
            "skin.renderer.command.limit",
            "Text codepoints exceed the remaining fixed glyph limit.");
        result.codepoints.clear();
        return result;
      }
      result.codepoints.push_back(scalar);
    }
  }
  return result;
}

TextLayoutInput prepareTextLayout(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinObjectDefinition &object,
                                  const SkinTextObject &text,
                                  std::size_t maximumCodepoints) {
  std::string value = text.literal;
  if (text.value) {
    auto resolved = resolveString(inputs, index, *text.value);
    if (resolved.failure) {
      return {.failure = *resolved.failure};
    }
    value = std::move(*resolved.value);
  }
  return prepareTextLayoutForValue(inputs, object, text, std::move(value),
                                   maximumCodepoints);
}

int pairKerning(const PreparedSkinTextAtlas &atlas, char32_t left,
                char32_t right) noexcept {
  const auto found = atlas.kerning.find({left, right});
  return found == atlas.kerning.end() ? 0 : found->second;
}

struct PreparedGlyphPageView {
  int width = 0;
  int height = 0;
};

std::optional<PreparedGlyphPageView>
preparedGlyphPage(const PreparedSkinTextAtlas &atlas,
                  const SkinPreparedGlyphMetrics &metrics) {
  if (atlas.pages.empty()) {
    if (metrics.page != 0 || atlas.width <= 0 || atlas.height <= 0) {
      return std::nullopt;
    }
    return PreparedGlyphPageView{.width = atlas.width,
                                 .height = atlas.height};
  }
  if (metrics.page >= atlas.pages.size()) {
    return std::nullopt;
  }
  const auto &page = atlas.pages[metrics.page];
  if (!page.available || page.width <= 0 || page.height <= 0) {
    return std::nullopt;
  }
  return PreparedGlyphPageView{.width = page.width, .height = page.height};
}

double lineLayoutWidth(const PreparedSkinTextAtlas &atlas,
                       std::span<const char32_t> line) {
  if (line.empty()) {
    return 0.0;
  }
  const auto &first = atlas.glyphs.at(line.front());
  const auto &last = atlas.glyphs.at(line.back());
  double width = -first.bearingX + last.bearingX + last.region.w;
  for (std::size_t index = 1; index < line.size(); ++index) {
    const char32_t previous = line[index - 1];
    width += atlas.glyphs.at(previous).advance +
             pairKerning(atlas, previous, line[index]);
  }
  return std::max(0.0, width);
}

std::vector<std::vector<char32_t>>
breakTextLines(const PreparedSkinTextAtlas &atlas,
               std::span<const char32_t> codepoints, bool wrapping,
               double maximumUnscaledWidth) {
  if (!wrapping) {
    std::vector<std::vector<char32_t>> lines(1);
    for (char32_t codepoint : codepoints) {
      if (codepoint == U'\n') {
        lines.emplace_back();
      } else {
        lines.back().push_back(codepoint);
      }
    }
    return lines;
  }

  std::vector<std::vector<char32_t>> lines(1);
  std::vector<char32_t> pendingWhitespace;
  struct LineLayoutState {
    double leading = 0.0;
    double pen = 0.0;
    double width = 0.0;
  } state;
  const auto isWhitespace = [](char32_t codepoint) {
    return codepoint == U' ' || codepoint == U'\t';
  };
  const auto appendState = [&](LineLayoutState current,
                               std::optional<char32_t> previous,
                               char32_t codepoint) {
    const auto &metrics = atlas.glyphs.at(codepoint);
    if (!previous) {
      current.leading = -metrics.bearingX;
      current.pen = 0.0;
      current.width = metrics.region.w;
      return current;
    }
    current.pen += atlas.glyphs.at(*previous).advance +
                   pairKerning(atlas, *previous, codepoint);
    current.width =
        current.leading + current.pen + metrics.bearingX + metrics.region.w;
    return current;
  };
  const auto appendWord = [&](std::span<const char32_t> word) {
    LineLayoutState candidate = state;
    std::optional<char32_t> previous =
        lines.back().empty() ? std::nullopt
                             : std::optional<char32_t>(lines.back().back());
    if (!lines.back().empty()) {
      for (char32_t codepoint : pendingWhitespace) {
        candidate = appendState(candidate, previous, codepoint);
        previous = codepoint;
      }
    }
    for (char32_t codepoint : word) {
      candidate = appendState(candidate, previous, codepoint);
      previous = codepoint;
    }
    if (!lines.back().empty() && maximumUnscaledWidth > 0.0 &&
        candidate.width > maximumUnscaledWidth) {
      lines.emplace_back();
      state = {};
    } else if (!lines.back().empty()) {
      lines.back().insert(lines.back().end(), pendingWhitespace.begin(),
                          pendingWhitespace.end());
      state = candidate;
      lines.back().insert(lines.back().end(), word.begin(), word.end());
      pendingWhitespace.clear();
      return;
    }
    pendingWhitespace.clear();
    for (char32_t codepoint : word) {
      const std::optional<char32_t> prior =
          lines.back().empty() ? std::nullopt
                               : std::optional<char32_t>(lines.back().back());
      auto next = appendState(state, prior, codepoint);
      if (!lines.back().empty() && maximumUnscaledWidth > 0.0 &&
          next.width > maximumUnscaledWidth) {
        lines.emplace_back();
        state = {};
        next = appendState(state, std::nullopt, codepoint);
      }
      lines.back().push_back(codepoint);
      state = next;
    }
  };

  std::size_t offset = 0;
  while (offset < codepoints.size()) {
    const char32_t codepoint = codepoints[offset];
    if (codepoint == U'\n') {
      pendingWhitespace.clear();
      lines.emplace_back();
      state = {};
      ++offset;
      continue;
    }
    if (isWhitespace(codepoint)) {
      pendingWhitespace.push_back(codepoint);
      ++offset;
      continue;
    }
    const std::size_t wordStart = offset;
    while (offset < codepoints.size() && codepoints[offset] != U'\n' &&
           !isWhitespace(codepoints[offset])) {
      ++offset;
    }
    appendWord(codepoints.subspan(wordStart, offset - wordStart));
  }
  return lines;
}

struct TextLoweringResult {
  std::optional<SkinDrawCommand> command;
  std::optional<SkinDiagnostic> failure;
  std::size_t glyphCount = 0;
};

TextLoweringResult lowerText(const SkinFrameInputs &inputs,
                             const SkinObjectDefinition &object,
                             const SkinDestination &destination,
                             const AuthoredDestinationGeometry &base,
                             const SkinTextObject &text,
                             const TextLayoutInput &prepared) {
  TextLoweringResult result;
  const auto &atlas = *prepared.atlas;
  if (atlas.layoutKind == SkinTextLayoutKind::Lr2Image) {
    std::vector<char32_t> glyphs;
    glyphs.reserve(prepared.codepoints.size());
    double measuredWidth = 0.0;
    const double heightScale = base.rect.height / atlas.originalSize;
    if (!std::isfinite(heightScale) || heightScale <= 0.0) {
      result.failure = diagnostic("skin.renderer.text.geometry",
                                  "LR2 text destination scale is invalid.");
      return result;
    }
    for (const char32_t codepoint : prepared.codepoints) {
      const auto metric = atlas.glyphs.find(codepoint);
      if (metric == atlas.glyphs.end() ||
          !preparedGlyphPage(atlas, metric->second)) {
        continue;
      }
      glyphs.push_back(codepoint);
      measuredWidth += metric->second.region.w * heightScale + atlas.margin;
    }
    if (glyphs.empty()) {
      return result;
    }
    const double shrink = base.rect.width < measuredWidth && measuredWidth > 0.0
                              ? base.rect.width / measuredWidth
                              : 1.0;
    const double drawnWidth = measuredWidth * shrink;
    const double startX = text.alignment == 2   ? base.rect.x - drawnWidth
                          : text.alignment == 1 ? base.rect.x - drawnWidth * 0.5
                                                : base.rect.x;
    SkinGlyphRunCommand run;
    run.atlas = atlas.id;
    run.glyphs.reserve(glyphs.size());
    double dx = 0.0;
    for (const char32_t codepoint : glyphs) {
      const auto &metrics = atlas.glyphs.at(codepoint);
      const auto page = preparedGlyphPage(atlas, metrics);
      if (!page) {
        continue;
      }
      const double width = metrics.region.w * heightScale * shrink;
      auto geometry = base;
      geometry.stretch = SkinStretchMode::Stretch;
      geometry.angleDegrees = 0.0;
      geometry.rect = {.x = startX + dx,
                       .y = base.rect.y,
                       .width = width,
                       .height = base.rect.height};
      const auto projected = projectSkinDestinationToUi(
          geometry,
          {.textureWidth = page->width,
           .textureHeight = page->height,
           .region = metrics.region},
          inputs.viewport);
      SkinGlyphInstance glyph{.codepoint = codepoint};
      const std::uint32_t color = packAbgr(projected.rgba);
      for (std::size_t vertex = 0; vertex < glyph.vertices.size(); ++vertex) {
        glyph.vertices[vertex] = {
            .x = static_cast<float>(projected.vertices[vertex][0]),
            .y = static_cast<float>(projected.vertices[vertex][1]),
            .u = static_cast<float>(projected.normalizedUvs[vertex][0]),
            .v = static_cast<float>(projected.normalizedUvs[vertex][1]),
            .rgba = color};
      }
      run.glyphs.push_back(std::move(glyph));
      dx += width + atlas.margin * shrink;
    }
    bool emptyClip = false;
    const auto projectedClip = projectSkinDestinationToUi(
        base,
        {.textureWidth = atlas.width,
         .textureHeight = atlas.height,
         .region = {.x = 0, .y = 0, .w = atlas.width, .h = atlas.height}},
        inputs.viewport);
    const auto clip = intersectClip(projectedClip.clip,
                                    projectedSkinScissorBounds(inputs.viewport),
                                    emptyClip);
    if (emptyClip) {
      return result;
    }
    run.state = {.blend = base.blend,
                 .filter = SkinFilterMode::Linear,
                 .scissor = clip};
    result.glyphCount = run.glyphs.size();
    result.command = SkinDrawCommand{
        .authoredOrdinal = destination.presentation.authoredOrdinal,
        .sourceObject = object.id,
        .payload = std::move(run)};
    return result;
  }
  const double scaleY = atlas.bitmapFont
                            ? static_cast<double>(text.pointSize) /
                                  atlas.originalSize
                            : base.rect.height / text.pointSize;
  if (!std::isfinite(scaleY) || scaleY <= 0.0) {
    result.failure = diagnostic("skin.renderer.text.geometry",
                                "Text destination scale is invalid.");
    return result;
  }
  auto lines = breakTextLines(atlas, prepared.codepoints, text.wrapping,
                              base.rect.width / scaleY);
  double scaleX = scaleY;
  if (!text.wrapping && text.overflow == 1) {
    double measured = 0.0;
    for (const auto &line : lines) {
      measured = std::max(measured, lineLayoutWidth(atlas, line) * scaleX);
    }
    if (measured > base.rect.width && measured > 0.0) {
      scaleX *= base.rect.width / measured;
    }
  }
  if (!text.wrapping && text.overflow == 2 && base.rect.width >= 0.0) {
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
      auto &line = lines[lineIndex];
      if (lineLayoutWidth(atlas, line) * scaleX <= base.rect.width) {
        continue;
      }
      std::size_t accepted = line.empty() ? 0 : 1;
      double pen = 0.0;
      const double leading =
          line.empty() ? 0.0 : -atlas.glyphs.at(line.front()).bearingX;
      for (std::size_t index = 1; index < line.size(); ++index) {
        const char32_t previous = line[index - 1];
        const char32_t current = line[index];
        pen += atlas.glyphs.at(previous).advance +
               pairKerning(atlas, previous, current);
        const auto &metrics = atlas.glyphs.at(current);
        if ((leading + pen + metrics.bearingX + metrics.region.w) * scaleX >
            base.rect.width) {
          break;
        }
        accepted = index + 1;
      }
      line.resize(accepted);
      lines.resize(lineIndex + 1);
      break;
    }
  }

  SkinGlyphRunCommand run;
  run.atlas = atlas.id;
  const std::size_t requested =
      prepared.codepoints.size() -
      static_cast<std::size_t>(std::count(prepared.codepoints.begin(),
                                          prepared.codepoints.end(), U'\n'));
  run.glyphs.reserve(requested);
  for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
    const auto &line = lines[lineIndex];
    const double width = lineLayoutWidth(atlas, line) * scaleX;
    const double startX = text.alignment == 2   ? base.rect.x - width
                          : text.alignment == 1 ? base.rect.x - width * 0.5
                                                : base.rect.x;
    const double baseline =
        base.rect.y + base.rect.height -
        static_cast<double>(lineIndex) * atlas.lineHeight * scaleY;
    double pen =
        line.empty() ? 0.0 : -atlas.glyphs.at(line.front()).bearingX * scaleX;
    std::optional<char32_t> previous;
    for (char32_t codepoint : line) {
      if (previous) {
        pen += pairKerning(atlas, *previous, codepoint) * scaleX;
      }
      const auto &metrics = atlas.glyphs.at(codepoint);
      const auto page = preparedGlyphPage(atlas, metrics);
      if (!page) {
        continue;
      }
      auto geometry = base;
      geometry.stretch = SkinStretchMode::Stretch;
      // SkinTextFont draws BitmapFont layouts directly and does not route
      // through SkinObject's rotated TextureRegion draw overload.
      geometry.angleDegrees = 0.0;
      geometry.rect = {.x = startX + pen + metrics.bearingX * scaleX,
                       .y = baseline + metrics.layoutOffsetY * scaleY,
                       .width = metrics.region.w * scaleX,
                       .height = metrics.region.h * scaleY};
      const auto projected =
          projectSkinDestinationToUi(geometry,
                                     {.textureWidth = page->width,
                                      .textureHeight = page->height,
                                      .region = metrics.region},
                                     inputs.viewport);
      SkinGlyphInstance glyph;
      glyph.codepoint = codepoint;
      const std::uint32_t color = packAbgr(projected.rgba);
      for (std::size_t vertex = 0; vertex < glyph.vertices.size(); ++vertex) {
        glyph.vertices[vertex] = {
            .x = static_cast<float>(projected.vertices[vertex][0]),
            .y = static_cast<float>(projected.vertices[vertex][1]),
            .u = static_cast<float>(projected.normalizedUvs[vertex][0]),
            .v = static_cast<float>(projected.normalizedUvs[vertex][1]),
            .rgba = color};
      }
      run.glyphs.push_back(std::move(glyph));
      pen += metrics.advance * scaleX;
      previous = codepoint;
    }
  }
  if (run.glyphs.empty()) {
    return result;
  }
  if (atlas.bitmapFont &&
      (atlas.bitmapFontType == 1 || atlas.bitmapFontType == 2)) {
    const std::uint32_t fallbackColor =
        packAbgr({1.0F, 1.0F, 1.0F, base.rgba[3]});
    for (const auto &glyph : run.glyphs) {
      const auto metrics = atlas.glyphs.find(glyph.codepoint);
      if (metrics == atlas.glyphs.end() ||
          metrics->second.bitmapFontType != 0) {
        continue;
      }
      auto overlay = glyph;
      for (auto &vertex : overlay.vertices) {
        vertex.rgba = fallbackColor;
      }
      run.fallbackColorOverlays.push_back(std::move(overlay));
    }
  }
  if ((!atlas.bitmapFont || atlas.bitmapFontType == 0) &&
      (text.shadowOffsetX != 0.0 || text.shadowOffsetY != 0.0)) {
    const double authoredShadowX = text.shadowOffsetX;
    const double authoredShadowY = -text.shadowOffsetY;
    const float shadowX = static_cast<float>(
        inputs.viewport.authoredToUi.m00 * authoredShadowX +
        inputs.viewport.authoredToUi.m01 * authoredShadowY);
    const float shadowY = static_cast<float>(
        inputs.viewport.authoredToUi.m10 * authoredShadowX +
        inputs.viewport.authoredToUi.m11 * authoredShadowY);
    auto shadow = run.glyphs;
    auto shadowColor = base.rgba;
    shadowColor[0] *= 0.5F;
    shadowColor[1] *= 0.5F;
    shadowColor[2] *= 0.5F;
    const std::uint32_t packedShadow = packAbgr(shadowColor);
    for (auto &glyph : shadow) {
      for (auto &vertex : glyph.vertices) {
        vertex.x += shadowX;
        vertex.y += shadowY;
        vertex.rgba = packedShadow;
      }
    }
    shadow.insert(shadow.end(), std::make_move_iterator(run.glyphs.begin()),
                  std::make_move_iterator(run.glyphs.end()));
    run.glyphs = std::move(shadow);
  }
  bool emptyClip = false;
  const auto projectedClip = projectSkinDestinationToUi(
      base,
      {.textureWidth = atlas.width,
       .textureHeight = atlas.height,
       .region = {.x = 0, .y = 0, .w = atlas.width, .h = atlas.height}},
      inputs.viewport);
  const auto clip = intersectClip(projectedClip.clip,
                                  projectedSkinScissorBounds(inputs.viewport),
                                  emptyClip);
  if (emptyClip) {
    return result;
  }
  run.state = {.blend = base.blend,
               .filter = atlas.bitmapFont ? SkinFilterMode::Linear
                                          : base.filter,
               .scissor = clip};
  if (atlas.bitmapFont &&
      (atlas.bitmapFontType == 1 || atlas.bitmapFontType == 2)) {
    run.state.distanceField = SkinRenderState::DistanceField{
        .colored = atlas.bitmapFontType == 2,
        .outlineDistance = std::max(0.1, 0.5 - text.outlineWidth / 2.0),
        .outlineRgba = text.outlineRgba,
        .shadowRgba = text.shadowRgba,
        .shadowSmoothing = text.shadowSmoothness / 2.0,
        .shadowOffsetU = text.shadowOffsetX / atlas.pageWidth,
        .shadowOffsetV = text.shadowOffsetY / atlas.pageHeight};
  }
  result.glyphCount = run.glyphs.size();
  result.command = SkinDrawCommand{.authoredOrdinal =
                                       destination.presentation.authoredOrdinal,
                                   .sourceObject = object.id,
                                   .payload = std::move(run)};
  return result;
}

struct PracticeLoweringResult {
  std::vector<SkinDrawCommand> commands;
  std::optional<SkinDiagnostic> failure;
  std::size_t glyphCount = 0;
  std::size_t primitiveVertices = 0;
};

PracticeLoweringResult lowerPracticeLegacy(
    const SkinFrameInputs &inputs, const FrameLookupIndex &lookupIndex,
    const SkinObjectDefinition &object, const SkinDestination &destination,
    const AuthoredDestinationGeometry &region,
    const SkinPracticeStateView &practice, std::size_t maximumGlyphs,
    std::size_t maximumCommands, std::size_t maximumPrimitiveVertices) {
  PracticeLoweringResult result;
  if (!practice.supported) {
    return result;
  }
  if (practice.graphType < 0 || practice.graphType > 2) {
    result.failure = diagnostic("skin.renderer.practice.state",
                                "Practice graph type is invalid.");
    return result;
  }
  const double x = region.rect.x + region.rect.width / 8.0;
  const double y = region.rect.y + region.rect.height * 7.0 / 8.0;
  constexpr double spacing = 22.0;
  constexpr double pointSize = 18.0;
  const std::array<float, 4> unfocused =
      practice.horizontalInputMode
          ? std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F}
          : std::array<float, 4>{0.0F, 1.0F, 1.0F, 1.0F};
  const std::array<float, 4> focused =
      practice.inputTurbo
          ? std::array<float, 4>{1.0F, 0.5F, 0.0F, 1.0F}
          : std::array<float, 4>{1.0F, 1.0F, 0.0F, 1.0F};
  constexpr std::array<float, 4> orange{1.0F, 0.5F, 0.0F, 1.0F};
  constexpr std::array<float, 4> white{1.0F, 1.0F, 1.0F, 1.0F};
  const bool preparedTextAvailable =
      inputs.resources.findTextAtlasForObject(object.id) != nullptr;

  const auto appendText = [&](std::string value, double lineX, double lineY,
                              std::array<float, 4> color) -> bool {
    if (value.empty() || !preparedTextAvailable) {
      return true;
    }
    if (result.commands.size() >= maximumCommands) {
      result.failure = diagnostic(
          "skin.renderer.command.limit",
          "Practice text exceeds the fixed frame command limit.");
      return false;
    }
    SkinTextObject text{
        .font = 0,
        .literal = std::move(value),
        .pointSize = static_cast<int>(pointSize),
        .alignment = 0,
    };
    const std::size_t remainingGlyphs =
        maximumGlyphs - std::min(maximumGlyphs, result.glyphCount);
    auto prepared = prepareTextLayout(inputs, lookupIndex, object, text,
                                      remainingGlyphs);
    if (prepared.failure) {
      result.failure = std::move(prepared.failure);
      return false;
    }
    if (prepared.suppressed) {
      return true;
    }
    auto geometry = region;
    geometry.rect = {.x = lineX,
                     .y = lineY - pointSize,
                     .width = std::max(0.0, region.rect.x + region.rect.width -
                                               lineX),
                     .height = pointSize};
    geometry.angleDegrees = 0.0;
    geometry.rgba = color;
    geometry.blend = SkinBlendMode::Normal;
    geometry.filter = SkinFilterMode::Linear;
    geometry.stretch = SkinStretchMode::Stretch;
    auto lowered = lowerText(inputs, object, destination, geometry, text,
                             prepared);
    if (lowered.failure) {
      result.failure = std::move(lowered.failure);
      return false;
    }
    if (lowered.command) {
      result.glyphCount += lowered.glyphCount;
      result.commands.push_back(std::move(*lowered.command));
    }
    return true;
  };

  for (std::size_t index = 0; index < practice.items.size(); ++index) {
    const auto &item = practice.items[index];
    if (!item.available) {
      continue;
    }
    const auto color = practice.cursorPosition == index ? focused : unfocused;
    const double itemY = y - spacing * static_cast<double>(index);
    if (!appendText(std::string(item.label), x, itemY, color) ||
        !appendText(std::string(item.value), x + 150.0, itemY, color)) {
      return result;
    }
  }

  std::string helpLine1;
  std::string helpLine2;
  if (practice.keyMode == 9) {
    helpLine1 = "KEYS: 2/8=UP, 3/7=DOWN, 4=LEFT, 6=RIGHT,";
    helpLine2 = "5=TURBO";
  } else if (practice.keyMode == 24 || practice.keyMode == 48) {
    helpLine1 = "KEYS: F#1/A#1=UP, G1/A1=DOWN, F1=LEFT,";
    helpLine2 = "B1=RIGHT, D#1/G#1=TURBO";
  } else {
    helpLine1 = "KEYS: SCR=UP/DOWN, 2+SCR=LEFT/RIGHT, 4=TURBO";
  }
  if (practice.mediaReady) {
    if (!helpLine2.empty()) {
      helpLine2 += ". ";
    }
    helpLine2 += practice.keyMode == 24 || practice.keyMode == 48
                     ? "PRESS C1 TO PLAY"
                     : "PRESS 1KEY TO PLAY";
  }
  if (!appendText(std::move(helpLine1), x, y - spacing * 12.0 - 12.0,
                  orange) ||
      !appendText(std::move(helpLine2), x, y - spacing * 13.0 - 12.0,
                  orange)) {
    return result;
  }

  constexpr std::array<std::string_view, 6> judgeLabels{
      "PGREAT :", "GREAT  :", "GOOD   :", "BAD    :", "POOR   :",
      "KPOOR  :"};
  for (std::size_t index = 0; index < judgeLabels.size(); ++index) {
    const auto &counts = practice.judgeCounts[index];
    std::string line(judgeLabels[index]);
    line += " " + std::to_string(counts.fast + counts.slow) + " " +
            std::to_string(counts.fast) + " " + std::to_string(counts.slow);
    if (!appendText(std::move(line), x + 250.0,
                    y - spacing * static_cast<double>(index), white)) {
      return result;
    }
  }

  auto graphGeometry = region;
  graphGeometry.rect.height = region.rect.height / 4.0;
  graphGeometry.angleDegrees = 0.0;
  graphGeometry.rgba = white;
  graphGeometry.blend = SkinBlendMode::Normal;
  graphGeometry.filter = SkinFilterMode::Nearest;
  graphGeometry.stretch = SkinStretchMode::Stretch;
  const SkinNoteDistributionGraphObject graph{
      .type = static_cast<SkinNoteDistributionGraphType>(practice.graphType)};
  std::optional<std::int64_t> currentMillis;
  const std::int64_t playTimerStart =
      inputs.state.timerProperty(SkinBuiltinPropertySelector{41});
  if (playTimerStart != INT64_MIN) {
    currentMillis = (inputs.visualTimeMicros / 1'000 -
                     playTimerStart / 1'000) *
                    practice.frequencyPercent / 100;
  }
  const std::size_t remainingCommands =
      maximumCommands - std::min(maximumCommands, result.commands.size());
  auto lowered = renderSkinNoteDistributionGraph(
      {.sourceObject = object.id,
       .authoredOrdinal = destination.presentation.authoredOrdinal,
       .graph = graph,
       .state = inputs.state.gameplayGraphState(),
       .geometry = graphGeometry,
       .viewport = inputs.viewport,
       .pmsMode = practice.keyMode == 9,
       .elapsedMillis = inputs.visualTimeMicros / 1'000,
       .startMillis = practice.startTimeMillis,
       .endMillis = practice.endTimeMillis,
       .currentMillis = currentMillis,
       .maximumCommands = remainingCommands,
       .maximumPrimitiveVertices = maximumPrimitiveVertices});
  if (lowered.failure) {
    result.failure = std::move(lowered.failure);
    result.commands.clear();
    result.glyphCount = 0;
    return result;
  }
  result.primitiveVertices = lowered.primitiveVertices;
  result.commands.insert(result.commands.end(),
                         std::make_move_iterator(lowered.commands.begin()),
                         std::make_move_iterator(lowered.commands.end()));
  return result;
}

bool configuredCondition(const BeatorajaSkinConfiguration &configuration,
                         int condition, bool &value) noexcept {
  if (condition == std::numeric_limits<int>::min()) {
    return false;
  }
  if (condition >= 0) {
    value = configuration.enabledOptionIds.contains(condition);
  } else {
    value = !configuration.enabledOptionIds.contains(-condition);
  }
  return true;
}

struct DestinationResolution {
  std::optional<AuthoredDestinationGeometry> geometry;
  std::vector<SkinDiagnostic> failures;
};

DestinationResolution
resolveDestination(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
                   const SkinDestinationBody &presentation,
                   bool relativeOffsets = false) {
  DestinationResolution result;
  // Beatoraja's Skin.prepare removes objects with no destination frames.
  // Suppress them before their conditions or timers can observe state.
  if (presentation.frames.empty()) {
    return result;
  }
  const std::size_t conditionCount =
      presentation.conditions.size() + (presentation.drawCondition ? 1U : 0U);
  std::unique_ptr<bool[]> conditions;
  if (conditionCount != 0) {
    conditions = std::make_unique<bool[]>(conditionCount);
  }
  std::size_t conditionIndex = 0;
  for (const auto &condition : presentation.conditions) {
    if (const auto *configured = std::get_if<int>(&condition)) {
      bool value = false;
      if (!configuredCondition(inputs.configuration, *configured, value)) {
        result.failures.push_back(diagnostic(
            "skin.renderer.condition.invalid",
            "Configured destination condition is outside its safe domain."));
        return result;
      }
      conditions[conditionIndex++] = value;
      if (!value) {
        return result;
      }
      continue;
    }
    const auto resolved = resolveBoolean(
        inputs, index, std::get<SkinBooleanPropertyId>(condition));
    if (resolved.failure) {
      result.failures.push_back(*resolved.failure);
      return result;
    }
    conditions[conditionIndex++] = *resolved.value;
    if (!*resolved.value) {
      return result;
    }
  }
  if (presentation.drawCondition) {
    const auto resolved =
        resolveBoolean(inputs, index, *presentation.drawCondition);
    if (resolved.failure) {
      result.failures.push_back(*resolved.failure);
      return result;
    }
    conditions[conditionIndex++] = *resolved.value;
    if (!*resolved.value) {
      return result;
    }
  }

  std::int64_t timerStartMicros = INT64_MIN;
  bool timerOff = false;
  if (presentation.timer) {
    const auto timer = resolveTimerUse(inputs, index, *presentation.timer);
    if (timer.failure) {
      result.failures.push_back(*timer.failure);
      return result;
    }
    timerStartMicros = timer.value;
    timerOff = timer.off;
  }
  auto temporal = evaluateSkinDestinationAuthored(
      presentation, {.nowMicros = inputs.visualTimeMicros,
                     .timerStartMicros = timerStartMicros,
                     .timerOff = timerOff,
                     .optionConditions = std::span<const bool>(conditions.get(),
                                                               conditionCount),
                     .orderedOffsets = {}});
  if (!temporal.diagnostics.empty()) {
    result.failures = std::move(temporal.diagnostics);
    return result;
  }
  if (!temporal.geometry) {
    return result;
  }

  std::vector<SkinRuntimeOffset> offsets;
  offsets.reserve(presentation.offsetIds.size());
  double relativeTranslationX = 0.0;
  double relativeTranslationY = 0.0;
  for (const int id : presentation.offsetIds) {
    if (id <= 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
      continue;
    }
    const auto configured = inputs.configuration.offsetsById.find(id);
    if (!pinnedLaneCoverRuntimeOffset(id) &&
        configured != inputs.configuration.offsetsById.end()) {
      offsets.push_back(skinRuntimeOffset(configured->second));
    } else {
      const auto dynamic = inputs.state.offsetProperty(id);
      if (!dynamic.supported) {
        result.failures.push_back(diagnostic(
            "skin.renderer.offset.missing",
            "Destination offset is unsupported by both configuration and "
            "frame state."));
        return result;
      }
      offsets.push_back(dynamic.value);
    }
    if (relativeOffsets) {
      const auto &offset = offsets.back();
      relativeTranslationX +=
          static_cast<double>(offset.x) - static_cast<double>(offset.w) * 0.5;
      relativeTranslationY +=
          static_cast<double>(offset.y) - static_cast<double>(offset.h) * 0.5;
    }
  }

  auto evaluated = evaluateSkinDestinationAuthored(
      presentation, {.nowMicros = inputs.visualTimeMicros,
                     .timerStartMicros = timerStartMicros,
                     .timerOff = timerOff,
                     .optionConditions = std::span<const bool>(conditions.get(),
                                                               conditionCount),
                     .orderedOffsets = offsets});
  result.geometry = std::move(evaluated.geometry);
  result.failures = std::move(evaluated.diagnostics);
  if (relativeOffsets && result.geometry) {
    result.geometry->rect.x -= relativeTranslationX;
    result.geometry->rect.y -= relativeTranslationY;
    if (result.geometry->clip) {
      result.geometry->clip->x -= relativeTranslationX;
      result.geometry->clip->y -= relativeTranslationY;
    }
  }
  return result;
}

struct QuadLoweringResult {
  std::optional<SkinDrawCommand> command;
  std::optional<SkinDiagnostic> failure;
};

bool projectedQuadFitsUpload(const UiDestinationGeometry &projected) noexcept {
  const auto fitsFloat = [](double value) {
    return std::isfinite(value) &&
           std::abs(value) <=
               static_cast<double>(std::numeric_limits<float>::max());
  };
  for (const auto &vertex : projected.vertices) {
    if (!fitsFloat(vertex[0]) || !fitsFloat(vertex[1])) {
      return false;
    }
  }
  for (const auto &uv : projected.normalizedUvs) {
    if (!fitsFloat(uv[0]) || !fitsFloat(uv[1])) {
      return false;
    }
  }
  if (projected.clip &&
      (!fitsFloat(projected.clip->x) || !fitsFloat(projected.clip->y) ||
       !fitsFloat(projected.clip->width) ||
       !fitsFloat(projected.clip->height))) {
    return false;
  }
  return std::ranges::all_of(projected.rgba,
                             [](float value) { return std::isfinite(value); });
}

QuadLoweringResult
lowerPreparedQuad(const SkinFrameInputs &inputs, SkinObjectId sourceObject,
                  std::uint32_t authoredOrdinal,
                  const AuthoredDestinationGeometry &geometry,
                  SkinResourceId resourceId, int textureWidth,
                  int textureHeight, const SkinSourceRect &region,
                  bool allowCollapsedSource = false) {
  QuadLoweringResult result;
  if (geometry.rgba[3] <= 0.0F) {
    return result;
  }
  if (resourceId == 0 || textureWidth <= 0 || textureHeight <= 0 ||
      (!allowCollapsedSource && (region.w == 0 || region.h == 0))) {
    result.failure = diagnostic("skin.renderer.resource.missing",
                                "Prepared image resource or region is absent.");
    return result;
  }
  const auto projected =
      projectSkinDestinationToUi(geometry,
                                 {.textureWidth = textureWidth,
                                  .textureHeight = textureHeight,
                                  .region = region},
                                 inputs.viewport, allowCollapsedSource);
  if (!projectedQuadFitsUpload(projected)) {
    result.failure = diagnostic(
        "skin.renderer.geometry.invalid",
        "Projected image geometry is non-finite or exceeds float range.");
    return result;
  }
  bool emptyClip = false;
  const auto clip =
      intersectClip(projected.clip, projectedSkinScissorBounds(inputs.viewport),
                    emptyClip);
  if (emptyClip) {
    return result;
  }
  SkinTexturedQuadCommand quad;
  quad.resource = resourceId;
  quad.state = {
      .blend = projected.blend, .filter = projected.filter, .scissor = clip};
  const std::uint32_t color = packAbgr(projected.rgba);
  for (std::size_t vertex = 0; vertex < quad.vertices.size(); ++vertex) {
    quad.vertices[vertex] = {
        .x = static_cast<float>(projected.vertices[vertex][0]),
        .y = static_cast<float>(projected.vertices[vertex][1]),
        .u = static_cast<float>(projected.normalizedUvs[vertex][0]),
        .v = static_cast<float>(projected.normalizedUvs[vertex][1]),
        .rgba = color};
  }
  result.command = SkinDrawCommand{.authoredOrdinal = authoredOrdinal,
                                   .sourceObject = sourceObject,
                                   .payload = std::move(quad)};
  return result;
}

QuadLoweringResult lowerSpriteQuad(const SkinFrameInputs &inputs,
                                   SkinObjectId sourceObject,
                                   std::uint32_t authoredOrdinal,
                                   const AuthoredDestinationGeometry &geometry,
                                   const SkinSpriteFrames &sprite,
                                   const SkinSourceRect &selectedFrame) {
  const auto *resource = inputs.resources.find(sprite.resource);
  const auto *region =
      inputs.resources.findResolvedRegion(sprite.resource, selectedFrame);
  if (!resource || !region) {
    return {.failure =
                diagnostic("skin.renderer.resource.missing",
                           "Prepared image resource or region is absent.")};
  }
  return lowerPreparedQuad(inputs, sourceObject, authoredOrdinal, geometry,
                           sprite.resource, resource->width, resource->height,
                           region->resolved);
}

std::optional<AuthoredRect>
intersectAuthoredRects(const std::optional<AuthoredRect> &left,
                       const std::optional<AuthoredRect> &right,
                       bool &empty) noexcept {
  empty = false;
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  const double x = std::max(left->x, right->x);
  const double y = std::max(left->y, right->y);
  const double maximumX =
      std::min(left->x + left->width, right->x + right->width);
  const double maximumY =
      std::min(left->y + left->height, right->y + right->height);
  if (maximumX <= x || maximumY <= y) {
    empty = true;
    return std::nullopt;
  }
  return AuthoredRect{
      .x = x, .y = y, .width = maximumX - x, .height = maximumY - y};
}

AuthoredDestinationGeometry
gameplayVisualGeometry(const SkinAuthoredRect &rect,
                       const std::optional<AuthoredRect> &outerClip) {
  AuthoredDestinationGeometry geometry;
  geometry.rect = rect;
  geometry.clip = outerClip;
  geometry.rgba = {1.0F, 1.0F, 1.0F, 1.0F};
  geometry.blend = SkinBlendMode::Normal;
  geometry.filter = SkinFilterMode::Nearest;
  geometry.stretch = SkinStretchMode::Stretch;
  return geometry;
}

struct GameplayVisualLoweringResult {
  std::vector<SkinDrawCommand> commands;
  std::size_t primitiveVertices = 0;
  std::optional<SkinDiagnostic> failure;
};

GameplayVisualLoweringResult lowerSynthesizedNoteVisual(
    const SkinFrameInputs &inputs, SkinObjectId sourceObject,
    std::uint32_t authoredOrdinal, const AuthoredDestinationGeometry &geometry,
    const SkinSynthesizedNoteVisual &visual) {
  GameplayVisualLoweringResult result;
  std::array<float, 4> rgba{};
  bool outline = false;
  switch (visual.kind) {
  case SkinNoteVisualKind::Normal:
    rgba = {1.0F, 1.0F, 1.0F, 1.0F};
    break;
  case SkinNoteVisualKind::Mine:
    rgba = {1.0F, 0.0F, 0.0F, 1.0F};
    break;
  case SkinNoteVisualKind::Hidden:
    rgba = {1.0F, 0.5F, 0.0F, 1.0F};
    outline = true;
    break;
  case SkinNoteVisualKind::Processed:
    rgba = {0.0F, 1.0F, 1.0F, 1.0F};
    outline = true;
    break;
  default:
    rgba = {1.0F, 1.0F, 0.0F, 1.0F};
    break;
  }

  const auto emit = [&](const AuthoredDestinationGeometry &primitiveGeometry,
                        SkinPrimitiveKind kind,
                        GameplayVisualLoweringResult &output) {
    auto colored = primitiveGeometry;
    colored.rgba = rgba;
    const auto projected =
        projectSkinDestinationToUi(colored,
                                   {.textureWidth = 1,
                                    .textureHeight = 1,
                                    .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
                                   inputs.viewport);
    if (!projectedQuadFitsUpload(projected)) {
      output.failure = diagnostic(
          "skin.renderer.geometry.invalid",
          "Projected synthesized note geometry is outside float range.");
      return;
    }
    bool emptyClip = false;
    const auto clip =
        intersectClip(projected.clip, projectedSkinScissorBounds(inputs.viewport),
                      emptyClip);
    if (emptyClip) {
      return;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = kind;
    primitive.state = {.blend = SkinBlendMode::Normal,
                       .filter = SkinFilterMode::Nearest,
                       .scissor = clip};
    const std::uint32_t color = packAbgr(rgba);
    if (kind == SkinPrimitiveKind::SolidQuad) {
      primitive.vertices.reserve(4);
      for (const auto &vertex : projected.vertices) {
        primitive.vertices.push_back({.x = static_cast<float>(vertex[0]),
                                      .y = static_cast<float>(vertex[1]),
                                      .rgba = color});
      }
    } else {
      primitive.vertices.reserve(5);
      constexpr std::array<std::size_t, 5> order{0, 1, 2, 3, 0};
      for (const std::size_t index : order) {
        primitive.vertices.push_back(
            {.x = static_cast<float>(projected.vertices[index][0]),
             .y = static_cast<float>(projected.vertices[index][1]),
             .rgba = color});
      }
    }
    output.primitiveVertices += primitive.vertices.size();
    output.commands.push_back({.authoredOrdinal = authoredOrdinal,
                               .sourceObject = sourceObject,
                               .payload = std::move(primitive)});
  };

  if (!outline) {
    emit(geometry, SkinPrimitiveKind::SolidQuad, result);
    return result;
  }
  const double insetX = geometry.rect.width / 32.0;
  const double insetY = geometry.rect.height / 8.0;
  const auto strip = [&](double x, double y, double width, double height) {
    auto border = geometry;
    border.rect = {.x = x, .y = y, .width = width, .height = height};
    emit(border, SkinPrimitiveKind::SolidQuad, result);
  };
  const double x = geometry.rect.x;
  const double y = geometry.rect.y;
  const double width = geometry.rect.width;
  const double height = geometry.rect.height;
  strip(x, y, width, insetY);
  strip(x + width - insetX, y + insetY, insetX, height - insetY * 2.0);
  strip(x, y + height - insetY, width, insetY);
  strip(x, y + insetY, insetX, height - insetY * 2.0);
  strip(x + insetX, y + insetY, width - insetX * 2.0, insetY);
  strip(x + width - insetX * 2.0, y + insetY * 2.0, insetX,
        height - insetY * 4.0);
  strip(x + insetX, y + height - insetY * 2.0, width - insetX * 2.0, insetY);
  strip(x + insetX, y + insetY * 2.0, insetX, height - insetY * 4.0);
  if (result.failure) {
    return result;
  }
  return result;
}

GameplayVisualLoweringResult
lowerNoteVisual(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
                SkinObjectId sourceObject, std::uint32_t authoredOrdinal,
                const AuthoredDestinationGeometry &geometry,
                const SkinNoteVisual &visual,
                const SpriteSelection *prepared = nullptr) {
  if (const auto *sprite = std::get_if<SkinSpriteFrames>(&visual)) {
    const auto selected =
        prepared ? *prepared : selectSpriteFrame(inputs, index, *sprite);
    if (selected.failure) {
      return {.failure = *selected.failure};
    }
    if (selected.suppressed || !selected.frame) {
      return {};
    }
    auto lowered = lowerSpriteQuad(inputs, sourceObject, authoredOrdinal,
                                   geometry, *sprite, *selected.frame);
    if (lowered.failure) {
      return {.failure = *lowered.failure};
    }
    GameplayVisualLoweringResult result;
    if (lowered.command) {
      result.commands.push_back(std::move(*lowered.command));
    }
    return result;
  }
  return lowerSynthesizedNoteVisual(
      inputs, sourceObject, authoredOrdinal, geometry,
      std::get<SkinSynthesizedNoteVisual>(visual));
}

const SkinNoteVisual *findNoteVisual(const SkinLaneNotePresentation &lane,
                                     SkinNoteVisualKind kind) noexcept {
  const auto found = lane.visuals.find(kind);
  return found == lane.visuals.end() ? nullptr : &found->second;
}

std::optional<SkinNoteLineKind>
noteLineKind(SkinProjectedLineKind kind) noexcept {
  switch (kind) {
  case SkinProjectedLineKind::Group:
    return SkinNoteLineKind::Group;
  case SkinProjectedLineKind::Bpm:
    return SkinNoteLineKind::Bpm;
  case SkinProjectedLineKind::Stop:
    return SkinNoteLineKind::Stop;
  case SkinProjectedLineKind::Time:
    return SkinNoteLineKind::Time;
  }
  return std::nullopt;
}

constexpr std::size_t kNoteVisualCount = 14;
using PreparedNoteVisuals =
    std::vector<std::array<SpriteSelection, kNoteVisualCount>>;

struct NotePreparationResult {
  PreparedNoteVisuals visuals;
  std::optional<SkinDiagnostic> failure;
};

NotePreparationResult prepareNoteVisuals(const SkinFrameInputs &inputs,
                                         const FrameLookupIndex &index,
                                         const SkinNoteObject &note) {
  NotePreparationResult result;
  result.visuals.resize(note.lanes.size());
  std::array<SkinNoteVisualKind, kNoteVisualCount> prepareOrder{
      SkinNoteVisualKind::Normal,          SkinNoteVisualKind::LnEnd,
      SkinNoteVisualKind::LnStart,         SkinNoteVisualKind::LnBodyActive,
      SkinNoteVisualKind::LnBodyInactive,  SkinNoteVisualKind::HcnEnd,
      SkinNoteVisualKind::HcnStart,        SkinNoteVisualKind::HcnBodyActive,
      SkinNoteVisualKind::HcnBodyInactive, SkinNoteVisualKind::HcnDamage,
      SkinNoteVisualKind::HcnReactive,     SkinNoteVisualKind::Mine,
      SkinNoteVisualKind::Hidden,          SkinNoteVisualKind::Processed};
  if (note.hcnBodySlotLayout == SkinHcnBodySlotLayout::Modern) {
    std::swap(prepareOrder[9], prepareOrder[10]);
  }
  for (std::size_t laneIndex = 0; laneIndex < note.lanes.size(); ++laneIndex) {
    const auto &lane = note.lanes[laneIndex];
    for (const auto kind : prepareOrder) {
      const auto *visual = findNoteVisual(lane, kind);
      if (!visual) {
        result.failure = diagnostic(
            "skin.renderer.note.visual",
            "Note lane is missing a visual required during preparation.");
        return result;
      }
      const auto *sprite = std::get_if<SkinSpriteFrames>(visual);
      if (!sprite) {
        continue;
      }
      auto selected = selectSpriteFrame(inputs, index, *sprite);
      if (selected.failure) {
        result.failure = *selected.failure;
        return result;
      }
      result.visuals[laneIndex][static_cast<std::size_t>(kind)] = selected;
    }
  }
  return result;
}

bool noteOuterClipDrawable(const SkinFrameInputs &inputs,
                           const AuthoredDestinationGeometry &geometry) {
  if (!geometry.clip) {
    return true;
  }
  const auto projected =
      projectSkinDestinationToUi(geometry,
                                 {.textureWidth = 1,
                                  .textureHeight = 1,
                                  .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
                                 inputs.viewport);
  bool emptyClip = false;
  (void)intersectClip(projected.clip, projectedSkinScissorBounds(inputs.viewport),
                      emptyClip);
  return !emptyClip;
}

GameplayVisualLoweringResult
lowerNoteObject(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
                const SkinObjectDefinition &object,
                const SkinDestination &destination, const SkinNoteObject &note,
                const AuthoredDestinationGeometry *outerGeometry,
                std::span<const SkinRuntimeOffset> offsets,
                std::span<const ProjectionElement> mergedElements,
                const PreparedNoteVisuals &preparedVisuals) {
  GameplayVisualLoweringResult result;
  double offsetX = 0.0;
  double offsetY = 0.0;
  double offsetWidth = 0.0;
  double offsetHeight = 0.0;
  for (const auto &offset : offsets) {
    offsetX += offset.x;
    offsetY += offset.y;
    offsetWidth += offset.w;
    offsetHeight += offset.h;
  }

  // LaneRenderer uses lane zero for the shared playfield origin and scroll
  // height. Lift raises that origin and shortens the scroll span before every
  // normal, long, mine, invisible-note, and line projection is evaluated.
  const auto laneCover = inputs.state.laneCoverState();
  const double sharedLaneOriginY = note.lanes.front().laneDestination.y;
  const double sharedLaneHeight = note.lanes.front().laneDestination.height;
  const double liftOffsetY =
      laneCover.supported && laneCover.liftEnabled
          ? sharedLaneHeight * laneCover.lift
          : 0.0;
  const double sharedScrollHeight = sharedLaneHeight - liftOffsetY;

  float expansionWidth = 1.0F;
  float expansionHeight = 1.0F;
  const bool expansionConfigured =
      note.expansionRatePercent != std::array<int, 2>{100, 100};
  bool expansionResolved = false;
  const auto resolveExpansion = [&](GameplayVisualLoweringResult &output) {
    if (!expansionConfigured || expansionResolved) {
      return true;
    }
    const auto expansion = inputs.state.noteExpansionState();
    if (!expansion.supported || expansion.elapsedSinceQuarterNoteMillis < 0) {
      output.failure = diagnostic(
          "skin.renderer.note.expansion",
          "Non-default note expansion requires a captured quarter-note phase.");
      return false;
    }
    constexpr std::int64_t expansionMillis = 9;
    constexpr std::int64_t contractionMillis = 150;
    const std::int64_t elapsed = expansion.elapsedSinceQuarterNoteMillis;
    float pulse = 0.0F;
    if (elapsed < expansionMillis) {
      pulse = static_cast<float>(elapsed) / static_cast<float>(expansionMillis);
    } else if (elapsed <= expansionMillis + contractionMillis) {
      pulse =
          static_cast<float>(contractionMillis - (elapsed - expansionMillis)) /
          static_cast<float>(contractionMillis);
    }
    expansionWidth =
        1.0F +
        (static_cast<float>(note.expansionRatePercent[0]) / 100.0F - 1.0F) *
            pulse;
    expansionHeight =
        1.0F +
        (static_cast<float>(note.expansionRatePercent[1]) / 100.0F - 1.0F) *
            pulse;
    if (!std::isfinite(expansionWidth) || !std::isfinite(expansionHeight)) {
      output.failure =
          diagnostic("skin.renderer.note.expansion",
                     "Note expansion produced non-finite lane geometry.");
      return false;
    }
    expansionResolved = true;
    return true;
  };

  const auto expandChip = [&](double x, double y, double width, double height,
                              double referenceWidth,
                              double referenceHeight) -> SkinAuthoredRect {
    if (!expansionConfigured) {
      return {.x = x, .y = y, .width = width, .height = height};
    }
    const float expandedWidth = static_cast<float>(width) * expansionWidth;
    const float expandedHeight = static_cast<float>(height) * expansionHeight;
    return {.x = static_cast<float>(x) -
                 (expandedWidth - static_cast<float>(referenceWidth)) * 0.5F,
            .y = static_cast<float>(y) -
                 (expandedHeight - static_cast<float>(referenceHeight)) * 0.5F,
            .width = expandedWidth,
            .height = expandedHeight};
  };

  const auto appendVisual = [&](const SkinLaneNotePresentation &lane,
                                SkinNoteVisualKind kind,
                                const SkinAuthoredRect &rect,
                                GameplayVisualLoweringResult &output,
                                float opacity = 1.0F,
                                bool pmsPoorDescent = false) {
    const auto *visual = findNoteVisual(lane, kind);
    if (!visual) {
      output.failure =
          diagnostic("skin.renderer.note.visual",
                     "Projected note selected a visual absent from its lane.");
      return;
    }
    bool emptyClip = false;
    const auto laneClip =
        pmsPoorDescent
            ? (outerGeometry ? outerGeometry->clip : std::nullopt)
            : intersectAuthoredRects(
                  outerGeometry ? outerGeometry->clip : std::nullopt,
                  AuthoredRect{.x = lane.laneDestination.x,
                               .y = lane.laneDestination.y,
                               .width = lane.laneDestination.width,
                               .height = lane.laneDestination.height},
                  emptyClip);
    if (emptyClip) {
      return;
    }
    auto geometry = gameplayVisualGeometry(rect, laneClip);
    geometry.rgba[3] *= opacity;
    auto lowered = lowerNoteVisual(
        inputs, index, object.id, destination.presentation.authoredOrdinal,
        geometry,
        *visual,
        &preparedVisuals[static_cast<std::size_t>(lane.authoredLane)]
                        [static_cast<std::size_t>(kind)]);
    if (lowered.failure) {
      output.failure = std::move(lowered.failure);
      return;
    }
    if (lowered.commands.size() >
            skinFrameMaximumCommands(inputs) - output.commands.size() ||
        lowered.primitiveVertices >
            skinFrameMaximumPrimitiveVertices(inputs) -
                output.primitiveVertices) {
      output.failure =
          diagnostic("skin.renderer.command.limit",
                     "Projected note visual exceeds a fixed command limit.");
      return;
    }
    output.primitiveVertices += lowered.primitiveVertices;
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING) ||                         \
    defined(ASOBMASHOW_SKIN_RENDERER_TESTING)
    const auto slot = std::visit(
        [](const auto &selected) {
          return selected.authoredNoteSlot.value_or(-1);
        },
        *visual);
    for (auto &command : lowered.commands) {
      command.longNoteSlotForTesting = slot;
    }
#endif
    output.commands.insert(output.commands.end(),
                           std::make_move_iterator(lowered.commands.begin()),
                           std::make_move_iterator(lowered.commands.end()));
  };

  const auto laneAt = [&](int lane) -> const SkinLaneNotePresentation * {
    if (lane < 0 || static_cast<std::size_t>(lane) >= note.lanes.size()) {
      return nullptr;
    }
    const auto &presentation = note.lanes[static_cast<std::size_t>(lane)];
    return presentation.authoredLane == lane ? &presentation : nullptr;
  };

  const auto resolveScrollDisplacement =
      [&](double authoredDisplacement,
          const std::optional<double> &scrollSpeed) -> double {
    if (!scrollSpeed) {
      // Unit-test and non-gameplay callers historically publish pixels.
      return authoredDisplacement;
    }
    // Beatoraja LaneRenderer derives one shared rxhs from lanes[0].region,
    // then applies it to every note and timeline line. `scrollSpeed` is the
    // captured hispeed, so this is its exact skin-owned pixel conversion.
    return authoredDisplacement * sharedScrollHeight * *scrollSpeed;
  };

  const auto lowerProjectedNote = [&](const SkinProjectedNoteView &projected,
                                      GameplayVisualLoweringResult &output) {
    const auto *lane = laneAt(projected.lane);
    const bool pmsPoorDescent = projected.pmsPoorYDisplacement.has_value();
    if (!lane || !std::isfinite(projected.authoredYDisplacement)) {
      output.failure = diagnostic(
          "skin.renderer.note.projection",
          "Projected note lane or authored displacement is invalid.");
      return;
    }
    const double authoredY =
        pmsPoorDescent ? *projected.pmsPoorYDisplacement
                        : resolveScrollDisplacement(
                              projected.authoredYDisplacement,
                              projected.scrollSpeed);
    if (!std::isfinite(authoredY)) {
      output.failure = diagnostic(
          "skin.renderer.note.projection",
          "Projected note lane or authored displacement is invalid.");
      return;
    }
    SkinNoteVisualKind kind = SkinNoteVisualKind::Normal;
    bool applyOffsets = true;
    switch (projected.kind) {
    case SkinProjectedNoteKind::Normal:
      // LaneRenderer selects processedImage only when PlayerConfig's
      // Mark Processed Note option is enabled; the default is normalImage.
      kind = !pmsPoorDescent && projected.judged && inputs.markProcessedNotes
                 ? SkinNoteVisualKind::Processed
                 : SkinNoteVisualKind::Normal;
      break;
    case SkinProjectedNoteKind::Invisible:
      kind = SkinNoteVisualKind::Hidden;
      applyOffsets = false;
      break;
    case SkinProjectedNoteKind::Mine:
      kind = SkinNoteVisualKind::Mine;
      break;
    }
    if ((applyOffsets || pmsPoorDescent) && !resolveExpansion(output)) {
      return;
    }
    const double noteHeight = lane->authoredNoteHeight.value_or(8.0);
    SkinAuthoredRect rect;
    if (pmsPoorDescent) {
      rect = expandChip(lane->laneDestination.x,
                        sharedLaneOriginY + liftOffsetY + authoredY,
                        lane->laneDestination.width, noteHeight,
                        lane->laneDestination.width, noteHeight);
    } else if (applyOffsets) {
      rect = expandChip(
          lane->laneDestination.x + offsetX,
          sharedLaneOriginY + liftOffsetY + authoredY + offsetY,
          lane->laneDestination.width + offsetWidth, noteHeight + offsetHeight,
          lane->laneDestination.width, noteHeight);
    } else {
      rect = {.x = lane->laneDestination.x,
              .y = sharedLaneOriginY + liftOffsetY + authoredY,
              .width = lane->laneDestination.width,
              .height = noteHeight};
    }
    // LaneRenderer's Constant processor sets the sprite color before this
    // row; authored destination alpha remains multiplicative.
    appendVisual(*lane, kind, rect, output,
                 static_cast<float>(projected.opacity), pmsPoorDescent);
  };

  const auto lowerProjectedLongNote =
      [&](const SkinProjectedLongNoteView &projected,
          GameplayVisualLoweringResult &output) {
        const auto *lane = laneAt(projected.lane);
        if (!lane || !std::isfinite(projected.headAuthoredYDisplacement) ||
            !std::isfinite(projected.tailAuthoredYDisplacement)) {
          output.failure = diagnostic(
              "skin.renderer.note.projection",
              "Projected long-note lane or authored displacement is invalid.");
          return;
        }
        const double headY = resolveScrollDisplacement(
            projected.headAuthoredYDisplacement, projected.scrollSpeed);
        const double tailAuthoredY = resolveScrollDisplacement(
            projected.tailAuthoredYDisplacement, projected.scrollSpeed);
        if (!std::isfinite(headY) || !std::isfinite(tailAuthoredY)) {
          output.failure = diagnostic(
              "skin.renderer.note.projection",
              "Projected long-note lane or authored displacement is invalid.");
          return;
        }
        if (!resolveExpansion(output)) {
          return;
        }
        const double referenceHeight = lane->authoredNoteHeight.value_or(8.0);
        const double displacement = tailAuthoredY - headY;
        if (displacement <= 0.0) {
          return;
        }
        const auto chip =
            expandChip(lane->laneDestination.x + offsetX,
                       sharedLaneOriginY + liftOffsetY + headY + offsetY,
                       lane->laneDestination.width + offsetWidth,
                       referenceHeight + offsetHeight,
                       lane->laneDestination.width, referenceHeight);
        const double tailY = chip.y + displacement;
        SkinNoteVisualKind body = SkinNoteVisualKind::LnBodyInactive;
        SkinNoteVisualKind end = SkinNoteVisualKind::LnEnd;
        SkinNoteVisualKind start = SkinNoteVisualKind::LnStart;
        if (projected.mode == SkinProjectedLongNoteMode::HCN) {
          end = SkinNoteVisualKind::HcnEnd;
          start = SkinNoteVisualKind::HcnStart;
          if (projected.active) {
            body = SkinNoteVisualKind::HcnBodyActive;
          } else if (projected.reactive) {
            body = note.hcnBodySlotLayout == SkinHcnBodySlotLayout::Modern
                       ? SkinNoteVisualKind::HcnReactive
                       : SkinNoteVisualKind::HcnDamage;
          } else if (projected.damaged) {
            body = note.hcnBodySlotLayout == SkinHcnBodySlotLayout::Modern
                       ? SkinNoteVisualKind::HcnDamage
                       : SkinNoteVisualKind::HcnReactive;
          } else {
            body = SkinNoteVisualKind::HcnBodyInactive;
          }
        } else {
          body = projected.active ? SkinNoteVisualKind::LnBodyActive
                                  : SkinNoteVisualKind::LnBodyInactive;
        }
        appendVisual(*lane, body,
                     {.x = chip.x,
                      .y = chip.y + chip.height,
                      .width = chip.width,
                      .height = displacement - chip.height},
                     output, static_cast<float>(projected.opacity));
        if (output.failure) {
          return;
        }
        if (projected.mode != SkinProjectedLongNoteMode::LN) {
          appendVisual(*lane, end,
                       {.x = chip.x,
                        .y = tailY,
                        .width = chip.width,
                        .height = chip.height},
                       output, static_cast<float>(projected.opacity));
          if (output.failure) {
            return;
          }
        }
        appendVisual(*lane, start, chip, output,
                     static_cast<float>(projected.opacity));
      };

  const auto lowerProjectedLine = [&](const SkinProjectedLineView &projected,
                                      GameplayVisualLoweringResult &output) {
    if (note.lanes.empty() ||
        !std::isfinite(projected.authoredYDisplacement)) {
      output.failure =
          diagnostic("skin.renderer.note.projection",
                     "Projected line authored displacement is invalid.");
      return;
    }
    const double authoredY = resolveScrollDisplacement(
        projected.authoredYDisplacement, projected.scrollSpeed);
    if (!std::isfinite(authoredY)) {
      output.failure =
          diagnostic("skin.renderer.note.projection",
                     "Projected line authored displacement is invalid.");
      return;
    }
    const auto requestedKind = noteLineKind(projected.kind);
    const std::size_t groupCount = note.lines.size() / 4;
    if (!requestedKind || note.lines.size() % 4 != 0 || groupCount == 0) {
      output.failure = diagnostic(
          "skin.renderer.note.line",
          "Projected timeline line has no valid lane-group presentation.");
      return;
    }
    (void)projected.timelineVisualId;
    const std::size_t kindIndex = static_cast<std::size_t>(*requestedKind);
    for (std::size_t group = 0; group < groupCount; ++group) {
      const auto &line = note.lines[kindIndex * groupCount + group];
      if (line.kind != *requestedKind) {
        output.failure = diagnostic(
            "skin.renderer.note.line",
            "Projected timeline line kind does not match the model.");
        return;
      }
      if (!line.sprite || !line.destination) {
        output.failure =
            diagnostic("skin.renderer.note.line",
                       "Projected timeline line selected a sparse model hole.");
        return;
      }
      auto evaluated = resolveDestination(inputs, index, *line.destination);
      if (!evaluated.failures.empty()) {
        output.failure = std::move(evaluated.failures.front());
        return;
      }
      const auto selected = selectSpriteFrame(inputs, index, *line.sprite);
      if (selected.failure) {
        output.failure = *selected.failure;
        return;
      }
      if (!evaluated.geometry || selected.suppressed || !selected.frame) {
        continue;
      }
      evaluated.geometry->rect.y += authoredY;
      evaluated.geometry->rgba[3] *= static_cast<float>(projected.opacity);
      // LaneRenderer invokes nested line images directly, so their own clip
      // is inert. The containing Note clip remains active for every group.
      // Unlike the thin group-image destination, the default vertical play
      // area comes from the lane geometry used by LaneRenderer's shared
      // scroll calculation. The group image supplies its horizontal span.
      bool emptyClip = false;
      evaluated.geometry->clip = intersectAuthoredRects(
          outerGeometry ? outerGeometry->clip : std::nullopt,
          AuthoredRect{.x = line.laneGroupDestination.x,
                       .y = note.lanes.front().laneDestination.y,
                       .width = line.laneGroupDestination.width,
                       .height = note.lanes.front().laneDestination.height},
          emptyClip);
      if (emptyClip) {
        continue;
      }
      auto lowered = lowerSpriteQuad(
          inputs, object.id, destination.presentation.authoredOrdinal,
          *evaluated.geometry, *line.sprite, *selected.frame);
      if (lowered.failure) {
        output.failure = *lowered.failure;
        return;
      }
      if (lowered.command) {
        if (output.commands.size() >= skinFrameMaximumCommands(inputs)) {
          output.failure =
              diagnostic("skin.renderer.command.limit",
                         "Projected line exceeds the fixed command limit.");
          return;
        }
        output.commands.push_back(std::move(*lowered.command));
      }
    }
  };

  const auto notes = inputs.state.projectedNotes();
  const auto longNotes = inputs.state.projectedLongNotes();
  const auto lines = inputs.state.projectedLines();
  for (const auto &element : mergedElements) {
    switch (element.kind) {
    case ProjectionElementKind::Note:
      lowerProjectedNote(notes[element.index], result);
      break;
    case ProjectionElementKind::LongNote:
      lowerProjectedLongNote(longNotes[element.index], result);
      break;
    case ProjectionElementKind::Line:
      lowerProjectedLine(lines[element.index], result);
      break;
    }
    if (result.failure) {
      result.commands.clear();
      result.primitiveVertices = 0;
      return result;
    }
  }
  return result;
}

struct JudgeLoweringResult {
  std::vector<SkinDrawCommand> commands;
  std::optional<SkinDiagnostic> failure;
};

JudgeLoweringResult
lowerJudge(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
           std::span<const SkinObjectDefinition *const> objects,
           const SkinDestination &outerDestination,
           const SkinJudgeObject &judge, const SkinJudgeStateView &state,
           const AuthoredDestinationGeometry *outerGeometry,
           bool wrapperVisible) {
  JudgeLoweringResult result;
  if (!state.supported) {
    result.failure =
        diagnostic("skin.renderer.judge.state", "Judge state is unsupported.");
    return result;
  }
  if (!state.optionalZeroBasedGrade) {
    return result;
  }
  const int grade = *state.optionalZeroBasedGrade;
  if (grade < 0 || grade > 5) {
    result.failure = diagnostic("skin.renderer.judge.state",
                                "Judge grade is outside the supported range.");
    return result;
  }

  const SkinNestedObjectPresentation *imagePresentation = nullptr;
  const SkinNestedObjectPresentation *detailPresentation = nullptr;
  const auto presentationAt =
      [&](std::size_t index) -> const SkinJudgeGradePresentation * {
    return index < judge.grades.size() ? &judge.grades[index] : nullptr;
  };
  const auto *selected = presentationAt(static_cast<std::size_t>(grade));
  if (grade == 0 && state.maximumGauge) {
    const auto *maximum = presentationAt(6);
    if (maximum && maximum->image) {
      imagePresentation = &*maximum->image;
    } else if (selected && selected->image) {
      imagePresentation = &*selected->image;
    }
    if (maximum && maximum->detailNumber) {
      detailPresentation = &*maximum->detailNumber;
    } else if (selected && selected->detailNumber) {
      detailPresentation = &*selected->detailNumber;
    }
  } else if (selected) {
    if (selected->image) {
      imagePresentation = &*selected->image;
    }
    if (grade < 3 && selected->detailNumber) {
      detailPresentation = &*selected->detailNumber;
    }
  }
  // Pinned SkinJudge simply has nothing to draw when a selected image slot is
  // absent. A detail number never renders without its judge image.
  if (!imagePresentation) {
    return result;
  }

  const auto *imageObject = findObject(objects, imagePresentation->object);
  const auto *image = imageObject
                          ? std::get_if<SkinImageObject>(&imageObject->payload)
                          : nullptr;
  if (!imageObject || !image || image->orderedStates.size() != 1 ||
      image->stateIndex) {
    result.failure = diagnostic(
        "skin.renderer.judge.image",
        "Judge image presentation does not reference one inline image state.");
    return result;
  }
  auto imageDestination =
      resolveDestination(inputs, index, imagePresentation->destination);
  if (!imageDestination.failures.empty()) {
    result.failure = std::move(imageDestination.failures.front());
    return result;
  }
  const auto &imageSprite = image->orderedStates.front();
  const auto selectedImage = selectSpriteFrame(inputs, index, imageSprite);
  if (selectedImage.failure) {
    result.failure = *selectedImage.failure;
    return result;
  }
  if (selectedImage.suppressed || !selectedImage.frame) {
    return result;
  }
  if (!imageDestination.geometry) {
    return result;
  }
  imageDestination.geometry->clip =
      outerGeometry ? outerGeometry->clip : std::nullopt;
  const auto *preparedImage = inputs.resources.find(imageSprite.resource);
  const auto *preparedImageRegion = inputs.resources.findResolvedRegion(
      imageSprite.resource, *selectedImage.frame);
  if (!preparedImage || !preparedImageRegion || preparedImage->width <= 0 ||
      preparedImage->height <= 0 || preparedImageRegion->resolved.w <= 0 ||
      preparedImageRegion->resolved.h <= 0) {
    result.failure =
        diagnostic("skin.renderer.resource.missing",
                   "Prepared judge image resource or region is absent.");
    return result;
  }

  double detailLength = 0.0;
  if (detailPresentation) {
    const auto *detailObject = findObject(objects, detailPresentation->object);
    const auto *number =
        detailObject ? std::get_if<SkinNumberObject>(&detailObject->payload)
                     : nullptr;
    if (!detailObject || !number || !number->relativeToJudgeImage) {
      result.failure = diagnostic(
          "skin.renderer.judge.detail",
          "Judge detail presentation does not reference a relative number.");
      return result;
    }
    auto layout = prepareNumberLayoutForValue(*number, state.combo);
    if (layout.failure) {
      result.failure = *layout.failure;
      return result;
    }
    if (!layout.suppressed) {
      auto detailDestination = resolveDestination(
          inputs, index, detailPresentation->destination, true);
      if (!detailDestination.failures.empty()) {
        result.failure = std::move(detailDestination.failures.front());
        return result;
      }
      if (detailDestination.geometry) {
        detailDestination.geometry->rect.x += imageDestination.geometry->rect.x;
        detailDestination.geometry->rect.y += imageDestination.geometry->rect.y;
        // Nested Judge children are prepared directly and are never passed
        // through Skin.drawObject. The outer Judge owns the only active clip.
        detailDestination.geometry->clip =
            outerGeometry ? outerGeometry->clip : std::nullopt;
        if (!selectNumericAnimation(inputs, index, layout)) {
          if (layout.failure) {
            result.failure = *layout.failure;
          }
          return result;
        }
        const std::size_t visible = static_cast<std::size_t>(
            std::count_if(layout.glyphs.begin(), layout.glyphs.end(),
                          [](int glyph) { return glyph >= 0; }));
        detailLength =
            (detailDestination.geometry->rect.width + layout.spacing) *
            static_cast<double>(visible);
        if (wrapperVisible && detailDestination.geometry->rgba[3] > 0.0F) {
          SkinDestination nestedDestination{
              .object = detailObject->id,
              .presentation = detailPresentation->destination};
          nestedDestination.presentation.authoredOrdinal =
              outerDestination.presentation.authoredOrdinal;
          auto lowered = lowerNumeric(inputs, *detailObject, nestedDestination,
                                      *detailDestination.geometry, layout);
          if (lowered.failure) {
            result.failure = *lowered.failure;
            return result;
          }
          result.commands = std::move(lowered.commands);
        }
      }
    }
  }

  if (judge.shiftImageByHalfDetailWidth) {
    imageDestination.geometry->rect.x -= detailLength * 0.5;
  }
  // SkinJudge has its own constructor destination, so a top-level Judge may
  // legitimately be registered without `dst` frames. Its nested image still
  // owns the rendered geometry; the absent wrapper only means no outer clip.
  // A configured wrapper with frames remains a normal SkinObject: its failed
  // destination condition suppresses draw while nested children still prepare.
  if (!wrapperVisible || imageDestination.geometry->rgba[3] <= 0.0F) {
    return result;
  }
  auto loweredImage = lowerPreparedQuad(
      inputs, imageObject->id, outerDestination.presentation.authoredOrdinal,
      *imageDestination.geometry, imageSprite.resource, preparedImage->width,
      preparedImage->height, preparedImageRegion->resolved);
  if (loweredImage.failure) {
    result.failure = *loweredImage.failure;
    result.commands.clear();
    return result;
  }
  if (loweredImage.command) {
    result.commands.push_back(std::move(*loweredImage.command));
  }
  return result;
}

struct PreparedMusicSelectPresentation {
  const SkinObjectDefinition *object = nullptr;
  std::optional<AuthoredDestinationGeometry> geometry;
  const SkinSpriteFrames *sprite = nullptr;
  const SkinSourceRect *spriteFrame = nullptr;
  std::optional<NumericLayout> number;
  std::optional<TextLayoutInput> text;
  std::vector<const SkinSourceRect *> graphFrames;
  std::vector<SkinDiagnostic> failures;
};

struct MusicSelectSongListLoweringResult {
  std::vector<SkinDrawCommand> commands;
  std::vector<SkinMusicSelectBarInteractionGeometry> interactions;
  std::vector<SkinDiagnostic> failures;
  std::size_t glyphCount = 0;
};

void translateMusicSelectGeometry(AuthoredDestinationGeometry &geometry,
                                  double x, double y) {
  geometry.rect.x += x;
  geometry.rect.y += y;
  if (geometry.clip) {
    geometry.clip->x += x;
    geometry.clip->y += y;
  }
}

PreparedMusicSelectPresentation prepareMusicSelectPresentation(
    const SkinFrameInputs &inputs, const FrameLookupIndex &index,
    std::span<const SkinObjectDefinition *const> objects,
    const SkinSongListPresentation &presentation,
    std::size_t maximumCodepoints,
    std::optional<int> forcedImageState = std::nullopt,
    std::optional<std::int64_t> forcedNumberValue = std::nullopt) {
  PreparedMusicSelectPresentation result;
  if (presentation.object == 0) {
    return result;
  }
  result.object = findObject(objects, presentation.object);
  if (result.object == nullptr) {
    result.failures.push_back(diagnostic(
        "skin.renderer.music_select.object_missing",
        "Song-list presentation references an absent skin object."));
    return result;
  }
  auto destination =
      resolveDestination(inputs, index, presentation.destination);
  result.failures = std::move(destination.failures);
  result.geometry = std::move(destination.geometry);
  if (!result.failures.empty() || !result.geometry) {
    return result;
  }

  if (const auto *image =
          std::get_if<SkinImageObject>(&result.object->payload)) {
    if (image->orderedStates.empty()) {
      result.failures.push_back(diagnostic(
          "skin.renderer.image.states",
          "Song-list image object has no source states."));
      return result;
    }
    std::int64_t state = forcedImageState.value_or(0);
    if (!forcedImageState && image->stateIndex) {
      const auto resolved = resolveInteger(inputs, index, *image->stateIndex);
      if (resolved.failure) {
        result.failures.push_back(*resolved.failure);
        return result;
      }
      state = *resolved.value;
    }
    if (state < 0) {
      result.geometry.reset();
      return result;
    }
    std::size_t stateIndex = static_cast<std::size_t>(state);
    if (stateIndex >= image->orderedStates.size()) {
      stateIndex = 0;
    }
    result.sprite = &image->orderedStates[stateIndex];
    const auto selected = selectSpriteFrame(inputs, index, *result.sprite);
    if (selected.failure) {
      result.failures.push_back(*selected.failure);
      return result;
    }
    if (selected.suppressed || selected.frame == nullptr) {
      result.geometry.reset();
      return result;
    }
    result.spriteFrame = selected.frame;
    return result;
  }

  if (const auto *number =
          std::get_if<SkinNumberObject>(&result.object->payload)) {
    result.number = forcedNumberValue
                        ? prepareNumberLayoutForValue(*number,
                                                      *forcedNumberValue)
                        : prepareNumberLayout(inputs, index, *number);
    if (result.number->failure) {
      result.failures.push_back(*result.number->failure);
      return result;
    }
    if (result.number->suppressed ||
        !selectNumericAnimation(inputs, index, *result.number)) {
      if (result.number->failure) {
        result.failures.push_back(*result.number->failure);
      }
      result.geometry.reset();
    }
    return result;
  }

  if (const auto *text =
          std::get_if<SkinTextObject>(&result.object->payload)) {
    result.text = prepareTextLayout(inputs, index, *result.object, *text,
                                    maximumCodepoints);
    if (result.text->failure) {
      result.failures.push_back(*result.text->failure);
      return result;
    }
    return result;
  }

  if (const auto *graph =
          std::get_if<SkinSelectDistributionGraphObject>(
              &result.object->payload)) {
    const std::size_t states =
        graph->type == SkinSelectDistributionGraphType::Normal ? 11U : 28U;
    if (graph->sprite.resource == 0 || graph->sprite.frames.empty() ||
        graph->sprite.frames.size() % states != 0) {
      result.failures.push_back(diagnostic(
          "skin.renderer.music_select.graph_sprite",
          "Song-list distribution graph has an invalid source grid."));
      return result;
    }
    const std::size_t animationFrames = graph->sprite.frames.size() / states;
    result.graphFrames.reserve(states);
    for (std::size_t state = 0; state < states; ++state) {
      const auto selected = selectAnimationFrame(
          inputs, index, animationFrames, graph->sprite.cycleMillis,
          graph->sprite.timer);
      if (selected.failure) {
        result.failures.push_back(*selected.failure);
        return result;
      }
      result.graphFrames.push_back(
          selected.suppressed
              ? nullptr
              : &graph->sprite.frames[selected.frame * states + state]);
    }
    return result;
  }

  result.failures.push_back(diagnostic(
      "skin.renderer.music_select.object_unsupported",
      "Song-list presentation resolved to an unsupported object type."));
  return result;
}

MusicSelectSongListLoweringResult lowerMusicSelectSongList(
    const SkinFrameInputs &inputs, const FrameLookupIndex &index,
    std::span<const SkinObjectDefinition *const> objects,
    const SkinDestination &outerDestination,
    const SkinSongListObject &songList, std::size_t maximumCommands,
    std::size_t maximumGlyphs) {
  MusicSelectSongListLoweringResult result;
  if (inputs.musicSelectSongList == nullptr) {
    result.failures.push_back(diagnostic(
        "skin.renderer.music_select.state_missing",
        "Song-list rendering requires a music-select frame snapshot."));
    return result;
  }

  // SkinBar.prepare calls its wrapper first, then prepares every nested object
  // in these exact field orders before BarRenderer calculates its 60 rows.
  const bool hasAuthoredOuterFrames =
      !outerDestination.presentation.frames.empty();
  const auto outer = hasAuthoredOuterFrames
                         ? resolveDestination(
                               inputs, index, outerDestination.presentation)
                         : DestinationResolution{};
  result.failures.insert(result.failures.end(), outer.failures.begin(),
                         outer.failures.end());
  std::map<const SkinSongListPresentation *, PreparedMusicSelectPresentation>
      prepared;
  const auto prepareAll = [&](const auto &presentations) {
    for (const auto &presentation : presentations) {
      prepared.emplace(
          &presentation,
          prepareMusicSelectPresentation(inputs, index, objects,
                                         presentation, maximumGlyphs));
    }
  };
  prepareAll(songList.listOn);
  prepareAll(songList.listOff);
  prepareAll(songList.trophy);
  prepareAll(songList.text);
  prepareAll(songList.level);
  prepareAll(songList.label);
  prepareAll(songList.lamp);
  prepareAll(songList.playerLamp);
  prepareAll(songList.rivalLamp);
  if (songList.graph) {
    prepareAll(std::span<const SkinSongListPresentation>{&*songList.graph, 1});
  }
  // JsonSelectSkinObjectLoader constructs every SkinBar child independently.
  // A null or unpreparable child is simply absent from that bar; it does not
  // invalidate the SkinBar wrapper or the other children.
  if (!result.failures.empty() || (hasAuthoredOuterFrames && !outer.geometry)) {
    return result;
  }

  const auto plan = MusicSelectBarRenderer{}.plan(
      songList, *inputs.musicSelectSongList);
  if (plan.failure) {
    result.failures.push_back(
        diagnostic("skin.renderer.music_select.plan", *plan.failure));
    return result;
  }

  struct RowGeometry {
    bool drawable = false;
    std::size_t barIndex = 0;
    double preparedX = 0.0;
    double preparedY = 0.0;
    double x = 0.0;
    double y = 0.0;
    AuthoredRect pointerRegion;
  };
  std::array<RowGeometry, MusicSelectBarRenderer::barCount> rows{};
  for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
    const auto &logical = plan.rows[rowIndex];
    const bool selected = static_cast<int>(rowIndex) == songList.center;
    const auto &bars = selected ? songList.listOn : songList.listOff;
    if (rowIndex >= bars.size() || logical.value == -1) {
      continue;
    }
    const auto &bar = bars[rowIndex];
    const auto found = prepared.find(&bar);
    if (found == prepared.end() || !found->second.geometry) {
      continue;
    }
    auto &row = rows[rowIndex];
    row.drawable = true;
    row.barIndex = logical.barIndex;
    row.preparedX = found->second.geometry->rect.x;
    row.preparedY = found->second.geometry->rect.y;
    row.x = row.preparedX;
    row.y = row.preparedY;
    row.pointerRegion = found->second.geometry->rect;
  }

  const auto &frame = *inputs.musicSelectSongList;
  if (frame.movementDirection != 0 &&
      frame.movementEndMillis > frame.wallClockMillis) {
    const double interpolation =
        frame.movementDirection < 0
            ? static_cast<double>(frame.wallClockMillis -
                                  frame.movementEndMillis) /
                  frame.movementDirection
            : static_cast<double>(frame.movementEndMillis -
                                  frame.wallClockMillis) /
                  frame.movementDirection;
    const auto stationary = rows;
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
      auto &row = rows[rowIndex];
      if (!row.drawable) {
        continue;
      }
      const int adjacent = static_cast<int>(rowIndex) +
                           (frame.movementDirection >= 0 ? 1 : -1);
      if (adjacent < 0 || adjacent >= static_cast<int>(rows.size()) ||
          !stationary[static_cast<std::size_t>(adjacent)].drawable) {
        continue;
      }
      const auto &next = stationary[static_cast<std::size_t>(adjacent)];
      row.x += (next.x - row.x) *
               std::clamp(interpolation, -1.0, 1.0);
      row.y += (next.y - row.y) * interpolation;
    }
  }
  for (auto &row : rows) {
    if (row.drawable) {
      row.x = static_cast<double>(truncatingJavaInt(row.x));
      row.y = static_cast<double>(truncatingJavaInt(row.y));
    }
  }

  const auto presentationFor = [&](const MusicSelectBarDrawCommand &command)
      -> const SkinSongListPresentation * {
    const auto from = [&](const auto &values) {
      return command.slot < values.size() ? &values[command.slot] : nullptr;
    };
    switch (command.family) {
    case MusicSelectBarDrawFamily::BarImage: {
      const bool selected =
          static_cast<int>(command.row) == songList.center;
      return from(selected ? songList.listOn : songList.listOff);
    }
    case MusicSelectBarDrawFamily::FolderGraph:
      return songList.graph ? &*songList.graph : nullptr;
    case MusicSelectBarDrawFamily::Title:
      return from(songList.text);
    case MusicSelectBarDrawFamily::Trophy:
      return from(songList.trophy);
    case MusicSelectBarDrawFamily::Lamp:
      return from(songList.lamp);
    case MusicSelectBarDrawFamily::PlayerLamp:
      return from(songList.playerLamp);
    case MusicSelectBarDrawFamily::RivalLamp:
      return from(songList.rivalLamp);
    case MusicSelectBarDrawFamily::Level:
      return from(songList.level);
    case MusicSelectBarDrawFamily::Label:
      return from(songList.label);
    }
    return nullptr;
  };
  const auto fakeDestination = [&](const SkinSongListPresentation &value) {
    SkinDestination destination{.object = value.object,
                                .presentation = value.destination};
    destination.presentation.authoredOrdinal =
        outerDestination.presentation.authoredOrdinal;
    return destination;
  };
  const auto appendCommand = [&](SkinDrawCommand command) {
    if (result.commands.size() >= maximumCommands) {
      return false;
    }
    command.authoredOrdinal = outerDestination.presentation.authoredOrdinal;
    result.commands.push_back(std::move(command));
    return true;
  };

  for (const auto &command : plan.commands) {
    if (command.row >= rows.size() || !rows[command.row].drawable) {
      continue;
    }
    const auto *presentation = presentationFor(command);
    if (presentation == nullptr || presentation->object == 0) {
      continue;
    }
    const auto &row = rows[command.row];
    const auto found = prepared.find(presentation);
    if (found == prepared.end() || found->second.object == nullptr) {
      continue;
    }
    const auto &initial = found->second;
    const auto *image =
        std::get_if<SkinImageObject>(&initial.object->payload);
    const auto *number =
        std::get_if<SkinNumberObject>(&initial.object->payload);
    const auto *text = std::get_if<SkinTextObject>(&initial.object->payload);
    const auto *graph =
        std::get_if<SkinSelectDistributionGraphObject>(
            &initial.object->payload);

    if (command.family == MusicSelectBarDrawFamily::BarImage ||
        command.family == MusicSelectBarDrawFamily::Level) {
      auto dynamic = prepareMusicSelectPresentation(
          inputs, index, objects, *presentation,
          maximumGlyphs - std::min(maximumGlyphs, result.glyphCount),
          command.family == MusicSelectBarDrawFamily::BarImage
              ? std::optional<int>{command.value}
              : std::nullopt,
          command.family == MusicSelectBarDrawFamily::Level
              ? std::optional<std::int64_t>{command.value}
              : std::nullopt);
      if (!dynamic.geometry) {
        continue;
      }
      if (command.family == MusicSelectBarDrawFamily::BarImage) {
        translateMusicSelectGeometry(*dynamic.geometry,
                                     row.x - row.preparedX,
                                     row.y - row.preparedY);
      } else {
        // BarRenderer supplies the current bar origin when it draws a level.
        // Level destinations are authored relative to that origin.
        translateMusicSelectGeometry(*dynamic.geometry, row.x, row.y);
      }
      if (image && dynamic.sprite && dynamic.spriteFrame) {
        auto lowered = lowerSpriteQuad(
            inputs, initial.object->id,
            outerDestination.presentation.authoredOrdinal, *dynamic.geometry,
            *dynamic.sprite, *dynamic.spriteFrame);
        if (!lowered.failure && lowered.command &&
                   !appendCommand(std::move(*lowered.command))) {
          result.failures.push_back(diagnostic(
              "skin.renderer.command.limit",
              "Song-list commands exceed the fixed frame limit."));
          return result;
        }
      } else if (number && dynamic.number) {
        auto destination = fakeDestination(*presentation);
        auto lowered = lowerNumeric(inputs, *initial.object, destination,
                                    *dynamic.geometry, *dynamic.number);
        if (!lowered.failure && lowered.commands.size() >
                   maximumCommands - result.commands.size()) {
          result.failures.push_back(diagnostic(
              "skin.renderer.command.limit",
              "Song-list number commands exceed the fixed frame limit."));
          return result;
        } else {
          result.commands.insert(
              result.commands.end(),
              std::make_move_iterator(lowered.commands.begin()),
              std::make_move_iterator(lowered.commands.end()));
        }
      }
      continue;
    }

    if (!initial.geometry) {
      continue;
    }
    auto geometry = *initial.geometry;
    translateMusicSelectGeometry(geometry, row.x, row.y);
    if (image && initial.sprite && initial.spriteFrame) {
      auto lowered = lowerSpriteQuad(
          inputs, initial.object->id,
          outerDestination.presentation.authoredOrdinal, geometry,
          *initial.sprite, *initial.spriteFrame);
      if (!lowered.failure && lowered.command &&
                 !appendCommand(std::move(*lowered.command))) {
        result.failures.push_back(diagnostic(
            "skin.renderer.command.limit",
            "Song-list commands exceed the fixed frame limit."));
        return result;
      }
      continue;
    }
    if (text) {
      auto layout = prepareTextLayoutForValue(
          inputs, *initial.object, *text, command.text,
          maximumGlyphs - std::min(maximumGlyphs, result.glyphCount));
      if (layout.failure) {
        continue;
      }
      if (layout.suppressed) {
        continue;
      }
      auto destination = fakeDestination(*presentation);
      auto lowered = lowerText(inputs, *initial.object, destination, geometry,
                               *text, layout);
      if (!lowered.failure && lowered.command &&
                 (result.commands.size() >= maximumCommands ||
                  lowered.glyphCount > maximumGlyphs - result.glyphCount)) {
        result.failures.push_back(diagnostic(
            "skin.renderer.command.limit",
            "Song-list text commands exceed a fixed frame limit."));
        return result;
      } else if (lowered.command) {
        result.glyphCount += lowered.glyphCount;
        appendCommand(std::move(*lowered.command));
      }
      continue;
    }
    if (graph) {
      const auto counts =
          graph->type == SkinSelectDistributionGraphType::Normal
              ? std::span<const int>(command.folderLampCounts)
              : std::span<const int>(command.folderRankCounts);
      const int total = std::accumulate(counts.begin(), counts.end(), 0);
      if (total <= 0 || initial.graphFrames.size() != counts.size()) {
        continue;
      }
      int prior = 0;
      for (std::size_t reversed = counts.size(); reversed > 0; --reversed) {
        const std::size_t state = reversed - 1;
        const int count = counts[state];
        const auto *frameRegion = initial.graphFrames[state];
        if (count <= 0 || frameRegion == nullptr) {
          prior += std::max(0, count);
          continue;
        }
        auto segment = geometry;
        segment.rect.x += segment.rect.width * prior / total;
        segment.rect.width = segment.rect.width * count / total;
        auto lowered = lowerSpriteQuad(
            inputs, initial.object->id,
            outerDestination.presentation.authoredOrdinal, segment,
            graph->sprite, *frameRegion);
        if (!lowered.failure && lowered.command &&
                   !appendCommand(std::move(*lowered.command))) {
          result.failures.push_back(diagnostic(
              "skin.renderer.command.limit",
              "Song-list graph commands exceed the fixed frame limit."));
          return result;
        }
        prior += count;
      }
    }
  }

  for (const int clickable : songList.clickable) {
    if (clickable < 0 || clickable >= static_cast<int>(rows.size())) {
      continue;
    }
    const auto &row = rows[static_cast<std::size_t>(clickable)];
    if (!row.drawable) {
      continue;
    }
    result.interactions.push_back(
        {.authoredOrdinal = outerDestination.presentation.authoredOrdinal,
         .row = static_cast<std::size_t>(clickable),
         .barIndex = row.barIndex,
         .authoredRegion = row.pointerRegion});
  }
  return result;
}

bool reportObjectFailure(SkinFrameEvaluationResult &result,
                         const SkinObjectDefinition &object,
                         SkinDiagnostic failure) {
  if (!object.critical) {
    failure.severity = DiagnosticSeverity::Warning;
  }
  result.diagnostics.push_back(std::move(failure));
  return object.critical;
}

} // namespace

std::optional<AuthoredPoint>
SkinInteractionLayout::authoredPointForUi(double x, double y) const noexcept {
  if (!std::isfinite(x) || !std::isfinite(y) || !finiteAffine(uiToAuthored)) {
    return std::nullopt;
  }
  const AuthoredPoint result{
      .x = uiToAuthored.m00 * x + uiToAuthored.m01 * y + uiToAuthored.tx,
      .y = uiToAuthored.m10 * x + uiToAuthored.m11 * y + uiToAuthored.ty};
  return std::isfinite(result.x) && std::isfinite(result.y)
             ? std::optional<AuthoredPoint>{result}
             : std::nullopt;
}

std::optional<SkinMusicSelectBarHit>
SkinInteractionLayout::musicSelectBarAt(UiLogicalPoint point) const noexcept {
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) {
    return std::nullopt;
  }
  for (const auto &bar : musicSelectBars) {
    const auto &rect = bar.authoredRegion;
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
        !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
        rect.width < 0.0 || rect.height < 0.0) {
      continue;
    }
    if (rect.x <= authored->x && rect.x + rect.width >= authored->x &&
        rect.y <= authored->y && rect.y + rect.height >= authored->y) {
      return SkinMusicSelectBarHit{.authoredOrdinal = bar.authoredOrdinal,
                                   .row = bar.row,
                                   .barIndex = bar.barIndex};
    }
  }
  return std::nullopt;
}

PresentationUiHit
SkinInteractionLayout::hitTestUiControl(UiLogicalPoint point) const noexcept {
  const auto contains = [](double x, double y, const auto &rect) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(rect.x) &&
           std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width >= 0.0 &&
           rect.height >= 0.0 && x >= rect.x && x <= rect.x + rect.width &&
           y >= rect.y && y <= rect.y + rect.height;
  };
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) {
    return {};
  }
  for (const auto &control : controlsTopmostFirst) {
    const auto hit = std::visit(
        [&](const auto &candidate) -> std::optional<PresentationUiHit> {
          using T = std::decay_t<decltype(candidate)>;
          if constexpr (std::is_same_v<T, SkinSliderInteractionGeometry>) {
            if (!candidate.writer || candidate.range <= 0.0 ||
                !std::isfinite(candidate.range) || candidate.direction < 0 ||
                candidate.direction > 3 ||
                !contains(authored->x, authored->y,
                          candidate.authoredHitRegion)) {
              return std::nullopt;
            }
            return PresentationUiHit{
                .kind = candidate.kind,
                .layoutRevision = revision,
                .sourceObject = candidate.sourceObject,
                .authoredOrdinal = candidate.authoredOrdinal,
                .writer = candidate.writer};
          } else if constexpr (std::is_same_v<
                                   T, SkinImageInteractionGeometry>) {
            if (!candidate.event || candidate.clickMode < 0 ||
                candidate.clickMode > 3 ||
                !contains(authored->x, authored->y, candidate.authoredRegion)) {
              return std::nullopt;
            }
            return PresentationUiHit{
                .kind = PresentationUiControlKind::Image,
                .layoutRevision = revision,
                .sourceObject = candidate.sourceObject,
                .authoredOrdinal = candidate.authoredOrdinal,
                .eventBinding = candidate.event.value};
          } else {
            if (!candidate.writer ||
                !contains(authored->x, authored->y,
                          candidate.authoredRegion)) {
              return std::nullopt;
            }
            return PresentationUiHit{
                .kind = PresentationUiControlKind::Text,
                .layoutRevision = revision,
                .sourceObject = candidate.sourceObject,
                .authoredOrdinal = candidate.authoredOrdinal};
          }
        },
        control);
    if (hit) {
      return *hit;
    }
  }
  return {};
}

const SkinTextInteractionGeometry *
SkinInteractionLayout::editableTextAtUi(UiLogicalPoint point) const noexcept {
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) {
    return nullptr;
  }
  const auto contains = [&](const AuthoredRect &rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) &&
           std::isfinite(rect.width) && std::isfinite(rect.height) &&
           rect.width >= 0.0 && rect.height >= 0.0 && authored->x >= rect.x &&
           authored->x <= rect.x + rect.width && authored->y >= rect.y &&
           authored->y <= rect.y + rect.height;
  };
  for (const auto &control : controlsTopmostFirst) {
    const auto match = std::visit(
        [&](const auto &candidate)
            -> std::pair<bool, const SkinTextInteractionGeometry *> {
          using T = std::decay_t<decltype(candidate)>;
          if constexpr (std::is_same_v<T, SkinSliderInteractionGeometry>) {
            return {candidate.writer && candidate.range > 0.0 &&
                        std::isfinite(candidate.range) &&
                        candidate.direction >= 0 && candidate.direction <= 3 &&
                        contains(candidate.authoredHitRegion),
                    nullptr};
          } else if constexpr (std::is_same_v<T,
                                              SkinImageInteractionGeometry>) {
            return {candidate.event && candidate.clickMode >= 0 &&
                        candidate.clickMode <= 3 &&
                        contains(candidate.authoredRegion),
                    nullptr};
          } else {
            const bool hit = candidate.writer &&
                             contains(candidate.authoredRegion);
            return {hit, hit ? &candidate : nullptr};
          }
        },
        control);
    if (match.first) {
      return match.second;
    }
  }
  return nullptr;
}

std::vector<PresentationUiHitRegion>
SkinInteractionLayout::uiHitRegions() const {
  std::vector<PresentationUiHitRegion> result;
  result.reserve(controlsTopmostFirst.size());
  const double determinant =
      uiToAuthored.m00 * uiToAuthored.m11 -
      uiToAuthored.m01 * uiToAuthored.m10;
  if (!std::isfinite(determinant) || determinant == 0.0) {
    return result;
  }
  const auto toUi = [&](double authoredX,
                        double authoredY) -> std::optional<UiLogicalPoint> {
    const double translatedX = authoredX - uiToAuthored.tx;
    const double translatedY = authoredY - uiToAuthored.ty;
    const double uiX = (uiToAuthored.m11 * translatedX -
                        uiToAuthored.m01 * translatedY) /
                       determinant;
    const double uiY = (-uiToAuthored.m10 * translatedX +
                        uiToAuthored.m00 * translatedY) /
                       determinant;
    if (!std::isfinite(uiX) || !std::isfinite(uiY)) {
      return std::nullopt;
    }
    return UiLogicalPoint{.x = static_cast<float>(uiX),
                          .y = static_cast<float>(uiY)};
  };
  for (const auto &control : controlsTopmostFirst) {
    const auto source = std::visit(
        [&](const auto &candidate)
            -> std::optional<std::pair<PresentationUiHit, AuthoredRect>> {
          using T = std::decay_t<decltype(candidate)>;
          if constexpr (std::is_same_v<T, SkinSliderInteractionGeometry>) {
            if (!candidate.writer || candidate.range <= 0.0 ||
                !std::isfinite(candidate.range) || candidate.direction < 0 ||
                candidate.direction > 3) {
              return std::nullopt;
            }
            return std::pair{PresentationUiHit{
                                 .kind = candidate.kind,
                                 .layoutRevision = revision,
                                 .sourceObject = candidate.sourceObject,
                                 .authoredOrdinal = candidate.authoredOrdinal,
                                 .writer = candidate.writer},
                             candidate.authoredHitRegion};
          } else if constexpr (std::is_same_v<
                                   T, SkinImageInteractionGeometry>) {
            if (!candidate.event || candidate.clickMode < 0 ||
                candidate.clickMode > 3) {
              return std::nullopt;
            }
            return std::pair{PresentationUiHit{
                                 .kind = PresentationUiControlKind::Image,
                                 .layoutRevision = revision,
                                 .sourceObject = candidate.sourceObject,
                                 .authoredOrdinal = candidate.authoredOrdinal,
                                 .eventBinding = candidate.event.value},
                             candidate.authoredRegion};
          } else {
            if (!candidate.writer) {
              return std::nullopt;
            }
            return std::pair{PresentationUiHit{
                                 .kind = PresentationUiControlKind::Text,
                                 .layoutRevision = revision,
                                 .sourceObject = candidate.sourceObject,
                                 .authoredOrdinal = candidate.authoredOrdinal},
                             candidate.authoredRegion};
          }
        },
        control);
    if (!source) {
      continue;
    }
    const auto &rect = source->second;
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
        !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
        rect.width < 0.0 || rect.height < 0.0) {
      continue;
    }
    const double left = rect.x;
    const double right = left + rect.width;
    const double top = rect.y;
    const double bottom = top + rect.height;
    const auto first = toUi(left, top);
    const auto second = toUi(right, top);
    const auto third = toUi(right, bottom);
    const auto fourth = toUi(left, bottom);
    if (!first || !second || !third || !fourth) {
      continue;
    }
    result.push_back(
        {.hit = source->first, .boundary = {*first, *second, *third, *fourth}});
  }
  return result;
}

std::optional<SkinWriterInvocation>
SkinInteractionLayout::writerInvocationFor(const PresentationUiHit &hit,
                                           UiLogicalPoint point,
                                           long long eventMicros) const
    noexcept {
  if ((hit.kind != PresentationUiControlKind::Slider &&
       hit.kind != PresentationUiControlKind::LaneCover) ||
      hit.layoutRevision != revision || !hit.writer) {
    return std::nullopt;
  }
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) {
    return std::nullopt;
  }
  const auto contains = [](const AuthoredPoint &candidate,
                           const AuthoredRect &rect) {
    return std::isfinite(candidate.x) && std::isfinite(candidate.y) &&
           std::isfinite(rect.x) && std::isfinite(rect.y) &&
           std::isfinite(rect.width) && std::isfinite(rect.height) &&
           rect.width >= 0.0 && rect.height >= 0.0 &&
           candidate.x >= rect.x && candidate.x <= rect.x + rect.width &&
           candidate.y >= rect.y && candidate.y <= rect.y + rect.height;
  };
  for (const auto &control : controlsTopmostFirst) {
    const auto *slider = std::get_if<SkinSliderInteractionGeometry>(&control);
    if (slider == nullptr) {
      continue;
    }
    if (slider->sourceObject != hit.sourceObject ||
        slider->kind != hit.kind ||
        slider->authoredOrdinal != hit.authoredOrdinal ||
        slider->writer != hit.writer || !slider->writer ||
        slider->range <= 0.0 || !std::isfinite(slider->range) ||
        !contains(*authored, slider->authoredHitRegion)) {
      continue;
    }
    double displacement = 0.0;
    switch (slider->direction) {
    case 0:
      displacement = authored->y - slider->valueZero.y;
      break;
    case 1:
      displacement = authored->x - slider->valueZero.x;
      break;
    case 2:
      displacement = slider->valueZero.y - authored->y;
      break;
    case 3:
      displacement = slider->valueZero.x - authored->x;
      break;
    default:
      return std::nullopt;
    }
    double normalizedValue = 0.0;
    if (std::abs(displacement) < 1.0) {
      normalizedValue = 0.0;
    } else if (std::abs(displacement - slider->range) < 1.0) {
      normalizedValue = 1.0;
    } else {
      normalizedValue = std::clamp(displacement / slider->range, 0.0, 1.0);
    }
    return SkinWriterInvocation{
        .writer = *slider->writer,
        .normalizedValue = static_cast<float>(normalizedValue),
        .eventMicros = eventMicros};
  }
  return std::nullopt;
}

std::optional<SkinWriterInvocation>
SkinInteractionLayout::sliderWriterInvocationAt(
    UiLogicalPoint point, long long eventMicros) const noexcept {
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) return std::nullopt;
  for (const auto &control : controlsTopmostFirst) {
    const auto *slider = std::get_if<SkinSliderInteractionGeometry>(&control);
    if (slider == nullptr || !slider->writer || slider->range <= 0.0 ||
        !std::isfinite(slider->range) || slider->direction < 0 ||
        slider->direction > 3) {
      continue;
    }
    const auto &region = slider->authoredHitRegion;
    if (!std::isfinite(region.x) || !std::isfinite(region.y) ||
        !std::isfinite(region.width) || !std::isfinite(region.height) ||
        region.width < 0.0 || region.height < 0.0 ||
        authored->x < region.x || authored->x > region.x + region.width ||
        authored->y < region.y || authored->y > region.y + region.height) {
      continue;
    }
    return writerInvocationFor(
        {.kind = slider->kind,
         .layoutRevision = revision,
         .sourceObject = slider->sourceObject,
         .authoredOrdinal = slider->authoredOrdinal,
         .writer = slider->writer},
        point, eventMicros);
  }
  return std::nullopt;
}

std::optional<SkinEventInvocation>
SkinInteractionLayout::eventInvocationFor(const PresentationUiHit &hit,
                                          UiLogicalPoint point,
                                          long long eventMicros) const noexcept {
  if (hit.kind != PresentationUiControlKind::Image ||
      hit.layoutRevision != revision || !hit.eventBinding) {
    return std::nullopt;
  }
  const auto authored = authoredPointForUi(point.x, point.y);
  if (!authored) {
    return std::nullopt;
  }
  const auto contains = [](const AuthoredPoint &candidate,
                           const AuthoredRect &rect) {
    return std::isfinite(candidate.x) && std::isfinite(candidate.y) &&
           std::isfinite(rect.x) && std::isfinite(rect.y) &&
           std::isfinite(rect.width) && std::isfinite(rect.height) &&
           rect.width >= 0.0 && rect.height >= 0.0 &&
           candidate.x >= rect.x && candidate.x <= rect.x + rect.width &&
           candidate.y >= rect.y && candidate.y <= rect.y + rect.height;
  };
  for (const auto &control : controlsTopmostFirst) {
    const auto *image = std::get_if<SkinImageInteractionGeometry>(&control);
    if (image == nullptr || image->sourceObject != hit.sourceObject ||
        image->authoredOrdinal != hit.authoredOrdinal ||
        image->event.value != *hit.eventBinding ||
        !contains(*authored, image->authoredRegion)) {
      continue;
    }
    int argument = 0;
    switch (image->clickMode) {
    case 0:
      argument = 1;
      break;
    case 1:
      argument = -1;
      break;
    case 2:
      argument = authored->x >= image->authoredRegion.x +
                                  image->authoredRegion.width / 2.0
                     ? 1
                     : -1;
      break;
    case 3:
      argument = authored->y >= image->authoredRegion.y +
                                  image->authoredRegion.height / 2.0
                     ? 1
                     : -1;
      break;
    default:
      return std::nullopt;
    }
    return SkinEventInvocation{.eventBinding = image->event.value,
                               .argument = argument,
                               .eventMicros = eventMicros};
  }
  return std::nullopt;
}

#if defined(ASOBMASHOW_SKIN_RENDERER_TESTING)
void resetSkinRendererLookupComparisonsForTesting() noexcept {
  lookupComparisonsForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinRendererLookupComparisonsForTesting() noexcept {
  return lookupComparisonsForTesting.load(std::memory_order_relaxed);
}
#endif

SkinFrameEvaluationResult
Skin2DRenderer::evaluateFrame(const SkinFrameInputs &inputs) {
  return evaluateFrameImpl(inputs, true);
}

SkinFrameEvaluationResult Skin2DRenderer::evaluateFrame(
    const SkinFrameInputs &inputs, SkinExternalFrameOwnership &&ownership) {
  SkinFrameEvaluationResult result;
  if (ownership.consumed_ || ownership.frameSerial_ == 0 ||
      ownership.sessionSerial_ == 0 ||
      ownership.frameSerial_ != inputs.frameSerial ||
      ownership.sessionSerial_ != inputs.sessionSerial) {
    result.diagnostics.push_back(diagnostic(
        "skin.renderer.frame.ownership",
        "Externally begun frame ownership does not match this evaluation."));
    ownership.consumed_ = true;
    return result;
  }
  ownership.consumed_ = true;
  if (externalOwnershipSessionSerial_ != inputs.sessionSerial) {
    externalOwnershipSessionSerial_ = inputs.sessionSerial;
    lastExternalOwnershipFrameSerial_ = 0;
  }
  if (inputs.frameSerial <= lastExternalOwnershipFrameSerial_) {
    result.diagnostics.push_back(diagnostic(
        "skin.renderer.frame.ownership",
        "Externally begun frame ownership was already consumed."));
    return result;
  }
  lastExternalOwnershipFrameSerial_ = inputs.frameSerial;
  return evaluateFrameImpl(inputs, false);
}

SkinFrameEvaluationResult Skin2DRenderer::evaluateFrameImpl(
    const SkinFrameInputs &inputs, bool beginRuntimeFrame) {
  SkinFrameEvaluationResult result;
  try {
    if (inputs.state.frameSerial() != inputs.frameSerial) {
      result.diagnostics.push_back(diagnostic(
          "skin.renderer.frame.serial",
          "Frame state serial does not match the evaluation serial."));
      return result;
    }
    if (inputs.sessionSerial == 0) {
      result.diagnostics.push_back(
          diagnostic("skin.renderer.session.serial",
                     "Gameplay skin session serial must be nonzero."));
      return result;
    }
    if (beginRuntimeFrame && inputs.runtime != nullptr) {
      const auto begun = inputs.runtime->beginFrame(inputs.frameSerial);
      if (!begun.ok) {
        result.diagnostics.push_back(begun.failure.value_or(
            diagnostic("skin.renderer.frame.begin",
                       "Lua runtime rejected the render-frame serial.")));
        return result;
      }
    }
    if (!invertibleViewport(inputs.viewport)) {
      result.diagnostics.push_back(
          diagnostic("skin.renderer.viewport.invalid",
                     "Gameplay skin viewport is not projectable."));
      return result;
    }
    if (gaugeAnimationSessionSerial_ != inputs.sessionSerial) {
      gaugeAnimationStates_.clear();
      gaugeAnimationSessionSerial_ = inputs.sessionSerial;
    }
    if (hitErrorVisualizerSessionSerial_ != inputs.sessionSerial ||
        hitErrorVisualizerModelIdentity_ != &inputs.model) {
      hitErrorVisualizerStates_.clear();
      hitErrorVisualizerSessionSerial_ = inputs.sessionSerial;
      hitErrorVisualizerModelIdentity_ = &inputs.model;
    }
    if (generatedTextureCacheSessionSerial_ != inputs.sessionSerial) {
      generatedTextureCache_.clear();
      generatedTextureCacheSessionSerial_ = inputs.sessionSerial;
    }

    std::vector<ProjectionElement> mergedProjectionElements;
    validateAndMergeProjection(inputs.state, mergedProjectionElements);

    const auto lookupIndex = buildFrameLookupIndex(inputs.model);
    if (!lookupIndex.uniqueBindingIds) {
      result.diagnostics.push_back(diagnostic(
          "skin.renderer.model.binding_id",
          "Gameplay skin binding IDs must be unique within each registry."));
      return result;
    }

    std::vector<const SkinObjectDefinition *> objects;
    objects.reserve(inputs.model.model.objects.size());
    for (const auto &object : inputs.model.model.objects) {
      objects.push_back(&object);
    }
    std::ranges::sort(objects, {}, &SkinObjectDefinition::id);
    const bool modelHasPracticeObject = std::ranges::any_of(
        objects, [](const SkinObjectDefinition *object) {
          return std::holds_alternative<SkinPracticeObject>(object->payload);
        });
    if (std::adjacent_find(objects.begin(), objects.end(),
                           [](const auto *left, const auto *right) {
                             return left->id == right->id;
                           }) != objects.end()) {
      result.diagnostics.push_back(
          diagnostic("skin.renderer.model.object_id",
                     "Gameplay skin object IDs must be unique."));
      return result;
    }

    SkinCommandBuffer buffer;
    buffer.frameSerial = inputs.frameSerial;
    SkinInteractionLayout interactionLayout{
        .frameSerial = inputs.frameSerial,
        .revision = inputs.sessionSerial,
        .uiToAuthored = inputs.viewport.uiToAuthored,
        .safeUiBounds = inputs.viewport.safeUiBounds};
    // PlaySkin owns one static lane-region array. Each later authored Note
    // replaces that array during loading, independent of destination visibility.
    const SkinObjectDefinition *noteLayoutSource = nullptr;
    for (const auto &object : inputs.model.model.objects) {
      if (!disabledOptionalObject(lookupIndex, object.id) &&
          std::holds_alternative<SkinNoteObject>(object.payload)) {
        noteLayoutSource = &object;
      }
    }
    if (noteLayoutSource != nullptr) {
      const auto &note = std::get<SkinNoteObject>(noteLayoutSource->payload);
      for (const auto &lane : note.lanes) {
        if (lane.authoredLane >= 0) {
          interactionLayout.laneRegions.push_back(
              {.sourceObject = noteLayoutSource->id,
               .authoredLane = lane.authoredLane,
               .authoredRegion = lane.laneDestination});
        }
      }
      if (note.lines.size() % 4 == 0) {
        const std::size_t groupCount = note.lines.size() / 4;
        for (std::size_t group = 0; group < groupCount; ++group) {
          const auto &line = note.lines[group];
          if (line.kind == SkinNoteLineKind::Group) {
            interactionLayout.laneGroupRegions.push_back(
                {.sourceObject = noteLayoutSource->id,
                 .authoredGroup = group,
                 .authoredRegion = line.laneGroupDestination});
          }
        }
      }
    }
    std::size_t glyphInstances = 0;
    std::size_t primitiveVertices = 0;
    buffer.commands.reserve(std::min(inputs.model.model.destinations.size(),
                                     skinFrameMaximumCommands(inputs)));
    struct DeferredNoteLowering {
      std::size_t insertionIndex = 0;
      const SkinObjectDefinition *object = nullptr;
      const SkinDestination *destination = nullptr;
      const SkinNoteObject *note = nullptr;
      std::optional<AuthoredDestinationGeometry> geometry;
      std::vector<SkinRuntimeOffset> offsets;
      PreparedNoteVisuals preparedVisuals;
    };
    std::vector<DeferredNoteLowering> deferredNotes;
    deferredNotes.reserve(inputs.model.model.destinations.size());

    for (const auto &destination : inputs.model.model.destinations) {
      const auto *object = findObject(objects, destination.object);
      if (!object) {
        result.diagnostics.push_back(
            diagnostic("skin.renderer.model.destination_object",
                       "Destination references an absent skin object."));
        return result;
      }
      if (disabledOptionalObject(lookupIndex, object->id)) {
        continue;
      }
      if (const auto *songList =
              std::get_if<SkinSongListObject>(&object->payload)) {
        auto lowered = lowerMusicSelectSongList(
            inputs, lookupIndex, objects, destination, *songList,
            skinFrameMaximumCommands(inputs) - buffer.commands.size(),
            skinFrameMaximumGlyphInstances(inputs) - glyphInstances);
        if (!lowered.failures.empty()) {
          bool critical = false;
          for (auto &failure : lowered.failures) {
            critical = reportObjectFailure(result, *object,
                                           std::move(failure)) ||
                       critical;
          }
          if (critical) {
            return result;
          }
          continue;
        }
        glyphInstances += lowered.glyphCount;
        buffer.commands.insert(
            buffer.commands.end(),
            std::make_move_iterator(lowered.commands.begin()),
            std::make_move_iterator(lowered.commands.end()));
        interactionLayout.musicSelectBars.insert(
            interactionLayout.musicSelectBars.end(),
            std::make_move_iterator(lowered.interactions.begin()),
            std::make_move_iterator(lowered.interactions.end()));
        continue;
      }
      if (const auto *practiceObject =
              std::get_if<SkinPracticeObject>(&object->payload);
          practiceObject != nullptr && practiceObject->visibleItems > 0) {
        if (!inputs.state.stagePracticeVisibleItemCount(
                practiceObject->visibleItems)) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.practice.mutation",
                             "Practice visible-row count could not be staged."))) {
            return result;
          }
        }
        continue;
      }
      // JsonSkinLoader removes ordinary empty-dst SkinObjects during
      // validation. JsonPlaySkinObjectLoader has two exceptions: SkinNote
      // owns independent per-lane note.dst geometry, while SkinJudge keeps
      // its constructor destination and renders its nested children directly.
      // Real Lua skins use an intentionally empty containing destination for
      // both forms.
      const bool isNoteObject =
          std::holds_alternative<SkinNoteObject>(object->payload);
      const bool isJudgeObject =
          std::holds_alternative<SkinJudgeObject>(object->payload);
      const bool noteUsesLaneGeometryWithoutFrame =
          isNoteObject && destination.presentation.frames.empty();
      const bool judgeUsesConstructorDestinationWithoutFrame =
          isJudgeObject && destination.presentation.frames.empty();
      if (destination.presentation.frames.empty() && !isNoteObject &&
          !isJudgeObject) {
        continue;
      }
      // Numeric option conditions are resolved by Skin.prepare's static
      // dstop filtering before an object enters Beatoraja's frame object
      // array. A rejected Gauge/Judge therefore performs no special state or
      // child preparation; only runtime Boolean dstdraw rejection continues
      // through those overrides.
      bool rejectedByConfiguredOption = false;
      if (!judgeUsesConstructorDestinationWithoutFrame) {
        for (const auto &condition : destination.presentation.conditions) {
          const auto *configured = std::get_if<int>(&condition);
          if (!configured) {
            continue;
          }
          bool enabled = false;
          if (!configuredCondition(inputs.configuration, *configured, enabled) ||
              !enabled) {
            rejectedByConfiguredOption = true;
            break;
          }
        }
      }
      if (rejectedByConfiguredOption) {
        continue;
      }
      const auto *image = std::get_if<SkinImageObject>(&object->payload);
      const auto *number = std::get_if<SkinNumberObject>(&object->payload);
      const auto *floating = std::get_if<SkinFloatObject>(&object->payload);
      const auto *text = std::get_if<SkinTextObject>(&object->payload);
      const auto *slider = std::get_if<SkinSliderObject>(&object->payload);
      const auto *graph = std::get_if<SkinGraphObject>(&object->payload);
      const auto *selectDistribution =
          std::get_if<SkinSelectDistributionGraphObject>(&object->payload);
      const auto *noteDistribution =
          std::get_if<SkinNoteDistributionGraphObject>(&object->payload);
      const auto *gaugeGraph =
          std::get_if<SkinGaugeGraphObject>(&object->payload);
      const auto *bpmGraph =
          std::get_if<SkinBpmGraphObject>(&object->payload);
      const auto *timingVisualizer =
          std::get_if<SkinTimingVisualizerObject>(&object->payload);
      // SkinTimingVisualizer.prepare accepts BMSPlayer only and returns
      // before SkinObject.prepare for either result state. Keep its result
      // declaration inert, including all destination callbacks.
      if (timingVisualizer && (inputs.model.model.header.type == 7 ||
                               inputs.model.model.header.type == 15)) {
        continue;
      }
      const auto *timingDistribution =
          std::get_if<SkinTimingDistributionGraphObject>(&object->payload);
      // SkinTimingDistributionGraph.prepare accepts MusicResult only and
      // returns before SkinObject.prepare for every other state. Reject it
      // before evaluating destination callbacks, like TimingVisualizer.
      if (timingDistribution && inputs.model.model.header.type != 7) {
        continue;
      }
      const auto *hitErrorVisualizer =
          std::get_if<SkinHitErrorVisualizerObject>(&object->payload);
      const auto *gauge = std::get_if<SkinGaugeObject>(&object->payload);
      const auto *note = std::get_if<SkinNoteObject>(&object->payload);
      const auto *cover = std::get_if<SkinCoverObject>(&object->payload);
      const auto *judge = std::get_if<SkinJudgeObject>(&object->payload);
      const auto *bga = std::get_if<SkinBgaObject>(&object->payload);
      const auto *practice =
          std::get_if<SkinPracticeObject>(&object->payload);
      const auto *builtinImage =
          std::get_if<SkinBuiltinImageObject>(&object->payload);
      const auto *pmChara =
          std::get_if<SkinPmCharaObject>(&object->payload);
      const auto *invalidInGameplay =
          std::get_if<SkinInvalidInGameplayObject>(&object->payload);
      const auto *blank = std::get_if<SkinBlankObject>(&object->payload);
      if (!image && !number && !floating && !text && !slider && !graph &&
          !selectDistribution &&
          !noteDistribution && !gaugeGraph && !bpmGraph &&
          !timingVisualizer && !timingDistribution &&
          !hitErrorVisualizer &&
          !gauge && !note && !cover && !judge && !bga && !builtinImage &&
          !practice && !pmChara && !invalidInGameplay && !blank) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.object.unsupported",
                           "Skin object lowering is not implemented."))) {
          return result;
        }
        continue;
      }

      // Optional v1 widgets without renderers are valid empty draws.
      if (invalidInGameplay || blank) {
        continue;
      }

      std::optional<SkinJudgeStateView> judgeState;
      if (judge) {
        judgeState = inputs.state.judgeState(judge->player);
        if (!judgeState->supported) {
          if (reportObjectFailure(result, *object,
                                  diagnostic("skin.renderer.judge.state",
                                             "Judge state is unsupported."))) {
            return result;
          }
          continue;
        }
        // SkinJudge reads the current grade before calling super.prepare, so
        // an absent grade skips the wrapper destination and all its callbacks.
        if (!judgeState->optionalZeroBasedGrade) {
          continue;
        }
        if (*judgeState->optionalZeroBasedGrade < 0 ||
            *judgeState->optionalZeroBasedGrade > 5) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.judge.state",
                             "Judge grade is outside the supported range."))) {
            return result;
          }
          continue;
        }
      }

      std::size_t stateIndex = 0;
      SpriteSelection selected;
      const PreparedSkinMovie *preparedMovie = nullptr;
      std::int64_t movieTimeMillis = 0;
      std::optional<NumericLayout> numericLayout;
      std::optional<TextLayoutInput> textLayout;
      if (image) {
        if (image->orderedStates.empty()) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.image.states",
                             "Image object has no source states."))) {
            return result;
          }
          continue;
        }
        if (image->stateIndex) {
          const auto resolved =
              resolveInteger(inputs, lookupIndex, *image->stateIndex);
          if (resolved.failure) {
            if (reportObjectFailure(result, *object, *resolved.failure)) {
              return result;
            }
            continue;
          }
          if (*resolved.value < 0) {
            continue;
          }
          stateIndex = static_cast<std::size_t>(*resolved.value);
          if (stateIndex >= image->orderedStates.size()) {
            stateIndex = 0;
          }
        }
        if (inputs.movies != nullptr) {
          preparedMovie = inputs.movies->findMovie(
              image->orderedStates[stateIndex].resource);
        }
      } else if (number || floating) {
        numericLayout =
            number ? prepareNumberLayout(inputs, lookupIndex, *number)
                   : prepareFloatLayout(inputs, lookupIndex, *floating);
        if (numericLayout->failure) {
          if (reportObjectFailure(result, *object, *numericLayout->failure)) {
            return result;
          }
          continue;
        }
        if (numericLayout->suppressed) {
          continue;
        }
      }

      const std::size_t conditionCount =
          judgeUsesConstructorDestinationWithoutFrame
              ? 0U
              : destination.presentation.conditions.size() +
                    (destination.presentation.drawCondition ? 1U : 0U);
      std::unique_ptr<bool[]> conditions;
      if (conditionCount != 0) {
        conditions = std::make_unique<bool[]>(conditionCount);
      }
      std::size_t conditionIndex = 0;
      bool conditionFailure = false;
      bool destinationVisible = true;
      if (!judgeUsesConstructorDestinationWithoutFrame) {
        for (const auto &condition : destination.presentation.conditions) {
          if (const auto *configured = std::get_if<int>(&condition)) {
            bool value = false;
            if (!configuredCondition(inputs.configuration, *configured, value)) {
              conditionFailure = true;
              break;
            }
            conditions[conditionIndex++] = value;
            if (!value) {
              destinationVisible = false;
              break;
            }
          } else {
            const auto resolved = resolveBoolean(
                inputs, lookupIndex, std::get<SkinBooleanPropertyId>(condition));
            if (resolved.failure) {
              if (reportObjectFailure(result, *object, *resolved.failure)) {
                return result;
              }
              conditionFailure = true;
              break;
            }
            conditions[conditionIndex++] = *resolved.value;
            if (!*resolved.value) {
              destinationVisible = false;
              break;
            }
          }
        }
      }
      if (conditionFailure) {
        continue;
      }
      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible &&
          destination.presentation.drawCondition) {
        const auto resolved = resolveBoolean(
            inputs, lookupIndex, *destination.presentation.drawCondition);
        if (resolved.failure) {
          if (reportObjectFailure(result, *object, *resolved.failure)) {
            return result;
          }
          continue;
        }
        conditions[conditionIndex++] = *resolved.value;
        destinationVisible = *resolved.value;
      }

      if (!destinationVisible && !image && !gauge && !note && !cover &&
          !judge && !pmChara) {
        continue;
      }

      std::int64_t timerStartMicros = INT64_MIN;
      bool timerOff = false;
      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible &&
          destination.presentation.timer) {
        const auto timer = resolveTimerUse(inputs, lookupIndex,
                                           *destination.presentation.timer);
        if (timer.failure) {
          if (reportObjectFailure(result, *object, *timer.failure)) {
            return result;
          }
          continue;
        }
        timerStartMicros = timer.value;
        timerOff = timer.off;
      }
      SkinDestinationEvaluationResult evaluated;
      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible &&
          !noteUsesLaneGeometryWithoutFrame) {
        evaluated = evaluateSkinDestinationAuthored(
            destination.presentation,
            {.nowMicros = inputs.visualTimeMicros,
             .timerStartMicros = timerStartMicros,
             .timerOff = timerOff,
             .optionConditions =
                 std::span<const bool>(conditions.get(), conditionCount),
             .orderedOffsets = {}});
        if (!evaluated.diagnostics.empty()) {
          bool critical = false;
          for (auto &item : evaluated.diagnostics) {
            critical = reportObjectFailure(result, *object, std::move(item)) ||
                       critical;
          }
          if (critical) {
            return result;
          }
          continue;
        }
        destinationVisible = evaluated.geometry.has_value();
      }
      if (!destinationVisible && !image && !gauge && !note && !cover &&
          !judge && !pmChara) {
        continue;
      }

      std::vector<SkinRuntimeOffset> offsets;
      std::optional<double> coverLiftOffsetY;
      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible) {
        offsets.reserve(destination.presentation.offsetIds.size());
      }
      bool offsetFailure = false;
      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible) {
        for (const int id : destination.presentation.offsetIds) {
          // SkinObject.setOffsetID ignores the decoder's zero default and
          // every ID outside SkinProperty's pinned 1...199 range.
          if (id <= 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
            continue;
          }
          const auto found = inputs.configuration.offsetsById.find(id);
          if (!pinnedLaneCoverRuntimeOffset(id) &&
              found != inputs.configuration.offsetsById.end()) {
            offsets.push_back(skinRuntimeOffset(found->second));
            if (cover && id == kSkinCoverLiftOffsetId) {
              coverLiftOffsetY = found->second.y;
            }
            continue;
          }
          const auto dynamic = inputs.state.offsetProperty(id);
          if (!dynamic.supported) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic("skin.renderer.offset.missing",
                               "Destination offset is unsupported by both "
                               "configuration and frame state."))) {
              return result;
            }
            offsetFailure = true;
            break;
          }
          offsets.push_back(dynamic.value);
          if (cover && id == kSkinCoverLiftOffsetId) {
            coverLiftOffsetY = dynamic.value.y;
          }
        }
      }
      if (offsetFailure) {
        continue;
      }

      if (!judgeUsesConstructorDestinationWithoutFrame && destinationVisible &&
          !noteUsesLaneGeometryWithoutFrame) {
        evaluated = evaluateSkinDestinationAuthored(
            destination.presentation,
            {.nowMicros = inputs.visualTimeMicros,
             .timerStartMicros = timerStartMicros,
             .timerOff = timerOff,
             .optionConditions =
                 std::span<const bool>(conditions.get(), conditionCount),
             .orderedOffsets = offsets});
      }
      if (!evaluated.diagnostics.empty()) {
        bool critical = false;
        for (auto &item : evaluated.diagnostics) {
          critical =
              reportObjectFailure(result, *object, std::move(item)) || critical;
        }
        if (critical) {
          return result;
        }
        continue;
      }
      if (!evaluated.geometry && !image && !gauge && !note && !cover &&
          !judge && !pmChara) {
        continue;
      }

      if (builtinImage) {
        if (!evaluated.geometry || evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        const auto source =
            inputs.resources.builtinImageResource(builtinImage->referenceId);
        if (!source) {
          // SkinSourceReference#getImage returns null when the requested
          // system image is unavailable, and SkinImage suppresses that draw.
          continue;
        }
        const auto *resource = inputs.resources.find(*source);
        if (resource == nullptr || resource->regionMappings.empty()) {
          continue;
        }
        auto lowered = lowerPreparedQuad(
            inputs, object->id, destination.presentation.authoredOrdinal,
            *evaluated.geometry, *source, resource->width, resource->height,
            resource->regionMappings.front().resolved);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.command) {
          if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic("skin.renderer.command.limit",
                               "Skin command buffer exceeds its fixed limit."))) {
              return result;
            }
            continue;
          }
          buffer.commands.push_back(std::move(*lowered.command));
        }
        continue;
      }

      if (practice) {
        if (!evaluated.geometry) {
          continue;
        }
        auto lowered = lowerPracticeLegacy(
            inputs, lookupIndex, *object, destination, *evaluated.geometry,
            inputs.state.practiceState(),
            skinFrameMaximumGlyphInstances(inputs) - glyphInstances,
            skinFrameMaximumCommands(inputs) - buffer.commands.size(),
            skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        glyphInstances += lowered.glyphCount;
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (pmChara) {
        const auto *prepared = inputs.resources.findPomyuChara(object->id);
        if (prepared == nullptr || !evaluated.geometry) {
          continue;
        }
        for (const auto &animation : prepared->animations) {
          bool optionsMatch = true;
          for (const int option : animation.options) {
            if (option == 0) {
              continue;
            }
            const auto resolved = inputs.state.booleanProperty(
                SkinBuiltinPropertySelector{std::abs(option)});
            if (!resolved.supported ||
                resolved.value != (option > 0)) {
              optionsMatch = false;
              break;
            }
          }
          if (!optionsMatch || animation.frames.empty()) {
            continue;
          }
          std::int64_t elapsedMillis = 0;
          if (animation.builtinTimerId) {
            const std::int64_t start = inputs.state.timerProperty(
                SkinBuiltinPropertySelector{*animation.builtinTimerId});
            if (start == INT64_MIN) {
              continue;
            }
            elapsedMillis = std::max<std::int64_t>(
                0, inputs.visualTimeMicros / 1'000 - start / 1'000);
          } else if (animation.frameMillis > 0) {
            if (!destination.presentation.timer || timerOff) {
              continue;
            }
            elapsedMillis = std::max<std::int64_t>(
                0, inputs.visualTimeMicros / 1'000 -
                       timerStartMicros / 1'000);
          }
          std::size_t frameIndex = 0;
          if (animation.frameMillis > 0 && animation.cycleMillis > 0 &&
              animation.frames.size() > 1) {
            const std::size_t loopStart = std::min(
                animation.loopStartFrame, animation.frames.size() - 1);
            const std::int64_t introMillis =
                static_cast<std::int64_t>(loopStart) *
                animation.frameMillis;
            if (loopStart != 0 && elapsedMillis < introMillis) {
              frameIndex = std::min<std::size_t>(
                  static_cast<std::size_t>(elapsedMillis /
                                           animation.frameMillis),
                  loopStart - 1);
            } else {
              const std::size_t repeatingFrames =
                  animation.frames.size() - loopStart;
              const std::int64_t repeatingMillis =
                  static_cast<std::int64_t>(repeatingFrames) *
                  animation.frameMillis;
              if (repeatingMillis <= 0) {
                continue;
              }
              frameIndex = loopStart + static_cast<std::size_t>(
                  ((elapsedMillis - introMillis) % repeatingMillis) /
                  animation.frameMillis);
            }
          }
          const auto &frame = animation.frames[frameIndex];
          if (frame.resource == 0 || frame.region.w <= 0 ||
              frame.region.h <= 0) {
            continue;
          }
          const auto *resource = inputs.resources.find(frame.resource);
          const auto *region = inputs.resources.findResolvedRegion(
              frame.resource, frame.region);
          if (!resource || !region || resource->width <= 0 ||
              resource->height <= 0) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic("skin.renderer.resource.missing",
                               "Prepared Pomyu character resource is absent."))) {
              return result;
            }
            continue;
          }
          AuthoredDestinationGeometry geometry = *evaluated.geometry;
          if (prepared->relativePlacement) {
            const AuthoredRect base = geometry.rect;
            geometry.rect = {
                .x = base.x + frame.x * base.width /
                                  prepared->coordinateWidth,
                .y = base.y + base.height -
                     (frame.y + frame.height) * base.height /
                         prepared->coordinateHeight,
                .width = frame.width * base.width /
                         prepared->coordinateWidth,
                .height = frame.height * base.height /
                          prepared->coordinateHeight};
            geometry.angleDegrees = frame.angleDegrees;
            geometry.rgba = {1.0F, 1.0F, 1.0F,
                             static_cast<float>(frame.alpha) / 255.0F};
            geometry.blend = SkinBlendMode::Subtractive;
            geometry.filter = SkinFilterMode::Linear;
            geometry.stretch = SkinStretchMode::Stretch;
          }
          const auto projected = projectSkinDestinationToUi(
              geometry,
              {.textureWidth = resource->width,
               .textureHeight = resource->height,
               .region = region->resolved},
              inputs.viewport);
          bool emptyClip = false;
          const auto clip = intersectClip(
              projected.clip, projectedSkinScissorBounds(inputs.viewport),
              emptyClip);
          if (emptyClip) {
            continue;
          }
          if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic("skin.renderer.command.limit",
                               "Pomyu commands exceed the fixed frame limit."))) {
              return result;
            }
            continue;
          }
          SkinTexturedQuadCommand command;
          command.resource = frame.resource;
          command.state = {.blend = projected.blend,
                           .filter = projected.filter,
                           .scissor = clip};
          const std::uint32_t color = packAbgr(projected.rgba);
          for (std::size_t index = 0; index < command.vertices.size();
               ++index) {
            command.vertices[index] = {
                .x = static_cast<float>(projected.vertices[index][0]),
                .y = static_cast<float>(projected.vertices[index][1]),
                .u = static_cast<float>(projected.normalizedUvs[index][0]),
                .v = static_cast<float>(projected.normalizedUvs[index][1]),
                .rgba = color};
          }
          buffer.commands.push_back(
              {.authoredOrdinal = destination.presentation.authoredOrdinal,
               .sourceObject = object->id,
               .payload = std::move(command)});
        }
        continue;
      }

      if (selectDistribution) {
        // SkinDistributionGraph reads the currently selected DirectoryBar;
        // a non-directory selection simply has no graph to draw.
        if (!evaluated.geometry || evaluated.geometry->rgba[3] <= 0.0F ||
            inputs.musicSelectSongList == nullptr) {
          continue;
        }
        const auto &songList = *inputs.musicSelectSongList;
        if (songList.selectedIndex >= songList.bars.size()) {
          continue;
        }
        const auto &selectedBar = songList.bars[songList.selectedIndex];
        if (!musicSelectIsDirectoryBarKind(selectedBar.kind)) {
          continue;
        }
        const auto counts =
            selectDistribution->type == SkinSelectDistributionGraphType::Normal
                ? std::span<const int>(selectedBar.folderLampCounts)
                : std::span<const int>(selectedBar.folderRankCounts);
        const std::size_t stateCount = counts.size();
        if (selectDistribution->sprite.resource == 0 ||
            selectDistribution->sprite.frames.empty() ||
            selectDistribution->sprite.frames.size() % stateCount != 0) {
          continue;
        }
        const auto animated = selectAnimationFrame(
            inputs, lookupIndex,
            selectDistribution->sprite.frames.size() / stateCount,
            selectDistribution->sprite.cycleMillis,
            selectDistribution->sprite.timer);
        if (animated.failure || animated.suppressed) {
          continue;
        }
        const int total = std::accumulate(counts.begin(), counts.end(), 0);
        if (total <= 0) {
          continue;
        }
        int prior = 0;
        for (std::size_t reversed = stateCount; reversed > 0; --reversed) {
          const std::size_t state = reversed - 1;
          const int count = counts[state];
          if (count <= 0) {
            prior += std::max(0, count);
            continue;
          }
          auto segment = *evaluated.geometry;
          segment.rect.x += segment.rect.width * prior / total;
          segment.rect.width = segment.rect.width * count / total;
          const auto &frame = selectDistribution->sprite.frames[
              animated.frame * stateCount + state];
          auto lowered = lowerSpriteQuad(
              inputs, object->id, destination.presentation.authoredOrdinal,
              segment, selectDistribution->sprite, frame);
          if (lowered.failure) {
            continue;
          }
          if (lowered.command) {
            if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
              break;
            }
            buffer.commands.push_back(std::move(*lowered.command));
          }
          prior += count;
        }
        continue;
      }

      if (noteDistribution) {
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        std::optional<std::int64_t> currentMillis;
        const std::int64_t playTimerStart =
            inputs.state.timerProperty(SkinBuiltinPropertySelector{41});
        if (playTimerStart != INT64_MIN) {
          currentMillis = inputs.visualTimeMicros / 1000 -
                          playTimerStart / 1000;
        }
        auto lowered = renderSkinNoteDistributionGraph(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .graph = *noteDistribution,
             .state = inputs.state.gameplayGraphState(),
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .pmsMode = inputs.model.model.header.type == 4,
             .elapsedMillis = inputs.visualTimeMicros / 1000,
             .currentMillis = currentMillis,
             .maximumCommands =
                 skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (gaugeGraph) {
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        auto lowered = renderSkinGaugeGraph(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .graph = *gaugeGraph,
             .state = inputs.state.gameplayGraphState(),
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .elapsedMillis = inputs.visualTimeMicros / 1000,
             .maximumCommands =
                 skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (bpmGraph) {
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        auto lowered = renderSkinBpmGraph(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .graph = *bpmGraph,
             .state = inputs.state.gameplayGraphState(),
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .elapsedMillis = inputs.visualTimeMicros / 1000,
             .maximumCommands =
                 skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (timingVisualizer) {
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        auto lowered = renderSkinTimingVisualizer(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .visualizer = *timingVisualizer,
             .state = inputs.state.gameplayGraphState(),
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .maximumCommands =
                 skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (timingDistribution) {
        // SkinTimingDistributionGraph.prepare accepts MusicResult only. A
        // course-result skin may declare the object, but Beatoraja keeps it
        // hidden rather than visualizing the aggregate course snapshot.
        if (inputs.model.model.header.type != 7) {
          continue;
        }
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        auto lowered = renderSkinTimingDistributionGraph(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .graph = *timingDistribution,
             .state = inputs.state.gameplayGraphState(),
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .maximumCommands = skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (hitErrorVisualizer) {
        if (evaluated.geometry->rgba[3] <= 0.0F) {
          continue;
        }
        auto &presentation = hitErrorVisualizerStates_[object->id];
        const auto graphState = inputs.state.gameplayGraphState();
        const bool indexAdvanced = advanceSkinHitErrorVisualizerEma(
            *hitErrorVisualizer, graphState, presentation);
        (void)indexAdvanced;
        auto lowered = renderSkinHitErrorVisualizer(
            {.sourceObject = object->id,
             .authoredOrdinal = destination.presentation.authoredOrdinal,
             .visualizer = *hitErrorVisualizer,
             .state = graphState,
             .emaMillis = presentation.emaMillis,
             .geometry = *evaluated.geometry,
             .viewport = inputs.viewport,
             .maximumCommands =
                 skinFrameMaximumCommands(inputs) - buffer.commands.size(),
             .maximumPrimitiveVertices =
                 skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices,
             .cache = &generatedTextureCache_});
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        primitiveVertices += lowered.primitiveVertices;
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(lowered.commands.begin()),
                               std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (gauge) {
        const auto gaugeState = inputs.state.gaugeState();
        float gaugeValue = static_cast<float>(gaugeState.value);
        const float gaugeMinimum = static_cast<float>(gaugeState.minimum);
        const float gaugeMaximum = static_cast<float>(gaugeState.maximum);
        const float gaugeBorder = static_cast<float>(gaugeState.border);
        if (inputs.model.model.header.type == 7 ||
            inputs.model.model.header.type == 15) {
          const std::int64_t elapsedMillis = inputs.visualTimeMicros / 1'000;
          if (elapsedMillis < gauge->resultStartMillis) {
            gaugeValue = gaugeMinimum;
          } else if (elapsedMillis < gauge->resultEndMillis) {
            const float reveal = gaugeMaximum * static_cast<float>(
                elapsedMillis - gauge->resultStartMillis) /
                static_cast<float>(gauge->resultEndMillis -
                                   gauge->resultStartMillis);
            gaugeValue = std::min(gaugeValue,
                                  std::max(reveal, gaugeMinimum));
          }
        }
        const bool modelValid =
            gauge->orderedNodes.size() == 36 && gauge->parts >= 1 &&
            gauge->parts <= 512 && gauge->animationRange >= 0 &&
            gauge->animationRange <= 1024 && gauge->animationCycleMillis >= 1 &&
            gauge->animationCycleMillis <= 60'000 &&
            (gauge->animation != SkinGaugeAnimationType::Flicker ||
             gauge->animationCycleMillis >= 4) &&
            gauge->resultStartMillis >= 0 &&
            gauge->resultStartMillis < gauge->resultEndMillis &&
            gauge->resultEndMillis <= 600'000;
        const bool stateValid =
            gaugeState.supported && std::isfinite(gaugeState.value) &&
            std::isfinite(gaugeState.minimum) &&
            std::isfinite(gaugeState.maximum) &&
            std::isfinite(gaugeState.border) && std::isfinite(gaugeValue) &&
            std::isfinite(gaugeMinimum) && std::isfinite(gaugeMaximum) &&
            std::isfinite(gaugeBorder) && gaugeMaximum > 0.0F &&
            gaugeMaximum > gaugeMinimum && gaugeState.gaugeType >= 0 &&
            gaugeState.gaugeType <= 8;
        if (!modelValid || !stateValid) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.gauge.invalid",
                             "Gauge model or frame state is outside its safe "
                             "rendering domain."))) {
            return result;
          }
          continue;
        }

        GaugeAnimationState animationState;
        if (const auto found = gaugeAnimationStates_.find(object->id);
            found != gaugeAnimationStates_.end()) {
          animationState = found->second;
        }
        const std::int64_t visualMillis = inputs.visualTimeMicros / 1000;
        if (gauge->animation != SkinGaugeAnimationType::Flicker &&
            animationState.deadlineMillis < visualMillis) {
          ++animationState.epoch;
          const int modulus = gauge->animationRange + 1;
          switch (gauge->animation) {
          case SkinGaugeAnimationType::Random: {
            if (!inputs.gaugeRandomSource) {
              if (reportObjectFailure(
                      result, *object,
                      diagnostic("skin.renderer.gauge.random",
                                 "Random gauge animation has no session-owned "
                                 "random source."))) {
                return result;
              }
              continue;
            }
            const auto random = inputs.gaugeRandomSource->next(
                object->id, animationState.epoch,
                static_cast<std::uint32_t>(modulus));
            if (!random || *random >= static_cast<std::uint32_t>(modulus)) {
              if (reportObjectFailure(
                      result, *object,
                      diagnostic("skin.renderer.gauge.random",
                                 "Random gauge source returned an invalid "
                                 "animation value."))) {
                return result;
              }
              continue;
            }
            animationState.animation = static_cast<int>(*random);
            break;
          }
          case SkinGaugeAnimationType::Increase:
            animationState.animation =
                (animationState.animation + gauge->animationRange) % modulus;
            break;
          case SkinGaugeAnimationType::Decrease:
            animationState.animation = (animationState.animation + 1) % modulus;
            break;
          case SkinGaugeAnimationType::Flicker:
            break;
          }
          animationState.deadlineMillis =
              visualMillis > std::numeric_limits<std::int64_t>::max() -
                                 gauge->animationCycleMillis
                  ? std::numeric_limits<std::int64_t>::max()
                  : visualMillis + gauge->animationCycleMillis;
        }

        if (!evaluated.geometry) {
          gaugeAnimationStates_.insert_or_assign(object->id, animationState);
          continue;
        }

        const int family = gaugeState.gaugeType >= 6 ? gaugeState.gaugeType - 3
                                                     : gaugeState.gaugeType;
        const int notes =
            gaugeValue > 0.0F
                ? std::max(1,
                           truncatingJavaInt(gaugeValue *
                                             static_cast<float>(gauge->parts) /
                                             gaugeMaximum))
                : 0;
        std::vector<SkinDrawCommand> gaugeCommands;
        gaugeCommands.reserve(
            static_cast<std::size_t>(gauge->parts) *
            (gauge->animation == SkinGaugeAnimationType::Flicker ? 2U : 1U));
        std::optional<SkinDiagnostic> gaugeFailure;
        auto emitRole = [&](int part, int role, float alphaMultiplier) {
          if (gaugeFailure) {
            return;
          }
          const std::size_t nodeIndex =
              static_cast<std::size_t>(family * 6 + role);
          if (nodeIndex >= gauge->orderedNodes.size()) {
            gaugeFailure = diagnostic("skin.renderer.gauge.node",
                                      "Gauge selected an absent node role.");
            return;
          }
          const auto &node = gauge->orderedNodes[nodeIndex];
          const auto selectedNode =
              selectSpriteFrame(inputs, lookupIndex, node);
          if (selectedNode.failure) {
            gaugeFailure = *selectedNode.failure;
            return;
          }
          if (selectedNode.suppressed || !selectedNode.frame) {
            return;
          }
          auto geometry = *evaluated.geometry;
          geometry.rect.x += geometry.rect.width *
                             static_cast<double>(part - 1) / gauge->parts;
          geometry.rect.width /= gauge->parts;
          geometry.rgba[3] *= alphaMultiplier;
          // SkinGauge draws its nodes straight through SkinObjectRenderer and
          // forces TYPE_NORMAL. It does not use SkinObject's textured-region
          // stretch, rotation, or destination filtering path.
          geometry.stretch = SkinStretchMode::Stretch;
          geometry.angleDegrees = 0.0;
          geometry.filter = SkinFilterMode::Nearest;
          auto lowered = lowerSpriteQuad(
              inputs, object->id, destination.presentation.authoredOrdinal,
              geometry, node, *selectedNode.frame);
          if (lowered.failure) {
            gaugeFailure = *lowered.failure;
          } else if (lowered.command) {
            gaugeCommands.push_back(std::move(*lowered.command));
          }
        };
        for (int part = 1; part <= gauge->parts && !gaugeFailure; ++part) {
          const float partBorder = static_cast<float>(part) * gaugeMaximum /
                                   static_cast<float>(gauge->parts);
          const bool lowSide = partBorder < gaugeBorder;
          const int side = lowSide ? 1 : 0;
          if (gauge->animation == SkinGaugeAnimationType::Flicker) {
            emitRole(part, (notes >= part ? 0 : 2) + side, 1.0F);
            if (part == notes) {
              const std::int64_t cycle = gauge->animationCycleMillis;
              const std::int64_t phase =
                  ((visualMillis % cycle) + cycle) % cycle;
              const double half = static_cast<double>(cycle) / 2.0;
              const double alpha =
                  phase < half
                      ? static_cast<double>(phase) / (half - 1.0)
                      : static_cast<double>(cycle - 1 - phase) / (half - 1.0);
              emitRole(part, 4 + side, static_cast<float>(alpha));
            }
          } else {
            const int role =
                notes == part
                    ? 4
                    : (notes - animationState.animation > part ? 0 : 2);
            emitRole(part, role + side, 1.0F);
          }
        }
        if (gaugeFailure) {
          if (reportObjectFailure(result, *object, *gaugeFailure)) {
            return result;
          }
          continue;
        }
        if (gaugeCommands.size() >
            skinFrameMaximumCommands(inputs) - buffer.commands.size()) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "Gauge commands exceed the fixed frame limit."))) {
            return result;
          }
          continue;
        }
        gaugeAnimationStates_.insert_or_assign(object->id, animationState);
        buffer.commands.insert(buffer.commands.end(),
                               std::make_move_iterator(gaugeCommands.begin()),
                               std::make_move_iterator(gaugeCommands.end()));
        continue;
      }

      if (slider || graph) {
        const SkinSpriteFrames &sprite = slider ? slider->knob : graph->fill;
        SkinResourceId sourceResource = sprite.resource;
        const PreparedSkinResource *resource = nullptr;
        const SkinResolvedRegion *region = nullptr;
        if (graph && graph->builtinImageReference) {
          const auto resolved = inputs.resources.builtinImageResource(
              *graph->builtinImageReference);
          if (!resolved) {
            // SkinSourceReference returns null for an unavailable system
            // image, suppressing the graph before its value callback.
            continue;
          }
          sourceResource = *resolved;
          resource = inputs.resources.find(sourceResource);
          if (resource != nullptr && !resource->regionMappings.empty()) {
            region = &resource->regionMappings.front();
          }
        } else {
          const auto selectedSprite =
              selectSpriteFrame(inputs, lookupIndex, sprite);
          if (selectedSprite.failure) {
            if (reportObjectFailure(result, *object,
                                    *selectedSprite.failure)) {
              return result;
            }
            continue;
          }
          if (selectedSprite.suppressed || !selectedSprite.frame) {
            continue;
          }
          resource = inputs.resources.find(sourceResource);
          region = inputs.resources.findResolvedRegion(
              sourceResource, *selectedSprite.frame);
        }
        // Pinned SkinSlider/SkinGraph obtain the source image before reading
        // their RateProperty. Preserve that callback order and do not invoke a
        // value callback when the prepared source is absent.
        if (!resource || !region || resource->width <= 0 ||
            resource->height <= 0 || region->resolved.w <= 0 ||
            region->resolved.h <= 0) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.resource.missing",
                             "Prepared slider/graph resource is absent."))) {
            return result;
          }
          continue;
        }
        const auto rate = resolveRate(inputs, lookupIndex,
                                      slider ? slider->value : graph->value);
        if (rate.failure) {
          if (reportObjectFailure(result, *object, *rate.failure)) {
            return result;
          }
          continue;
        }
        // FloatProperty.get and LuaValue.tofloat both cross a Java float
        // boundary before SkinSlider/SkinGraph perform their arithmetic.
        const float objectRate = static_cast<float>(*rate.value);

        QuadLoweringResult lowered;
        if (slider) {
          if (!std::isfinite(slider->range)) {
            lowered.failure = diagnostic(
                "skin.renderer.slider.invalid",
                "Slider range is outside its safe domain.");
          } else {
            auto geometry = *evaluated.geometry;
            const float displacement =
                objectRate * static_cast<float>(slider->range);
            switch (slider->direction) {
            case 0:
              geometry.rect.y =
                  static_cast<float>(geometry.rect.y) + displacement;
              break;
            case 1:
              geometry.rect.x =
                  static_cast<float>(geometry.rect.x) + displacement;
              break;
            case 2:
              geometry.rect.y =
                  static_cast<float>(geometry.rect.y) - displacement;
              break;
            case 3:
              geometry.rect.x =
                  static_cast<float>(geometry.rect.x) - displacement;
              break;
            }
            lowered = lowerPreparedQuad(
                inputs, object->id, destination.presentation.authoredOrdinal,
                geometry, sourceResource, resource->width, resource->height,
                region->resolved);
          }
        } else if (graph->direction < 0 || graph->direction > 1) {
          lowered.failure =
              diagnostic("skin.renderer.graph.invalid",
                         "Graph direction is outside its validated domain.");
        } else if (objectRate != 0.0F) {
          auto geometry = *evaluated.geometry;
          auto cropped = region->resolved;
          if (graph->direction == 1) {
            const int height =
                truncatingJavaInt(static_cast<float>(cropped.h) * objectRate);
            const double shiftedY = static_cast<double>(cropped.y) +
                                    static_cast<double>(cropped.h) -
                                    static_cast<double>(height);
            if (shiftedY <
                    static_cast<double>(std::numeric_limits<int>::min()) ||
                shiftedY >
                    static_cast<double>(std::numeric_limits<int>::max())) {
              lowered.failure = diagnostic(
                  "skin.renderer.graph.range",
                  "Graph source crop exceeds its safe integer range.");
            } else {
              cropped.y = static_cast<int>(shiftedY);
            }
            cropped.h = height;
            geometry.rect.height =
                static_cast<float>(geometry.rect.height) * objectRate;
          } else {
            cropped.w =
                truncatingJavaInt(static_cast<float>(cropped.w) * objectRate);
            geometry.rect.width =
                static_cast<float>(geometry.rect.width) * objectRate;
          }
          if (!lowered.failure) {
            lowered = lowerPreparedQuad(
                inputs, object->id, destination.presentation.authoredOrdinal,
                geometry, sourceResource, resource->width, resource->height,
                cropped, true);
          }
        }
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.command) {
          if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
            if (reportObjectFailure(result, *object,
                                    diagnostic("skin.renderer.command.limit",
                                               "Slider/graph command exceeds "
                                               "the fixed frame limit."))) {
              return result;
            }
            continue;
          }
          buffer.commands.push_back(std::move(*lowered.command));
          if (slider) {
            PresentationUiControlKind interactionKind =
                PresentationUiControlKind::Slider;
            if (slider->writer &&
                laneCoverRateProperty(inputs.model, *slider)) {
              interactionKind = PresentationUiControlKind::LaneCover;
            }
            interactionLayout.slidersTopmostFirst.push_back(
                sliderInteraction(object->id,
                                  destination.presentation.authoredOrdinal,
                                  *evaluated.geometry, *slider,
                                  interactionKind));
            interactionLayout.controlsTopmostFirst.push_back(
                interactionLayout.slidersTopmostFirst.back());
          }
        }
        continue;
      }

      if (judge) {
        auto lowered = lowerJudge(
            inputs, lookupIndex, objects, destination, *judge, *judgeState,
            evaluated.geometry ? &*evaluated.geometry : nullptr,
            destinationVisible || destination.presentation.frames.empty());
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.commands.size() >
            skinFrameMaximumCommands(inputs) - buffer.commands.size()) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "Judge commands exceed the fixed frame limit."))) {
            return result;
          }
          continue;
        }
        buffer.commands.insert(
            buffer.commands.end(),
            std::make_move_iterator(lowered.commands.begin()),
            std::make_move_iterator(lowered.commands.end()));
        continue;
      }

      if (note) {
        auto prepared = prepareNoteVisuals(inputs, lookupIndex, *note);
        if (prepared.failure) {
          if (reportObjectFailure(result, *object, *prepared.failure)) {
            return result;
          }
          continue;
        }
        if (!destinationVisible) {
          continue;
        }
        if (evaluated.geometry &&
            !noteOuterClipDrawable(inputs, *evaluated.geometry)) {
          continue;
        }
        deferredNotes.push_back(
            {.insertionIndex = buffer.commands.size(),
             .object = object,
             .destination = &destination,
             .note = note,
             .geometry = std::move(evaluated.geometry),
             .offsets = std::move(offsets),
             .preparedVisuals = std::move(prepared.visuals)});
        continue;
      }

      if (cover) {
        const auto selectedCover =
            selectSpriteFrame(inputs, lookupIndex, cover->sprite);
        if (selectedCover.failure) {
          if (reportObjectFailure(result, *object, *selectedCover.failure)) {
            return result;
          }
          continue;
        }
        if (selectedCover.suppressed || !selectedCover.frame) {
          continue;
        }
        if (!evaluated.geometry) {
          continue;
        }
        auto geometry = *evaluated.geometry;
        if (cover->disappearLine >= 0.0) {
          if (cover->disappearLineLinksLift && !coverLiftOffsetY) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic("skin.renderer.cover.lift",
                               "Linked cover has no resolved Lift offset."))) {
              return result;
            }
            continue;
          }
          const double line =
              cover->disappearLine +
              (cover->disappearLineLinksLift ? *coverLiftOffsetY : 0.0);
          const double top = geometry.rect.y + geometry.rect.height;
          if (top <= line) {
            continue;
          }
          if (geometry.rect.y < line) {
            bool emptyClip = false;
            geometry.clip = intersectAuthoredRects(
                geometry.clip,
                AuthoredRect{.x = geometry.rect.x,
                             .y = line,
                             .width = geometry.rect.width,
                             .height = top - line},
                emptyClip);
            if (emptyClip) {
              continue;
            }
          }
        }
        auto lowered = lowerSpriteQuad(
            inputs, object->id, destination.presentation.authoredOrdinal,
            geometry, cover->sprite, *selectedCover.frame);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.command) {
          if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
            if (reportObjectFailure(
                    result, *object,
                    diagnostic(
                        "skin.renderer.command.limit",
                        "Cover command exceeds the fixed frame limit."))) {
              return result;
            }
            continue;
          }
          buffer.commands.push_back(std::move(*lowered.command));
        }
        continue;
      }

      if (bga) {
        const auto practiceState = inputs.state.practiceState();
        if (practiceState.practiceMode && !modelHasPracticeObject) {
          auto lowered = lowerPracticeLegacy(
              inputs, lookupIndex, *object, destination, *evaluated.geometry,
              practiceState,
              skinFrameMaximumGlyphInstances(inputs) - glyphInstances,
              skinFrameMaximumCommands(inputs) - buffer.commands.size(),
              skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices);
          if (lowered.failure) {
            if (reportObjectFailure(result, *object, *lowered.failure)) {
              return result;
            }
            continue;
          }
          glyphInstances += lowered.glyphCount;
          primitiveVertices += lowered.primitiveVertices;
          buffer.commands.insert(
              buffer.commands.end(),
              std::make_move_iterator(lowered.commands.begin()),
              std::make_move_iterator(lowered.commands.end()));
          continue;
        }
        if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "BGA command exceeds the fixed frame limit."))) {
            return result;
          }
          continue;
        }
        SkinBgaCommand command{.authoredGeometry = *evaluated.geometry,
                               .viewport = inputs.viewport,
                               .authoredOrdinal =
                                   destination.presentation.authoredOrdinal};
        buffer.commands.push_back(
            {.authoredOrdinal = destination.presentation.authoredOrdinal,
             .sourceObject = object->id,
             .payload = std::move(command)});
        continue;
      }

      if (image) {
        if (preparedMovie != nullptr) {
          movieTimeMillis = selectMovieTime(inputs);
        } else {
          selected = selectSpriteFrame(inputs, lookupIndex,
                                       image->orderedStates[stateIndex]);
          if (selected.failure) {
            if (reportObjectFailure(result, *object, *selected.failure)) {
              return result;
            }
            continue;
          }
          if (selected.suppressed) {
            continue;
          }
        }
      } else if (numericLayout) {
        if (!selectNumericAnimation(inputs, lookupIndex, *numericLayout)) {
          if (numericLayout->failure &&
              reportObjectFailure(result, *object, *numericLayout->failure)) {
            return result;
          }
          continue;
        }
      } else if (!pmChara) {
        textLayout = prepareTextLayout(
            inputs, lookupIndex, *object, *text,
            skinFrameMaximumGlyphInstances(inputs) - glyphInstances);
        if (textLayout->failure) {
          if (reportObjectFailure(result, *object, *textLayout->failure)) {
            return result;
          }
          continue;
        }
        if (textLayout->suppressed) {
          continue;
        }
      }
      if (!evaluated.geometry) {
        continue;
      }
      if (evaluated.geometry->rgba[3] <= 0.0F) {
        continue;
      }

      if (preparedMovie != nullptr) {
        if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "Movie command exceeds the fixed frame limit."))) {
            return result;
          }
          continue;
        }
        SkinMovieCommand command{
            .resource = preparedMovie->resource.id,
            .sourceTimeMillis = movieTimeMillis,
            .geometry = *evaluated.geometry,
            .state = {.blend = evaluated.geometry->blend,
                      .filter = evaluated.geometry->filter}};
        buffer.commands.push_back(
            {.authoredOrdinal = destination.presentation.authoredOrdinal,
             .sourceObject = object->id,
             .payload = std::move(command)});
        continue;
      }

      if (numericLayout) {
        auto lowered = lowerNumeric(inputs, *object, destination,
                                    *evaluated.geometry, *numericLayout);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.commands.size() >
            skinFrameMaximumCommands(inputs) - buffer.commands.size()) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic(
                      "skin.renderer.command.limit",
                      "Numeric commands exceed the fixed frame limit."))) {
            return result;
          }
          continue;
        }
        buffer.commands.insert(
            buffer.commands.end(),
            std::make_move_iterator(lowered.commands.begin()),
            std::make_move_iterator(lowered.commands.end()));
        continue;
      }
      if (textLayout) {
        auto lowered = lowerText(inputs, *object, destination,
                                 *evaluated.geometry, *text, *textLayout);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.command &&
            (buffer.commands.size() >= skinFrameMaximumCommands(inputs) ||
            lowered.glyphCount >
                skinFrameMaximumGlyphInstances(inputs) - glyphInstances)) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "Text commands exceed a fixed frame limit."))) {
            return result;
          }
          continue;
        }
        if (text->editable && text->writer) {
          interactionLayout.textsTopmostFirst.push_back(textInteraction(
              object->id, destination.presentation.authoredOrdinal,
              *evaluated.geometry, *text, textLayout->value));
          interactionLayout.controlsTopmostFirst.push_back(
              interactionLayout.textsTopmostFirst.back());
        }
        if (lowered.command) {
          glyphInstances += lowered.glyphCount;
          buffer.commands.push_back(std::move(*lowered.command));
        }
        continue;
      }

      const SkinSpriteFrames &sprite = image->orderedStates[stateIndex];
      const auto *resource = inputs.resources.find(sprite.resource);
      const auto *region =
          inputs.resources.findResolvedRegion(sprite.resource, *selected.frame);
      if (!resource || !region || resource->width <= 0 ||
          resource->height <= 0 || region->resolved.w <= 0 ||
          region->resolved.h <= 0) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.resource.missing",
                           "Prepared image resource or region is absent."))) {
          return result;
        }
        continue;
      }
      const auto projected =
          projectSkinDestinationToUi(*evaluated.geometry,
                                     {.textureWidth = resource->width,
                                      .textureHeight = resource->height,
                                      .region = region->resolved},
                                     inputs.viewport);
      bool emptyClip = false;
      const auto clip = intersectClip(
          projected.clip, projectedSkinScissorBounds(inputs.viewport), emptyClip);
      if (emptyClip) {
        continue;
      }
      if (buffer.commands.size() >= skinFrameMaximumCommands(inputs)) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.command.limit",
                           "Skin command buffer exceeds its fixed limit."))) {
          return result;
        }
        continue;
      }

      SkinTexturedQuadCommand command;
      command.resource = sprite.resource;
      command.state = {.blend = projected.blend,
                       .filter = projected.filter,
                       .scissor = clip};
      const std::uint32_t color = packAbgr(projected.rgba);
      for (std::size_t index = 0; index < command.vertices.size(); ++index) {
        command.vertices[index] = {
            .x = static_cast<float>(projected.vertices[index][0]),
            .y = static_cast<float>(projected.vertices[index][1]),
            .u = static_cast<float>(projected.normalizedUvs[index][0]),
            .v = static_cast<float>(projected.normalizedUvs[index][1]),
            .rgba = color};
      }
      buffer.commands.push_back(
          {.authoredOrdinal = destination.presentation.authoredOrdinal,
           .sourceObject = object->id,
           .payload = std::move(command)});
      if (image->clickEvent && image->clickMode >= 0 &&
          image->clickMode <= 3) {
        interactionLayout.imagesTopmostFirst.push_back(imageInteraction(
            object->id, destination.presentation.authoredOrdinal,
            *evaluated.geometry, *image));
        interactionLayout.controlsTopmostFirst.push_back(
            interactionLayout.imagesTopmostFirst.back());
      }
    }

    std::size_t insertedNoteCommands = 0;
    for (auto &deferred : deferredNotes) {
      auto lowered = lowerNoteObject(
          inputs, lookupIndex, *deferred.object, *deferred.destination,
          *deferred.note,
          deferred.geometry ? &*deferred.geometry : nullptr, deferred.offsets,
          mergedProjectionElements, deferred.preparedVisuals);
      if (lowered.failure) {
        if (reportObjectFailure(result, *deferred.object, *lowered.failure)) {
          return result;
        }
        continue;
      }
      if (lowered.commands.size() >
              skinFrameMaximumCommands(inputs) - buffer.commands.size() ||
          lowered.primitiveVertices >
              skinFrameMaximumPrimitiveVertices(inputs) - primitiveVertices) {
        if (reportObjectFailure(
                result, *deferred.object,
                diagnostic("skin.renderer.command.limit",
                           "Note commands exceed a fixed frame limit."))) {
          return result;
        }
        continue;
      }
      primitiveVertices += lowered.primitiveVertices;
      const auto insertion =
          buffer.commands.begin() +
          static_cast<std::ptrdiff_t>(deferred.insertionIndex +
                                      insertedNoteCommands);
      insertedNoteCommands += lowered.commands.size();
      buffer.commands.insert(insertion,
                             std::make_move_iterator(lowered.commands.begin()),
                             std::make_move_iterator(lowered.commands.end()));
    }

    // The final loaded SkinNote owns Beatoraja's lane geometry. Publish only
    // when that exact object participated in this evaluated frame, so the
    // synthetic replay overlay cannot outlive a hidden/disabled note object.
    if (noteLayoutSource != nullptr) {
      const auto layout = std::ranges::find_if(
          deferredNotes,
          [noteLayoutSource](const DeferredNoteLowering &deferred) {
            return deferred.object == noteLayoutSource;
          });
      if (layout != deferredNotes.end() && layout->note != nullptr &&
          !layout->note->lanes.empty()) {
        double offsetX = 0.0;
        double offsetY = 0.0;
        double offsetWidth = 0.0;
        double offsetHeight = 0.0;
        for (const auto &offset : layout->offsets) {
          offsetX += offset.x;
          offsetY += offset.y;
          offsetWidth += offset.w;
          offsetHeight += offset.h;
        }
        SyntheticReplayGhostGeometry replayGhostGeometry{
            .frameSerial = inputs.frameSerial,
            .viewport = inputs.viewport,
            .sharedLaneOriginY =
                layout->note->lanes.front().laneDestination.y,
            .sharedLaneHeight =
                layout->note->lanes.front().laneDestination.height};
        for (const auto &lane : layout->note->lanes) {
          if (lane.authoredLane < 0) {
            continue;
          }
          bool emptyClip = false;
          const auto clip = intersectAuthoredRects(
              layout->geometry ? layout->geometry->clip : std::nullopt,
              AuthoredRect{.x = lane.laneDestination.x,
                           .y = lane.laneDestination.y,
                           .width = lane.laneDestination.width,
                           .height = lane.laneDestination.height},
              emptyClip);
          const double noteHeight = lane.authoredNoteHeight.value_or(8.0);
          const AuthoredRect normalNote{
              .x = lane.laneDestination.x + offsetX,
              .y = lane.laneDestination.y + offsetY,
              .width = lane.laneDestination.width + offsetWidth,
              .height = noteHeight + offsetHeight};
          if (emptyClip || !clip || !std::isfinite(normalNote.x) ||
              !std::isfinite(normalNote.y) || !std::isfinite(normalNote.width) ||
              !std::isfinite(normalNote.height) || normalNote.width <= 0.0 ||
              normalNote.height <= 0.0) {
            continue;
          }
          replayGhostGeometry.lanes.push_back(
              {.lane = lane.authoredLane,
               .normalNote = normalNote,
               .clip = *clip});
        }
        if (!replayGhostGeometry.lanes.empty() &&
            std::isfinite(replayGhostGeometry.sharedLaneHeight) &&
            replayGhostGeometry.sharedLaneHeight > 0.0) {
          result.syntheticReplayGhostGeometry = std::move(replayGhostGeometry);
        }
      }
    }

    buildAdjacentBatches(buffer);
    std::ranges::reverse(interactionLayout.slidersTopmostFirst);
    std::ranges::reverse(interactionLayout.imagesTopmostFirst);
    std::ranges::reverse(interactionLayout.textsTopmostFirst);
    std::ranges::reverse(interactionLayout.controlsTopmostFirst);
    result.submitReady = std::move(buffer);
    result.interactionLayout = std::move(interactionLayout);
    return result;
  } catch (...) {
    result.submitReady.reset();
    result.diagnostics.push_back(diagnostic(
        "skin.renderer.allocation",
        "Skin frame evaluation could not allocate bounded command storage."));
    return result;
  }
}

} // namespace skin

#endif
