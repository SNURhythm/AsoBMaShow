#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace settings_scene {

enum class ProfileInlineEditAction { Rename, Duplicate };

struct ProfileInlineEditRequest {
  ProfileInlineEditAction action = ProfileInlineEditAction::Rename;
  std::string profileId;
  std::string name;
};

class ProfileInlineEditorState {
public:
  void beginRename(std::string_view profileId, std::string_view currentName) {
    begin(ProfileInlineEditAction::Rename, profileId, currentName);
  }

  void beginDuplicate(std::string_view profileId,
                      std::string_view currentName) {
    begin(ProfileInlineEditAction::Duplicate, profileId,
          std::string(currentName) + " Copy");
  }

  [[nodiscard]] bool active() const { return action_.has_value(); }
  [[nodiscard]] bool activeFor(std::string_view profileId) const {
    return active() && profileId_ == profileId;
  }
  [[nodiscard]] std::string_view draft() const { return draft_; }

  void updateDraft(std::string draft) {
    if (active()) {
      draft_ = std::move(draft);
    }
  }

  [[nodiscard]] std::optional<ProfileInlineEditRequest>
  requestFor(std::string_view profileId) const {
    if (!activeFor(profileId)) {
      return std::nullopt;
    }
    return ProfileInlineEditRequest{*action_, profileId_, draft_};
  }

  void clearIfTargetUnavailable(bool targetAvailable) {
    if (active() && !targetAvailable) {
      clear();
    }
  }

  void clear() {
    action_.reset();
    profileId_.clear();
    draft_.clear();
  }

private:
  void begin(ProfileInlineEditAction action, std::string_view profileId,
             std::string_view draft) {
    action_ = action;
    profileId_ = profileId;
    draft_ = draft;
  }

  std::optional<ProfileInlineEditAction> action_;
  std::string profileId_;
  std::string draft_;
};

} // namespace settings_scene
