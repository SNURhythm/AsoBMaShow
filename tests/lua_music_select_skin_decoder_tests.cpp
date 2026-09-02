#include "skin/beatoraja/LuaSkinTableDecoder.h"
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
  Fixture()
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions = temp.root() / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("MusicSelectDecoder").package),
        entry(*normalizeEntryPath(package, "select/main.luaskin").entry) {
    const fs::path source = temp.root() / "source";
    fs::create_directories(source / "select");
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/skin/music_select/songlist_contract.luaskin",
                  source / "select/main.luaskin");
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
              header.header->name == "Header Select",
          "first pass returns the type-5 header without skin_config");
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
  auto configured = created.runtime->loadConfigured(*reconciled.configuration);
  require(configured.value.has_value(), "configured type-5 pass executes");
  if (!configured.value) {
    return;
  }
  const auto decoded = decoder.decodeMusicSelect(
      *configured.value,
      {.runtime = *created.runtime, .builtins = {}});
  require(decoded.model && decoded.model->header.type == 5 &&
              decoded.model->header.name == "Configured Select",
          "configured type-5 Lua table decodes");
  require(std::ranges::none_of(decoded.diagnostics, [](const auto &value) {
            return value.severity == DiagnosticSeverity::Error;
          }),
          "a text definition whose font cannot be resolved is silently "
          "omitted like JsonSkinObjectLoader");
  if (!decoded.model || !decoded.model->songListDefinition) {
    require(false, "type-5 model retains its songlist definition");
    return;
  }
  const auto &songList = *decoded.model->songListDefinition;
  require(songList.id == "song-list" && songList.center == 100000,
          "authored song-list identity and center are not clamped");
  require(songList.clickable.size() == 3 && songList.clickable.front() == -1000 &&
              songList.clickable.back() == 1000,
          "clickable values outside rendered slots survive decode");
  require(songList.listOff.size() == 61 && songList.listOn.size() == 61 &&
              songList.text.size() == 12 && songList.level.size() == 8 &&
              songList.lamp.size() == 12 && songList.playerLamp.size() == 12 &&
              songList.rivalLamp.size() == 12 && songList.trophy.size() == 4 &&
              songList.label.size() == 6,
          "song-list arrays longer than fixed SkinBar slots survive decode");
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
  require(std::ranges::none_of(decoded.model->objects, [](const auto &object) {
            return object.authoredName == "text-1";
          }),
          "the unresolved song-list text object is not materialized");
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
        *configured.value, {.runtime = *created.runtime, .builtins = {}});
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
