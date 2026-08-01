#pragma once

#include "IrDriver.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

namespace detail {

class IrManualBatchOutcomeIndex {
public:
  explicit IrManualBatchOutcomeIndex(
      std::span<const IrManualBatchItemOutcome> outcomes);

  [[nodiscard]] std::optional<std::size_t>
  findUnique(std::string_view attemptId);
  [[nodiscard]] std::size_t operationCount() const noexcept {
    return operationCount_;
  }

private:
  struct AttemptIdHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  struct IndexedOutcome {
    std::size_t index = 0;
    bool duplicate = false;
  };

  std::unordered_map<std::string, IndexedOutcome, AttemptIdHash,
                     std::equal_to<>>
      outcomes_;
  std::size_t operationCount_ = 0;
};

} // namespace detail

[[nodiscard]] IrSavedResultBatchUploadResult executeIrSavedResultBatchUpload(
    std::string_view providerId, std::span<const IrSubmission> submissions,
    const IrSavedResultBatchUploadDependencies &dependencies) noexcept;

} // namespace ir
