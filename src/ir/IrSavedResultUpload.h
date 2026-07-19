#pragma once

#include "IrDriver.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ir {

enum class IrSavedResultUploadState {
  Queued,
  RetryQueued,
  AlreadyActive,
  AlreadySubmitted,
  Failed,
};

struct IrSavedResultUploadResult {
  IrSavedResultUploadState state = IrSavedResultUploadState::Failed;
  bool accepted = false;
  std::string message;
};

struct IrSavedResultUploadDependencies {
  std::function<IrOutboxReadOutcome(std::string_view providerId,
                                    std::string_view attemptId)>
      loadOutbox;
  std::function<BuildDraftOutcome(const IrSubmission &)> buildDraft;
  std::function<IrOutboxInsertOutcome(const IrOutboxDraft &)> enqueue;
  std::function<IrOutboxMutationOutcome(std::int64_t rowId)> retry;
};

[[nodiscard]] IrSavedResultUploadResult executeIrSavedResultUpload(
    std::string_view providerId, const IrSubmission &submission,
    const IrSavedResultUploadDependencies &dependencies) noexcept;

} // namespace ir
