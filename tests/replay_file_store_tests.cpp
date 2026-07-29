#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileStore.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Bytes = std::vector<std::byte>;
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
    static unsigned long long serial = 0;
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-replay-store-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++serial));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

Bytes bytes(std::string_view value) {
  const auto raw = std::as_bytes(std::span(value.data(), value.size()));
  return {raw.begin(), raw.end()};
}

replay::ReplayPathIdentity identity(int history = 0) {
  std::string diagnostic;
  const auto stem =
      replay::chartStem(std::string(64, 'a'), 1, false, diagnostic);
  return *replay::pathForStem(*stem, history, diagnostic);
}

void write(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void testInstallInspectRetryAndOccupiedRefusal() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes first = bytes("first replay bytes");
  const auto reservation = store.reserve(identity(), first, "attempt-1");
  expect(reservation.reservation.has_value(),
         "canonical contained path can be reserved");
  if (!reservation.reservation) {
    return;
  }
  const auto installed = store.install(*reservation.reservation, first);
  expect(installed.state == replay::ReplayInstallState::InstalledVerified &&
             installed.file &&
             installed.file->lifecycle.state ==
                 replay::ReplayFileLifecycleState::InstalledUnassociated &&
             !installed.existingIdenticalFile,
         "durable install remains explicitly unassociated");
  if (!installed.file) {
    return;
  }
  expect(store.inspect(installed.file->metadata).state ==
             replay::ReplayFileState::Available,
         "installed bytes verify by size and hash");

  const auto retry = store.install(*reservation.reservation, first);
  expect(retry.state == replay::ReplayInstallState::InstalledVerified &&
             retry.file && retry.existingIdenticalFile &&
             retry.file->metadata == installed.file->metadata,
         "exact attempt and bytes reconcile as idempotent retry");

  const Bytes second = bytes("different replay bytes");
  const auto collision = store.reserve(identity(), second, "attempt-2");
  expect(collision.reservation.has_value(),
         "reservation itself does not infer path ownership");
  if (collision.reservation) {
    const auto refused = store.install(*collision.reservation, second);
    expect(refused.state == replay::ReplayInstallState::Occupied &&
               store.inspect(installed.file->metadata).state ==
                   replay::ReplayFileState::Available,
           "occupied slot is never overwritten by isolated store");
  }
}

void testContainmentAndSymlinksFailClosed() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes payload = bytes("payload");
  auto unsafe = identity();
  unsafe.relativePath = std::filesystem::path("..") / "escape.brd";
  expect(!store.reserve(unsafe, payload, "attempt").reservation,
         "traversal path cannot be reserved");
  unsafe = identity();
  unsafe.relativePath = std::filesystem::absolute("escape.brd");
  expect(!store.reserve(unsafe, payload, "attempt").reservation,
         "absolute path cannot be reserved");
  expect(!store.reserve(identity(), payload, "").reservation,
         "empty attempt token cannot reserve a file");

  replay::ReplayFileMetadata nonCanonical{
      .relativePath = "replay/not-a-stock-stem.brd",
      .sha256 = std::string(64, 'a'),
      .compressedSize = 1,
      .codecVersion = 3,
  };
  expect(store.inspect(nonCanonical).state == replay::ReplayFileState::Unsafe,
         "metadata inspection uses the same path grammar as reservation");

  TempDirectory outside;
  std::filesystem::create_directory_symlink(outside.path,
                                            profile.path / "replay");
  const auto reservation = store.reserve(identity(), payload, "attempt");
  expect(reservation.reservation.has_value(),
         "pure reservation does not follow filesystem links");
  if (reservation.reservation) {
    expect(store.install(*reservation.reservation, payload).state ==
               replay::ReplayInstallState::Unsafe,
           "symlinked replay directory is rejected without writing outside");
  }
  expect(std::filesystem::is_empty(outside.path),
         "unsafe directory target remains untouched");
}

