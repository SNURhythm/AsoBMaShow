#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"

namespace replay_result {
RhythmState BuildResultState(bms_parser::Chart &chart,
                             const ReplayData &replay);
}
