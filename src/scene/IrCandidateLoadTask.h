#pragma once

#include "IrUploadsController.h"

namespace ir_uploads {

// Application-thread owner. The reader/repository must remain valid until reset.
class CandidateLoadTask final {
public:
  struct Result {
    std::vector<ir::IrUploadCandidate> candidates;
    i18n::Text error, diagnostic;
  };
  using Reader = std::function<ir::IrUploadCandidateReadOutcome(std::optional<int>, std::size_t)>;
  ~CandidateLoadTask();
  bool start(Reader reader);
  bool hasWorker() const noexcept { return worker_.joinable(); }
  std::optional<Result> takeResult();
  void requestStop() { worker_.request_stop(); }
  void reset();

private:
  std::mutex mutex_;
  std::optional<Result> pending_;
  std::jthread worker_;
};
} // namespace ir_uploads
