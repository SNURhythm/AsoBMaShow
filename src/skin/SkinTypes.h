#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
#include "../context.h"

class View;

struct ResultSkinData {
  const RhythmState *state;
  const bms_parser::ChartMeta *meta;
  ApplicationContext *context;
  View **outGraphPlaceholder = nullptr;
  bool showControls = true;
};
