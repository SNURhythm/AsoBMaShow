#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/JsonGameplaySkinDecoder.h"
#include "skin/beatoraja/Lr2GameplaySkinDecoder.h"
#include "skin/beatoraja/Lr2SkinHeaderDecoder.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/Skin2DRenderer.h"
#include "skin/beatoraja/SkinModelValidator.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Json readJson(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "cross-format expected JSON opens");
  return input.good() ? Json::parse(input, nullptr, true, true)
                      : Json::object();
}

std::vector<std::byte> readBytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "cross-format fixture opens");
  const std::string text{std::istreambuf_iterator<char>(input), {}};
  const auto bytes = std::as_bytes(std::span(text));
  return {bytes.begin(), bytes.end()};
}

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-cross-format-" + std::to_string(++serial));
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

constexpr auto kFixtureBuiltins = std::to_array({
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::IntegerProperty,
                 .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
        .selector = SkinBuiltinPropertySelector{700}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::IntegerProperty,
                 .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
        .selector = SkinBuiltinPropertySelector{701}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::IntegerProperty,
                 .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
        .selector = SkinBuiltinPropertySelector{700}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::FloatProperty,
                 .floatDomain = SkinFloatPropertyDomain::FloatValue},
        .selector = SkinBuiltinPropertySelector{800}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::FloatProperty,
                 .floatDomain = SkinFloatPropertyDomain::Rate},
        .selector = SkinBuiltinPropertySelector{800}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::FloatProperty,
                 .floatDomain = SkinFloatPropertyDomain::Rate},
        .selector = SkinBuiltinPropertySelector{17}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::TimerProperty},
        .selector = SkinBuiltinPropertySelector{42}},
    SkinBuiltinBindingCatalogEntry{
        .type = {.kind = SkinBindingKind::TimerProperty},
        .selector = SkinBuiltinPropertySelector{43}},
});

SkinBuiltinBindingCatalogView fixtureBuiltins() {
  return SkinBuiltinBindingCatalogView(kFixtureBuiltins);
}

BeatorajaSkinModelDecodeResult decodeLuaFixture() {
  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = *normalizePackageId("CrossFormatLua").package;
  const fs::path source = fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/lua/model";
  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "Lua parity fixture snapshots");
  if (!snapshot.prepared) return {};

  const auto entry =
      *normalizeEntryPath(package, "all_v1_objects.luaskin").entry;
  const auto fileSystem = [&]() {
    return LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  };
  auto runtimeFileSystem = fileSystem();
  auto reconciliationFileSystem = fileSystem();
  expect(runtimeFileSystem && reconciliationFileSystem,
         "Lua parity filesystems create");
  if (!runtimeFileSystem || !reconciliationFileSystem) return {};

  auto created = LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::Validation,
                                         .fileSystem =
                                             std::move(runtimeFileSystem)});
  expect(created.runtime != nullptr, "Lua parity runtime creates");
  if (!created.runtime) return {};
  auto value = created.runtime->loadHeader();
  expect(value.value.has_value(), "Lua parity header executes");
  if (!value.value) return {};
  LuaSkinTableDecoder decoder;
  const auto header = decoder.decodeHeader(*value.value);
  expect(header.header.has_value(), "Lua parity header decodes");
  if (!header.header) return {};
  value.value.reset();
  const auto reconciled = reconcileSkinConfiguration(
      *header.header, nullptr, *reconciliationFileSystem);
  expect(reconciled.configuration.has_value(),
         "Lua parity configuration reconciles");
  if (!reconciled.configuration) return {};
  auto configured = created.runtime->loadConfigured(*reconciled.configuration);
  expect(configured.value.has_value(), "Lua parity configured phase executes");
  if (!configured.value) return {};
  return decoder.decodeGameplay(*configured.value,
                                {.runtime = *created.runtime,
                                 .builtins = fixtureBuiltins()});
}

SkinEntryId staticEntry(std::string_view packageName,
                        std::string_view relativePath) {
  const auto package = *normalizePackageId(packageName).package;
  return *normalizeEntryPath(package, relativePath).entry;
}

