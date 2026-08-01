#include "ProfileSettingsController.h"

#include "../context.h"

#include <atomic>
#include <utility>

namespace {
ProfileSettingsControllerDependencies
applicationDependencies(ApplicationContext &context) {
  return {
      .listProfiles = [&context]() {
        if (!context.profileReady()) {
          return ProfileListResult{
              .error = ProfileError::SwitchBlocked,
              .message = context.profileInitializationResult.message.empty()
                             ? "Player profiles are not initialized."
                             : context.profileInitializationResult.message};
        }
        return ProfileListResult{
            .profiles = context.profileManager.listProfiles(),
            .activeProfileId = context.profileManager.activeProfile().id};
      },
      .create = [&context](std::string name) {
        return context.profileManager.createProfile(std::move(name));
      },
      .rename = [&context](std::string_view profileId, std::string name) {
        return context.profileManager.renameProfile(profileId, std::move(name));
      },
      .duplicate = [&context](std::string_view profileId, std::string name) {
        return context.profileManager.duplicateProfile(profileId,
                                                       std::move(name));
      },
      .remove = [&context](std::string_view profileId) {
        ProfileResult deleted{.error = ProfileError::IoFailure};
        const auto coordinated = ir::coordinateProfileCredentialDeletion(
            context.pendingIrCredentialCleanup, profileId,
            [&context, profileId, &deleted](std::string &diagnostic) {
              deleted = context.profileManager.deleteProfile(profileId);
              diagnostic = deleted.message;
              return deleted.ok();
            },
            [&context, profileId](std::string &diagnostic) {
              return context.removeProfileIrCredentials(profileId,
                                                        diagnostic);
            });
        if (coordinated.status ==
            ir::ProfileCredentialDeletionStatus::QueueFailed) {
          return ProfileResult{
              .error = ProfileError::IoFailure,
              .message = coordinated.diagnostic.empty()
                             ? "Secure credential cleanup could not be queued; "
                               "the profile was not deleted."
                             : coordinated.diagnostic};
        }
        if (coordinated.status ==
            ir::ProfileCredentialDeletionStatus::ProfileDeletionFailed) {
          if (!coordinated.diagnostic.empty()) {
            deleted.message = coordinated.diagnostic;
          }
          return deleted;
        }
        if (coordinated.status ==
            ir::ProfileCredentialDeletionStatus::CredentialCleanupPending) {
          deleted.message = coordinated.diagnostic.empty()
                                ? "The profile was deleted; secure IR "
                                  "credential cleanup will retry."
                                : coordinated.diagnostic +
                                      " Cleanup will retry automatically.";
        }
        return deleted;
      },
      .activate = [&context](std::string_view profileId) {
        return context.switchProfile(profileId);
      },
      .exportProfile =
          [&context](std::string_view profileId,
                     const std::filesystem::path &destination) {
            if (!context.profileArchiveOperationActive.load(
                    std::memory_order_acquire)) {
              return ProfileArchiveResult{
                  .error = ProfileError::SwitchBlocked,
                  .message = "The profile archive pipeline is not active."};
            }
            ProfileArchiveService service(context.profileManager);
            return service.Export(profileId, destination);
          },
      .importProfile =
          [&context](const std::filesystem::path &archive,
                     const ProfileImportOptions &options) {
            if (!context.profileArchiveOperationActive.load(
                    std::memory_order_acquire)) {
              return ProfileArchiveResult{
                  .error = ProfileError::SwitchBlocked,
                  .message = "The profile archive pipeline is not active."};
            }
            ProfileArchiveService service(context.profileManager);
            auto imported = service.Import(archive, options);
            if (!imported.ok() || !imported.profile ||
                options.mode != ProfileImportMode::Overwrite ||
                !options.overwriteProfileId) {
              return imported;
            }
            const auto cleanup =
                ir::finishProfileCredentialOverwriteCleanup(
                    context.pendingIrCredentialCleanup,
                    *options.overwriteProfileId,
                    [&context, &options](std::string &diagnostic) {
                      return context.removeProfileIrCredentials(
                          *options.overwriteProfileId, diagnostic);
                    });
            if (cleanup.status ==
                ir::ProfileCredentialOverwriteCleanupStatus::CleanupPending) {
              if (!imported.message.empty()) {
                imported.message += "; ";
              }
              imported.message += cleanup.diagnostic.empty()
                                      ? "secure IR credential cleanup will "
                                        "retry automatically"
                                      : cleanup.diagnostic +
                                            " Cleanup will retry "
                                            "automatically.";
            }
            return imported;
          },
      .flushSettings = [&context](std::string &errorMessage) {
        return context.saveSettings(&errorMessage);
      },
      .flushInput = [&context](std::string &errorMessage) {
        if (!context.profileReady()) {
          errorMessage = context.profileInitializationResult.message.empty()
                             ? "Player profiles are not initialized."
                             : context.profileInitializationResult.message;
          return false;
        }
        return context.saveActiveInputProfile(context.inputProfile,
                                              errorMessage);
      },
      .beginArchivePipeline = [&context](std::string &errorMessage) {
        bool expected = false;
        if (context.profileArchiveOperationActive.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          return true;
        }
        errorMessage = "Another profile archive operation is active.";
        return false;
      },
      .endArchivePipeline = [&context]() {
        context.profileArchiveOperationActive.store(false,
                                                    std::memory_order_release);
      }};
}
} // namespace

ProfileSettingsController::ProfileSettingsController(
    ApplicationContext &context)
    : ProfileSettingsController(applicationDependencies(context)) {}
