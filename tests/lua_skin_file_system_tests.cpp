#include "skin/SkinStoragePaths.h"
#include "skin/SkinSafetyPolicy.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/SkinCompatibilityDiagnostics.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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
              ("asobmashow-lua-filesystem-test-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    for (fs::recursive_directory_iterator iterator(root_, ignored), end;
         !ignored && iterator != end; ++iterator) {
      fs::permissions(iterator->path(), fs::perms::owner_all,
                      fs::perm_options::add, ignored);
    }
    ignored.clear();
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

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::span<const std::byte> bytesOf(std::string_view value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

std::string bytesToString(std::span<const std::byte> value) {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "visible",
          .privateRevisions = root / "revisions",
          .privateCatalog = root / "catalog",
          .profileOverlays = root / "overlays"};
}

SkinEntryId entryId(const SkinPackageId &package,
                    std::string_view relativePath) {
  return *normalizeEntryPath(package, relativePath).entry;
}

class PackageFixture {
public:
  PackageFixture()
      : roots(rootsBelow(temp.root())),
        package(*normalizePackageId("FixtureSkin").package),
        entry(entryId(package, "entry/play.luaskin")) {
    const fs::path source = temp.root() / "source";
    const fs::path committedFixture =
        fs::path(ASOBMASHOW_SOURCE_DIR) /
        "tests/fixtures/beatoraja_skin/lua/filesystem";
    fs::copy(committedFixture, source, fs::copy_options::recursive);
    writeText(source / "entry/direct.lua", "return 'direct'\n");
    writeText(source / "entry/direct/init.lua", "return 'later'\n");
    writeText(source / "entry/fallback/init.lua", "return 'fallback'\n");
    writeText(source / "entry/data/state.txt", "package-state");
    writeText(source / "entry/resources/image.bin", "package-resource");
    writeText(source / "entry/lines.txt", "one\ntwo\nthree\n");
    writeText(source / "entry/binary.lua", std::string("\x1bLua", 4));
    writeText(source / "alternate/play.luaskin", "return {}\n");

    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "package fixture snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  LuaSkinFileSystemCreateResult create(
      const SkinEntryId &selectedEntry, bool writes,
      SkinSafetyLevel safetyLevel = SkinSafetyLevel::Standard) {
    return LuaSkinFileSystem::create({.revision = prepared->readView(),
                                      .entry = selectedEntry,
                                      .storageRoots = roots,
                                      .profileId = std::nullopt,
                                      .allowDataWrites = writes,
                                      .safetyPolicy =
                                          SkinSafetyPolicy(safetyLevel)});
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  SkinEntryId entry;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

void testCompatibilityDiagnosticsDeduplicateAndRetainCriticality() {
  SkinCompatibilityDiagnostics diagnostics;
  SkinDiagnostic first{
      .code = "skin_file_unsupported",
      .message = "first stable explanation",
      .virtualPath = "entry/play.luaskin",
      .severity = DiagnosticSeverity::Warning,
      .source = SkinSourceLocation{.virtualPath = "entry/play.luaskin",
                                   .line = 7,
                                   .column = 3},
  };
  diagnostics.report(first, false, "optional-object");
  first.message = "duplicate wording must not create another record";
  diagnostics.report(first, true, "optional-object");
  diagnostics.report(first, false, "different-object");

  const auto entries = diagnostics.entries();
  expect(entries.size() == 2,
         "compatibility diagnostics deduplicate by source and object");
  if (entries.size() == 2) {
    expect(entries[0].diagnostic.message == "first stable explanation" &&
               entries[0].critical,
           "the first diagnostic text is stable and criticality is upgraded");
    expect(entries[1].objectId == "different-object" && !entries[1].critical,
           "different object attribution remains a distinct diagnostic");
  }
  expect(diagnostics.hasCritical(),
         "the collector reports whether any diagnostic is critical");
}

void testBeatorajaDirectSkinDirectorySemantics() {
  PackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  const fs::path visiblePackage =
      fixture.roots.visiblePackages / fixture.package.directoryName;
  fs::create_directories(visiblePackage.parent_path());
  fs::copy(fixture.temp.root() / "source", visiblePackage,
           fs::copy_options::recursive);
  writeText(visiblePackage / "entry/play.luaskin", "return { live = true }\n");
  writeText(visiblePackage / "entry/direct.lua", "return 'visible'\n");
  writeText(visiblePackage / "entry/choice.lua", "return 'choice'\n");
  writeText(visiblePackage / "entry/binary.lua", std::string("\x1bLua", 4));

  auto created = fixture.create(fixture.entry, true);
  expect(created.fileSystem != nullptr,
         "a live visible-package filesystem is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;

  const auto entry = fileSystem.readEntry(4096);
  expect(!entry.failure && bytesToString(entry.bytes) == "return { live = true }\n",
         "entry execution reads the visible package rather than its snapshot");
  const auto module = fileSystem.readModule("direct", 4096);
  expect(!module.failure && bytesToString(module.bytes) == "return 'visible'\n",
         "module execution observes Files-app edits without rebuilding a snapshot");
  const auto bytecode = fileSystem.read("binary.lua", SkinFileUse::LuaModule,
                                        4096);
  expect(!bytecode.failure && bytesToString(bytecode.bytes) ==
                                 std::string("\x1bLua", 4),
         "Lua bytecode is not rejected by an AsoBMaShow-only file boundary");

  const auto regexListed =
      fileSystem.list(".", "(direct|choice)%.lua", 1);
  expect(!regexListed.failure && regexListed.entries.size() == 2 &&
             std::ranges::find(regexListed.entries, "direct.lua") !=
                 regexListed.entries.end() &&
             std::ranges::find(regexListed.entries, "choice.lua") !=
                 regexListed.entries.end(),
         "file listing keeps Beatoraja's unrestricted regex and ignores the "
         "former caller entry cap");

  writeText(fixture.roots.visiblePackages / "Hub/const.lua",
            "return { source = 'shared-skins-root' }\n");
  const auto sharedModule =
      fileSystem.readLuaPath("skin/Hub/const.lua", 4096);
  expect(!sharedModule.failure &&
             bytesToString(sharedModule.bytes) ==
                 "return { source = 'shared-skins-root' }\n",
         "Beatoraja skin-prefixed package paths search the shared Skins root");

  const fs::path absoluteInside = visiblePackage / "entry/direct.lua";
  const auto acceptedAbsolute =
      fileSystem.resolve(absoluteInside.string(), SkinFileUse::LuaModule);
  expect(!acceptedAbsolute.failure && acceptedAbsolute.normalizedVirtualPath &&
             *acceptedAbsolute.normalizedVirtualPath ==
                 absoluteInside.lexically_normal().generic_string(),
         "absolute Lua paths inside the selected skin directory are accepted");

  const fs::path outside = fixture.temp.root() / "outside-resource.bin";
  writeText(outside, "outside-resource");
  const auto resource = fileSystem.resolveResourceCandidates(outside.string(),
                                                              outside.string());
  expect(!resource.failure && resource.normalizedVirtualPath &&
             *resource.normalizedVirtualPath ==
                 outside.lexically_normal().generic_string(),
         "resource paths keep Beatoraja's normal absolute-path resolution");
  if (resource.normalizedVirtualPath) {
    const auto bytes =
        fileSystem.readResolvedResource(*resource.normalizedVirtualPath, 4096);
    expect(!bytes.failure && bytesToString(bytes.bytes) == "outside-resource",
           "resource reads follow the resolved normal filesystem path");
  }

  const auto write =
      fileSystem.writeData("History/260805/history.txt", bytesOf("record\n"),
                           false);
  const fs::path history = visiblePackage / "entry/History/260805/history.txt";
  expect(!write.failure && readText(history) == "record\n",
         "Lua data writes directly modify the visible skin directory");

  const fs::path otherPackage = fixture.roots.visiblePackages / "OtherSkin";
  writeText(otherPackage / "state.txt", "other package state\n");
  const auto foreignWrite = fileSystem.writeData(
      "skin/OtherSkin/state.txt", bytesOf("must not cross package\n"),
      false);
  expect(foreignWrite.failure &&
             foreignWrite.failure->code == SkinFileError::EscapesPackage &&
             readText(otherPackage / "state.txt") == "other package state\n",
         "skin-prefixed Lua writes remain confined to the selected package");
}

void testReadOnlyFilesystemRejectsDataWrites() {
  PackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  auto created = fixture.create(fixture.entry, false);
  expect(created.fileSystem != nullptr,
         "a read-only filesystem is created for catalog work");
  if (!created.fileSystem) {
    return;
  }

  const auto write = created.fileSystem->writeData(
      "History/260805/history.txt", bytesOf("must not be written\n"), false);
  expect(write.failure && write.failure->code == SkinFileError::WrongUse,
         "a catalog filesystem refuses a Lua data write");
}

void testUnrestrictedFilesystemLiftsWriteAndContainmentGuards() {
  PackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  auto created = fixture.create(fixture.entry, false,
                                SkinSafetyLevel::Unrestricted);
  expect(created.fileSystem != nullptr,
         "an unrestricted filesystem is created for catalog work");
  if (!created.fileSystem) {
    return;
  }

  const fs::path outside = fixture.temp.root() / "outside" / "state.txt";
  const auto write = created.fileSystem->writeData(
      outside.string(), bytesOf("unrestricted write\n"), false);
  expect(!write.failure && readText(outside) == "unrestricted write\n",
         "Unrestricted permits catalog writes outside the selected package");
}

void testCompatibilityFilesystemLiftsOnlyProtectiveWriteGuard() {
  PackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  const fs::path visiblePackage =
      fixture.roots.visiblePackages / fixture.package.directoryName;
  fs::create_directories(visiblePackage.parent_path());
  fs::copy(fixture.temp.root() / "source", visiblePackage,
           fs::copy_options::recursive);

  auto created = fixture.create(fixture.entry, false,
                                SkinSafetyLevel::BeatorajaCompatibility);
  expect(created.fileSystem != nullptr,
         "a compatibility filesystem is created for catalog work");
  if (!created.fileSystem) {
    return;
  }

  const auto inside = created.fileSystem->writeData(
      "History/compatibility.txt", bytesOf("compatible write\n"), false);
  expect(!inside.failure &&
             readText(created.fileSystem->skinDirectory() /
                      "History/compatibility.txt") == "compatible write\n",
         "Beatoraja compatibility permits catalog writes in the skin package");

  const fs::path outside = fixture.temp.root() / "outside" / "state.txt";
  const auto escaped = created.fileSystem->writeData(
      outside.string(), bytesOf("must remain contained\n"), false);
  expect(escaped.failure && escaped.failure->code == SkinFileError::EscapesPackage,
         "Beatoraja compatibility retains catastrophic path containment");
}

void testLinkedVisibleSkinChildCannotEscapeLuaIoBoundary() {
  PackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  const fs::path visiblePackage =
      fixture.roots.visiblePackages / fixture.package.directoryName;
  fs::create_directories(visiblePackage.parent_path());
  fs::copy(fixture.temp.root() / "source", visiblePackage,
           fs::copy_options::recursive);

  const fs::path outside = fixture.temp.root() / "outside";
  writeText(outside / "secret.lua", "return 'outside'\n");
  std::error_code linkError;
  fs::create_directory_symlink(outside, visiblePackage / "entry/escape",
                               linkError);
  expect(!linkError, "visible skin symlink fixture creates");
  if (linkError) {
    return;
  }

  auto created = fixture.create(fixture.entry, true);
  expect(created.fileSystem != nullptr,
         "filesystem creates before linked child access");
  if (!created.fileSystem) {
    return;
  }

  const auto read =
      created.fileSystem->readLuaPath("escape/secret.lua", 4096);
  expect(read.failure && read.failure->code == SkinFileError::EscapesPackage,
         "Lua reads reject linked children that escape the skin directory");

  const auto write = created.fileSystem->writeData(
      "escape/secret.lua", bytesOf("mutated\n"), false);
  expect(write.failure &&
             write.failure->code == SkinFileError::EscapesPackage &&
             readText(outside / "secret.lua") == "return 'outside'\n",
         "Lua writes cannot follow a linked child outside the skin directory");
}

} // namespace

int main() {
  testBeatorajaDirectSkinDirectorySemantics();
  testReadOnlyFilesystemRejectsDataWrites();
  testCompatibilityFilesystemLiftsOnlyProtectiveWriteGuard();
  testUnrestrictedFilesystemLiftsWriteAndContainmentGuards();
  testLinkedVisibleSkinChildCannotEscapeLuaIoBoundary();
  testCompatibilityDiagnosticsDeduplicateAndRetainCriticality();
  if (failures != 0) {
    std::cerr << failures << " lua skin filesystem test(s) failed\n";
    return 1;
  }
  std::cout << "lua skin filesystem tests passed\n";
  return 0;
}
