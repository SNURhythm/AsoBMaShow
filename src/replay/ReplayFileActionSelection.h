#pragma once

#include "ReplayFileActionService.h"

#include "../ResultRecordSummary.h"

#include <optional>
#include <utility>

namespace replay {

struct ReplayFileActionSelection {
  std::optional<ReplayFileActionRequest> request;
  bool shareVisible = false;
  bool deleteVisible = false;
  bool enabled = false;
};

class ReplayFileDeleteConfirmation {
public:
  [[nodiscard]] bool begin(
      const ReplayFileActionSelection &selection) noexcept {
    pending_.reset();
    if (!selection.enabled || !selection.deleteVisible ||
        !selection.request.has_value()) {
      return false;
    }
    pending_ = selection.request;
    return true;
  }

  [[nodiscard]] bool active() const noexcept { return pending_.has_value(); }

  [[nodiscard]] const ReplayFileActionRequest *request() const noexcept {
    return pending_ ? &*pending_ : nullptr;
  }

  void cancel() noexcept { pending_.reset(); }

  [[nodiscard]] std::optional<ReplayFileActionRequest> confirm() noexcept {
    auto request = std::move(pending_);
    pending_.reset();
    return request;
  }

private:
  std::optional<ReplayFileActionRequest> pending_;
};

[[nodiscard]] ReplayFileActionSelection
replayFileActionSelection(const ResultRecordSummary &summary,
                          bool interactive) noexcept;

} // namespace replay
