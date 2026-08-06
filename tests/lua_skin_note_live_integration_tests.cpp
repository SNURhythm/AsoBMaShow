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
#include <sstream>
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

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-note-live-" + std::to_string(++serial));
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
        package_(*normalizePackageId("NoteLiveContract").package),
        entry_(*normalizeEntryPath(package_, "skin/model.luaskin").entry) {
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/model.luaskin", sourceText);
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "live Note package snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));

    auto fileSystem =
        LuaSkinFileSystem::create({.revision = prepared_->readView(),
                                   .entry = entry_,
                                   .storageRoots = roots_,
                                   .profileId = *makeSkinProfileId(
                                       "88888888-8888-4888-8888-888888888888"),
                                   .allowDataWrites = true});
    expect(fileSystem.fileSystem != nullptr, "live Note filesystem creates");
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(created.runtime != nullptr, "live Note runtime creates");
    runtime_ = std::move(created.runtime);
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "live Note header executes");
    if (!header.value) {
      return;
    }
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "live Note configured phase runs");
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

const SkinObjectDefinition *noteDefinition(const BeatorajaSkinModel &model) {
  const auto found = std::ranges::find_if(model.objects, [](const auto &item) {
    return std::holds_alternative<SkinNoteObject>(item.payload);
  });
  return found != model.objects.end() ? &*found : nullptr;
}

SkinObjectDefinition *noteDefinition(BeatorajaSkinModel &model) {
  const auto found = std::ranges::find_if(model.objects, [](const auto &item) {
    return std::holds_alternative<SkinNoteObject>(item.payload);
  });
  return found != model.objects.end() ? &*found : nullptr;
}

const SkinSpriteFrames *visualSprite(const SkinLaneNotePresentation &lane,
                                     SkinNoteVisualKind kind) {
  const auto found = lane.visuals.find(kind);
  return found != lane.visuals.end()
             ? std::get_if<SkinSpriteFrames>(&found->second)
             : nullptr;
}

int spriteX(const SkinLaneNotePresentation &lane, SkinNoteVisualKind kind) {
  const auto *sprite = visualSprite(lane, kind);
  return sprite && !sprite->frames.empty() ? sprite->frames.front().x : -1;
}

std::size_t diagnosticCount(const BeatorajaSkinModelDecodeResult &result,
                            std::string_view code) {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.diagnostics, [&](const auto &diagnostic) {
        return diagnostic.code == code;
      }));
}

constexpr auto kBuiltins = std::to_array<SkinBuiltinBindingCatalogEntry>({
    {.type = {.kind = SkinBindingKind::BooleanProperty},
     .selector = SkinBuiltinPropertySelector{std::string("known_bool")}},
    {.type = {.kind = SkinBindingKind::TimerProperty},
     .selector = SkinBuiltinPropertySelector{7}},
});

