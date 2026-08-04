#include "skin/beatoraja/LuaSkinBindingDecoder.h"

#include "skin/SkinProfileSettings.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-binding-test-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

constexpr std::string_view kEntry = R"lua(
if skin_config == nil then
  return {type = 0, w = 1280, h = 720, name = "Binding", author = "Tests"}
end

local shared = function(a, b)
  return (a or 0) + (b or 0) + 5
end

local many_callbacks = {}
for i = 1, 3000 do
  many_callbacks[i] = function() return i end
end

return {
  named_callback = shared,
  bindings = {
    numeric = 42,
    numeric_string = "42",
    space_numeric_string = " 42 ",
    hex_numeric_string = "0x2a",
    fractional_numeric_string = "42.9",
    plus_numeric_string = "+42",
    exponent_numeric_string = "4.2e1",
    tab_numeric_string = "\t42\t",
    hex_fraction_string = "0x2a.0",
    infinity_string = "Infinity",
    large_numeric_string = "1e9999",
    out_of_range_numeric_string = "2147483648",
    raw_positive_infinity = 1 / 0,
    raw_nan = 0 / 0,
    truncated_numeric_garbage = string.rep("9", 64) .. "x",
    unknown_numeric = 999,
    known_boolean = "known_boolean",
    known_integer = "known_integer",
    known_image_index = "known_image_index",
    known_rate = "known_rate",
    known_float = "known_float",
    known_string = "known_string",
    known_float_writer = "known_float_writer",
    known_string_writer = "known_string_writer",
    known_event = "known_event",
    shared = shared,
    shared_again = shared,
    boolean_script = "40 < 42",
    boolean_script_again = "40 < 42",
    integer_script = "40 + 2",
    float_script = "1 / 4",
    string_script = "'scripted'",
    timer_script = "function() return 9001 end",
    timer_catalog_name = "123",
    timer_budget_a = "(function() for i = 1, 11000000 do end; return function() return 1 end end)()",
    timer_budget_b = "(function() for i = 1, 11000000 do end; return function() return 2 end end)()",
    float_writer_script = "return ...",
    string_writer_script = "return ...",
    event_script = "local state = ...; return state + 13",
    invalid_script = "function(",
    quota_script = "(function() while true do end end)()",
    invalid_table = {},
    invalid_boolean = true,
    invalid_nil = nil,
    nested = {deeper = {callback = shared}},
    many_callbacks = many_callbacks,
    maximum_source = string.rep(" ", 65532) .. "true",
    oversized_source = string.rep("x", 65537),
  }
}
)lua";

class BindingFixture {
public:
  BindingFixture()
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions = temp.root() / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("BindingContract").package),
        entry(*normalizeEntryPath(package, "skin/entry.luaskin").entry) {
    const fs::path source = temp.root() / "source";
    writeText(source / "skin/entry.luaskin", kEntry);
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "binding package snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  struct Session {
    std::unique_ptr<LuaSkinRuntime> runtime;
    std::optional<LuaValueHandle> configured;
  };

  Session session() {
    if (!prepared) {
      return {};
    }
    auto fileSystem =
        LuaSkinFileSystem::create({.revision = prepared->readView(),
                                   .entry = entry,
                                   .storageRoots = roots,
                                   .profileId = *makeSkinProfileId(
                                       "11111111-1111-4111-8111-111111111111"),
                                   .allowDataWrites = true});
    expect(fileSystem.fileSystem != nullptr, "binding filesystem creates");
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(created.runtime != nullptr, "binding runtime creates");
    if (!created.runtime) {
      return {};
    }
    expect(created.runtime->loadHeader().value.has_value(),
           "binding header phase runs");
    auto configured = created.runtime->loadConfigured({});
    expect(configured.value.has_value(), "binding configured phase runs");
    return {.runtime = std::move(created.runtime),
            .configured = std::move(configured.value)};
  }

private:
  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  SkinEntryId entry;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

BindingFixture &fixture() {
  static BindingFixture value;
  return value;
}

LuaValuePath path(std::string_view field) {
  return {LuaValuePathElement::field("bindings"),
          LuaValuePathElement::field(field)};
}

LuaSkinBindingRequest request(SkinBindingKind kind, std::string_view field,
                              std::uint32_t ordinal = 0) {
  return {
      .type = {.kind = kind}, .path = path(field), .authoredOrdinal = ordinal};
}

LuaSkinBindingRequest integerRequest(SkinIntegerPropertyDomain domain,
                                     std::string_view field,
                                     std::uint32_t ordinal = 0) {
  auto value = request(SkinBindingKind::IntegerProperty, field, ordinal);
  value.type.integerDomain = domain;
  return value;
}

