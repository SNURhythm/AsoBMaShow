#pragma once

#include "../SkinPresentationTypes.h"
#include "BeatorajaSkinConfiguration.h"
#include "LuaSkinRuntime.h"
#include "SkinCompatibilityDiagnostics.h"

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace skin {

struct SkinHeaderCategory {
  std::string name;
  std::vector<std::string> items;
};

struct SkinHeaderOptionChoice {
  std::string label;
  int value = 0;
};

struct SkinHeaderOption {
  std::string category;
  std::string name;
  std::vector<SkinHeaderOptionChoice> choices;
  std::string defaultLabel;
};

struct SkinHeaderFile {
  std::string category;
  std::string name;
  std::string pattern;
  std::string defaultValue;
};

struct SkinHeaderOffset {
  std::string category;
  std::string name;
  int id = 0;
  OffsetPermissionMask permissions = 0;
};

struct BeatorajaSkinHeader {
  int type = -1;
  int width = 1280;
  int height = 720;
  std::string name;
  std::string author;
  std::vector<SkinHeaderCategory> categories;
  std::vector<SkinHeaderOption> options;
  std::vector<SkinHeaderFile> files;
  std::vector<SkinHeaderOffset> offsets;
};

struct HeaderDecodeResult {
  std::optional<BeatorajaSkinHeader> header;
  std::vector<SkinDiagnostic> diagnostics;
};

struct ConfigurationReconcileResult {
  std::optional<BeatorajaSkinConfiguration> configuration;
  EntryProfileSettings reconciledSettings;
  std::vector<SkinDiagnostic> diagnostics;
};

using SkinObjectId = std::uint32_t;
using SkinResourceId = std::uint32_t;

template <typename Tag> struct SkinBindingId {
  std::uint32_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinBindingId &) const = default;
};

struct SkinBooleanPropertyTag;
struct SkinIntegerPropertyTag;
struct SkinFloatPropertyTag;
struct SkinStringPropertyTag;
struct SkinTimerPropertyTag;
struct SkinStringWriterTag;
struct SkinEventBindingTag;

using SkinBooleanPropertyId = SkinBindingId<SkinBooleanPropertyTag>;
using SkinIntegerPropertyId = SkinBindingId<SkinIntegerPropertyTag>;
using SkinFloatPropertyId = SkinBindingId<SkinFloatPropertyTag>;
using SkinStringPropertyId = SkinBindingId<SkinStringPropertyTag>;
using SkinTimerPropertyId = SkinBindingId<SkinTimerPropertyTag>;
using SkinStringWriterId = SkinBindingId<SkinStringWriterTag>;
using SkinEventBindingId = SkinBindingId<SkinEventBindingTag>;

struct SkinBuiltinPropertySelector {
  std::variant<int, std::string> value;
};

template <typename Id> struct SkinRuntimeBinding {
  Id id{};
  std::variant<SkinBuiltinPropertySelector, LuaCallbackId> source;
  std::uint32_t authoredOrdinal = 0;
};

using SkinBooleanPropertyBinding = SkinRuntimeBinding<SkinBooleanPropertyId>;

enum class SkinIntegerPropertyDomain : std::uint8_t {
  IntegerValue,
  ImageIndex,
};

struct SkinIntegerPropertyBinding {
  SkinIntegerPropertyId id{};
  SkinIntegerPropertyDomain domain = SkinIntegerPropertyDomain::IntegerValue;
  std::variant<SkinBuiltinPropertySelector, LuaCallbackId> source;
  std::uint32_t authoredOrdinal = 0;
};

enum class SkinFloatPropertyDomain : std::uint8_t {
  Rate,
  FloatValue,
};

struct SkinFloatPropertyBinding {
  SkinFloatPropertyId id{};
  SkinFloatPropertyDomain domain = SkinFloatPropertyDomain::Rate;
  std::variant<SkinBuiltinPropertySelector, LuaCallbackId> source;
  std::uint32_t authoredOrdinal = 0;
};

