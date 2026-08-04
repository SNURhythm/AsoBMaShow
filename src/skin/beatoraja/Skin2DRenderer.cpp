#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

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

SkinDiagnostic diagnostic(std::string code, std::string message,
                          DiagnosticSeverity severity =
                              DiagnosticSeverity::Error) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = severity};
}

template <typename T> struct ResolvedValue {
  std::optional<T> value;
  std::optional<SkinDiagnostic> failure;
};

bool sameRect(const UiLogicalRect &left, const UiLogicalRect &right) noexcept {
  return left.x == right.x && left.y == right.y &&
         left.width == right.width && left.height == right.height;
}

bool sameState(const SkinRenderState &left,
               const SkinRenderState &right) noexcept {
  if (left.blend != right.blend || left.filter != right.filter ||
      left.scissor.has_value() != right.scissor.has_value()) {
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

template <typename View>
bool strictlyIncreasingOrdinals(std::span<const View> values) noexcept {
  std::uint32_t previous = 0;
  for (const auto &value : values) {
    if (value.submissionOrdinal == 0 ||
        value.submissionOrdinal <= previous) {
      return false;
    }
    previous = value.submissionOrdinal;
  }
  return true;
}

bool checkedAdd(std::size_t &value, std::size_t increment,
                std::size_t maximum) noexcept {
  if (increment > maximum - value) {
    return false;
  }
  value += increment;
  return true;
}

bool validateAndMergeProjection(const ISkinFrameState &state,
                                std::vector<std::uint32_t> &merged,
                                std::vector<SkinDiagnostic> &diagnostics) {
  const auto notes = state.projectedNotes();
  const auto longNotes = state.projectedLongNotes();
  const auto lines = state.projectedLines();
  if (notes.size() > SkinCommandPolicy::maximumProjectedNotes ||
      longNotes.size() > SkinCommandPolicy::maximumProjectedLongNotes ||
      lines.size() > SkinCommandPolicy::maximumProjectedLines) {
    diagnostics.push_back(diagnostic(
        "skin.renderer.projection.limit",
        "Projected gameplay span exceeds its fixed frame limit."));
    return false;
  }
  std::size_t total = 0;
  if (!checkedAdd(total, notes.size(),
                  SkinCommandPolicy::maximumProjectedElements) ||
      !checkedAdd(total, longNotes.size(),
                  SkinCommandPolicy::maximumProjectedElements) ||
      !checkedAdd(total, lines.size(),
                  SkinCommandPolicy::maximumProjectedElements)) {
    diagnostics.push_back(diagnostic(
        "skin.renderer.projection.limit",
        "Projected gameplay spans exceed the aggregate frame limit."));
    return false;
  }
  if (!strictlyIncreasingOrdinals(notes) ||
      !strictlyIncreasingOrdinals(longNotes) ||
      !strictlyIncreasingOrdinals(lines)) {
    diagnostics.push_back(diagnostic(
        "skin.renderer.projection.order",
        "Projection ordinals must be nonzero and strictly increasing."));
    return false;
  }

  merged.clear();
  merged.reserve(total);
  std::array<std::size_t, 3> index{};
  while (merged.size() < total) {
    std::uint32_t candidate = std::numeric_limits<std::uint32_t>::max();
    if (index[0] < notes.size()) {
      candidate = std::min(candidate, notes[index[0]].submissionOrdinal);
    }
    if (index[1] < longNotes.size()) {
      candidate =
          std::min(candidate, longNotes[index[1]].submissionOrdinal);
    }
    if (index[2] < lines.size()) {
      candidate = std::min(candidate, lines[index[2]].submissionOrdinal);
    }
    int matches = 0;
    if (index[0] < notes.size() &&
        notes[index[0]].submissionOrdinal == candidate) {
      ++matches;
    }
    if (index[1] < longNotes.size() &&
        longNotes[index[1]].submissionOrdinal == candidate) {
      ++matches;
    }
    if (index[2] < lines.size() &&
        lines[index[2]].submissionOrdinal == candidate) {
      ++matches;
    }
    if (matches != 1) {
      diagnostics.push_back(diagnostic(
          "skin.renderer.projection.order",
          "Projection ordinals must be unique across all gameplay spans."));
      return false;
    }
    merged.push_back(candidate);
    if (index[0] < notes.size() &&
        notes[index[0]].submissionOrdinal == candidate) {
      ++index[0];
    } else if (index[1] < longNotes.size() &&
               longNotes[index[1]].submissionOrdinal == candidate) {
      ++index[1];
    } else {
      ++index[2];
    }
  }
  return true;
}

const SkinObjectDefinition *
findObject(std::span<const SkinObjectDefinition *const> objects,
           SkinObjectId id) noexcept {
  const auto found = std::lower_bound(
      objects.begin(), objects.end(), id,
      [](const SkinObjectDefinition *object, SkinObjectId value) {
        return object->id < value;
      });
  return found == objects.end() || (*found)->id != id ? nullptr : *found;
}

struct FrameLookupIndex {
  std::vector<const SkinBooleanPropertyBinding *> booleans;
  std::vector<const SkinIntegerPropertyBinding *> integers;
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

FrameLookupIndex buildFrameLookupIndex(
    const ValidatedBeatorajaSkinModel &model) {
  FrameLookupIndex index;
  index.booleans = sortedBindingPointers(model.model.booleanProperties,
                                         index.uniqueBindingIds);
  index.integers = sortedBindingPointers(model.model.integerProperties,
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
  const auto found = std::lower_bound(
      bindings.begin(), bindings.end(), id,
      [](const Binding *binding, Id value) {
        recordLookupComparison();
        return binding->id.value < value.value;
      });
  return found == bindings.end() || (*found)->id != id ? nullptr : *found;
}

bool disabledOptionalObject(const FrameLookupIndex &index,
                            SkinObjectId id) noexcept {
  const auto found = std::lower_bound(
      index.disabledOptionalObjects.begin(),
      index.disabledOptionalObjects.end(), id,
      [](SkinObjectId candidate, SkinObjectId value) {
        recordLookupComparison();
        return candidate < value;
      });
  return found != index.disabledOptionalObjects.end() && *found == id;
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
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value || !std::holds_alternative<bool>(*invoked.value)) {
    return {.failure = diagnostic(
                "skin.renderer.binding.type",
                "Boolean callback returned a non-boolean value.")};
  }
  return {.value = std::get<bool>(*invoked.value)};
}

ResolvedValue<std::int64_t> resolveInteger(const SkinFrameInputs &inputs,
                                           const FrameLookupIndex &index,
                                           SkinIntegerPropertyId id) {
  const auto *binding = findBinding(index.integers, id);
  if (!binding) {
    return {.failure = diagnostic(
                "skin.renderer.binding.missing",
                "Integer property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto found = inputs.state.integerProperty(*builtin);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "Integer property is unsupported by the frame state.")};
    }
    return {.value = found.value};
  }
  const auto invoked =
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value ||
      !std::holds_alternative<std::int64_t>(*invoked.value)) {
    return {.failure = diagnostic(
                "skin.renderer.binding.type",
                "Integer callback returned a non-integer value.")};
  }
  return {.value = std::get<std::int64_t>(*invoked.value)};
}

ResolvedValue<std::int64_t> resolveTimer(const SkinFrameInputs &inputs,
                                         const FrameLookupIndex &index,
                                         SkinTimerPropertyId id) {
  const auto *binding = findBinding(index.timers, id);
  if (!binding) {
    return {.failure = diagnostic(
                "skin.renderer.binding.missing",
                "Timer property binding is absent from the model.")};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    return {.value = inputs.state.timerProperty(*builtin)};
  }
  const auto invoked =
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value ||
      !std::holds_alternative<std::int64_t>(*invoked.value)) {
    return {.failure = diagnostic(
                "skin.renderer.binding.type",
                "Timer callback returned a non-integer value.")};
  }
  return {.value = std::get<std::int64_t>(*invoked.value)};
}

struct SpriteSelection {
  const SkinSourceRect *frame = nullptr;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

SpriteSelection selectSpriteFrame(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinSpriteFrames &sprite) {
  if (sprite.resource == 0 || sprite.frames.empty()) {
    return {.failure = diagnostic(
                "skin.renderer.sprite.invalid",
                "Sprite has no resource or animation frames.")};
  }
  std::int64_t elapsedMicros = inputs.visualTimeMicros;
  if (sprite.timer) {
    const auto timer = resolveTimer(inputs, index, *sprite.timer);
    if (timer.failure) {
      return {.failure = *timer.failure};
    }
    if (*timer.value == INT64_MIN) {
      return {.suppressed = true};
    }
    const auto difference = static_cast<__int128>(inputs.visualTimeMicros) -
                            static_cast<__int128>(*timer.value);
    if (difference < 0) {
      return {.suppressed = true};
    }
    elapsedMicros = difference > std::numeric_limits<std::int64_t>::max()
                        ? std::numeric_limits<std::int64_t>::max()
                        : static_cast<std::int64_t>(difference);
  }
  std::size_t frame = 0;
  if (sprite.frames.size() > 1 && sprite.cycleMillis > 0) {
    const std::int64_t elapsedMillis = std::max<std::int64_t>(0, elapsedMicros / 1000);
    const std::int64_t position = elapsedMillis % sprite.cycleMillis;
    const auto scaled = static_cast<__int128>(position) *
                        static_cast<__int128>(sprite.frames.size());
    frame = static_cast<std::size_t>(scaled / sprite.cycleMillis);
    frame = std::min(frame, sprite.frames.size() - 1);
  }
  return {.frame = &sprite.frames[frame]};
}

std::optional<UiLogicalRect>
intersectClip(const std::optional<UiLogicalRect> &clip,
              const UiLogicalRect &bounds, bool &empty) noexcept {
  empty = false;
  if (!clip) {
    return std::nullopt;
  }
  const double left = std::max(clip->x, bounds.x);
  const double top = std::max(clip->y, bounds.y);
  const double right =
      std::min(clip->x + clip->width, bounds.x + bounds.width);
  const double bottom =
      std::min(clip->y + clip->height, bounds.y + bounds.height);
  if (right <= left || bottom <= top) {
    empty = true;
    return std::nullopt;
  }
  return UiLogicalRect{.x = left,
                       .y = top,
                       .width = right - left,
                       .height = bottom - top};
}

std::uint8_t colorByte(float value) noexcept {
  return static_cast<std::uint8_t>(
      std::clamp(value, 0.0F, 1.0F) * 255.0F);
}

std::uint32_t packAbgr(const std::array<float, 4> &rgba) noexcept {
  const std::uint32_t red = colorByte(rgba[0]);
  const std::uint32_t green = colorByte(rgba[1]);
  const std::uint32_t blue = colorByte(rgba[2]);
  const std::uint32_t alpha = colorByte(rgba[3]);
  return (alpha << 24U) | (blue << 16U) | (green << 8U) | red;
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
  SkinFrameEvaluationResult result;
  try {
    if (inputs.state.frameSerial() != inputs.frameSerial) {
      result.diagnostics.push_back(diagnostic(
          "skin.renderer.frame.serial",
          "Frame state serial does not match the evaluation serial."));
      return result;
    }
    const auto begun = inputs.runtime.beginFrame(inputs.frameSerial);
    if (!begun.ok) {
      result.diagnostics.push_back(
          begun.failure.value_or(diagnostic(
              "skin.renderer.frame.begin",
              "Lua runtime rejected the render-frame serial.")));
      return result;
    }
    if (!inputs.viewport.valid) {
      result.diagnostics.push_back(diagnostic(
          "skin.renderer.viewport.invalid",
          "Gameplay skin viewport is not projectable."));
      return result;
    }

    std::vector<std::uint32_t> mergedProjectionOrdinals;
    if (!validateAndMergeProjection(inputs.state, mergedProjectionOrdinals,
                                    result.diagnostics)) {
      return result;
    }

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
    if (std::adjacent_find(objects.begin(), objects.end(),
                           [](const auto *left, const auto *right) {
                             return left->id == right->id;
                           }) != objects.end()) {
      result.diagnostics.push_back(diagnostic(
          "skin.renderer.model.object_id",
          "Gameplay skin object IDs must be unique."));
      return result;
    }

    SkinCommandBuffer buffer;
    buffer.frameSerial = inputs.frameSerial;
    buffer.commands.reserve(std::min(inputs.model.model.destinations.size(),
                                     SkinCommandPolicy::maximumCommands));

    for (const auto &destination : inputs.model.model.destinations) {
      const auto *object = findObject(objects, destination.object);
      if (!object) {
        result.diagnostics.push_back(diagnostic(
            "skin.renderer.model.destination_object",
            "Destination references an absent skin object."));
        return result;
      }
      if (disabledOptionalObject(lookupIndex, object->id)) {
        continue;
      }
      const auto *image = std::get_if<SkinImageObject>(&object->payload);
      if (!image) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.object.unsupported",
                           "Skin object lowering is not implemented."))) {
          return result;
        }
        continue;
      }
      if (image->orderedStates.empty()) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.image.states",
                           "Image object has no source states."))) {
          return result;
        }
        continue;
      }

      std::size_t stateIndex = 0;
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
      const auto selected = selectSpriteFrame(
          inputs, lookupIndex, image->orderedStates[stateIndex]);
      if (selected.failure) {
        if (reportObjectFailure(result, *object, *selected.failure)) {
          return result;
        }
        continue;
      }
      if (selected.suppressed) {
        continue;
      }

      const std::size_t conditionCount =
          destination.presentation.conditions.size() +
          (destination.presentation.drawCondition ? 1U : 0U);
      std::unique_ptr<bool[]> conditions;
      if (conditionCount != 0) {
        conditions = std::make_unique<bool[]>(conditionCount);
      }
      std::size_t conditionIndex = 0;
      bool conditionFailure = false;
      for (const auto &condition : destination.presentation.conditions) {
        if (const auto *configured = std::get_if<int>(&condition)) {
          bool value = false;
          if (!configuredCondition(inputs.configuration, *configured, value)) {
            conditionFailure = true;
            break;
          }
          conditions[conditionIndex++] = value;
        } else {
          const auto resolved = resolveBoolean(
              inputs, lookupIndex,
              std::get<SkinBooleanPropertyId>(condition));
          if (resolved.failure) {
            if (reportObjectFailure(result, *object, *resolved.failure)) {
              return result;
            }
            conditionFailure = true;
            break;
          }
          conditions[conditionIndex++] = *resolved.value;
        }
      }
      if (conditionFailure) {
        continue;
      }
      if (destination.presentation.drawCondition) {
        const auto resolved = resolveBoolean(
            inputs, lookupIndex, *destination.presentation.drawCondition);
        if (resolved.failure) {
          if (reportObjectFailure(result, *object, *resolved.failure)) {
            return result;
          }
          continue;
        }
        conditions[conditionIndex++] = *resolved.value;
      }

      std::vector<ConfigOffset> offsets;
      offsets.reserve(destination.presentation.offsetIds.size());
      bool offsetFailure = false;
      for (const int id : destination.presentation.offsetIds) {
        // SkinObject.setOffsetID ignores the decoder's zero default and every
        // ID outside SkinProperty's pinned 1...199 range.
        if (id <= 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
          continue;
        }
        const auto found = inputs.configuration.offsetsById.find(id);
        if (found != inputs.configuration.offsetsById.end()) {
          offsets.push_back(found->second);
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
      }
      if (offsetFailure) {
        continue;
      }

      std::int64_t timerStartMicros = INT64_MIN;
      if (destination.presentation.timer) {
        const auto timer = resolveTimer(
            inputs, lookupIndex, *destination.presentation.timer);
        if (timer.failure) {
          if (reportObjectFailure(result, *object, *timer.failure)) {
            return result;
          }
          continue;
        }
        timerStartMicros = *timer.value;
      }
      auto evaluated = evaluateSkinDestinationAuthored(
          destination.presentation,
          {.nowMicros = inputs.visualTimeMicros,
           .timerStartMicros = timerStartMicros,
           .optionConditions =
               std::span<const bool>(conditions.get(), conditionCount),
           .orderedOffsets = offsets});
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
      if (!evaluated.geometry || evaluated.geometry->rgba[3] <= 0.0F) {
        continue;
      }

      const SkinSpriteFrames &sprite = image->orderedStates[stateIndex];
      const auto *resource = inputs.resources.find(sprite.resource);
      const auto *region = inputs.resources.findResolvedRegion(
          sprite.resource, *selected.frame);
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
      const auto projected = projectSkinDestinationToUi(
          *evaluated.geometry,
          {.textureWidth = resource->width,
           .textureHeight = resource->height,
           .region = region->resolved},
          inputs.viewport);
      bool emptyClip = false;
      const auto clip = intersectClip(projected.clip,
                                      inputs.viewport.safeUiBounds, emptyClip);
      if (emptyClip) {
        continue;
      }
      if (buffer.commands.size() >= SkinCommandPolicy::maximumCommands) {
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
    }

    buildAdjacentBatches(buffer);
    result.submitReady = std::move(buffer);
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
