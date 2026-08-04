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
  bga->BgaPoor = bms_parser::BgaPoorSequence{{1, bms_parser::BgaSequenceBlank,
                                              2}};
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

  auto *secondMeasure = new bms_parser::Measure();
  auto *secondPoor = new bms_parser::TimeLine(8, false);
  secondPoor->Timing = 2'000'000;
  secondPoor->BeatPosition = 8.0;
  secondPoor->Bpm = 180.0;
  secondPoor->Scroll = 0.5;
  secondPoor->IsFirstInMeasure = true;
  secondPoor->BgaPoor = bms_parser::BgaPoorSequence{
      {bms_parser::BgaSequenceBlank, bms_parser::BgaSequenceBlank}};
  secondMeasure->TimeLines = {secondPoor};
  chart.Measures.push_back(secondMeasure);

  const auto model = buildPlayfieldChartVisualModel(chart, 0);
  if (model.timelines.size() != 3 || !model.timelines.front().bgaOnly ||
      model.timelines.front().retainedForProjection ||
      !model.timelines.back().retainedForProjection ||
      !model.timelines.back().bgaOnly ||
      model.timelines[1].stopMicros <= 0 ||
      model.timelines[1].bpm != 180.0 ||
      model.timelines[1].scrollRate != 0.5 ||
      model.timelines[1].speed != 1.0 || model.notes.size() != 1 ||
      model.notes.front().lane != 2 || model.scrollPrefix.size() != 3 ||
      model.scrollPrefix.back() != 6.0 ||
      model.bgaPoorSequences.size() != 2 ||
      model.bgaPoorSequences[0].startBgaMicros != 100 ||
      model.bgaPoorSequences[0].authoredOrdinal != 0 ||
      model.bgaPoorSequences[0].frames !=
          std::vector<int>{1, bms_parser::BgaSequenceBlank, 2} ||
      model.bgaPoorSequences[1].startBgaMicros != 2'000'000 ||
      model.bgaPoorSequences[1].authoredOrdinal != 2 ||
      model.bgaPoorSequences[1].frames !=
          std::vector<int>{bms_parser::BgaSequenceBlank,
                           bms_parser::BgaSequenceBlank}) {
    std::cerr << "chart visual model retained/poor-BGA/prefix contract failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