using SkinStringPropertyBinding = SkinRuntimeBinding<SkinStringPropertyId>;
using SkinTimerPropertyBinding = SkinRuntimeBinding<SkinTimerPropertyId>;
using SkinFloatWriterBinding = SkinRuntimeBinding<SkinFloatWriterId>;
using SkinStringWriterBinding = SkinRuntimeBinding<SkinStringWriterId>;
using SkinEventBinding = SkinRuntimeBinding<SkinEventBindingId>;

enum class SkinValueKind : std::uint8_t { Integer, Float, String };

struct SkinSourceRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  // Grid coordinates defer -1 full-texture axes until dimensions are known.
  int gridColumn = 0;
  int gridRow = 0;
  int gridColumns = 1;
  int gridRows = 1;
};

struct SkinImageResource {
  SkinResourceId id = 0;
  std::string authoredName;
  std::string virtualPath;
  std::uint32_t authoredOrdinal = 0;
};

struct SkinFontFallbackResource {
  std::string virtualPath;
  int type = 0;
};

struct SkinFontResource {
  SkinResourceId id = 0;
  std::string authoredName;
  std::string virtualPath;
  int type = 0;
  std::vector<SkinFontFallbackResource> fallbacks;
  std::uint32_t authoredOrdinal = 0;
};

using SkinResourceDefinition =
    std::variant<SkinImageResource, SkinFontResource>;

struct SkinSpriteFrames {
  SkinResourceId resource = 0;
  std::vector<SkinSourceRect> frames;
  int cycleMillis = 0;
  std::optional<SkinTimerPropertyId> timer;
};

struct SkinImageObject {
  std::vector<SkinSpriteFrames> orderedStates;
  std::optional<SkinIntegerPropertyId> stateIndex;
  std::optional<SkinEventBindingId> clickEvent;
  int clickMode = 0;
};

enum class SkinZeroPaddingMode : std::uint8_t {
  None = 0,
  Zero = 1,
  AlternateZero = 2,
};

