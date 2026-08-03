#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "skin/SkinProfileSettings.h"
#include "skin/SkinStoragePaths.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#if !defined(_WIN32)
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
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
    root_ = fs::temp_directory_path() /
            ("asobmashow-snapshot-test-" + std::to_string(++serial));
    fs::create_directories(root_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add,
                    ignored);
    for (fs::recursive_directory_iterator iterator(root_, ignored), end;
         !ignored && iterator != end; ++iterator) {
      if (iterator->is_directory(ignored)) {
        fs::permissions(iterator->path(), fs::perms::owner_all,
                        fs::perm_options::add, ignored);
      }
    }
    ignored.clear();
    fs::remove_all(root_, ignored);
  }
  const fs::path &root() const { return root_; }

private:
  fs::path root_;
};

void writeBytes(const fs::path &path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "visible",
          .privateRevisions = root / "revisions",
          .privateCatalog = root / "catalog",
          .profileOverlays = root / "overlays"};
}

SkinPackageId packageId() { return *normalizePackageId("FixtureSkin").package; }

SkinEntryId entryId(std::string_view package, std::string_view relativePath) {
  const auto normalizedPackage = normalizePackageId(package);
  return *normalizeEntryPath(*normalizedPackage.package, relativePath).entry;
}

class FakeAliasDetector final : public SkinAliasDetector {
public:
  explicit FakeAliasDetector(
      SkinRejectedLinkKind kind = SkinRejectedLinkKind::None,
      std::string filename = {})
      : kind_(kind), filename_(std::move(filename)) {}

  SkinRejectedLinkKind inspectNoFollow(const fs::path &path) const override {
    if (filename_.empty() || path.filename() == filename_) {
      return kind_;
    }
    return SkinRejectedLinkKind::None;
  }

private:
  SkinRejectedLinkKind kind_;
  std::string filename_;
};

std::size_t stagingEntryCount(const SkinStorageRoots &roots) {
  const fs::path staging = roots.privateRevisions / ".staging";
  std::error_code error;
  if (!fs::exists(staging, error)) {
    return 0;
  }
  return static_cast<std::size_t>(std::distance(
      fs::directory_iterator(staging, error), fs::directory_iterator{}));
}

void testDigestUsesExactTreeV1FramingAndStableSorting() {
  TempDirectory temp;
  const fs::path source = temp.root() / "source";
  writeBytes(source / "dir/b.bin", std::string("\0\1", 2));
  writeBytes(source / "a.txt", "A");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);

  auto result = snapshotter.snapshot(source, packageId(), {}, {});
  expect(result.prepared.has_value(), "a regular tree is prepared");
  if (result.prepared) {
    expect(
        result.prepared->revision().lowercaseSha256 ==
            "085e4def6052e393241dd2a90abd38bc0814b078f557f0c0e6e71f25056bd4af",
        "digest exactly matches independently framed SkinTreeDigestV1");
    expect(result.prepared->revision().fileCount == 2,
           "revision records the regular-file count");
    expect(result.prepared->revision().totalBytes == 3,
           "revision records total content bytes");
    expect(fs::exists(result.prepared->stagingRoot() / "a.txt"),
           "snapshot copies root files");
    expect(fs::exists(result.prepared->stagingRoot() / "dir/b.bin"),
           "snapshot copies nested files");
  }
}

void testEmptyTreeIsRejectedForDigestParity() {
  TempDirectory temp;
  const fs::path source = temp.root() / "source";
  fs::create_directories(source);
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const auto result = snapshotter.snapshot(source, packageId(), {}, {});
  expect(!result.prepared,
         "an empty tree is rejected rather than hashing a non-package");
}

void testRevisionStoresTheNormalizedCanonicalPackageIdentity() {
  TempDirectory temp;
  const fs::path source = temp.root() / "source";
  writeBytes(source / "main.luaskin", "return {}\n");
  const auto canonical = *normalizePackageId("Caf\xC3\xA9").package;
  SkinPackageId authoredAlias = canonical;
  authoredAlias.directoryName = "Cafe\xCC\x81";
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const auto result = snapshotter.snapshot(source, authoredAlias, {}, {});
  expect(result.prepared && result.prepared->revision().package == canonical,
         "revision stores the normalized package ID, not caller spelling");
}

