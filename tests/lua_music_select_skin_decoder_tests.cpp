#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "music_select_skin_ledger_evidence.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/beatoraja/MusicSelectSkinModelResolver.h"
#include "skin/beatoraja/MusicSelectSkinStateBridge.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace skin;

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

int failures = 0;

void require(bool condition, std::string_view message) {
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
              ("asobmashow-music-select-decoder-" +
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

class Fixture {
public:
  explicit Fixture(std::optional<std::string_view> script = std::nullopt)
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions = temp.root() / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("MusicSelectDecoder").package),
        entry(*normalizeEntryPath(package, "select/main.luaskin").entry) {
    const fs::path source = temp.root() / "source";
    fs::create_directories(source / "select");
    if (script) {
      std::ofstream output(source / "select/main.luaskin");
      output << *script;
    } else {
      fs::copy_file(
          fs::path(ASOBMASHOW_SOURCE_DIR) /
              "tests/fixtures/skin/music_select/songlist_contract.luaskin",
          source / "select/main.luaskin");
    }
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    require(snapshot.prepared.has_value(), "music-select fixture snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  std::unique_ptr<LuaSkinFileSystem> fileSystem(bool writes = false) {
    if (!prepared) {
      return {};
    }
    return LuaSkinFileSystem::create(
               {.revision = prepared->readView(),
                .entry = entry,
                .storageRoots = roots,
                .profileId = writes ? makeSkinProfileId(
                                          "77777777-7777-4777-8777-777777777777")
                                    : std::nullopt,
                .allowDataWrites = writes})
        .fileSystem;
  }

private:
  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  SkinEntryId entry;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

void testType5DestinationRemainsRequired() {
  Fixture fixture{"return { type = 5, w = 1280, h = 720 }"};
  auto runtimeFileSystem = fixture.fileSystem();
  require(runtimeFileSystem != nullptr,
          "destination fixture filesystem creates");
  if (!runtimeFileSystem) return;
  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::MusicSelect,
       .fileSystem = std::move(runtimeFileSystem)});
  require(created.runtime != nullptr, "destination fixture runtime creates");
  if (!created.runtime) return;
  auto value = created.runtime->loadHeader();
  require(value.value.has_value(), "destination fixture header executes");
  if (!value.value) return;
  LuaSkinTableDecoder decoder;
  const auto decoded = decoder.decodeMusicSelect(
      *value.value,
      {.runtime = *created.runtime,
       .builtins = gameplaySkinBuiltinCatalog()});
  require(!decoded.model &&
              std::ranges::any_of(decoded.diagnostics, [](const auto &diagnostic) {
                return diagnostic.code == "skin_lua_model_destination_missing";
              }),
          "type-5 omits its model when the required destination array is absent");
}

void testType5DestinationIdsAndEventAritiesMatchSkinLuaAccessor() {
  constexpr std::string_view commonPrefix = R"(
return {
  type = 5,
  source = {{id = "source", path = "image.png"}},
  image = {{id = "button", src = "source", x = 0, y = 0,
            w = 1, h = 1, act = )";
  constexpr std::string_view commonSuffix = R"(}},
  destination = {{id = "button", dst = {{}}}},
}
)";
  const auto decode = [](std::string script) {
    Fixture fixture{script};
    auto fileSystem = fixture.fileSystem();
    if (!fileSystem) return BeatorajaSkinModelDecodeResult{};
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::MusicSelect,
         .fileSystem = std::move(fileSystem)});
    if (!created.runtime) return BeatorajaSkinModelDecodeResult{};
    auto value = created.runtime->loadHeader();
    if (!value.value) return BeatorajaSkinModelDecodeResult{};
    return LuaSkinTableDecoder{}.decodeMusicSelect(
        *value.value,
        {.runtime = *created.runtime,
         .builtins = gameplaySkinBuiltinCatalog()});
  };

  const auto twoArguments = decode(std::string(commonPrefix) +
                                   "function(first, second) end" +
                                   std::string(commonSuffix));
  require(twoArguments.model &&
              std::ranges::any_of(twoArguments.model->objects,
                                  [](const auto &object) {
                                    return object.authoredName == "button" &&
                                           std::holds_alternative<SkinImageObject>(
                                               object.payload) &&
                                           std::get<SkinImageObject>(object.payload)
                                               .clickEvent.has_value();
                                  }),
          "SkinLuaAccessor keeps a two-argument event callback");

  const auto threeArguments = decode(std::string(commonPrefix) +
                                     "function(first, second, third) end" +
                                     std::string(commonSuffix));
  require(threeArguments.model &&
              std::ranges::any_of(threeArguments.model->objects,
                                  [](const auto &object) {
                                    return object.authoredName == "button" &&
                                           std::holds_alternative<SkinImageObject>(
                                               object.payload) &&
                                           !std::get<SkinImageObject>(object.payload)
                                                .clickEvent.has_value();
                                  }),
          "SkinLuaAccessor omits an event callback with more than two arguments");

  const auto missingId = decode(R"(
return {
  type = 5,
  source = {{id = "source", path = "image.png"}},
  image = {{id = "button", src = "source", x = 0, y = 0, w = 1, h = 1}},
  destination = {{dst = {{}}}},
}
)");
  require(!missingId.model &&
              std::ranges::any_of(missingId.diagnostics,
                                  [](const auto &diagnostic) {
                                    return diagnostic.code ==
                                           "skin_lua_model_destination_id_missing";
                                  }),
          "a missing destination id fails only once the selector loader would "
          "dereference it");

  const auto nonTableDestination = decode(R"(
return { type = 5, destination = 1 }
)");
  require(!nonTableDestination.model &&
              std::ranges::any_of(nonTableDestination.diagnostics,
                                  [](const auto &diagnostic) {
                                    return diagnostic.code ==
                                           "skin_lua_model_destination_missing";
                                  }),
          "a non-table destination follows the pinned loader's failed-array "
          "path instead of rendering a blank selector");
}

