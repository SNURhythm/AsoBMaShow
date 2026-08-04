#include "scene/play/PlayfieldProjection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool closeTo(double actual, double expected) {
  return std::abs(actual - expected) < 0.00001;
}

bool containsNote(const std::vector<ProjectedPlayfieldNote> &notes,
                  ChartVisualId id) {
  return std::ranges::any_of(
      notes, [id](const auto &note) { return note.noteId == id; });
}

} // namespace

int main() {
  PlayfieldChartVisualModel model;
  model.laneOrder = {1, 2};
  model.timelines = {
      {.id = 1,
       .timeMicros = 0,
       .scrollPosition = 0.0,
       .bpm = 120.0,
       .sectionLine = true,
       .retainedForProjection = true,
       .authoredOrdinal = 0,
       .retainedOrdinal = 0},
      {.id = 2,
       .timeMicros = 1'000'000,
       .scrollPosition = 4.0,
       .bpm = 120.0,
       .stopMicros = 200'000,
       .retainedForProjection = true,
       .authoredOrdinal = 1,
       .retainedOrdinal = 1},
      {.id = 3,
       .timeMicros = 2'000'000,
       .scrollPosition = 7.2,
       .bpm = 180.0,
       .scrollRate = 0.8,
       .retainedForProjection = true,
       .authoredOrdinal = 2,
       .retainedOrdinal = 2},
      {.id = 4,
       .timeMicros = 3'000'000,
       .scrollPosition = 10.4,
       .bpm = 180.0,
       .scrollRate = 0.8,
       .sectionLine = true,
       .retainedForProjection = true,
       .authoredOrdinal = 3,
       .retainedOrdinal = 3},
  };
  model.notes = {
      {.id = 11,
       .timelineId = 1,
       .lane = 1,
       .kind = ChartVisualNoteKind::Invisible,
       .source = ChartVisualNoteSource::Invisible,
       .authoredOrdinal = 0},
      {.id = 12,
       .timelineId = 3,
       .lane = 1,
       .kind = ChartVisualNoteKind::Normal,
       .authoredOrdinal = 1},
      {.id = 13,
       .timelineId = 3,
       .lane = 2,
       .kind = ChartVisualNoteKind::Mine,
       .source = ChartVisualNoteSource::Mine,
       .authoredOrdinal = 2},
      {.id = 20,
       .timelineId = 2,
       .pairId = 21,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::HCN,
       .authoredOrdinal = 3},
      {.id = 21,
       .timelineId = 4,
       .pairId = 20,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::HCN,
       .authoredOrdinal = 4},
      {.id = 22,
       .timelineId = 3,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongHead,
       .authoredOrdinal = 5},
      {.id = 23,
       .timelineId = 3,
       .pairId = 24,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongHead,
       .authoredOrdinal = 6},
      {.id = 24,
       .timelineId = 4,
       .pairId = 23,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongHead,
       .authoredOrdinal = 7},
      {.id = 30,
       .timelineId = 2,
       .pairId = 31,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 8},
      {.id = 31,
       .timelineId = 3,
       .pairId = 30,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 9},
      {.id = 40,
       .timelineId = 2,
       .pairId = 41,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 10},
      {.id = 41,
       .timelineId = 4,
       .pairId = 40,
       .lane = 2,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 11},
  };
  const auto originalModel = model;

  PlayfieldVisualState state;
  state.clock = {.serial = 42, .visualTimeMicros = 1'100'000};
  state.notes = {
      {.id = 20,
       .judged = true,
       .longActive = true,
       .longDamaged = true,
       .longReactive = true},
      {.id = 21, .dead = true},
  };
  const auto originalState = state;

  PlayfieldProjection projection;
  const auto result = projection.project(model, state,
                                         {.visibleScrollBefore = 10.0,
                                          .visibleScrollAfter = 10.0,
                                          .includeInvisibleNotes = true});

  if (result.frameSerial != 42 || !closeTo(result.currentScrollPosition, 4.0) ||
      !closeTo(scrollPositionAtTime(model, 1'100'000), 4.0) ||
      result.timelines.size() != 4 || result.longNotes.size() != 3 ||
      containsNote(result.notes, 11) || !containsNote(result.notes, 12) ||
      !containsNote(result.notes, 13) || !containsNote(result.notes, 22) ||
      model != originalModel ||
      state.clock.serial != originalState.clock.serial ||
      state.clock.visualTimeMicros != originalState.clock.visualTimeMicros ||
      state.notes != originalState.notes) {
    std::cerr << "projection state, stop, invisible, or retained timeline "
                 "contract failed\n";
    return EXIT_FAILURE;
  }

  const auto &longNote = result.longNotes.front();
  if (longNote.headId != 20 || longNote.tailId != 21 ||
      longNote.headTimelineId != 2 || longNote.tailTimelineId != 4 ||
      longNote.headTimeMicros != 1'000'000 ||
      longNote.tailTimeMicros != 3'000'000 ||
      longNote.headRetainedOrdinal != 1 || longNote.tailRetainedOrdinal != 3 ||
      longNote.headSource != ChartVisualNoteSource::Playable ||
      longNote.tailSource != ChartVisualNoteSource::Playable ||
      longNote.mode != ChartLongNoteMode::HCN || !longNote.active ||
      !longNote.damaged || !longNote.reactive || !longNote.headJudged ||
      !longNote.tailJudged || longNote.submissionOrdinal <= 181U) {
    std::cerr << "long-note endpoint/state/order contract failed\n";
    return EXIT_FAILURE;
  }
  if (result.longNotes[1].mode != ChartLongNoteMode::LN ||
      result.longNotes[2].mode != ChartLongNoteMode::CN ||
      result.notes.size() != 5 || result.notes[0].noteId != 12 ||
      result.notes[0].submissionOrdinal != 181U ||
      result.notes[1].noteId != 13 ||
      result.notes[1].submissionOrdinal != 182U) {
    std::cerr << "projection mode or deterministic ordering contract failed\n";
    return EXIT_FAILURE;
  }
  if (result.timelines[1].timelineId != 2 ||
      result.timelines[1].timeMicros != 1'000'000 ||
      result.timelines[1].authoredOrdinal != 1 ||
      result.timelines[1].retainedOrdinal != 1 ||
      result.notes[0].timelineId != 3 ||
      result.notes[0].timeMicros != 2'000'000 ||
      result.notes[0].retainedTimelineOrdinal != 2 ||
      result.notes[0].source != ChartVisualNoteSource::Playable) {
    std::cerr << "projection value DTO contract failed\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel invisibleLongModel;
  invisibleLongModel.laneOrder = {1};
  invisibleLongModel.timelines = {
      {.id = 100,
       .timeMicros = 1'000'000,
       .scrollPosition = 1.0,
       .retainedForProjection = true,
       .authoredOrdinal = 7,
       .retainedOrdinal = 0},
      {.id = 101,
       .timeMicros = 2'000'000,
       .scrollPosition = 2.0,
       .retainedForProjection = true,
       .authoredOrdinal = 8,
       .retainedOrdinal = 1},
  };
  invisibleLongModel.notes = {
      {.id = 1001,
       .timelineId = 100,
       .pairId = 1002,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .source = ChartVisualNoteSource::Invisible,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 3},
      {.id = 1002,
       .timelineId = 101,
       .pairId = 1001,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .source = ChartVisualNoteSource::Invisible,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 4},
  };
  PlayfieldVisualState invisibleLongState;
  invisibleLongState.clock.visualTimeMicros = 500'000;
  const auto invisibleLongResult = projection.project(
      invisibleLongModel, invisibleLongState, {.includeInvisibleNotes = true});
  // Projection descriptors are parser-free value snapshots: disposing of the
  // input model cannot alter the endpoint data retained for this frame.
  invisibleLongModel = {};
  if (invisibleLongResult.longNotes.size() != 0 ||
      invisibleLongResult.notes.size() != 2 ||
      invisibleLongResult.notes[0].source != ChartVisualNoteSource::Invisible ||
      invisibleLongResult.notes[0].kind != ChartVisualNoteKind::LongHead ||
      invisibleLongResult.notes[0].longNoteMode != ChartLongNoteMode::CN ||
      invisibleLongResult.notes[0].pairId != 1002 ||
      invisibleLongResult.notes[0].timelineId != 100 ||
      invisibleLongResult.notes[0].timeMicros != 1'000'000 ||
      invisibleLongResult.notes[0].builtInDepth != 183U ||
      invisibleLongResult.notes[1].source != ChartVisualNoteSource::Invisible ||
      invisibleLongResult.notes[1].kind != ChartVisualNoteKind::LongTail ||
      invisibleLongResult.notes[1].pairId != 1001 ||
      invisibleLongResult.notes[1].retainedTimelineOrdinal != 1 ||
      invisibleLongResult.notes[1].builtInDepth != 184U) {
    std::cerr << "invisible long endpoint source contract failed\n";
    return EXIT_FAILURE;
  }

  const auto lineKindCount = [&result](ProjectedLineKind kind) {
    return std::count_if(
        result.lines.begin(), result.lines.end(),
        [kind](const auto &line) { return line.kind == kind; });
  };
  if (lineKindCount(ProjectedLineKind::Section) != 2 ||
      lineKindCount(ProjectedLineKind::BpmChange) != 1 ||
      lineKindCount(ProjectedLineKind::Stop) != 1 ||
      lineKindCount(ProjectedLineKind::Time) != 4) {
    std::cerr << "line descriptor contract failed\n";
    return EXIT_FAILURE;
  }

  const auto views = adaptPlayfieldProjectionForSkin(result);
  const auto hasSkinLineKind = [&views](skin::SkinProjectedLineKind kind) {
    return std::ranges::any_of(
        views.lines, [kind](const auto &line) { return line.kind == kind; });
  };
  if (views.longNotes.size() != 3 ||
      views.lines.size() != result.lines.size() ||
      views.longNotes.front().headVisualId != 20 ||
      views.longNotes.front().tailVisualId != 21 ||
      !views.longNotes.front().active || !views.longNotes.front().damaged ||
      !views.longNotes.front().reactive ||
      views.longNotes.front().submissionOrdinal != longNote.submissionOrdinal ||
      !closeTo(views.longNotes.front().headAuthoredYDisplacement,
               longNote.headScrollDelta) ||
      views.lines.front().submissionOrdinal !=
          result.lines.front().submissionOrdinal ||
      !hasSkinLineKind(skin::SkinProjectedLineKind::Group) ||
      !hasSkinLineKind(skin::SkinProjectedLineKind::Bpm) ||
      !hasSkinLineKind(skin::SkinProjectedLineKind::Stop) ||
      !hasSkinLineKind(skin::SkinProjectedLineKind::Time)) {
    std::cerr << "Task 14 projection adapter contract failed\n";
    return EXIT_FAILURE;
  }
  if (views.lines.empty() || views.lines.front().submissionOrdinal == 0) {
    std::cerr << "Task 14 adapter requires nonzero line submission ordinals\n";
    return EXIT_FAILURE;
  }

  PlayfieldVisualState crossingState = state;
  crossingState.clock.visualTimeMicros = 2'000'000;
  const auto crossingWindow = projection.project(
      model, crossingState,
      {.visibleScrollBefore = 1.0, .visibleScrollAfter = 4.0});
  if (crossingWindow.longNotes.size() != 3) {
    std::cerr << "long notes crossing the viewport boundary were culled\n";
    return EXIT_FAILURE;
  }

  const auto narrowWindow = projection.project(
      model, state, {.visibleScrollBefore = 0.1, .visibleScrollAfter = 0.1});
  if (!narrowWindow.notes.empty() || containsNote(narrowWindow.notes, 12) ||
      narrowWindow.longNotes.size() != 3) {
    std::cerr << "projection visible-window boundary contract failed\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel budgetModel;
  budgetModel.laneOrder = {1};
  budgetModel.timelines = {
      {.id = 1, .scrollPosition = 0.0, .retainedForProjection = true}};
  budgetModel.notes = {{.id = 1,
                        .timelineId = 1,
                        .lane = 1,
                        .kind = ChartVisualNoteKind::Normal},
                       {.id = 2,
                        .timelineId = 1,
                        .lane = 1,
                        .kind = ChartVisualNoteKind::Normal}};
  const auto budgetResult = projection.project(
      budgetModel, state,
      {.visibleScrollBefore = 1.0, .visibleScrollAfter = 1.0, .maxNotes = 1});
  if (budgetResult.notes.size() != 1 || !budgetResult.budgetExceeded) {
    std::cerr << "projection note budget contract failed\n";
    return EXIT_FAILURE;
  }

  // Desired DTO contract: ProjectedPlayfieldNote::builtInDepth gives a
  // same-row normal and mine the legacy row-primary depth together, while an
  // invisible note receives its own later built-in depth. Submission ordinals
  // remain a unique authored stream for Skin2DRenderer.
  PlayfieldChartVisualModel sharedRowDepthModel;
  sharedRowDepthModel.laneOrder = {1, 2};
  sharedRowDepthModel.timelines = {
      {.id = 50,
       .timeMicros = 2'000'000,
       .scrollPosition = 10.0,
       .retainedForProjection = true,
       .authoredOrdinal = 0},
  };
  sharedRowDepthModel.notes = {
      {.id = 501,
       .timelineId = 50,
       .lane = 1,
       .kind = ChartVisualNoteKind::Normal,
       .authoredOrdinal = 0},
      {.id = 502,
       .timelineId = 50,
       .lane = 2,
       .kind = ChartVisualNoteKind::Mine,
       .authoredOrdinal = 1},
      {.id = 503,
       .timelineId = 50,
       .lane = 1,
       .kind = ChartVisualNoteKind::Invisible,
       .authoredOrdinal = 2},
  };
  PlayfieldVisualState sharedRowDepthState;
  sharedRowDepthState.clock.visualTimeMicros = 1'000'000;
  const auto sharedRowDepthResult =
      projection.project(sharedRowDepthModel, sharedRowDepthState,
                         {.includeInvisibleNotes = true});
  if (sharedRowDepthResult.notes.size() != 3 ||
      sharedRowDepthResult.notes[0].noteId != 501 ||
      sharedRowDepthResult.notes[0].submissionOrdinal != 181U ||
      sharedRowDepthResult.notes[1].noteId != 502 ||
      sharedRowDepthResult.notes[1].submissionOrdinal != 182U ||
      sharedRowDepthResult.notes[2].noteId != 503 ||
      sharedRowDepthResult.notes[2].submissionOrdinal != 183U ||
      sharedRowDepthResult.notes[0].builtInDepth != 183U ||
      sharedRowDepthResult.notes[1].builtInDepth != 183U ||
      sharedRowDepthResult.notes[2].builtInDepth != 184U) {
    std::cerr << "same-row playable and invisible depth contract failed\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel pastLongDepthModel;
  pastLongDepthModel.laneOrder = {1};
  pastLongDepthModel.timelines = {
      {.id = 60,
       .timeMicros = 0,
       .scrollPosition = 0.0,
       .retainedForProjection = true,
       .authoredOrdinal = 0},
      {.id = 61,
       .timeMicros = 1'000'000,
       .scrollPosition = 1.0,
       .retainedForProjection = true,
       .authoredOrdinal = 1},
  };
  pastLongDepthModel.notes = {
      {.id = 601,
       .timelineId = 60,
       .pairId = 602,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 0},
      {.id = 602,
       .timelineId = 61,
       .pairId = 601,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 1},
  };
  PlayfieldVisualState pastLongDepthState;
  pastLongDepthState.clock.visualTimeMicros = 0;
  const auto pastLongDepthResult =
      projection.project(pastLongDepthModel, pastLongDepthState, {});
  // Desired DTO contract: the always pre-reserved past-long body/endpoint
  // depths are 181/182, so the first authored long note exposes body 183 and
  // endpoint 184 while its submission ordinal remains a separate, unique
  // authored-stream value.
  if (pastLongDepthResult.longNotes.size() != 1 ||
      pastLongDepthResult.longNotes[0].bodyDepth != 183U ||
      pastLongDepthResult.longNotes[0].endpointDepth != 184U ||
      pastLongDepthResult.longNotes[0].submissionOrdinal == 0U) {
    std::cerr << "past long-note depth reservation contract failed\n";
    return EXIT_FAILURE;
  }

  // BMSRenderer reserves 181/182 for every long body whose head has already
  // passed the late-poor window.  Those rows must not consume an additional
  // per-row depth before the current/future traversal begins.  This exact
  // sequence characterizes the historical prepass drift: one past long head
  // used to shift three live long rows and two normal rows by two depths.
  PlayfieldChartVisualModel pastHeadDepthModel;
  pastHeadDepthModel.laneOrder = {1};
  pastHeadDepthModel.timelines = {
      {.id = 90, .timeMicros = 0, .scrollPosition = 0.0,
       .retainedForProjection = true, .authoredOrdinal = 0},
      {.id = 91, .timeMicros = 1'000'000, .scrollPosition = 1.0,
       .retainedForProjection = true, .authoredOrdinal = 1},
      {.id = 92, .timeMicros = 1'100'000, .scrollPosition = 1.1,
       .retainedForProjection = true, .authoredOrdinal = 2},
      {.id = 93, .timeMicros = 1'200'000, .scrollPosition = 1.2,
       .retainedForProjection = true, .authoredOrdinal = 3},
      {.id = 94, .timeMicros = 1'300'000, .scrollPosition = 1.3,
       .retainedForProjection = true, .authoredOrdinal = 4},
      {.id = 95, .timeMicros = 1'400'000, .scrollPosition = 1.4,
       .retainedForProjection = true, .authoredOrdinal = 5},
      {.id = 96, .timeMicros = 1'500'000, .scrollPosition = 1.5,
       .retainedForProjection = true, .authoredOrdinal = 6},
      {.id = 97, .timeMicros = 1'600'000, .scrollPosition = 1.6,
       .retainedForProjection = true, .authoredOrdinal = 7},
      {.id = 98, .timeMicros = 1'700'000, .scrollPosition = 1.7,
       .retainedForProjection = true, .authoredOrdinal = 8},
  };
  pastHeadDepthModel.notes = {
      {.id = 901, .timelineId = 90, .pairId = 902, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 0},
      {.id = 903, .timelineId = 91, .pairId = 904, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 1},
      {.id = 904, .timelineId = 92, .pairId = 903, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .authoredOrdinal = 2},
      {.id = 905, .timelineId = 93, .pairId = 906, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 3},
      {.id = 906, .timelineId = 94, .pairId = 905, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .authoredOrdinal = 4},
      {.id = 907, .timelineId = 95, .pairId = 908, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 5},
      {.id = 908, .timelineId = 96, .pairId = 907, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .authoredOrdinal = 6},
      {.id = 909, .timelineId = 97, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 7},
      {.id = 910, .timelineId = 98, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 8},
      {.id = 902, .timelineId = 97, .pairId = 901, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .authoredOrdinal = 9},
  };
  PlayfieldVisualState pastHeadDepthState;
  pastHeadDepthState.clock.visualTimeMicros = 1'000'000;
  const auto pastHeadDepthResult = projection.project(
      pastHeadDepthModel, pastHeadDepthState,
      {.latePoorTimingMicros = 200'000});
  if (pastHeadDepthResult.longNotes.size() != 4 ||
      pastHeadDepthResult.longNotes[0].bodyDepth != 181U ||
      pastHeadDepthResult.longNotes[0].endpointDepth != 182U ||
      pastHeadDepthResult.longNotes[1].bodyDepth != 183U ||
      pastHeadDepthResult.longNotes[1].endpointDepth != 184U ||
      pastHeadDepthResult.longNotes[2].bodyDepth != 185U ||
      pastHeadDepthResult.longNotes[2].endpointDepth != 186U ||
      pastHeadDepthResult.longNotes[3].bodyDepth != 187U ||
      pastHeadDepthResult.longNotes[3].endpointDepth != 188U ||
      pastHeadDepthResult.notes.size() != 2 ||
      pastHeadDepthResult.notes[0].builtInDepth != 189U ||
      pastHeadDepthResult.notes[1].builtInDepth != 190U) {
    std::cerr << "past long heads must not shift live built-in depths\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel spanningLongModel;
  spanningLongModel.laneOrder = {1};
  spanningLongModel.timelines = {
      {.id = 70,
       .timeMicros = 0,
       .scrollPosition = -3.0,
       .retainedForProjection = true,
       .authoredOrdinal = 0},
      {.id = 71,
       .timeMicros = 500'000,
       .scrollPosition = 0.0,
       .retainedForProjection = true,
       .authoredOrdinal = 1},
      {.id = 72,
       .timeMicros = 1'000'000,
       .scrollPosition = 3.0,
       .retainedForProjection = true,
       .authoredOrdinal = 2},
  };
  spanningLongModel.notes = {
      {.id = 701,
       .timelineId = 70,
       .pairId = 702,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 0},
      {.id = 702,
       .timelineId = 72,
       .pairId = 701,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 1},
  };
  PlayfieldVisualState spanningLongState;
  spanningLongState.clock.visualTimeMicros = 500'000;
  const auto spanningLongResult = projection.project(
      spanningLongModel, spanningLongState,
      {.visibleScrollBefore = 1.0, .visibleScrollAfter = 1.0});
  if (spanningLongResult.longNotes.size() != 1 ||
      spanningLongResult.longNotes[0].headId != 701 ||
      spanningLongResult.longNotes[0].tailId != 702) {
    std::cerr << "long note spanning both viewport bounds was culled\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel longStateModel;
  longStateModel.laneOrder = {1};
  longStateModel.timelines = {
      {.id = 80,
       .timeMicros = 0,
       .scrollPosition = 0.0,
       .retainedForProjection = true,
       .authoredOrdinal = 0},
      {.id = 81,
       .timeMicros = 1'000'000,
       .scrollPosition = 1.0,
       .retainedForProjection = true,
       .authoredOrdinal = 1},
  };
  longStateModel.notes = {
      {.id = 801,
       .timelineId = 80,
       .pairId = 802,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 0},
      {.id = 802,
       .timelineId = 81,
       .pairId = 801,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 1},
      {.id = 803,
       .timelineId = 80,
       .pairId = 804,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 2},
      {.id = 804,
       .timelineId = 81,
       .pairId = 803,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 3},
      {.id = 805,
       .timelineId = 80,
       .pairId = 806,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongHead,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 4},
      {.id = 806,
       .timelineId = 81,
       .pairId = 805,
       .lane = 1,
       .kind = ChartVisualNoteKind::LongTail,
       .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 5},
  };
  PlayfieldVisualState longState;
  longState.clock.visualTimeMicros = 500'000;
  longState.notes = {
      {.id = 801, .dead = true},
      {.id = 803, .judged = true},
      {.id = 804, .judged = true, .playedTimeMicros = 500'000},
      {.id = 805, .judged = true},
      {.id = 806, .judged = true, .dead = true, .playedTimeMicros = 1'000'000},
  };
  const auto longStateResult =
      projection.project(longStateModel, longState, {});
  // Desired DTO contract: ProjectedLongNoteDescriptor::headDead and
  // ::tailDead preserve death independently from judgement, so a dead-head
  // orphan, an early-release CN tail, and a dead resolved tail do not collapse
  // to the same pair of judged booleans.
  if (longStateResult.longNotes.size() != 3 ||
      !longStateResult.longNotes[0].headJudged ||
      longStateResult.longNotes[0].tailJudged ||
      !longStateResult.longNotes[0].headDead ||
      longStateResult.longNotes[0].tailDead ||
      longStateResult.longNotes[1].mode != ChartLongNoteMode::CN ||
      !longStateResult.longNotes[1].headJudged ||
      !longStateResult.longNotes[1].tailJudged ||
      longStateResult.longNotes[1].headDead ||
      longStateResult.longNotes[1].tailDead ||
      !longStateResult.longNotes[1].tailPlayed ||
      longStateResult.longNotes[1].tailPlayedTimeMicros != 500'000 ||
      !longStateResult.longNotes[1].tailReleasedEarly ||
      longStateResult.longNotes[1].tailMissedWithHead ||
      longStateResult.longNotes[1].tailResolvedAtOrAfterTiming ||
      !longStateResult.longNotes[2].headJudged ||
      !longStateResult.longNotes[2].tailJudged ||
      longStateResult.longNotes[2].headDead ||
      !longStateResult.longNotes[2].tailDead ||
      longStateResult.longNotes[2].tailReleasedEarly ||
      !longStateResult.longNotes[2].tailResolvedAtOrAfterTiming) {
    std::cerr << "long-note dead and resolved state preservation failed\n";
    return EXIT_FAILURE;
  }

  PlayfieldChartVisualModel reverseStopModel;
  reverseStopModel.laneOrder = {1};
  reverseStopModel.timelines = {
      {.id = 90,
       .timeMicros = 0,
       .scrollPosition = 0.0,
       .bpm = 120.0,
       .retainedForProjection = true,
       .authoredOrdinal = 0},
      {.id = 91,
       .timeMicros = 1'000'000,
       .scrollPosition = -1.0,
       .bpm = 120.0,
       .stopMicros = 500'000,
       .retainedForProjection = true,
       .authoredOrdinal = 1},
      {.id = 92,
       .timeMicros = 1'000'000,
       .scrollPosition = -1.0,
       .bpm = 120.0,
       .retainedForProjection = true,
       .authoredOrdinal = 2},
      {.id = 93,
       .timeMicros = 2'000'000,
       .scrollPosition = -2.0,
       .bpm = 120.0,
       .retainedForProjection = true,
       .authoredOrdinal = 3},
  };
  reverseStopModel.notes = {
      {.id = 901,
       .timelineId = 91,
       .lane = 1,
       .kind = ChartVisualNoteKind::Normal,
       .authoredOrdinal = 0},
      {.id = 902,
       .timelineId = 92,
       .lane = 1,
       .kind = ChartVisualNoteKind::Mine,
       .authoredOrdinal = 1},
  };
  PlayfieldVisualState reverseStopState;
  reverseStopState.clock.visualTimeMicros = 1'250'000;
  const auto reverseStopResult = projection.project(
      reverseStopModel, reverseStopState,
      {.visibleScrollBefore = 0.25, .visibleScrollAfter = 0.25});
  if (!closeTo(reverseStopResult.currentScrollPosition, -1.25) ||
      reverseStopResult.timelines.size() != 2 ||
      reverseStopResult.timelines[0].timelineId != 91 ||
      reverseStopResult.timelines[1].timelineId != 92 ||
      reverseStopResult.notes.size() != 2 ||
      reverseStopResult.notes[0].noteId != 901 ||
      reverseStopResult.notes[1].noteId != 902 ||
      reverseStopResult.notes[0].submissionOrdinal >=
          reverseStopResult.notes[1].submissionOrdinal) {
    std::cerr
        << "reverse scroll, stop, and equal-row traversal contract failed\n";
    return EXIT_FAILURE;
  }

  // The built-in path must retain its own unbounded traversal plan even when
  // generic skin DTO limits would drop the same rows.  The forward stop tests
  // the previous future row, so the first row beyond the upper boundary is
  // still visited; the next is not.
  PlayfieldChartVisualModel builtInPlanModel;
  builtInPlanModel.laneOrder = {1};
  builtInPlanModel.timelines = {
      {.id = 1000, .timeMicros = 0, .beat = 0.0, .scrollPosition = 0.0,
       .retainedForProjection = true, .authoredOrdinal = 0,
       .retainedOrdinal = 0},
      {.id = 1001, .timeMicros = 100, .beat = 1.0, .scrollPosition = 1.0,
       .retainedForProjection = true, .authoredOrdinal = 1,
       .retainedOrdinal = 1},
      {.id = 1002, .timeMicros = 300, .beat = 2.0, .scrollPosition = 2.0,
       .sectionLine = true, .retainedForProjection = true,
       .authoredOrdinal = 2, .retainedOrdinal = 2},
      {.id = 1003, .timeMicros = 400, .beat = 3.0, .scrollPosition = 3.0,
       .retainedForProjection = true, .authoredOrdinal = 3,
       .retainedOrdinal = 3},
      {.id = 1004, .timeMicros = 500, .beat = 4.0, .scrollPosition = 4.0,
       .retainedForProjection = true, .authoredOrdinal = 4,
       .retainedOrdinal = 4},
      {.id = 1005, .timeMicros = 600, .beat = 5.0, .scrollPosition = 5.0,
       .retainedForProjection = true, .authoredOrdinal = 5,
       .retainedOrdinal = 5},
  };
  builtInPlanModel.notes = {
      {.id = 1110, .timelineId = 1001, .pairId = 1111, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 0},
      {.id = 1102, .timelineId = 1002, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 1},
      {.id = 1103, .timelineId = 1003, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 2},
      {.id = 1104, .timelineId = 1004, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 3},
      {.id = 1105, .timelineId = 1005, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 4},
      {.id = 1111, .timelineId = 1005, .pairId = 1110, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .authoredOrdinal = 5},
  };
  PlayfieldVisualState builtInPlanState;
  builtInPlanState.clock.visualTimeMicros = 200;
  const auto builtInPlanResult = projection.project(
      builtInPlanModel, builtInPlanState,
      {.maxTimelines = 1,
       .maxNotes = 1,
       .builtInTraversal = BuiltInRendererTraversal{.judgeY = 0.0F,
                                                     .upperBound = 1.5F,
                                                     .rxhs = 1.0F}});
  if (builtInPlanResult.builtInPlan.traversedTimelineOrdinals !=
          std::vector<std::uint32_t>({0, 1, 2, 3, 4}) ||
      builtInPlanResult.builtInPlan.nextStartRetainedOrdinal != 1U ||
      builtInPlanResult.builtInPlan.entries.size() != 5U ||
      builtInPlanResult.builtInPlan.entries[0].kind !=
          BuiltInRendererPlanEntryKind::SectionLine ||
      builtInPlanResult.builtInPlan.entries[1].kind !=
          BuiltInRendererPlanEntryKind::Note ||
      builtInPlanResult.builtInPlan.entries[3].descriptorIndex != 2U ||
      builtInPlanResult.builtInPlan.entries[4].kind !=
          BuiltInRendererPlanEntryKind::LongNote ||
      !builtInPlanResult.builtInPlan.entries[4].tailAtUpperBound ||
      builtInPlanResult.builtInPlan.entries[4].renderY != -1.0F ||
      builtInPlanResult.builtInPlan.entries[4].tailRenderY != 1.5F) {
    std::cerr << "built-in plan must preserve bounded traversal independently "
                 "of skin DTO limits\n";
    return EXIT_FAILURE;
  }
  const auto repeatedBuiltInPlanResult = projection.project(
      builtInPlanModel, builtInPlanState,
      {.builtInTraversal = BuiltInRendererTraversal{
           .judgeY = 0.0F,
           .upperBound = 1.5F,
           .rxhs = 1.0F,
           .startRetainedOrdinal =
               builtInPlanResult.builtInPlan.nextStartRetainedOrdinal}});
  if (repeatedBuiltInPlanResult.builtInPlan.traversedTimelineOrdinals !=
          std::vector<std::uint32_t>({1, 2, 3, 4}) ||
      repeatedBuiltInPlanResult.builtInPlan.nextStartRetainedOrdinal != 1U) {
    std::cerr << "built-in plan must preserve the renderer-owned cursor "
                 "across frames\n";
    return EXIT_FAILURE;
  }

  // A later immutable frame starts from the renderer-owned cursor.  Its LN
  // head is deliberately before that cursor while the tail remains ahead of
  // the visual clock, so the plan must retain the spanning body with a
  // lower-bound head.  BMSRenderer consumes the resulting entry directly;
  // this guards against reintroducing a head-traversed-only draw condition.
  PlayfieldVisualState laterBuiltInPlanState = builtInPlanState;
  laterBuiltInPlanState.clock.visualTimeMicros = 300;
  const auto laterBuiltInPlanResult = projection.project(
      builtInPlanModel, laterBuiltInPlanState,
      {.latePoorTimingMicros = 0,
       .builtInTraversal = BuiltInRendererTraversal{
           .lowerBound = -1.0F,
           .judgeY = 0.0F,
           .upperBound = 1.5F,
           .rxhs = 1.0F,
           .startRetainedOrdinal =
               builtInPlanResult.builtInPlan.nextStartRetainedOrdinal}});
  if (laterBuiltInPlanResult.builtInPlan.traversedTimelineOrdinals.empty() ||
      laterBuiltInPlanResult.builtInPlan.traversedTimelineOrdinals.front() !=
          1U ||
      laterBuiltInPlanResult.builtInPlan.longNotes.size() != 1U ||
      laterBuiltInPlanResult.builtInPlan.longNotes.front().headId != 1110U ||
      laterBuiltInPlanResult.builtInPlan.entries.empty() ||
      laterBuiltInPlanResult.builtInPlan.entries.back().kind !=
          BuiltInRendererPlanEntryKind::LongNote ||
      laterBuiltInPlanResult.builtInPlan.entries.back().renderY != -1.0F) {
    std::cerr << "cursor-advanced built-in plan must retain a spanning "
                 "long-note body\n";
    return EXIT_FAILURE;
  }

  // Equal-time future rows make the legacy incremental denominator zero. The
  // NaN row itself is visited, then the prior-row comparison halts before the
  // next row; the plan must retain that nonfinite Y rather than recomputing an
  // absolute scroll delta in BMSRenderer.
  PlayfieldChartVisualModel nonfinitePlanModel;
  nonfinitePlanModel.laneOrder = {1};
  nonfinitePlanModel.timelines = {
      {.id = 1200, .timeMicros = 0, .beat = 0.0, .scrollPosition = 0.0,
       .retainedForProjection = true, .authoredOrdinal = 0,
       .retainedOrdinal = 0},
      {.id = 1201, .timeMicros = 100, .beat = 1.0, .scrollPosition = 1.0,
       .retainedForProjection = true, .authoredOrdinal = 1,
       .retainedOrdinal = 1},
      {.id = 1202, .timeMicros = 100, .beat = 2.0, .scrollPosition = 2.0,
       .retainedForProjection = true, .authoredOrdinal = 2,
       .retainedOrdinal = 2},
      {.id = 1203, .timeMicros = 200, .beat = 3.0, .scrollPosition = 3.0,
       .retainedForProjection = true, .authoredOrdinal = 3,
       .retainedOrdinal = 3},
  };
  nonfinitePlanModel.notes = {
      {.id = 1210, .timelineId = 1202, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 0},
  };
  PlayfieldVisualState nonfinitePlanState;
  nonfinitePlanState.clock.visualTimeMicros = 100;
  const auto nonfinitePlanResult = projection.project(
      nonfinitePlanModel, nonfinitePlanState,
      {.builtInTraversal = BuiltInRendererTraversal{
           .judgeY = 0.0F, .upperBound = 10.0F, .rxhs = 1.0F}});
  if (nonfinitePlanResult.builtInPlan.traversedTimelineOrdinals !=
          std::vector<std::uint32_t>({0, 1, 2}) ||
      nonfinitePlanResult.builtInPlan.entries.size() != 1U ||
      std::isfinite(nonfinitePlanResult.builtInPlan.entries[0].renderY)) {
    std::cerr << "built-in plan must freeze equal-time nonfinite traversal\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
