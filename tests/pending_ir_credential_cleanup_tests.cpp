#include "ir/PendingIrCredentialCleanup.h"
#include "targets.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if TARGET_OS_ANDROID
namespace {
std::string androidInternalFilesDirectory;
}

std::string GetAndroidInternalFilesDir() {
  return androidInternalFilesDirectory;
}
#endif

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-pending-ir-cleanup-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
    std::filesystem::create_directories(path);
#if TARGET_OS_ANDROID
    androidInternalFilesDirectory = (path / "private-files").string();
#endif
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

void testFailedProfileDeletionCancelsCleanupWithoutTouchingCredentials() {
  TemporaryDirectory temp;
  ir::PendingIrCredentialCleanup pending(temp.path);
  int credentialRemovals = 0;
  const auto result = ir::coordinateProfileCredentialDeletion(
      pending, "profile-a",
      [](std::string &diagnostic) {
        diagnostic = "profile deletion failed";
        return false;
      },
      [&](std::string &) {
        ++credentialRemovals;
        return true;
      });

  std::string diagnostic;
  expect(result.status ==
             ir::ProfileCredentialDeletionStatus::ProfileDeletionFailed,
         "profile deletion failure is preserved");
  expect(credentialRemovals == 0,
         "credentials remain untouched when the profile still exists");
  expect(pending.pending(diagnostic).empty(),
         "failed profile deletion cancels its pending cleanup marker");
}

void testFailedCredentialRemovalPersistsAndRetriesAfterRestart() {
  TemporaryDirectory temp;
  constexpr std::string_view profileId = "profile-b";
  {
    ir::PendingIrCredentialCleanup pending(temp.path);
    const auto result = ir::coordinateProfileCredentialDeletion(
        pending, profileId, [](std::string &) { return true; },
        [](std::string &diagnostic) {
          diagnostic = "transient Keychain failure";
          return false;
        });
    expect(result.status ==
               ir::ProfileCredentialDeletionStatus::CredentialCleanupPending,
           "failed secure cleanup reports a retryable pending state");
#if !TARGET_OS_ANDROID
    expect(std::filesystem::is_regular_file(
               temp.path / ".pending-ir-credential-cleanup" / "profile-b.pending"),
           "non-Android deletion retry state remains under application data");
#endif
  }

  ir::PendingIrCredentialCleanup afterRestart(temp.path);
  std::string diagnostic;
  expect(afterRestart.pending(diagnostic) ==
             std::vector<std::string>{std::string(profileId)},
         "a new queue instance observes the durable cleanup marker");

  int credentialRemovals = 0;
  const auto retry = ir::retryPendingProfileCredentialCleanup(
      afterRestart, [](std::string_view) { return false; },
      [&](std::string_view id, std::string &) {
        ++credentialRemovals;
        return id == profileId;
      });
  expect(retry.completed == 1 && retry.retained == 0 &&
             credentialRemovals == 1,
         "restart retry removes credentials for the absent profile");
  expect(afterRestart.pending(diagnostic).empty(),
         "successful retry durably clears the marker");
}

void testRetryNeverRemovesCredentialsForLiveProfile() {
  TemporaryDirectory temp;
  ir::PendingIrCredentialCleanup pending(temp.path);
  std::string diagnostic;
  expect(pending.schedule("profile-c", diagnostic),
         "live-profile fixture schedules a cleanup marker");

  int credentialRemovals = 0;
  const auto retry = ir::retryPendingProfileCredentialCleanup(
      pending, [](std::string_view id) { return id == "profile-c"; },
      [&](std::string_view, std::string &) {
        ++credentialRemovals;
        return true;
      });
  expect(retry.cancelled == 1 && retry.completed == 0 &&
             credentialRemovals == 0,
         "startup recovery cancels a pre-delete marker for a live profile");
  expect(pending.pending(diagnostic).empty(),
         "the cancelled live-profile marker does not linger");
}

void testOverwriteResetRetriesForAReplacedLiveProfile() {
  TemporaryDirectory temp;
  constexpr std::string_view profileId = "profile-d";
  const auto profiles = temp.path / "profiles";
  const auto staging = profiles / ".staging-profile-d";
  const auto installed = profiles / profileId;
  std::filesystem::create_directories(staging);

  {
    ir::PendingIrCredentialCleanup pending(temp.path);
    std::string diagnostic;
    expect(pending.scheduleOverwriteReset(profileId, diagnostic),
           "overwrite staging records a durable credential reset marker");
    std::filesystem::rename(staging, installed);
    expect(pending.pendingOverwriteResets(diagnostic) ==
               std::vector<std::string>{std::string(profileId)},
           "the marker moves atomically with the replacement profile");

    const auto cleanup = ir::finishProfileCredentialOverwriteCleanup(
        pending, profileId, [](std::string &error) {
          error = "transient Keychain failure";
          return false;
        });
    expect(cleanup.status ==
               ir::ProfileCredentialOverwriteCleanupStatus::CleanupPending,
           "a transient overwrite cleanup failure remains retryable");
    expect(pending.pendingOverwriteResets(diagnostic) ==
               std::vector<std::string>{std::string(profileId)},
           "failed overwrite cleanup retains its committed marker");
  }

  ir::PendingIrCredentialCleanup afterRestart(temp.path);
  int credentialRemovals = 0;
  const auto retry = ir::retryPendingProfileCredentialOverwriteCleanup(
      afterRestart,
      [&](std::string_view id, std::string &) {
        ++credentialRemovals;
        return id == profileId;
      });
  std::string diagnostic;
  expect(retry.completed == 1 && retry.retained == 0 &&
             credentialRemovals == 1,
         "restart cleanup removes stale credentials despite the live profile");
  expect(afterRestart.pendingOverwriteResets(diagnostic).empty(),
         "successful overwrite cleanup removes the embedded marker");
}