void testScuroShapedModernNoteAndNestedLinesDecodeLive() {
  const fs::path fixture =
      fs::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/lua/model/note_live_contract.luaskin";
  LiveSession session(readText(fixture));
  const SkinBuiltinBindingCatalogView builtins(kBuiltins);
  auto decoded = session.decode(builtins);
  expect(decoded.model.has_value(),
         "SCURO-shaped modern Note fixture decodes live");
  expect(diagnosticCount(decoded,
                         "skin_lua_model_authored_note_visual_ignored") == 1,
         "authored hidden and processed fields emit one diagnostic");
  if (!decoded.model || !session.runtime()) {
    return;
  }

  const auto *definition = noteDefinition(*decoded.model);
  const auto *note =
      definition ? std::get_if<SkinNoteObject>(&definition->payload) : nullptr;
  expect(definition && definition->critical && note && note->lanes.size() == 2,
         "live Note is critical and preserves two authored lanes");
  if (!note || note->lanes.size() != 2) {
    return;
  }

  const auto &first = note->lanes[0];
  const auto &second = note->lanes[1];
  expect(spriteX(first, SkinNoteVisualKind::LnBodyActive) == 90 &&
             spriteX(first, SkinNoteVisualKind::LnBodyInactive) == 110 &&
             spriteX(first, SkinNoteVisualKind::HcnBodyActive) == 180 &&
             spriteX(first, SkinNoteVisualKind::HcnBodyInactive) == 200 &&
             spriteX(first, SkinNoteVisualKind::HcnDamage) == 250 &&
             spriteX(first, SkinNoteVisualKind::HcnReactive) == 230,
         "nonempty modern LN and HCN fields independently select exact roles");
  expect(
      first.laneDestination.x == 100 && first.laneDestination.y == 200 &&
          first.authoredNoteHeight == 12 && !first.secondaryDestinationY &&
          second.laneDestination.x == 140 && !second.authoredNoteHeight &&
          !second.secondaryDestinationY &&
          note->expansionRatePercent == std::array<int, 2>{125, 75},
      "lane dst, explicit/deferred size, dst2 sentinel, and expansion survive");
  const auto hidden = first.visuals.find(SkinNoteVisualKind::Hidden);
  const auto processed = first.visuals.find(SkinNoteVisualKind::Processed);
  expect(
      hidden != first.visuals.end() && processed != first.visuals.end() &&
          std::holds_alternative<SkinSynthesizedNoteVisual>(hidden->second) &&
          std::holds_alternative<SkinSynthesizedNoteVisual>(processed->second),
      "hidden and processed visuals are synthesized, never authored images");

  expect(note->lines.size() == 8,
         "two groups materialize group/BPM/stop/time slots including holes");
  if (note->lines.size() == 8) {
    const auto &group = note->lines[0];
    expect(group.kind == SkinNoteLineKind::Group &&
               group.laneGroupDestination.x == 12 &&
               group.laneGroupDestination.y == 22 && group.sprite &&
               group.sprite->timer && group.destination &&
               group.destination->timer && group.destination->loop == 3 &&
               group.destination->center == 4 &&
               group.destination->offsetIds == std::vector<int>({6, 7, 8}) &&
               group.destination->conditions.size() == 3 &&
               group.destination->drawCondition &&
               group.destination->frames.size() == 2 &&
               group.destination->frames.front().timeMillis == 10,
           "group line retains nested image timer and complete destination "
           "bindings");
    expect(
        !note->lines[1].sprite && note->lines[1].destination &&
            note->lines[1].laneGroupDestination.x == 50,
        "missing group image remains an optional hole beside required dst[1]");
    expect(note->lines[2].kind == SkinNoteLineKind::Bpm &&
               note->lines[2].sprite && note->lines[2].sprite->resource != 0 &&
               note->lines[3].kind == SkinNoteLineKind::Bpm &&
               !note->lines[3].sprite && !note->lines[3].destination &&
               note->lines[4].kind == SkinNoteLineKind::Stop &&
               note->lines[5].sprite && note->lines[5].sprite->resource != 0 &&
               note->lines[6].kind == SkinNoteLineKind::Time &&
               !note->lines[6].sprite && !note->lines[6].destination &&
               !note->lines[7].sprite && !note->lines[7].destination,
           "auxiliary prefixes truncate to groups and materialize sparse "
           "suffix holes");
  }

  const auto validated = SkinModelValidator{}.validate(
      *decoded.model, {.builtins = builtins,
                       .callbacks = session.runtime()->callbackLiveness()});
  expect(validated.model.has_value() && !validated.criticalFailure,
         "live modern Note and nested callback bindings validate end to end");
}

constexpr std::string_view kLegacyNote = R"lua(
local ids = {'n','m','le','ls','lb','la','he','hs','hb','ha','hd','hr'}
local images = {}
for i,id in ipairs(ids) do
  images[i] = {id=id,src='atlas',x=i*10,y=0,w=8,h=9}