void testFramingSeparatesDifferentFileBoundaries() {
  TempDirectory temp;
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(rootsBelow(temp.root()), aliases);
  const fs::path first = temp.root() / "first";
  const fs::path second = temp.root() / "second";
  writeBytes(first / "foo", "ab");
  writeBytes(first / "x", "c");
  writeBytes(second / "foo", "a");
  writeBytes(second / "x", "bc");

  auto firstResult = snapshotter.snapshot(first, packageId(), {}, {});
  auto secondResult = snapshotter.snapshot(second, packageId(), {}, {});
  expect(firstResult.prepared.has_value() && secondResult.prepared.has_value(),
         "both boundary fixtures prepare");
  if (firstResult.prepared && secondResult.prepared) {
    expect(
        firstResult.prepared->revision().lowercaseSha256 ==
            "d72cf796f29c9edadc6a8059505650673a9b789d79f383e7dd51c07f4e3b9202",
        "first hand-framed digest matches");
    expect(
        secondResult.prepared->revision().lowercaseSha256 ==
            "0c054f16ef5654a5f04cd094c6b36f03a78af7321f499362c573c0621fab6ecd",
        "second hand-framed digest matches");
    expect(firstResult.prepared->revision().lowercaseSha256 !=
               secondResult.prepared->revision().lowercaseSha256,
           "file-length framing separates concatenation boundaries");
  }
}

void testInjectedFinderAliasAndWindowsReparsePointAreRejected() {
  for (const auto kind : {SkinRejectedLinkKind::AppleFinderAlias,
                          SkinRejectedLinkKind::WindowsReparsePoint}) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "alias.dat", "payload");
    FakeAliasDetector aliases(kind, "alias.dat");
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto result = snapshotter.snapshot(source, packageId(), {}, {});
    expect(!result.prepared.has_value(),
           "injected alias/reparse classification rejects the tree");
    expect(!result.diagnostics.empty(), "rejection has a diagnostic");
    expect(stagingEntryCount(roots) == 0,
           "alias rejection leaves no incomplete staging tree");
  }
}

void testPlatformDetectorClassifiesNoFollowNodes() {
  TempDirectory temp;
  const fs::path regular = temp.root() / "regular";
  writeBytes(regular, "payload");
  auto detector = createPlatformSkinAliasDetector();
  expect(detector != nullptr, "platform alias detector is available");
  if (!detector) {
    return;
  }
  expect(detector->inspectNoFollow(regular) == SkinRejectedLinkKind::None,
         "platform detector accepts a one-link regular file");
#if !defined(_WIN32)
  const fs::path symlink = temp.root() / "symlink";
  fs::create_symlink("regular", symlink);
  expect(detector->inspectNoFollow(symlink) ==
             SkinRejectedLinkKind::SymbolicLink,
         "platform detector does not follow symbolic links");
  const fs::path hardlink = temp.root() / "hardlink";
  fs::create_hard_link(regular, hardlink);
  expect(detector->inspectNoFollow(regular) == SkinRejectedLinkKind::HardLink &&
             detector->inspectNoFollow(hardlink) ==
                 SkinRejectedLinkKind::HardLink,
         "platform detector rejects every name for a multiply-linked file");
#endif
}

void testSymbolicHardAndNonRegularNodesAreRejected() {
#if !defined(_WIN32)
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "real", "payload");
    fs::create_symlink("real", source / "link");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    expect(!snapshotter.snapshot(source, packageId(), {}, {}).prepared,
           "symbolic links are rejected without following");
    expect(stagingEntryCount(roots) == 0,
           "symlink rejection leaves no staging tree");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "one", "payload");
    fs::create_hard_link(source / "one", source / "two");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    expect(!snapshotter.snapshot(source, packageId(), {}, {}).prepared,
           "multi-link regular files are rejected");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    fs::create_directories(source);
    const fs::path fifo = source / "fifo";
    expect(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO fixture is created");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    expect(!snapshotter.snapshot(source, packageId(), {}, {}).prepared,
           "FIFOs are rejected without opening");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    fs::create_directories(source);
    const fs::path socketPath = source / "socket";
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string native = socketPath.string();
    std::copy(native.begin(), native.end(), address.sun_path);
    expect(descriptor >= 0 &&
               ::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)) == 0,
           "socket fixture is created");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    expect(!snapshotter.snapshot(source, packageId(), {}, {}).prepared,
           "socket nodes are rejected without opening");
    if (descriptor >= 0) {
      ::close(descriptor);
    }
  }