LuaSkinBindingRequest floatRequest(SkinFloatPropertyDomain domain,
                                   std::string_view field,
                                   std::uint32_t ordinal = 0) {
  auto value = request(SkinBindingKind::FloatProperty, field, ordinal);
  value.type.floatDomain = domain;
  return value;
}

template <typename Id>
Id decodedId(const LuaSkinBindingDecodeResult &result,
             std::string_view message) {
  expect(result.id.has_value() && !result.failure, message);
  if (!result.id) {
    return {};
  }
  const auto *id = std::get_if<Id>(&*result.id);
  expect(id != nullptr && static_cast<bool>(*id),
         "decoded binding has the requested nonzero strong ID");
  return id != nullptr ? *id : Id{};
}

template <typename Binding>
const LuaCallbackId *callbackSource(const Binding &binding) {
  return std::get_if<LuaCallbackId>(&binding.source);
}

void testPinnedDispatchAndTypedInterning() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }

  const std::array builtinEntries{
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::BooleanProperty},
          .selector =
              SkinBuiltinPropertySelector{std::string("known_boolean")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
          .selector =
              SkinBuiltinPropertySelector{std::string("known_integer")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
          .selector =
              SkinBuiltinPropertySelector{std::string("known_image_index")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{std::string("known_rate")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::FloatValue},
          .selector = SkinBuiltinPropertySelector{std::string("known_float")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{std::string("known_string")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::TimerProperty},
          .selector = SkinBuiltinPropertySelector{std::string("123")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatWriter},
          .selector =
              SkinBuiltinPropertySelector{std::string("known_float_writer")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector =
              SkinBuiltinPropertySelector{std::string("known_string_writer")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{std::string("42")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::Event},
          .selector = SkinBuiltinPropertySelector{std::string("known_event")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::BooleanProperty},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::FloatValue},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::TimerProperty},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::TimerProperty},
          .selector = SkinBuiltinPropertySelector{123}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatWriter},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{.type = {.kind = SkinBindingKind::Event},
                                     .selector =
                                         SkinBuiltinPropertySelector{42}},
  };
  const SkinBuiltinBindingCatalogView builtins(builtinEntries);
  LuaSkinBindingDecoder decoder(*session.runtime, builtins);

  const auto booleanNumeric = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::BooleanProperty, "numeric", 10)),
      "Boolean numeric selector decodes");
  const auto booleanNumericAgain = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::BooleanProperty, "numeric", 99)),
      "repeated Boolean selector decodes");
  expect(booleanNumeric == booleanNumericAgain,
         "repeated source interns once and preserves first authored ordinal");
  expect(decoder.bindings()
                 .booleanProperties[booleanNumeric.value - 1]
                 .authoredOrdinal == 10,
         "interned binding retains the first authored ordinal");
  for (const auto field : {"numeric_string", "space_numeric_string",
                           "hex_numeric_string", "fractional_numeric_string",
                           "plus_numeric_string", "exponent_numeric_string"}) {
    const auto coerced = decodedId<SkinBooleanPropertyId>(
        decoder.decode(*session.configured,
                       request(SkinBindingKind::BooleanProperty, field)),
        "LuaJ/LuaJIT-overlap numeric string decodes through the ID factory");
    expect(coerced == booleanNumeric,
           "numeric string coercion precedes name and script dispatch");
  }

  const auto namedBoolean = decodedId<SkinBooleanPropertyId>(
      decoder.decode(
          *session.configured,
          request(SkinBindingKind::BooleanProperty, "known_boolean")),
      "recognized Boolean name remains built in");
  const auto namedInteger = decodedId<SkinIntegerPropertyId>(
      decoder.decode(*session.configured,
                     integerRequest(SkinIntegerPropertyDomain::IntegerValue,
                                    "known_integer")),
      "recognized Integer name remains built in");
  const auto namedImageIndex = decodedId<SkinIntegerPropertyId>(
      decoder.decode(*session.configured,
                     integerRequest(SkinIntegerPropertyDomain::ImageIndex,
                                    "known_image_index")),
      "recognized image-index name uses its own domain");
  const auto namedRate = decodedId<SkinFloatPropertyId>(
      decoder.decode(*session.configured,
                     floatRequest(SkinFloatPropertyDomain::Rate, "known_rate")),
      "recognized rate name remains built in");
  const auto namedFloat = decodedId<SkinFloatPropertyId>(
      decoder.decode(
          *session.configured,
          floatRequest(SkinFloatPropertyDomain::FloatValue, "known_float")),
      "recognized FloatValue name uses its own domain");
  const auto namedString = decodedId<SkinStringPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringProperty, "known_string")),
      "recognized String name remains built in");
  const auto namedFloatWriter = decodedId<SkinFloatWriterId>(
      decoder.decode(*session.configured, request(SkinBindingKind::FloatWriter,
                                                  "known_float_writer")),
      "recognized FloatWriter name remains built in");
  const auto namedStringWriter = decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured, request(SkinBindingKind::StringWriter,
                                                  "known_string_writer")),
      "recognized StringWriter name remains built in");
  const auto namedEvent = decodedId<SkinEventBindingId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::Event, "known_event")),
      "recognized Event name remains built in");

  const auto namedView = decoder.bindings();
  expect(
      std::holds_alternative<SkinBuiltinPropertySelector>(
          namedView.booleanProperties[namedBoolean.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.integerProperties[namedInteger.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.integerProperties[namedImageIndex.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.floatProperties[namedRate.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.floatProperties[namedFloat.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.stringProperties[namedString.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.floatWriters[namedFloatWriter.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.stringWriters[namedStringWriter.value - 1].source) &&
          std::holds_alternative<SkinBuiltinPropertySelector>(
              namedView.events[namedEvent.value - 1].source),
      "recognized names use the seven permitted built-in factories");

  const auto timer = decodedId<SkinTimerPropertyId>(
      decoder.decode(
          *session.configured,
          request(SkinBindingKind::TimerProperty, "timer_catalog_name")),
      "Timer numeric string uses its permitted ID factory");
  const auto view = decoder.bindings();
  const auto *timerBuiltin = std::get_if<SkinBuiltinPropertySelector>(
      &view.timerProperties[timer.value - 1].source);
  expect(timerBuiltin != nullptr &&
             std::get_if<int>(&timerBuiltin->value) != nullptr &&
             *std::get_if<int>(&timerBuiltin->value) == 123,
         "Timer numeric string becomes built-in ID 123, not a callback");

  const auto numericStringWriter = decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringWriter, "numeric")),
      "StringWriter raw number falls through to its string-name factory");
  const auto writerView = decoder.bindings();
  const auto *numericStringWriterBuiltin =
      numericStringWriter &&
              numericStringWriter.value <= writerView.stringWriters.size()
          ? std::get_if<SkinBuiltinPropertySelector>(
                &writerView.stringWriters[numericStringWriter.value - 1].source)
          : nullptr;
  expect(numericStringWriterBuiltin != nullptr &&
             std::get_if<std::string>(&numericStringWriterBuiltin->value) !=
                 nullptr &&
             *std::get_if<std::string>(&numericStringWriterBuiltin->value) ==
                 "42",
         "StringWriter raw number preserves pinned LuaNumber string fallback");

  for (const auto field :
       {"tab_numeric_string", "hex_fraction_string", "infinity_string"}) {
    const auto failClosed = decodedId<SkinBooleanPropertyId>(
        decoder.decode(*session.configured,
                       request(SkinBindingKind::BooleanProperty, field)),
        "numeric spelling outside the LuaJ/LuaJIT overlap remains a script");
    expect(callbackSource(
               decoder.bindings().booleanProperties[failClosed.value - 1]) !=
               nullptr,
           "runtime-only numeric coercion never becomes a built-in selector");
  }

  for (const auto field :
       {"large_numeric_string", "out_of_range_numeric_string",
        "raw_positive_infinity", "raw_nan"}) {
    const auto unsafeNumeric = decoder.decode(
        *session.configured, request(SkinBindingKind::BooleanProperty, field));
    expect(!unsafeNumeric.id && unsafeNumeric.failure &&
               unsafeNumeric.failure->code == "skin_lua_binding_number_invalid",
           "nonfinite and out-of-range numeric coercion fails closed");
  }
  const auto truncatedGarbage = decoder.decode(
      *session.configured,
      request(SkinBindingKind::BooleanProperty, "truncated_numeric_garbage"));
  expect(!truncatedGarbage.id && truncatedGarbage.failure &&
             truncatedGarbage.failure->code ==
                 "skin_lua_callback_script_invalid",
         "LuaJ's 64-byte trailing-garbage parser quirk fails closed as script");

  const auto numericStringWriterText = decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringWriter, "numeric_string")),
      "StringWriter numeric string uses the same string-name factory");
  expect(numericStringWriterText == numericStringWriter,
         "StringWriter numeric text and numeric value intern by string form");

  const auto integerValue = decodedId<SkinIntegerPropertyId>(
      decoder.decode(
          *session.configured,
          integerRequest(SkinIntegerPropertyDomain::IntegerValue, "numeric")),
      "IntegerValue numeric selector decodes");
  const auto imageIndex = decodedId<SkinIntegerPropertyId>(
      decoder.decode(
          *session.configured,
          integerRequest(SkinIntegerPropertyDomain::ImageIndex, "numeric")),
      "ImageIndex numeric selector decodes");
  expect(integerValue != imageIndex,
         "identical Integer selectors do not intern across domains");

  const auto rate = decodedId<SkinFloatPropertyId>(
      decoder.decode(*session.configured,
                     floatRequest(SkinFloatPropertyDomain::Rate, "numeric")),
      "Rate numeric selector decodes");
  const auto floatValue = decodedId<SkinFloatPropertyId>(
      decoder.decode(
          *session.configured,
          floatRequest(SkinFloatPropertyDomain::FloatValue, "numeric")),
      "FloatValue numeric selector decodes");
  expect(rate != floatValue,
         "identical Float selectors do not intern across domains");

  decodedId<SkinStringPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringProperty, "numeric")),
      "String numeric selector uses its permitted ID factory");
  decodedId<SkinTimerPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::TimerProperty, "numeric")),
      "Timer numeric selector uses its permitted ID factory");
  decodedId<SkinFloatWriterId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::FloatWriter, "numeric")),
      "FloatWriter numeric selector uses its permitted ID factory");
  decodedId<SkinEventBindingId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::Event, "numeric")),
      "Event numeric selector uses its permitted ID factory");

  const auto functionFirst = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::BooleanProperty, "shared")),
      "nested Lua function retains as Boolean callback");
  const auto functionAgain = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::BooleanProperty, "shared_again")),
      "same Lua function retains again");
  expect(functionFirst == functionAgain,
         "same Lua function source interns within one kind");

  decodedId<SkinIntegerPropertyId>(
      decoder.decode(
          *session.configured,
          integerRequest(SkinIntegerPropertyDomain::IntegerValue, "shared")),
      "Lua function remains separately typed as Integer");
  decodedId<SkinFloatPropertyId>(
      decoder.decode(*session.configured,
                     floatRequest(SkinFloatPropertyDomain::Rate, "shared")),
      "Lua function remains separately typed as Float");
  decodedId<SkinStringPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringProperty, "shared")),
      "Lua function remains separately typed as String");
  decodedId<SkinTimerPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::TimerProperty, "shared")),
      "Lua function remains separately typed as Timer");
  decodedId<SkinFloatWriterId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::FloatWriter, "shared")),
      "Lua function remains separately typed as FloatWriter");
  decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringWriter, "shared")),
      "Lua function remains separately typed as StringWriter");
  decodedId<SkinEventBindingId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::Event, "shared")),
      "Lua function remains separately typed as Event");

  const auto catalog = decoder.bindings();
  expect(!catalog.booleanProperties.empty() &&
             !catalog.integerProperties.empty() &&
             !catalog.floatProperties.empty() &&
             !catalog.stringProperties.empty() &&
             !catalog.timerProperties.empty() &&
             !catalog.floatWriters.empty() && !catalog.stringWriters.empty() &&
             !catalog.events.empty(),
         "immutable binding catalog covers all eight pinned binding kinds");
  expect(builtins.contains(
             {.kind = SkinBindingKind::IntegerProperty,
              .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
             SkinBuiltinPropertySelector{std::string("known_integer")}) &&
             !builtins.contains(
                 {.kind = SkinBindingKind::IntegerProperty,
                  .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
                 SkinBuiltinPropertySelector{std::string("known_integer")}),
         "built-in catalog lookup is kind and domain aware");
  expect(builtins.contains({.kind = SkinBindingKind::BooleanProperty},
                           SkinBuiltinPropertySelector{42}) &&
             builtins.contains({.kind = SkinBindingKind::StringWriter},
                               SkinBuiltinPropertySelector{42}) &&
             !builtins.contains({.kind = SkinBindingKind::BooleanProperty},
                                SkinBuiltinPropertySelector{999}),
         "built-in catalog distinguishes typed fallback and unknown IDs");

  const auto wrongKindName = decoder.decode(
      *session.configured,
      integerRequest(SkinIntegerPropertyDomain::IntegerValue, "known_boolean"));
  const auto wrongKindId = decodedId<SkinIntegerPropertyId>(
      wrongKindName, "wrong-kind name follows script fallback");
  expect(callbackSource(
             decoder.bindings().integerProperties[wrongKindId.value - 1]) !=
             nullptr,
         "a name recognized for another kind never reuses that built-in");

  const auto unknownNumeric = decoder.decode(
      *session.configured,
      request(SkinBindingKind::BooleanProperty, "unknown_numeric"));
  const auto unknownNumericId = decodedId<SkinBooleanPropertyId>(
      unknownNumeric,
      "unknown numeric remains typed for validator disposition");
  const auto &unknownBinding =
      decoder.bindings().booleanProperties[unknownNumericId.value - 1];
  const auto *unknownBuiltin =
      std::get_if<SkinBuiltinPropertySelector>(&unknownBinding.source);
  expect(unknownBuiltin != nullptr &&
             !builtins.contains({.kind = SkinBindingKind::BooleanProperty},
                                *unknownBuiltin),
         "immutable host catalog lets validation reject an unknown numeric ID");

  for (const auto &[field, code] :
       std::array{std::pair{"invalid_table", "skin_lua_binding_type_invalid"},
                  std::pair{"invalid_boolean", "skin_lua_binding_type_invalid"},
                  std::pair{"invalid_nil", "skin_lua_binding_missing"}}) {
    const auto invalidValue = decoder.decode(
        *session.configured, request(SkinBindingKind::BooleanProperty, field));
    expect(
        !invalidValue.id && invalidValue.failure &&
            invalidValue.failure->code == code,
        "table, Boolean, and nil binding values fail precisely without ID 0");
  }
}

void testMissingFieldUsesTypedNumericFallback() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }

  const std::array builtinEntries{
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
          .selector = SkinBuiltinPropertySelector{42}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{42}},
  };
  LuaSkinBindingDecoder decoder(*session.runtime,
                                SkinBuiltinBindingCatalogView(builtinEntries));

  auto integerFallback = integerRequest(SkinIntegerPropertyDomain::IntegerValue,
                                        "invalid_nil", 17);
  integerFallback.fallbackNumeric = 42;
  const auto integer = decodedId<SkinIntegerPropertyId>(
      decoder.decode(*session.configured, integerFallback),
      "missing Integer field uses its authored numeric fallback");
  const auto integerView = decoder.bindings();
  const auto *integerBuiltin = std::get_if<SkinBuiltinPropertySelector>(
      &integerView.integerProperties[integer.value - 1].source);
  expect(integerBuiltin && std::get<int>(integerBuiltin->value) == 42 &&
             integerView.integerProperties[integer.value - 1].authoredOrdinal ==
                 17,
         "typed fallback preserves numeric selector and authored ordinal");

  auto writerFallback =
      request(SkinBindingKind::StringWriter, "invalid_nil", 19);
  writerFallback.fallbackNumeric = 42;
  const auto writer = decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured, writerFallback),
      "missing StringWriter field uses its authored ref fallback");
  const auto writerView = decoder.bindings();
  const auto *writerBuiltin = std::get_if<SkinBuiltinPropertySelector>(
      &writerView.stringWriters[writer.value - 1].source);
  const auto *writerSelector =
      writerBuiltin ? std::get_if<int>(&writerBuiltin->value) : nullptr;
  expect(writerSelector && *writerSelector == 42,
         "StringWriter ref fallback preserves the pinned integer overload");
}

