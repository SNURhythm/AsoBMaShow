#include "skin/beatoraja/LuaSkinBindingDecoder.h"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
              ("asobmashow-lua-binding-live-" + std::to_string(++serial));
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

class LiveSession {
public:
  explicit LiveSession(std::string_view sourceText)
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("BindingLiveContract").package),
        entry_(*normalizeEntryPath(package_, "skin/model.luaskin").entry) {
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/model.luaskin", sourceText);
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "live binding package snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));

    auto fileSystem =
        LuaSkinFileSystem::create({.revision = prepared_->readView(),
                                   .entry = entry_,
                                   .storageRoots = roots_,
                                   .profileId = *makeSkinProfileId(
                                       "22222222-2222-4222-8222-222222222222"),
                                   .allowDataWrites = true});
    expect(fileSystem.fileSystem != nullptr, "live binding filesystem creates");
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(created.runtime != nullptr, "live binding runtime creates");
    runtime_ = std::move(created.runtime);
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "live binding header executes");
    if (!header.value) {
      return;
    }
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "live binding configured phase runs");
    configured_ = std::move(configured.value);
  }

  BeatorajaSkinModelDecodeResult
  decode(SkinBuiltinBindingCatalogView builtins = {}) {
    if (!runtime_ || !configured_) {
      return {};
    }
    return LuaSkinTableDecoder{}.decodeGameplay(
        *configured_, {.runtime = *runtime_, .builtins = builtins});
  }

  LuaSkinRuntime *runtime() const noexcept { return runtime_.get(); }
  void releaseConfigured() { configured_.reset(); }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaValueHandle> configured_;
};

const SkinObjectDefinition *objectNamed(const BeatorajaSkinModel &model,
                                        std::string_view name) {
  const auto found = std::ranges::find_if(model.objects, [&](const auto &item) {
    return item.authoredName == name;
  });
  return found != model.objects.end() ? &*found : nullptr;
}

template <typename Binding>
const Binding *bindingById(const std::vector<Binding> &bindings,
                           std::uint32_t id) {
  const auto found = std::ranges::find_if(
      bindings, [=](const auto &binding) { return binding.id.value == id; });
  return found != bindings.end() ? &*found : nullptr;
}

template <typename Binding>
const LuaCallbackId *callbackSource(const Binding &binding) {
  return std::get_if<LuaCallbackId>(&binding.source);
}

const SkinBuiltinPropertySelector *builtinSource(
    const std::variant<SkinBuiltinPropertySelector, LuaCallbackId> &source) {
  return std::get_if<SkinBuiltinPropertySelector>(&source);
}

constexpr std::string_view kAllKinds = R"lua(
local event_function = function(state) return state + 13 end
local float_function = function() return 0.25 end
local string_writer = function(value) return value end
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},
  font={{id='main',path='main.fnt'}},
  image={
    {id='state-image',src='atlas',w=20,h=10,divx=2,len=2,
     timer='function() return 9001 end',ref='42',act=event_function,click=2},
    {id='timed-node',src='atlas',w=10,h=10,timer=7}
  },
  imageset={{id='image-set',ref=41,value='42',images={'timed-node'},
             act='local state = ...; return state + 13',click=3}},
  value={{id='number',src='atlas',w=100,h=10,divx=10,digit=4,
          timer=7,ref=101,value='40 + 2'}},
  floatvalue={{id='float',src='atlas',w=120,h=10,divx=12,iketa=2,
               fketa=2,ref=202,value=float_function}},
  slider={{id='slider',src='atlas',w=10,h=10,angle=1,range=100,type=301,
           value='known_rate',event='return ...',timer=7}},
  text={
    {id='text',font='main',size=20,ref=500,value="'live'",
     event=string_writer},
    {id='text-fallback',font='main',size=20,ref=501}
  },
  graph={{id='graph',src='atlas',w=10,h=10,type=302,
          value='known_rate',timer=7,angle=1}},
  note={
    id='note',
    note={'state-image'},mine={'state-image'},
    lnend={'state-image'},lnstart={'state-image'},
    lnbodyActive={'state-image'},lnbody={'state-image'},
    hcnend={'state-image'},hcnstart={'state-image'},
    hcnbodyActive={'state-image'},hcnbody={'state-image'},
    hcnbodyMiss={'state-image'},hcnbodyReactive={'state-image'},
    dst={{x=0,y=0,w=20,h=10}}
  },
  destination={
    {id='state-image',timer=7,op={9,'known_bool',function() return true end},
     draw='known_bool',offsets={2,3},offset=4,dst={{}}},
    {id='image-set',dst={{}}},{id='number',dst={{}}},
    {id='float',dst={{}}},{id='slider',dst={{}}},
    {id='text',dst={{}}},{id='text-fallback',dst={{}}},
    {id='graph',dst={{}}},{id='note',dst={{}}}
  }
}
)lua";

