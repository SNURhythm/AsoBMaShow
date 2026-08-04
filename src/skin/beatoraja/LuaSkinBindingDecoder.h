#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinRuntime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace skin {

enum class SkinBindingKind : std::uint8_t {
  BooleanProperty,
  IntegerProperty,
  FloatProperty,
  StringProperty,
  TimerProperty,
  FloatWriter,
  StringWriter,
  Event,
};

struct SkinBindingType {
  SkinBindingKind kind = SkinBindingKind::BooleanProperty;
  SkinIntegerPropertyDomain integerDomain =
      SkinIntegerPropertyDomain::IntegerValue;
  SkinFloatPropertyDomain floatDomain = SkinFloatPropertyDomain::Rate;
};

struct SkinBuiltinBindingCatalogEntry {
  SkinBindingType type;
  SkinBuiltinPropertySelector selector;
};

// Some audited factories have a contiguous numeric selector contract. Ranges
// keep those contracts finite and typed without materializing billions of
// catalog entries. Empty/default catalog views remain exact-entry-only.
struct SkinBuiltinBindingCatalogRange {
  SkinBindingType type;
  int first = 0;
  int last = -1;
};

class SkinBuiltinBindingCatalogView final {
public:
  SkinBuiltinBindingCatalogView() noexcept = default;
  explicit SkinBuiltinBindingCatalogView(
      std::span<const SkinBuiltinBindingCatalogEntry> entries,
      std::span<const SkinBuiltinBindingCatalogRange> ranges = {}) noexcept
      : entries_(entries), ranges_(ranges) {}

  [[nodiscard]] bool
  contains(SkinBindingType type,
           const SkinBuiltinPropertySelector &selector) const noexcept {
    for (const auto &entry : entries_) {
      if (sameType(type, entry.type) &&
          entry.selector.value == selector.value) {
        return true;
      }
    }
    const auto *numeric = std::get_if<int>(&selector.value);
    if (numeric != nullptr) {
      for (const auto &range : ranges_) {
        if (sameType(type, range.type) && *numeric >= range.first &&
            *numeric <= range.last) {
          return true;
        }
      }
    }
    return false;
  }

private:
  [[nodiscard]] static bool sameType(SkinBindingType left,
                                     SkinBindingType right) noexcept {
    return left.kind == right.kind &&
           (left.kind != SkinBindingKind::IntegerProperty ||
            left.integerDomain == right.integerDomain) &&
           (left.kind != SkinBindingKind::FloatProperty ||
            left.floatDomain == right.floatDomain);
  }

  std::span<const SkinBuiltinBindingCatalogEntry> entries_;
  std::span<const SkinBuiltinBindingCatalogRange> ranges_;
};

struct SkinBindingCatalogView {
  std::span<const SkinBooleanPropertyBinding> booleanProperties;
  std::span<const SkinIntegerPropertyBinding> integerProperties;
  std::span<const SkinFloatPropertyBinding> floatProperties;
  std::span<const SkinStringPropertyBinding> stringProperties;
  std::span<const SkinTimerPropertyBinding> timerProperties;
  std::span<const SkinFloatWriterBinding> floatWriters;
  std::span<const SkinStringWriterBinding> stringWriters;
  std::span<const SkinEventBinding> events;
};

struct SkinBindingValidationContext {
  SkinBuiltinBindingCatalogView builtins;
  LuaCallbackLivenessView callbacks;
};

using SkinDecodedBindingId =
    std::variant<SkinBooleanPropertyId, SkinIntegerPropertyId,
                 SkinFloatPropertyId, SkinStringPropertyId, SkinTimerPropertyId,
                 SkinFloatWriterId, SkinStringWriterId, SkinEventBindingId>;

struct LuaSkinBindingRequest {
  SkinBindingType type;
  LuaValuePath path;
  std::uint32_t authoredOrdinal = 0;
  std::optional<int> fallbackNumeric;
  // Synthesized numeric overloads (Text.ref and implicit Slider.type) are
  // selected by the loader without serializing or inspecting an authored Lua
  // field.  This keeps ignored event fields out of callback/work accounting.
  bool numericFallbackOnly = false;
};

struct LuaSkinBindingDecodeResult {
  std::optional<SkinDecodedBindingId> id;
  std::optional<SkinDiagnostic> failure;
};

// Invalid decoder requests and session-global resource failures abort model
// decoding. User-authored binding failures remain typed zero dependencies for
// the validator to dispose at object/session scope.
[[nodiscard]] bool luaSkinBindingFailureIsFatal(std::string_view code) noexcept;

struct LuaSkinBindingDecoderPolicy {
  static constexpr std::size_t maxBindingsPerKind = 20'000;
  // The aggregate matches the table decoder's 8 MiB copied-text ceiling. One
  // inline name/script may consume at most 1/128 of that session allowance.
  static constexpr std::size_t maxSourceTextBytes = 64 * 1024;
  static constexpr std::size_t maxSourceWorkBytes = 8 * 1024 * 1024;
};

class LuaSkinBindingDecoder final {
public:
  LuaSkinBindingDecoder(LuaSkinRuntime &runtime,
                        SkinBuiltinBindingCatalogView builtins) noexcept
      : runtime_(&runtime), builtins_(builtins) {}

  LuaSkinBindingDecodeResult decode(const LuaValueHandle &,
                                    const LuaSkinBindingRequest &);
  [[nodiscard]] SkinBindingCatalogView bindings() const noexcept;

private:
  struct InternKey {
    SkinBindingType type;
    LuaBindingSourceValue source;
    bool script = false;

    bool operator==(const InternKey &) const noexcept;
  };

  struct InternKeyHash {
    std::size_t operator()(const InternKey &) const noexcept;
  };

  LuaSkinRuntime *runtime_ = nullptr;
  SkinBuiltinBindingCatalogView builtins_;
  std::size_t consumedSourceWorkBytes_ = 0;
  std::unordered_map<InternKey, SkinDecodedBindingId, InternKeyHash> interned_;
  std::vector<SkinBooleanPropertyBinding> booleanProperties_;
  std::vector<SkinIntegerPropertyBinding> integerProperties_;
  std::vector<SkinFloatPropertyBinding> floatProperties_;
  std::vector<SkinStringPropertyBinding> stringProperties_;
  std::vector<SkinTimerPropertyBinding> timerProperties_;
  std::vector<SkinFloatWriterBinding> floatWriters_;
  std::vector<SkinStringWriterBinding> stringWriters_;
  std::vector<SkinEventBinding> events_;
};

} // namespace skin
