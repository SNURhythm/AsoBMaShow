#pragma once

#include "BeatorajaHiSpeed.h"

#include "../../bms_parser.hpp"

namespace gameplay_hispeed {

// LaneRenderer.init's start/min/max/main BPM acquisition for the parser model
// AsoBMaShow actually renders. MAIN aggregates playable note heads per BPM.
[[nodiscard]] ChartBpmSummary summarizeChartBpm(const bms_parser::Chart &);

} // namespace gameplay_hispeed