#endif
}

void testUnicodeAndCaseFoldCollisionsAreRejected() {
  for (bool nfcCollision : {false, true}) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    if (nfcCollision) {
      writeBytes(source / fs::path("Caf\xC3\xA9.lua"), "one");
      writeBytes(source / fs::path("Cafe\xCC\x81.lua"), "two");
    } else {
      writeBytes(source / fs::path("\xEF\xAC\x80.lua"), "one");
      writeBytes(source / "ff.lua", "two");
    }
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    const auto physicalCount = static_cast<std::size_t>(std::distance(
        fs::directory_iterator(source), fs::directory_iterator{}));
    if (physicalCount == 2) {
      expect(!snapshotter.snapshot(source, packageId(), {}, {}).prepared,
             "NFC and full-casefold path aliases collide");
    } else {
      const auto first = normalizeEntryPath(
          packageId(), nfcCollision ? "Caf\xC3\xA9.lua" : "\xEF\xAC\x80.lua");
      const auto second = normalizeEntryPath(
          packageId(), nfcCollision ? "Cafe\xCC\x81.lua" : "ff.lua");
      expect(first.entry && second.entry &&
                 first.entry->collisionKey == second.entry->collisionKey,
             "host-collapsed aliases retain one canonical identity");
    }
  }
}

void testMutationAtEveryStableCopyBoundaryIsRejected() {
  for (int boundary = 0; boundary < 3; ++boundary) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "a", std::string(128 * 1024, 'a'));
    writeBytes(source / "b", std::string(128 * 1024, 'b'));
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    bool changed = false;
    auto result = snapshotter.snapshot(
        source, packageId(), {}, [&](const SkinProgress &progress) {
          if (changed) {
            return;
          }
          const bool atBoundary =
              (boundary == 0 &&
               progress.phase == SkinProgressPhase::Inspecting &&
               progress.completedFiles == 2) ||
              (boundary == 1 && progress.phase == SkinProgressPhase::Copying &&
               progress.completedFiles == 1) ||
              (boundary == 2 &&
               progress.phase == SkinProgressPhase::Validating &&
               progress.completedFiles == 0);
          if (atBoundary) {
            writeBytes(source / "b", "mutated-size");
            changed = true;
          }
        });
    expect(changed, "mutation callback reached the requested boundary");
    expect(!result.prepared.has_value(),
           "source mutation cannot produce a prepared revision");
    expect(stagingEntryCount(roots) == 0,
           "unstable copy leaves no incomplete staging tree");
  }
}

void testSourceRootReplacementWithTheSameFilesIsRejected() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  const fs::path displaced = temp.root() / "displaced";
  writeBytes(source / "a", "payload");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  bool replaced = false;
  auto result = snapshotter.snapshot(
      source, packageId(), {}, [&](const SkinProgress &progress) {
        if (!replaced && progress.phase == SkinProgressPhase::Validating &&
            progress.completedFiles == 0) {
          fs::rename(source, displaced);
          fs::create_directory(source);
          fs::rename(displaced / "a", source / "a");
          fs::remove(displaced);
          replaced = true;
        }
      });
  expect(replaced, "source-root replacement fixture ran");
  expect(!result.prepared,
         "changing the source root identity invalidates the stable snapshot");
}

void testPublishingCallbackMutationCannotEscapeFinalValidation() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "a", "original");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  bool mutated = false;
  const auto result = snapshotter.snapshot(
      source, packageId(), {}, [&](const SkinProgress &progress) {
        if (!mutated && progress.phase == SkinProgressPhase::Publishing) {
          writeBytes(source / "a", "changed-after-validation");
          mutated = true;
        }
      });
  expect(mutated, "publishing callback mutation fixture ran");
  expect(!result.prepared,
         "source mutation in the publishing callback is rejected");
  expect(stagingEntryCount(roots) == 0,
         "publishing callback mutation leaves no staging orphan");
}