void testNestedFunctionSurvivesConfiguredHandleDestruction() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  const auto namedCallback =
      session.configured->callbackNamed("named_callback");
  expect(namedCallback.has_value(), "legacy named lookup retains the callback");
  LuaSkinBindingDecoder decoder(*session.runtime, {});
  const LuaValuePath nestedPath{
      LuaValuePathElement::field("bindings"),
      LuaValuePathElement::field("nested"),
      LuaValuePathElement::field("deeper"),
      LuaValuePathElement::field("callback"),
  };
  auto nestedRequest = request(SkinBindingKind::Event, "shared", 7);
  nestedRequest.path = nestedPath;
  const auto id = decodedId<SkinEventBindingId>(
      decoder.decode(*session.configured, nestedRequest),
      "arbitrarily nested function decodes");
  const auto bindings = decoder.bindings();
  const auto *callback = callbackSource(bindings.events[id.value - 1]);
  expect(callback != nullptr && namedCallback && *callback == *namedCallback,
         "nested function reuses the runtime identity index");
  if (callback == nullptr) {
    return;
  }

  const LuaCallbackId retained = *callback;
  const auto liveness = session.runtime->callbackLiveness();
  const SkinBindingValidationContext validationContext{.builtins = {},
                                                       .callbacks = liveness};
  expect(
      validationContext.callbacks.contains(retained) &&
          !validationContext.callbacks.contains({}) &&
          !validationContext.callbacks.contains(
              {.slot = retained.slot, .generation = retained.generation + 1}),
      "validator context exposes immutable callback liveness without ID 0");

  session.configured.reset();
  expect(session.runtime->enterRenderPhase().ok,
         "configured handle can be destroyed before render");
  expect(session.runtime->beginFrame(1).ok, "callback frame begins");
  const std::array<LuaScalar, 2> arguments{std::int64_t{10}, std::int64_t{20}};
  const auto invoked = session.runtime->invoke(retained, arguments);
  const auto *value =
      invoked.value ? std::get_if<std::int64_t>(&*invoked.value) : nullptr;
  expect(value != nullptr && *value == 35 && !invoked.failure,
         "retained nested function survives table handle destruction");
}