BeatorajaSkinModelDecodeResult decodeJsonFixture() {
  const fs::path path = fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/json/"
                        "all_gameplay_objects.json";
  auto decoded = JsonGameplaySkinDecoder{}.decode(
      readBytes(path), staticEntry("CrossFormatJson", "all_gameplay_objects.json"),
      nullptr, fixtureBuiltins());
  return {.model = std::move(decoded.model),
          .diagnostics = std::move(decoded.diagnostics)};
}

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](char character) {
    return character >= 'a' && character <= 'z'
               ? static_cast<char>(character - ('a' - 'A'))
               : character;
  });
  return value;
}

std::vector<std::string> splitFields(std::string_view line) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      result.emplace_back(line.substr(start));
      return result;
    }
    result.emplace_back(line.substr(start, comma - start));
    start = comma + 1;
  }
}

std::vector<Lr2SkinCommand> readLr2Commands() {
  const fs::path path = fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/lr2/"
                        "all_gameplay_objects.lr2skin";
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "LR2 parity fixture opens");
  std::vector<Lr2SkinCommand> commands;
  std::string line;
  std::uint32_t lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() != '#') continue;
    auto fields = splitFields(line);
    std::string name = upperAscii(fields.front().substr(1));
    fields.erase(fields.begin());
    commands.push_back({.name = std::move(name),
                        .fields = std::move(fields),
                        .source = {.virtualPath =
                                       "all_gameplay_objects.lr2skin",
                                   .line = lineNumber,
                                   .column = 1},
                        .includeChain = {"all_gameplay_objects.lr2skin"}});
  }
  return commands;
}

BeatorajaSkinModelDecodeResult decodeLr2Fixture() {
  const auto commands = readLr2Commands();
  const auto header =
      Lr2SkinHeaderDecoder{}.decode(commands, "all_gameplay_objects.lr2skin");
  expect(header.header.has_value(), "LR2 parity header decodes");
  if (!header.header) return {};
  const auto configuration =
      reconcileLr2GameplaySkinConfiguration(*header.header, nullptr);
  expect(configuration.configuration.has_value(),
         "LR2 parity configuration reconciles");
  if (!configuration.configuration) return {};
  auto decoded = Lr2GameplaySkinDecoder{}.decode(
      *header.header, commands, nullptr, fixtureBuiltins());
  return {.model = std::move(decoded.model),
          .diagnostics = std::move(decoded.diagnostics)};
}

const SkinObjectDefinition *findSlider(const BeatorajaSkinModel &model) {
  const auto found = std::ranges::find_if(model.objects, [](const auto &object) {
    return std::holds_alternative<SkinSliderObject>(object.payload);
  });
  return found == model.objects.end() ? nullptr : &*found;
}

const SkinDestination *findDestination(const BeatorajaSkinModel &model,
                                       SkinObjectId object) {
  const auto found =
      std::ranges::find_if(model.destinations, [&](const auto &destination) {
        return destination.object == object;
      });
  return found == model.destinations.end() ? nullptr : &*found;
}

template <typename BindingSource>
std::optional<int> numericSelector(const BindingSource &source) {
  const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(&source);
  if (!builtin) return std::nullopt;
  const auto *number = std::get_if<int>(&builtin->value);
  return number ? std::optional<int>(*number) : std::nullopt;
}

Json sourceRect(const SkinSourceRect &rect) {
  return {{"x", rect.x},
          {"y", rect.y},
          {"w", rect.w},
          {"h", rect.h},
          {"gridColumn", rect.gridColumn},
          {"gridRow", rect.gridRow},
          {"gridColumns", rect.gridColumns},
          {"gridRows", rect.gridRows}};
}

