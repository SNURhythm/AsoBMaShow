#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileStore.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

} // namespace

int main() {
  testInstallInspectRetryAndOccupiedRefusal();
  testContainmentAndSymlinksFailClosed();
  testFaultsAreRecoverableAtExactReservation();
  testCorruptUserDeletionAndAutomaticOwnershipGuard();
  testStaleCleanupOnlyRecognizesPrivateTemporaryGrammar();
  if (failures != 0) {
    std::cerr << failures << " replay file store test(s) failed\n";
    return 1;
  }
  std::cout << "replay file store tests passed\n";
  return 0;
}