void testBindingPathDepthIsBounded() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }

  LuaValuePath boundaryPath;
  boundaryPath.reserve(LuaRuntimePolicy::maxBindingPathDepth);
  boundaryPath.push_back(LuaValuePathElement::field("bindings"));
  while (boundaryPath.size() < LuaRuntimePolicy::maxBindingPathDepth) {
    boundaryPath.push_back(LuaValuePathElement::field("missing"));
  }
  const auto boundary = session.configured->lookupBindingSource(boundaryPath);
  expect(!boundary.source && boundary.failure &&
             boundary.failure->code == "skin_lua_binding_missing",
         "a binding path at the maximum depth is still evaluated");

  boundaryPath.push_back(LuaValuePathElement::field("one_over"));
  const auto oneOver = session.configured->lookupBindingSource(boundaryPath);
  expect(!oneOver.source && oneOver.failure &&
             oneOver.failure->code == "skin_lua_binding_path_too_deep",
         "a binding path one over the maximum depth is rejected precisely");
}

void testIncompleteDecoderRequestIsSessionFatal() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  LuaSkinBindingDecoder decoder(*session.runtime, {});
  const auto incomplete = decoder.decode(
      *session.configured,
      {.type = {.kind = SkinBindingKind::TimerProperty}, .path = {}});
  expect(!incomplete.id && incomplete.failure &&
             incomplete.failure->code == "skin_lua_binding_invalid" &&
             luaSkinBindingFailureIsFatal(incomplete.failure->code),
         "an incomplete internal binding request is session-fatal rather than "
         "an object-local dependency");
}