Json canonicalOverlap(const BeatorajaSkinModel &model) {
  const auto *object = findSlider(model);
  expect(object != nullptr, "parity model retains its slider");
  if (!object) return Json::object();
  const auto &slider = std::get<SkinSliderObject>(object->payload);
  expect(std::holds_alternative<SkinFloatPropertyId>(slider.value),
         "parity slider uses a rate binding");
  const auto value = std::holds_alternative<SkinFloatPropertyId>(slider.value)
                         ? std::get<SkinFloatPropertyId>(slider.value)
                         : SkinFloatPropertyId{};
  const auto binding = std::ranges::find_if(
      model.floatProperties,
      [&](const auto &candidate) { return candidate.id == value; });
  expect(binding != model.floatProperties.end(),
         "parity slider binding is present");
  const auto *destination = findDestination(model, object->id);
  expect(destination != nullptr && destination->presentation.frames.size() == 1,
         "parity slider has one destination frame");
  const auto resource = std::ranges::find_if(
      model.resources, [&](const SkinResourceDefinition &candidate) {
        const auto *image = std::get_if<SkinImageResource>(&candidate);
        return image && image->id == slider.knob.resource;
      });
  expect(resource != model.resources.end(), "parity slider resource is present");

  Json frames = Json::array();
  for (const auto &frame : slider.knob.frames) frames.push_back(sourceRect(frame));
  Json destinationFrames = Json::array();
  if (destination) {
    for (const auto &frame : destination->presentation.frames) {
      destinationFrames.push_back(
          {{"timeMillis", frame.timeMillis},
           {"x", frame.x},
           {"y", frame.y},
           {"width", frame.width},
           {"height", frame.height},
           {"angleDegrees", frame.angleDegrees},
           {"rgba", frame.rgba},
           {"acceleration", frame.acceleration}});
    }
  }
  const auto *image = resource == model.resources.end()
                          ? nullptr
                          : std::get_if<SkinImageResource>(&*resource);
  Json normalizedSelector = nullptr;
  if (binding != model.floatProperties.end()) {
    if (const auto selector = numericSelector(binding->source)) {
      normalizedSelector = *selector;
    }
  }
  return {
      {"header", {{"type", model.header.type},
                  {"width", model.header.width},
                  {"height", model.header.height}}},
      {"resources", Json::array({{{"kind", "image"},
                                  {"virtualPath",
                                   image ? image->virtualPath : std::string{}}}})},
      {"bindings", Json::array({{{"kind", "float"},
                                 {"domain", "rate"},
                                 {"selector", std::move(normalizedSelector)}}})},
      {"objects", Json::array({{{"kind", "slider"},
                                {"frames", std::move(frames)},
                                {"cycleMillis", slider.knob.cycleMillis},
                                {"direction", slider.direction},
                                {"range", slider.range},
                                {"changeable", slider.changeable}}})},
      {"destinations",
       Json::array({{{"loop", destination ? destination->presentation.loop : 0},
                     {"blend", destination
                                   ? static_cast<int>(destination->presentation.blend)
                                   : 0},
                     {"filter", destination
                                    ? static_cast<int>(destination->presentation.filter)
                                    : 0},
                     {"frames", std::move(destinationFrames)}}})},
  };
}

BeatorajaSkinModel overlapProjection(const BeatorajaSkinModel &model) {
  BeatorajaSkinModel projected;
  projected.header = model.header;
  const auto *object = findSlider(model);
  if (!object) return projected;
  projected.objects.push_back(*object);
  if (const auto *destination = findDestination(model, object->id)) {
    projected.destinations.push_back(*destination);
  }
  const auto &slider = std::get<SkinSliderObject>(object->payload);
  const auto resource = std::ranges::find_if(
      model.resources, [&](const SkinResourceDefinition &candidate) {
        const auto *image = std::get_if<SkinImageResource>(&candidate);
        return image && image->id == slider.knob.resource;
      });
  if (resource != model.resources.end()) projected.resources.push_back(*resource);
  if (const auto *value = std::get_if<SkinFloatPropertyId>(&slider.value)) {
    const auto binding = std::ranges::find_if(
        model.floatProperties,
        [&](const auto &candidate) { return candidate.id == *value; });
    if (binding != model.floatProperties.end()) {
      projected.floatProperties.push_back(*binding);
    }
  }
  return projected;
}