void testTransientParentSymlinkCannotBeFollowedDuringCopy() {
#if !defined(_WIN32)
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  const fs::path nested = source / "nested";
  const fs::path displaced = temp.root() / "displaced";
  writeBytes(source / "a", "first");
  writeBytes(nested / "b", "second");
  const auto sourceTime = fs::last_write_time(source);
  const auto nestedTime = fs::last_write_time(nested);
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  bool installedSymlink = false;
  bool restoredDirectory = false;
  auto result = snapshotter.snapshot(
      source, packageId(), {}, [&](const SkinProgress &progress) {
        if (progress.phase != SkinProgressPhase::Copying) {
          return;
        }
        if (!installedSymlink && progress.completedFiles == 1) {
          fs::rename(nested, displaced);
          fs::create_directory_symlink("../displaced", nested);
          installedSymlink = true;
        } else if (installedSymlink && !restoredDirectory &&
                   progress.completedFiles == 2) {
          fs::remove(nested);
          fs::rename(displaced, nested);
          fs::last_write_time(nested, nestedTime);
          fs::last_write_time(source, sourceTime);
          restoredDirectory = true;
        }
      });
  expect(installedSymlink,
         "transient parent-symlink fixture ran during the copy");
  expect(!result.prepared,
         "copy never follows a transient symlink in a parent component");
  if (!restoredDirectory) {
    fs::remove(nested);
    fs::rename(displaced, nested);
  }
#endif
}

void testTransientFifoReplacementCannotBlockOpen() {
#if !defined(_WIN32)
  TempDirectory temp;
  const pid_t child = ::fork();
  expect(child >= 0, "FIFO replacement test process starts");
  if (child == 0) {
    ::alarm(2);
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "a", "first");
    writeBytes(source / "b", "second");
    const auto sourceTime = fs::last_write_time(source);
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    bool replaced = false;
    const auto result = snapshotter.snapshot(
        source, packageId(), {}, [&](const SkinProgress &progress) {
          if (!replaced && progress.phase == SkinProgressPhase::Copying &&
              progress.completedFiles == 1) {
            fs::remove(source / "b");
            if (::mkfifo((source / "b").c_str(), 0600) != 0) {
              ::_exit(3);
            }
            fs::last_write_time(source, sourceTime);
            replaced = true;
          }
        });
    ::_exit(replaced && !result.prepared ? 0 : 4);
  }
  if (child > 0) {
    int status = 0;
    expect(::waitpid(child, &status, 0) == child,
           "FIFO replacement test process completes");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "a FIFO leaf replacement is rejected without blocking open");
  }
#endif
}

void testCancellationAndPreparedDestructionCleanStaging() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "a", "payload");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    std::stop_source cancellation;
    auto result = snapshotter.snapshot(
        source, packageId(), cancellation.get_token(),
        [&](const SkinProgress &) { cancellation.request_stop(); });
    expect(result.cancelled, "cancelled snapshot reports cancellation");
    expect(!result.prepared, "cancelled snapshot is never prepared");
    expect(stagingEntryCount(roots) == 0, "cancelled snapshot removes staging");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path source = temp.root() / "source";
    writeBytes(source / "a", "payload");
    FakeAliasDetector aliases;
    SkinTreeSnapshotter snapshotter(roots, aliases);
    {
      auto result = snapshotter.snapshot(source, packageId(), {}, {});
      expect(result.prepared.has_value(), "prepared fixture exists");
      expect(stagingEntryCount(roots) == 1,
             "prepared revision owns one staging tree");
    }
    expect(stagingEntryCount(roots) == 0,
           "unpublished prepared destructor removes its staging tree");
  }
}

