#pragma once

#include "bms_parser.hpp"

#include <optional>
#include <vector>

namespace prep_metronome {

struct PrepMetronomeClick {
  long long timeMicros = 0;
  bool accent = false;
};

struct PrepMetronomePlan {
  bool enabled = false;
  double bpm = 120.0;
  int beatsPerMeasure = 4;
  long long beatIntervalMicros = 500000;
  long long leadInMicros = 2000000;
  long long startTimeMicros = 0;
  std::vector<PrepMetronomeClick> clicks;
};

bool isSaneBpm(double bpm);
double effectiveBpm(const bms_parser::ChartMeta &meta);
std::optional<double> firstMeasureBpmCandidate(const bms_parser::Chart &chart);
int effectiveBeatsPerMeasure(const bms_parser::ChartMeta &meta);
PrepMetronomePlan buildPlan(const bms_parser::ChartMeta &meta,
                            bool settingEnabled, bool chartPreviewPlayback,
                            long long playbackAnchorMicros);
PrepMetronomePlan buildPlan(const bms_parser::Chart &chart,
                            bool settingEnabled, bool chartPreviewPlayback,
                            long long playbackAnchorMicros);

} // namespace prep_metronome
