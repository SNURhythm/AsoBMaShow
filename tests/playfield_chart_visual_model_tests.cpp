#include "scene/play/PlayfieldChartVisualModel.h"
#include "bms_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <tuple>

namespace {

const ChartVisualNote *noteInLane(const PlayfieldChartVisualModel &model,
                                  int lane) {
  const auto it = std::ranges::find_if(
      model.notes, [lane](const auto &note) { return note.lane == lane; });
  return it == model.notes.end() ? nullptr : &*it;
}

const ChartVisualNote *noteForTimeline(const PlayfieldChartVisualModel &model,
                                       ChartVisualId timelineId, int lane) {
  const auto it =
      std::ranges::find_if(model.notes, [timelineId, lane](const auto &note) {
        return note.timelineId == timelineId && note.lane == lane;
      });
  return it == model.notes.end() ? nullptr : &*it;
}

bool testStaticChartMetadata() {
  bms_parser::Chart chart;
  chart.Meta.Difficulty = 3;
  chart.Meta.MD5 = "chart-md5";
  chart.Meta.SHA256 = "chart-sha256";
  chart.Meta.Rank = 72;
  chart.Meta.MinBpm = 124.25;
  chart.Meta.MaxBpm = 248.5;
  chart.Meta.TotalLength = 123'456'789;
  chart.Meta.TotalNotes = 987;
  chart.Meta.TotalLandmineNotes = 8;
  chart.Meta.RandomValues = {2};
  chart.Meta.StageFile = "stage.png";
  chart.Meta.BackBmp = "back.png";
  chart.ReferencedBmpTable.emplace(1, "bga.png");
  auto *measure = new bms_parser::Measure();
  auto *stopTimeline = new bms_parser::TimeLine(8, false);
  stopTimeline->StopLength = 48.0;
  measure->TimeLines.push_back(stopTimeline);
  chart.Measures.push_back(measure);

  const auto model = buildPlayfieldChartVisualModel(chart, 0);
  const auto &metadata = model.staticMetadata;
  if (model.chartMd5 != "chart-md5" || model.chartSha256 != "chart-sha256" ||
      metadata.difficulty != 3 || metadata.judgeRank != 72 ||
      metadata.minimumBpm != 124.25 || metadata.maximumBpm != 248.5 ||
      metadata.durationMicros != 123'456'789 || metadata.totalNotes != 987 ||
      metadata.totalLandmineNotes != 8 || !metadata.hasBga ||
      !metadata.hasRandomSequence || !metadata.hasBpmStop ||
      metadata.stageFilePath != "stage.png" ||
      metadata.backBmpPath != "back.png") {
    std::cerr << "chart visual model static metadata conversion failed\n";
    return false;
  }
  return true;
}

bool testExactBeatorajaChartPropertyMetadata() {
  const auto appendTimeline = [](bms_parser::Measure &measure,
                                 long long timing, double bpm) {
    auto *timeline = new bms_parser::TimeLine(8, false);
    timeline->Timing = timing;
    timeline->BeatPosition = static_cast<double>(timing) / 1'000'000.0;
    timeline->Bpm = bpm;
    measure.TimeLines.push_back(timeline);
    return timeline;
  };

  {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    chart.Meta.PlayLevelText = "+007";
    auto *measure = new bms_parser::Measure();
    auto *bpm120 = appendTimeline(*measure, 0, 120.0);
    bpm120->SetNote(0, new bms_parser::Note(1));
    auto *bpm180 = appendTimeline(*measure, 1'000'000, 180.0);
    bpm180->SetNote(1, new bms_parser::Note(2));
    chart.Measures.push_back(measure);

    const auto model = buildPlayfieldChartVisualModel(chart, 1);
    if (model.staticMetadata.authoredPlayLevel != "+007" ||
        model.staticMetadata.playLevel != 7 ||
        model.staticMetadata.mainBpm != 180.0) {
      std::cerr << "exact playlevel/Java HashMap BPM-tie metadata failed\n";
      return false;
    }
  }

  for (const std::string invalid : {
           "7.5", " 7", "2147483648", "999999999999999999999999999999999",
           "not-a-level"}) {
    bms_parser::Chart chart;
    chart.Meta.PlayLevelText = invalid;
    const auto model = buildPlayfieldChartVisualModel(chart, 1);
    if (model.staticMetadata.authoredPlayLevel != invalid ||
        model.staticMetadata.playLevel != 0) {
      std::cerr << "Java Integer.parseInt playlevel fallback failed for "
                << invalid << '\n';
      return false;
    }
  }

  for (const std::string javaDigits : {
           "\xef\xbc\x91\xef\xbc\x92", // fullwidth 12
           "\xd9\xa1\xd9\xa2",         // Arabic-Indic 12
       }) {
    bms_parser::Chart chart;
    chart.Meta.PlayLevelText = javaDigits;
    const auto model = buildPlayfieldChartVisualModel(chart, 1);
    if (model.staticMetadata.playLevel != 12) {
      std::cerr << "Java Character.digit playlevel parsing failed\n";
      return false;
    }
  }

  {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure();
    auto *chargeHeadTimeline = appendTimeline(*measure, 0, 120.0);
    auto *chargeTailTimeline = appendTimeline(*measure, 100'000, 120.0);
    auto *chargeHead = new bms_parser::LongNote(
        1, bms_parser::LongNoteType::ChargeNote);
    auto *chargeTail = new bms_parser::LongNote(
        1, bms_parser::LongNoteType::ChargeNote);
    chargeHead->Tail = chargeTail;
    chargeTail->Head = chargeHead;
    chargeHeadTimeline->SetNote(0, chargeHead);
    chargeTailTimeline->SetNote(0, chargeTail);
    auto *normal = appendTimeline(*measure, 200'000, 180.0);
    normal->SetNote(1, new bms_parser::Note(2));
    chart.Measures.push_back(measure);

    const auto model = buildPlayfieldChartVisualModel(chart, 1);
    if (model.staticMetadata.mainBpm != 120.0) {
      std::cerr << "CN endpoint BPM note weighting failed\n";
      return false;
    }
  }

  {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure();
    auto *undefinedHeadTimeline = appendTimeline(*measure, 0, 120.0);
    auto *undefinedTailTimeline = appendTimeline(*measure, 100'000, 120.0);
    auto *undefinedHead = new bms_parser::LongNote(
        1, bms_parser::LongNoteType::Undefined);
    auto *undefinedTail = new bms_parser::LongNote(
        1, bms_parser::LongNoteType::Undefined);
    undefinedHead->Tail = undefinedTail;
    undefinedTail->Head = undefinedHead;
    undefinedHeadTimeline->SetNote(0, undefinedHead);
    undefinedTailTimeline->SetNote(0, undefinedTail);
    auto *normal = appendTimeline(*measure, 200'000, 180.0);
    normal->SetNote(1, new bms_parser::Note(2));
    chart.Measures.push_back(measure);

    const auto model = buildPlayfieldChartVisualModel(chart, 2);
    if (model.staticMetadata.mainBpm != 120.0) {
      std::cerr << "effective undefined-CN BPM note weighting failed\n";
      return false;
    }
  }

  {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure();
    auto *normalKey = appendTimeline(*measure, 0, 120.0);
    normalKey->SetNote(0, new bms_parser::Note(1));
    auto *normalScratch = appendTimeline(*measure, 100'000, 120.0);
    normalScratch->SetNote(7, new bms_parser::Note(2));

    auto *undefinedKeyHeadTimeline =
        appendTimeline(*measure, 200'000, 120.0);
    auto *undefinedKeyTailTimeline =
        appendTimeline(*measure, 300'000, 120.0);
    auto *undefinedKeyHead = new bms_parser::LongNote(
        3, bms_parser::LongNoteType::Undefined);
    auto *undefinedKeyTail = new bms_parser::LongNote(
        3, bms_parser::LongNoteType::Undefined);
    undefinedKeyHead->Tail = undefinedKeyTail;
    undefinedKeyTail->Head = undefinedKeyHead;
    undefinedKeyHeadTimeline->SetNote(1, undefinedKeyHead);
    undefinedKeyTailTimeline->SetNote(1, undefinedKeyTail);

    auto *chargeKeyHeadTimeline = appendTimeline(*measure, 400'000, 120.0);
    auto *chargeKeyTailTimeline = appendTimeline(*measure, 500'000, 120.0);
    auto *chargeKeyHead = new bms_parser::LongNote(
        4, bms_parser::LongNoteType::ChargeNote);
    auto *chargeKeyTail = new bms_parser::LongNote(
        4, bms_parser::LongNoteType::ChargeNote);
    chargeKeyHead->Tail = chargeKeyTail;
    chargeKeyTail->Head = chargeKeyHead;
    chargeKeyHeadTimeline->SetNote(2, chargeKeyHead);
    chargeKeyTailTimeline->SetNote(2, chargeKeyTail);

    auto *lnScratchHeadTimeline = appendTimeline(*measure, 600'000, 120.0);
    auto *lnScratchTailTimeline = appendTimeline(*measure, 700'000, 120.0);
    auto *lnScratchHead = new bms_parser::LongNote(
        5, bms_parser::LongNoteType::LongNote);
    auto *lnScratchTail = new bms_parser::LongNote(
        5, bms_parser::LongNoteType::LongNote);
    lnScratchHead->Tail = lnScratchTail;
    lnScratchTail->Head = lnScratchHead;
    lnScratchHeadTimeline->SetNote(7, lnScratchHead);
    lnScratchTailTimeline->SetNote(7, lnScratchTail);
    chart.Measures.push_back(measure);
    // BMSModel.getTotalNotes() is the gameplay scoring authority. The
    // displayed note-type breakdown below is independently projected.
    chart.Meta.TotalNotes = 7;

    const auto model = buildPlayfieldChartVisualModel(chart, 2);
    const auto &metadata = model.staticMetadata;
    if (metadata.normalKeyNotes != 1 || metadata.longKeyNotes != 4 ||
        metadata.normalScratchNotes != 1 || metadata.longScratchNotes != 1 ||
        metadata.totalNotes != 7) {
      std::cerr << "LN-mode-dependent mutually exclusive note counts failed\n";
      return false;
    }
  }
  return true;
}

bool testSongInformationDensityMatchesPinnedConstructor() {
  // Pinned SongInformation bins prepared notes into whole seconds. These
  // three ordinary notes yield [1, 1, 1, 0], so density is .75 while peak and
  // end density are 1. TOTAL remains the authored chart total.
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 3;
  chart.Meta.Total = 100.0;
  auto *measure = new bms_parser::Measure();
  for (const auto [time, lane, wav] :
       std::array<std::tuple<long long, int, int>, 3>{{
           {0, 0, 1}, {1'000'000, 1, 2}, {2'000'000, 2, 3}}}) {
    auto *timeline = new bms_parser::TimeLine(8, false);
    timeline->Timing = time;
    timeline->Bpm = 120.0;
    timeline->SetNote(lane, new bms_parser::Note(wav));
    measure->TimeLines.push_back(timeline);
  }
  chart.Measures.push_back(measure);

  const auto model = buildPlayfieldChartVisualModel(chart, 1);
  const auto &information = model.staticMetadata.songInformation;
  if (!information || std::abs(information->density - 0.75) > 0.000001 ||
      std::abs(information->peakDensity - 1.0) > 0.000001 ||
      std::abs(information->endDensity - 1.0) > 0.000001 ||
      std::abs(information->total - 100.0) > 0.000001) {
    std::cerr << "pinned SongInformation density conversion failed\n";
    return false;
  }
  return true;
}

bool testTerminalZeroNoteTimelinesRemainProjectionAnchors() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  const auto appendTimeline = [&measure](long long timing, double beat) {
    auto *timeline = new bms_parser::TimeLine(8, false);
    timeline->Timing = timing;
    timeline->BeatPosition = beat;
    timeline->Bpm = 120.0;
    measure->TimeLines.push_back(timeline);
    return timeline;
  };

  auto *measureStart = appendTimeline(0, 0.0);
  measureStart->IsFirstInMeasure = true;
  auto *lastNote = appendTimeline(1'000'000, 1.0);
  lastNote->SetNote(0, new bms_parser::Note(1));
  // These rows are real chart timelines but carry no note or scroll event.
  // They must still advance a processed final note after it passes the judge
  // line, including when the note falls in the chart's final measure.
  appendTimeline(1'500'000, 1.5);
  appendTimeline(2'000'000, 2.0);
  chart.Measures.push_back(measure);

  const auto model = buildPlayfieldChartVisualModel(chart, 0);
  if (model.timelines.size() != 4 || !model.timelines[2].retainedForProjection ||
      !model.timelines[3].retainedForProjection ||
      model.timelines[2].retainedOrdinal == kNoRetainedTimelineOrdinal ||
      model.timelines[3].retainedOrdinal == kNoRetainedTimelineOrdinal) {
    std::cerr << "terminal zero-note timelines must remain scroll anchors\n";
    return false;
  }
  return true;
}

bool testParserStoredLandminesRetainTheirMineSource() {
  // bms-parser currently puts BMS mine-channel notes in TimeLine::Notes.
  // BMSRenderer recognizes their runtime type; the shared skin model must
  // retain that same source family instead of treating the slot as normal.
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 1'000'000;
  timeline->BeatPosition = 4.0;
  timeline->Bpm = 150.0;
  timeline->SetNote(3, new bms_parser::LandmineNote(25.0F));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);

  const auto model = buildPlayfieldChartVisualModel(chart, 0);
  if (model.notes.size() != 1 ||
      model.notes.front().kind != ChartVisualNoteKind::Mine ||
      model.notes.front().source != ChartVisualNoteSource::Mine ||
      model.notes.front().mineDamage != 25) {
    std::cerr << "parser-stored landmine must retain the Mine source family\n";
    return false;
  }
  // JudgeManager's score note total excludes BMS mine-channel entries.
  // The skin score, graph, and rank properties take this exact metadata
  // total, so a parser-stored mine must not inflate their denominator.
  if (model.staticMetadata.totalNotes != 0) {
    std::cerr << "parser-stored landmine must not contribute to score or "
                 "grade totals\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  if (!testStaticChartMetadata()) {
    return EXIT_FAILURE;
  }
  if (!testExactBeatorajaChartPropertyMetadata()) {
    return EXIT_FAILURE;
  }
  if (!testSongInformationDensityMatchesPinnedConstructor()) {
    return EXIT_FAILURE;
  }
  if (!testTerminalZeroNoteTimelinesRemainProjectionAnchors()) {
    return EXIT_FAILURE;
  }
  if (!testParserStoredLandminesRetainTheirMineSource()) {
    return EXIT_FAILURE;
  }

  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *bga = new bms_parser::TimeLine(8, false);
  bga->Timing = 100;
  bga->BeatPosition = 0.0;
  bga->BgaPoor =
      bms_parser::BgaPoorSequence{{1, bms_parser::BgaSequenceBlank, 2}};
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

  auto *invisible = new bms_parser::Note(2);
  invisible->Lane = 3;
  gimmick->SetInvisibleNote(3, invisible);

  auto *mine = new bms_parser::LandmineNote(20.0F);
  mine->Lane = 4;
  gimmick->SetLandmineNote(4, mine);

  auto *invisibleLongTailTimeline = new bms_parser::TimeLine(8, false);
  invisibleLongTailTimeline->Timing = 1'500'000;
  invisibleLongTailTimeline->BeatPosition = 6.0;
  invisibleLongTailTimeline->Bpm = 180.0;
  invisibleLongTailTimeline->Scroll = 0.5;
  auto *invisibleLongHeadNote =
      new bms_parser::LongNote(3, bms_parser::LongNoteType::LongNote);
  auto *invisibleLongTailNote =
      new bms_parser::LongNote(3, bms_parser::LongNoteType::LongNote);
  invisibleLongHeadNote->Tail = invisibleLongTailNote;
  invisibleLongTailNote->Head = invisibleLongHeadNote;
  invisibleLongHeadNote->Lane = 5;
  invisibleLongTailNote->Lane = 5;
  gimmick->SetInvisibleNote(5, invisibleLongHeadNote);
  invisibleLongTailTimeline->SetInvisibleNote(5, invisibleLongTailNote);
  measure->TimeLines = {bga, gimmick, invisibleLongTailTimeline};
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
  if (model.timelines.size() != 4 || !model.timelines.front().bgaOnly ||
      model.timelines.front().retainedForProjection ||
      !model.timelines.back().retainedForProjection ||
      !model.timelines.back().bgaOnly || model.timelines[1].stopMicros <= 0 ||
      model.timelines[1].bpm != 180.0 || model.timelines[1].scrollRate != 0.5 ||
      model.timelines[1].speed != 1.0 || model.notes.size() != 5 ||
      model.scrollPrefix.size() != 4 || model.scrollPrefix.back() != 6.0 ||
      model.bgaPoorSequences.size() != 2 ||
      model.bgaPoorSequences[0].startBgaMicros != 100 ||
      model.bgaPoorSequences[0].authoredOrdinal != 0 ||
      model.bgaPoorSequences[0].frames !=
          std::vector<int>{1, bms_parser::BgaSequenceBlank, 2} ||
      model.bgaPoorSequences[1].startBgaMicros != 2'000'000 ||
      model.bgaPoorSequences[1].authoredOrdinal != 3 ||
      model.bgaPoorSequences[1].frames !=
          std::vector<int>{bms_parser::BgaSequenceBlank,
                           bms_parser::BgaSequenceBlank}) {
    std::cerr
        << "chart visual model retained/poor-BGA/prefix contract failed\n";
    return EXIT_FAILURE;
  }

  const auto *playable = noteInLane(model, 2);
  const auto *hidden = noteInLane(model, 3);
  const auto *landmine = noteInLane(model, 4);
  const auto *invisibleLongHead =
      noteForTimeline(model, model.timelines[1].id, 5);
  const auto *invisibleLongTail =
      noteForTimeline(model, model.timelines[2].id, 5);
  if (playable == nullptr || hidden == nullptr || landmine == nullptr ||
      invisibleLongHead == nullptr || invisibleLongTail == nullptr ||
      playable->source != ChartVisualNoteSource::Playable ||
      hidden->source != ChartVisualNoteSource::Invisible ||
      landmine->source != ChartVisualNoteSource::Mine ||
      invisibleLongHead->source != ChartVisualNoteSource::Invisible ||
      invisibleLongTail->source != ChartVisualNoteSource::Invisible ||
      invisibleLongHead->kind != ChartVisualNoteKind::LongHead ||
      invisibleLongTail->kind != ChartVisualNoteKind::LongTail ||
      invisibleLongHead->pairId != invisibleLongTail->id ||
      invisibleLongTail->pairId != invisibleLongHead->id) {
    std::cerr << "chart visual model note source/endpoint contract failed\n";
    return EXIT_FAILURE;
  }

  if (model.timelines[0].authoredOrdinal != 0 ||
      model.timelines[0].retainedOrdinal != kNoRetainedTimelineOrdinal ||
      model.timelines[1].authoredOrdinal != 1 ||
      model.timelines[1].retainedOrdinal != 0 ||
      model.timelines[2].authoredOrdinal != 2 ||
      model.timelines[2].retainedOrdinal != 1 ||
      model.timelines[3].authoredOrdinal != 3 ||
      model.timelines[3].retainedOrdinal != 2) {
    std::cerr
        << "chart visual model retained traversal ordinal contract failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
