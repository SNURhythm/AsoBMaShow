#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/SkinModelValidator.h"
#include "lua_skin_binding_test_support.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

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

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-text-graph-test-" +
               std::to_string(++serial));
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

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

BeatorajaSkinModelDecodeResult decodeInline(std::string_view sourceText) {
  static const std::array writerBuiltins{
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{10}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{30}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{102}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringProperty},
          .selector = SkinBuiltinPropertySelector{202}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{102}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{std::string("303")}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::StringWriter},
          .selector = SkinBuiltinPropertySelector{30}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatWriter},
          .selector = SkinBuiltinPropertySelector{601}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatWriter},
          .selector = SkinBuiltinPropertySelector{703}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{501}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{700}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
          .selector = SkinBuiltinPropertySelector{402}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::IntegerProperty,
                   .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
          .selector = SkinBuiltinPropertySelector{702}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{403}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{601}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{703}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{704}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::FloatProperty,
                   .floatDomain = SkinFloatPropertyDomain::Rate},
          .selector = SkinBuiltinPropertySelector{705}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::TimerProperty},
          .selector = SkinBuiltinPropertySelector{77}},
      SkinBuiltinBindingCatalogEntry{
          .type = {.kind = SkinBindingKind::Event},
          .selector = SkinBuiltinPropertySelector{74}},
  };
  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = *normalizePackageId("TextGraphContract").package;
  const fs::path source = temp.root() / "source";
  writeText(source / "skin/model.luaskin", sourceText);

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "text/graph fixture snapshots");
  if (!snapshot.prepared) {
    return {};
  }

  const auto entry = *normalizeEntryPath(package, "skin/model.luaskin").entry;
  const auto makeFileSystem = [&]() {
    return LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  };
  auto runtimeFiles = makeFileSystem();
  auto reconciliationFiles = makeFileSystem();
  expect(runtimeFiles != nullptr && reconciliationFiles != nullptr,
         "text/graph filesystems create");
  if (!runtimeFiles || !reconciliationFiles) {
    return {};
  }

  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Validation,
       .fileSystem = std::move(runtimeFiles)});
  expect(created.runtime != nullptr, "text/graph runtime creates");
  if (!created.runtime) {
    return {};
  }
  auto headerValue = created.runtime->loadHeader();
  expect(headerValue.value.has_value(), "text/graph header executes");
  if (!headerValue.value) {
    return {};
  }
  LuaSkinTableDecoder decoder;
  const auto header = decoder.decodeHeader(*headerValue.value);
  expect(header.header.has_value(), "text/graph header decodes");
  if (!header.header) {
    return {};
  }
  headerValue.value.reset();

  const auto reconciled = reconcileSkinConfiguration(
      *header.header, nullptr, *reconciliationFiles);
  expect(reconciled.configuration.has_value(),
         "text/graph configuration reconciles");
  if (!reconciled.configuration) {
    return {};
  }
  auto configured =
      created.runtime->loadConfigured(*reconciled.configuration);
  expect(configured.value.has_value(), "text/graph configured phase executes");
  if (!configured.value) {
    return {};
  }
  return decoder.decodeGameplay(
      *configured.value,
      {.runtime = *created.runtime,
       .builtins = SkinBuiltinBindingCatalogView(writerBuiltins)});
}

const SkinObjectDefinition *objectNamed(const BeatorajaSkinModel &model,
                                        std::string_view name) {
  const auto found = std::ranges::find_if(model.objects, [&](const auto &item) {
    return item.authoredName == name;
  });
  return found != model.objects.end() ? &*found : nullptr;
}

SkinObjectDefinition *objectNamed(BeatorajaSkinModel &model,
                                  std::string_view name) {
  const auto found = std::ranges::find_if(model.objects, [&](const auto &item) {
    return item.authoredName == name;
  });
  return found != model.objects.end() ? &*found : nullptr;
}

const int *builtinSelector(
    const std::variant<SkinBuiltinPropertySelector, LuaCallbackId> &source) {
  const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(&source);
  return builtin ? std::get_if<int>(&builtin->value) : nullptr;
}

const std::string *builtinName(
    const std::variant<SkinBuiltinPropertySelector, LuaCallbackId> &source) {
  const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(&source);
  return builtin ? std::get_if<std::string>(&builtin->value) : nullptr;
}

template <typename Binding>
const Binding *bindingById(const std::vector<Binding> &bindings,
                           std::uint32_t id) {
  const auto found = std::ranges::find_if(
      bindings, [=](const auto &binding) { return binding.id.value == id; });
  return found != bindings.end() ? &*found : nullptr;
}