void testFaultsAreRecoverableAtExactReservation() {
  const Bytes payload = bytes("payload");
  {
    TempDirectory profile;
    replay::ReplayFileStore store(profile.path,
                                  {.failAt = [](std::string_view point) {
                                    return point == "temporary-write";
                                  }});
    const auto reservation = store.reserve(identity(), payload, "attempt");
    const auto outcome = store.install(*reservation.reservation, payload);
    expect(outcome.state == replay::ReplayInstallState::Failed &&
               !std::filesystem::exists(profile.path / "replay" /
                                        identity().relativePath.filename()),
           "write failure installs no final replay");
  }
  {
    TempDirectory profile;
    replay::ReplayFileStore store(
        profile.path,
        {.failAt = [](std::string_view point) { return point == "install"; }});
    const auto reservation = store.reserve(identity(), payload, "attempt");
    const auto outcome = store.install(*reservation.reservation, payload);
    expect(
        outcome.state == replay::ReplayInstallState::Failed &&
            !std::filesystem::exists(
                profile.path / reservation.reservation->temporaryRelativePath),
        "pre-install failure cleans the exact private temporary file");
  }
  {
    TempDirectory profile;
    bool lostAcknowledgement = false;
    replay::ReplayFileStore store(profile.path,
                                  {.failAt = [&](std::string_view point) {
                                    if (point == "directory-sync") {
                                      lostAcknowledgement = true;
                                      return true;
                                    }
                                    return false;
                                  }});
    const auto reservation = store.reserve(identity(), payload, "attempt");
    const auto recovered = store.install(*reservation.reservation, payload);
    expect(lostAcknowledgement &&
               recovered.state == replay::ReplayInstallState::InstalledVerified,
           "lost directory-sync acknowledgement triggers a successful "
           "parent re-sync");
  }
  {
    TempDirectory profile;
    bool inject = true;
    replay::ReplayFileStore interrupted(
        profile.path, {.failAt = [&](std::string_view point) {
          if (point == "after-install" && inject) {
            inject = false;
            return true;
          }
          return false;
        }});
    const auto reservation =
        interrupted.reserve(identity(), payload, "attempt");
    const auto ambiguous =
        interrupted.install(*reservation.reservation, payload);
    expect(ambiguous.state == replay::ReplayInstallState::RetryableAmbiguous &&
               std::filesystem::exists(profile.path / identity().relativePath),
           "interruption after atomic install is reported as ambiguous");

    replay::ReplayFileStore retry(profile.path);
    const auto reconciled = retry.install(*reservation.reservation, payload);
    expect(reconciled.state == replay::ReplayInstallState::InstalledVerified &&
               reconciled.existingIdenticalFile,
           "exact retry re-syncs and verifies ambiguous installed bytes");
  }
}

void testCorruptUserDeletionAndAutomaticOwnershipGuard() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes payload = bytes("payload");
  const auto reservation = store.reserve(identity(), payload, "attempt");
  const auto installed = store.install(*reservation.reservation, payload);
  const auto metadata = installed.file->metadata;
  const auto path = profile.path / metadata.relativePath;
  write(path, "user replacement");
  expect(store.inspect(metadata).state == replay::ReplayFileState::Corrupt,
         "replacement bytes do not invalidate the stored result metadata");

  std::string diagnostic;
  expect(!store.removeIfMatches(metadata, diagnostic) &&
             std::filesystem::exists(path),
         "automatic cleanup refuses bytes it cannot prove it owns");
  expect(store.removeReferencedEntry(metadata, diagnostic) &&
             !std::filesystem::exists(path),
         "explicit user deletion removes corrupt referenced entry only");
  expect(store.removeReferencedEntry(metadata, diagnostic),
         "user deletion is idempotent after an already missing file");
}

