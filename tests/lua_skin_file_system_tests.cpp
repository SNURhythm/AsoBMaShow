#include "FileChecksum.h"
#include "skin/SkinProfileSettings.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/SkinCompatibilityDiagnostics.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

void enableAdversarialStagingMutation(const fs::path &directory) {
  // Prepared revisions are normally consumed unchanged. These security tests
  // deliberately forge nodes after snapshotting to prove every host call
  // rechecks the opened target rather than trusting preparation alone.
  std::error_code error;
  fs::permissions(directory, fs::perms::owner_write, fs::perm_options::add,
                  error);
  expect(!error, "adversarial fixture directory becomes test-writable");
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

SkinProfileId profile(std::string_view id) { return *makeSkinProfileId(id); }

class PackageFixture {
public:
  PackageFixture()
      : roots(rootsBelow(temp.root())),
        package(*normalizePackageId("FixtureSkin").package),
        entry(entryId(package, "entry/play.luaskin")),
        alternateEntry(entryId(package, "alternate/play.luaskin")) {
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

  LuaSkinFileSystemCreateResult
  create(const SkinEntryId &selectedEntry,
         std::optional<SkinProfileId> selectedProfile, bool writes,
         SkinDataOverlayPolicy policy = {}) {
    return LuaSkinFileSystem::create({.revision = prepared->readView(),
                                      .entry = selectedEntry,
                                      .storageRoots = roots,
                                      .profileId = std::move(selectedProfile),
                                      .allowDataWrites = writes,
                                      .dataPolicy = policy});
  }

  fs::path overlayRoot(const SkinEntryId &selectedEntry,
                       const SkinProfileId &selectedProfile) const {
    return *deriveSkinPrivateOverlayRoot(roots, selectedProfile, selectedEntry)
                .root;
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  SkinEntryId entry;
  SkinEntryId alternateEntry;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

bool failedWith(const std::optional<SkinFileFailure> &failure,
                SkinFileError error) {
  return failure && failure->code == error;
}

void expectNoHostPath(const SkinFileFailure &failure,
                      const PackageFixture &fixture, std::string_view message) {
  expect(failure.virtualPath.find(fixture.temp.root().string()) ==
                 std::string::npos &&
             failure.message.find(fixture.temp.root().string()) ==
                 std::string::npos,
         message);
}

std::string overlayDigest(const fs::path &root) {
  if (!fs::exists(root)) {
    return file_checksum::sha256("missing");
  }
  std::vector<std::string> records;
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    const auto status = entry.symlink_status();
    const std::string relative =
        entry.path().lexically_relative(root).generic_string();
    if (fs::is_directory(status)) {
      records.push_back("d:" + relative);
    } else if (fs::is_regular_file(status)) {
      records.push_back("f:" + relative + ":" + readText(entry.path()));
    } else {
      records.push_back("x:" + relative);
    }
  }
  std::ranges::sort(records);
  std::string framed;
  for (const std::string &record : records) {
    framed += std::to_string(record.size()) + ":" + record;
  }
  return file_checksum::sha256(framed);
}

void testWorkingDirectoryPackageCeilingAndModuleSearch() {
  PackageFixture fixture;
  auto created = fixture.create(fixture.entry, std::nullopt, false);
  expect(created.fileSystem != nullptr,
         "read-only validation filesystem is created without a profile");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;

  const auto selectedEntry = fileSystem.readEntry(4096);
  expect(!selectedEntry.failure &&
             bytesToString(selectedEntry.bytes).find(
                 "Task 8 virtual filesystem fixture") != std::string::npos,
         "the selected entry is read directly from its typed identity");

  const auto entry = fileSystem.resolve("play.luaskin", SkinFileUse::LuaEntry);
  expect(entry.normalizedVirtualPath == "entry/play.luaskin" && !entry.failure,
         "entry references start at the entry parent");

  const auto parent =
      fileSystem.resolve("../shared.lua", SkinFileUse::LuaModule);
  expect(parent.normalizedVirtualPath == "shared.lua" && !parent.failure,
         "a normalized parent reference may remain within the package");

  const auto direct = fileSystem.resolveModule("direct");
  const auto fallback = fileSystem.resolveModule("fallback");
  const auto shared = fileSystem.resolveModule("../shared");
  expect(direct.normalizedVirtualPath == "entry/direct.lua",
         "module resolution checks ?.lua first");
  expect(fallback.normalizedVirtualPath == "entry/fallback/init.lua",
         "module resolution checks ?/init.lua second");
  expect(shared.normalizedVirtualPath == "shared.lua",
         "module resolution permits a shared parent inside one package");
  const auto directBytes = fileSystem.readModule("direct", 1024);
  expect(bytesToString(directBytes.bytes) == "return 'direct'\n",
         "module reads use the deterministic first candidate");

  for (const std::string &unsafe :
       {"../../Sibling/secret.lua", "/etc/passwd", "C:/secret.lua",
        "//server/share.lua", "..\\secret.lua"}) {
    const auto denied = fileSystem.resolve(unsafe, SkinFileUse::Resource);
    expect(!denied.normalizedVirtualPath && denied.failure &&
               (denied.failure->code == SkinFileError::InvalidPath ||
                denied.failure->code == SkinFileError::EscapesPackage),
           "absolute, sibling, and parent escapes are rejected");
    if (denied.failure) {
      expectNoHostPath(*denied.failure, fixture,
                       "path failures never disclose a host path");
    }
  }
  for (const std::string &windowsAlias :
       {"state.txt:stream", "NUL.txt", "trailing. ", "wild?.lua"}) {
    const auto denied = fileSystem.resolve(windowsAlias, SkinFileUse::Resource);
    expect(failedWith(denied.failure, SkinFileError::InvalidPath),
           "Windows alternate names and reserved components are rejected");
  }

  auto forgedEntry = fixture.entry;
  forgedEntry.package = *normalizePackageId("OtherPackage").package;
  const auto mismatch = fixture.create(forgedEntry, std::nullopt, false);
  expect(!mismatch.fileSystem && mismatch.failure,
         "entry and revision package identities must match");
  const auto aliasedEntry =
      normalizeEntryPath(fixture.entry.package, "entry/NUL.luaskin");
  const auto aliased = fixture.create(*aliasedEntry.entry, std::nullopt, false);
  expect(!aliased.fileSystem &&
             failedWith(aliased.failure, SkinFileError::InvalidPath),
         "selected entries with Windows namespace aliases are rejected");
}

void testNoFollowReadsAndOverlayUseSeparation() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("11111111-1111-4111-8111-111111111111");
  auto created = fixture.create(fixture.entry, selectedProfile, true);
  expect(created.fileSystem != nullptr,
         "profile filesystem with writes is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;

  const auto binaryModule =
      fileSystem.read("binary.lua", SkinFileUse::LuaModule, 1024);
  expect(failedWith(binaryModule.failure, SkinFileError::BinaryChunk),
         "binary Lua chunks are rejected");
  const auto binaryResource =
      fileSystem.read("binary.lua", SkinFileUse::Resource, 1024);
  expect(bytesToString(binaryResource.bytes) == std::string("\x1bLua", 4),
         "binary resource bytes remain readable");
  const auto limited = fileSystem.read("lines.txt", SkinFileUse::DataRead, 3);
  expect(failedWith(limited.failure, SkinFileError::LimitExceeded),
         "read byte limits are enforced before allocation");
  const auto directory =
      fileSystem.read("resources", SkinFileUse::Resource, 1024);
  expect(failedWith(directory.failure, SkinFileError::NonRegular),
         "directories are never accepted as files");

  const auto firstWrite =
      fileSystem.writeData("data/state.txt", bytesOf("overlay-state"), false);
  expect(!firstWrite.failure && firstWrite.resultingFiles == 1 &&
             firstWrite.resultingBytes == 13,
         "the first overlay replacement reports exact quota usage");
  const auto dataRead =
      fileSystem.read("data/state.txt", SkinFileUse::DataRead, 1024);
  const auto resourceRead =
      fileSystem.read("data/state.txt", SkinFileUse::Resource, 1024);
  const auto moduleRead =
      fileSystem.read("../shared.lua", SkinFileUse::LuaModule, 1024);
  expect(bytesToString(dataRead.bytes) == "overlay-state",
         "generic data reads prefer the private overlay");
  expect(bytesToString(resourceRead.bytes) == "package-state",
         "resource reads bypass the private overlay");
  expect(bytesToString(moduleRead.bytes) ==
             "return \"package-shared-module\"\n",
         "module reads bypass the private overlay");
  expect(readText(fixture.prepared->stagingRoot() / "entry/data/state.txt") ==
             "package-state",
         "overlay writes never mutate the immutable package view");

  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
#if !defined(_WIN32)
  fs::create_directories(overlay / "entry/data");
  writeText(overlay / "entry/data/state.txt.tmp", "stale-temp");
  fs::permissions(overlay / "entry/data/state.txt.tmp",
                  fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);
  const auto staleTempWrite =
      fileSystem.writeData("data/state.txt", bytesOf("new-state"), false);
  expect(!staleTempWrite.failure &&
             readText(overlay / "entry/data/state.txt.tmp") == "stale-temp",
         "a stale predictable temp name cannot permanently deny replacement");
#endif

#if !defined(_WIN32)
  const fs::path outside = fixture.temp.root() / "outside.txt";
  writeText(outside, "outside");
  const fs::path outsideOverlay = fixture.temp.root() / "outside-overlay";
  fs::create_directories(outsideOverlay);
  auto linkedRoots = fixture.roots;
  linkedRoots.profileOverlays = fixture.temp.root() / "overlay-root-link";
  fs::create_directory_symlink(outsideOverlay, linkedRoots.profileOverlays);
  const auto linkedOverlayRoot =
      LuaSkinFileSystem::create({.revision = fixture.prepared->readView(),
                                 .entry = fixture.entry,
                                 .storageRoots = linkedRoots,
                                 .profileId = selectedProfile,
                                 .allowDataWrites = true});
  expect(!linkedOverlayRoot.fileSystem && linkedOverlayRoot.failure,
         "a symlink supplied as the private overlay ceiling is rejected");

  enableAdversarialStagingMutation(fixture.prepared->stagingRoot() / "entry");
  const fs::path packageLink =
      fixture.prepared->stagingRoot() / "entry/package-link.txt";
  fs::create_symlink(outside, packageLink);
  const auto linkedRead =
      fileSystem.read("package-link.txt", SkinFileUse::DataRead, 1024);
  expect(failedWith(linkedRead.failure, SkinFileError::NonRegular),
         "package symlinks are rejected without following them");

  const fs::path packageHardLink =
      fixture.prepared->stagingRoot() / "entry/package-hard-link.txt";
  fs::create_hard_link(outside, packageHardLink);
  const auto hardLinkedRead =
      fileSystem.read("package-hard-link.txt", SkinFileUse::DataRead, 1024);
  expect(failedWith(hardLinkedRead.failure, SkinFileError::NonRegular),
         "multiply-linked package files are rejected");

  fs::create_directories(overlay / "entry");
  const fs::path overlayLink = overlay / "entry/overlay-link.txt";
  fs::create_symlink(outside, overlayLink);
  const auto linkedWrite =
      fileSystem.writeData("overlay-link.txt", bytesOf("do-not-follow"), false);
  expect(failedWith(linkedWrite.failure, SkinFileError::NonRegular),
         "overlay symlink targets are rejected before replacement");
  expect(readText(outside) == "outside",
         "a rejected overlay symlink never mutates its target");

  const fs::path overlayHardLink = overlay / "entry/overlay-hard-link.txt";
  fs::create_hard_link(outside, overlayHardLink);
  const auto hardLinkedWrite = fileSystem.writeData(
      "overlay-hard-link.txt", bytesOf("do-not-replace"), false);
  expect(failedWith(hardLinkedWrite.failure, SkinFileError::NonRegular),
         "multiply-linked overlay files are rejected");
#endif
}

void testAtomicWritesNestedParentsAndQuotaRollback() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("22222222-2222-4222-8222-222222222222");
  auto created = fixture.create(fixture.entry, selectedProfile, true,
                                {.maximumBytes = 5, .maximumFiles = 1});
  expect(created.fileSystem != nullptr, "bounded overlay is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;

  const auto nested =
      fileSystem.writeData("fresh/deep/value.txt", bytesOf("abc"), false);
  expect(!nested.failure && nested.resultingBytes == 3 &&
             nested.resultingFiles == 1,
         "nested write safely creates missing overlay parents");
  const auto appended =
      fileSystem.writeData("fresh/deep/value.txt", bytesOf("de"), true);
  expect(!appended.failure && appended.resultingBytes == 5 &&
             appended.resultingFiles == 1,
         "append atomically extends the existing logical data");
  const auto byteQuota =
      fileSystem.writeData("fresh/deep/value.txt", bytesOf("f"), true);
  expect(failedWith(byteQuota.failure, SkinFileError::QuotaExceeded) &&
             byteQuota.resultingBytes == 5 && byteQuota.resultingFiles == 1,
         "byte quota failure reports unchanged exact usage");
  const auto fileQuota = fileSystem.writeData("second.txt", bytesOf(""), false);
  expect(failedWith(fileQuota.failure, SkinFileError::QuotaExceeded) &&
             fileQuota.resultingBytes == 5 && fileQuota.resultingFiles == 1,
         "an empty second file still consumes the file quota");

  const auto data =
      fileSystem.read("fresh/deep/value.txt", SkinFileUse::DataRead, 16);
  expect(bytesToString(data.bytes) == "abcde",
         "quota failures leave the prior atomic value intact");
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  expect(!fs::exists(overlay / "entry/fresh/deep/value.txt.tmp"),
         "atomic replacement leaves no temporary artifact");

  const auto cleared =
      fileSystem.writeData("fresh/deep/value.txt", bytesOf(""), false);
  expect(!cleared.failure && cleared.resultingBytes == 0 &&
             cleared.resultingFiles == 1,
         "truncate/clear preserves the file count and resets byte usage");
  const auto mkdirResult = fileSystem.mkdirData("cache");
  expect(!mkdirResult.failure && mkdirResult.resultingBytes == 0 &&
             mkdirResult.resultingFiles == 1 &&
             fs::is_directory(overlay / "entry/cache"),
         "legacy-compatible mkdir creates one overlay-only directory");

  const auto noProfile = fixture.create(fixture.entry, std::nullopt, true);
  expect(!noProfile.fileSystem && noProfile.failure &&
             noProfile.failure->code == SkinFileError::WrongUse,
         "data writes cannot be enabled without a typed profile");
  const auto readOnly =
      fixture.create(fixture.alternateEntry, std::nullopt, false);
  expect(readOnly.fileSystem != nullptr,
         "catalog/default validation remains overlay-free");
  if (readOnly.fileSystem) {
    const auto denied =
        readOnly.fileSystem->writeData("value.txt", bytesOf("x"), false);
    expect(failedWith(denied.failure, SkinFileError::WrongUse),
           "read-only validation cannot create an overlay");
  }
}

void testOverlayWorkIsBoundedBeforeMutation() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("88888888-8888-4888-8888-888888888888");
  auto created = fixture.create(fixture.entry, selectedProfile, true,
                                {.maximumBytes = 5, .maximumFiles = 1});
  expect(created.fileSystem != nullptr, "bounded-work fixture is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);

  const std::vector<std::byte> oversized(1024 * 1024, std::byte{'x'});
  const auto oversizedWrite =
      fileSystem.writeData("oversized.bin", oversized, false);
  expect(failedWith(oversizedWrite.failure, SkinFileError::QuotaExceeded) &&
             !fs::exists(overlay),
         "an oversized payload is rejected before overlay creation or copy");

#if !defined(_WIN32)
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  void *guardedPayload =
      ::mmap(nullptr, static_cast<std::size_t>(pageSize),
             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  expect(guardedPayload != MAP_FAILED,
         "oversized payload guard page is allocated");
  if (guardedPayload != MAP_FAILED) {
    std::fill_n(static_cast<std::byte *>(guardedPayload), pageSize,
                std::byte{0});
    expect(::mprotect(guardedPayload, static_cast<std::size_t>(pageSize),
                      PROT_NONE) == 0,
           "oversized payload guard page becomes inaccessible");
    const pid_t child = ::fork();
    if (child == 0) {
      const auto guarded = fileSystem.writeData(
          "guarded.bin",
          std::span(static_cast<const std::byte *>(guardedPayload),
                    static_cast<std::size_t>(pageSize)),
          false);
      ::_exit(failedWith(guarded.failure, SkinFileError::QuotaExceeded) ? 0
                                                                        : 1);
    }
    int status = 0;
    expect(child > 0 && ::waitpid(child, &status, 0) == child &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "oversized payload bytes are never touched before quota rejection");
    ::munmap(guardedPayload, static_cast<std::size_t>(pageSize));
  }

  fs::create_directories(overlay);
  fs::permissions(overlay, fs::perms::owner_all, fs::perm_options::replace);
  for (int index = 0; index < 1'025; ++index) {
    const fs::path directory = overlay / ("directory-" + std::to_string(index));
    fs::create_directory(directory);
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
  }
  const auto wide = fileSystem.writeData("wide.txt", bytesOf("x"), false);
  expect(failedWith(wide.failure, SkinFileError::QuotaExceeded) &&
             !fs::exists(overlay / "entry/wide.txt"),
         "overlay directory growth is bounded independently of file quota");

  fs::remove_all(overlay);
  fs::create_directories(overlay);
  fs::permissions(overlay, fs::perms::owner_all, fs::perm_options::replace);
  fs::path deep = overlay;
  for (int depth = 0; depth < 65; ++depth) {
    deep /= "d";
    fs::create_directory(deep);
    fs::permissions(deep, fs::perms::owner_all, fs::perm_options::replace);
  }
  const auto tooDeep =
      fileSystem.writeData("deep-limit.txt", bytesOf("x"), false);
  expect(failedWith(tooDeep.failure, SkinFileError::QuotaExceeded) &&
             !fs::exists(overlay / "entry/deep-limit.txt"),
         "overlay traversal stops at the fixed recursion-depth limit");
#endif

  enableAdversarialStagingMutation(fixture.prepared->stagingRoot() / "entry");
  const fs::path huge = fixture.prepared->stagingRoot() / "entry/huge";
  fs::create_directory(huge);
  for (int index = 0; index < 3'000; ++index) {
    writeText(huge / ("entry-" + std::to_string(index) + ".txt"), "x");
  }
  const auto hugeList = fileSystem.list("huge", "", 8);
  expect(failedWith(hugeList.failure, SkinFileError::LimitExceeded) &&
             hugeList.entries.empty(),
         "a huge directory listing stops at its caller limit without a partial "
         "result");

#if !defined(_WIN32)
  const fs::path poison = huge / "listing-poison";
  fs::create_symlink(fixture.temp.root() / "outside-list-target", poison);
  std::size_t poisonPosition = 0;
  std::size_t position = 0;
  for (const auto &entry : fs::directory_iterator(huge)) {
    ++position;
    if (entry.path().filename() == poison.filename()) {
      poisonPosition = position;
      break;
    }
  }
  expect(poisonPosition > 2, "listing poison follows bounded regular entries");
  if (poisonPosition > 2) {
    const auto stoppedBeforePoison =
        fileSystem.list("huge", "", poisonPosition - 2);
    expect(
        failedWith(stoppedBeforePoison.failure, SkinFileError::LimitExceeded),
        "listing stops at maximumEntries plus one before inspecting later "
        "children");
  }
#endif
}

void testInternalTemporaryNamespaceAndRecovery() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("99999999-9999-4999-8999-999999999999");
  auto created = fixture.create(fixture.entry, selectedProfile, true);
  expect(created.fileSystem != nullptr,
         "temporary recovery fixture is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;
  const auto reserved = fileSystem.resolve(
      ".asobmashow-internal/write-0123456789abcdef0123456789abcdef.tmp",
      SkinFileUse::DataWrite);
  const auto reservedCaseAlias = fileSystem.resolve(
      ".ASOBMASHOW-INTERNAL/write-0123456789abcdef0123456789abcdef.tmp",
      SkinFileUse::DataWrite);
  expect(failedWith(reserved.failure, SkinFileError::InvalidPath) &&
             failedWith(reservedCaseAlias.failure, SkinFileError::InvalidPath),
         "authored paths cannot enter the internal temporary namespace");

#if !defined(_WIN32)
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  fs::create_directories(overlay / ".asobmashow-internal");
  fs::permissions(overlay, fs::perms::owner_all, fs::perm_options::replace);
  fs::permissions(overlay / ".asobmashow-internal", fs::perms::owner_all,
                  fs::perm_options::replace);
  const fs::path stale = overlay / ".asobmashow-internal" /
                         "write-0123456789abcdef0123456789abcdef.tmp";
  writeText(stale, "abandoned");
  fs::permissions(stale, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);
  const auto recovered =
      fileSystem.writeData("recovered.txt", bytesOf("recovered"), false);
  expect(!recovered.failure && !fs::exists(stale),
         "an exactly named abandoned owned temporary is recovered before "
         "mutation");

  writeText(stale, "undeletable");
  fs::permissions(stale, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);
  fs::permissions(overlay / ".asobmashow-internal",
                  fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
  const auto cleanupDenied =
      fileSystem.writeData("cleanup-denied.txt", bytesOf("x"), false);
  expect(cleanupDenied.failure &&
             !fs::exists(overlay / "entry/cleanup-denied.txt"),
         "a failed abandoned-temporary deletion fails closed");
  fs::permissions(overlay / ".asobmashow-internal", fs::perms::owner_all,
                  fs::perm_options::replace);
#endif
}

void testExistingOverlayPrivacyIsValidated() {
#if !defined(_WIN32)
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  fs::create_directories(overlay);
  fs::permissions(overlay,
                  fs::perms::owner_all | fs::perms::group_read |
                      fs::perms::others_read,
                  fs::perm_options::replace);
  const auto publicRoot = fixture.create(fixture.entry, selectedProfile, true);
  expect(!publicRoot.fileSystem &&
             failedWith(publicRoot.failure, SkinFileError::NonRegular),
         "construction rejects an existing non-private overlay root");

  fs::permissions(overlay, fs::perms::owner_all, fs::perm_options::replace);
  auto created = fixture.create(fixture.entry, selectedProfile, true);
  expect(created.fileSystem != nullptr,
         "construction accepts an owner-private overlay root");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;
  expect(
      !fileSystem.writeData("private.txt", bytesOf("private"), false).failure,
      "private overlay fixture writes its initial file");
  const fs::path privateFile = overlay / "entry/private.txt";
  fs::permissions(privateFile,
                  fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::group_read,
                  fs::perm_options::replace);
  const auto fileDrift =
      fileSystem.read("private.txt", SkinFileUse::DataRead, 64);
  expect(failedWith(fileDrift.failure, SkinFileError::NonRegular),
         "overlay reads reject file privacy drift");
  fs::permissions(privateFile, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);
  fs::permissions(overlay, fs::perms::owner_all | fs::perms::group_read,
                  fs::perm_options::replace);
  const auto rootDrift =
      fileSystem.read("private.txt", SkinFileUse::DataRead, 64);
  expect(failedWith(rootDrift.failure, SkinFileError::NonRegular),
         "overlay reads reject root privacy drift after construction");
  fs::permissions(overlay, fs::perms::owner_all, fs::perm_options::replace);
#endif
}

void testPosixMutationPinsSurviveRenameSwaps() {
#if !defined(_WIN32)
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
  auto created = fixture.create(fixture.entry, selectedProfile, true);
  expect(created.fileSystem != nullptr, "rename-swap fixture is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;
  expect(!fileSystem.writeData("data/seed.txt", bytesOf("seed"), false).failure,
         "rename-swap fixture creates its overlay root");
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  for (int index = 0; index < 1'000; ++index) {
    const fs::path filler = overlay / ("filler-" + std::to_string(index));
    writeText(filler, "x");
    fs::permissions(filler, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
  }

  const fs::path parkedRoot = fixture.temp.root() / "parked-overlay";
  const fs::path attackerRoot = fixture.temp.root() / "attacker-overlay";
  fs::create_directories(attackerRoot / "entry/data");
  fs::permissions(attackerRoot, fs::perms::all, fs::perm_options::replace);
  fs::permissions(attackerRoot / "entry", fs::perms::all,
                  fs::perm_options::replace);
  fs::permissions(attackerRoot / "entry/data", fs::perms::all,
                  fs::perm_options::replace);
  std::barrier rootStart(2);
  std::thread rootWriter([&] {
    rootStart.arrive_and_wait();
    (void)fileSystem.writeData("data/root-swap.txt", bytesOf("redirect"),
                               false);
  });
  rootStart.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  fs::rename(overlay, parkedRoot);
  fs::rename(attackerRoot, overlay);
  rootWriter.join();
  fs::rename(overlay, attackerRoot);
  fs::rename(parkedRoot, overlay);
  expect(!fs::exists(attackerRoot / "entry/data/root-swap.txt"),
         "renaming and replacing the overlay root cannot redirect a mutation");

  const fs::path parkedEntry = fixture.temp.root() / "parked-entry";
  const fs::path attackerEntry = fixture.temp.root() / "attacker-entry";
  fs::create_directories(attackerEntry / "data");
  fs::permissions(attackerEntry, fs::perms::all, fs::perm_options::replace);
  fs::permissions(attackerEntry / "data", fs::perms::all,
                  fs::perm_options::replace);
  std::barrier ancestorStart(2);
  std::thread ancestorWriter([&] {
    ancestorStart.arrive_and_wait();
    (void)fileSystem.writeData("data/ancestor-swap.txt", bytesOf("redirect"),
                               false);
  });
  ancestorStart.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  fs::rename(overlay / "entry", parkedEntry);
  fs::rename(attackerEntry, overlay / "entry");
  ancestorWriter.join();
  fs::rename(overlay / "entry", attackerEntry);
  fs::rename(parkedEntry, overlay / "entry");
  expect(!fs::exists(attackerEntry / "data/ancestor-swap.txt"),
         "renaming and replacing a target ancestor cannot redirect a mutation");
#endif
}

void testDeterministicListingAndProfileEntryIsolation() {
  PackageFixture fixture;
  const SkinProfileId first = profile("33333333-3333-4333-8333-333333333333");
  const SkinProfileId second = profile("44444444-4444-4444-8444-444444444444");

  auto firstEntryFirstProfile = fixture.create(fixture.entry, first, true);
  auto firstEntrySecondProfile = fixture.create(fixture.entry, second, true);
  auto secondEntryFirstProfile =
      fixture.create(fixture.alternateEntry, first, true);
  auto secondEntrySecondProfile =
      fixture.create(fixture.alternateEntry, second, true);
  expect(firstEntryFirstProfile.fileSystem &&
             firstEntrySecondProfile.fileSystem &&
             secondEntryFirstProfile.fileSystem &&
             secondEntrySecondProfile.fileSystem,
         "two profiles by two entries create four isolated filesystems");
  if (!firstEntryFirstProfile.fileSystem ||
      !firstEntrySecondProfile.fileSystem ||
      !secondEntryFirstProfile.fileSystem ||
      !secondEntrySecondProfile.fileSystem) {
    return;
  }

  const std::vector<std::pair<LuaSkinFileSystem *, std::string>> writers{
      {firstEntryFirstProfile.fileSystem.get(), "entry-profile-one"},
      {firstEntrySecondProfile.fileSystem.get(), "entry-profile-two"},
      {secondEntryFirstProfile.fileSystem.get(), "alternate-profile-one"},
      {secondEntrySecondProfile.fileSystem.get(), "alternate-profile-two"},
  };
  for (const auto &[fileSystem, value] : writers) {
    const auto result =
        fileSystem->writeData("same.txt", bytesOf(value), false);
    expect(!result.failure, "isolated overlay write succeeds");
  }
  for (const auto &[fileSystem, value] : writers) {
    const auto result = fileSystem->read("same.txt", SkinFileUse::DataRead, 64);
    expect(bytesToString(result.bytes) == value,
           "overlay reads do not cross profile or entry identity");
  }

  const auto all = firstEntryFirstProfile.fileSystem->list(".", "", 32);
  expect(!all.failure && std::ranges::is_sorted(all.entries) &&
             !all.entries.empty(),
         "direct-child listing is deterministic and sorted");
  for (const std::string &entry : all.entries) {
    expect(entry.starts_with("entry/") &&
               entry.find(fixture.temp.root().string()) == std::string::npos,
           "directory listing returns package virtual paths only");
  }
  const auto luaOnly =
      firstEntryFirstProfile.fileSystem->list(".", "%.lua$", 32);
  expect(!luaOnly.failure && !luaOnly.entries.empty() &&
             std::ranges::all_of(luaOnly.entries,
                                 [](const std::string &path) {
                                   return path.ends_with(".lua");
                                 }),
         "Lua-pattern filtering keeps matching virtual entries");
  const auto limited = firstEntryFirstProfile.fileSystem->list(".", "", 1);
  expect(failedWith(limited.failure, SkinFileError::LimitExceeded) &&
             limited.entries.empty(),
         "listing fails without a partial result at the entry limit");
  const std::string oversizedPattern(SkinPackagePolicy::maxPathBytes + 1, 'a');
  const auto oversized =
      firstEntryFirstProfile.fileSystem->list(".", oversizedPattern, 32);
  expect(failedWith(oversized.failure, SkinFileError::LimitExceeded) &&
             oversized.entries.empty(),
         "listing rejects an oversized Lua pattern without a partial result");
  const auto backtracking =
      firstEntryFirstProfile.fileSystem->list(".", "(a+)+$", 32);
  expect(failedWith(backtracking.failure, SkinFileError::InvalidPath) &&
             backtracking.entries.empty(),
         "listing rejects patterns outside the deterministic Lua subset");

#if !defined(_WIN32)
  const fs::path outside = fixture.temp.root() / "list-outside";
  fs::create_directories(outside);
  enableAdversarialStagingMutation(fixture.prepared->stagingRoot() / "entry");
  fs::create_directory_symlink(outside, fixture.prepared->stagingRoot() /
                                            "entry/list-link");
  const auto hostile = firstEntryFirstProfile.fileSystem->list(".", "", 32);
  expect(failedWith(hostile.failure, SkinFileError::NonRegular),
         "listing rejects a symlink child instead of following it");
#endif
}

void testConcurrentWritesCannotOversubscribeQuota() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("66666666-6666-4666-8666-666666666666");
  auto created = fixture.create(fixture.entry, selectedProfile, true,
                                {.maximumBytes = 64, .maximumFiles = 1});
  expect(created.fileSystem != nullptr, "concurrent quota fixture is created");
  if (!created.fileSystem) {
    return;
  }

  constexpr int writerCount = 8;
  std::barrier start(writerCount + 1);
  std::atomic_int successes = 0;
  std::vector<std::thread> writers;
  writers.reserve(writerCount);
  for (int index = 0; index < writerCount; ++index) {
    writers.emplace_back([&, index] {
      start.arrive_and_wait();
      const std::string path = "concurrent-" + std::to_string(index) + ".txt";
      const auto result =
          created.fileSystem->writeData(path, bytesOf("x"), false);
      if (!result.failure) {
        successes.fetch_add(1, std::memory_order_relaxed);
      } else {
        expect(result.failure->code == SkinFileError::QuotaExceeded,
               "losing concurrent writes fail only with quota exhaustion");
      }
    });
  }
  start.arrive_and_wait();
  for (std::thread &writer : writers) {
    writer.join();
  }

  expect(successes.load(std::memory_order_relaxed) == 1,
         "concurrent writes serialize quota check and atomic replacement");
  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  std::size_t regularFiles = 0;
  for (const auto &entry : fs::recursive_directory_iterator(overlay)) {
    if (entry.is_regular_file()) {
      ++regularFiles;
    }
  }
  expect(regularFiles == 1,
         "concurrent writes cannot leave more files than the quota");
}

void testSeparateInstancesCannotOversubscribeQuota() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("77777777-7777-4777-8777-777777777777");
  constexpr int writerCount = 8;
  std::vector<std::unique_ptr<LuaSkinFileSystem>> fileSystems;
  fileSystems.reserve(writerCount);
  for (int index = 0; index < writerCount; ++index) {
    auto created = fixture.create(fixture.entry, selectedProfile, true,
                                  {.maximumBytes = 64, .maximumFiles = 1});
    expect(created.fileSystem != nullptr,
           "separate-instance quota fixture is created");
    if (created.fileSystem) {
      fileSystems.push_back(std::move(created.fileSystem));
    }
  }
  if (fileSystems.size() != writerCount) {
    return;
  }

  std::barrier start(writerCount + 1);
  std::atomic_int successes = 0;
  std::vector<std::thread> writers;
  writers.reserve(writerCount);
  for (int index = 0; index < writerCount; ++index) {
    writers.emplace_back([&, index] {
      start.arrive_and_wait();
      const auto result = fileSystems[index]->writeData(
          "separate-" + std::to_string(index) + ".txt", bytesOf("x"), false);
      if (!result.failure) {
        ++successes;
      }
    });
  }
  start.arrive_and_wait();
  for (std::thread &writer : writers) {
    writer.join();
  }

  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  std::size_t regularFiles = 0;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(overlay, error), end;
       !error && iterator != end; ++iterator) {
    if (iterator->is_regular_file(error)) {
      ++regularFiles;
    }
  }
  expect(successes == 1 && regularFiles == 1,
         "separate filesystem instances serialize one shared overlay quota");
}

void testRenderTransitionLocksCapturedOperationsAndCounters() {
  PackageFixture fixture;
  const SkinProfileId selectedProfile =
      profile("55555555-5555-4555-8555-555555555555");
  auto created = fixture.create(fixture.entry, selectedProfile, true);
  expect(created.fileSystem != nullptr, "render fixture is created");
  if (!created.fileSystem) {
    return;
  }
  auto &fileSystem = *created.fileSystem;
  expect(!fileSystem.writeData("state.txt", bytesOf("before-render"), false)
              .failure,
         "pre-render state is committed");

  auto readClosure = [&fileSystem] {
    return fileSystem.read("state.txt", SkinFileUse::DataRead, 64);
  };
  auto resolveClosure = [&fileSystem] {
    return fileSystem.resolve("play.luaskin", SkinFileUse::LuaEntry);
  };
  auto moduleClosure = [&fileSystem] {
    return fileSystem.resolveModule("direct");
  };
  auto listClosure = [&fileSystem] { return fileSystem.list(".", "", 32); };
  auto writeClosure = [&fileSystem] {
    return fileSystem.writeData("state.txt", bytesOf("after-render"), false);
  };
  auto mkdirClosure = [&fileSystem] {
    return fileSystem.mkdirData("after-render-directory");
  };

  const fs::path overlay = fixture.overlayRoot(fixture.entry, selectedProfile);
  const std::string before = overlayDigest(overlay);
  const auto transition = fileSystem.enterRenderPhase();
  expect(transition.ok && !transition.failure,
         "render transition succeeds without filesystem work");
  expect(failedWith(readClosure().failure, SkinFileError::RenderPhase),
         "captured read closure cannot read after render phase");
  expect(failedWith(resolveClosure().failure, SkinFileError::RenderPhase),
         "captured resolve closure is phase checked on invocation");
  expect(failedWith(moduleClosure().failure, SkinFileError::RenderPhase),
         "captured module loader is phase checked on invocation");
  expect(failedWith(listClosure().failure, SkinFileError::RenderPhase),
         "captured legacy listing is denied after render phase");
  expect(failedWith(writeClosure().failure, SkinFileError::RenderPhase),
         "captured write closure cannot mutate after render phase");
  expect(failedWith(mkdirClosure().failure, SkinFileError::RenderPhase),
         "captured legacy mkdir cannot mutate after render phase");

  const SkinFileActivityCounters counters = fileSystem.activityCounters();
  expect(counters.renderReadsDenied == 3 && counters.renderWritesDenied == 2 &&
             counters.renderDirectoryScansDenied == 1,
         "each denied operation increments its exact category once");
  expect(counters.renderReadsPerformed == 0 &&
             counters.renderWritesPerformed == 0 &&
             counters.renderDirectoryScansPerformed == 0,
         "render guards keep all performed counters at zero");
  expect(overlayDigest(overlay) == before,
         "transition and denied captured calls leave overlay digest unchanged");
}

void testPreparedAndPublishedViewsStaySynchronouslyOwned() {
  PackageFixture fixture;
  {
    auto preparedFileSystem =
        fixture.create(fixture.entry, std::nullopt, false);
    expect(preparedFileSystem.fileSystem &&
               !preparedFileSystem.fileSystem
                    ->read("play.luaskin", SkinFileUse::LuaEntry, 4096)
                    .failure,
           "prepared owner encloses a synchronous validation filesystem");
  }

  std::string publishError;
  auto lease = std::move(*fixture.prepared).publish(publishError);
  expect(lease.has_value() && publishError.empty(),
         "fixture publishes for lease-backed session test");
  if (!lease) {
    return;
  }
  auto sessionFileSystem =
      LuaSkinFileSystem::create({.revision = lease->readView(),
                                 .entry = fixture.entry,
                                 .storageRoots = fixture.roots,
                                 .profileId = std::nullopt,
                                 .allowDataWrites = false});
  expect(sessionFileSystem.fileSystem &&
             !sessionFileSystem.fileSystem
                  ->read("play.luaskin", SkinFileUse::LuaEntry, 4096)
                  .failure,
         "published lease outlives its filesystem read view");
  sessionFileSystem.fileSystem.reset();
  expect(lease->weakPin().hasLiveLease(),
         "session owner still holds the lease after filesystem destruction");
}

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

void testResourceCandidatesAreEntryAwareAndAmbiguitySafe() {
  PackageFixture fixture;
  auto created = fixture.create(fixture.entry, std::nullopt, false);
  expect(created.fileSystem != nullptr,
         "candidate fixture creates an entry-aware filesystem");
  if (!created.fileSystem) {
    return;
  }
  const auto same = created.fileSystem->resolveResourceCandidates(
      "play.luaskin", "entry/play.luaskin");
  expect(same.normalizedVirtualPath &&
             *same.normalizedVirtualPath == "entry/play.luaskin",
         "entry-relative and package-normalized aliases with one identity are accepted");
  const auto ambiguous = created.fileSystem->resolveResourceCandidates(
      "play.luaskin", "alternate/play.luaskin");
  expect(ambiguous.failure && !ambiguous.normalizedVirtualPath,
         "two distinct secure resource candidates are rejected instead of choosing one");
  const auto bytes = created.fileSystem->readResolvedResource(
      "entry/resources/image.bin", 1024);
  expect(!bytes.failure && bytesToString(bytes.bytes) == "package-resource",
         "resolved resource reads use the package root rather than an overlay");
}

} // namespace

int main() {
  testWorkingDirectoryPackageCeilingAndModuleSearch();
  testNoFollowReadsAndOverlayUseSeparation();
  testAtomicWritesNestedParentsAndQuotaRollback();
  testOverlayWorkIsBoundedBeforeMutation();
  testInternalTemporaryNamespaceAndRecovery();
  testExistingOverlayPrivacyIsValidated();
  testPosixMutationPinsSurviveRenameSwaps();
  testDeterministicListingAndProfileEntryIsolation();
  testConcurrentWritesCannotOversubscribeQuota();
  testSeparateInstancesCannotOversubscribeQuota();
  testRenderTransitionLocksCapturedOperationsAndCounters();
  testPreparedAndPublishedViewsStaySynchronouslyOwned();
  testCompatibilityDiagnosticsDeduplicateAndRetainCriticality();
  testResourceCandidatesAreEntryAwareAndAmbiguitySafe();
  if (failures != 0) {
    std::cerr << failures << " lua skin filesystem test(s) failed\n";
    return 1;
  }
  std::cout << "lua skin filesystem tests passed\n";
  return 0;
}