end
return {
  type=0,w=1280,h=720,
  source={{id='atlas',path='atlas.png'}},image=images,
  note={id='notes',note={'n'},mine={'m'},lnend={'le'},lnstart={'ls'},
        lnbody={'lb'},lnbodyActive={},lnactive={'la'},
        hcnend={'he'},hcnstart={'hs'},hcnbody={'hb'},hcnbodyActive={},
        hcnactive={'ha'},hcndamage={'hd'},hcnreactive={'hr'},
        dst={{x=1,y=2,w=3,h=4}},dst2=42},
  destination={{id='notes',dst={{}}}}
}
)lua";

void testEmptyModernFieldsSelectLegacyFamilies() {
  LiveSession session(kLegacyNote);
  auto decoded = session.decode();
  expect(decoded.model.has_value(), "legacy Note family fixture decodes");
  if (!decoded.model) {
    return;
  }
  const auto *definition = noteDefinition(*decoded.model);
  const auto *note =
      definition ? std::get_if<SkinNoteObject>(&definition->payload) : nullptr;
  expect(note && note->lanes.size() == 1,
         "legacy Note fixture preserves its lane");
  if (!note || note->lanes.empty()) {
    return;
  }
  const auto &lane = note->lanes.front();
  expect(spriteX(lane, SkinNoteVisualKind::LnBodyActive) == 50 &&
             spriteX(lane, SkinNoteVisualKind::LnBodyInactive) == 60 &&
             spriteX(lane, SkinNoteVisualKind::HcnBodyActive) == 90 &&
             spriteX(lane, SkinNoteVisualKind::HcnBodyInactive) == 100 &&
             spriteX(lane, SkinNoteVisualKind::HcnDamage) == 110 &&
             spriteX(lane, SkinNoteVisualKind::HcnReactive) == 120,
         "empty modern fields use every exact legacy LN/HCN role");
  expect(lane.authoredNoteHeight == 9 && lane.secondaryDestinationY == 42 &&
             note->expansionRatePercent == std::array<int, 2>{100, 100},
         "known frame height, authored dst2, and default expansion normalize");
}

std::string malformedNoteBody(std::string_view noteBody) {
  std::string source = R"lua(
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 image={{id='n',src='atlas',w=8,h=8}},
 note=)lua";
  source.append(noteBody);
  source.append(",destination={{id='notes',dst={{}}}}}");
  return source;
}

void expectDecodeFailure(std::string_view source, std::string_view message) {
  LiveSession session(source);
  const auto decoded = session.decode();
  expect(!decoded.model.has_value() && !decoded.diagnostics.empty(), message);
}

