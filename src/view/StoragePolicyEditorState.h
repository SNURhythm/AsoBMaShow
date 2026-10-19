#pragma once
#include "../storage/StoragePolicy.h"

// The acknowledgment belongs to policy editing, never to individual downloads.
class DownloadStoragePolicyEditorState {
public:
  bool AwaitingAcknowledgment() const { return awaitingAcknowledgment_; }
  void ChangePolicy(storage_management::StoragePreferences &preferences,
                    storage_management::StoragePolicy policy) {
    using storage_management::BgaAction;
    if (policy.bga == BgaAction::Delete && preferences.download.bga != BgaAction::Delete)
      preferences.afterDownload = false;
    preferences.download = policy;
    awaitingAcknowledgment_ = false;
  }
  // False means the first destructive enable click only armed confirmation.
  bool ToggleAutomation(storage_management::StoragePreferences &preferences) {
    if (!preferences.afterDownload &&
        preferences.download.bga == storage_management::BgaAction::Delete &&
        !awaitingAcknowledgment_) {
      awaitingAcknowledgment_ = true;
      return false;
    }
    preferences.afterDownload = !preferences.afterDownload;
    awaitingAcknowledgment_ = false;
    return true;
  }
private:
  bool awaitingAcknowledgment_ = false;
};
