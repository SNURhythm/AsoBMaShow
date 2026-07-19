#pragma once

#include "IrDriver.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct IrSavedResultBatchUploadDependencies {
  std::function<BuildDraftOutcome(const IrSubmission &)> buildDraft;
  std::function<IrManualBatchEnqueueOutcome(
      std::span<const IrOutboxDraft>)>
      enqueueBatch;
};

struct IrSavedResultBatchUploadResult {
  std::vector<IrManualBatchItemOutcome> items;
  std::size_t buildFailures = 0;
  std::string diagnostic;
};

[[nodiscard]] IrSavedResultBatchUploadResult executeIrSavedResultBatchUpload(
    std::string_view providerId, std::span<const IrSubmission> submissions,
    const IrSavedResultBatchUploadDependencies &dependencies) noexcept;

} // namespace ir