void testMalformedCriticalNotePathsFailClosed() {
  expectDecodeFailure(
      malformedNoteBody(
          "{id='notes',note={'n','n'},mine={'n','n'},lnend={'n','n'},"
          "lnstart={'n','n'},lnbodyActive={'n','n'},lnbody={'n','n'},"
          "hcnend={'n','n'},hcnstart={'n','n'},hcnbodyActive={'n','n'},"
          "hcnbody={'n','n'},hcnbodyMiss={'n','n'},"
          "hcnbodyReactive={'n','n'},dst={{x=0,y=0,w=1,h=1}}}"),
      "missing required lane dst rejects the critical Note entry");
  expectDecodeFailure(
      malformedNoteBody(
          "{id='notes',note={'n'},mine={},lnend={'n'},lnstart={'n'},"
          "lnbodyActive={'n'},lnbody={'n'},hcnend={'n'},hcnstart={'n'},"
          "hcnbodyActive={'n'},hcnbody={'n'},hcnbodyMiss={'n'},"
          "hcnbodyReactive={'n'},dst={{x=0,y=0,w=1,h=1}}}"),
      "unsafe selected visual cardinality rejects the critical Note entry");
  expectDecodeFailure(
      malformedNoteBody(
          "{id='notes',note={[1]='n',[3]='n'},mine={},lnend={},lnstart={},"
          "lnbody={},lnactive={},hcnend={},hcnstart={},hcnbody={},"
          "hcnactive={},hcndamage={},hcnreactive={},dst={}}"),
      "a visual array hole fails safely instead of indexing through it");
  expectDecodeFailure(
      malformedNoteBody(
          "{id='notes',note={},mine={},lnend={},lnstart={},lnbody={},"
          "lnactive={},hcnend={},hcnstart={},hcnbody={},hcnactive={},"
          "hcndamage={},hcnreactive={},dst={},expansionrate={100.5,100}}"),
      "fractional expansionrate is rejected rather than truncated");

  LiveSession oversizedExpansion(malformedNoteBody(
      "{id='notes',note={},mine={},lnend={},lnstart={},lnbody={},"
      "lnactive={},hcnend={},hcnstart={},hcnbody={},hcnactive={},"
      "hcndamage={},hcnreactive={},dst={},expansionrate={100,100,100}}"));
  const auto oversizedResult = oversizedExpansion.decode();
  expect(!oversizedResult.model && oversizedResult.diagnostics.size() == 1 &&
             oversizedResult.diagnostics.front().code ==
                 "skin_lua_header_invalid",
         "oversized expansionrate emits one deterministic diagnostic");

  expectDecodeFailure(
      malformedNoteBody(
          "{id='notes',note={},mine={},lnend={},lnstart={},lnbody={},"
          "lnactive={},hcnend={},hcnstart={},hcnbody={},hcnactive={},"
          "hcndamage={},hcnreactive={},dst={},"
          "group={{id='n',dst={}}}}"),
      "group line without dst[1] lane geometry rejects the critical Note");
}

std::string noteVisualBudgetFixture(int normalDivisions) {
  std::ostringstream source;
  source << R"lua(
local normal, mine, wide, dst = {}, {}, {}, {}
for i=1,100 do
  normal[i]='normal'; mine[i]='mine'; wide[i]='wide'
  dst[i]={x=i,y=0,w=1,h=1}
end
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 image={
  {id='normal',src='atlas',w=)lua"
         << normalDivisions << ",h=1,divx=" << normalDivisions << R"lua(},
  {id='mine',src='atlas',w=165,h=1,divx=165},
  {id='wide',src='atlas',w=167,h=1,divx=167}},
 note={id='notes',note=normal,mine=mine,lnend=wide,lnstart=wide,
       lnbodyActive=wide,lnbody=wide,hcnend=wide,hcnstart=wide,
       hcnbodyActive=wide,hcnbody=wide,hcnbodyMiss=wide,
       hcnbodyReactive=wide,dst=dst},
 destination={{id='notes',dst={{}}}}}
)lua";
  return source.str();
}

std::string noteLineBudgetFixture(int divisions) {
  std::ostringstream source;
  source << R"lua(
local group,bpm,stop,time={},{},{},{}
for i=1,100 do
 local line={id='line',dst={{x=i,y=0,w=1,h=1}}}
 group[i]=line;bpm[i]=line;stop[i]=line;time[i]=line
end
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 image={{id='line',src='atlas',w=)lua"
         << divisions << ",h=1,divx=" << divisions << R"lua(}},
 note={id='notes',note={},mine={},lnend={},lnstart={},lnbody={},
       lnactive={},hcnend={},hcnstart={},hcnbody={},hcnactive={},
       hcndamage={},hcnreactive={},dst={},group=group,bpm=bpm,
       stop=stop,time=time},
 destination={{id='notes',dst={{}}}}}
)lua";
  return source.str();
}

void testCompleteNoteMaterializationBudgets() {
  LiveSession exactVisuals(noteVisualBudgetFixture(165));
  expect(exactVisuals.decode().model.has_value(),
         "exact 200000 materialized visual frames remain accepted");
  LiveSession excessiveVisuals(noteVisualBudgetFixture(166));
  expect(!excessiveVisuals.decode().model.has_value(),
         "one extra visual frame per lane exceeds the materialized budget");

  LiveSession exactLines(noteLineBudgetFixture(500));
  expect(exactLines.decode().model.has_value(),
         "exact 200000 materialized line sprite frames remain accepted");
  LiveSession excessiveLines(noteLineBudgetFixture(501));
  expect(!excessiveLines.decode().model.has_value(),
         "line sprite frames above the exact budget fail closed");
}