constexpr std::string_view kValidFixture = R"lua(
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='images/atlas.png'}},
  font={{id='main-font',path='fonts/main.fnt',type=2,
         fallback={'fonts/string.fnt',{path='fonts/table.fnt',type=3},
                   {value='fonts/value.ttf',type=4},{}}}},
  image={{id='image',src='atlas',x=0,y=0,w=8,h=8}},
  slider={{id='slider',src='atlas',x=0,y=0,w=8,h=8,
           angle=0,range=10,type=601,changeable=true}},
  text={
    {id='text-explicit',font='main-font',size=36,align=2,
     ref=101,value=202,event=303,constantText='한글 text',editable=false,
     wrapping=true,overflow=1,outlineColor='10203040',outlineWidth=1.5,
     shadowColor='not-a-color',shadowOffsetX=-2.5,shadowOffsetY=3.5,
     shadowSmoothness=0.75},
    {id='text-fallback',font='main-font',size=20,ref=102}
  },
  graph={
    {id='graph-explicit',src='atlas',x=10,y=20,w=20,h=10,divx=2,
     timer=77,cycle=88,type=401,value=501,isRefNum=true,min=-5,max=9,
     angle=1},
    {id='graph-range',src='atlas',x=30,y=40,w=12,h=6,type=402,
     isRefNum=true,min=-50,max=150,angle=99},
    {id='graph-implicit',src='atlas',x=50,y=60,w=14,h=7,type=403,
     angle=-44}
  },
  destination={
    {id='graph-explicit',dst={{}}},{id='text-explicit',dst={{}}},
    {id='image',dst={{}}},{id='graph-range',dst={{}}},
    {id='text-fallback',dst={{}}},{id='slider',dst={{}}},
    {id='graph-implicit',dst={{}}}
  }
}
)lua";

BeatorajaSkinModel decodedValidModel() {
  auto decoded = decodeInline(kValidFixture);
  expect(decoded.model.has_value(), "valid live Text/Graph model decodes");
  return decoded.model ? std::move(*decoded.model) : BeatorajaSkinModel{};
}

void expectOnlyDisabled(const SkinModelValidationResult &validated,
                        SkinObjectId id, std::string_view message) {
  expect(validated.model && !validated.criticalFailure &&
             std::ranges::find(validated.model->disabledOptionalObjects, id) !=
                 validated.model->disabledOptionalObjects.end(),
         message);
}

void testLiveTextAndFontSemantics() {
  const auto model = decodedValidModel();
  expect(model.resources.size() == 2,
         "image and font resources share authored order and identity space");
  if (model.resources.size() == 2) {
    const auto *image = std::get_if<SkinImageResource>(&model.resources[0]);
    const auto *font = std::get_if<SkinFontResource>(&model.resources[1]);
    expect(image && image->id == 1 && image->authoredOrdinal == 0,
           "source becomes the first typed image resource");
    expect(font && font->id == 2 && font->authoredName == "main-font" &&
               font->virtualPath == "fonts/main.fnt" && font->type == 2 &&
               font->authoredOrdinal == 1 && font->fallbacks.size() == 4,
           "font becomes the next typed resource with exact metadata");
    if (font && font->fallbacks.size() == 4) {
      expect(font->fallbacks[0].virtualPath.empty() &&
                 font->fallbacks[0].type == 0 &&
                 font->fallbacks[1].virtualPath == "fonts/table.fnt" &&
                 font->fallbacks[1].type == 3 &&
                 font->fallbacks[2].virtualPath.empty() &&
                 font->fallbacks[2].type == 4 &&
                 font->fallbacks[3].virtualPath.empty(),
             "only fallback.path populates a path while every slot is preserved");
    }
  }

  const auto *explicitDefinition = objectNamed(model, "text-explicit");
  const auto *fallbackDefinition = objectNamed(model, "text-fallback");
  const auto *explicitText = explicitDefinition
                                 ? std::get_if<SkinTextObject>(
                                       &explicitDefinition->payload)
                                 : nullptr;
  const auto *fallbackText = fallbackDefinition
                                 ? std::get_if<SkinTextObject>(
                                       &fallbackDefinition->payload)
                                 : nullptr;
  expect(explicitText && explicitText->font == 2 && explicitText->value &&
             explicitText->writer && explicitText->literal == "한글 text" &&
             explicitText->pointSize == 36 && explicitText->alignment == 2 &&
             explicitText->wrapping && explicitText->overflow == 1 &&
             explicitText->outlineRgba == std::array<std::uint8_t, 4>{
                                                   0x10, 0x20, 0x30, 0x40} &&
             explicitText->outlineWidth == 1.5 &&
             explicitText->shadowRgba ==
                 std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
             explicitText->shadowOffsetX == -2.5 &&
             explicitText->shadowOffsetY == 3.5 &&
             explicitText->shadowSmoothness == 0.75 &&
             !explicitText->editable,
         "live Text preserves pinned style and explicit-writer editability");
  if (explicitText && explicitText->value && explicitText->writer) {
    const auto *value =
        bindingById(model.stringProperties, explicitText->value->value);
    const auto *writer =
        bindingById(model.stringWriters, explicitText->writer->value);
    expect(value && builtinSelector(value->source) &&
               *builtinSelector(value->source) == 202 && writer &&
               builtinName(writer->source) &&
               *builtinName(writer->source) == "303",
           "Text.value wins over ref and Text.event wins for its writer");
  }
  expect(fallbackText && fallbackText->value && fallbackText->writer &&
             fallbackText->editable,
         "Text.ref supplies both value and inferred editable writer fallback");
  if (fallbackText && fallbackText->value && fallbackText->writer) {
    const auto *value =
        bindingById(model.stringProperties, fallbackText->value->value);
    const auto *writer =
        bindingById(model.stringWriters, fallbackText->writer->value);
    expect(value && writer && builtinSelector(value->source) &&
               builtinSelector(writer->source) &&
               *builtinSelector(value->source) == 102 &&
               *builtinSelector(writer->source) == 102,
           "Text.ref fallback retains its integer selector in both registries");
  }
}