void testReservationCleanupRequiresExactOwnedMetadata() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes payload = bytes("unassociated replay");
  const auto reservation = store.reserve(identity(), payload, "attempt");
  const auto installed = store.install(*reservation.reservation, payload);
  expect(installed.state == replay::ReplayInstallState::InstalledVerified,
         "unassociated replay is installed for restart cleanup");
  std::string diagnostic;
  expect(store.removeIfMatches(installed.file->metadata, diagnostic) &&
             !std::filesystem::exists(profile.path / identity().relativePath),
         "restart cleanup removes bytes matching the ownership receipt");

  auto unsafe = installed.file->metadata;
  unsafe.relativePath = std::filesystem::path("..") / "outside.brd";
  expect(!store.removeIfMatches(unsafe, diagnostic),
         "restart cleanup rejects unsafe ownership metadata");
}

void testAutomaticCleanupCannotUnlinkARacingReplacement() {
  const Bytes payload = bytes("owned replay");
  {
    TempDirectory profile;
    replay::ReplayFileStore installer(profile.path);
    const auto reservation = installer.reserve(identity(), payload, "attempt");
    const auto installed = installer.install(*reservation.reservation, payload);
    expect(installed.file.has_value(),
           "competing cleanup fixture installs owned replay");
    if (!installed.file) {
      return;
    }
    const auto path = profile.path / installed.file->metadata.relativePath;
    replay::ReplayFileStore competitor(profile.path);
    bool competitorRemoved = false;
    replay::ReplayFileStore cleanup(
        profile.path, {.failAt = [&](std::string_view point) {
          if (point == "remove-before-quarantine") {
            std::string competingDiagnostic;
            competitorRemoved = competitor.removeIfMatches(
                installed.file->metadata, competingDiagnostic);
            return true;
          }
          return false;
        }});

    std::string diagnostic;
    expect(!cleanup.removeIfMatches(installed.file->metadata, diagnostic) &&
               !competitorRemoved && std::filesystem::exists(path),
           "one cleanup lease excludes a competing cleanup before quarantine");
  }
  {
    TempDirectory profile;
    replay::ReplayFileStore installer(profile.path);
    const auto reservation = installer.reserve(identity(), payload, "attempt");
    const auto installed = installer.install(*reservation.reservation, payload);
    expect(installed.file.has_value(), "race fixture installs owned replay");
    if (!installed.file) {
      return;
    }
    const auto path = profile.path / installed.file->metadata.relativePath;
    bool replaced = false;
    replay::ReplayFileStore cleanup(
        profile.path, {.failAt = [&](std::string_view point) {
          if (point == "remove-before-quarantine") {
            std::error_code removeError;
            std::filesystem::remove(path, removeError);
            write(path, "racing replacement");
            replaced = true;
          }
          return false;
        }});

    std::string diagnostic;
    expect(!cleanup.removeIfMatches(installed.file->metadata, diagnostic) &&
               replaced && std::filesystem::exists(path),
           "automatic cleanup refuses a replacement moved into quarantine");
    std::ifstream input(path, std::ios::binary);
    const std::string preserved{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
    expect(preserved == "racing replacement",
           "automatic cleanup restores the quarantined replacement bytes");
  }
  {
    TempDirectory profile;
    replay::ReplayFileStore installer(profile.path);
    const auto reservation = installer.reserve(identity(), payload, "attempt");
    const auto installed = installer.install(*reservation.reservation, payload);
    const auto path = profile.path / installed.file->metadata.relativePath;
    replay::ReplayFileStore cleanup(
        profile.path, {.failAt = [&](std::string_view point) {
          if (point == "remove-after-quarantine") {
            write(path, "post-quarantine replacement");
          }
          return false;
        }});
    std::string diagnostic;
    expect(cleanup.removeIfMatches(installed.file->metadata, diagnostic),
           "cleanup removes only the owned inode after quarantine");
    std::ifstream input(path, std::ios::binary);
    const std::string preserved{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
    expect(preserved == "post-quarantine replacement",
           "cleanup never unlinks a replacement created after quarantine");
  }
  {
    TempDirectory profile;
    replay::ReplayFileStore installer(profile.path);
    const auto reservation = installer.reserve(identity(), payload, "attempt");
    const auto installed = installer.install(*reservation.reservation, payload);
    const auto path = profile.path / installed.file->metadata.relativePath;
    replay::ReplayFileStore interrupted(
        profile.path, {.failAt = [](std::string_view point) {
          return point == "remove-after-quarantine";
        }});
    std::string diagnostic;
    expect(!interrupted.removeIfMatches(installed.file->metadata, diagnostic) &&
               !std::filesystem::exists(path),
           "interrupted cleanup retains the owned inode in quarantine");
    replay::ReplayFileStore retry(profile.path);
    expect(retry.removeIfMatches(installed.file->metadata, diagnostic) &&
               !std::filesystem::exists(path) &&
               std::filesystem::is_empty(profile.path / "replay"),
           "restart resumes deterministic quarantined cleanup");
  }
}

#if !defined(_WIN32)
void testAutomaticCleanupHonorsInterprocessLease() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes payload = bytes("interprocess-owned replay");
  const auto reservation = store.reserve(identity(), payload, "attempt");
  const auto installed = store.install(*reservation.reservation, payload);
  expect(installed.file.has_value(),
         "interprocess cleanup fixture installs owned replay");
  if (!installed.file) {
    return;
  }
  const auto path = profile.path / installed.file->metadata.relativePath;

  int readyPipe[2]{};
  int releasePipe[2]{};
  if (::pipe(readyPipe) != 0 || ::pipe(releasePipe) != 0) {
    expect(false, "interprocess cleanup fixture creates synchronization pipes");
    return;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(readyPipe[0]);
    ::close(readyPipe[1]);
    ::close(releasePipe[0]);
    ::close(releasePipe[1]);
    expect(false, "interprocess cleanup fixture forks a lease holder");
    return;
  }
  if (child == 0) {
    ::close(readyPipe[0]);
    ::close(releasePipe[1]);
    const auto leasePath = profile.path / ".asobmashow-replay-cleanup.lock";
    const int lease = ::open(leasePath.c_str(), O_RDWR | O_CREAT, 0600);
    const bool acquired = lease >= 0 && ::flock(lease, LOCK_EX) == 0;
    const char ready = acquired ? '1' : '0';
    (void)::write(readyPipe[1], &ready, 1);
    char release = 0;
    (void)::read(releasePipe[0], &release, 1);
    if (acquired) {
      (void)::flock(lease, LOCK_UN);
    }
    if (lease >= 0) {
      (void)::close(lease);
    }
    ::close(readyPipe[1]);
    ::close(releasePipe[0]);
    _exit(acquired ? 0 : 1);
  }
  ::close(readyPipe[1]);
  ::close(releasePipe[0]);
  char ready = 0;
  const bool childLocked = ::read(readyPipe[0], &ready, 1) == 1 && ready == '1';
  std::string diagnostic;
  const bool removedWhileLocked =
      store.removeIfMatches(installed.file->metadata, diagnostic);
  expect(childLocked && !removedWhileLocked && std::filesystem::exists(path),
         "interprocess lease defers cleanup without moving or deleting bytes");
  const char release = '1';
  (void)::write(releasePipe[1], &release, 1);
  ::close(readyPipe[0]);
  ::close(releasePipe[1]);
  int status = 0;
  (void)::waitpid(child, &status, 0);
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "interprocess cleanup lease holder exits cleanly");
  expect(store.removeIfMatches(installed.file->metadata, diagnostic) &&
             !std::filesystem::exists(path),
         "cleanup resumes after the interprocess lease is released");
}
#endif