void testNoteOnlyImagesIgnoreGenericStateAndActionFields() {
  constexpr std::string_view source = R"lua(
return {type=0,w=1280,h=720,
 source={{id='atlas',path='atlas.png'}},
 image={{id='note-sprite',src='atlas',w=20,h=10,divx=2,len=2,
         timer=7,cycle=33,act={unsupported=true}}},
 note={id='notes',note={'note-sprite'},mine={'note-sprite'},
       lnend={'note-sprite'},lnstart={'note-sprite'},
       lnbodyActive={'note-sprite'},lnbody={'note-sprite'},
       hcnend={'note-sprite'},hcnstart={'note-sprite'},
       hcnbodyActive={'note-sprite'},hcnbody={'note-sprite'},
       hcnbodyMiss={'note-sprite'},hcnbodyReactive={'note-sprite'},
       dst={{x=0,y=0,w=20,h=100}}},
 destination={{id='notes',dst={{}}}}}
)lua";
  LiveSession session(source);
  const SkinBuiltinBindingCatalogView builtins(kBuiltins);
  auto decoded = session.decode(builtins);
  expect(decoded.model.has_value(),
         "Note-only Image ignores unsupported generic ref/act controls");
  if (!decoded.model) {
    return;
  }
  const auto *definition = noteDefinition(*decoded.model);
  const auto *note =
      definition ? std::get_if<SkinNoteObject>(&definition->payload) : nullptr;
  const auto *sprite =
      note && !note->lanes.empty()
          ? visualSprite(note->lanes.front(), SkinNoteVisualKind::Normal)
          : nullptr;
  expect(sprite && sprite->frames.size() == 2 && sprite->cycleMillis == 33 &&
             sprite->timer && decoded.model->integerProperties.empty() &&
             decoded.model->events.empty(),
         "Note special path keeps raw frames/timer/cycle without generic "
         "bindings");
}

void testValidatorRetainsMalformedCriticalNoteState() {
  LiveSession session(kLegacyNote);
  auto decoded = session.decode();
  if (!decoded.model || !session.runtime()) {
    expect(false, "validator Note fixture decodes");
    return;
  }

  auto missingVisual = *decoded.model;
  auto *definition = noteDefinition(missingVisual);
  std::get<SkinNoteObject>(definition->payload)
      .lanes.front()
      .visuals.erase(SkinNoteVisualKind::Normal);
  auto missingResult = SkinModelValidator{}.validate(
      std::move(missingVisual),
      {.callbacks = session.runtime()->callbackLiveness()});
  expect(missingResult.model && !missingResult.criticalFailure &&
             missingResult.model->disabledOptionalObjects.empty(),
         "missing normalized visual stays object-local like Skin.prepare");

  auto invalidGeometry = *decoded.model;
  definition = noteDefinition(invalidGeometry);
  std::get<SkinNoteObject>(definition->payload)
      .lanes.front()
      .laneDestination.x = 9000;
  auto geometryResult = SkinModelValidator{}.validate(
      std::move(invalidGeometry),
      {.callbacks = session.runtime()->callbackLiveness()});
  expect(geometryResult.model && !geometryResult.criticalFailure &&
             geometryResult.model->disabledOptionalObjects.empty(),
         "out-of-contract lane geometry stays object-local like Skin.prepare");
}

} // namespace

int main() {
  testScuroShapedModernNoteAndNestedLinesDecodeLive();
  testEmptyModernFieldsSelectLegacyFamilies();
  testMalformedCriticalNotePathsFailClosed();
  testCompleteNoteMaterializationBudgets();
  testNoteOnlyImagesIgnoreGenericStateAndActionFields();
  testValidatorRetainsMalformedCriticalNoteState();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  return 0;
}
