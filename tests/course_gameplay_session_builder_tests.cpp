#include "scene/CourseGameplaySessionBuilder.h"

#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testBuildsTheSharedCourseRuntimeContract() {
  ChartMetaRecord first;
  first.meta.Title = "First";
  first.meta.BmsPath = "/songs/first.bms";
  ChartMetaRecord second;
  second.meta.Title = "Second";
  second.meta.BmsPath = "/songs/second.bms";
  main_menu_profile::Selections selections;
  selections.gaugeType = GaugeType::Hard;
  selections.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  selections.gaugeAutoShiftLowerBound = GaugeType::Easy;
  selections.playOption = "R-RANDOM";
  selections.longNoteMode = "LN";

  auto session = buildCourseGameplaySession(
      {.courseId = 7,
       .courseKey = "course-key",
       .courseName = "Group Course",
       .courseGroupName = "Group",
       .constraintJson = R"(["grade_mirror","no_speed","hcn"])",
       .records = {first, second},
       .selections = selections,
       .player2PlayOption = "S-RANDOM",
       .doublePlayFlip = true,
       .inputKeysoundEnabled = false});

  expect(session && session->courseId == 7 &&
             session->courseKey == "course-key" &&
             session->courseName == "Group Course" &&
             session->entries.size() == 2 &&
             session->entries[0].meta.Title == "First",
         "course identity and value-owned stage metadata are shared");
  expect(session->gaugeType == GaugeType::Hard &&
             session->gaugeProfile == GaugeProfile::CourseDefault &&
             session->gaugeAutoShift == GaugeAutoShiftMode::Continue &&
             session->gaugeAutoShiftLowerBound == GaugeType::Easy &&
             session->constraints.noSpeed &&
             session->constraints.longNoteMode == CourseLongNoteMode::HCN &&
             session->longNoteMode == long_note_mode::kHcnValue,
         "course constraints and selected gauge state use one authority");
  expect(session->requestedPlayOption == "NORMAL" &&
             session->requestedPlayOption2 == "NORMAL" &&
             !session->doublePlayFlip &&
             session->assistOption == assist_options::kOff &&
             session->autoKeySound,
         "grade option, assist, and keysound rules match native course start");
}

void testCourseRandomOptionsFollowBeatorajaConstraints() {
  main_menu_profile::Selections selections;
  selections.playOption = "MIRROR";
  auto mirror = buildCourseGameplaySession(
      {.constraintJson = R"(["grade_mirror"])",
       .selections = selections,
       .player2PlayOption = "RANDOM",
       .doublePlayFlip = false});
  expect(mirror->requestedPlayOption == "MIRROR" &&
             mirror->requestedPlayOption2 == "MIRROR" &&
             mirror->doublePlayFlip,
         "MIRROR constraint forces both sides and DP flip only for 1P mirror");

  selections.playOption = "S-RANDOM-EX";
  auto random = buildCourseGameplaySession(
      {.constraintJson = R"(["grade_random"])",
       .selections = selections,
       .player2PlayOption = "H-RANDOM",
       .doublePlayFlip = true});
  expect(random->requestedPlayOption == "NORMAL" &&
             random->requestedPlayOption2 == "NORMAL" &&
             random->doublePlayFlip,
         "RANDOM constraint independently resets options above index five");

  selections.playOption = "S-RANDOM";
  auto allowed = buildCourseGameplaySession(
      {.constraintJson = R"(["grade_random"])",
       .selections = selections,
       .player2PlayOption = "SPIRAL",
       .doublePlayFlip = true});
  expect(allowed->requestedPlayOption == "S-RANDOM" &&
             allowed->requestedPlayOption2 == "SPIRAL" &&
             allowed->doublePlayFlip,
         "RANDOM constraint preserves options zero through five and DP flip");
}
} // namespace

int main() {
  testBuildsTheSharedCourseRuntimeContract();
  testCourseRandomOptionsFollowBeatorajaConstraints();
  if (failures != 0) return 1;
  std::cout << "course gameplay session builder tests passed\n";
  return 0;
}