void testTextRefWriterFallbackUsesOnlySupportedIntegerSelectors() {
  const auto decoded = decodeInline(R"lua(
return {type=0,w=1280,h=720,
 font={{id='main',path='main.fnt'}},
 text={{id='title',font='main',size=20,ref=10},
       {id='searchword',font='main',size=20,ref=30}},
 destination={{id='title',dst={{}}},{id='searchword',dst={{}}}}}
)lua");
  expect(decoded.model.has_value() && decoded.diagnostics.empty(),
         "passive title and writable searchword Text definitions decode");
  if (!decoded.model) {
    return;
  }
  const auto *titleDefinition = objectNamed(*decoded.model, "title");
  const auto *searchDefinition = objectNamed(*decoded.model, "searchword");
  const auto *title =
      titleDefinition ? std::get_if<SkinTextObject>(&titleDefinition->payload)
                      : nullptr;
  const auto *search =
      searchDefinition ? std::get_if<SkinTextObject>(&searchDefinition->payload)
                       : nullptr;
  expect(title && title->value && !title->writer && !title->editable,
         "title remains passive when its integer ref has no writer factory");
  expect(search && search->value && search->writer && search->editable,
         "searchword attaches the integer writer returned for ref 30");
  if (search && search->writer) {
    const auto *binding =
        bindingById(decoded.model->stringWriters, search->writer->value);
    expect(binding && builtinSelector(binding->source) &&
               *builtinSelector(binding->source) == 30 &&
               !builtinName(binding->source),
           "searchword writer is not interned as decimal string text");
  }
}

void testStaticTextWithZeroRefKeepsItsLiteral() {
  const auto decoded = decodeInline(R"lua(
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 font={{id='main',path='main.fnt'}},
 text={{id='static',font='main',size=20,ref=0,constantText='STATIC'}},
 image={{id='judge-timing',src='atlas',w=1,h=1,act=74}},
 destination={{id='static',dst={{}}},{id='judge-timing',dst={{}}}}}
)lua");
  expect(decoded.model.has_value() && decoded.diagnostics.empty(),
         "static Text with Beatoraja's null ref decodes");
  if (!decoded.model) {
    return;
  }
  const auto *definition = objectNamed(*decoded.model, "static");
  const auto *text = definition
                         ? std::get_if<SkinTextObject>(&definition->payload)
                         : nullptr;
  expect(text && !text->value && text->literal == "STATIC",
         "Text.ref=0 leaves the StringProperty unset so constantText renders");
  expect(decoded.model->stringProperties.empty(),
         "a null upstream StringProperty does not create a typed binding");
  const auto *buttonDefinition = objectNamed(*decoded.model, "judge-timing");
  const auto *button =
      buttonDefinition
          ? std::get_if<SkinImageObject>(&buttonDefinition->payload)
          : nullptr;
  expect(button && button->clickEvent && decoded.model->events.size() == 1 &&
             builtinSelector(decoded.model->events.front().source) &&
             *builtinSelector(decoded.model->events.front().source) == 74,
         "EventFactory's numeric judge-timing action stays a built-in event");
}

