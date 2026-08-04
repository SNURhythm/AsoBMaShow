#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>

namespace skin {

struct SkinFloatWriterId {
  std::uint32_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinFloatWriterId &) const = default;
};

struct SkinWriterInvocation {
  SkinFloatWriterId writer{};
  float normalizedValue = 0.0F;
  long long eventMicros = 0;

  bool operator==(const SkinWriterInvocation &) const = default;
};

enum class SkinBlendMode : std::uint8_t {
  Normal,
  Additive,
  Subtractive,
  Multiply,
  Inverse,
};

enum class SkinFilterMode : std::uint8_t {
  Nearest,
  Linear,
};

enum class SkinStretchMode : std::uint8_t {
  Stretch = 0,
  KeepAspectRatioFitInner = 1,
  KeepAspectRatioFitOuter = 2,
  KeepAspectRatioFitOuterTrimmed = 3,
  KeepAspectRatioFitWidth = 4,
  KeepAspectRatioFitWidthTrimmed = 5,
  KeepAspectRatioFitHeight = 6,
  KeepAspectRatioFitHeightTrimmed = 7,
  KeepAspectRatioNoExpanding = 8,
  NoResize = 9,
  NoResizeTrimmed = 10,
};

} // namespace skin

struct UiLogicalPoint {
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const UiLogicalPoint &) const = default;
};

enum class PresentationUiControlKind : std::uint8_t {
  None,
  LaneCover,
  Slider,
  NativeOverlay,
};

struct PresentationUiHit {
  PresentationUiControlKind kind = PresentationUiControlKind::None;
  // Stable interaction-layout identity. This intentionally does not change
  // for an ordinary render frame so a pointer captured on Down can continue
  // to address the same authored control while frames advance.
  std::uint64_t layoutRevision = 0;
  std::uint32_t sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  std::optional<skin::SkinFloatWriterId> writer;
  // Only the built-in BMS lane-cover adapter opts into the legacy scene
  // handler. Topmost skin controls and native overlays remain authoritative
  // even when their main-thread callback rejects or becomes stale.
  bool permitsLegacyBuiltInFallback = false;

  bool operator==(const PresentationUiHit &) const = default;
};

struct PresentationUiHitRegion {
  PresentationUiHit hit;
  std::array<UiLogicalPoint, 4> boundary{};

  bool operator==(const PresentationUiHitRegion &) const = default;
};

struct PresentationTouchEvent {
  long long pointerId = 0;
  UiLogicalPoint uiPoint;
  long long eventMicros = 0;
  PresentationUiHit hit;

  bool operator==(const PresentationTouchEvent &) const = default;
};

struct PresentationTouchResult {
  bool consumed = false;
  bool excludeFromGameplay = false;

  bool operator==(const PresentationTouchResult &) const = default;
};
