#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"
#include "SkinCoverNormalization.h"
#include "../../rendering/SkinQuadBatchRenderer.h"

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
    if (value.submissionOrdinal == 0 || value.submissionOrdinal <= previous) {
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
    diagnostics.push_back(
        diagnostic("skin.renderer.projection.limit",
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
      candidate = std::min(candidate, longNotes[index[1]].submissionOrdinal);
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
    return {.failure =
                diagnostic("skin.renderer.binding.type",
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
  if (!invoked.value) {
    return {.failure =
                diagnostic("skin.renderer.binding.type",
                           "Integer callback returned no numeric value.")};
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
    const auto found = inputs.state.floatProperty(*builtin);
    if (!found.supported) {
      return {.failure = diagnostic(
                  "skin.renderer.binding.unsupported",
                  "Float property is unsupported by the frame state.")};
    }
    return {.value = found.value};
  }
  const auto invoked =
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    return {.failure = diagnostic("skin.renderer.binding.type",
                                  "Float callback returned no numeric value.")};
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
  const auto span = static_cast<std::int64_t>(integer.maximum) -
                    static_cast<std::int64_t>(integer.minimum);
  if (span == 0 || span < std::numeric_limits<int>::min() ||
      span > std::numeric_limits<int>::max()) {
    return {.failure = diagnostic(
                "skin.renderer.rate.range",
                "Integer rate needs a nonzero Java int denominator.")};
  }
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
  // subtracting min, while max-min remains Java int arithmetic.
  const float numerator =
      static_cast<float>(static_cast<int>(*resolved.value)) -
      static_cast<float>(integer.minimum);
  const float rate =
      std::abs(numerator / static_cast<float>(static_cast<int>(span)));
  if (!std::isfinite(rate)) {
    return {.failure = diagnostic(
                "skin.renderer.rate.invalid",
                "Integer rate produced a non-finite Java float value.")};
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
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value || !std::holds_alternative<std::string>(*invoked.value)) {
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
      inputs.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  if (invoked.failure) {
    return {.failure = *invoked.failure};
  }
  if (!invoked.value) {
    return {.failure = diagnostic("skin.renderer.binding.type",
                                  "Timer callback returned no numeric value.")};
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
    const auto difference =
        static_cast<__int128>(inputs.visualTimeMicros / 1000) -
        static_cast<__int128>(resolved.value / 1000);
    if (difference < 0) {
      return {.frame = 0};
    }
    const auto elapsedMillis =
        difference > std::numeric_limits<std::int64_t>::max()
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(difference);
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
    const auto scaled =
        static_cast<__int128>(position) * static_cast<__int128>(frameCount);
    frame = static_cast<std::size_t>(scaled / cycleMillis);
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
    return std::nullopt;
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
        intersectClip(projected.clip, inputs.viewport.safeUiBounds, emptyClip);
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
  std::vector<char32_t> codepoints;
  bool suppressed = false;
  std::optional<SkinDiagnostic> failure;
};

TextLayoutInput prepareTextLayout(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinObjectDefinition &object,
                                  const SkinTextObject &text,
                                  std::size_t maximumCodepoints) {
  TextLayoutInput result;
  std::string value = text.literal;
  if (text.value) {
    auto resolved = resolveString(inputs, index, *text.value);
    if (resolved.failure) {
      result.failure = *resolved.failure;
      return result;
    }
    value = std::move(*resolved.value);
  }
  if (value.empty()) {
    result.suppressed = true;
    return result;
  }
  result.atlas = inputs.resources.findTextAtlasForObject(object.id);
  if (!result.atlas || result.atlas->id == 0 || result.atlas->width <= 0 ||
      result.atlas->height <= 0 || result.atlas->lineHeight <= 0 ||
      text.pointSize <= 0) {
    result.failure =
        diagnostic("skin.renderer.text.atlas",
                   "Prepared text atlas or its fixed metrics are absent.");
    return result;
  }

  result.codepoints.reserve(std::min(value.size(), maximumCodepoints));
  std::size_t offset = 0;
  while (offset < value.size()) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(value.data() + offset),
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codepoint);
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

int pairKerning(const PreparedSkinTextAtlas &atlas, char32_t left,
                char32_t right) noexcept {
  const auto found = atlas.kerning.find({left, right});
  return found == atlas.kerning.end() ? 0 : found->second;
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
  const double scaleY = base.rect.height / text.pointSize;
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
                                     {.textureWidth = atlas.width,
                                      .textureHeight = atlas.height,
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
  bool emptyClip = false;
  const auto projectedClip = projectSkinDestinationToUi(
      base,
      {.textureWidth = atlas.width,
       .textureHeight = atlas.height,
       .region = {.x = 0, .y = 0, .w = atlas.width, .h = atlas.height}},
      inputs.viewport);
  const auto clip = intersectClip(projectedClip.clip,
                                  inputs.viewport.safeUiBounds, emptyClip);
  if (emptyClip) {
    return result;
  }
  run.state = {.blend = base.blend, .filter = base.filter, .scissor = clip};
  result.glyphCount = run.glyphs.size();
  result.command = SkinDrawCommand{.authoredOrdinal =
                                       destination.presentation.authoredOrdinal,
                                   .sourceObject = object.id,
                                   .payload = std::move(run)};
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

  std::vector<ConfigOffset> offsets;
  offsets.reserve(presentation.offsetIds.size());
  double relativeTranslationX = 0.0;
  double relativeTranslationY = 0.0;
  for (const int id : presentation.offsetIds) {
    if (id <= 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
      continue;
    }
    const auto configured = inputs.configuration.offsetsById.find(id);
    if (configured != inputs.configuration.offsetsById.end()) {
      offsets.push_back(configured->second);
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
                  int textureHeight, const SkinSourceRect &region) {
  QuadLoweringResult result;
  if (geometry.rgba[3] <= 0.0F) {
    return result;
  }
  if (resourceId == 0 || textureWidth <= 0 || textureHeight <= 0 ||
      region.w == 0 || region.h == 0) {
    result.failure = diagnostic("skin.renderer.resource.missing",
                                "Prepared image resource or region is absent.");
    return result;
  }
  const auto projected =
      projectSkinDestinationToUi(geometry,
                                 {.textureWidth = textureWidth,
                                  .textureHeight = textureHeight,
                                  .region = region},
                                 inputs.viewport);
  if (!projectedQuadFitsUpload(projected)) {
    result.failure = diagnostic(
        "skin.renderer.geometry.invalid",
        "Projected image geometry is non-finite or exceeds float range.");
    return result;
  }
  bool emptyClip = false;
  const auto clip =
      intersectClip(projected.clip, inputs.viewport.safeUiBounds, emptyClip);
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
        intersectClip(projected.clip, inputs.viewport.safeUiBounds, emptyClip);
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
  (void)intersectClip(projected.clip, inputs.viewport.safeUiBounds, emptyClip);
  return !emptyClip;
}

GameplayVisualLoweringResult
lowerNoteObject(const SkinFrameInputs &inputs, const FrameLookupIndex &index,
                const SkinObjectDefinition &object,
                const SkinDestination &destination, const SkinNoteObject &note,
                const AuthoredDestinationGeometry *outerGeometry,
                std::span<const ConfigOffset> offsets,
                std::span<const std::uint32_t> mergedOrdinals,
                const PreparedNoteVisuals &preparedVisuals) {
  GameplayVisualLoweringResult result;
  if (!outerGeometry) {
    return result;
  }
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
                                GameplayVisualLoweringResult &output) {
    const auto *visual = findNoteVisual(lane, kind);
    if (!visual) {
      output.failure =
          diagnostic("skin.renderer.note.visual",
                     "Projected note selected a visual absent from its lane.");
      return;
    }
    auto lowered = lowerNoteVisual(
        inputs, index, object.id, destination.presentation.authoredOrdinal,
        gameplayVisualGeometry(rect, outerGeometry->clip), *visual,
        &preparedVisuals[static_cast<std::size_t>(lane.authoredLane)]
                        [static_cast<std::size_t>(kind)]);
    if (lowered.failure) {
      output.failure = std::move(lowered.failure);
      return;
    }
    if (lowered.commands.size() >
            SkinCommandPolicy::maximumCommands - output.commands.size() ||
        lowered.primitiveVertices >
            SkinCommandPolicy::maximumPrimitiveVertices -
                output.primitiveVertices) {
      output.failure =
          diagnostic("skin.renderer.command.limit",
                     "Projected note visual exceeds a fixed command limit.");
      return;
    }
    output.primitiveVertices += lowered.primitiveVertices;
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

  const auto lowerProjectedNote = [&](const SkinProjectedNoteView &projected,
                                      GameplayVisualLoweringResult &output) {
    const auto *lane = laneAt(projected.lane);
    if (!lane || !std::isfinite(projected.authoredYDisplacement)) {
      output.failure = diagnostic(
          "skin.renderer.note.projection",
          "Projected note lane or authored displacement is invalid.");
      return;
    }
    SkinNoteVisualKind kind = SkinNoteVisualKind::Normal;
    bool applyOffsets = true;
    switch (projected.kind) {
    case SkinProjectedNoteKind::Normal:
      kind = projected.judged ? SkinNoteVisualKind::Processed
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
    if (applyOffsets && !resolveExpansion(output)) {
      return;
    }
    const double noteHeight = lane->authoredNoteHeight.value_or(8.0);
    SkinAuthoredRect rect;
    if (applyOffsets) {
      rect = expandChip(
          lane->laneDestination.x + offsetX,
          lane->laneDestination.y + projected.authoredYDisplacement + offsetY,
          lane->laneDestination.width + offsetWidth, noteHeight + offsetHeight,
          lane->laneDestination.width, noteHeight);
    } else {
      rect = {.x = lane->laneDestination.x,
              .y = lane->laneDestination.y + projected.authoredYDisplacement,
              .width = lane->laneDestination.width,
              .height = noteHeight};
    }
    appendVisual(*lane, kind, rect, output);
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
        if (!resolveExpansion(output)) {
          return;
        }
        const double referenceHeight = lane->authoredNoteHeight.value_or(8.0);
        const double displacement = projected.tailAuthoredYDisplacement -
                                    projected.headAuthoredYDisplacement;
        if (displacement <= 0.0) {
          return;
        }
        const auto chip =
            expandChip(lane->laneDestination.x + offsetX,
                       lane->laneDestination.y +
                           projected.headAuthoredYDisplacement + offsetY,
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
                     output);
        if (output.failure) {
          return;
        }
        if (projected.mode != SkinProjectedLongNoteMode::LN) {
          appendVisual(*lane, end,
                       {.x = chip.x,
                        .y = tailY,
                        .width = chip.width,
                        .height = chip.height},
                       output);
          if (output.failure) {
            return;
          }
        }
        appendVisual(*lane, start, chip, output);
      };

  const auto lowerProjectedLine = [&](const SkinProjectedLineView &projected,
                                      GameplayVisualLoweringResult &output) {
    if (!std::isfinite(projected.authoredYDisplacement)) {
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
      evaluated.geometry->rect.y += projected.authoredYDisplacement;
      // LaneRenderer invokes nested line images directly, so their own clip
      // is inert. The containing Note clip remains active for every group.
      evaluated.geometry->clip = outerGeometry->clip;
      auto lowered = lowerSpriteQuad(
          inputs, object.id, destination.presentation.authoredOrdinal,
          *evaluated.geometry, *line.sprite, *selected.frame);
      if (lowered.failure) {
        output.failure = *lowered.failure;
        return;
      }
      if (lowered.command) {
        if (output.commands.size() >= SkinCommandPolicy::maximumCommands) {
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
  std::array<std::size_t, 3> indices{};
  for (const std::uint32_t ordinal : mergedOrdinals) {
    if (indices[0] < notes.size() &&
        notes[indices[0]].submissionOrdinal == ordinal) {
      lowerProjectedNote(notes[indices[0]++], result);
    } else if (indices[1] < longNotes.size() &&
               longNotes[indices[1]].submissionOrdinal == ordinal) {
      lowerProjectedLongNote(longNotes[indices[1]++], result);
    } else if (indices[2] < lines.size() &&
               lines[indices[2]].submissionOrdinal == ordinal) {
      lowerProjectedLine(lines[indices[2]++], result);
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
           const AuthoredDestinationGeometry *outerGeometry) {
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
        if (outerGeometry && detailDestination.geometry->rgba[3] > 0.0F) {
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
  if (!outerGeometry) {
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
    if (inputs.sessionSerial == 0) {
      result.diagnostics.push_back(
          diagnostic("skin.renderer.session.serial",
                     "Gameplay skin session serial must be nonzero."));
      return result;
    }
    const auto begun = inputs.runtime.beginFrame(inputs.frameSerial);
    if (!begun.ok) {
      result.diagnostics.push_back(begun.failure.value_or(
          diagnostic("skin.renderer.frame.begin",
                     "Lua runtime rejected the render-frame serial.")));
      return result;
    }
    if (!inputs.viewport.valid) {
      result.diagnostics.push_back(
          diagnostic("skin.renderer.viewport.invalid",
                     "Gameplay skin viewport is not projectable."));
      return result;
    }
    if (gaugeAnimationSessionSerial_ != inputs.sessionSerial) {
      gaugeAnimationStates_.clear();
      gaugeAnimationSessionSerial_ = inputs.sessionSerial;
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
      result.diagnostics.push_back(
          diagnostic("skin.renderer.model.object_id",
                     "Gameplay skin object IDs must be unique."));
      return result;
    }

    SkinCommandBuffer buffer;
    buffer.frameSerial = inputs.frameSerial;
    std::size_t glyphInstances = 0;
    std::size_t primitiveVertices = 0;
    buffer.commands.reserve(std::min(inputs.model.model.destinations.size(),
                                     SkinCommandPolicy::maximumCommands));
    struct DeferredNoteLowering {
      std::size_t insertionIndex = 0;
      const SkinObjectDefinition *object = nullptr;
      const SkinDestination *destination = nullptr;
      const SkinNoteObject *note = nullptr;
      AuthoredDestinationGeometry geometry;
      std::vector<ConfigOffset> offsets;
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
      // Numeric option conditions are resolved by Skin.prepare's static
      // dstop filtering before an object enters Beatoraja's frame object
      // array. A rejected Gauge/Judge therefore performs no special state or
      // child preparation; only runtime Boolean dstdraw rejection continues
      // through those overrides.
      bool rejectedByConfiguredOption = false;
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
      if (rejectedByConfiguredOption) {
        continue;
      }
      const auto *image = std::get_if<SkinImageObject>(&object->payload);
      const auto *number = std::get_if<SkinNumberObject>(&object->payload);
      const auto *floating = std::get_if<SkinFloatObject>(&object->payload);
      const auto *text = std::get_if<SkinTextObject>(&object->payload);
      const auto *slider = std::get_if<SkinSliderObject>(&object->payload);
      const auto *graph = std::get_if<SkinGraphObject>(&object->payload);
      const auto *gauge = std::get_if<SkinGaugeObject>(&object->payload);
      const auto *note = std::get_if<SkinNoteObject>(&object->payload);
      const auto *cover = std::get_if<SkinCoverObject>(&object->payload);
      const auto *judge = std::get_if<SkinJudgeObject>(&object->payload);
      const auto *bga = std::get_if<SkinBgaObject>(&object->payload);
      if (!image && !number && !floating && !text && !slider && !graph &&
          !gauge && !note && !cover && !judge && !bga) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.object.unsupported",
                           "Skin object lowering is not implemented."))) {
          return result;
        }
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
          destination.presentation.conditions.size() +
          (destination.presentation.drawCondition ? 1U : 0U);
      std::unique_ptr<bool[]> conditions;
      if (conditionCount != 0) {
        conditions = std::make_unique<bool[]>(conditionCount);
      }
      std::size_t conditionIndex = 0;
      bool conditionFailure = false;
      bool destinationVisible = true;
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
      if (conditionFailure) {
        continue;
      }
      if (destinationVisible && destination.presentation.drawCondition) {
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
          !judge) {
        continue;
      }

      std::int64_t timerStartMicros = INT64_MIN;
      bool timerOff = false;
      if (destinationVisible && destination.presentation.timer) {
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
      if (destinationVisible) {
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
          !judge) {
        continue;
      }

      std::vector<ConfigOffset> offsets;
      std::optional<double> coverLiftOffsetY;
      if (destinationVisible) {
        offsets.reserve(destination.presentation.offsetIds.size());
      }
      bool offsetFailure = false;
      if (destinationVisible) {
        for (const int id : destination.presentation.offsetIds) {
          // SkinObject.setOffsetID ignores the decoder's zero default and
          // every ID outside SkinProperty's pinned 1...199 range.
          if (id <= 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
            continue;
          }
          const auto found = inputs.configuration.offsetsById.find(id);
          if (found != inputs.configuration.offsetsById.end()) {
            offsets.push_back(found->second);
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

      if (destinationVisible) {
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
          !judge) {
        continue;
      }

      if (gauge) {
        const auto gaugeState = inputs.state.gaugeState();
        const float gaugeValue = static_cast<float>(gaugeState.value);
        const float gaugeMinimum = static_cast<float>(gaugeState.minimum);
        const float gaugeMaximum = static_cast<float>(gaugeState.maximum);
        const float gaugeBorder = static_cast<float>(gaugeState.border);
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
            SkinCommandPolicy::maximumCommands - buffer.commands.size()) {
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
        const auto selectedSprite =
            selectSpriteFrame(inputs, lookupIndex, sprite);
        if (selectedSprite.failure) {
          if (reportObjectFailure(result, *object, *selectedSprite.failure)) {
            return result;
          }
          continue;
        }
        if (selectedSprite.suppressed || !selectedSprite.frame) {
          continue;
        }
        // Pinned SkinSlider/SkinGraph obtain the source image before reading
        // their RateProperty. Preserve that callback order and do not invoke a
        // value callback when the prepared source is absent.
        const auto *resource = inputs.resources.find(sprite.resource);
        const auto *region = inputs.resources.findResolvedRegion(
            sprite.resource, *selectedSprite.frame);
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
          if (!std::isfinite(slider->range) || slider->direction > 3) {
            lowered.failure = diagnostic(
                "skin.renderer.slider.invalid",
                "Slider range or direction is outside its safe domain.");
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
                geometry, sprite.resource, resource->width, resource->height,
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
          if (!lowered.failure && cropped.w != 0 && cropped.h != 0) {
            lowered = lowerPreparedQuad(
                inputs, object->id, destination.presentation.authoredOrdinal,
                geometry, sprite.resource, resource->width, resource->height,
                cropped);
          }
        }
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.command) {
          if (buffer.commands.size() >= SkinCommandPolicy::maximumCommands) {
            if (reportObjectFailure(result, *object,
                                    diagnostic("skin.renderer.command.limit",
                                               "Slider/graph command exceeds "
                                               "the fixed frame limit."))) {
              return result;
            }
            continue;
          }
          buffer.commands.push_back(std::move(*lowered.command));
        }
        continue;
      }

      if (judge) {
        auto lowered = lowerJudge(
            inputs, lookupIndex, objects, destination, *judge, *judgeState,
            evaluated.geometry ? &*evaluated.geometry : nullptr);
        if (lowered.failure) {
          if (reportObjectFailure(result, *object, *lowered.failure)) {
            return result;
          }
          continue;
        }
        if (lowered.commands.size() >
            SkinCommandPolicy::maximumCommands - buffer.commands.size()) {
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
        if (!evaluated.geometry ||
            !noteOuterClipDrawable(inputs, *evaluated.geometry)) {
          continue;
        }
        deferredNotes.push_back(
            {.insertionIndex = buffer.commands.size(),
             .object = object,
             .destination = &destination,
             .note = note,
             .geometry = *evaluated.geometry,
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
          if (buffer.commands.size() >= SkinCommandPolicy::maximumCommands) {
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
        if (buffer.commands.size() >= SkinCommandPolicy::maximumCommands) {
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
      } else if (numericLayout) {
        if (!selectNumericAnimation(inputs, lookupIndex, *numericLayout)) {
          if (numericLayout->failure &&
              reportObjectFailure(result, *object, *numericLayout->failure)) {
            return result;
          }
          continue;
        }
      } else {
        textLayout = prepareTextLayout(
            inputs, lookupIndex, *object, *text,
            SkinCommandPolicy::maximumGlyphInstances - glyphInstances);
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
            SkinCommandPolicy::maximumCommands - buffer.commands.size()) {
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
        if (!lowered.command) {
          continue;
        }
        if (buffer.commands.size() >= SkinCommandPolicy::maximumCommands ||
            lowered.glyphCount >
                SkinCommandPolicy::maximumGlyphInstances - glyphInstances) {
          if (reportObjectFailure(
                  result, *object,
                  diagnostic("skin.renderer.command.limit",
                             "Text commands exceed a fixed frame limit."))) {
            return result;
          }
          continue;
        }
        glyphInstances += lowered.glyphCount;
        buffer.commands.push_back(std::move(*lowered.command));
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

    std::size_t insertedNoteCommands = 0;
    for (auto &deferred : deferredNotes) {
      auto lowered = lowerNoteObject(
          inputs, lookupIndex, *deferred.object, *deferred.destination,
          *deferred.note, &deferred.geometry, deferred.offsets,
          mergedProjectionOrdinals, deferred.preparedVisuals);
      if (lowered.failure) {
        if (reportObjectFailure(result, *deferred.object, *lowered.failure)) {
          return result;
        }
        continue;
      }
      if (lowered.commands.size() >
              SkinCommandPolicy::maximumCommands - buffer.commands.size() ||
          lowered.primitiveVertices >
              SkinCommandPolicy::maximumPrimitiveVertices - primitiveVertices) {
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

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer, const SkinResourceCatalog &resources,
    RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer) const {
  renderer.begin(context, resources);
  if (!renderer.submit(buffer.commands)) {
    renderer.flush();
    return false;
  }
  renderer.flush();
  return true;
}

} // namespace skin

#endif
