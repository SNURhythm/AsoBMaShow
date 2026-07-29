#pragma once

#include "../ir/IrUploadCandidates.h"
#include "Button.h"
#include "CheckboxButtonContent.h"
#include "ImageView.h"
#include "RecyclerView.h"
#include "TextView.h"

#include <functional>
#include <unordered_set>
#include <vector>

class IrUploadCandidateListItemView final : public View {
public:
  IrUploadCandidateListItemView();

  void setCandidate(const ir::IrUploadCandidate &candidate, bool selected,
                    bool selectionLocked,
                    std::function<void(std::string)> selectionToggle);
  [[nodiscard]] bool hasClearLamp() const noexcept { return hasClearLamp_; }

private:
  View *clearLamp_ = nullptr;
  Button *selectionButton_ = nullptr;
  CheckboxButtonContent *selectionContent_ = nullptr;
  View *artworkFrame_ = nullptr;
  ImageView *jacketImage_ = nullptr;
  View *textColumn_ = nullptr;
  TextView *titleText_ = nullptr;
  TextView *artistText_ = nullptr;
  TextView *attemptText_ = nullptr;
  View *difficultyColumn_ = nullptr;
  TextView *difficultyText_ = nullptr;
  TextView *keyModeText_ = nullptr;
  View *scoreColumn_ = nullptr;
  TextView *scoreText_ = nullptr;
  TextView *rankText_ = nullptr;
  TextView *statusText_ = nullptr;
  bool hasClearLamp_ = false;
};

class IrUploadCandidateListView : public RecyclerView<ir::IrUploadCandidate> {
public:
  IrUploadCandidateListView();

  void setCandidates(const std::vector<ir::IrUploadCandidate> &candidates,
                     const std::unordered_set<std::string> &selectedAttemptIds);
  void setSelectedAttemptIds(
      const std::unordered_set<std::string> &selectedAttemptIds);
  void setSelectionLocked(bool locked);

  std::function<void(std::string attemptId)> onSelectionToggle;

private:
  std::unordered_set<std::string> selectedAttemptIds_;
  bool selectionLocked_ = false;
};