void testType5GaugeDoesNotApplyGameplayExpansionValidation() {
  Fixture fixture{R"(
return {
  type = 5,
  source = {{id = "source", path = "image.png"}},
  image = {
    {id = "gauge-0", src = "source", x = 0, y = 0, w = 1, h = 1},
    {id = "gauge-1", src = "source", x = 0, y = 0, w = 1, h = 1},
    {id = "gauge-2", src = "source", x = 0, y = 0, w = 1, h = 1},
    {id = "gauge-3", src = "source", x = 0, y = 0, w = 1, h = 1},
  },
  gauge = {
    id = "gauge",
    nodes = {"gauge-0", "gauge-1", "gauge-2", "gauge-3"},
    parts = -99, type = 999, range = -7, cycle = -1,
    starttime = 10, endtime = -10,
  },
  destination = {{id = "gauge", dst = {{}}}},
}
)"};
  auto fileSystem = fixture.fileSystem();
  require(fileSystem != nullptr, "type-5 gauge fixture filesystem creates");
  if (!fileSystem) return;
  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::MusicSelect,
       .fileSystem = std::move(fileSystem)});
  require(created.runtime != nullptr, "type-5 gauge fixture runtime creates");
  if (!created.runtime) return;
  const auto value = created.runtime->loadHeader();
  require(value.value.has_value(), "type-5 gauge fixture executes");
  if (!value.value) return;
  const auto decoded = LuaSkinTableDecoder{}.decodeMusicSelect(
      *value.value,
      {.runtime = *created.runtime,
       .builtins = gameplaySkinBuiltinCatalog()});
  require(decoded.model &&
              std::ranges::none_of(decoded.diagnostics, [](const auto &value) {
                return value.severity == DiagnosticSeverity::Error;
              }),
          "type-5 Gauge keeps source-authored parameters out of gameplay-only "
          "expansion validation");
}

