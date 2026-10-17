#pragma once
#include "View.h"
#include "../storage/StoragePolicy.h"
#include <functional>
#include <vector>
class Button;
class TextView;

// A value editor. Its owner decides how and when to persist the policy.
class StoragePolicyView : public View {
public:
  using Policy = storage_management::StoragePolicy;
  StoragePolicyView(Policy, std::function<void(Policy)> onChange);
  void SetPolicy(Policy);
  void SetEnabled(bool);
  void onLanguageChanged() override;
private:
  void Refresh();
  Policy policy_;
  bool enabled_ = true;
  std::vector<Button *> buttons_;
  std::function<void(Policy)> onChange_;
  std::vector<TextView *> values_;
};
