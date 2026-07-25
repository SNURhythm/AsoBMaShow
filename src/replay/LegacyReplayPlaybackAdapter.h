#pragma once

#include "ReplayPlaybackData.h"
#include "ReplayPlaybackMaterializer.h"
#include "../ReplayData.h"
#include "../ResultPersistenceModel.h"

#include <optional>

namespace replay {

// Migration-only visual playback adapter. The returned ReplayData must never
// be used to construct persisted results or IR submissions.
[[nodiscard]] std::optional<ReplayData> makeLegacyPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta);

// Compatibility bridge for consumers that still render judged ReplayData.
// This derived value must never be persisted or used for IR.
[[nodiscard]] ReplayData makeMaterializedPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const MaterializedReplay &materialized,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta);

} // namespace replay