constexpr auto kBuiltins = std::to_array<SkinBuiltinBindingCatalogEntry>({
    {.type = {.kind = SkinBindingKind::BooleanProperty},
     .selector = SkinBuiltinPropertySelector{std::string("known_bool")}},
    {.type = {.kind = SkinBindingKind::IntegerProperty,
              .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
     .selector = SkinBuiltinPropertySelector{42}},
    {.type = {.kind = SkinBindingKind::FloatProperty,
              .floatDomain = SkinFloatPropertyDomain::Rate},
     .selector = SkinBuiltinPropertySelector{std::string("known_rate")}},
    {.type = {.kind = SkinBindingKind::StringProperty},
     .selector = SkinBuiltinPropertySelector{501}},
    {.type = {.kind = SkinBindingKind::StringWriter},
     .selector = SkinBuiltinPropertySelector{std::string("501")}},
    {.type = {.kind = SkinBindingKind::TimerProperty},
     .selector = SkinBuiltinPropertySelector{7}},
});

void testAllKindsDecodeWithPinnedDispatchAndLiveCallbacks() {
  LiveSession session(kAllKinds);
  const SkinBuiltinBindingCatalogView builtins(kBuiltins);
  auto decoded = session.decode(builtins);
  expect(decoded.model.has_value() && decoded.diagnostics.empty(),
         "live binding fixture decodes without diagnostics");
  if (!decoded.model || !session.runtime()) {
    return;
  }
  auto &model = *decoded.model;
  expect(!model.booleanProperties.empty() && !model.integerProperties.empty() &&
             !model.floatProperties.empty() &&
             !model.stringProperties.empty() &&
             !model.timerProperties.empty() && !model.floatWriters.empty() &&
             !model.stringWriters.empty() && !model.events.empty(),
         "live model retains all eight typed binding registries");

  const auto *stateDefinition = objectNamed(model, "state-image");
  const auto *stateImage =
      stateDefinition ? std::get_if<SkinImageObject>(&stateDefinition->payload)
                      : nullptr;
  expect(stateImage && stateImage->stateIndex && stateImage->clickEvent &&
             stateImage->clickMode == 2 &&
             stateImage->orderedStates.size() == 2 &&
             stateImage->orderedStates.front().timer,
         "Image timer/ref/act/click materialize through typed bindings");
  if (stateImage && stateImage->stateIndex) {
    const auto *binding =
        bindingById(model.integerProperties, stateImage->stateIndex->value);
    const auto *builtin = binding ? builtinSource(binding->source) : nullptr;
    expect(binding &&
               binding->domain == SkinIntegerPropertyDomain::ImageIndex &&
               builtin && std::get<int>(builtin->value) == 42,
           "numeric-string Image.ref takes numeric factory precedence");
  }

  const auto *sliderDefinition = objectNamed(model, "slider");
  const auto *graphDefinition = objectNamed(model, "graph");
  const auto *slider =
      sliderDefinition
          ? std::get_if<SkinSliderObject>(&sliderDefinition->payload)
          : nullptr;
  const auto *graph =
      graphDefinition ? std::get_if<SkinGraphObject>(&graphDefinition->payload)
                      : nullptr;
  const auto *sliderRate =
      slider ? std::get_if<SkinFloatPropertyId>(&slider->value) : nullptr;
  const auto *graphRate =
      graph ? std::get_if<SkinFloatPropertyId>(&graph->value) : nullptr;
  expect(slider && sliderRate && slider->writer && graphRate &&
             *sliderRate == *graphRate,
         "named rate binding deduplicates across Slider and Graph");
  if (sliderRate) {
    const auto *binding = bindingById(model.floatProperties, sliderRate->value);
    expect(binding && binding->authoredOrdinal == 1,
           "deduplicated binding keeps its first per-kind authored ordinal");
  }

  const auto destination =
      std::ranges::find_if(model.destinations, [&](const auto &item) {
        return stateDefinition && item.object == stateDefinition->id;
      });
  expect(destination != model.destinations.end() &&
             destination->presentation.timer &&
             destination->presentation.conditions.size() == 3 &&
             std::get<int>(destination->presentation.conditions[0]) == 9 &&
             destination->presentation.drawCondition &&
             destination->presentation.offsetIds == std::vector<int>({2, 3, 4}),
         "Destination timer/options/draw/offsets preserve authored forms");

  const auto *noteDefinition = objectNamed(model, "note");
  const auto *note = noteDefinition
                         ? std::get_if<SkinNoteObject>(&noteDefinition->payload)
                         : nullptr;
  const SkinSpriteFrames *noteSprite = nullptr;
  if (note && !note->lanes.empty()) {
    const auto normal =
        note->lanes.front().visuals.find(SkinNoteVisualKind::Normal);
    if (normal != note->lanes.front().visuals.end()) {
      noteSprite = std::get_if<SkinSpriteFrames>(&normal->second);
    }
  }
  expect(noteDefinition && noteDefinition->critical && noteSprite &&
             noteSprite->timer,
         "Note sprite keeps its referenced image Timer binding");

  const auto valid = SkinModelValidator{}.validate(
      model, {.builtins = builtins,
              .callbacks = session.runtime()->callbackLiveness()});
  expect(valid.model && !valid.criticalFailure &&
             valid.model->disabledOptionalObjects.empty(),
         "live binding catalog and callback generation validate end to end");

  const auto timerCallback =
      std::ranges::find_if(model.timerProperties, [](const auto &binding) {
        return callbackSource(binding) != nullptr;
      });
  const auto eventCallback =
      std::ranges::find_if(model.events, [](const auto &binding) {
        return callbackSource(binding) != nullptr;
      });
  expect(timerCallback != model.timerProperties.end() &&
             eventCallback != model.events.end(),
         "Timer trial and Event function retain live callbacks");
  if (timerCallback == model.timerProperties.end() ||
      eventCallback == model.events.end()) {
    return;
  }
  const LuaCallbackId timerId = *callbackSource(*timerCallback);
  const LuaCallbackId eventId = *callbackSource(*eventCallback);
  session.releaseConfigured();
  expect(session.runtime()->enterRenderPhase().ok,
         "live binding session enters render");
  expect(session.runtime()->beginFrame(1).ok,
         "Timer callback receives a frame budget");
  const auto timerValue = session.runtime()->invoke(timerId, {});
  expect(timerValue.value && std::get<std::int64_t>(*timerValue.value) == 9001,
         "Timer script trial unwraps the returned function");
  expect(session.runtime()->beginFrame(2).ok,
         "Event callback receives a fresh frame budget");
  const std::array<LuaScalar, 1> argument{std::int64_t{8}};
  const auto eventValue = session.runtime()->invoke(eventId, argument);
  expect(eventValue.value && std::get<std::int64_t>(*eventValue.value) == 21,
         "Event callback receives the pinned one state argument");
}

void testValidationRejectsDeadCallbacksAndWrongBuiltinDomain() {
  LiveSession session(kAllKinds);
  const SkinBuiltinBindingCatalogView builtins(kBuiltins);
  auto decoded = session.decode(builtins);
  if (!decoded.model || !session.runtime()) {
    expect(false, "validator fixture decodes");
    return;
  }

  auto optionalDead = *decoded.model;
  optionalDead.objects.erase(
      std::remove_if(optionalDead.objects.begin(), optionalDead.objects.end(),
                     [](const auto &object) { return object.critical; }),
      optionalDead.objects.end());
  optionalDead.destinations.erase(
      std::remove_if(optionalDead.destinations.begin(),
                     optionalDead.destinations.end(),
                     [&](const auto &item) {
                       return std::ranges::none_of(
                           optionalDead.objects, [&](const auto &object) {
                             return object.id == item.object;
                           });
                     }),
      optionalDead.destinations.end());
  const auto dead = SkinModelValidator{}.validate(
      std::move(optionalDead), {.builtins = builtins, .callbacks = {}});
  expect(dead.model && !dead.criticalFailure &&
             !dead.model->disabledOptionalObjects.empty(),
         "dead callback generation disables optional dependents");

  const auto criticalDead = SkinModelValidator{}.validate(
      *decoded.model, {.builtins = builtins, .callbacks = {}});
  expect(!criticalDead.model && criticalDead.criticalFailure,
         "dead callback generation fails a critical Note dependency");

  auto invalidCallback = *decoded.model;
  const auto *textDefinition = objectNamed(invalidCallback, "text");
  const auto *text = textDefinition
                         ? std::get_if<SkinTextObject>(&textDefinition->payload)
                         : nullptr;
  auto invalidString =
      text && text->value
          ? std::ranges::find_if(
                invalidCallback.stringProperties,
                [&](const auto &binding) { return binding.id == *text->value; })
          : invalidCallback.stringProperties.end();
  expect(textDefinition && text &&
             invalidString != invalidCallback.stringProperties.end(),
         "invalid callback mutation finds the Text string binding");
  if (textDefinition &&
      invalidString != invalidCallback.stringProperties.end()) {
    const SkinObjectId textObjectId = textDefinition->id;
    invalidString->source = LuaCallbackId{};
    const auto invalid = SkinModelValidator{}.validate(
        std::move(invalidCallback),
        {.builtins = builtins,
         .callbacks = session.runtime()->callbackLiveness()});
    expect(invalid.model && !invalid.criticalFailure &&
               std::ranges::find(invalid.model->disabledOptionalObjects,
                                 textObjectId) !=
                   invalid.model->disabledOptionalObjects.end(),
           "zero callback ID disables its optional consumer even in a live "
           "generation");
  }

  auto wrongEntries = kBuiltins;
  wrongEntries[2].type.floatDomain = SkinFloatPropertyDomain::FloatValue;
  const SkinBuiltinBindingCatalogView wrongBuiltins(wrongEntries);
  const auto wrong = SkinModelValidator{}.validate(
      *decoded.model, {.builtins = wrongBuiltins,
                       .callbacks = session.runtime()->callbackLiveness()});
  const auto *slider = objectNamed(*decoded.model, "slider");
  expect(
      wrong.model && slider &&
          std::ranges::find(wrong.model->disabledOptionalObjects, slider->id) !=
              wrong.model->disabledOptionalObjects.end(),
      "wrong built-in Float domain disables optional consumers");

  auto criticalWrong = *decoded.model;
  auto criticalSlider =
      std::ranges::find_if(criticalWrong.objects, [](const auto &object) {
        return object.authoredName == "slider";
      });
  criticalSlider->critical = true;
  const auto failed = SkinModelValidator{}.validate(
      std::move(criticalWrong),
      {.builtins = wrongBuiltins,
       .callbacks = session.runtime()->callbackLiveness()});
  expect(!failed.model && failed.criticalFailure,
         "wrong built-in Float domain fails a critical consumer");
}

void testExactIndexedBindingFailurePath() {
  LiveSession session(R"lua(
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 image={{id='first',src='atlas',w=1,h=1},
        {id='second',src='atlas',w=1,h=1}},
 destination={{id='first',dst={{}}},
              {id='second',op={1,2,{}},dst={{}}}}}
)lua");
  const auto decoded = session.decode();
  expect(!decoded.model && decoded.diagnostics.size() == 1 &&
             decoded.diagnostics.front().code ==
                 "skin_lua_binding_type_invalid" &&
             decoded.diagnostics.front().virtualPath == "destination[2].op[3]",
         "invalid binding reports its exact indexed source path");
}