void testThousandsOfDistinctFunctionsStayIndexed() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  LuaSkinBindingDecoder decoder(*session.runtime, {});

  SkinBooleanPropertyId first;
  LuaCallbackId lastCallback;
  for (std::uint32_t index = 1; index <= 3000; ++index) {
    LuaSkinBindingRequest distinct{
        .type = {.kind = SkinBindingKind::BooleanProperty},
        .path = {LuaValuePathElement::field("bindings"),
                 LuaValuePathElement::field("many_callbacks"),
                 LuaValuePathElement::index(index)},
        .authoredOrdinal = index - 1,
    };
    const auto id = decodedId<SkinBooleanPropertyId>(
        decoder.decode(*session.configured, distinct),
        "distinct function decodes through bounded native indexes");
    if (index == 1) {
      first = id;
    }
    const auto view = decoder.bindings();
    const auto *callback = callbackSource(view.booleanProperties[id.value - 1]);
    expect(callback != nullptr && callback->slot == index,
           "each distinct function receives one indexed callback slot");
    if (callback != nullptr) {
      lastCallback = *callback;
    }
  }

  const LuaSkinBindingRequest repeated{
      .type = {.kind = SkinBindingKind::BooleanProperty},
      .path = {LuaValuePathElement::field("bindings"),
               LuaValuePathElement::field("many_callbacks"),
               LuaValuePathElement::index(1)},
      .authoredOrdinal = 4000,
  };
  const auto repeatedFirst = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured, repeated),
      "indexed callback and typed binding are reused");
  expect(first == repeatedFirst &&
             decoder.bindings().booleanProperties.size() == 3000 &&
             session.runtime->callbackLiveness().contains(lastCallback),
         "thousands of functions remain distinct while repeats intern once");
}

