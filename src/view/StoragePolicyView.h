#pragma once
#include "View.h"
#include "../storage/StoragePolicy.h"
#include <functional>
#include <vector>
class DropdownView;
class OverlayPortal;

// A value editor. Its owner decides how and when to persist the policy.
class StoragePolicyView : public View {
public:
  using Policy = storage_management::StoragePolicy;
  StoragePolicyView(Policy, std::function<void(Policy)> onChange, OverlayPortal *portal = nullptr);
  void SetPolicy(Policy);
  void SetEnabled(bool);
  void onLanguageChanged() override;
private:
  void Refresh();
  Policy policy_;
  bool enabled_ = true;
  int openDropdown_ = -1;
  std::vector<DropdownView *> dropdowns_;
  std::function<void(Policy)> onChange_;
};