void testPublishedRevisionIsImmutableAndLeaseClonesShareThePin() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "a", "payload");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto result = snapshotter.snapshot(source, packageId(), {}, {});
  expect(result.prepared.has_value(), "publication fixture prepares");
  if (!result.prepared) {
    return;
  }
  std::string error;
  auto lease = std::move(*result.prepared).publish(error);
  expect(lease.has_value(), "prepared revision publishes atomically: " + error);
  expect(error.empty(), "successful publication has no error: " + error);
  if (!lease) {
    return;
  }
  const fs::path publishedRoot = lease->root();
  expect(publishedRoot != source && fs::exists(publishedRoot / "a"),
         "lease exposes only the private immutable root");
  const auto permissions = fs::status(publishedRoot / "a").permissions();
  expect((permissions & fs::perms::owner_write) == fs::perms::none,
         "published files are not owner-writable");
  auto second = lease->clone();
  expect(second.root() == publishedRoot &&
             second.revision().package == lease->revision().package &&
             second.revision().lowercaseSha256 ==
                 lease->revision().lowercaseSha256,
         "clone is an independent handle over the same revision pin");
  lease.reset();
  expect(fs::exists(second.readView().root() / "a"),
         "clone remains valid after the original lease is released");
}

void testPreparedRevisionIsFrozenAndPublishRevalidatesItsDigest() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "a", "original");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  fs::path stagingRoot;
  {
    auto result = snapshotter.snapshot(source, packageId(), {}, {});
    expect(result.prepared.has_value(), "tamper fixture prepares");
    if (!result.prepared) {
      return;
    }
    stagingRoot = result.prepared->stagingRoot();
    const auto permissions = fs::status(stagingRoot / "a").permissions();
    expect((permissions & fs::perms::owner_write) == fs::perms::none,
           "prepared files are frozen before their digest is exposed");

    std::error_code permissionError;
    fs::permissions(stagingRoot / "a", fs::perms::owner_write,
                    fs::perm_options::add, permissionError);
    writeBytes(stagingRoot / "a", "tampered-after-prepare");
    expect(readBytes(stagingRoot / "a") == "tampered-after-prepare",
           "adversarial owner can alter permissions for the tamper fixture");
    std::string error;
    const auto lease = std::move(*result.prepared).publish(error);
    expect(!lease && !error.empty(),
           "publish revalidates and rejects altered staging content");
  }
  expect(!fs::exists(stagingRoot),
         "failed tampered publication leaves no staging orphan");
}

void testPublishRejectsPoisonedExistingDigestDestinations() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "a", "original");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);

  auto initial = snapshotter.snapshot(source, packageId(), {}, {});
  expect(initial.prepared.has_value(), "poison fixture prepares");
  if (!initial.prepared) {
    return;
  }
  const fs::path destination =
      roots.privateRevisions / initial.prepared->revision().lowercaseSha256;
  initial.prepared.reset();
  writeBytes(destination / "a", "poison");

  fs::path stagingRoot;
  {
    auto poisoned = snapshotter.snapshot(source, packageId(), {}, {});
    expect(poisoned.prepared.has_value(), "poisoned publication prepares");
    if (!poisoned.prepared) {
      return;
    }
    stagingRoot = poisoned.prepared->stagingRoot();
    std::string error;
    const auto lease = std::move(*poisoned.prepared).publish(error);
    expect(!lease && !error.empty(),
           "publish rejects a mismatched pre-existing digest directory");
  }
  expect(readBytes(destination / "a") == "poison",
         "verification never trusts or rewrites poisoned existing content");
  expect(!fs::exists(stagingRoot),
         "poisoned destination failure leaves no staging orphan");
}