void testBindingSourceWorkAndIndividualTextAreBounded() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  LuaSkinBindingDecoder decoder(*session.runtime, {});
  const auto maximum =
      request(SkinBindingKind::BooleanProperty, "maximum_source", 1);
  const auto first = decodedId<SkinBooleanPropertyId>(
      decoder.decode(*session.configured, maximum),
      "one binding source at the individual text boundary decodes");
  const std::size_t allowedLookups =
      LuaSkinBindingDecoderPolicy::maxSourceWorkBytes /
      LuaSkinBindingDecoderPolicy::maxSourceTextBytes;
  for (std::size_t lookup = 1; lookup < allowedLookups; ++lookup) {
    const auto repeated = decodedId<SkinBooleanPropertyId>(
        decoder.decode(*session.configured, maximum),
        "a repeated source lookup within the cumulative work budget decodes");
    expect(repeated == first, "repeated maximum source remains interned");
  }
  const auto exhausted = decoder.decode(*session.configured, maximum);
  expect(!exhausted.id && exhausted.failure &&
             exhausted.failure->code == "skin_lua_binding_work_limit_exceeded",
         "one lookup beyond the cumulative source-work boundary is rejected");

  LuaSkinBindingDecoder oversizedDecoder(*session.runtime, {});
  const auto oversized = oversizedDecoder.decode(
      *session.configured,
      request(SkinBindingKind::BooleanProperty, "oversized_source"));
  expect(!oversized.id && oversized.failure &&
             oversized.failure->code == "skin_lua_binding_source_too_large",
         "a source one byte beyond the individual boundary is rejected");
}

