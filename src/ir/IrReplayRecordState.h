#pragma once

#include "IrReceiptModels.h"
#include "../repositories/ReplayRepository.h"

namespace ir {

void resolveReplayIrRecordState(
    ReplaySummary &summary,
    IrRecordActivity activity = IrRecordActivity::None) noexcept;

} // namespace ir