void testConfiguredType5SongListPreservesEveryAuthoredValue() {
  Fixture fixture;
  auto runtimeFileSystem = fixture.fileSystem(true);
  auto reconciliationFileSystem = fixture.fileSystem();
  require(runtimeFileSystem && reconciliationFileSystem,
          "music-select filesystems create");
  if (!runtimeFileSystem || !reconciliationFileSystem) {
    return;
  }
  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::MusicSelect,
       .fileSystem = std::move(runtimeFileSystem)});
  require(created.runtime != nullptr, "music-select runtime creates");
  if (!created.runtime) {
    return;
  }
  auto headerValue = created.runtime->loadHeader();
  require(headerValue.value.has_value(), "type-5 header executes");
  if (!headerValue.value) {
    return;
  }
  LuaSkinTableDecoder decoder;
  const auto header = decoder.decodeHeader(*headerValue.value);
  require(header.header && header.header->type == 5 &&
              header.header->name == "Header Select" &&
              header.header->width == 0 && header.header->height == -1,
          "first pass retains the type-5 header without host dimension "
          "validation");
  if (!header.header) {
    return;
  }
  headerValue.value.reset();
  const auto reconciled = reconcileSkinConfiguration(
      *header.header, nullptr, *reconciliationFileSystem);
  require(reconciled.configuration.has_value(),
          "type-5 configuration reconciles");
  if (!reconciled.configuration) {
    return;
  }
  require(reconciled.configuration->options.contains(""),
          "type-5 configuration retains an empty authored property name");
  auto configured = created.runtime->loadConfigured(*reconciled.configuration);
  require(configured.value.has_value(), "configured type-5 pass executes");
  if (!configured.value) {
    return;
  }
  const auto decoded = decoder.decodeMusicSelect(
      *configured.value,
      {.runtime = *created.runtime,
       .builtins = gameplaySkinBuiltinCatalog()});
  require(decoded.model && decoded.model->header.type == 5 &&
              decoded.model->header.name == "Configured Select",
          "configured type-5 Lua table decodes");
  require(std::ranges::none_of(decoded.diagnostics, [](const auto &value) {
            return value.severity == DiagnosticSeverity::Error;
          }),
          "source-tolerated missing resources and callback scripts do not "
          "become host-fatal diagnostics");
  if (!decoded.model || !decoded.model->songListDefinition) {
    require(false, "type-5 model retains its songlist definition");
    return;
  }
  const auto &songList = *decoded.model->songListDefinition;
  require(songList.id == "song-list" && songList.center == 100000,
          "authored song-list identity and center are not clamped");
  require(songList.clickable.size() == 3 &&
              std::ranges::find(songList.clickable, -1000) !=
                  songList.clickable.end() &&
              std::ranges::find(songList.clickable, 0) !=
                  songList.clickable.end() &&
              std::ranges::find(songList.clickable, 1000) !=
                  songList.clickable.end(),
          "sparse and mixed-key clickable values survive decode");
  require(songList.listOff.size() == 61 && songList.listOn.size() == 61 &&
              songList.text.size() == 12 && songList.level.size() == 8 &&
              songList.lamp.size() == 12 && songList.playerLamp.size() == 12 &&
              songList.rivalLamp.size() == 12 && songList.trophy.size() == 4 &&
              songList.label.size() == 6,
          "song-list arrays longer than fixed SkinBar slots survive decode");
  const auto &unvalidatedPresentation = songList.listOn.front().destination;
  require(unvalidatedPresentation.blend == SkinBlendMode::Normal &&
              unvalidatedPresentation.filter == SkinFilterMode::Linear &&
              unvalidatedPresentation.stretch == SkinStretchMode::Stretch &&
              unvalidatedPresentation.offsetIds.size() == 2 &&
              unvalidatedPresentation.offsetIds.front() == 1001 &&
              unvalidatedPresentation.conditions.size() == 1 &&
              std::get<int>(unvalidatedPresentation.conditions.front()) == 900 &&
              unvalidatedPresentation.frames.front().rgba ==
                  std::array<std::uint8_t, 4>{0, 255, 255, 0},
          "type-5 destinations retain Beatoraja's sparse arrays and "
          "nonstandard presentation values with Color's source clamp");
  require(songList.trophy.front().destination.frames.size() == 4 &&
              std::ranges::all_of(
                  songList.trophy.front().destination.frames,
                  [](const auto &frame) {
                    return frame.timeMillis == 0 && frame.x == 0.0 &&
                           frame.y == 0.0 && frame.width == 0.0 &&
                           frame.height == 0.0;
                  }),
          "Lua object arrays retain Beatoraja's default objects for scalar "
          "table values");
  require(songList.graph && songList.graph->objectName == "distribution" &&
              songList.graph->destination.loop == -1 &&
              songList.graph->destination.frames.size() == 1 &&
              songList.graph->destination.frames.front().timeMillis == 99,
          "nullable graph destination preserves authored presentation");
  const auto canonical = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "song-list" &&
               std::holds_alternative<SkinSongListObject>(object.payload);
      });
  require(canonical != decoded.model->objects.end(),
          "song-list destination creates a canonical object placeholder");
  const auto imageSet = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        const auto *image = std::get_if<SkinImageObject>(&object.payload);
        return object.authoredName == "on-1" && image &&
               image->definitionKind == SkinImageDefinitionKind::ImageSet;
      });
  require(imageSet != decoded.model->objects.end() &&
              std::get<SkinImageObject>(imageSet->payload)
                      .orderedStates.front()
                      .cycleMillis == 300,
          "SongList ImageSet definitions retain the first image timing");
  require(std::ranges::any_of(decoded.model->objects, [](const auto &object) {
            return object.authoredName == "distribution" &&
                   std::holds_alternative<SkinSelectDistributionGraphObject>(
                       object.payload);
          }),
          "negative SongList graph definitions retain select semantics");
  const auto directGraph = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "direct-distribution" &&
               std::holds_alternative<SkinSelectDistributionGraphObject>(
                   object.payload);
      });
  require(directGraph != decoded.model->objects.end() &&
              std::get<SkinSelectDistributionGraphObject>(
                  directGraph->payload).type ==
                  SkinSelectDistributionGraphType::Judge &&
              std::ranges::none_of(decoded.model->objects,
                                   [](const auto &object) {
                                     return object.authoredName ==
                                            "missing-graph";
                                   }),
          "top-level negative graphs retain selector semantics and an "
          "unresolvable graph is omitted without rejecting the skin");
  const auto lenFloor = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "len-floor" &&
               std::holds_alternative<SkinImageObject>(object.payload);
      });
  require(lenFloor != decoded.model->objects.end() &&
              std::get<SkinImageObject>(lenFloor->payload)
                      .orderedStates.size() == 3 &&
              std::ranges::all_of(
                  std::get<SkinImageObject>(lenFloor->payload).orderedStates,
                  [](const SkinSpriteFrames &state) {
                    return state.frames.size() == 1 &&
                           state.cycleMillis == 0;
                  }) &&
              std::get<SkinImageObject>(lenFloor->payload).clickMode == -1 &&
              !std::get<SkinImageObject>(lenFloor->payload).clickEvent,
          "type-5 Image.len floors each state width, ignores trailing cells, "
          "uses LuaJ integer coercion, and omits a callback script Beatoraja "
          "cannot compile");
  const auto keyedImage = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "keyed-image" &&
               std::holds_alternative<SkinImageObject>(object.payload);
      });
  require(keyedImage != decoded.model->objects.end() &&
              std::get<SkinImageObject>(keyedImage->payload).clickEvent &&
              std::ranges::any_of(
                  decoded.model->events, [&](const auto &event) {
                    const auto *selector =
                        std::get_if<SkinBuiltinPropertySelector>(&event.source);
                    return event.id == *std::get<SkinImageObject>(
                                           keyedImage->payload)
                                           .clickEvent &&
                           selector != nullptr &&
                           std::get<int>(selector->value) == -1;
                  }),
          "type-5 array entries keyed by strings retain their direct Lua "
          "binding source and LuaJ-wrapped numeric event selector");
  const auto level = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "level-1" &&
               std::holds_alternative<SkinNumberObject>(object.payload);
      });
  require(level != decoded.model->objects.end() &&
              std::get<SkinNumberObject>(level->payload)
                      .perDigitOffsets.size() == 1 &&
              std::get<SkinNumberObject>(level->payload)
                      .perDigitOffsets.front().x == 0.0 &&
              std::get<SkinNumberObject>(level->payload)
                      .perDigitOffsets.front().y ==
                  static_cast<double>(static_cast<float>(1.23456789)),
          "type-5 float fields preserve LuaJ zero coercion and float "
          "narrowing for sparse object arrays");
  require(std::ranges::none_of(decoded.model->objects, [](const auto &object) {
            return object.authoredName == "text-1";
          }),
          "the unresolved song-list text object is not materialized");
  const auto unvalidatedText = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "unvalidated-text" &&
               std::holds_alternative<SkinTextObject>(object.payload);
      });
  require(unvalidatedText != decoded.model->objects.end(),
          "type-5 Text fields accepted by Beatoraja are not rejected by host "
          "normalization");
  if (unvalidatedText != decoded.model->objects.end()) {
    const auto &text = std::get<SkinTextObject>(unvalidatedText->payload);
    require(text.pointSize == -123 && text.alignment == 77 &&
                text.overflow == -9 && text.outlineWidth == -10000.0 &&
                text.shadowSmoothness == -20.0 &&
                text.outlineRgba == std::array<std::uint8_t, 4>{255, 255,
                                                                 255, 0} &&
                text.shadowRgba == std::array<std::uint8_t, 4>{255, 255,
                                                                255, 0},
            "type-5 Text preserves Beatoraja's omitted transparent colour "
            "defaults alongside authored layout and style values");
  }
  require(std::ranges::none_of(decoded.model->objects, [](const auto &object) {
            return object.authoredName == "play-only-bga";
          }),
          "type-5 uses JsonSelectSkinObjectLoader and ignores play-only "
          "definitions and their unresolved destinations");
  const auto emptyImageSet = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "empty-imageset" &&
               std::holds_alternative<SkinImageObject>(object.payload);
      });
  const auto emptyGaugeGraph = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "empty-gaugegraph" &&
               std::holds_alternative<SkinGaugeGraphObject>(object.payload);
      });
  const auto defaultGaugeGraph = std::ranges::find_if(
      decoded.model->objects, [](const auto &object) {
        return object.authoredName == "default-gaugegraph" &&
               std::holds_alternative<SkinGaugeGraphObject>(object.payload);
      });
  const auto lenDestination = std::ranges::find_if(
      decoded.model->destinations, [&](const auto &destination) {
        return lenFloor != decoded.model->objects.end() &&
               destination.object == lenFloor->id;
      });
  require(emptyImageSet != decoded.model->objects.end() &&
              std::get<SkinImageObject>(emptyImageSet->payload)
                  .orderedStates.empty() &&
              emptyGaugeGraph != decoded.model->objects.end() &&
              defaultGaugeGraph != decoded.model->objects.end() &&
              std::get<SkinGaugeGraphObject>(defaultGaugeGraph->payload)
                      .rgba[0] ==
                  std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                                0xff00ffffU, 0x440044ffU} &&
              lenDestination != decoded.model->destinations.end() &&
              lenDestination->presentation.mouseRect &&
              lenDestination->presentation.mouseRect->x == 0.0 &&
              lenDestination->presentation.mouseRect->y == 0.0 &&
              lenDestination->presentation.mouseRect->width == 0.0 &&
              lenDestination->presentation.mouseRect->height == 0.0,
          "type-5 preserves Beatoraja's Java defaults while converting "
          "scalar arrays and object fields without rejecting the skin");
}