void testTimerTrialsShareOneConfiguredCompilationBudget() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  LuaSkinBindingDecoder decoder(*session.runtime, {});
  const auto first =
      decoder.decode(*session.configured,
                     request(SkinBindingKind::TimerProperty, "timer_budget_a"));
  expect(first.id.has_value() && !first.failure,
         "one Timer trial below the configured compilation budget succeeds");
  const auto aggregate =
      decoder.decode(*session.configured,
                     request(SkinBindingKind::TimerProperty, "timer_budget_b"));
  expect(!aggregate.id && aggregate.failure &&
             aggregate.failure->code == "skin_lua_instruction_limit_exceeded",
         "a second individually-valid Timer trial exhausts the shared budget");
}

void testUnrecognizedScriptsCompileWithPinnedShapesAndBudgets() {
  auto session = fixture().session();
  if (!session.runtime || !session.configured) {
    return;
  }
  LuaSkinBindingDecoder decoder(*session.runtime, {});

  const auto booleanId = decodedId<SkinBooleanPropertyId>(
      decoder.decode(
          *session.configured,
          request(SkinBindingKind::BooleanProperty, "boolean_script", 1)),
      "Boolean expression script compiles");
  const auto repeatedBooleanId = decodedId<SkinBooleanPropertyId>(
      decoder.decode(
          *session.configured,
          request(SkinBindingKind::BooleanProperty, "boolean_script_again", 2)),
      "repeated Boolean expression script compiles");
  expect(booleanId == repeatedBooleanId,
         "repeated script text is compiled and interned once");
  const auto integerId = decodedId<SkinIntegerPropertyId>(
      decoder.decode(*session.configured,
                     integerRequest(SkinIntegerPropertyDomain::IntegerValue,
                                    "integer_script")),
      "Integer expression script compiles");
  const auto floatId = decodedId<SkinFloatPropertyId>(
      decoder.decode(
          *session.configured,
          floatRequest(SkinFloatPropertyDomain::FloatValue, "float_script")),
      "Float expression script compiles");
  const auto stringId = decodedId<SkinStringPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::StringProperty, "string_script")),
      "String expression script compiles");
  const auto timerId = decodedId<SkinTimerPropertyId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::TimerProperty, "timer_script")),
      "Timer script returning a function compiles and unwraps");
  const auto floatWriterId = decodedId<SkinFloatWriterId>(
      decoder.decode(*session.configured, request(SkinBindingKind::FloatWriter,
                                                  "float_writer_script")),
      "FloatWriter statement script compiles");
  const auto stringWriterId = decodedId<SkinStringWriterId>(
      decoder.decode(*session.configured, request(SkinBindingKind::StringWriter,
                                                  "string_writer_script")),
      "StringWriter statement script compiles");
  const auto eventId = decodedId<SkinEventBindingId>(
      decoder.decode(*session.configured,
                     request(SkinBindingKind::Event, "event_script")),
      "Event statement script compiles");

  const auto invalid = decoder.decode(
      *session.configured,
      request(SkinBindingKind::BooleanProperty, "invalid_script"));
  expect(!invalid.id && invalid.failure &&
             invalid.failure->code == "skin_lua_callback_script_invalid",
         "invalid script fails precisely and never creates ID 0");

  const auto catalog = decoder.bindings();
  std::vector<LuaCallbackId> callbacks{
      *callbackSource(catalog.booleanProperties[booleanId.value - 1]),
      *callbackSource(catalog.integerProperties[integerId.value - 1]),
      *callbackSource(catalog.floatProperties[floatId.value - 1]),
      *callbackSource(catalog.stringProperties[stringId.value - 1]),
      *callbackSource(catalog.timerProperties[timerId.value - 1]),
      *callbackSource(catalog.floatWriters[floatWriterId.value - 1]),
      *callbackSource(catalog.stringWriters[stringWriterId.value - 1]),
      *callbackSource(catalog.events[eventId.value - 1]),
  };
  expect(session.runtime->callbackLiveness().containsAll(callbacks),
         "all compiled callbacks are live in the validator snapshot");

  session.configured.reset();
  expect(session.runtime->enterRenderPhase().ok,
         "script session enters render");
  std::uint64_t sequence = 0;
  const auto invoke = [&](LuaCallbackId callback,
                          std::span<const LuaScalar> arguments = {}) {
    expect(session.runtime->beginFrame(++sequence).ok,
           "script callback receives a fresh frame budget");
    return session.runtime->invoke(callback, arguments);
  };
  expect(std::get<bool>(*invoke(callbacks[0]).value),
         "Boolean expression callback evaluates");
  expect(std::get<std::int64_t>(*invoke(callbacks[1]).value) == 42,
         "Integer expression callback evaluates");
  expect(std::get<double>(*invoke(callbacks[2]).value) == 0.25,
         "Float expression callback evaluates");
  expect(std::get<std::string>(*invoke(callbacks[3]).value) == "scripted",
         "String expression callback evaluates");
  expect(std::get<std::int64_t>(*invoke(callbacks[4]).value) == 9001,
         "Timer callback returned by the script evaluates");
  const std::array<LuaScalar, 1> floatArgument{0.75};
  expect(std::get<double>(*invoke(callbacks[5], floatArgument).value) == 0.75,
         "FloatWriter statement receives its argument");
  const std::array<LuaScalar, 1> stringArgument{std::string("written")};
  expect(std::get<std::string>(*invoke(callbacks[6], stringArgument).value) ==
             "written",
         "StringWriter statement receives its argument");
  // The pinned custom LuaJ jar inherits LuaValue.narg() for LuaFunction, so
  // loadEvent(LuaFunction) selects its one-argument state adapter.
  const std::array<LuaScalar, 1> eventArguments{std::int64_t{8}};
  expect(std::get<std::int64_t>(*invoke(callbacks[7], eventArguments).value) ==
             21,
         "Event statement receives the pinned one state argument");

  auto quotaSession = fixture().session();
  if (!quotaSession.runtime || !quotaSession.configured) {
    return;
  }
  LuaSkinBindingDecoder quotaDecoder(*quotaSession.runtime, {});
  const auto exhausted = quotaDecoder.decode(
      *quotaSession.configured,
      request(SkinBindingKind::TimerProperty, "quota_script"));
  expect(!exhausted.id && exhausted.failure &&
             exhausted.failure->code == "skin_lua_instruction_limit_exceeded",
         "Timer trial execution is interrupted by the load quota");
}

void testPassiveCustomTimerIsExplicitAndNeverUsesBindingZero() {
  const SkinCustomTimer passive{.id = 900, .timer = std::nullopt};
  const SkinCustomTimer active{.id = 901, .timer = SkinTimerPropertyId{1}};
  expect(!passive.timer && active.timer && active.timer->value == 1,
         "pinned passive CustomTimer is optional, not binding ID 0");
}

} // namespace

int main() {
  testPinnedDispatchAndTypedInterning();
  testMissingFieldUsesTypedNumericFallback();
  testNestedFunctionSurvivesConfiguredHandleDestruction();
  testBindingPathDepthIsBounded();
  testIncompleteDecoderRequestIsSessionFatal();
  testThousandsOfDistinctFunctionsStayIndexed();
  testBindingSourceWorkAndIndividualTextAreBounded();
  testTimerTrialsShareOneConfiguredCompilationBudget();
  testUnrecognizedScriptsCompileWithPinnedShapesAndBudgets();
  testPassiveCustomTimerIsExplicitAndNeverUsesBindingZero();
  if (failures == 0) {
    std::cout << "lua skin binding decoder tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