void expectJson(const Json &actual, const Json &expected,
                std::string_view message) {
  if (actual == expected) return;
  expect(false, message);
  std::cerr << "EXPECTED:\n" << expected.dump(2) << "\nACTUAL:\n"
            << actual.dump(2) << '\n';
}

class FakeResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id, const SkinSourceRect &region) {
    PreparedSkinResource resource;
    resource.id = id;
    resource.width = 10;
    resource.height = 10;
    resource.regions = {region};
    resource.regionMappings = {{.authored = region, .resolved = region}};
    images_.emplace(id, std::move(resource));
  }

  std::optional<SkinResourceId>
  builtinImageResource(int) const noexcept override {
    return std::nullopt;
  }
  const PreparedSkinResource *find(SkinResourceId id) const noexcept override {
    const auto found = images_.find(id);
    return found == images_.end() ? nullptr : &found->second;
  }
  const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId id,
                     const SkinSourceRect &authored) const noexcept override {
    const auto *resource = find(id);
    if (!resource) return nullptr;
    const auto found = std::ranges::find_if(
        resource->regionMappings, [&](const auto &mapping) {
          return mapping.authored.x == authored.x &&
                 mapping.authored.y == authored.y &&
                 mapping.authored.w == authored.w &&
                 mapping.authored.h == authored.h &&
                 mapping.authored.gridColumn == authored.gridColumn &&
                 mapping.authored.gridRow == authored.gridRow &&
                 mapping.authored.gridColumns == authored.gridColumns &&
                 mapping.authored.gridRows == authored.gridRows;
        });
    return found == resource->regionMappings.end() ? nullptr : &*found;
  }
  const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId) const noexcept override {
    return nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept override {
    return nullptr;
  }
  const PreparedPomyuCharaResource *
  findPomyuChara(SkinObjectId) const noexcept override {
    return nullptr;
  }

private:
  std::map<SkinResourceId, PreparedSkinResource> images_;
};

class FakeState final : public ISkinFrameState {
public:
  std::uint64_t frameSerial() const noexcept override { return 1; }
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override {
    return {};
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override {
    return {.value = 0.5, .supported = true};
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override {
    return {};
  }
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override {
    return 0;
  }
  std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override {
    return {};
  }
  SkinGaugeStateView gaugeState() const noexcept override { return {}; }
  SkinJudgeStateView judgeState(int) const noexcept override { return {}; }
  SkinNoteExpansionStateView noteExpansionState() const noexcept override {
    return {};
  }
};

std::optional<ValidatedBeatorajaSkinModel>
validate(BeatorajaSkinModel model) {
  auto validated = SkinModelValidator{}.validate(
      std::move(model), {.builtins = fixtureBuiltins(), .callbacks = std::nullopt});
  expect(validated.model.has_value() && !validated.criticalFailure,
         "parity model validates");
  return std::move(validated.model);
}

Json canonicalDrawTrace(ValidatedBeatorajaSkinModel &model) {
  const auto *object = findSlider(model.model);
  if (!object) return Json::object();
  const auto &slider = std::get<SkinSliderObject>(object->payload);
  FakeResources resources;
  for (const auto &frame : slider.knob.frames) {
    resources.addImage(slider.knob.resource, frame);
    break;
  }
  FakeState state;
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 1280.0, .height = 720.0},
      {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0}, {});
  const BeatorajaSkinConfiguration configuration;
  Skin2DRenderer renderer;
  const auto evaluated = renderer.evaluateFrame(
      {.frameSerial = 1,
       .sessionSerial = 1,
       .visualTimeMicros = 0,
       .model = model,
       .configuration = configuration,
       .resources = resources,
       .viewport = viewport,
       .runtime = nullptr,
       .state = state});
  expect(evaluated.submitReady.has_value(), "parity draw frame evaluates");
  if (!evaluated.submitReady) return Json::object();
  expect(evaluated.submitReady->commands.size() == 1,
         "parity draw emits one overlapping command");
  if (evaluated.submitReady->commands.size() != 1) return Json::object();
  const auto *quad = std::get_if<SkinTexturedQuadCommand>(
      &evaluated.submitReady->commands.front().payload);
  expect(quad != nullptr, "parity slider lowers to a textured quad");
  if (!quad) return Json::object();
  Json vertices = Json::array();
  for (const auto &vertex : quad->vertices) {
    vertices.push_back({{"x", vertex.x},
                        {"y", vertex.y},
                        {"u", vertex.u},
                        {"v", vertex.v},
                        {"rgba", vertex.rgba}});
  }
  return {{"frameSerial", evaluated.submitReady->frameSerial},
          {"commands",
           Json::array({{{"kind", "textured-quad"},
                         {"vertices", std::move(vertices)},
                         {"blend", static_cast<int>(quad->state.blend)},
                         {"filter", static_cast<int>(quad->state.filter)}}})}};
}