void testType5GenericVisualizersKeepPinnedColorDefaults() {
  Fixture fixture{R"(
return {
  type = 5,
  timingvisualizer = {{id = "timing"}},
  timingdistributiongraph = {{id = "distribution"}},
  hiterrorvisualizer = {{id = "hiterror"}},
  destination = {
    {id = "timing", dst = {{}}},
    {id = "distribution", dst = {{}}},
    {id = "hiterror", dst = {{}}},
  },
}
)"};
  auto fileSystem = fixture.fileSystem();
  if (!fileSystem) return;
  auto runtime = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::MusicSelect,
       .fileSystem = std::move(fileSystem)});
  if (!runtime.runtime) return;
  const auto value = runtime.runtime->loadHeader();
  if (!value.value) return;
  const auto decoded = LuaSkinTableDecoder{}.decodeMusicSelect(
      *value.value,
      {.runtime = *runtime.runtime,
       .builtins = gameplaySkinBuiltinCatalog()});
  const auto object = [&](std::string_view id) -> const SkinObjectDefinition * {
    if (!decoded.model) return nullptr;
    const auto found = std::ranges::find(
        decoded.model->objects, id, &SkinObjectDefinition::authoredName);
    return found == decoded.model->objects.end() ? nullptr : &*found;
  };
  const auto *timing = object("timing");
  const auto *distribution = object("distribution");
  const auto *hiterror = object("hiterror");
  const auto *timingPayload = timing != nullptr
                                  ? std::get_if<SkinTimingVisualizerObject>(
                                        &timing->payload)
                                  : nullptr;
  const auto *distributionPayload =
      distribution != nullptr
          ? std::get_if<SkinTimingDistributionGraphObject>(
                &distribution->payload)
          : nullptr;
  const auto *hiterrorPayload = hiterror != nullptr
                                    ? std::get_if<SkinHitErrorVisualizerObject>(
                                          &hiterror->payload)
                                    : nullptr;
  require(timingPayload != nullptr &&
              timingPayload->judgeRgba[4] == 0x000000ffU &&
              distributionPayload != nullptr &&
              distributionPayload->graphRgba == 0x00ff00ffU &&
              distributionPayload->judgeRgba[4] == 0x000000ffU &&
              hiterrorPayload != nullptr &&
              hiterrorPayload->judgeRgba[4] == 0xcc292980U,
          "type-5 generic visualizers preserve pinned Java color defaults "
          "when Lua omits their optional color fields");
}

