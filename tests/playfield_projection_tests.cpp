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
  return std::ranges::any_of(notes,
                             [id](const auto &note) { return note.noteId == id; });
}

} // namespace

int main() {
  PlayfieldChartVisualModel model;
  model.laneOrder = {1, 2};
  model.timelines = {
      {.id = 1, .timeMicros = 0, .scrollPosition = 0.0, .bpm = 120.0,
       .sectionLine = true, .retainedForProjection = true, .authoredOrdinal = 0},
      {.id = 2, .timeMicros = 1'000'000, .scrollPosition = 4.0, .bpm = 120.0,
       .stopMicros = 200'000, .retainedForProjection = true, .authoredOrdinal = 1},
      {.id = 3, .timeMicros = 2'000'000, .scrollPosition = 7.2, .bpm = 180.0,
       .scrollRate = 0.8, .retainedForProjection = true, .authoredOrdinal = 2},
      {.id = 4, .timeMicros = 3'000'000, .scrollPosition = 10.4, .bpm = 180.0,
       .scrollRate = 0.8, .sectionLine = true, .retainedForProjection = true,
       .authoredOrdinal = 3},
  };
  model.notes = {
      {.id = 11, .timelineId = 1, .lane = 1,
       .kind = ChartVisualNoteKind::Invisible, .authoredOrdinal = 0},
      {.id = 12, .timelineId = 3, .lane = 1,
       .kind = ChartVisualNoteKind::Normal, .authoredOrdinal = 1},
      {.id = 13, .timelineId = 3, .lane = 2,
       .kind = ChartVisualNoteKind::Mine, .authoredOrdinal = 2},
      {.id = 20, .timelineId = 2, .pairId = 21, .lane = 1,
       .kind = ChartVisualNoteKind::LongHead, .longNoteMode = ChartLongNoteMode::HCN,
       .authoredOrdinal = 3},
      {.id = 21, .timelineId = 4, .pairId = 20, .lane = 1,
       .kind = ChartVisualNoteKind::LongTail, .longNoteMode = ChartLongNoteMode::HCN,
       .authoredOrdinal = 4},
      {.id = 22, .timelineId = 3, .lane = 2,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 5},
      {.id = 23, .timelineId = 3, .pairId = 24, .lane = 2,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 6},
      {.id = 24, .timelineId = 4, .pairId = 23, .lane = 2,
       .kind = ChartVisualNoteKind::LongHead, .authoredOrdinal = 7},
      {.id = 30, .timelineId = 2, .pairId = 31, .lane = 2,
       .kind = ChartVisualNoteKind::LongHead, .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 8},
      {.id = 31, .timelineId = 3, .pairId = 30, .lane = 2,
       .kind = ChartVisualNoteKind::LongTail, .longNoteMode = ChartLongNoteMode::LN,
       .authoredOrdinal = 9},
      {.id = 40, .timelineId = 2, .pairId = 41, .lane = 2,
       .kind = ChartVisualNoteKind::LongHead, .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 10},
      {.id = 41, .timelineId = 4, .pairId = 40, .lane = 2,
       .kind = ChartVisualNoteKind::LongTail, .longNoteMode = ChartLongNoteMode::CN,
       .authoredOrdinal = 11},
  };
  const auto originalModel = model;

  PlayfieldVisualState state;
  state.clock = {.serial = 42, .visualTimeMicros = 1'100'000};
  state.notes = {
      {.id = 20, .judged = true, .longActive = true, .longDamaged = true,
       .longReactive = true},
      {.id = 21, .dead = true},
  };
  const auto originalState = state;

  PlayfieldProjection projection;
  const auto result = projection.project(
      model, state,
      {.visibleScrollBefore = 10.0, .visibleScrollAfter = 10.0,
       .includeInvisibleNotes = true});

  if (result.frameSerial != 42 || !closeTo(result.currentScrollPosition, 4.0) ||
      !closeTo(scrollPositionAtTime(model, 1'100'000), 4.0) ||
      result.timelines.size() != 4 || result.longNotes.size() != 3 ||
      containsNote(result.notes, 11) || !containsNote(result.notes, 12) ||
      !containsNote(result.notes, 13) || !containsNote(result.notes, 22) ||
      model != originalModel || state.clock.serial != originalState.clock.serial ||
      state.clock.visualTimeMicros != originalState.clock.visualTimeMicros ||
      state.notes != originalState.notes) {
    std::cerr << "projection state, stop, invisible, or retained timeline contract failed\n";
    return EXIT_FAILURE;
  }

  const auto &longNote = result.longNotes.front();
  if (longNote.headId != 20 || longNote.tailId != 21 ||
      longNote.mode != ChartLongNoteMode::HCN || !longNote.active ||
      !longNote.damaged || !longNote.reactive || !longNote.headJudged ||
      !longNote.tailJudged || longNote.submissionOrdinal <= 181U) {
    std::cerr << "long-note endpoint/state/order contract failed\n";
    return EXIT_FAILURE;
  }
  if (result.longNotes[1].mode != ChartLongNoteMode::LN ||
      result.longNotes[2].mode != ChartLongNoteMode::CN ||
      result.notes.size() != 5 || result.notes[0].noteId != 12 ||
      result.notes[0].submissionOrdinal != 181U || result.notes[1].noteId != 13 ||
      result.notes[1].submissionOrdinal != 182U) {
    std::cerr << "projection mode or deterministic ordering contract failed\n";
    return EXIT_FAILURE;
  }

  const auto lineKindCount = [&result](ProjectedLineKind kind) {
    return std::count_if(result.lines.begin(), result.lines.end(),
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
    return std::ranges::any_of(views.lines,
                               [kind](const auto &line) { return line.kind == kind; });
  };
  if (views.longNotes.size() != 3 || views.lines.size() != result.lines.size() ||
      views.longNotes.front().headVisualId != 20 ||
      views.longNotes.front().tailVisualId != 21 || !views.longNotes.front().active ||
      !views.longNotes.front().damaged || !views.longNotes.front().reactive ||
      views.longNotes.front().submissionOrdinal != longNote.submissionOrdinal ||
      !closeTo(views.longNotes.front().headAuthoredYDisplacement,
               longNote.headScrollDelta) ||
      views.lines.front().submissionOrdinal != result.lines.front().submissionOrdinal ||
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
  budgetModel.timelines = {{.id = 1, .scrollPosition = 0.0,
                            .retainedForProjection = true}};
  budgetModel.notes = {{.id = 1, .timelineId = 1, .lane = 1,
                        .kind = ChartVisualNoteKind::Normal},
                       {.id = 2, .timelineId = 1, .lane = 1,
                        .kind = ChartVisualNoteKind::Normal}};
  const auto budgetResult = projection.project(
      budgetModel, state,
      {.visibleScrollBefore = 1.0, .visibleScrollAfter = 1.0, .maxNotes = 1});
  if (budgetResult.notes.size() != 1 || !budgetResult.budgetExceeded) {
    std::cerr << "projection note budget contract failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
