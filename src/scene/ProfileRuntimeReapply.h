#pragma once

#include "../ProfileSessionCoordinator.h"
#include "../i18n/Localization.h"

#include <functional>
#include <string>
#include <vector>

enum class ProfileDisplayRuntimeOutcome { Applied, PreviewPending, Failed };

struct ProfileDisplayRuntimeResult {
  ProfileDisplayRuntimeOutcome outcome = ProfileDisplayRuntimeOutcome::Applied;
  i18n::Text message;
};

struct ProfileRuntimeReapplyCallbacks {
  std::function<void()> sanitize;
  std::function<void()> applyTheme;
  std::function<void()> applyJukebox;
  std::function<i18n::Text()> applyMetadata;
  std::function<i18n::Text()> applyAudio;
  std::function<void()> refreshDrafts;
  std::function<ProfileDisplayRuntimeResult()> applyDisplay;
};

struct ProfileRuntimeReapplyResult {
  bool profileCommitted = false;
  std::vector<i18n::Text> warnings;
};

ProfileRuntimeReapplyResult ReapplyProfileRuntimeAfterSwitch(
    const ProfileSwitchResult &switchResult,
    const ProfileRuntimeReapplyCallbacks &callbacks);