void testCrossFormatGameplayOverlap() {
  const SkinBindingType rateType{.kind = SkinBindingKind::FloatProperty,
                                 .floatDomain =
                                     SkinFloatPropertyDomain::Rate};
  expect(gameplaySkinBuiltinCatalog().contains(
             rateType, SkinBuiltinPropertySelector{17}),
         "the shared fixture selector is in the pinned gameplay catalog");
  const auto luaDecoded = decodeLuaFixture();
  const auto jsonDecoded = decodeJsonFixture();
  const auto lr2Decoded = decodeLr2Fixture();
  expect(luaDecoded.model.has_value(), "Lua parity model decodes");
  expect(jsonDecoded.model.has_value(), "JSON parity model decodes");
  expect(lr2Decoded.model.has_value(), "LR2 parity model decodes");
  if (!luaDecoded.model || !jsonDecoded.model || !lr2Decoded.model) return;

  auto luaModel = overlapProjection(*luaDecoded.model);
  auto jsonModel = overlapProjection(*jsonDecoded.model);
  auto lr2Model = overlapProjection(*lr2Decoded.model);
  const Json luaCanonical = canonicalOverlap(luaModel);
  const Json jsonCanonical = canonicalOverlap(jsonModel);
  const Json lr2Canonical = canonicalOverlap(lr2Model);
  const fs::path expectedPath =
      fs::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/model/all_gameplay_objects.expected.json";
  const Json expected = readJson(expectedPath);
  expectJson(luaCanonical, expected["overlap"],
             "Lua overlap matches the committed canonical contract");
  expectJson(jsonCanonical, expected["overlap"],
             "JSON overlap matches the committed canonical contract");
  expectJson(lr2Canonical, expected["overlap"],
             "LR2 overlap matches the committed canonical contract");
  expect(luaCanonical == jsonCanonical && jsonCanonical == lr2Canonical,
         "Lua, JSON, and LR2 retain equivalent overlap models");

  auto luaValidated = validate(std::move(luaModel));
  auto jsonValidated = validate(std::move(jsonModel));
  auto lr2Validated = validate(std::move(lr2Model));
  if (!luaValidated || !jsonValidated || !lr2Validated) return;
  const Json luaDraw = canonicalDrawTrace(*luaValidated);
  const Json jsonDraw = canonicalDrawTrace(*jsonValidated);
  const Json lr2Draw = canonicalDrawTrace(*lr2Validated);
  expectJson(luaDraw, expected["drawTrace"],
             "Lua draw matches the committed canonical contract");
  expectJson(jsonDraw, expected["drawTrace"],
             "JSON draw matches the committed canonical contract");
  expectJson(lr2Draw, expected["drawTrace"],
             "LR2 draw matches the committed canonical contract");
  expect(luaDraw == jsonDraw && jsonDraw == lr2Draw,
         "Lua, JSON, and LR2 emit equivalent overlap draw commands");

  for (const auto &relative : expected["formatExclusiveEvidence"]) {
    expect(fs::is_regular_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                               relative.get<std::string>()),
           "format-exclusive surface retains separate fixture evidence");
  }
}

} // namespace

int main() {
  testCrossFormatGameplayOverlap();
  if (failures == 0) {
    std::cout << "Beatoraja gameplay cross-format tests passed\n";
    return 0;
  }
  return 1;
}
