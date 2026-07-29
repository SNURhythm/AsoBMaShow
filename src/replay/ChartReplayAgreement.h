#pragma once

#include "BeatorajaReplayCodec.h"

#include "../ModernResult.h"

#include <string>

namespace replay {

enum class ChartReplayAgreementIssue {
  None,
  ChartIdentity,
  LongNoteMode,
  SharedSetup,
};

struct ChartReplayAgreement {
  ChartReplayAgreementIssue issue = ChartReplayAgreementIssue::None;
  std::string diagnostic;

  [[nodiscard]] bool agrees() const noexcept {
    return issue == ChartReplayAgreementIssue::None;
  }
};

[[nodiscard]] ChartReplayAgreement compareChartReplayToResult(
    const ReplayChartDocument &replay,
    const result_persistence::ModernChartResult &result) noexcept;

} // namespace replay