void testPublishRejectsMutableExistingRevisionAndSymlinkDestination() {
#if !defined(_WIN32)
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "a", "original");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);

  auto first = snapshotter.snapshot(source, packageId(), {}, {});
  std::string firstError;
  auto firstLease = std::move(*first.prepared).publish(firstError);
  expect(firstLease.has_value(),
         "existing-revision fixture publishes: " + firstError);
  if (!firstLease) {
    return;
  }
  const fs::path destination = firstLease->root();
  std::error_code permissionError;
  fs::permissions(destination / "a", fs::perms::owner_write,
                  fs::perm_options::add, permissionError);
  auto mutableExisting = snapshotter.snapshot(source, packageId(), {}, {});
  std::string mutableError;
  const auto mutableLease =
      std::move(*mutableExisting.prepared).publish(mutableError);
  expect(!mutableLease && !mutableError.empty(),
         "publish rejects an existing revision that became mutable");

  fs::permissions(destination, fs::perms::owner_all, fs::perm_options::add,
                  permissionError);
  fs::remove_all(destination, permissionError);
  const fs::path outside = temp.root() / "outside";
  writeBytes(outside / "sentinel", "untouched");
  fs::create_directory_symlink(outside, destination);
  fs::path stagingRoot;
  {
    auto symlinkExisting = snapshotter.snapshot(source, packageId(), {}, {});
    stagingRoot = symlinkExisting.prepared->stagingRoot();
    std::string symlinkError;
    const auto symlinkLease =
        std::move(*symlinkExisting.prepared).publish(symlinkError);
    expect(!symlinkLease && !symlinkError.empty(),
           "publish rejects a symlink at the digest destination");
  }
  expect(readBytes(outside / "sentinel") == "untouched",
         "digest-destination verification never follows a symlink");
  expect(!fs::exists(stagingRoot),
         "symlink destination failure leaves no staging orphan");
#endif
}

void testOverlayIdentityIsPrivateContainedAndCanonical() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const auto firstProfile =
      *makeSkinProfileId("11111111-1111-4111-8111-111111111111");
  const auto secondProfile =
      *makeSkinProfileId("22222222-2222-4222-8222-222222222222");
  const auto firstEntry = entryId("Stra\xC3\x9F"
                                  "e",
                                  "Caf\xC3\xA9.luaskin");
  const auto firstEntryAlias = entryId("STRASSE", "Cafe\xCC\x81.luaskin");
  const auto secondEntry = entryId("Other", "main.luaskin");

  std::set<fs::path> uniqueRoots;
  for (const auto &profile : {firstProfile, secondProfile}) {
    for (const auto &entry : {firstEntry, secondEntry}) {
      const auto result = deriveSkinPrivateOverlayRoot(roots, profile, entry);
      expect(result.root.has_value(),
             "a normalized profile and entry derive an overlay root");
      if (!result.root) {
        continue;
      }
      const std::string leaf = result.root->filename().string();
      expect(result.root->parent_path() == roots.profileOverlays,
             "derived overlay is a direct child of the private overlay root");
      expect(leaf.size() == 64 && leaf.find_first_not_of("0123456789abcdef") ==
                                      std::string::npos,
             "overlay leaf is a lowercase SHA-256 digest");
      expect(leaf.find(profile.opaque) == std::string::npos &&
                 leaf.find(entry.package.directoryName) == std::string::npos &&
                 leaf.find(entry.packageRelativePath) == std::string::npos,
             "overlay leaf never contains raw profile or entry text");
      uniqueRoots.insert(*result.root);
    }
  }
  expect(uniqueRoots.size() == 4,
         "two profiles by two entries derive four unique roots");

  const auto aliasResult =
      deriveSkinPrivateOverlayRoot(roots, firstProfile, firstEntryAlias);
  const auto originalResult =
      deriveSkinPrivateOverlayRoot(roots, firstProfile, firstEntry);
  expect(aliasResult.root && originalResult.root &&
             aliasResult.root == originalResult.root,
         "NFC and case-fold aliases share one typed overlay identity");
  if (originalResult.root) {
    expect(
        originalResult.root->filename() ==
            "d139f0ad58318474c932f945c4ac3e1e4828f1c1f411bcf87bb94409fc12e4cd",
        "overlay digest matches the independently framed V1 identity");
  }

  const auto uppercaseProfile =
      *makeSkinProfileId("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA");
  const auto lowercaseProfile =
      *makeSkinProfileId("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  const auto uppercaseResult =
      deriveSkinPrivateOverlayRoot(roots, uppercaseProfile, firstEntry);
  const auto lowercaseResult =
      deriveSkinPrivateOverlayRoot(roots, lowercaseProfile, firstEntry);
  expect(uppercaseResult.root && lowercaseResult.root &&
             uppercaseResult.root == lowercaseResult.root,
         "normalized opaque profile aliases share one overlay identity");
}