void testType5GaugeDereferencesMissingDestinationId() {
  Fixture fixture{R"(
return {
  type = 5,
  gauge = {id = "gauge"},
  destination = {{dst = {{}}}},
}
)"};
  auto fileSystem = fixture.fileSystem();
  if (!fileSystem) return;
  auto runtime = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::MusicSelect,
       .fileSystem = std::move(fileSystem)});
  if (!runtime.runtime) return;
  const auto value = runtime.runtime->loadHeader();
  if (!value.value) return;
  const auto decoded = LuaSkinTableDecoder{}.decodeMusicSelect(
      *value.value,
      {.runtime = *runtime.runtime,
       .builtins = gameplaySkinBuiltinCatalog()});
  require(!decoded.model &&
              std::ranges::any_of(decoded.diagnostics, [](const auto &entry) {
                return entry.code == "skin_lua_model_destination_id_missing";
              }),
          "type-5 gauge destinations preserve the pinned null-id dereference "
          "failure");
}

void testInstalledAcceptanceSkinsDecodeWhenRequested() {
  const char *acceptanceRoot =
      std::getenv("ASOBMASHOW_SKIN_ACCEPTANCE_ROOT");
  if (acceptanceRoot == nullptr || std::string_view(acceptanceRoot).empty()) {
    return;
  }

  struct AcceptanceSkin {
    std::string package;
    fs::path source;
    std::string entry;
  };
  const fs::path root(acceptanceRoot);
  const char *packageFilter =
      std::getenv("ASOBMASHOW_SKIN_ACCEPTANCE_PACKAGE");
  const std::array skins{
      AcceptanceSkin{"ModernChicAcceptance", root / "ModernChic",
                     "musicselect.luaskin"},
      AcceptanceSkin{"LITONE12Acceptance", root / "LITONE12",
                     "Select/select.luaskin"},
  };

  for (const auto &skin : skins) {
    if (packageFilter != nullptr && std::string_view(packageFilter) !=
                                        skin.package) {
      continue;
    }
    TempDirectory temp;
    SkinStorageRoots roots{.visiblePackages = temp.root() / "visible",
                           .privateRevisions = temp.root() / "revisions",
                           .privateCatalog = temp.root() / "catalog",
                           .profileOverlays = temp.root() / "overlays",
                           .liveSources = true};
    AcceptFiles aliases;
    const auto package = normalizePackageId(skin.package);
    require(package.package.has_value(), "acceptance package ID normalizes");
    if (!package.package) continue;
    const auto entry = normalizeEntryPath(*package.package, skin.entry);
    require(entry.entry.has_value(), "acceptance entry normalizes");
    if (!entry.entry) continue;
    const fs::path installedPackage =
        roots.visiblePackages / package.package->directoryName;
    std::error_code copyError;
    fs::create_directories(roots.visiblePackages, copyError);
    if (!copyError) {
      fs::copy(skin.source, installedPackage, fs::copy_options::recursive,
               copyError);
    }
    require(!copyError,
            skin.package + " copies into an isolated live installation");
    if (copyError) continue;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot =
        snapshotter.snapshot(installedPackage, *package.package, {}, {});
    require(snapshot.prepared.has_value(),
            skin.package + " snapshots as an acceptance fixture");
    if (!snapshot.prepared) continue;

    auto makeFileSystem = [&](bool writes) {
      return LuaSkinFileSystem::create(
                 {.revision = snapshot.prepared->readView(),
                  .entry = *entry.entry,
                  .storageRoots = roots,
                  .profileId =
                      writes
                          ? makeSkinProfileId(
                                "77777777-7777-4777-8777-777777777777")
                          : std::nullopt,
                  .allowDataWrites = writes})
          .fileSystem;
    };
    auto runtimeFileSystem = makeFileSystem(true);
    auto reconciliationFileSystem = makeFileSystem(false);
    require(runtimeFileSystem && reconciliationFileSystem,
            skin.package + " filesystems create");
    if (!runtimeFileSystem || !reconciliationFileSystem) continue;
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::MusicSelect,
         .fileSystem = std::move(runtimeFileSystem)});
    require(created.runtime != nullptr,
            skin.package + " music-select runtime creates");
    if (!created.runtime) continue;
    auto headerValue = created.runtime->loadHeader();
    require(headerValue.value.has_value(),
            skin.package + " header executes");
    if (!headerValue.value) continue;
    LuaSkinTableDecoder decoder;
    const auto header = decoder.decodeHeader(*headerValue.value);
    require(header.header && header.header->type == 5,
            skin.package + " declares a type-5 header");
    if (!header.header) continue;
    headerValue.value.reset();
    const auto reconciled = reconcileSkinConfiguration(
        *header.header, nullptr, *reconciliationFileSystem);
    require(reconciled.configuration.has_value(),
            skin.package + " configuration reconciles");
    if (!reconciled.configuration) continue;
    MusicSelectSkinFrame configuredFrame;
    MusicSelectSkinStateBridge configuredState(configuredFrame);
    created.runtime->setFrameState(&configuredState);
    auto configured =
        created.runtime->loadConfigured(*reconciled.configuration);
    created.runtime->setFrameState(nullptr);
    if (!configured.value && configured.failure) {
      std::cerr << skin.package << ": " << configured.failure->code << ": "
                << configured.failure->message << '\n';
    }
    require(configured.value.has_value(),
            skin.package + " configured pass executes");
    if (!configured.value) continue;
    const auto decoded = decoder.decodeMusicSelect(
        *configured.value,
        {.runtime = *created.runtime,
         .builtins = gameplaySkinBuiltinCatalog()});
    const bool hasErrorDiagnostic =
        std::ranges::any_of(decoded.diagnostics, [](const auto &diagnostic) {
          return diagnostic.severity == DiagnosticSeverity::Error;
        });
    if (!decoded.model || hasErrorDiagnostic) {
      for (const auto &diagnostic : decoded.diagnostics) {
        std::cerr << skin.package << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
      }
    }
    require(decoded.model.has_value(),
            skin.package + " configured type-5 table decodes");
    require(!hasErrorDiagnostic,
            skin.package + " configured type-5 table has no errors");
    if (skin.package == "ModernChicAcceptance" && decoded.model) {
      const auto resolved = MusicSelectSkinModelResolver{}.resolve(*decoded.model);
      require(resolved.songList.has_value(),
              "ModernChicAcceptance song list resolves");
      if (resolved.songList) {
        require(resolved.songList->listOn.size() == 17 &&
                    std::ranges::all_of(resolved.songList->listOn,
                                        [](const auto &presentation) {
                                          return presentation.object != 0;
                                        }),
                "ModernChicAcceptance resolves all 17 chart-list bars");
      }
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  testConfiguredType5SongListPreservesEveryAuthoredValue();
  testType5DestinationRemainsRequired();
  testType5DestinationIdsAndEventAritiesMatchSkinLuaAccessor();
  testType5GaugeDoesNotApplyGameplayExpansionValidation();
  testType5GenericVisualizersKeepPinnedColorDefaults();
  testType5GaugeDereferencesMissingDestinationId();
  testInstalledAcceptanceSkinsDecodeWhenRequested();
  return music_select_skin_ledger_evidence::finish(
      argc, argv, "lua_music_select_skin_decoder_tests", failures,
      {
          "json.field.song-list-center",
          "json.field.song-list-clickable",
          "json.field.song-list-graph",
          "json.field.song-list-id",
          "json.field.song-list-label",
          "json.field.song-list-lamp",
          "json.field.song-list-level",
          "json.field.song-list-listoff",
          "json.field.song-list-liston",
          "json.field.song-list-playerlamp",
          "json.field.song-list-rivallamp",
          "json.field.song-list-text",
          "json.field.song-list-trophy",
          "lua.object-field.skin-songlist",
          "lua.object-field.song-list-center",
          "lua.object-field.song-list-clickable",
          "lua.object-field.song-list-graph",
          "lua.object-field.song-list-id",
          "lua.object-field.song-list-label",
          "lua.object-field.song-list-lamp",
          "lua.object-field.song-list-level",
          "lua.object-field.song-list-listoff",
          "lua.object-field.song-list-liston",
          "lua.object-field.song-list-playerlamp",
          "lua.object-field.song-list-rivallamp",
          "lua.object-field.song-list-text",
          "lua.object-field.song-list-trophy",
      },
      "Lua music-select decoder test(s) failed",
      "Lua music-select decoder tests passed");
}
