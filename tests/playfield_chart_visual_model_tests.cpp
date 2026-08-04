#include "scene/play/PlayfieldChartVisualModel.h"
#include "bms_parser.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *bga = new bms_parser::TimeLine(8, false);
  bga->Timing = 100;
  bga->BeatPosition = 0.0;
  bga->BgaBase = 1;
  auto *gimmick = new bms_parser::TimeLine(8, false);
  gimmick->Timing = 1'000'000;
  gimmick->BeatPosition = 4.0;
  gimmick->Bpm = 180.0;
  gimmick->Scroll = 0.5;
  gimmick->StopLength = 48.0;
  gimmick->IsFirstInMeasure = true;
  auto *normal = new bms_parser::Note(1);
  normal->Lane = 2;
  gimmick->SetNote(2, normal);
  measure->TimeLines = {bga, gimmick};
  chart.Measures.push_back(measure);

  const auto model = buildPlayfieldChartVisualModel(chart, 0);
  if (model.timelines.size() != 2 || !model.timelines.front().bgaOnly ||
      model.timelines.front().retainedForProjection ||
      !model.timelines.back().retainedForProjection ||
      model.timelines.back().stopMicros <= 0 ||
      model.timelines.back().bpm != 180.0 ||
      model.timelines.back().scrollRate != 0.5 ||
      model.timelines.back().speed != 1.0 || model.notes.size() != 1 ||
      model.notes.front().lane != 2 || model.scrollPrefix.size() != 2 ||
      model.scrollPrefix.back() != 4.0) {
    std::cerr << "chart visual model retained/BGA/prefix contract failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