struct SkinDigitOffset {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct SkinDigitSpriteSet {
  SkinSpriteFrames positive;
  std::optional<SkinSpriteFrames> negative;
  int glyphsPerAnimationFrame = 0;
};

struct SkinNumberObject {
  SkinDigitSpriteSet digits;
  SkinIntegerPropertyId value{};
  int digitCount = 0;
  int spacing = 0;
  int alignment = 0;
  // JsonPlaySkinObjectLoader enables SkinNumber's relative mode for a Judge
  // count, so its destination is resolved against the selected judge image.
  bool relativeToJudgeImage = false;
  SkinZeroPaddingMode zeroPadding = SkinZeroPaddingMode::None;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct SkinFloatObject {
  SkinDigitSpriteSet digits;
  SkinFloatPropertyId value{};
  int integerDigits = 0;
  int fractionalDigits = 0;
  int spacing = 0;
  int alignment = 0;
  SkinZeroPaddingMode zeroPadding = SkinZeroPaddingMode::None;
  bool signVisible = false;
  double gain = 1.0;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct SkinTextObject {
  SkinResourceId font = 0;
  std::optional<SkinStringPropertyId> value;
  std::optional<SkinStringWriterId> writer;
  std::string literal;
  int pointSize = 0;
  int alignment = 0;
  bool wrapping = false;
  int overflow = 0;
  std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
  double outlineWidth = 0.0;
  std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
  double shadowOffsetX = 0.0;
  double shadowOffsetY = 0.0;
  double shadowSmoothness = 0.0;
  bool editable = false;
};

struct SkinSliderObject {
  struct IntegerRangeSource {
    SkinIntegerPropertyId value{};
    int minimum = 0;
    int maximum = 0;
  };

  SkinSpriteFrames knob;
  std::variant<SkinFloatPropertyId, IntegerRangeSource> value;
  std::optional<SkinFloatWriterId> writer;
  std::uint8_t direction = 0;
  double range = 0.0;
  bool changeable = true;
};

struct SkinGraphObject {
  SkinSpriteFrames fill;
  std::variant<SkinFloatPropertyId, SkinSliderObject::IntegerRangeSource> value;
  int direction = 0;
};

enum class SkinGaugeAnimationType : std::uint8_t {
  Random = 0,
  Increase = 1,
  Decrease = 2,
  Flicker = 3,
};

struct SkinGaugeObject {
  std::vector<SkinSpriteFrames> orderedNodes;
  int parts = 50;
  SkinGaugeAnimationType animation = SkinGaugeAnimationType::Random;
  int animationRange = 3;
  int animationCycleMillis = 33;
  int resultStartMillis = 0;
  int resultEndMillis = 500;
};

struct SkinAuthoredRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct SkinDestinationFrame {
  int timeMillis = 0;
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  double angleDegrees = 0.0;
  std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
  int acceleration = 0;
  std::optional<SkinSourceRect> clip;
};

struct SkinDestinationBody {
  std::optional<SkinTimerPropertyId> timer;
  // Java JsonSkin.Destination leaves an omitted primitive int at zero.
  // Explicit -1 is the one-shot/suppress-after-end mode.
  int loop = 0;
  std::vector<std::variant<int, SkinBooleanPropertyId>> conditions;
  std::vector<int> offsetIds;
  std::optional<SkinBooleanPropertyId> drawCondition;
  int center = 0;
  std::optional<SkinAuthoredRect> mouseRect;
  SkinBlendMode blend = SkinBlendMode::Normal;
  SkinFilterMode filter = SkinFilterMode::Nearest;
  SkinStretchMode stretch = SkinStretchMode::Stretch;
  std::vector<SkinDestinationFrame> frames;
  std::uint32_t authoredOrdinal = 0;
};

enum class SkinNoteVisualKind : std::uint8_t {
  Normal,
  Mine,
  Hidden,
  Processed,
  LnEnd,
  LnStart,
  LnBodyActive,
  LnBodyInactive,
  HcnEnd,
  HcnStart,
  HcnBodyActive,
  HcnBodyInactive,
  HcnDamage,
  HcnReactive,
};

struct SkinSynthesizedNoteVisual {
  SkinNoteVisualKind kind = SkinNoteVisualKind::Hidden;
};

using SkinNoteVisual =
    std::variant<SkinSpriteFrames, SkinSynthesizedNoteVisual>;

struct SkinLaneNotePresentation {
  int authoredLane = -1;
  SkinAuthoredRect laneDestination;
  std::optional<double> authoredNoteHeight;
  std::optional<double> secondaryDestinationY;
  std::map<SkinNoteVisualKind, SkinNoteVisual> visuals;
};

enum class SkinNoteLineKind : std::uint8_t { Group, Bpm, Stop, Time };

struct SkinNoteLinePresentation {
  SkinNoteLineKind kind = SkinNoteLineKind::Group;
  std::optional<SkinSpriteFrames> sprite;
  SkinAuthoredRect laneGroupDestination;
  std::optional<SkinDestinationBody> destination;
};

enum class SkinHcnBodySlotLayout : std::uint8_t { Legacy, Modern };

struct SkinNoteObject {
  std::vector<SkinLaneNotePresentation> lanes;
  std::vector<SkinNoteLinePresentation> lines;
  std::array<int, 2> expansionRatePercent{100, 100};
  // Pinned drawLongNote selects positional slots 8/9. Their semantic names
  // are reversed between the legacy and modern JSON/Lua field families.
  SkinHcnBodySlotLayout hcnBodySlotLayout = SkinHcnBodySlotLayout::Legacy;
};

enum class SkinCoverKind : std::uint8_t { Hidden, Lift };

struct SkinCoverObject {
  SkinCoverKind kind = SkinCoverKind::Hidden;
  SkinSpriteFrames sprite;
  double disappearLine = -1.0;
  bool disappearLineLinksLift = true;
};

struct SkinNestedObjectPresentation {
  SkinObjectId object = 0;
  SkinDestinationBody destination;
};

struct SkinJudgeGradePresentation {
  std::optional<SkinNestedObjectPresentation> image;
  std::optional<SkinNestedObjectPresentation> detailNumber;
};

struct SkinJudgeObject {
  std::vector<SkinJudgeGradePresentation> grades;
  int player = 0;
  bool shiftImageByHalfDetailWidth = false;
};

struct SkinBgaObject {};

// JsonSkinLoader recognizes every negative destination ID before resolving
// authored definitions.  The upstream source reference can legitimately have
// no texture for an unknown ID, in which case SkinImage simply does not draw.
struct SkinBuiltinImageObject {
  int referenceId = 0;
};

// These gameplay widgets are accepted by the pinned loader.  Their rendering
// is intentionally deferred, but they remain live objects so their authored
// destinations do not turn into a validation failure.
struct SkinBlankObject {};

using SkinObjectPayload =
    std::variant<SkinImageObject, SkinNumberObject, SkinFloatObject,
                 SkinTextObject, SkinSliderObject, SkinGraphObject,
                 SkinGaugeObject, SkinNoteObject, SkinCoverObject,
                 SkinJudgeObject, SkinBgaObject, SkinBuiltinImageObject,
                 SkinBlankObject>;

struct SkinObjectDefinition {
  SkinObjectId id = 0;
  std::string authoredName;
  SkinObjectPayload payload;
  std::uint32_t authoredOrdinal = 0;
  bool critical = false;
};

struct SkinDestination {
  SkinObjectId object = 0;
  SkinDestinationBody presentation;
};

struct SkinCustomTimer {
  int id = 0;
  // Pinned Beatoraja also permits a passive custom timer whose value is
  // driven externally.  Absence is distinct from binding ID zero.
  std::optional<SkinTimerPropertyId> timer;
};

struct SkinCustomEvent {
  int id = 0;
  SkinEventBindingId action{};
  std::optional<SkinBooleanPropertyId> condition;
  int minimumIntervalMillis = 0;
};

struct SkinGameplayTiming {
  int fadeoutMillis = 0;
  int inputMillis = 0;
  int sceneMillis = 0;
  int closeMillis = 0;
  int loadEndMillis = 0;
  int playStartMillis = 0;
  int judgeTimerMillis = 1;
  int finishMarginMillis = 0;
};

struct BeatorajaSkinModel {
  BeatorajaSkinHeader header;
  SkinGameplayTiming timing;
  std::vector<SkinBooleanPropertyBinding> booleanProperties;
  std::vector<SkinIntegerPropertyBinding> integerProperties;
  std::vector<SkinFloatPropertyBinding> floatProperties;
  std::vector<SkinStringPropertyBinding> stringProperties;
  std::vector<SkinTimerPropertyBinding> timerProperties;
  std::vector<SkinFloatWriterBinding> floatWriters;
  std::vector<SkinStringWriterBinding> stringWriters;
  std::vector<SkinEventBinding> events;
  std::vector<SkinResourceDefinition> resources;
  std::vector<SkinObjectDefinition> objects;
  std::vector<SkinDestination> destinations;
  std::vector<SkinCustomTimer> customTimers;
  std::vector<SkinCustomEvent> customEvents;
};

struct BeatorajaSkinModelDecodeResult {
  std::optional<BeatorajaSkinModel> model;
  std::vector<SkinDiagnostic> diagnostics;
};

struct ValidatedBeatorajaSkinModel {
  BeatorajaSkinModel model;
  std::map<std::string, SkinResourceId, std::less<>> resourceIds;
  std::map<std::string, SkinObjectId, std::less<>> objectIds;
  std::vector<SkinObjectId> disabledOptionalObjects;
  std::vector<SkinFloatPropertyId> laneCoverRatePropertyIds;
  bool laneCoverRatePropertyIndexReady = false;
};

struct SkinModelValidationResult {
  std::optional<ValidatedBeatorajaSkinModel> model;
  std::vector<SkinDiagnostic> diagnostics;
  bool criticalFailure = false;
};

} // namespace skin
