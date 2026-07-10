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
        return context.profileManager.deleteProfile(profileId);
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
            return service.Import(archive, options);
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
