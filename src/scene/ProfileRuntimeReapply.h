#pragma once

#include "../ProfileSessionCoordinator.h"

#include <functional>
#include <string>
#include <vector>

enum class ProfileDisplayRuntimeOutcome { Applied, PreviewPending, Failed };

struct ProfileDisplayRuntimeResult {
  ProfileDisplayRuntimeOutcome outcome = ProfileDisplayRuntimeOutcome::Applied;
  std::string message;
};

struct ProfileRuntimeReapplyCallbacks {
  std::function<void()> sanitize;
  std::function<void()> applyTheme;
  std::function<void()> applyJukebox;
  std::function<std::string()> applyMetadata;
  std::function<std::string()> applyAudio;
  std::function<void()> refreshDrafts;
  std::function<ProfileDisplayRuntimeResult()> applyDisplay;
};

struct ProfileRuntimeReapplyResult {
  bool profileCommitted = false;
  std::vector<std::string> warnings;
};

ProfileRuntimeReapplyResult ReapplyProfileRuntimeAfterSwitch(
    const ProfileSwitchResult &switchResult,
    const ProfileRuntimeReapplyCallbacks &callbacks);