void testSliderBindingPrecedenceIgnoresInactiveEvents() {
  const auto decoded = decodeInline(R"lua(
local explicit_writer = function(value) return value end
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 slider={
   {id='explicit',src='atlas',w=8,h=8,type=701,value=700,
    event=explicit_writer,isRefNum=true,changeable=false},
   {id='integer-range',src='atlas',w=8,h=8,type=702,isRefNum=true,
    min=-5,max=5,event={}},
   {id='implicit-writable',src='atlas',w=8,h=8,type=703,
    changeable=true,event={}},
   {id='implicit-passive',src='atlas',w=8,h=8,type=704,
    changeable=false,event={}},
   {id='implicit-unsupported-writer',src='atlas',w=8,h=8,type=705,
    changeable=true,event={}}
 },
 destination={{id='explicit',dst={{}}},{id='integer-range',dst={{}}},
              {id='implicit-writable',dst={{}}},
              {id='implicit-passive',dst={{}}},
              {id='implicit-unsupported-writer',dst={{}}}}}
)lua");
  expect(decoded.model.has_value() && decoded.diagnostics.empty(),
         "ignored Slider.event fields consume no binding dispatch");
  if (!decoded.model) {
    return;
  }
  const auto sliderNamed = [&](std::string_view name) {
    const auto *definition = objectNamed(*decoded.model, name);
    return definition ? std::get_if<SkinSliderObject>(&definition->payload)
                      : nullptr;
  };
  const auto *explicitSlider = sliderNamed("explicit");
  const auto *integerRange = sliderNamed("integer-range");
  const auto *implicitWritable = sliderNamed("implicit-writable");
  const auto *implicitPassive = sliderNamed("implicit-passive");
  const auto *unsupportedWriter = sliderNamed("implicit-unsupported-writer");
  expect(explicitSlider && explicitSlider->writer &&
             std::holds_alternative<SkinFloatPropertyId>(explicitSlider->value),
         "explicit Slider.value selects its optional authored event branch");
  expect(integerRange && !integerRange->writer &&
             std::holds_alternative<SkinSliderObject::IntegerRangeSource>(
                 integerRange->value),
         "isRefNum Slider ignores event and has no writer");
  expect(
      implicitWritable && implicitWritable->writer &&
          std::holds_alternative<SkinFloatPropertyId>(implicitWritable->value),
      "changeable implicit Slider synthesizes a supported numeric writer");
  expect(implicitPassive && !implicitPassive->writer && unsupportedWriter &&
             !unsupportedWriter->writer,
         "passive or unsupported implicit Slider has no writer");
  if (implicitWritable && implicitWritable->writer) {
    const auto *binding = bindingById(decoded.model->floatWriters,
                                      implicitWritable->writer->value);
    expect(binding && builtinSelector(binding->source) &&
               *builtinSelector(binding->source) == 703,
           "implicit Slider writer uses its numeric type selector");
  }
}

