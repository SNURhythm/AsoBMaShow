#include "ir/PendingIrCredentialCleanup.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
} // namespace

int main() {
  testFailedProfileDeletionCancelsCleanupWithoutTouchingCredentials();
  testFailedCredentialRemovalPersistsAndRetriesAfterRestart();
  testRetryNeverRemovesCredentialsForLiveProfile();
  if (failures != 0) {
    std::cerr << failures << " pending IR cleanup test(s) failed\n";
    return 1;
  }
  std::cout << "Pending IR credential cleanup tests passed\n";
  return 0;
}
