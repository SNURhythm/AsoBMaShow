#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
#include "../scene/play/RhythmState.h"
#include "../context.h"

class View;

struct ResultSkinData {
    const RhythmState* state;
    const bms_parser::ChartMeta* meta;
    ApplicationContext* context;
    // Pointers to output view references that the skin should populate
    View** outGraphPlaceholder; 
};
