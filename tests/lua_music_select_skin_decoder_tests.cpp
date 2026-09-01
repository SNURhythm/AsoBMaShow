#include "skin/beatoraja/LuaSkinTableDecoder.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
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
  require(songList.graph && songList.graph->objectName == "distribution" &&
              songList.graph->destination.loop == -1 &&
              songList.graph->destination.frames.size() == 1 &&
              songList.graph->destination.frames.front().timeMillis == 99,
          "nullable graph destination preserves authored presentation");
}

} // namespace

int main() {
  testConfiguredType5SongListPreservesEveryAuthoredValue();
  if (failures != 0) {
    std::cerr << failures << " Lua music-select decoder test(s) failed\n";
    return 1;
  }
  std::cout << "Lua music-select decoder tests passed\n";
  return 0;
}