void testInvalidExplicitRefSourceFailsClosed() {
  LiveSession session(R"lua(
return {type=0,w=1280,h=720,
 font={{id='main',path='main.fnt'}},
 text={{id='text',font='main',size=20,ref=501,event={}}},
 destination={{id='text',dst={{}}}}}
)lua");
  const auto decoded = session.decode();
  expect(
      !decoded.model && decoded.diagnostics.size() == 1 &&
          decoded.diagnostics.front().code == "skin_lua_binding_type_invalid" &&
          decoded.diagnostics.front().virtualPath == "text[1].event",
      "invalid explicit ref-backed source fails closed at its authored path");
}

void testLiveAggregateBindingSourceBudget() {
  LiveSession session(R"lua(
local huge = string.rep(' ', 65513) .. 'function() return 1 end'
local images = {}
for i = 1, 129 do
  images[i] = {id='image-' .. i,src='atlas',w=1,h=1,timer=huge}
end
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},image=images}
)lua");
  const auto decoded = session.decode();
  const bool rejected =
      !decoded.model && decoded.diagnostics.size() == 1 &&
      decoded.diagnostics.front().code ==
          "skin_lua_binding_work_limit_exceeded" &&
      decoded.diagnostics.front().virtualPath == "image[129].timer";
  if (!rejected) {
    for (const auto &item : decoded.diagnostics) {
      std::cerr << "budget diagnostic: " << item.code << " @ "
                << item.virtualPath << '\n';
    }
  }
  expect(rejected,
         "live decode preserves the aggregate 8 MiB binding work budget");
}

} // namespace

int main() {
  testAllKindsDecodeWithPinnedDispatchAndLiveCallbacks();
  testValidationRejectsDeadCallbacksAndWrongBuiltinDomain();
  testExactIndexedBindingFailurePath();
  testInvalidExplicitRefSourceFailsClosed();
  testLiveAggregateBindingSourceBudget();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin binding live integration tests passed\n";
  return 0;
}