void testLiveGraphPrecedenceAndAuthoredOrder() {
  const auto model = decodedValidModel();
  constexpr std::array<std::string_view, 7> expectedOrder{
      "graph-explicit", "text-explicit", "image", "graph-range",
      "text-fallback", "slider", "graph-implicit"};
  expect(model.objects.size() == expectedOrder.size() &&
             model.destinations.size() == expectedOrder.size(),
         "every authored destination materializes once");
  for (std::size_t i = 0; i < std::min(model.objects.size(),
                                      expectedOrder.size()); ++i) {
    expect(model.objects[i].authoredName == expectedOrder[i] &&
               model.objects[i].authoredOrdinal == i &&
               model.destinations[i].object == model.objects[i].id &&
               !model.objects[i].critical,
           "Text/Graph objects preserve destination order and optionality");
  }

  const auto graphNamed = [&](std::string_view name) -> const SkinGraphObject * {
    const auto *definition = objectNamed(model, name);
    return definition ? std::get_if<SkinGraphObject>(&definition->payload)
                      : nullptr;
  };
  const auto *explicitGraph = graphNamed("graph-explicit");
  const auto *rangeGraph = graphNamed("graph-range");
  const auto *implicitGraph = graphNamed("graph-implicit");
  const auto *explicitRate =
      explicitGraph ? std::get_if<SkinFloatPropertyId>(&explicitGraph->value)
                    : nullptr;
  expect(explicitGraph && explicitRate && explicitGraph->direction == 1 &&
             explicitGraph->fill.resource == 1 &&
             explicitGraph->fill.frames.size() == 2 &&
             explicitGraph->fill.frames[0].x == 10 &&
             explicitGraph->fill.frames[1].x == 20 &&
             explicitGraph->fill.cycleMillis == 88 &&
             explicitGraph->fill.timer,
         "Graph retains fill frames, timing, resource, and down direction");
  if (explicitRate) {
    const auto *binding = bindingById(model.floatProperties,
                                      explicitRate->value);
    expect(binding &&
               binding->domain == SkinFloatPropertyDomain::Rate &&
               builtinSelector(binding->source) &&
               *builtinSelector(binding->source) == 501,
           "explicit Graph.value wins over type and isRefNum");
  }
  const auto *range = rangeGraph
                          ? std::get_if<SkinSliderObject::IntegerRangeSource>(
                                &rangeGraph->value)
                          : nullptr;
  if (range) {
    const auto *binding =
        bindingById(model.integerProperties, range->value.value);
    expect(rangeGraph->direction == 0 && range->minimum == -50 &&
               range->maximum == 150 && binding &&
               binding->domain == SkinIntegerPropertyDomain::IntegerValue &&
               builtinSelector(binding->source) &&
               *builtinSelector(binding->source) == 402,
           "isRefNum Graph uses type as integer source and preserves range");
  } else {
    expect(false, "isRefNum Graph materializes an integer range source");
  }
  const auto *implicitRate =
      implicitGraph ? std::get_if<SkinFloatPropertyId>(&implicitGraph->value)
                    : nullptr;
  if (implicitRate) {
    const auto *binding =
        bindingById(model.floatProperties, implicitRate->value);
    expect(implicitGraph->direction == 0 && binding &&
               binding->domain == SkinFloatPropertyDomain::Rate &&
               builtinSelector(binding->source) &&
               *builtinSelector(binding->source) == 403,
           "implicit Graph uses type as rate and non-one direction is right");
  } else {
    expect(false, "implicit Graph materializes a rate source");
  }
}

void testDistributionGraphIsDiagnosedAndDisabled() {
  const auto decoded = decodeInline(R"lua(
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 graph={{id='distribution',src='atlas',w=10,h=10,type=-1}},
 destination={{id='distribution',dst={{}}}}}
)lua");
  expect(decoded.model.has_value() &&
             std::ranges::any_of(decoded.diagnostics, [](const auto &item) {
               return item.code ==
                      "skin_lua_model_distribution_graph_unsupported";
             }),
         "negative Graph type produces the dedicated compatibility diagnostic");
  if (!decoded.model) {
    return;
  }
  const auto *definition = objectNamed(*decoded.model, "distribution");
  expect(definition && !definition->critical,
         "unsupported distribution Graph remains optional");
  const auto validated =
      test_support::validateWithAuthoredBuiltins(*decoded.model);
  expect(definition && validated.model && !validated.criticalFailure &&
             std::ranges::find(validated.model->disabledOptionalObjects,
                               definition->id) !=
                 validated.model->disabledOptionalObjects.end(),
         "validator disables the unsupported distribution Graph placeholder");
}