#if TARGET_OS_ANDROID
void testAndroidDeletionRetrySurvivesRemovingPublicDocuments() {
  for (const bool internalDocumentsFallback : {false, true}) {
    TemporaryDirectory temp;
    const auto privateFiles = temp.path / "private-files";
    const auto documents =
        (internalDocumentsFallback ? privateFiles : temp.path / "external-files") /
        "Documents";
    const auto staging = documents / "profiles" / ".staging-profile-replaced";
    std::filesystem::create_directories(staging);
    std::string diagnostic;
    {
      ir::PendingIrCredentialCleanup pending(documents);
      const auto deletion = ir::coordinateProfileCredentialDeletion(
          pending, "profile-deleted", [](std::string &) { return true; },
          [](std::string &) { return false; });
      expect(deletion.status ==
                 ir::ProfileCredentialDeletionStatus::CredentialCleanupPending,
             "Android credential removal failure schedules a durable retry");
      expect(std::filesystem::is_regular_file(
                 privateFiles / ".pending-ir-credential-cleanup" /
                 "profile-deleted.pending"),
             "Android deletion retry state is stored in private internal files");
      expect(!std::filesystem::exists(
                 documents / ".pending-ir-credential-cleanup"),
             "Android deletion retry state is not exposed through Documents");
      expect(pending.scheduleOverwriteReset("profile-replaced", diagnostic),
             "private deletion queue still stages overwrite resets with profiles");
      expect(pending.pendingOverwriteResets(diagnostic).empty(),
             "staged overwrite resets remain invisible until publication");
      std::filesystem::rename(staging,
                              documents / "profiles" / "profile-replaced");
      expect(pending.pendingOverwriteResets(diagnostic) ==
                 std::vector<std::string>{"profile-replaced"},
             "overwrite reset publishes atomically with the public profile");
    }

    std::filesystem::remove_all(documents);
    ir::PendingIrCredentialCleanup afterRestart(documents);
    const auto retry = ir::retryPendingProfileCredentialCleanup(
        afterRestart, [](std::string_view) { return false; },
        [](std::string_view id, std::string &) {
          return id == "profile-deleted";
        });
    expect(retry.completed == 1 && retry.retained == 0,
           "deleting exposed Documents cannot lose private credential retries");
    expect(afterRestart.pending(diagnostic).empty() && diagnostic.empty(),
           "successful Android cleanup removes the private retry marker");
  }
}

void testAndroidMissingPrivateStorageFailsClosed() {
  TemporaryDirectory temp;
  androidInternalFilesDirectory.clear();
  ir::PendingIrCredentialCleanup pending(temp.path);
  bool profileDeleted = false;
  const auto deletion = ir::coordinateProfileCredentialDeletion(
      pending, "profile-unavailable",
      [&](std::string &) {
        profileDeleted = true;
        return true;
      },
      [](std::string &) { return true; });
  expect(deletion.status == ir::ProfileCredentialDeletionStatus::QueueFailed &&
             !deletion.diagnostic.empty() && !profileDeleted,
         "unavailable private storage blocks deletion before credentials can leak");
  expect(!std::filesystem::exists(temp.path / ".pending-ir-credential-cleanup"),
         "unavailable private storage never falls back to public Documents");
  std::string diagnostic;
  expect(pending.pending(diagnostic).empty() && !diagnostic.empty(),
         "unavailable private storage reports retry discovery failure");
  expect(!pending.complete("profile-unavailable", diagnostic) &&
             !diagnostic.empty(),
         "unavailable private storage cannot report cleanup completion");
}
#endif
} // namespace

int main() {
  testFailedProfileDeletionCancelsCleanupWithoutTouchingCredentials();
  testFailedCredentialRemovalPersistsAndRetriesAfterRestart();
  testRetryNeverRemovesCredentialsForLiveProfile();
  testOverwriteResetRetriesForAReplacedLiveProfile();
#if TARGET_OS_ANDROID
  testAndroidDeletionRetrySurvivesRemovingPublicDocuments();
  testAndroidMissingPrivateStorageFailsClosed();
#endif
  if (failures != 0) {
    std::cerr << failures << " pending IR cleanup test(s) failed\n";
    return 1;
  }
  std::cout << "Pending IR credential cleanup tests passed\n";
  return 0;
}
