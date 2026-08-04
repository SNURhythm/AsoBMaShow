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
    const auto resolved = resolveTimer(inputs, index, *timer);
    if (resolved.failure) {
      return {.failure = *resolved.failure};
    }
    if (*resolved.value == INT64_MIN) {
      return {.frame = 0};
    }
    const auto difference = static_cast<__int128>(inputs.visualTimeMicros) -
                            static_cast<__int128>(*resolved.value);
    if (difference < 0) {
      return {.frame = 0};
    }
    elapsedMicros = difference > std::numeric_limits<std::int64_t>::max()
                        ? std::numeric_limits<std::int64_t>::max()
                        : static_cast<std::int64_t>(difference);
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

NumericLayout prepareNumberLayout(const SkinFrameInputs &inputs,
                                  const FrameLookupIndex &index,
                                  const SkinNumberObject &number) {
  NumericLayout layout;
  layout.spacing = number.spacing;
  layout.alignment = number.alignment;
  layout.offsets = &number.perDigitOffsets;
  const auto resolved = resolveInteger(inputs, index, number.value);
  if (resolved.failure) {
    layout.failure = *resolved.failure;
    return layout;
  }
  if (*resolved.value <= std::numeric_limits<std::int32_t>::min() ||
      *resolved.value >= std::numeric_limits<std::int32_t>::max() ||
      number.digitCount <= 0) {
    layout.suppressed = true;
    return layout;
  }
  const auto value = static_cast<std::int32_t>(*resolved.value);
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

std::int64_t truncatingJavaLong(double value) noexcept {
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
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
    buffer.commands.reserve(std::min(inputs.model.model.destinations.size(),
                                     SkinCommandPolicy::maximumCommands));

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
      const auto *image = std::get_if<SkinImageObject>(&object->payload);
      const auto *number = std::get_if<SkinNumberObject>(&object->payload);
      const auto *floating = std::get_if<SkinFloatObject>(&object->payload);
      const auto *text = std::get_if<SkinTextObject>(&object->payload);
      if (!image && !number && !floating && !text) {
        if (reportObjectFailure(
                result, *object,
                diagnostic("skin.renderer.object.unsupported",
                           "Skin object lowering is not implemented."))) {
          return result;
        }
        continue;
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
              inputs, lookupIndex, std::get<SkinBooleanPropertyId>(condition));
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
        const auto timer =
            resolveTimer(inputs, lookupIndex, *destination.presentation.timer);
        if (timer.failure) {
          if (reportObjectFailure(result, *object, *timer.failure)) {
            return result;
          }
          continue;
        }
        timerStartMicros = *timer.value;
      }
      auto evaluated = evaluateSkinDestinationAuthored(
          destination.presentation, {.nowMicros = inputs.visualTimeMicros,
                                     .timerStartMicros = timerStartMicros,
                                     .optionConditions = std::span<const bool>(
                                         conditions.get(), conditionCount),
                                     .orderedOffsets = offsets});
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
      if (!evaluated.geometry) {
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