void testStaleCleanupOnlyRecognizesPrivateTemporaryGrammar() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const Bytes payload = bytes("payload");
  const auto reservation = store.reserve(identity(), payload, "attempt");
  std::filesystem::create_directories(
      profile.path /
      reservation.reservation->temporaryRelativePath.parent_path());
  const auto privatePath =
      profile.path / reservation.reservation->temporaryRelativePath;
  write(privatePath, "temporary");
  const auto neighbor = privatePath.parent_path() / "user-file.tmp";
  write(neighbor, "keep");
  const auto old =
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
  std::filesystem::last_write_time(privatePath, old);
  std::filesystem::last_write_time(neighbor, old);

  store.removeStaleTemporaryFiles(std::chrono::system_clock::now() -
                                  std::chrono::hours(1));
  expect(!std::filesystem::exists(privatePath) &&
             std::filesystem::exists(neighbor),
         "stale cleanup removes only proven private replay temp names");
  expect(
      replay::isPrivateReplayTemporaryFilename(
          reservation.reservation->temporaryRelativePath.filename().string()) &&
          !replay::isPrivateReplayTemporaryFilename("user-file.tmp"),
      "temporary filename grammar is narrow and testable");
}

void testStaleShareCleanupOnlyRemovesProvenPrivateSnapshots() {
  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const auto temporaryRoot = std::filesystem::temp_directory_path();
  std::string token(24, 'a');
  const std::string stamp = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  token.replace(
      token.size() - std::min(token.size(), stamp.size()),
      std::min(token.size(), stamp.size()),
      stamp.substr(stamp.size() - std::min(token.size(), stamp.size())));
  const auto stale = temporaryRoot / ("asobmashow-replay-share-" + token);
  token.front() = token.front() == 'a' ? 'b' : 'a';
  const auto recent = temporaryRoot / ("asobmashow-replay-share-" + token);
  token[1] = token[1] == 'a' ? 'b' : 'a';
  const auto unsafe = temporaryRoot / ("asobmashow-replay-share-" + token);
  const auto neighbor =
      temporaryRoot / ("asobmashow-replay-share-" + token + "-user");
  std::filesystem::create_directory(stale);
  std::filesystem::create_directory(recent);
  std::filesystem::create_directory(unsafe);
  std::filesystem::create_directory(neighbor);
  const auto replayName = identity().relativePath.filename();
  write(stale / replayName, "snapshot");
  write(recent / replayName, "snapshot");
  write(unsafe / replayName, "snapshot");
  write(unsafe / "unexpected.txt", "keep");
  write(neighbor / replayName, "keep");
  const auto old =
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
  std::filesystem::last_write_time(stale, old);
  std::filesystem::last_write_time(unsafe, old);
  std::filesystem::last_write_time(neighbor, old);

  store.removeStaleShareSnapshots(std::chrono::system_clock::now() -
                                  std::chrono::hours(1));
  expect(!std::filesystem::exists(stale) && std::filesystem::exists(recent) &&
             std::filesystem::exists(unsafe) &&
             std::filesystem::exists(neighbor),
         "stale share cleanup removes only old proven private snapshots");
  expect(replay::isPrivateReplayShareDirectoryName(stale.filename().string()) &&
             !replay::isPrivateReplayShareDirectoryName(
                 neighbor.filename().string()),
         "share snapshot directory grammar is narrow and testable");

  std::error_code ignored;
  std::filesystem::remove_all(recent, ignored);
  std::filesystem::remove_all(unsafe, ignored);
  std::filesystem::remove_all(neighbor, ignored);
}

} // namespace

int main() {
  testInstallInspectRetryAndOccupiedRefusal();
  testContainmentAndSymlinksFailClosed();
  testFaultsAreRecoverableAtExactReservation();
  testCorruptUserDeletionAndAutomaticOwnershipGuard();
  testReservationCleanupRequiresExactOwnedMetadata();
  testAutomaticCleanupCannotUnlinkARacingReplacement();
#if !defined(_WIN32)
  testAutomaticCleanupHonorsInterprocessLease();
#endif
  testStaleCleanupOnlyRecognizesPrivateTemporaryGrammar();
  testStaleShareCleanupOnlyRemovesProvenPrivateSnapshots();
  if (failures != 0) {
    std::cerr << failures << " replay file store test(s) failed\n";
    return 1;
  }
  std::cout << "replay file store tests passed\n";
  return 0;
}
