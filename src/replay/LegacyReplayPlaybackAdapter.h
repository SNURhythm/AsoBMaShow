#pragma once

#include "ReplayPlaybackData.h"
#include "ReplayPlaybackMaterializer.h"
#include "../analysis/JudgedPlaybackData.h"
#include "../ResultPersistenceModel.h"

#include <optional>

namespace replay {

// Migration-only visual playback adapter. The returned JudgedPlaybackData must never
// be used to construct persisted results or IR submissions.
[[nodiscard]] std::optional<JudgedPlaybackData> makeLegacyPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::Chart &chart, ReplayMaterializationSeed seed = {});

// Compatibility bridge for consumers that still render judged JudgedPlaybackData.
// This derived value must never be persisted or used for IR.
[[nodiscard]] std::optional<JudgedPlaybackData>
makeMaterializedPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const MaterializedReplay &materialized,
    const gameplay::GameplayRulesetPolicy &policy,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta);

} // namespace replay
