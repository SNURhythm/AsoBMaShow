#pragma once
#include "View.h"
#include "../storage/StoragePolicy.h"
#include "../storage/StorageVideoCapabilities.h"
#include <functional>
#include <future>
#include <vector>
class DropdownView;
class OverlayPortal;
class Button;
class ToggleButtonContent;
class TextView;
class StorageCodecModal;

// A value editor. Its owner decides how and when to persist the policy.
class StoragePolicyView : public View {
public:
  using Policy = storage_management::StoragePolicy;
  StoragePolicyView(Policy, std::function<void(Policy)> onChange, OverlayPortal *portal = nullptr);
  ~StoragePolicyView() override;
  void SetPolicy(Policy);
  void SetEnabled(bool);
  void onLanguageChanged() override;
private:
  void Refresh();
  void PollCapabilities();
  void OpenCodecModal();
  void CloseCodecModal();
  void renderImpl(RenderContext &) override;
  bool handleEventsImpl(SDL_Event &) override;
  Policy policy_;
  bool enabled_ = true;
  int openDropdown_ = -1;
  std::vector<DropdownView *> dropdowns_;
  OverlayPortal *portal_ = nullptr;
  std::unique_ptr<StorageCodecModal> codecModal_;
  Button *codecButton_ = nullptr;
  TextView *codecButtonText_ = nullptr;
  Button *efficientVideoButton_ = nullptr;
  ToggleButtonContent *efficientVideoContent_ = nullptr;
  TextView *codecStatus_ = nullptr;
  std::shared_future<storage_management::StorageVideoCapabilities> capabilitiesFuture_;
  storage_management::StorageVideoCapabilities capabilities_{};
  bool capabilitiesReady_ = false;
  std::function<void(Policy)> onChange_;
};
