#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/SkinModelValidator.h"
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
  return decoder.decodeGameplay(*configured.value);
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
               builtinSelector(writer->source) &&
               *builtinSelector(writer->source) == 303,
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
           "Text.ref fallback retains one selector in each typed registry");
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
  const auto validated = SkinModelValidator{}.validate(*decoded.model);
  expect(definition && validated.model && !validated.criticalFailure &&
             std::ranges::find(validated.model->disabledOptionalObjects,
                               definition->id) !=
                 validated.model->disabledOptionalObjects.end(),
         "validator disables the unsupported distribution Graph placeholder");
}

void testValidatorEnforcesTypedResourcesBindingsAndDestinations() {
  auto base = decodedValidModel();
  const auto valid = SkinModelValidator{}.validate(base);
  expect(valid.model && !valid.criticalFailure &&
             valid.model->disabledOptionalObjects.empty(),
         "decoded Text/Graph fixture passes validation before mutation");

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
    expectOnlyDisabled(SkinModelValidator{}.validate(std::move(model)),
                       id, message);
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
  expectOnlyDisabled(SkinModelValidator{}.validate(destinationModel),
                     graph->id,
                     "invalid optional destination timer disables its object");

  auto criticalDestination = destinationModel;
  auto *criticalGraph = objectNamed(criticalDestination, "graph-explicit");
  criticalGraph->critical = true;
  const auto critical =
      SkinModelValidator{}.validate(std::move(criticalDestination));
  expect(!critical.model && critical.criticalFailure,
         "the same invalid destination fails a critical object");
}

} // namespace

int main() {
  testLiveTextAndFontSemantics();
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