void testValidatorEnforcesTypedResourcesBindingsAndDestinations() {
  auto base = decodedValidModel();
  const auto valid = test_support::validateWithAuthoredBuiltins(base);
  const auto *explicitText = objectNamed(base, "text-explicit");
  const auto *fallbackText = objectNamed(base, "text-fallback");
  expect(
      valid.model && !valid.criticalFailure && explicitText && fallbackText &&
          valid.model->disabledOptionalObjects ==
              std::vector<SkinObjectId>{explicitText->id, fallbackText->id} &&
          std::ranges::count_if(
              valid.diagnostics,
              [](const auto &item) {
                return item.code ==
                       "skin_lua_model_text_interaction_unsupported";
              }) == 2,
      "Text event/editable surfaces are diagnosed and disabled");

  auto criticalInteraction = base;
  auto *criticalText = objectNamed(criticalInteraction, "text-explicit");
  criticalText->critical = true;
  const auto rejectedInteraction = test_support::validateWithAuthoredBuiltins(
      std::move(criticalInteraction));
  expect(!rejectedInteraction.model && rejectedInteraction.criticalFailure &&
             std::ranges::any_of(
                 rejectedInteraction.diagnostics,
                 [](const auto &item) {
                   return item.code ==
                          "skin_lua_model_text_interaction_unsupported";
                 }),
         "critical Text interaction surface is diagnosed and rejected");

  const auto invalidObject = [&](std::string_view name, auto mutate,
                                 std::string_view message) {
    auto model = base;
    auto *object = objectNamed(model, name);
    expect(object != nullptr, "validator mutation target exists");
    if (!object) {
      return;
    }
    const auto id = object->id;
    mutate(model, *object);
    expectOnlyDisabled(
        test_support::validateWithAuthoredBuiltins(std::move(model)), id,
        message);
  };

  invalidObject(
      "text-explicit",
      [](auto &, auto &definition) {
        std::get<SkinTextObject>(definition.payload).font = 1;
      },
      "Text cannot consume an image resource as a font");
  invalidObject(
      "graph-explicit",
      [](auto &, auto &definition) {
        std::get<SkinGraphObject>(definition.payload).fill.resource = 2;
      },
      "Graph fill cannot consume a font resource as an image");
  invalidObject(
      "text-explicit",
      [](auto &, auto &definition) {
        std::get<SkinTextObject>(definition.payload).writer =
            SkinStringWriterId{999};
      },
      "Text requires an existing typed string writer");
  invalidObject(
      "text-explicit",
      [](auto &, auto &definition) {
        std::get<SkinTextObject>(definition.payload).value =
            SkinStringPropertyId{999};
      },
      "Text requires an existing typed string value");
  invalidObject(
      "graph-explicit",
      [](auto &, auto &definition) {
        std::get<SkinGraphObject>(definition.payload).fill.timer =
            SkinTimerPropertyId{999};
      },
      "Graph fill requires an existing timer");
  invalidObject(
      "graph-explicit",
      [](auto &model, auto &definition) {
        const auto value =
            std::get<SkinFloatPropertyId>(
                std::get<SkinGraphObject>(definition.payload).value);
        const auto found = std::ranges::find_if(
            model.floatProperties,
            [&](const auto &binding) { return binding.id == value; });
        found->domain = SkinFloatPropertyDomain::FloatValue;
      },
      "Graph rate rejects a float binding from the FloatValue domain");
  invalidObject(
      "image",
      [](auto &, auto &definition) {
        std::get<SkinImageObject>(definition.payload).clickEvent =
            SkinEventBindingId{999};
      },
      "Image click action requires an existing event binding");
  invalidObject(
      "slider",
      [](auto &, auto &definition) {
        std::get<SkinSliderObject>(definition.payload).writer =
            SkinFloatWriterId{999};
      },
      "Slider writer requires an existing float writer binding");

  auto destinationModel = base;
  const auto *graph = objectNamed(destinationModel, "graph-explicit");
  const auto destination = std::ranges::find_if(
      destinationModel.destinations,
      [&](const auto &item) { return graph && item.object == graph->id; });
  expect(graph && destination != destinationModel.destinations.end(),
         "validator destination mutation target exists");
  if (!graph || destination == destinationModel.destinations.end()) {
    return;
  }
  destination->presentation.timer = SkinTimerPropertyId{999};
  expectOnlyDisabled(
      test_support::validateWithAuthoredBuiltins(destinationModel), graph->id,
      "invalid optional destination timer disables its object");

  auto criticalDestination = destinationModel;
  auto *criticalGraph = objectNamed(criticalDestination, "graph-explicit");
  criticalGraph->critical = true;
  const auto critical = test_support::validateWithAuthoredBuiltins(
      std::move(criticalDestination));
  expect(!critical.model && critical.criticalFailure,
         "the same invalid destination fails a critical object");
}

} // namespace

int main() {
  testLiveTextAndFontSemantics();
  testTextRefWriterFallbackUsesOnlySupportedIntegerSelectors();
  testStaticTextWithZeroRefKeepsItsLiteral();
  testSliderBindingPrecedenceIgnoresInactiveEvents();
  testLiveGraphPrecedenceAndAuthoredOrder();
  testDistributionGraphIsDiagnosedAndDisabled();
  testValidatorEnforcesTypedResourcesBindingsAndDestinations();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin Text/Graph live integration tests passed\n";
  return 0;
}