void testOverlayIdentityRejectsUnsafeInputsAndFramesFields() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const auto profile =
      *makeSkinProfileId("11111111-1111-4111-8111-111111111111");
  const auto entry = entryId("Fixture", "main.luaskin");

  auto relativeRoots = roots;
  relativeRoots.profileOverlays =
      fs::relative(temp.root() / "relative-overlays", fs::current_path());
  const auto relativeOverlay =
      deriveSkinPrivateOverlayRoot(relativeRoots, profile, entry);
  expect(!relativeOverlay.root && relativeOverlay.failure,
         "a relative private overlay root is rejected fail-closed");

  for (const SkinProfileId invalidProfile :
       {SkinProfileId{.opaque = "../profile"},
        SkinProfileId{.opaque = std::string("\xff", 1)}}) {
    const auto result =
        deriveSkinPrivateOverlayRoot(roots, invalidProfile, entry);
    expect(!result.root && result.failure,
           "traversal and invalid UTF-8 profile identities are rejected");
  }

  auto invalidEntry = entry;
  invalidEntry.packageRelativePath = "../main.luaskin";
  expect(!deriveSkinPrivateOverlayRoot(roots, profile, invalidEntry).root,
         "a manually forged traversal entry is rejected");
  invalidEntry = entry;
  invalidEntry.package.directoryName = std::string("\xff", 1);
  expect(!deriveSkinPrivateOverlayRoot(roots, profile, invalidEntry).root,
         "a manually forged invalid UTF-8 package is rejected");

  const auto firstBoundary =
      deriveSkinPrivateOverlayRoot(roots, profile, entryId("ab", "c"));
  const auto secondBoundary =
      deriveSkinPrivateOverlayRoot(roots, profile, entryId("a", "bc"));
  expect(firstBoundary.root && secondBoundary.root &&
             firstBoundary.root != secondBoundary.root,
         "length framing separates identities with the same naive text join");
}

void testSnapshotRejectsRelativePrivateRevisionRoot() {
  TempDirectory temp;
  auto roots = rootsBelow(temp.root());
  roots.privateRevisions =
      fs::relative(temp.root() / "relative-revisions", fs::current_path());
  const fs::path source = temp.root() / "source";
  writeBytes(source / "main.luaskin", "return {}\n");
  FakeAliasDetector aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  const auto result = snapshotter.snapshot(source, packageId(), {}, {});
  expect(!result.prepared,
         "snapshot rejects a relative private revision root fail-closed");
}

} // namespace

int main() {
  static_assert(!std::is_copy_constructible_v<SkinRevisionLease>);
  static_assert(!std::is_copy_assignable_v<SkinRevisionLease>);
  static_assert(std::is_move_constructible_v<SkinRevisionLease>);
  testDigestUsesExactTreeV1FramingAndStableSorting();
  testEmptyTreeIsRejectedForDigestParity();
  testRevisionStoresTheNormalizedCanonicalPackageIdentity();
  testFramingSeparatesDifferentFileBoundaries();
  testInjectedFinderAliasAndWindowsReparsePointAreRejected();
  testPlatformDetectorClassifiesNoFollowNodes();
  testSymbolicHardAndNonRegularNodesAreRejected();
  testUnicodeAndCaseFoldCollisionsAreRejected();
  testMutationAtEveryStableCopyBoundaryIsRejected();
  testSourceRootReplacementWithTheSameFilesIsRejected();
  testPublishingCallbackMutationCannotEscapeFinalValidation();
  testTransientParentSymlinkCannotBeFollowedDuringCopy();
  testTransientFifoReplacementCannotBlockOpen();
  testCancellationAndPreparedDestructionCleanStaging();
  testPublishedRevisionIsImmutableAndLeaseClonesShareThePin();
  testPreparedRevisionIsFrozenAndPublishRevalidatesItsDigest();
  testPublishRejectsPoisonedExistingDigestDestinations();
  testPublishRejectsMutableExistingRevisionAndSymlinkDestination();
  testOverlayIdentityIsPrivateContainedAndCanonical();
  testOverlayIdentityRejectsUnsafeInputsAndFramesFields();
  testSnapshotRejectsRelativePrivateRevisionRoot();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "skin tree snapshotter tests passed\n";
  return 0;
}
