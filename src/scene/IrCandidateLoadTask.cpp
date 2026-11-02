#include "IrCandidateLoadTask.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ir_uploads {
CandidateLoadTask::~CandidateLoadTask() { reset(); }

bool CandidateLoadTask::start(Reader reader) {
  if (worker_.joinable()) return false;
  worker_ = std::jthread([this, reader = std::move(reader)](std::stop_token stop) {
    Result result;
    try {
      std::optional<int> before;
      do {
        if (stop.stop_requested()) return;
        // Bound cancellation latency and repository lock ownership per page.
        auto page = reader(before, 32);
        if (stop.stop_requested()) return;
        if (page.status != ir::IrUploadCandidateReadStatus::Loaded) {
          result.error = page.diagnostic.empty()
              ? i18n::message("ir.upload.saved_results_failed_loaded.message")
              : i18n::Text(ir::sanitizeDiagnostic(page.diagnostic));
          break;
        }
        if (result.diagnostic.empty() && !page.diagnostic.empty())
          result.diagnostic = page.diagnostic;
        const auto accepted = std::min(page.candidates.size(),
            ir::kMaximumIrUploadCandidateRows - result.candidates.size());
        result.candidates.insert(result.candidates.end(),
            std::make_move_iterator(page.candidates.begin()),
            std::make_move_iterator(page.candidates.begin() + accepted));
        if (accepted != page.candidates.size() ||
            (result.candidates.size() == ir::kMaximumIrUploadCandidateRows &&
             page.nextBeforeModernChartResultId)) {
          if (result.diagnostic.empty())
            result.diagnostic = i18n::message("ir.upload.only_newest_saved_ir_candidates_shown.message");
          break;
        }
        if (page.nextBeforeModernChartResultId &&
            (*page.nextBeforeModernChartResultId <= 0 ||
             (before && *page.nextBeforeModernChartResultId >= *before))) {
          result.error = i18n::message("ir.upload.pagination_failed.message");
          break;
        }
        before = page.nextBeforeModernChartResultId;
      } while (before);
    } catch (...) {
      result.error = i18n::message("ir.upload.saved_results_failed_loaded.message");
    }
    if (!result.error.empty()) result.candidates.clear();
    std::lock_guard lock(mutex_);
    pending_ = std::move(result);
  });
  return true;
}

std::optional<CandidateLoadTask::Result> CandidateLoadTask::takeResult() {
  std::optional<Result> result;
  {
    std::lock_guard lock(mutex_);
    result = std::exchange(pending_, {});
  }
  if (result && worker_.joinable()) worker_.join();
  return result;
}

void CandidateLoadTask::reset() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  std::lock_guard lock(mutex_);
  pending_.reset();
}
} // namespace ir_uploads
