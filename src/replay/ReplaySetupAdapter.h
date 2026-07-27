#pragma once

#include "ReplaySetup.h"

#include "../ReplayData.h"

#include <optional>
#include <string>

namespace replay {

// The sole setup-to-runtime projection used after a replay has passed the
// shared context agreement checks. It intentionally carries no result facts.
[[nodiscard]] std::optional<ReplayData> makeReplayDataFromSetup(
    const ReplaySetup &setup, const ScoreProvenance &provenance,
    const bms_parser::ChartMeta &parsedChartMeta,
    std::string &diagnostic) noexcept;

} // namespace replay
